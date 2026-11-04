# -*- coding: utf-8 -*-
src = open("esp32s3_wifi_adapter.c").read()

# 1) disable esp_wifi lib auto reconnect right after STA start
a1 = '  ret = esp_wifi_start();\n  if (ret)\n    {\n      wlerr("Failed to start Wi-Fi with mode=%d ret=%d\\n", mode, ret);\n      ret = wifi_errno_trans(ret);\n      goto errout;\n    }\n\n  g_sta_started = true;'
assert a1 in src, "anchor1 missing"
src = src.replace(a1, a1.replace('  g_sta_started = true;',
  '  /* ZXB-dbg: explicit connect only (no lib auto-reconnect loop) */\n  esp_wifi_set_auto_connect(false);\n\n  g_sta_started = true;'), 1)

# 2) helper to raise/lower esp wifi log verbosity around connect attempts
helper = '''
static void dbg_wifi_logs(bool verbose)
{
  esp_log_level_set("wifi", verbose ? ESP_LOG_VERBOSE : ESP_LOG_WARN);
  esp_log_level_set("wpa_supplicant", verbose ? ESP_LOG_VERBOSE : ESP_LOG_WARN);
}

'''
anchor_h = "int esp_wifi_sta_connect(void)"
i = src.find(anchor_h)
assert i > 0, "esp_wifi_sta_connect anchor missing"
src = src[:i] + helper + src[i:]

# 3) raise logs at start of esp_wifi_sta_connect body (after lock, before set_config)
a3 = '  esp_wifi_lock(true);\n\n  if (g_sta_connected)\n    {\n      wlinfo("Wi-Fi has connected AP\\n");\n      esp_wifi_lock(false);\n      return OK;\n    }\n\n  g_sta_reconnect = true;'
assert a3 in src, "anchor3 missing"
src = src.replace(a3, '  esp_wifi_lock(true);\n\n  if (g_sta_connected)\n    {\n      wlinfo("Wi-Fi has connected AP\\n");\n      esp_wifi_lock(false);\n      return OK;\n    }\n\n  dbg_wifi_logs(true);\n\n  g_sta_reconnect = true;', 1)

# 4) lower logs at end (the timeout branch that returns -1)
a4 = '      g_sta_reconnect = false;\n      wlinfo("Failed to connect to AP\\n");\n      dbg_console("[WLANDBG] assoc TIMEOUT last_reason=%d\\n",\n                  g_dbg_last_disconnect_reason);\n      return -1;'
assert a4 in src, "anchor4 missing"
src = src.replace(a4, a4.replace('      return -1;', '      dbg_wifi_logs(false);\n      return -1;'), 1)

open("esp32s3_wifi_adapter.c","w").write(src)
print("patched OK")
