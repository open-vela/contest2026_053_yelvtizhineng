#include <nuttx/config.h>

#include <arpa/inet.h>
#include <errno.h>
#include <net/ethernet.h>
#include <net/if.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include <netutils/netlib.h>
#include <netutils/dhcpd.h>
#include <wireless/wapi.h>

#include "soil_wifi.h"

#define SOIL_WIFI_STA_IF "wlan0"
#define SOIL_WIFI_AP_IF "wlan1"

static void json_escape(char *dest, size_t dest_size, const char *source)
{
  size_t out = 0;

  while (*source != '\0' && out + 1 < dest_size)
    {
      unsigned char ch = (unsigned char)*source++;
      if (ch == '"' || ch == '\\')
        {
          if (out + 2 >= dest_size)
            {
              break;
            }

          dest[out++] = '\\';
          dest[out++] = (char)ch;
        }
      else if (ch >= 0x20)
        {
          dest[out++] = (char)ch;
        }
    }

  dest[out] = '\0';
}

static int soil_wifi_find_best(const char *ssid, uint8_t bssid[6],
                               uint32_t *channel, int *rssi)
{
  struct wapi_list_s list;
  struct wapi_scan_info_s *item;
  struct wapi_scan_info_s *best = NULL;
  int sock;
  int ret;
  int attempt;

  memset(&list, 0, sizeof(list));
  sock = wapi_make_socket();
  if (sock < 0)
    {
      return sock;
    }

  ret = wapi_scan_init(sock, SOIL_WIFI_STA_IF, NULL);
  if (ret < 0)
    {
      close(sock);
      return ret;
    }

  for (attempt = 0; attempt < 60; attempt++)
    {
      ret = wapi_scan_stat(sock, SOIL_WIFI_STA_IF);
      if (ret == 0)
        {
          break;
        }

      usleep(100000);
    }

  ret = wapi_scan_coll(sock, SOIL_WIFI_STA_IF, &list);
  close(sock);
  if (ret < 0)
    {
      return ret;
    }

  for (item = list.head.scan; item != NULL; item = item->next)
    {
      if (item->has_essid && strcmp(item->essid, ssid) == 0 &&
          (!best || (item->has_rssi && item->rssi > best->rssi)))
        {
          best = item;
        }
    }

  ret = -ENOENT;
  if (best != NULL && best->has_freq)
    {
      int found_channel = 0;

      sock = wapi_make_socket();
      if (sock >= 0)
        {
          ret = wapi_freq2chan(sock, SOIL_WIFI_STA_IF, best->freq,
                               &found_channel);
          close(sock);
        }

      if (ret == 0 && found_channel > 0)
        {
          memcpy(bssid, best->ap.ether_addr_octet, 6);
          *channel = (uint32_t)found_channel;
          if (rssi != NULL)
            {
              *rssi = best->has_rssi ? best->rssi : -127;
            }

          ret = 0;
        }
    }

  wapi_scan_coll_free(&list);
  return ret;
}

static int soil_wifi_join(struct soil_settings_s *settings, char *ip,
                          size_t ip_size, bool use_hint, int wait_seconds)
{
  struct in_addr address;
  int sock;
  int ret;
  int attempt;

  memset(&address, 0, sizeof(address));
  if (ip != NULL && ip_size > 0)
    {
      ip[0] = '\0';
    }

  /* Call the network APIs directly.  Spawning short-lived shell commands
   * and waiting for their exit can strand this task in waitpid on builds
   * without parent/child status retention.
   */

  ret = netlib_ifdown(SOIL_WIFI_STA_IF);
  if (ret < 0)
    {
      return ret;
    }

  ret = netlib_ifup(SOIL_WIFI_STA_IF);
  if (ret < 0)
    {
      return ret;
    }

  sock = wapi_make_socket();
  if (sock < 0)
    {
      return sock;
    }

  ret = wapi_set_mode(sock, SOIL_WIFI_STA_IF, WAPI_MODE_MANAGED);
  if (ret >= 0 && settings->wifi_password[0] != '\0')
    {
      ret = wpa_driver_wext_set_auth_param(sock, SOIL_WIFI_STA_IF,
                  IW_AUTH_WPA_VERSION, IW_AUTH_WPA_VERSION_WPA2);
      if (ret >= 0)
        {
          ret = wpa_driver_wext_set_auth_param(sock, SOIL_WIFI_STA_IF,
                      IW_AUTH_CIPHER_PAIRWISE, IW_AUTH_CIPHER_CCMP);
        }

      if (ret >= 0)
        {
          ret = wpa_driver_wext_set_key_ext(sock, SOIL_WIFI_STA_IF,
                      WPA_ALG_CCMP, settings->wifi_password,
                      strlen(settings->wifi_password));
        }
    }

  close(sock);
  if (ret < 0)
    {
      return ret;
    }

  if (use_hint && settings->wifi_hint_valid && settings->wifi_channel > 0)
    {
      struct ether_addr ap;
      double frequency = 0;

      sock = wapi_make_socket();
      if (sock >= 0)
        {
          memcpy(ap.ether_addr_octet, settings->wifi_bssid, 6);
          if (wapi_chan2freq(sock, SOIL_WIFI_STA_IF,
                             settings->wifi_channel, &frequency) == 0)
            {
              wapi_set_freq(sock, SOIL_WIFI_STA_IF, frequency,
                            WAPI_FREQ_FIXED);
            }

          wapi_set_ap(sock, SOIL_WIFI_STA_IF, &ap);
          close(sock);
        }
    }

  sock = wapi_make_socket();
  if (sock < 0)
    {
      return sock;
    }

  ret = wapi_set_essid(sock, SOIL_WIFI_STA_IF, settings->wifi_ssid,
                       WAPI_ESSID_ON);
  close(sock);

  if (ret < 0)
    {
      return ret;
    }

  netlib_ifup(SOIL_WIFI_STA_IF);
  sleep(1);

  ret = netlib_obtain_ipv4addr(SOIL_WIFI_STA_IF);
  if (ret < 0)
    {
      return ret;
    }

  sock = wapi_make_socket();
  if (sock < 0)
    {
      return sock;
    }

  for (attempt = 0; attempt < wait_seconds * 2; attempt++)
    {
      ret = wapi_get_ip(sock, SOIL_WIFI_STA_IF, &address);
      if (ret == 0 && address.s_addr != 0)
        {
          break;
        }

      usleep(500000);
    }

  if (address.s_addr == 0)
    {
      close(sock);
      return -ETIMEDOUT;
    }

  wapi_set_power_save(sock, SOIL_WIFI_STA_IF, settings->power_save);
  if (ip != NULL && ip_size > 0)
    {
      inet_ntop(AF_INET, &address, ip, ip_size);
    }

  close(sock);

  return 0;
}

int soil_wifi_station_connect(struct soil_settings_s *settings,
                              char *ip, size_t ip_size)
{
  uint8_t bssid[6];
  uint32_t channel = 0;
  int rssi = -127;
  int ret;

  if (!settings->wifi_hint_valid)
    {
      ret = soil_wifi_find_best(settings->wifi_ssid, bssid, &channel, &rssi);
      if (ret == 0)
        {
          memcpy(settings->wifi_bssid, bssid, sizeof(bssid));
          settings->wifi_channel = channel;
          settings->wifi_hint_valid = true;
          settings->wifi_hint_changed = true;
          soil_log("WIFI_HINT_LEARNED BSSID=%02X:%02X:%02X:%02X:%02X:%02X "
                   "CH=%lu RSSI=%d\n", bssid[0], bssid[1], bssid[2],
                   bssid[3], bssid[4], bssid[5],
                   (unsigned long)channel, rssi);
        }
      else
        {
          soil_log("WIFI_HINT_SCAN_FAILED ret=%d\n", ret);
        }
    }

  if (settings->wifi_hint_valid)
    {
      soil_log("WIFI_FAST_HINT BSSID=%02X:%02X:%02X:%02X:%02X:%02X CH=%lu\n",
               settings->wifi_bssid[0], settings->wifi_bssid[1],
               settings->wifi_bssid[2], settings->wifi_bssid[3],
               settings->wifi_bssid[4], settings->wifi_bssid[5],
               (unsigned long)settings->wifi_channel);
      ret = soil_wifi_join(settings, ip, ip_size, true, 8);
      if (ret == 0)
        {
          return 0;
        }

      soil_log("WIFI_FAST_HINT_FAILED ret=%d\n", ret);
    }

  return soil_wifi_join(settings, ip, ip_size, false, 20);
}

bool soil_wifi_station_is_connected(void)
{
  struct in_addr address;
  int sock;
  int ret;

  sock = wapi_make_socket();
  if (sock < 0)
    {
      return false;
    }

  memset(&address, 0, sizeof(address));
  ret = wapi_get_ip(sock, SOIL_WIFI_STA_IF, &address);
  close(sock);
  return ret == 0 && address.s_addr != 0;
}

int soil_wifi_get_rssi(int *rssi)
{
  int sock;
  int ret;

  if (rssi == NULL)
    {
      return -EINVAL;
    }

  sock = wapi_make_socket();
  if (sock < 0)
    {
      return sock;
    }

  ret = wapi_get_sensitivity(sock, SOIL_WIFI_STA_IF, rssi);
  close(sock);
  if (ret < 0)
    {
      return ret;
    }

  if (*rssi == 128)
    {
      return -ENOTCONN;
    }

  return 0;
}

int soil_wifi_make_ap_ssid(char *buffer, size_t buffer_size)
{
  uint8_t mac[IFHWADDRLEN];
  int ret;

  ret = netlib_getmacaddr(SOIL_WIFI_STA_IF, mac);
  if (ret < 0)
    {
      return ret;
    }

  snprintf(buffer, buffer_size, "ZXB-Soil-%02X%02X%02X",
           mac[3], mac[4], mac[5]);
  return 0;
}

int soil_wifi_start_ap(const char *ssid)
{
  struct in_addr address;
  struct in_addr netmask;
  struct in_addr router;
  int sock;
  int ret;

  netlib_ifup(SOIL_WIFI_AP_IF);
  sock = wapi_make_socket();
  if (sock < 0)
    {
      return sock;
    }

  ret = wapi_set_mode(sock, SOIL_WIFI_AP_IF, WAPI_MODE_MASTER);
  if (ret == 0)
    {
      ret = wapi_set_essid(sock, SOIL_WIFI_AP_IF, ssid, WAPI_ESSID_ON);
    }

  close(sock);
  if (ret < 0)
    {
      return ret;
    }

  inet_pton(AF_INET, "192.168.4.1", &address);
  inet_pton(AF_INET, "255.255.255.0", &netmask);
  inet_pton(AF_INET, "192.168.4.1", &router);
  netlib_set_ipv4addr(SOIL_WIFI_AP_IF, &address);
  netlib_set_ipv4netmask(SOIL_WIFI_AP_IF, &netmask);
  netlib_set_dripv4addr(SOIL_WIFI_AP_IF, &router);
  netlib_ifup(SOIL_WIFI_AP_IF);

  return dhcpd_start(SOIL_WIFI_AP_IF);
}

int soil_wifi_stop_ap(void)
{
  dhcpd_stop();
  netlib_ifdown(SOIL_WIFI_AP_IF);
  return 0;
}

int soil_wifi_scan_json(char *buffer, size_t buffer_size)
{
  struct wapi_list_s list;
  struct wapi_scan_info_s *item;
  size_t used = 0;
  int sock;
  int ret;
  int attempt;
  bool first = true;

  if (buffer_size < 3)
    {
      return -ENOSPC;
    }

  memset(&list, 0, sizeof(list));
  sock = wapi_make_socket();
  if (sock < 0)
    {
      return sock;
    }

  ret = wapi_scan_init(sock, SOIL_WIFI_STA_IF, NULL);
  if (ret < 0)
    {
      close(sock);
      return ret;
    }

  for (attempt = 0; attempt < 60; attempt++)
    {
      ret = wapi_scan_stat(sock, SOIL_WIFI_STA_IF);
      if (ret == 0)
        {
          break;
        }

      usleep(100000);
    }

  ret = wapi_scan_coll(sock, SOIL_WIFI_STA_IF, &list);
  close(sock);
  if (ret < 0)
    {
      return ret;
    }

  buffer[used++] = '[';
  buffer[used] = '\0';

  for (item = list.head.scan; item != NULL; item = item->next)
    {
      char escaped[WAPI_ESSID_MAX_SIZE * 2 + 1];
      int channel = 0;
      int written;

      if (item->essid[0] == '\0')
        {
          continue;
        }

      json_escape(escaped, sizeof(escaped), item->essid);
      if (item->has_freq)
        {
          wapi_freq2chan(sock, SOIL_WIFI_STA_IF, item->freq, &channel);
        }

      written = snprintf(buffer + used, buffer_size - used,
                         "%s{\"ssid\":\"%s\",\"rssi\":%d,\"channel\":%d}",
                         first ? "" : ",", escaped,
                         item->has_rssi ? item->rssi : -127,
                         channel);
      if (written < 0 || used + (size_t)written + 2 >= buffer_size)
        {
          break;
        }

      used += (size_t)written;
      first = false;
    }

  buffer[used++] = ']';
  buffer[used] = '\0';
  wapi_scan_coll_free(&list);
  return 0;
}
