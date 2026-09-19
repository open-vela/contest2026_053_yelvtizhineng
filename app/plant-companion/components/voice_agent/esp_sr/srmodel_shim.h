#pragma once

/* esp_sr 模型注册表（srmodel 机制）NuttX 内存版 —— 见 srmodel_shim.c */
int srmodel_nuttx_init(void);

/* 静音 esp_sr 库内部的 ESP_LOGI 输出（USB-Serial-JTAG FIFO 满会阻塞） */
void esp_sr_log_silence(void);
