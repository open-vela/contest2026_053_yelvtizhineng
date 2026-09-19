# VOICE_NOTES.md — 音频子系统注意事项（组件内权威参考）

> 2026-08-25 整理。原散落于 ai_voice/VOICE_DEBUG.md 的调试记录已随调试代码
> 移除；本文件保留**已验证、必须遵守**的结论。硬件参考 PDF 已删除。

## 1. 硬件接线（ESP32-S3-BOX / CHD 板）

| 信号 | GPIO | 说明 |
|------|------|------|
| I2S MCLK | 2 | 必须走 GPIO 矩阵输出（官方驱动 OUTPUT_FUNCTION_2 实测可输出） |
| I2S BCLK | 17 | master 输出 |
| I2S WS | 45 | master 输出（esp-box-3 同款；esp-box 一代才是 47） |
| I2S DOUT | 15 | TX → ES8311 |
| I2S DIN | 16 | RX ← ES7210 |
| PA 使能 | 46 | 高电平 = 功放开（hal_i2s_init 置位） |
| I2C | SDA=8 SCL=18 | ES8311=0x18、ES7210=0x40 |

⚠️ 严禁操作 GPIO19/20（USB_D-/D+）。

## 2. 驱动架构（2026-08-25 迁移 NuttX 官方 esp32s3_i2s 驱动）

`hal_i2s.c` = 官方驱动 `esp32s3_i2sbus_initialize` 的**封装层**（保持 hal_i2s.h API）：
- 全双工 master、2 槽立体声、24kHz、16bit（Kconfig: CONFIG_ESP32S3_I2S0_*）
- 数据通路：apb_buffer + `i2s_send`/`i2s_receive` 异步 → 回调内提取/释放

### ⚠️ 必须遵守（踩过的坑）
1. **TX_START 必须显式置位**（hal_i2s_init 内）——官方驱动只在 TX 数据排队时
   置它（esp32s3_i2s.c L708）；record 只读 RX 时 TX_START=0 → BCLK/WS 不输出
   → ES7210 无时钟 → RX 3s 超时（r=-110）。**RX 稳定依赖 TX 时钟**。
2. **STOP_EN=0**——FIFO 空也持续输出 BCLK/WS（hal_i2s_init 设置）。
3. **apb 释放必须在回调内**（官方驱动完成路径 = callback → apb_free）：
   调用者在 sem 后 apb_free 会与官方驱动竞态 → UAF → 堆损坏。
4. **RX 回调的 rctx 必须 static**（非栈）——官方驱动超时后延迟回调会写已释放栈。
5. 官方驱动 RX 单次 ≤4095B（ESP32S3_DMA_BUFLEN_MAX）→ 读大缓冲需分块。
6. RX 每帧存 2 字（L+R 立体声）→ 取左声道 = MIC1 24k（hal_i2s_read_slot）。

## 3. 码片配置（与 xiaozhi esp-box-3 逐位对照验证）

### ES7210（2 麦标准模式，非 TDM）
- REG12=0x00（非 TDM！官方驱动只支持 2 槽——xiaozhi 麦数<3 同样配 0x00）
- REG01=0x34（只开 MIC1/2 时钟）、REG02=0xc1、REG07=0x20、REG11=0x60(16bit)
- REG43/44=0x1E（37.5dB=值14，xiaozhi 同款）、REG45/46=0x00（MIC3/4 关）
- REG47/48=0x08、REG49/4A=0xFF（MIC3/4 电源关）、REG4B=0x00、REG4C=0xFF
- REG14/15 解除静音（bit[1:0]=00）
- **MCLK 启动后必须 es7210_restore_analog()**（REG40 LDO_EN 会被清 → VMID 异常）
- 输出 = 24bit ADC 高 16 位截断 → 原始幅度小（安静 ±几十）**正常**

### ES8311（DAC）
- REG09 bit6 必须=0（DAC SDP MUTE 解除——历史坑：误置后无声）
- REG32=0xE6（音量 70%）、slave 模式 + use_mclk

## 4. 滤波链（ai_voice.c，PLANT_NO_NS 分支）

```
高通220Hz(α=1/16) → 11点中值 → 低通5kHz(α=0.86) → 限幅±30000
→ 软噪声门：|s|<500 衰减 1/4、500-1000 渐变、>1000 全通
```
- **软门 500 有实测依据**：安静 RMS=284/P90=173/P99=1447，语音峰值 7000+
- **不要恢复硬噪声门 80**（安静环境把声音归零 → "听不到"）
- record 降采样：RX 单声道 24k → 3:2 线性插值 → 16k WAV

## 5. 已知问题（未解决，勿重踩）

1. **PSRAM 映射破坏 I2S**（esp_sr NSNet2 需要 PSRAM 放 337KB 模型）：
   NuttX esp32s3 `psram_get_available_size()` 返回 0（标准映射 0 空间）；
   手写 cache_dbus_mmu_set 映射（即使 cache_suspend/resume 保护）→ I2S DMA 挂
   → RX 超时。**NS 版当前不可用，NS 开关 = CONFIG_PLANT_VOICE_NS（默认 n）**。
   攻关方向：修 NuttX psram 驱动 / 换映射地址 / 模型 flash XIP。
2. **控制台假卡死**：USB-Serial-JTAG xmit 缓冲（4096B）被大量 printf 灌满后
   host 不读 → printf 阻塞。调试打印必须限量。
3. NS 曾崩在 `dl_nn_args_t.c:137`（esp_sr 库内部，形状断言）——已确认与
   apb UAF（已修）相关，NS 恢复条件已变化（见问题1的 PSRAM 阻塞）。

## 6. 保留命令

```
plant voice i2ctest    I2C 码片验证
plant voice tone <hz>  播放测试音
plant voice rec <sec>  录音 → WAV（16k mono，含幅度统计）
plant voice loop <sec> 录 → 回放（听感验证）
```

## 7. 构建

```bash
source ~/openvela-venv/bin/activate
./build.sh vendor/espressif/boards/esp32s3/esp32s3-box/configs/openvela/
# NS 开关：Kconfig CONFIG_PLANT_VOICE_NS（默认 n，勿开——见问题1）
```
