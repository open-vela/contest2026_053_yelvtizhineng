#include <nuttx/config.h>

#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include <nuttx/board.h>

#include "soil_cloud_config.h"
#include "soil_http.h"
#include "soil_mqtt.h"
#include "soil_state.h"
#include "soil_wifi.h"

#define SOIL_HTTP_PORT 80
#define SOIL_DNS_PORT 53
#define SOIL_HTTP_REQUEST_MAX 2048
#define SOIL_HTTP_CLIENT_TIMEOUT_SEC 5
#define SOIL_PORTAL_HARD_TIMEOUT_SEC (15 * 60)

static const char g_portal_page[] = R"HTML(
<!doctype html>
<html lang="zh-CN">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>植小伴设备配置</title>
<style>
:root{color-scheme:light;--ink:#163126;--muted:#65786c;--line:#cbd8cf;--ok:#176b3a;--warn:#8a5a00;--paper:#fff}
*{box-sizing:border-box}body{margin:0;background:#eef4ed;font-family:-apple-system,BlinkMacSystemFont,"Segoe UI",sans-serif;color:var(--ink)}
main{max-width:680px;margin:0 auto;padding:28px 18px 50px}.brand{font-size:13px;color:var(--muted);margin-bottom:8px}h1{font-size:26px;line-height:1.2;margin:0 0 8px}.lead{color:var(--muted);line-height:1.6;margin:0 0 22px}
section{background:var(--paper);border:1px solid var(--line);border-radius:8px;padding:20px;margin:14px 0}section h2{font-size:16px;margin:0 0 16px}.grid{display:grid;grid-template-columns:repeat(2,minmax(0,1fr));gap:14px}.wide{grid-column:1/-1}
label{display:block;font-size:13px;font-weight:650;margin:0 0 6px}input,select{width:100%;border:1px solid var(--line);border-radius:6px;background:#fbfdfb;color:var(--ink);padding:11px 12px;font:inherit}input:focus,select:focus{outline:2px solid #c9a83f;outline-offset:0}
.check{display:flex;align-items:center;gap:9px;margin-top:18px;font-size:14px}.check input{width:18px;height:18px;margin:0}
.actions{display:flex;gap:10px;flex-wrap:wrap;margin-top:18px}button{border:0;border-radius:6px;padding:11px 16px;font:inherit;font-weight:650;cursor:pointer;background:var(--ok);color:#fff}button.secondary{background:#f5f8f5;color:var(--ink);border:1px solid var(--line)}button.danger{background:#fff;color:#9a2e22;border:1px solid #e2bdb7}
.status{min-height:24px;margin-top:14px;color:var(--warn);font-weight:650}.device{font-size:13px;line-height:1.8;color:var(--muted);word-break:break-all}.foot{font-size:12px;color:var(--muted);text-align:center;margin-top:18px}
@media(max-width:620px){main{padding:20px 12px 40px}.grid{grid-template-columns:1fr}h1{font-size:23px}section{padding:17px}}
</style>
</head>
<body><main>
<div class="brand">北京见壤智能科技</div>
<h1>植小伴土壤节点</h1>
<p class="lead">配置设备网络、采集周期和传感器参数。保存前会验证 Wi-Fi 与云端连接。</p>
<form id="config">
<section>
<h2>网络</h2>
<div class="grid">
<div class="wide"><label for="networks">附近网络</label><select id="networks"><option value="">正在扫描...</option></select></div>
<div class="wide"><label for="ssid">Wi-Fi 名称</label><input id="ssid" name="ssid" maxlength="32" required autocomplete="off"></div>
<div class="wide"><label for="password">Wi-Fi 密码</label><input id="password" name="password" type="password" maxlength="64" autocomplete="current-password"></div>
</div>
</section>
<section>
<h2>采集与传感器</h2>
<div class="grid">
<div><label for="interval">采样/上报周期（秒）</label><input id="interval" name="interval" type="number" min="10" max="3600" required></div>
<div><label for="address">Modbus 地址</label><input id="address" name="address" type="number" min="1" max="247" required></div>
<div><label for="baud">Modbus 波特率</label><select id="baud" name="baud"><option>4800</option><option>9600</option><option>19200</option><option>38400</option></select></div>
<div><label for="temp_offset">温度偏移（°C）</label><input id="temp_offset" name="temp_offset" type="number" min="-20" max="20" step="0.1" required></div>
<div><label for="ph_offset">pH 偏移</label><input id="ph_offset" name="ph_offset" type="number" min="-5" max="5" step="0.1" required></div>
</div>
<label class="check"><input id="power_save" name="power_save" type="checkbox"> 周期上报后深度睡眠（电池供电推荐）</label>
</section>
<section>
<h2>云端凭据</h2>
<div class="grid">
<div><label for="mqtt_user">MQTT 用户名</label><input id="mqtt_user" name="mqtt_user" maxlength="64" autocomplete="username"></div>
<div><label for="mqtt_password">MQTT 密码</label><input id="mqtt_password" name="mqtt_password" type="password" maxlength="96" autocomplete="current-password"></div>
</div>
<p class="device">凭据留空时保留设备内已有配置。凭据只写入设备本地，不返回到网页。</p>
</section>
<section>
<h2>最近状态</h2>
<div class="device" id="runtime">正在读取...</div>
</section>
<section>
<h2>设备信息</h2>
<div class="device" id="device">正在读取...</div>
</section>
<div class="actions"><button id="save" type="submit">验证并保存</button><button id="rescan" class="secondary" type="button">重新扫描</button><button id="reset" class="danger" type="button">清除全部配置</button></div>
<div class="status" id="status"></div>
</form>
<div class="foot">设备热点：ZXB-Soil-xxxxxx · 配置地址：http://192.168.4.1</div>
</main>
<script>
const form=document.getElementById('config'),statusEl=document.getElementById('status'),scanEl=document.getElementById('networks');
const ssid=document.getElementById('ssid');
function setStatus(t){statusEl.textContent=t}
scanEl.onchange=()=>{if(scanEl.value)ssid.value=scanEl.value};
async function scan(){setStatus('正在扫描 Wi-Fi...');try{const r=await fetch('/api/scan');const list=await r.json();scanEl.innerHTML='<option value="">选择附近网络</option>';list.sort((a,b)=>b.rssi-a.rssi).forEach(n=>{const o=document.createElement('option');o.value=n.ssid;o.textContent=n.ssid+'  '+n.rssi+' dBm';scanEl.appendChild(o)});setStatus(list.length?'':'未扫描到网络，可手动输入') }catch(e){setStatus('扫描失败，可手动输入 Wi-Fi 名称')}}
async function load(){try{const r=await fetch('/api/settings');const s=await r.json();document.getElementById('interval').value=s.interval_s;document.getElementById('address').value=s.modbus_addr;document.getElementById('baud').value=String(s.modbus_baud);document.getElementById('temp_offset').value=s.temp_offset;document.getElementById('ph_offset').value=s.ph_offset;document.getElementById('power_save').checked=!!s.power_save;ssid.value=s.wifi_ssid||'';document.getElementById('device').innerHTML='设备 ID：'+s.device_id+'<br>上报主题：'+s.topic+'<br>云端：'+s.mqtt_host+':'+s.mqtt_port+(s.mqtt_tls?' · TLS':'')+'<br>云端凭据：'+(s.mqtt_user_configured?'已配置':'未配置')}catch(e){setStatus('参数读取失败，请刷新页面')}}
async function refreshStatus(){try{const r=await fetch('/api/status');const s=await r.json();const d=s.last_reading||{};const when=s.last_update?new Date(s.last_update*1000).toLocaleString():'暂无';document.getElementById('runtime').innerHTML='启动次数：'+s.boot_count+'<br>最近采样：'+when+'<br>Wi-Fi 信号：'+(s.rssi?s.rssi+' dBm':'暂无')+'<br>采样 / 上报：'+(s.sample_ok?'成功':'失败')+' / '+(s.publish_ok?'成功':'失败')+'<br>水分：'+(d.moisture??'--')+'% · 温度：'+(d.temp??'--')+'°C<br>EC：'+(d.ec??'--')+' · pH：'+(d.ph??'--')+'<br>氮磷钾：'+(d.nitrogen??'--')+' / '+(d.phosphorus??'--')+' / '+(d.potassium??'--')}catch(e){document.getElementById('runtime').textContent='状态读取失败'}}
document.getElementById('rescan').onclick=scan;
document.getElementById('reset').onclick=async()=>{if(!confirm('确定清除 Wi-Fi 和全部参数？'))return;setStatus('正在清除...');try{const r=await fetch('/api/reset',{method:'POST'});setStatus(r.ok?'配置已清除，请重新配网':'清除失败')}catch(e){setStatus('清除失败')}};
form.onsubmit=async e=>{e.preventDefault();const submit=document.getElementById('save');submit.disabled=true;setStatus('正在验证网络和云端，请稍候...');const data=new URLSearchParams(new FormData(form));data.set('power_save',document.getElementById('power_save').checked?'1':'0');try{const r=await fetch('/api/save',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:data});const text=await r.text();setStatus(text);if(r.ok){setTimeout(()=>window.location.reload(),2200)}}catch(e){setStatus('保存失败，请检查网络后重试')}finally{submit.disabled=false}};
scan();load();
refreshStatus();setInterval(refreshStatus,5000);
</script>
</body></html>
)HTML";

static int send_all(int fd, const void *data, size_t length)
{
  const uint8_t *bytes = data;
  size_t sent = 0;

  while (sent < length)
    {
      ssize_t ret = send(fd, bytes + sent, length - sent, 0);
      if (ret < 0)
        {
          if (errno == EINTR)
            {
              continue;
            }

          return -errno;
        }

      if (ret == 0)
        {
          return -EIO;
        }

      sent += (size_t)ret;
    }

  return 0;
}

static void send_response(int fd, int status, const char *content_type,
                          const char *body)
{
  char header[256];
  const char *reason = status == 200 ? "OK" :
                       status == 400 ? "Bad Request" :
                       status == 404 ? "Not Found" :
                       status == 405 ? "Method Not Allowed" :
                       status == 503 ? "Service Unavailable" : "Found";
  int length = (int)strlen(body);
  int header_length = snprintf(header, sizeof(header),
                               "HTTP/1.1 %d %s\r\n"
                               "Content-Type: %s\r\n"
                               "Content-Length: %d\r\n"
                               "Cache-Control: no-store\r\n"
                               "Connection: close\r\n\r\n",
                               status, reason, content_type, length);
  soil_log("HTTP_RESPONSE status=%d body=%d\n", status, length);
  send_all(fd, header, (size_t)header_length);
  send_all(fd, body, (size_t)length);
}

static void redirect_to_portal(int fd)
{
  static const char header[] =
    "HTTP/1.1 302 Found\r\nLocation: http://192.168.4.1/\r\n"
    "Content-Length: 0\r\nConnection: close\r\n\r\n";
  send_all(fd, header, sizeof(header) - 1);
}

static void url_decode(const char *source, size_t source_length,
                       char *dest, size_t dest_size)
{
  size_t in = 0;
  size_t out = 0;

  while (in < source_length && out + 1 < dest_size)
    {
      if (source[in] == '+')
        {
          dest[out++] = ' ';
          in++;
        }
      else if (source[in] == '%' && in + 2 < source_length &&
               isxdigit((unsigned char)source[in + 1]) &&
               isxdigit((unsigned char)source[in + 2]))
        {
          char hex[3] = {source[in + 1], source[in + 2], '\0'};
          dest[out++] = (char)strtoul(hex, NULL, 16);
          in += 3;
        }
      else
        {
          dest[out++] = source[in++];
        }
    }

  dest[out] = '\0';
}

static bool form_get(const char *body, const char *key,
                     char *value, size_t value_size)
{
  size_t key_length = strlen(key);
  const char *cursor = body;

  while (cursor != NULL && *cursor != '\0')
    {
      const char *end = strchr(cursor, '&');
      size_t part_length = end == NULL ? strlen(cursor) :
                           (size_t)(end - cursor);

      if (part_length > key_length && cursor[key_length] == '=' &&
          strncmp(cursor, key, key_length) == 0)
        {
          url_decode(cursor + key_length + 1,
                     part_length - key_length - 1, value, value_size);
          return true;
        }

      cursor = end == NULL ? NULL : end + 1;
    }

  if (value_size > 0)
    {
      value[0] = '\0';
    }

  return false;
}

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

static void handle_settings(int fd, const struct soil_settings_s *settings)
{
  char ssid[SOIL_SSID_MAX * 2 + 1];
  char json[1024];

  json_escape(ssid, sizeof(ssid), settings->wifi_ssid);
  snprintf(json, sizeof(json),
           "{\"wifi_ssid\":\"%s\",\"interval_s\":%lu,"
           "\"modbus_addr\":%u,\"modbus_baud\":%lu,"
           "\"temp_offset\":%d.%d,\"ph_offset\":%d.%d,"
           "\"power_save\":%u,\"device_id\":\"%s\","
           "\"topic\":\"%s\",\"mqtt_host\":\"%s\",\"mqtt_port\":%u,"
           "\"mqtt_tls\":%u,\"mqtt_user_configured\":%u}",
           ssid, (unsigned long)settings->interval_sec,
           settings->modbus_addr, (unsigned long)settings->modbus_baud,
           settings->temp_offset_x10 / 10,
           abs(settings->temp_offset_x10 % 10),
           settings->ph_offset_x10 / 10, abs(settings->ph_offset_x10 % 10),
           settings->power_save ? 1 : 0,
           SOIL_DEVICE_ID, SOIL_MQTT_TOPIC, SOIL_MQTT_HOST, SOIL_MQTT_PORT,
           SOIL_MQTT_USE_TLS, settings->mqtt_user[0] != '\0' ? 1 : 0);
  send_response(fd, 200, "application/json; charset=utf-8", json);
}

static void handle_scan(int fd)
{
  char json[4096];
  int ret = soil_wifi_scan_json(json, sizeof(json));

  if (ret < 0)
    {
      snprintf(json, sizeof(json), "{\"error\":%d}", ret);
      send_response(fd, 503, "application/json; charset=utf-8", json);
      return;
    }

  send_response(fd, 200, "application/json; charset=utf-8", json);
}

static void handle_status(int fd)
{
  char json[768];
  int ret = soil_state_json(json, sizeof(json));

  if (ret < 0)
    {
      send_response(fd, 503, "application/json; charset=utf-8",
                    "{\"error\":1}");
      return;
    }

  send_response(fd, 200, "application/json; charset=utf-8", json);
}

static void handle_dns_query(int fd)
{
  static const uint8_t portal_ip[4] = {192, 168, 4, 1};
  struct sockaddr_in client;
  socklen_t client_length = sizeof(client);
  uint8_t query[512];
  uint8_t response[512];
  ssize_t received;
  size_t question_end = 12;
  size_t response_length;
  uint16_t qtype;
  uint16_t qclass;
  bool answer = false;

  received = recvfrom(fd, query, sizeof(query), 0,
                      (struct sockaddr *)&client, &client_length);
  if (received < 17)
    {
      return;
    }

  while (question_end < (size_t)received)
    {
      uint8_t label = query[question_end++];
      if (label == 0)
        {
          break;
        }

      if ((label & 0xc0) != 0 || question_end + label > (size_t)received)
        {
          return;
        }

      question_end += label;
    }

  if (question_end + 4 > (size_t)received)
    {
      return;
    }

  qtype = ((uint16_t)query[question_end] << 8) | query[question_end + 1];
  qclass = ((uint16_t)query[question_end + 2] << 8) |
            query[question_end + 3];
  question_end += 4;
  answer = qtype == 1 && qclass == 1;

  memset(response, 0, sizeof(response));
  response[0] = query[0];
  response[1] = query[1];
  response[2] = 0x81;
  response[3] = 0x80;
  response[5] = 1;
  response[7] = answer ? 1 : 0;
  memcpy(response + 12, query + 12, question_end - 12);
  response_length = question_end;

  if (answer)
    {
      response[response_length++] = 0xc0;
      response[response_length++] = 0x0c;
      response[response_length++] = 0x00;
      response[response_length++] = 0x01;
      response[response_length++] = 0x00;
      response[response_length++] = 0x01;
      response[response_length++] = 0x00;
      response[response_length++] = 0x00;
      response[response_length++] = 0x00;
      response[response_length++] = 0x3c;
      response[response_length++] = 0x00;
      response[response_length++] = 0x04;
      memcpy(response + response_length, portal_ip, sizeof(portal_ip));
      response_length += sizeof(portal_ip);
    }

  sendto(fd, response, response_length, 0,
         (struct sockaddr *)&client, client_length);
  soil_log("DNS_PORTAL_REDIRECT answer=%u\n", answer ? 1 : 0);
}

static bool handle_save(int fd, const char *body,
                        struct soil_settings_s *settings)
{
  struct soil_settings_s next = *settings;
  char value[128];
  char error[128];
  char ip[INET_ADDRSTRLEN];
  char message[256];
  int ret;

  if (!form_get(body, "ssid", next.wifi_ssid, sizeof(next.wifi_ssid)) ||
      next.wifi_ssid[0] == '\0')
    {
      send_response(fd, 400, "text/plain; charset=utf-8",
                    "Wi-Fi 名称不能为空");
      return false;
    }

  form_get(body, "password", next.wifi_password,
           sizeof(next.wifi_password));
  if (form_get(body, "mqtt_user", value, sizeof(value)) && value[0] != '\0')
    {
      strncpy(next.mqtt_user, value, sizeof(next.mqtt_user) - 1);
      next.mqtt_user[sizeof(next.mqtt_user) - 1] = '\0';
    }

  if (form_get(body, "mqtt_password", value, sizeof(value)) &&
      value[0] != '\0')
    {
      strncpy(next.mqtt_password, value, sizeof(next.mqtt_password) - 1);
      next.mqtt_password[sizeof(next.mqtt_password) - 1] = '\0';
    }

  if (form_get(body, "interval", value, sizeof(value)))
    {
      next.interval_sec = strtoul(value, NULL, 10);
    }

  if (form_get(body, "address", value, sizeof(value)))
    {
      next.modbus_addr = (uint8_t)strtoul(value, NULL, 10);
    }

  if (form_get(body, "baud", value, sizeof(value)))
    {
      next.modbus_baud = strtoul(value, NULL, 10);
    }

  if (form_get(body, "temp_offset", value, sizeof(value)))
    {
      next.temp_offset_x10 = (int16_t)(strtod(value, NULL) * 10.0);
    }

  if (form_get(body, "ph_offset", value, sizeof(value)))
    {
      next.ph_offset_x10 = (int16_t)(strtod(value, NULL) * 10.0);
    }

  if (form_get(body, "power_save", value, sizeof(value)))
    {
      next.power_save = strtoul(value, NULL, 10) != 0;
    }

  if (soil_settings_validate(&next, error, sizeof(error)) < 0)
    {
      send_response(fd, 400, "text/plain; charset=utf-8", error);
      return false;
    }

  if (soil_wifi_station_is_connected() &&
      strcmp(next.wifi_ssid, settings->wifi_ssid) == 0 &&
      strcmp(next.wifi_password, settings->wifi_password) == 0)
    {
      soil_log("WIFI_REUSE_CONNECTED SSID=%s\n", next.wifi_ssid);
    }
  else
    {
      ret = soil_wifi_station_connect(&next, ip, sizeof(ip));
      if (ret < 0)
        {
          snprintf(message, sizeof(message),
                   "Wi-Fi 连接失败，请检查名称、密码和 2.4 GHz 网络 (%d)",
                   ret);
          send_response(fd, 400, "text/plain; charset=utf-8", message);
          return false;
        }
    }

  ret = soil_mqtt_test_connection(&next);
  if (ret < 0)
    {
      snprintf(message, sizeof(message), "云端连接失败 (%d)", ret);
      send_response(fd, 400, "text/plain; charset=utf-8", message);
      return false;
    }

  ret = soil_settings_save(&next);
  if (ret < 0)
    {
      snprintf(message, sizeof(message), "配置写入失败 (%d)", ret);
      send_response(fd, 400, "text/plain; charset=utf-8", message);
      return false;
    }

  *settings = next;
  send_response(fd, 200, "text/plain; charset=utf-8",
                "配置成功，设备正在启动采集与上报");
  return true;
}

static bool handle_request(int fd, const char *request,
                           struct soil_settings_s *settings)
{
  char method[16];
  char path[128];
  const char *body;
  const char *header_end;

  if (sscanf(request, "%15s %127s", method, path) != 2)
    {
      send_response(fd, 400, "text/plain; charset=utf-8", "无效请求");
      return false;
    }

  header_end = strstr(request, "\r\n\r\n");
  body = header_end == NULL ? "" : header_end + 4;

  if (strcmp(method, "GET") == 0 && strcmp(path, "/") == 0)
    {
      send_response(fd, 200, "text/html; charset=utf-8", g_portal_page);
    }
  else if (strcmp(method, "GET") == 0 &&
           strcmp(path, "/api/settings") == 0)
    {
      handle_settings(fd, settings);
    }
  else if (strcmp(method, "GET") == 0 && strcmp(path, "/api/scan") == 0)
    {
      handle_scan(fd);
    }
  else if (strcmp(method, "GET") == 0 && strcmp(path, "/api/status") == 0)
    {
      handle_status(fd);
    }
  else if (strcmp(method, "POST") == 0 && strcmp(path, "/api/save") == 0)
    {
      return handle_save(fd, body, settings);
    }
  else if (strcmp(method, "POST") == 0 && strcmp(path, "/api/reset") == 0)
    {
      int ret = soil_settings_clear();

      if (ret < 0)
        {
          send_response(fd, 400, "text/plain; charset=utf-8",
                        "配置清除失败");
        }
      else
        {
          soil_defaults(settings);
          send_response(fd, 200, "text/plain; charset=utf-8",
                        "配置已清除，设备正在重启进入配网");
          sleep(1);
          board_reset(EXIT_SUCCESS);
        }
    }
  else
    {
      redirect_to_portal(fd);
    }

  return false;
}

int soil_http_portal_run(struct soil_settings_s *settings,
                         uint32_t timeout_ms)
{
  struct sockaddr_in address;
  struct pollfd pfd[2];
  char ap_ssid[SOIL_AP_SSID_MAX + 1];
  char request[SOIL_HTTP_REQUEST_MAX + 1];
  struct timeval client_timeout;
  time_t hard_deadline;
  time_t idle_deadline;
  int dns_server = -1;
  int server;
  int reuse = 1;
  int ret;
  bool saved = false;

  ret = soil_wifi_make_ap_ssid(ap_ssid, sizeof(ap_ssid));
  if (ret < 0)
    {
      soil_log("PORTAL_AP_SSID_FAILED ret=%d\n", ret);
      return ret;
    }

  ret = soil_wifi_start_ap(ap_ssid);
  if (ret < 0)
    {
      soil_log("PORTAL_AP_START_FAILED ret=%d\n", ret);
      return ret;
    }

  server = socket(AF_INET, SOCK_STREAM, 0);
  if (server < 0)
    {
      soil_wifi_stop_ap();
      return -errno;
    }

  setsockopt(server, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
  memset(&address, 0, sizeof(address));
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_ANY);
  address.sin_port = htons(SOIL_HTTP_PORT);

  if (bind(server, (struct sockaddr *)&address, sizeof(address)) < 0 ||
      listen(server, 4) < 0)
    {
      ret = -errno;
      close(server);
      soil_wifi_stop_ap();
      return ret;
    }

  dns_server = socket(AF_INET, SOCK_DGRAM, 0);
  if (dns_server >= 0)
    {
      memset(&address, 0, sizeof(address));
      address.sin_family = AF_INET;
      address.sin_addr.s_addr = htonl(INADDR_ANY);
      address.sin_port = htons(SOIL_DNS_PORT);
      setsockopt(dns_server, SOL_SOCKET, SO_REUSEADDR,
                 &reuse, sizeof(reuse));
      if (bind(dns_server, (struct sockaddr *)&address,
               sizeof(address)) < 0)
        {
          close(dns_server);
          dns_server = -1;
          soil_log("DNS_PORTAL_BIND_FAILED\n");
        }
    }

  soil_log("PROVISIONING_AP SSID=%s URL=http://192.168.4.1\n", ap_ssid);
  pfd[0].fd = server;
  pfd[0].events = POLLIN;
  pfd[1].fd = dns_server;
  pfd[1].events = POLLIN;
  idle_deadline = time(NULL) + (time_t)(timeout_ms / 1000);
  hard_deadline = time(NULL) + SOIL_PORTAL_HARD_TIMEOUT_SEC;
  client_timeout.tv_sec = SOIL_HTTP_CLIENT_TIMEOUT_SEC;
  client_timeout.tv_usec = 0;

  while (!saved && time(NULL) < idle_deadline &&
         time(NULL) < hard_deadline)
    {
      int client;
      size_t used = 0;
      size_t content_length = 0;
      const char *header_end;

      pfd[0].revents = 0;
      pfd[1].revents = 0;
      ret = poll(pfd, dns_server >= 0 ? 2 : 1, 500);
      if (ret <= 0)
        {
          continue;
        }

      if (dns_server >= 0 && (pfd[1].revents & POLLIN) != 0)
        {
          handle_dns_query(dns_server);
        }

      if ((pfd[0].revents & POLLIN) == 0)
        {
          continue;
        }

      client = accept(server, NULL, NULL);
      if (client < 0)
        {
          continue;
        }

      soil_log("HTTP_CLIENT_ACCEPTED\n");
      setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &client_timeout,
                 sizeof(client_timeout));
      idle_deadline = time(NULL) + (time_t)(timeout_ms / 1000);

      while (used < SOIL_HTTP_REQUEST_MAX)
        {
          ssize_t got = recv(client, request + used,
                             SOIL_HTTP_REQUEST_MAX - used, 0);
          if (got <= 0)
            {
              break;
            }

          used += (size_t)got;
          idle_deadline = time(NULL) + (time_t)(timeout_ms / 1000);
          request[used] = '\0';
          header_end = strstr(request, "\r\n\r\n");
          if (header_end != NULL)
            {
              const char *length_header = strstr(request, "Content-Length:");
              if (length_header != NULL)
                {
                  content_length = strtoul(length_header +
                                           strlen("Content-Length:"), NULL, 10);
                }

              if (used >= (size_t)(header_end + 4 - request) + content_length)
                {
                  break;
                }
            }
        }

      request[used] = '\0';
      soil_log("HTTP_REQUEST len=%u\n", (unsigned int)used);
      saved = handle_request(client, request, settings);
      close(client);
    }

  close(server);
  if (dns_server >= 0)
    {
      close(dns_server);
    }
  if (saved)
    {
      soil_log("PROVISIONING_SAVED_SSID=%s\n", settings->wifi_ssid);
      sleep(1);
    }
  else
    {
      if (time(NULL) >= hard_deadline)
        {
          soil_log("PROVISIONING_HARD_TIMEOUT\n");
        }
      else
        {
          soil_log("PROVISIONING_IDLE_TIMEOUT\n");
        }
    }

  soil_wifi_stop_ap();
  return saved ? 1 : 0;
}
