#include <nuttx/config.h>

#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include <mqtt.h>

#if defined(CONFIG_NETUTILS_MQTTC_WITH_MBEDTLS)
#  include <mbedtls/ctr_drbg.h>
#  include <mbedtls/entropy.h>
#  include <mbedtls/net_sockets.h>
#  include <mbedtls/ssl.h>
#  include <mbedtls/x509_crt.h>
#endif

#include "soil_app.h"
#include "soil_cloud_config.h"
#include "soil_mqtt.h"
#include "soil_mqtt_ca.h"

#define SOIL_MQTT_TX_SIZE 512
#define SOIL_MQTT_RX_SIZE 512
#define SOIL_MQTT_PUBLISH_POLLS 80
#define SOIL_TLS_HANDSHAKE_POLLS 100

struct soil_mqtt_context_s
{
  struct mqtt_client client;
  uint8_t tx_buffer[SOIL_MQTT_TX_SIZE];
  uint8_t rx_buffer[SOIL_MQTT_RX_SIZE];
#if defined(CONFIG_NETUTILS_MQTTC_WITH_MBEDTLS) && SOIL_MQTT_USE_TLS
  mbedtls_ssl_context ssl;
  mbedtls_ssl_config ssl_config;
  mbedtls_x509_crt ca_cert;
  mbedtls_entropy_context entropy;
  mbedtls_ctr_drbg_context ctr_drbg;
  int socket_fd;
  bool tls_ready;
#endif
};

static int mqtt_socket_connect(const struct soil_settings_s *settings)
{
  struct addrinfo hints;
  struct addrinfo *servinfo;
  struct addrinfo *item;
  char port[8];
  int fd = -1;
  int ret;

  memset(&hints, 0, sizeof(hints));
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_STREAM;
  snprintf(port, sizeof(port), "%u", SOIL_MQTT_PORT);

  ret = getaddrinfo(SOIL_MQTT_HOST, port, &hints, &servinfo);
  if (ret != 0)
    {
      return -EHOSTUNREACH;
    }

  for (item = servinfo; item != NULL; item = item->ai_next)
    {
      fd = socket(item->ai_family, item->ai_socktype, item->ai_protocol);
      if (fd < 0)
        {
          continue;
        }

      if (connect(fd, item->ai_addr, item->ai_addrlen) == 0)
        {
          break;
        }

      close(fd);
      fd = -1;
    }

  freeaddrinfo(servinfo);
  if (fd < 0)
    {
      return -ECONNREFUSED;
    }

  ret = fcntl(fd, F_GETFL, 0);
  if (ret < 0 || fcntl(fd, F_SETFL, ret | O_NONBLOCK) < 0)
    {
      close(fd);
      return -errno;
    }

  UNUSED(settings);
  return fd;
}

#if defined(CONFIG_NETUTILS_MQTTC_WITH_MBEDTLS) && SOIL_MQTT_USE_TLS
static int tls_send(void *context, const unsigned char *buffer, size_t length)
{
  int fd = (int)(intptr_t)context;
  ssize_t ret = send(fd, buffer, length, 0);

  if (ret < 0)
    {
      if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
        {
          return MBEDTLS_ERR_SSL_WANT_WRITE;
        }

      return MBEDTLS_ERR_NET_SEND_FAILED;
    }

  return (int)ret;
}

static int tls_recv(void *context, unsigned char *buffer, size_t length)
{
  int fd = (int)(intptr_t)context;
  ssize_t ret = recv(fd, buffer, length, 0);

  if (ret < 0)
    {
      if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
        {
          return MBEDTLS_ERR_SSL_WANT_READ;
        }

      return MBEDTLS_ERR_NET_RECV_FAILED;
    }

  if (ret == 0)
    {
      return MBEDTLS_ERR_NET_CONN_RESET;
    }

  return (int)ret;
}

static void mqtt_transport_close(struct soil_mqtt_context_s *context)
{
  /* The ESP32 HAL mbedTLS build in this tree crashes when zeroizing the
   * dynamically allocated TLS buffers during ssl_free().  A soil cycle runs
   * in a fresh boot and deep sleep reclaims the heap, so close the socket and
   * let the reset release these allocations.  Do not call this path in a
   * long-lived, power-save-disabled deployment without bounded reboots.
   */

  context->tls_ready = false;
  close(context->socket_fd);
}

static int tls_open(struct soil_mqtt_context_s *context, int fd)
{
  static const char personalization[] = "soil-node-tls";
  int attempt;
  int ret;

  context->socket_fd = fd;
  esp_mbedtls_ssl_init(&context->ssl);
  esp_mbedtls_ssl_config_init(&context->ssl_config);
  esp_mbedtls_x509_crt_init(&context->ca_cert);
  esp_mbedtls_entropy_init(&context->entropy);
  esp_mbedtls_ctr_drbg_init(&context->ctr_drbg);

  ret = esp_mbedtls_x509_crt_parse(&context->ca_cert,
                                   (const unsigned char *)SOIL_MQTT_ROOT_CA_PEM,
                                   sizeof(SOIL_MQTT_ROOT_CA_PEM));
  if (ret != 0)
    {
      soil_log("TLS_CA_PARSE_FAILED ret=%d\n", ret);
      goto failed;
    }

  ret = esp_mbedtls_ctr_drbg_seed(&context->ctr_drbg,
                                  esp_mbedtls_entropy_func,
                                  &context->entropy,
                                  (const unsigned char *)personalization,
                                  sizeof(personalization) - 1);
  if (ret != 0)
    {
      soil_log("TLS_RNG_SEED_FAILED ret=%d\n", ret);
      goto failed;
    }

  ret = esp_mbedtls_ssl_config_defaults(&context->ssl_config,
                                        MBEDTLS_SSL_IS_CLIENT,
                                        MBEDTLS_SSL_TRANSPORT_STREAM,
                                        MBEDTLS_SSL_PRESET_DEFAULT);
  if (ret != 0)
    {
      soil_log("TLS_CONFIG_FAILED ret=%d\n", ret);
      goto failed;
    }

  esp_mbedtls_ssl_conf_authmode(&context->ssl_config,
                                MBEDTLS_SSL_VERIFY_REQUIRED);
  esp_mbedtls_ssl_conf_ca_chain(&context->ssl_config, &context->ca_cert, NULL);
  esp_mbedtls_ssl_conf_rng(&context->ssl_config,
                           esp_mbedtls_ctr_drbg_random,
                           &context->ctr_drbg);

  ret = esp_mbedtls_ssl_setup(&context->ssl, &context->ssl_config);
  if (ret != 0)
    {
      soil_log("TLS_SETUP_FAILED ret=%d\n", ret);
      goto failed;
    }

  ret = esp_mbedtls_ssl_set_hostname(&context->ssl, SOIL_MQTT_HOST);
  if (ret != 0)
    {
      soil_log("TLS_HOSTNAME_FAILED ret=%d\n", ret);
      goto failed;
    }

  esp_mbedtls_ssl_set_bio(&context->ssl, (void *)(intptr_t)fd,
                          tls_send, tls_recv, NULL);

  for (attempt = 0; attempt < SOIL_TLS_HANDSHAKE_POLLS; attempt++)
    {
      ret = esp_mbedtls_ssl_handshake(&context->ssl);
      if (ret == 0)
        {
          uint32_t verify_result =
            esp_mbedtls_ssl_get_verify_result(&context->ssl);

          if (verify_result != 0)
            {
              soil_log("TLS_CERT_INVALID verify=%lu\n",
                       (unsigned long)verify_result);
              goto failed;
            }

          context->tls_ready = true;
          soil_log("TLS_READY host=%s cert=verified verify=0\n",
                   SOIL_MQTT_HOST);
          return 0;
        }

      if (ret != MBEDTLS_ERR_SSL_WANT_READ &&
          ret != MBEDTLS_ERR_SSL_WANT_WRITE)
        {
          soil_log("TLS_HANDSHAKE_FAILED ret=%d verify=%u\n", ret,
                   (unsigned int)
                   esp_mbedtls_ssl_get_verify_result(&context->ssl));
          goto failed;
        }

      usleep(100000);
    }

  soil_log("TLS_HANDSHAKE_TIMEOUT\n");

failed:
  context->tls_ready = false;
  return -EIO;
}
#endif

static int mqtt_open(struct soil_mqtt_context_s *context,
                     const struct soil_settings_s *settings)
{
  struct mqtt_queued_message *message;
  int fd;
  int attempt;
  int ret;

  memset(context, 0, sizeof(*context));
  fd = mqtt_socket_connect(settings);
  if (fd < 0)
    {
      return fd;
    }

#if defined(CONFIG_NETUTILS_MQTTC_WITH_MBEDTLS) && SOIL_MQTT_USE_TLS
  ret = tls_open(context, fd);
  if (ret < 0)
    {
      close(fd);
      return ret;
    }

  ret = mqtt_init(&context->client, &context->ssl,
                  context->tx_buffer, sizeof(context->tx_buffer),
                  context->rx_buffer, sizeof(context->rx_buffer), NULL);
#else
#  if SOIL_MQTT_USE_TLS
  close(fd);
  soil_log("MQTT_TLS_NOT_COMPILED\n");
  return -ENOTSUP;
#  else
  ret = mqtt_init(&context->client, fd,
                  context->tx_buffer, sizeof(context->tx_buffer),
                  context->rx_buffer, sizeof(context->rx_buffer), NULL);
#  endif
#endif
  if (ret != MQTT_OK)
    {
#if defined(CONFIG_NETUTILS_MQTTC_WITH_MBEDTLS) && SOIL_MQTT_USE_TLS
      mqtt_transport_close(context);
#else
      close(fd);
#endif
      return -EIO;
    }

  ret = mqtt_connect(&context->client, SOIL_MQTT_CLIENT_ID,
                     NULL, NULL, 0,
                     settings->mqtt_user[0] != '\0' ? settings->mqtt_user : NULL,
                     settings->mqtt_password[0] != '\0' ?
                     settings->mqtt_password : NULL,
                     0, 60);
  if (ret != MQTT_OK)
    {
#if defined(CONFIG_NETUTILS_MQTTC_WITH_MBEDTLS) && SOIL_MQTT_USE_TLS
      mqtt_transport_close(context);
#else
      close(fd);
#endif
      return -EIO;
    }

  for (attempt = 0; attempt < 60; attempt++)
    {
      mqtt_sync(&context->client);
      message = mqtt_mq_find(&context->client.mq, MQTT_CONTROL_CONNECT, NULL);
      if (message == NULL || message->state == MQTT_QUEUED_COMPLETE)
        {
          return 0;
        }

      usleep(100000);
    }

#if defined(CONFIG_NETUTILS_MQTTC_WITH_MBEDTLS) && SOIL_MQTT_USE_TLS
  mqtt_transport_close(context);
#else
  close(fd);
#endif
  return -ETIMEDOUT;
}

static void mqtt_close(struct soil_mqtt_context_s *context)
{
  mqtt_disconnect(&context->client);
  mqtt_sync(&context->client);

#if defined(CONFIG_NETUTILS_MQTTC_WITH_MBEDTLS) && SOIL_MQTT_USE_TLS
  mqtt_transport_close(context);
#else
  close(context->client.socketfd);
#endif
}

int soil_mqtt_test_connection(const struct soil_settings_s *settings)
{
  struct soil_mqtt_context_s *context;
  int ret;

  context = calloc(1, sizeof(*context));
  if (context == NULL)
    {
      return -ENOMEM;
    }

  ret = mqtt_open(context, settings);
  if (ret < 0)
    {
      free(context);
      return ret;
    }

  mqtt_close(context);
  free(context);
  return 0;
}

int soil_mqtt_publish_json(const struct soil_settings_s *settings,
                           const char *payload)
{
  struct soil_mqtt_context_s *context;
  struct mqtt_queued_message *message;
  int attempt;
  int ret;

  context = calloc(1, sizeof(*context));
  if (context == NULL)
    {
      return -ENOMEM;
    }

  ret = mqtt_open(context, settings);
  if (ret < 0)
    {
      free(context);
      return ret;
    }

  ret = mqtt_publish(&context->client, SOIL_MQTT_TOPIC,
                     payload, strlen(payload), MQTT_PUBLISH_QOS_1);
  if (ret != MQTT_OK)
    {
      mqtt_close(context);
      free(context);
      return -EIO;
    }

  for (attempt = 0; attempt < SOIL_MQTT_PUBLISH_POLLS; attempt++)
    {
      mqtt_sync(&context->client);
      message = mqtt_mq_find(&context->client.mq, MQTT_CONTROL_PUBLISH, NULL);
      if ((message == NULL || message->state == MQTT_QUEUED_COMPLETE) &&
          context->client.error == MQTT_OK)
        {
          break;
        }

      usleep(100000);
    }

  if (attempt >= SOIL_MQTT_PUBLISH_POLLS)
    {
      ret = -ETIMEDOUT;
    }
  else
    {
      ret = context->client.error == MQTT_OK ? 0 : -EIO;
    }

  mqtt_close(context);
  free(context);
  return ret;
}
