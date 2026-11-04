import io

path = "esp32s3_wifi_adapter.c"
src = open(path, encoding="utf-8").read()

anchor = """  wifi_cfg.dynamic_tx_buf_num = CONFIG_ESP32S3_WIFI_DYNAMIC_TXBUF_NUM;

  ret = esp_wifi_init(&wifi_cfg);"""
assert anchor in src, "anchor missing"

add = """  wifi_cfg.dynamic_tx_buf_num = CONFIG_ESP32S3_WIFI_DYNAMIC_TXBUF_NUM;

  /* ZXB-WIFI-TX: SPI-RAM 板卡在 NuttX 移植下 CONFIG_SPIRAM 未透传给 esp_wifi,
   * 默认 cache_tx_buf_num=0, 连 AP 时管理帧(probe/auth)无法发出;
   * 显式给内部缓存 TX 缓冲池以恢复连接。
   */
  wifi_cfg.cache_tx_buf_num = 16;

  wlinfo("ZXB-WIFI init cfg: cache_tx=%d mgmt_sbuf=%d tx_type=%d st_tx=%d dyn_tx=%d feat=%llx\\n",
         wifi_cfg.cache_tx_buf_num, wifi_cfg.mgmt_sbuf_num, wifi_cfg.tx_buf_type,
         wifi_cfg.static_tx_buf_num, wifi_cfg.dynamic_tx_buf_num,
         (unsigned long long)wifi_cfg.feature_caps);

  ret = esp_wifi_init(&wifi_cfg);"""

src = src.replace(anchor, add, 1)
open(path, "w", encoding="utf-8").write(src)
print("patched OK")
