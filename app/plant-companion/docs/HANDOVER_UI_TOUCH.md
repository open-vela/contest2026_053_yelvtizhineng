# HANDOVER_UI_TOUCH — 新话题交接：UI 触摸交互

> 交接人：上一会话（音频链路修复 + 固件精简）。目标：让下一个话题能快速
> 上手"触摸屏点击驱动 UI 功能"，不踩已踩过的坑。

## 1. 项目一句话
OpenVela（NuttX RTOS）跑在 ESP32-S3-BOX（CHD 板）上，flat build，
`apps/plant-companion` 是"智小伴"植物陪伴应用（LVGL UI + 传感器 + 语音 + AI）。

## 2. 环境 / 构建 / 烧录（照抄，勿改）

```bash
# 编译（无 NS 版；NS 是 Kconfig 开关，默认关）
cd /home/vboxuser/openvela
source ~/openvela-venv/bin/activate
./build.sh vendor/espressif/boards/esp32s3/esp32s3-box/configs/openvela/

# 烧录（只写 app 槽）
esptool --chip esp32s3 --port /dev/ttyACM0 --baud 921600 \
  --before default-reset --after hard-reset write-flash \
  --flash-mode dio --flash-freq 80m --flash-size 16MB \
  0x10000 nuttx/nuttx.bin

# 串口
minicom -D /dev/ttyACM0 -b 115200
```

## 3. 代码结构

| 目录 | 内容 |
|---|---|
| `apps/plant-companion/ui/` | LVGL UI：`ui_app.c/h`（入口/事件循环）、`screens/`（7 屏）、`widgets/`、`theme/`、`assets/` |
| `apps/plant-companion/services/` | 业务服务：`sensor_service`（轮询土壤）、`voice_service`（录音→AI→TTS 状态机）、`ai_service`、`record_service`（任务/日记）、`plant_state` |
| `apps/plant-companion/components/` | 驱动：`voice_agent`（ES8311/ES7210/esp_sr）、`sensor_driver`（RS485 土壤）、`camera_capture`、`gesture_sensor`、`ui_panel`、`system_monitor` |
| `apps/plant-companion/main/app_main.c` | NSH 命令分发（`plant ...`） |
| `nuttx/boards/xtensa/esp32s3/esp32s3-box/src/` | 板级：`esp32s3_board_touchsceen_gt911.c` / `tt21100.c`（触摸）、LCD、SPI |

## 4. UI 与触摸现状（已可用）

- **LVGL** 官方 lv_nuttx 移植：`/dev/lcd0`（显示）+ `/dev/input0`（触摸）
- **触摸输入**：`ui_app.c` L250 用 `info.input_path = "/dev/input0"` 注册 touch indev；
  若 `[UI] WARNING: touch indev NOT created!` → 查 /dev/input0 是否注册
- **7 屏**：home（首页）/ data（数据）/ camera（拍照）/ diagnose（诊断）/
  voice（语音）/ tasks（任务）/ diary（日记）——见 `ui/screens/`
- **服务层**：`voice_service` 状态机（IDLE/LISTENING/THINKING/SPEAKING）回调 UI；
  `sensor_service` 轮询土壤数据供首页/数据页
- 主题/中文字体已配好（`ui/theme/`、`ui/assets/fonts/`）

## 5. 下一步目标（用户新话题）
让**触摸屏点击驱动 UI 功能**：按钮/图标点击 → 切屏 + 调服务（说话→录音、
拍照→诊断、任务→打卡、浇水提醒等）。参考 `docs/UI_SPEC.md`、`docs/ARCHITECTURE.md`。

## 6. ⚠️ 硬教训（勿重踩，全部已验证）

0. **触摸失灵先冷启动，别急着查代码（2026-09-03 实锤）**：烧录/连续硬复位后 GT911
   可能停在异常状态 → probe 不应答 → `/dev/input0` 没注册 → 症状 = "画面在动
   （LVGL 正常）但点不动"。开机日志见 `[LVGL-Touch] open /dev/input0 FAILED
   errno=2` + `[UI] WARNING: touch indev NOT created!`，更早的板级段应有
   `[Touch] ERROR: Failed to read product ID`。**处理：拔 USB 等 3 秒重插**
   （冷启动，非复位键）让 GT911 重新走 RST/INT 上电时序即恢复。驱动注册判读行：
   `[Touch] GT911 detected: PID=...` / `Failed to read product ID` /
   `Failed to initialize I2C port 0` / `registered at /dev/input0`。
   GT911 实际地址 0x5d（板级 TOUCHSCEEN_ADDR，文档写 0x14 是旧笔误），
   触摸在 I2C0（SCL=18/SDA=8），与音频 codec/IMU 同总线。

1. **控制台假卡死**：USB-Serial-JTAG 的 printf 缓冲（4096B）满后 host 不读 →
   printf 永久阻塞（假死机）。**调试打印必须限量**（几行/次）。
2. **PSRAM 破坏 I2S**：esp_sr NSNet2 需要 PSRAM（337KB 模型），但 esp32s3 的
   NuttX PSRAM 初始化坏（`psram_get_available_size`=0），手写 MMU 映射会破坏
   I2S DMA → 录音超时。**NS 默认关（CONFIG_PLANT_VOICE_NS=n），勿开**。
3. **音频驱动**（`nuttx/arch/xtensa/src/esp32s3/hal_i2s.c` = 官方驱动封装）：
   - **TX_START 必须显式置位**（否则 BCLK/WS 不输出 → RX 超时）——已修
   - apb 释放必须在回调内；RX 回调 ctx 必须 static（防 UAF）
   - STOP_EN=0（FIFO 空也保持时钟）
4. **构建系统**：
   - NS 开关是 **Kconfig**（`CONFIG_PLANT_VOICE_NS`），**不是** make 环境变量
     （PLANT_NO_NS=1 传不到 apps，会误编 NS）
   - **libapps.a 删除后重建**：`tools/LibTargets.mk` 已修（调 apps 传 TOPDIR）；
     `.built` 时间戳陷阱：删 `find apps -name '*.built' -delete` 再 build
5. **安全红线**：严禁 GPIO19/20（USB_D-/D+）。
6. **音频验证命令**：`plant voice rec 2` / `plant voice loop 2` / `plant voice tone 440`。

## 7. 固件现状
- `nuttx/nuttx.bin` ≈ 975KB（无 NS、精简调试）
- 音频链路全通（官方 I2S 驱动 + ES7210 2 麦 + 37.5dB + 软噪声门）
- 调试命令已精简（voice 只剩 i2ctest/tone/rec/loop）
- 注意：`apps/libapps.a` 已删除重建成 58MB（正常，链接只取需要部分）
