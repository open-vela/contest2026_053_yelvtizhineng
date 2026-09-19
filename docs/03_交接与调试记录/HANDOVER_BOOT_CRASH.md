# 交接文档：启动复位循环排查（ESP32-S3-BOX / OpenVela / NuttX）

> 本文档记录 2026-08-25 排查"固件启动即复位循环"的全部工作、症状、证据链、
> 已做改动与下一步建议。**交新话题继续排查用，新话题先通读本文档再动手。**

---

## 1. 目标设备与环境

- 硬件：Xiaomi ESP32-S3-BOX-3（CHD 板），ST7796 480×320 触摸屏，OV3660 摄像头
- 系统：OpenVela（NuttX flat build），`apps/plant-companion` 应用，NSH builtin `plant`
- 构建：`./build.sh vendor/espressif/boards/esp32s3/esp32s3-box/configs/openvela/`
- 烧录：esptool write-flash `0x10000 nuttx/nuttx.bin`（bootloader+分区表已烧好）
- 串口：`minicom -D /dev/ttyACM0 -b 115200`（**USB-Serial-JTAG**）
- 用户自己编译烧录；调试打印必须限量（USB-Serial-JTAG 4096B 缓冲，满了假卡死）

## 2. 症状演变（关键时间线）

### 阶段 A：无限复位循环（最早的日志）
- 现象：bootloader 加载 app 成功后立即 `rst:0x3 (RTC_SW_SYS_RST)` 无限循环，
  **NuttX 完全无输出**（连 LCD 初始化都没有）
- `Saved PC:0x400006e7` = `software_reset_cpu`(0x400006e4)+3
- 根因（已确认）：**堆为负**。旧配置 `.bss` 静态数据占满 dram0_0_seg，
  `_sheap=0x3fcda610` 越过堆顶 `dram0_rtos_reserved_start`(≈0x3fcd7e00)，
  `up_allocate_heap` 的 `DEBUGASSERT(utop > ubase)` 失败 → panic → 复位循环

### 阶段 B：能跑到 WiFi PHY 校准（堆修复后）
- 现象：`T7796 full init + RED fill` → `lcd: ST7796 init complete` →
  `[Touch] GT911 detected` → `[Touch] registered at /dev/input0` →
  `I (1766) phy_...` → 又复位循环
- **堆修复生效**：系统正常启动到 board bringup，WiFi PHY 校准崩溃
- 此阶段配置 = `.config.backup`（16:16 保存）：
  `BOOT_INIT=n` + `COMMON_HEAP=n` + `IGNORE_NOTFOUND=y`

### 阶段 C：又回到全崩（配置被改坏）
- 现象：**连 LCD 输出都没有**，纯 bootloader 循环，`Saved PC` 出现 `0x40044684`（ROM 内部异常处理地址）
- **根因（已确认）：我的 sed 把配置改反了**
  - 当时 `.config` 里 `COMMON_HEAP` 已是 `not set`（用户阶段 B 就关的）
  - 我执行 `s/^CONFIG_ESP32S3_SPIRAM_COMMON_HEAP=y/...not set/` 没匹配到 =y 行（无效）
  - 但 `s/^# CONFIG_ESP32S3_SPIRAM_IGNORE_NOTFOUND is not set/=y/` 把本已正确的 IGNORE 改没了
  - 结果：COMMON_HEAP 仍是 y（PSRAM 进堆）+ IGNORE 丢失 → 启动崩
- **已修复**：`.config` 恢复 `COMMON_HEAP=n` + `IGNORE_NOTFOUND=y`（与阶段 B 一致）

## 3. 已确认的技术事实（新话题勿再重复排查）

### 3.1 内存布局（Zephyr 官方权威，esp32s3/memory.h）
```
0x3fc88000  SRAM_DRAM_START (NuttX dram0_0_seg origin)
0x3fcd7e00  DRAM_BUFFERS_START（UART/USB/SPI 下载模式缓冲，运行时可回收）
0x3fce9710  PRO CPU stack（RTOS 启动后可回收为堆）
0x3fceb710  APP CPU stack（RTOS 启动后可回收为堆）
0x3fced710  ROM .bss/.data（不可回收）
0x3fcf0000  SOC_DIRAM_DRAM_HIGH
```
- **堆顶应取 0x3fced710**（不是 ROM 布局表的 dram0_rtos_reserved_start≈0x3fcd7e00）

### 3.2 已做且有效的改动
1. **`nuttx/arch/xtensa/src/esp32s3/esp32s3_allocateheap.c`**：
   新增 `#define ESP32S3_HEAP_TOP 0x3fced710`，flat build 的 `up_allocate_heap`
   用 `utop = ESP32S3_HEAP_TOP`（替换 `ets_rom_layout_p->dram0_rtos_reserved_start`）
   → 堆从≈0 变 **204KB**（map 证实：`_sheap=0x3fcb9ac0`，`0x3fced710-0x3fcb9ac0=0x33100`）
   **这是解决阶段 A 复位循环的关键**
2. **`apps/plant-companion/ui/screens/screen_camera.c`**：
   `static uint8_t jpeg_buf[128*1024]`（128KB 常驻 .bss）→ 拍照瞬间 `malloc`、
   `ai_service_analyze_async`（内部已 memcpy 拷贝）后立即 `free`——**拍完即擦**。
   → `_sheap` 从 0x3fcda610 降到 0x3fcb9ac0（释放 128KB）
3. **SPIRAM 配置（defconfig + .config）**：
   ```
   # CONFIG_ESP32S3_SPIRAM_BOOT_INIT is not set
   # CONFIG_ESP32S3_SPIRAM_COMMON_HEAP is not set
   CONFIG_ESP32S3_SPIRAM_IGNORE_NOTFOUND=y
   ```
   NuttX PSRAM 支持是坏的（交接文档早记录：`psram_get_available_size=0`），
   堆已够用，PSRAM 不进启动路径、不进堆。

### 3.3 待排查：阶段 B 的 WiFi PHY 校准崩溃
- 现象：`I (1766) phy_`（= `esp_phy_load_cal_and_init` 里 `ESP_LOGI(TAG,"phy_version %s",...)`）
  之后立即复位
- 链路：`esp_wifi_adapter_init`(esp32s3_wifi_adapter.c:4627) → `esp_wifi_init`(esp32s3_wireless.c:1362)
  → `esp_wifi_init_internal` → ... → `esp_phy_enable` → `esp_phy_load_cal_and_init`
  (esp-hal-3rdparty/components/esp_phy/src/phy_init.c:879) → `register_chipv7_phy`(libphy.a 预编译, RF 校准)
- 关键代码（phy_init.c:879-960）：
  1. `phy_version` 打印（**日志里 `I (1766) phy_` 就是它**）
  2. `calloc(1, sizeof(esp_phy_calibration_data_t))` = 1904B，NULL 则 abort()
  3. `esp_phy_get_init_data()` — NuttX 版返回静态 &phy_init_data（不会 NULL）
  4. **`register_chipv7_phy(init_data, cal_data, PHY_RF_CAL_FULL)`** ← 全量 RF 校准，libphy.a 预编译
- 已加 6 个 diag 标记（phy_init.c，ESP_LOGI(TAG,"diag: ...")）：
  after phy_version / cal_data allocated / before+after register_chipv7_phy(mode=%d) / before+after register_chipv7_phy(FULL)
  **下次烧录后看哪个 diag 出现/消失即定位**
- 已排除：ROM 符号（ets_delay_us=0x40000600 等）在 map 里全部正确解析；
  esp_phy_get_init_data 不会 NULL；calloc 1904B 不会失败（堆 204KB）

## 4. 当前代码/配置状态（2026-08-25 16:xx 修复后）

- `.config`：BOOT_INIT=n, COMMON_HEAP=n, IGNORE=y（与阶段 B 一致）
- defconfig：同步补了 COMMON_HEAP=n + IGNORE=y 注释
- 堆顶改动、jpeg_buf 动态化、phy_init.c diag：都在
- **注意**：esp-hal-3rdparty 是构建时克隆的 pinned repo，本地改动保留
  （见 Make.defs:310 "do NOT delete on distclean"），phy_init.c 的 diag 会保留

## 5. 下一步（建议按顺序）

1. **重新编译烧录当前状态**（配置已修复回阶段 B）：
   ```bash
   cd /home/vboxuser/openvela
   find apps -name '.built' -delete && rm -f nuttx/staging/libapps.a
   ./build.sh vendor/espressif/boards/esp32s3/esp32s3-box/configs/openvela/
   esptool --chip esp32s3 --port /dev/ttyACM0 --baud 921600 \
     --before default-reset --after hard-reset write-flash \
     --flash-mode dio --flash-freq 80m --flash-size 16MB \
     0x10000 nuttx/nuttx.bin
   minicom -D /dev/ttyACM0 -b 115200
   ```
   预期回到阶段 B：LCD+Touch 正常，WiFi PHY 崩（`I (1766) phy_` 后复位）
2. **看 diag 标记**：若 `diag: after register_chipv7_phy(FULL)` 出现 → 校准完成，
   问题在之后（esp_supplicant_init 等）；若卡在 before/after 之间 → 校准内部崩
3. **WiFi PHY 校准崩溃的候选方向**：
   - `register_chipv7_phy` 全量校准需要 `ets_delay_us`（ROM 0x40000600，已解析）+
     RF 寄存器访问，怀疑校准期间某 ROM 函数/IRAM 函数未就绪
   - 可试 `CONFIG_ESP_PHY_RF_CAL_NONE`（跳过校准）看是否过；但校准模式 Kconfig
     当前未被 NuttX 主 Kconfig source（`.config` 无 CONFIG_ESP_PHY_*），需查
     esp_phy 的 Kconfig 如何接入
   - esp-hal 3rdparty 的 esp_phy 在 NuttX 下的已知适配问题（phy_override.c /
     phy_common.c）是排查重点
4. **早期崩溃定位手段**：NuttX 早期输出走 UART0（IO42/40=RS485 土壤口），
   用户 minicom 看不到。若要早期调试，可临时用
   `usb_serial_jtag_ll_write_txfifo()`（hal/esp32s3/include/hal/usb_serial_jtag_ll.h:134）
   直写 USB-Serial-JTAG，或改 `esp32s3_lowsetup` 指向

## 6. 大坑提醒（勿再踩）

1. **不要用 sed 盲目改 .config**：先 diff 确认目标行当前状态再改（阶段 C 教训）
2. **不要同时跑两个 build.sh**：会损坏 LVGL .o 和 libapps.a
3. **libapps.a 的 .built 陷阱**：改了 apps 代码必须
   `find apps -name '.built' -delete && rm -f nuttx/staging/libapps.a`
4. **PSRAM 是坏的**（NuttX 侧），别指望它；堆靠堆顶扩展 + jpeg_buf 动态化已够
5. **早期输出走 UART0(RS485) 不是 ttyACM0**：panic 消息看不到，靠 diag/Saved PC 定位
6. `Saved PC` 含义：0x400006e7=software_reset_cpu（NuttX 主动复位）；
   其他 ROM 地址=CPU 异常进入 ROM panic handler
