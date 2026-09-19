# 语音模块排查全记录（扬声器无声 → 根因修复）

> 更新：2025-08-19
> 结论先行：**扬声器无声的根因是 ES8311 REG09 bit6 "DAC SDP MUTE" 被误当作"DAC 使能"置位，DAC 从开机起一直处于静音状态。已修复，tone/loop 有声音。**

---

## 一、问题现象

- `plant voice tone 1000`、`plant voice loop 2` 均**无声**（数据侧"一切正常"）
- 录音"有数据"：`loop 2` 返回 64052 字节（2 秒 16kHz 单声道 WAV）
- I2C 两个码片都通、ES8311 寄存器"看起来正确"、MCLK 12.288MHz 在 GPIO2 上

**最大迷惑点**：所有中间环节都正常，唯独最终没声音——典型的"最后一环"问题。

## 二、硬件架构（ESP32-S3-BOX-3 CHQ V2.0）

```
ESP32-S3 I2S0
  ├─ MCLK(GPIO2)  → ES8311/ES7210 主时钟（12.288MHz = 24kHz × 512）
  ├─ BCK(GPIO17)  → 位时钟（768kHz = MCLK/16）
  ├─ WS(GPIO45)   → 帧同步（24kHz）
  ├─ DOUT(GPIO15) → ES8311 DAC 数据输入
  ├─ DIN(GPIO16)  ← ES7210 ADC 数据输出（麦克风）
  └─ PA(GPIO46)   → NS4150B 功放使能（HIGH=开）
I2C: ES8311@0x18, ES7210@0x40（SDA=IO8, SCL=IO18）
```

## 三、排查历程（方法 = 分层排除 + 官方源码对照）

### 阶段 1：示波器测量陷阱（排除误判）
- 现象：空闲时 GPIO45 读数"MHz 级噪声"、GPIO2 12MHz 波形"不好看"
- 结论：×1 探笔带宽只有 ~6-10MHz，测 12MHz 以上信号会衰减成噪声样；必须 **×10 探笔 + 短地线 + NORMAL 触发 + 合适时基**。波形"不漂亮"是 12MHz 方波谐波被 70MHz 示波器带宽截断的正常现象
- 方法：示波器自带的 1kHz CAL 方波校验，读数必须精确 1kHz

### 阶段 2：I2S 寄存器诊断（clkdump）
- 发现诊断命令自身 4 处**解码位错**（误导排查）：
  - `I2S0_CLK_EN` 查 bit15 → 实际 **bit4**
  - `IO_MUX MCU_SEL` 查 bit11 → 实际 **bit[13:12]**
  - `BCK_DIV_NUM` 查 bit9 → 实际 **bit[12:7]**
  - RX `CLK_EN`(bit29) 实为 **mclk_sel**，=0 才是正确值
- 修正后确认：MCLK 分数分频（160MHz/(13+1/48)=12.288MHz）、BCK=768kHz、WS=24kHz **全部正确**
- 教训：寄存器位定义必须查硬件头文件/TRM，不能凭记忆

### 阶段 3：发现 DMA 静默失败 bug（hal_i2s_write）
- **现象**：数据"发出去了"但无声音
- **根因**：`esp32s3_dma_setup` 单次最多 2 描述符 × 4095B = **8190B**；`hal_i2s_write` 直接传整个 buffer：
  - tone = 48000B > 8190 → `-ENOSPC`
  - WAV 播放块 = 9600B > 8190 → `-ENOSPC`
  - **失败是静默的**（无错误打印）→ 音频数据从未真正发出
  - 录音不受影响：chunk=8000B < 8190
- **修复**：`hal_i2s_write` 内部按 ≤8190B 循环分块发送

### 阶段 4：TX_STOP_EN 写在 UPDATE 之后不生效
- **现象**：MCLK 一直有，BCK/WS 空闲时停（示波器只有 MCLK）
- **根因**：`SIG_LOOPBACK`/`TX_STOP_EN` 写在 `I2S_TX_UPDATE` 之后——I2S 配置寄存器靠 update 位从 APB 域同步进时钟域，写在后面**从未生效**；TX_STOP_EN 默认=1（FIFO 空停 BCK/WS）
- **修复**：把这两个写入移到 UPDATE 之前

### 阶段 5：I2S 数据通路验证（把空壳测试做成真测试）
- `i2ctest` 原是**空壳**（if/else 无打印）→ 重写为真读 ID/应答判定
- `i2sloop` 原只测 TX → 重写为 TX→RX 数据回环 + 非零数/峰值/匹配率统计
- 结果：RX 收到真实数据（峰值 18369/32767）→ **I2S 双向数据通路正常**，ES7210 在输出
- 录音统计（新增）：`loop 2` 峰值 32768（满量程）、非零仅 17% → 数据存在但**通道/格式错位**（见遗留问题）

### 阶段 6：根因——DAC SDP MUTE（关键！）
- 数字侧全部验证正确后仍无声 → 怀疑码片配置
- **方法**：对照三个权威来源逐位核对 ES8311 寄存器：
  1. **Espressif 官方 esp_codec_dev 驱动**（`~/.cache/Espressif/ComponentManager/.../device/es8311/es8311.c`）：DAC 模式下 `REG09 &= ~BITS(6)`
  2. **Linux 内核驱动**（rockchip es8311.c）：
     ```c
     SOC_SINGLE("DAC SDP MUTE", ES8311_SDPIN_REG09, 6, 1, 0)
     SOC_DAPM_ENUM("DAC SDP ROUTE", ...)  // bit7 = 数据源选择
     ```
  3. ES8311 数据手册（everest-semi）：从模式需外部 LRCK/SCLK
- **结论**：**REG09 bit6 = "DAC SDP MUTE"（1=静音！）**，我们代码注释误标为"DAC使能"并置位 → **DAC 一直被静音**
- 连带：旧文档"REG09=0xcc 正确"是双错（bit6 静音 + bit7 数据走内部 ADC 回环）
- **修复**：`es8311_start` 改为**清 bit6**（REG09 0x4c → 0x0c）
- **验证**：`tone 1000` 出声 ✓，`loop 2` 回放出声 ✓，初始化打印 `[ES8311] DAC SDP MUTE cleared (REG09=0x0c)`

## 四、本次修复清单

| 文件 | 改动 |
|------|------|
| `nuttx/arch/xtensa/src/esp32s3/hal_i2s.c` | ① `hal_i2s_write` 分块发送（修 -ENOSPC 静默失败）② `SIG_LOOPBACK`/`TX_STOP_EN` 移到 UPDATE 前 ③ 声明补到 hal_i2s.h |
| `apps/plant-companion/components/voice_agent/es8311.c` | `es8311_start` 清 REG09 bit6（**解除 DAC 静音 = 根因修复**）|
| `apps/plant-companion/ai_module/ai_voice/ai_voice.c` | ① `i2ctest` 空壳→真检测 ② `i2sloop` 只测TX→数据回环分析 ③ `rec`/`loop` 加录音统计（峰值/非零）④ `clkdump` 修 4 处解码位错 + 补 TX_CONF/GPIO_ENABLE 读取 ⑤ diag 显示 MUTE 位 |

## 五、遗留问题：录音通道错位（下一步）

- **现象**：`loop 2` 统计峰值=32768（满量程削顶）、非零仅 5425/32004（17%）
- **原因**：RX 配置为 chan0+chan1 **双槽**（L,R,L,R 交织），录音代码按**单声道**连续处理 → 左右声道混叠、MIC2 静音导致稀疏
- **方向**：录音路径只取槽 0（MIC1）= 抽偶采样；若仍削顶再降 MIC PGA 增益（当前 30dB）
- 参考资料：`components/voice_agent/C365743_...ES7210_规格书.pdf`（4 通道 ADC，2 通道 Standard 模式输出 L=MIC1/R=MIC2）

## 六、验证命令（当前固件）

```bash
plant voice i2ctest     # 两码片 I2C 应答（现在有真输出）
plant voice es8311      # REG09 MUTE=0 ← 修复标志
plant voice tone 1000   # 1kHz 测试音（500ms）
plant voice loop 2      # 录音+回放 + 录音统计
plant voice i2sloop     # TX→RX 数据回环（非零/峰值/匹配率）
plant voice clkdump     # I2S 时钟/控制寄存器全览
```

## 七、排查方法论沉淀

1. **先校准测量工具**：示波器 CAL 口 1kHz 校验，×10 探笔，NORMAL 触发
2. **诊断命令必须真实可读**：空壳测试（无打印）等于没测
3. **寄存器位定义查权威源**：TRM/硬件头文件/官方驱动，不凭记忆；解码位错会误导整个排查
4. **分层排除**：数字（时钟→数据→寄存器）→ 模拟（DAC 输出→功放→喇叭），每层用可判定的测试关闭
5. **最终根因靠官方源码对照**：esp_codec_dev + Linux kernel 驱动逐位对比，1 分钟定位"看起来对其实错"的配置

## 八、录音确定性数据根因分析（两次采样逐字节相同）

**现象**：`plant voice loop 3` 两次录音逐字节相同；每次 `hal_i2s_read` 0 ticks 返回（<10ms）；chunk@750 后首64采样指纹恒定 1370994（≈0x53AD/采样）。

**三层确定性叠加（回答"为什么两次全一样"）**：

1. **数据源确定性（根本）**：DIN(IO16) 上没有实时音频。指纹平均 0x53AD ≈ 0x5555 交替图案 ≠ 全零静音（静音应是 0x0000）→ 不是"ES7210 输出静音"，而是 **DIN 浮空/未被驱动（SDOUT 三态）被 768kHz BCK 采样出的固定图案**。→ ES7210 没在真正输出 ADC 数据（无 MCLK / 时钟配置 / 上电顺序疑点，待万用表验证 MICBIAS12≈2.87V）。
2. **EOF 阈值错误（每次只取 130 字节）**：RXEOF_NUM 官方公式 = (bits_mod+1)×(num+1) bit = 2×(num+1) 字节；默认 0x40=64 → **EOF 在 130 字节触发**（130B/96KB/s≈1.35ms → 0 tick ✓）。hal 写"4000 字节"语义错（应写 1999），且疑似 RX_UPDATE 未同步 → 停在 64。后果：每读只刷新缓冲前 ~130 字节，其余为残留 → 指纹收敛固定。
3. **软件管线纯函数**：相位检测→提取→降采样→WAV 编码对相同输入逐字节相同输出。

**对照官方 `nuttx/arch/xtensa/src/esp32s3/esp32s3_i2s.c`（xiaozhi 同款可用驱动）发现的配置差异**：

| 项 | 我们的 hal_i2s.c | 官方驱动 | 影响 |
|----|----------------|---------|------|
| RX 主从 | **RX_SLAVE_MOD=0（RX master）** | 全双工时**强制 RX slave**（"BCLK/WS 共享只能一个 master，force RX as slave"） | 时钟域冲突/相位不对齐 |
| 完成信号 | 轮询 I2S_INT_RAW bit0 (RX_DONE) | 等 **DMA in_suc_eof 中断** | EOF 计数歧义（hal 的 g_rx_sem 已接好但没用）|
| RXEOF_NUM | 写字节数 4000（语义错）| 写 eof_nbytes（同样近似，但靠 DMA buf_len 兜底）| EOF 阈值错 |

**下一步验证（GDB/OpenOCD，不要改代码）**：
```gdb
x/wx 0x6000F064   # RXEOF_NUM：0xFA0=写入生效 / 0x40=卡在默认64
x/wx 0x6000F00C   # I2S_INT_RAW：RX_DONE(bit0) 是否 RX_START 后立即置位
x/wx 0x6000F020   # RX_CONF：RX_START(bit2) 是否真的置位、RX_SLAVE_MOD(bit9) 值
x/wx 0x6000F028   # RX_CONF1：rx_bck_div_num(bit13:8) 是否=7
```
**硬件验证**：万用表 ADC_MICBIAS12（应≈2.87V）；示波器 IO16 说话时是否变化。

**⚠️ 第四次迭代发现（RX slave 无时钟 → -110 超时）**：改 RX slave 后 DMA 3 秒超时
（`[Voice] RX error: -110`）——RX slave 完全没收到数据。查 IDF 官方 `i2s_std.c`
`i2s_std_set_gpio`：**slave 通道的 BCK/WS 引脚 is_input=true，接 s_rx_bck_sig/s_rx_ws_sig**，
即 RX slave 的 BCK/WS 从**引脚回环**读入（TX master 输出 → 同一引脚 → GPIO 矩阵
`I2S0I_BCK_IN_IDX(26)`/`I2S0I_WS_IN_IDX(27)`），不是靠 sig_loopback 内部供给！
我们的 configure_gpio 只接了 TX 输出信号 → RX 无时钟 → DMA 永不完成。
**修复**：BCK/WS 引脚改 `INPUT|OUTPUT|FUNCTION`（输出驱动+输入缓冲同开），
补 `gpio_matrix_in(BCLK, I2S0I_BCK_IN_IDX)` + `gpio_matrix_in(WS, I2S0I_WS_IN_IDX)`。
（IDF 依据：https://github.com/espressif/esp-idf/blob/master/components/esp_driver_i2s/i2s_std.c
 `i2s_std_set_gpio`，以及 i2s_ll_share_bck_ws 仅在全双工时置位）

**⚠️ 第五次迭代（RX slave 仍超时，加 TX 时钟保持 + 超时诊断）**：改 RX slave + 引脚回环后
仍是 `RX error: -110`（3s 超时，DMA 一个字节都没收到 → RX 无时钟）。两个动作：
① `hal_i2s_read` 每次读取前 **fire-and-forget 喂 8190B TX 静音**（≈85ms@768kHz，覆盖 41.7ms
   的 RX 读）→ 保证 TX 输出的 BCK/WS 在录音期间持续（RX slave 时钟源来自 TX 引脚回环）；
② 超时时打印 `I2S_INT_RAW` / `DMA_IN_INT_ST` / `RXEOF_NUM` / 缓冲前 8 字节 →
   一次运行即可定位断点：
   - INT_RAW 有 RX_DONE(bit0) 但 DMA_IN_ST 无 in_suc_eof → 数据在流，DMA 中断没送到 → 查中断
   - INT_RAW 无 RX_DONE 且 DMA_IN_ST 无 → 时钟没到 RX → 查 TX 时钟/引脚回环
   - buf 全 0 但状态有 → DMA 写入了 0（ES7210 输出全 0？）
   - buf 非 0 恒定 → ES7210 输出固定图案（硬件：MCLK/MICBIAS12）

**已实施修复（对照官方驱动三处一起改）**：
① `RX_SLAVE_MOD` 置位（全双工 RX=slave，官方 esp32s3_i2s.c L1622-1630 强制要求）；
② 使能 GDMA IN `DMA_IN_SUC_EOF_CH0_INT_ENA` + `up_enable_irq(g_rx_irq)`，`hal_i2s_read`/`write_read_sync`
   改用 `g_rx_sem` 信号量（nxsem_tickwait 3s 超时）等 DMA 完成，替代轮询 I2S_RX_DONE；
③ `RXEOF_NUM` 按 TRM 公式 2×(num+1) 字节写（4000 字节 → 写 1999），写后触发 rx_update。
预期：读耗时从 0 tick 变为 ~4 tick（4000B/96KB/s≈41.7ms）；若数据仍恒定 → 锁死 ES7210 硬件
（MCLK 是否到 ES7210 / MICBIAS12≈2.87V / IO16 示波器说话是否变化）。
