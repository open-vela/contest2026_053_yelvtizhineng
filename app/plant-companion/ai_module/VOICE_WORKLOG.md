# 语音 AI 工作日记（完整排查记录）

> 项目：ESP32-S3-BOX (OpenVela/NuttX) 语音 AI 模块
> 时间跨度：I2S 音频驱动调试 → ES8311/ES7210 码片配置 → 根因分析
> 最后更新：2025-08-18

---

## 一、目标

实现语音 AI 对话（植小伴）：
- 麦克风采集（ES7210 ADC）→ 16kHz WAV → MiMo 全模态 API
- MiMo 响应 → 16kHz WAV → 喇叭播放（ES8311 DAC + NS4150B 功放）

## 二、硬件架构

```
ESP32-S3 I2S0
  ├─ DOUT(GPIO15) → ES8311 DAC → NS4150B → EMX-7T06SP 喇叭
  ├─ DIN(GPIO16)  ← ES7210 ADC ← MEMS 麦克风
  ├─ MCLK(GPIO2)  → 码片主时钟
  ├─ BCLK(GPIO17) → I2S 位时钟
  ├─ WS(GPIO45)   → I2S 帧同步
  └─ PA_CTRL(GPIO46) → NS4150B 使能（HIGH=开）
```

### 信号链路

| 路径 | 组件 | I2C 地址 | 引脚 |
|------|------|---------|------|
| 喇叭输出 | ESP32 I2S → ES8311 DAC → NS4150B → 喇叭 | 0x18 | DOUT=15 |
| 麦克风输入 | 麦克风 → ES7210 ADC → ESP32 I2S | 0x40 | DIN=16 |
| 控制总线 | SDA=IO8, SCL=IO18 | — | — |

## 三、已验证正常（✅）

| 项目 | 测试方法 | 结果 |
|------|---------|------|
| I2C 总线 | `plant voice i2ctest` | ES8311(0x18) OK, ES7210(0x40) OK |
| GPIO 输出 | `plant voice patest` + GPIO_OUT_REG 验证 | GPIO46/GPIO10 输出寄存器正确翻转 |
| PA 使能 | ESP32-S3 技术参考手册 GPIO_OUT1_REG 读取 | GPIO46 bit14 = HIGH（功放使能）|
| I2S TX DMA | `tx_done_int_raw` (I2S_INT_RAW_REG bit1) 触发 | DMA 发送完成 |
| ES8311 寄存器配置 | `plant voice es8311` 诊断读取 | REG09=0xcc：16-bit I2S Standard ✓，DAC+ADC 都使能 |
| ES8311 DAC 音量 | REG32=0xe6（+20dB，70% 最大量）| 音量设置正确 |

## 四、未解决问题（❌）

### 4.1 喇叭没有声音

**现象**：`plant voice tone 1000` 无声音
- TX DMA 完成（`tx_done_int_raw` 触发）
- ES8311 REG09=0xcc（16-bit Standard）配置正确
- PA(GPIO46) = HIGH
- 振幅 30000/32767 = 92%
- MCLK=12.307MHz，BCLK=768kHz

**可能原因**：
1. BCK/WS/MCLK 没有实际输出到 GPIO 引脚（需要示波器验证）
2. ES8311 DAC 没有产生模拟输出（需要示波器量 OUTP/OUTN）
3. NS4150B 功放没有放大（PA_CTRL 可能不是 GPIO46，或极性反了）
4. 喇叭线接触不良

### 4.2 麦克风没有数据

**现象**：`plant voice rec 3` 返回 0 样本
- RX DMA 完成（`rx_done_int_raw` 触发），但数据全零
- 返回 OK（不是超时，不是 ENOSPC）
- 说明 RX DMA 空转（没有有效输入信号）

**可能原因**：
1. ES7210 没有正确采样（需要示波器量 DOUT）
2. BCK/WS 没有实际输出到 GPIO 引脚
3. I2S 数据格式不匹配（ES7210 配置 vs ESP32 输出）

## 五、排查历程

### 5.1 NuttX I2S 驱动问题（已绕过）

**问题**：`nuttx/arch/xtensa/src/esp32s3/esp32s3_i2s.c` 是上游未验证代码
- TX DMA 能跑，但 RX DMA 永远不完成
- `SIG_LOOPBACK` 只共享 BCK/WS 时钟，不直连 TX→RX 数据
- `tx_stop_en=1` 导致 TX 发完后 BCK 停止，RX 无时钟

**解决方案**：写自定义 `hal_i2s.c`（直接操作寄存器）

### 5.2 自定义 I2S HAL 问题（当前）

**已修复**：
1. ✅ MCLK 分数分频：160MHz / (26+1/24) = 6.144MHz（24kHz × 256）
2. ✅ BCLK 分频：MCLK / 8 = 768kHz
3. ✅ Stereo 模式（tx_mono=0，左右声道都启用）
4. ✅ ES8311 寄存器配置（完全按 IDF 流程）
5. ✅ PA 使能（GPIO46 = HIGH）
6. ✅ DMA 中断注册（esp32s3_setup_irq + irq_attach）

**未解决**：
1. ❌ BCK/WS/MCLK 是否真的输出到 GPIO 引脚？
2. ❌ DOUT 是否有 I2S 数据？
3. ❌ ES8311 DAC 是否产生模拟输出？

### 5.3 ES8311 寄存器配置问题（已修复）

**问题**：`es8311_start()` 里直接写 `0xC0` 到 REG09，覆盖了 `es8311_set_format()` 设置的 16-bit（bits[3:2]=0x0c）

**修复**：改为 read-modify-write，只修改使能位，保留格式/位宽设置
```c
// 之前（错误）：
es8311_write_reg(i2c, ES8311_SDPIN_REG09, 0xC0);  // 覆盖了 16-bit！

// 之后（正确）：
es8311_read_reg(i2c, ES8311_SDPIN_REG09, &regv);
regv &= 0xBF;  // 清 bit6
es8311_write_reg(i2c, ES8311_SDPIN_REG09, regv);
// 后面再设 bit6 使能 DAC
```

**验证**：REG09 从 0xc0（24-bit）变成 0xcc（16-bit）✓

## 六、参考对比（xiaozhi-esp32）

xiaozhi-esp32 的 ESP-BOX-3 音频配置（已验证可用）：

| 参数 | xiaozhi 配置 | 我们的配置 |
|------|-------------|-----------|
| 采样率 | 24kHz | 24kHz ✓ |
| MCLK | 6.144MHz (24kHz × 256) | 6.144MHz ✓ |
| 输出模式 | Stereo | Stereo ✓ |
| 输出 slot | I2S_STD_SLOT_BOTH | BOTH ✓ |
| 输出 left_align | true | true ✓ |
| 输出 bit_shift | true | true ✓ |
| 输出 din | UNUSED | UNUSED ✓ |
| 输入模式 | TDM（4 通道） | Standard ❌ |
| 输入 left_align | false | 未设 ❌ |
| ES8311 codec_mode | WORK_MODE_DAC | DAC ✓ |

**关键差异**：输入用 TDM 模式（4 通道），我们用 Standard（单通道）——这可能导致 RX 数据格式不匹配。

## 七、下一步行动

### 最高优先级：验证 I2S 信号

**方法 1：示波器/逻辑分析仪**
- 量 GPIO2(MCLK)、GPIO17(BCK)、GPIO45(WS)、GPIO15(DOUT) 波形
- 跑 `plant voice tone 1000` 看有没有时钟和数据

**方法 2：GPIO 直接翻转测试（无需示波器）**
- 在 `hal_i2s.c` 里加一个测试：用 `esp32s3_gpiowrite(17, true/false)` 直接翻转 BCK 引脚
- 如果 NS4150B 能收到噪声，说明 GPIO17→NS4150B 通路连通

**方法 3：改用 NuttX I2S 驱动（CONFIG_ESP32S3_I2S=y）**
- 这是唯一证明过能在 BOX-3 上工作的方案（xiaozhi 用的是 ESP-IDF I2S 驱动）
- 风险：NuttX I2S 驱动的 RX 有问题（之前绕过的）

## 八、文件清单

| 文件 | 说明 |
|------|------|
| `nuttx/arch/xtensa/src/esp32s3/hal_i2s.c` | 自定义 I2S HAL（直接操作寄存器） |
| `nuttx/arch/xtensa/src/esp32s3/hal_i2s.h` | I2S HAL 头文件 |
| `apps/plant-companion/ai_module/ai_voice/ai_voice.c` | 语音 AI 模块（录音/播放/tone） |
| `apps/plant-companion/ai_module/ai_voice/ai_voice.h` | 语音模块头文件 |
| `apps/plant-companion/components/voice_agent/es8311.c` | ES8311 DAC 驱动（IDF 寄存器配置） |
| `apps/plant-companion/components/voice_agent/es7210.c` | ES7210 ADC 驱动 |
| `apps/plant-companion/components/voice_agent/es8311.h` | ES8311 寄存器定义 |
| `apps/plant-companion/components/voice_agent/es7210.h` | ES7210 寄存器定义 |

## 九、测试命令

```bash
plant voice i2ctest    # I2C 通信测试（码片应答）
plant voice es8311     # ES8311 寄存器诊断
plant voice patest     # GPIO46 PA 功能测试
plant voice tone 1000  # 喇叭测试音（1kHz，500ms）
plant voice rec 3      # 麦克风录音（3秒）
plant voice loop 2     # 录音+回放（2秒）
```
