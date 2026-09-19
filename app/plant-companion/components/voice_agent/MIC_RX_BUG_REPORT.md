# BUG 报告：ES7210 麦克风 RX 无实时数据（确定性数据 / RX 超时）

> 状态：**未解决（已定位到 RX 时钟链与 ES7210 输出两处疑点，见「假设排序」）**
> 文档目的：完整交接排查过程，下一位工程师/AI 可直接从此文档继续，无需重复历史。
> 最后更新：2026-08-20，固件编译通过（nuttx.bin 已生成，含本报告描述的全部修复）。

---

## 1. 一句话总结

ESP32-S3-BOX 的 I2S0 RX 无法采集到 ES7210 麦克风 ADC 的实时数据：
早期（RX=master）采到**确定性固定图案**（两次录音逐字节相同）；改为官方推荐的
RX=slave + DMA 中断等待后，RX 完全超时（`-ETIMEDOUT`）。**RX 时钟链**（TX 是否持续输出
BCK/WS、引脚回环是否生效）与 **ES7210 是否真正输出**（MCLK/上电）是剩余两大疑点。

---

## 2. 硬件背景（已核实，勿再怀疑引脚）

| 信号 | 引脚 | 方向 | 说明 |
|------|------|------|------|
| MCLK | GPIO2 | ESP32→码片 | I2S0_MCLK_OUT，6.144MHz（24kHz×256） |
| BCK (SCLK) | GPIO17 | ESP32→码片 | I2S0O_BCK_OUT_IDX=22 |
| WS (LRCLK) | GPIO45 | ESP32→码片 | I2S0O_WS_OUT_IDX=24 |
| DOUT | GPIO15 | ESP32→ES8311 | I2S0O_SD_OUT_IDX=25（TX 数据） |
| DIN | GPIO16 | ES7210→ESP32 | I2S0I_SD_IN_IDX=25（RX 数据，SDOUT1 链路确认存在） |
| PA_CTRL | GPIO46 | ESP32→NS4150B | 功放使能 |

- 码片：ES8311（DAC，I2C 0x18）、ES7210（ADC，I2C 0x40）、NS4150B（功放）
- ES7210 配置：TDM 模式（REG12=0x02），4 通道，MIC1-4 上电，MICBIAS12 供电麦克风
- **扬声器通路已通**（tone/回放出声）→ TX 数据路径、MCLK/BCK/WS 输出、ES8311 全部正常
- **唯一故障 = RX（麦克风采集）**

## 3. 症状清单（按时间顺序）

| 阶段 | 配置 | 现象 |
|------|------|------|
| A（初始） | RX=master，轮询 I2S_RX_DONE | 读返回 0 tick；数据确定性（两次录音逐字节相同）；指纹恒定 1370994（≈0x53AD/采样）；非零 34255/48096，峰值 32768，均值 -173 |
| B（修复①RX slave） | RX=slave + DMA 信号量等待 + RXEOF 公式 | `RX error: -110`（3s 超时，got=0）——RX 完全没数据 |
| C（修复②引脚回环） | + BCK/WS 引脚 `INPUT|OUTPUT|FUNCTION` + 矩阵输入 | 仍 -110 |
| D（修复③TX 静音） | + 每次读前 fire-and-forget 8190B TX 静音 + 超时诊断 | 仍 -110，但诊断输出：`INT_RAW=0x0a DMA_IN_ST=0x00 RXEOF_NUM=0x7cf buf: 165c 3fcb 164c 3fcb` |
| E（修复④循环续命） | + 等待期间每 50ms 重新喂 TX 静音 | **本报告交付时刚编译，未实测** |

## 4. 阶段 D 诊断数据解码（关键！）

```
[Voice] RX TIMEOUT: INT_RAW=0x0a DMA_IN_ST=0x00 RXEOF_NUM=0x7cf buf[0..7]: 165c 3fcb 164c 3fcb
```

| 字段 | 值 | 含义 |
|------|-----|------|
| INT_RAW | 0x0a | bit3=**TX_HUNG** + bit1=TX_DONE；**bit0(RX_DONE)=0** → I2S RX 模块 3s 内未收到 4000 字节 |
| DMA_IN_ST | 0x00 | GDMA IN 通道无任何中断状态 → 无 in_suc_eof（与 RX_DONE=0 一致） |
| RXEOF_NUM | 0x7cf=1999 | **RXEOF 写入生效**（公式 2×(n+1) 字节 → EOF 应于 4000 字节触发） |
| buf[0..7] | 165c 3fcb 164c 3fcb | 2 个值交替的周期图案；**可能是真实 DMA 数据（DIN 固定电平），也可能是 malloc 堆残留**（无法仅凭此项区分） |

**推论**：RXEOF 已生效、RX_DONE 未触发、缓冲有（疑似）内容 → RX 只收到极少量数据就停了，
或根本没收到（buf 是残留）。RX 在 3s 内收不满 4000 字节 ⇒ **RX 的 BCK/WS 时钟断续/缺失**。

## 5. 原因分析（三层）

### 5.1 第 1 层：RX 时钟来源（当前最大疑点）
- ESP32-S3 的 I2S **RX slave 的 BCK/WS 来自 GPIO 矩阵输入信号**（I2S0I_BCK_IN_IDX=26 /
  I2S0I_WS_IN_IDX=27），即**从引脚读回 TX master 输出的时钟**（IDF 官方
  `i2s_std.c::i2s_std_set_gpio`：`is_input = role==SLAVE`，接 `s_rx_bck_sig`/`s_rx_ws_sig`）。
  sig_loopback（TX_CONF bit27）只是共享信号，不是 RX slave 的时钟来源。
- 已做：BCK/WS 引脚配置 `INPUT|OUTPUT|FUNCTION`（输出驱动+输入缓冲同开）+ 两条 `gpio_matrix_in`。
- 疑点 A：**TX 在 FIFO 空 / DMA 完成后是否持续输出 BCK/WS**。配了 `tx_stop_en=0`（应为不停），
  但阶段 D 的 TX_HUNG 置位 + RX 收不满暗示**时钟在静音耗尽后停止**（TX 单次静音 8190B≈85ms，
  而 RX 在 chan0-only 下 4000 字节需 ~83ms —— 正好卡在边缘）。
  → 修复④已改为**每 50ms 循环续命**，若仍超时则证明"时钟根本没过引脚回环"。
- 疑点 B：**RX slave 是否真的在用引脚时钟**（vs 需要 rx_clkm 或别的）。若修复④仍超时，
  需 GDB 读 `I2S_STATE`(0x6000F06C) / RX_CONF 确认。

### 5.2 第 2 层：RXEOF_NUM 语义（已修复并验证）
- 寄存器公式（TRM）：EOF 位长 = `(bits_mod+1) × (rx_eof_num+1)`，单位 16-bit 字 = **2 字节/单位**。
- 默认 0x40=64 → EOF 于 **130 字节**触发 → 旧代码"每读只拿 130 字节，缓冲 97% 是残留"→ 指纹恒定。
- 修复：写 `bytes/2 - 1`（4000B → 1999）+ 写后 `I2S_RX_UPDATE`。阶段 D 验证 `RXEOF_NUM=0x7cf` ✓ 生效。

### 5.3 第 3 层：ES7210 数据源（根本问题，若 RX 通路修通后仍恒定数据则锁定此层）
- 早期 RX master 采到的恒定图案（0x53AD ≈ 0x5555 交替）**不是静音**（静音应为 0x0000），
  更像 **DIN 线固定电平被高速采样**的图案 → ES7210 的 SDOUT 可能未真正驱动实时数据。
- 阶段 D 的 165c/3fcb 交替图案同理可疑。
- 待验证：**ADC_MICBIAS12 电压（应≈2.87V，麦克风供电）**；**示波器看 IO16 说话时是否翻转**；
  **MCLK(6.144MHz) 是否确实到达 ES7210**（ES7210 无 MCLK 则 ADC 不工作、SDOUT 不输出）。

## 6. 代码现状（nuttx/arch/xtensa/src/esp32s3/hal_i2s.c，全部改动已编译）

| 位置 | 内容 |
|------|------|
| `configure_gpio()` | BCK/WS 引脚 `INPUT|OUTPUT|FUNCTION` + `gpio_matrix_out(TX)` + `gpio_matrix_in(RX 回环)`；MCLK/DOUT/DIN 原样 |
| `configure_i2s_registers()` | **RX_SLAVE_MOD 置位**（全双工 RX=slave，官方 esp32s3_i2s.c L1622 强制）；sig_loopback=1；tx_stop_en=0 |
| `hal_i2s_init()` | 使能 GDMA IN `DMA_IN_SUC_EOF_CH0_INT_ENA` + `up_enable_irq(g_rx_irq)`；`rx_dma_isr` 清 GDMA 状态后 post `g_rx_sem` |
| `hal_i2s_read()` | ① `arm_tx_silence()`：8190B 静音 fire-and-forget（时钟续命）② RXEOF 按 2×(n+1) 写 + RX_UPDATE ③ 等 `g_rx_sem`（每 50ms 超时重喂静音，总 3s）④ 超时打印 INT_RAW/DMA_IN_ST/RXEOF/buf 前 8 |
| `hal_i2s_write_read_sync()` | RXEOF 同样按公式写 + 信号量等待 |

## 7. 假设排序（按可能性，带验证方法）

| # | 假设 | 证据 | 验证（一次可判别） |
|---|------|------|-------------------|
| 1 | **TX 在 FIFO 空后停止输出 BCK/WS**（tx_stop_en=0 未如预期生效 / TX_HUNG 停时钟） | TX_HUNG 置位；RX 收不满；静音 85ms vs RX 需 83ms 卡边缘 | 示波器 IO17/IO45 在**录音期间**（无播放）看是否有持续 768kHz；或先测修复④（循环续命） |
| 2 | **RX slave 引脚回环未生效**（RX 实际没拿到时钟） | 修复④后若仍超时则坐实 | GDB 断点 `hal_i2s_read` 读 `0x6000F020`(RX_CONF, 看 slave_mod) / `0x6000F06C`(I2S_STATE) |
| 3 | **ES7210 未输出实时数据**（无 MCLK / 上电时序 / MICBIAS 问题） | 所有阶段数据均为固定图案；IO16 链路在但无实时内容 | 万用表 **ADC_MICBIAS12≈2.87V**；示波器 IO16 说话是否翻转；量 ES7210 MCLK 引脚 6.144MHz |
| 4 | RX DMA 中断未送达（已基本排除） | DMA_IN_ST=0 与 RX_DONE=0 一致，数据没到才合理 | 若 buf 非残留且 DMA_IN_ST 有值但信号量没等到 → 查中断 |

## 8. 下一步清单（按顺序做）

1. **烧录当前固件**（含修复④循环续命）跑 `plant voice loop 3`：
   - 若成功（读到数据）→ 去第 5.3 层：看数据是否随说话变化 → 决定 ES7210 硬件检查。
   - 若仍 `-110` → 打印的 buf 是否变化/非 0 → 判断数据有没有进 RX。
2. **示波器（×10 探头）**：录音期间量 IO17(BCK) / IO45(WS)：
   - 有持续 768kHz/24kHz → TX 时钟 OK → 问题在 RX 引脚回环或 RX 配置（GDB 查）。
   - 只在播放/写静音时有时无 → **TX FIFO 空后时钟停** → 改 TX 配置或自环 DMA 链。
3. **万用表**：`ADC_MICBIAS12`（应 ≈2.87V）；**示波器** IO16 说话时是否翻转；ES7210 MCLK 引脚。
4. **GDB/OpenOCD**（如需要寄存器级确认）：
   - `x/wx 0x6000F020`（RX_CONF：bit9=RX_SLAVE_MOD 应=1；bit2=RX_START）
   - `x/wx 0x6000F064`（RXEOF_NUM，应=0x7cf=1999）
   - `x/wx 0x6000F06C`（I2S_STATE：bit0=busy，bit1=tx_idle，bit2=rx_idle）
   - `x/wx 0x6000F00C`（INT_RAW：RX_DONE 是否在 RX_START 后置位）
   - OpenOCD：`adapter speed 1000` + `set ESP32S3_ONLYCPU0 1` + udev 规则，minicom 关闭。

## 9. 参考资料

- 本仓库官方驱动（对照基准）：`nuttx/arch/xtensa/src/esp32s3/esp32s3_i2s.c`
  （RX slave 强制：L1622-1630；DMA in_suc_eof 中断：L2180-2183；ISR：L2361-2387）
- IDF 官方（RX slave 引脚时钟来源）：`esp-idf/components/esp_driver_i2s/i2s_std.c`
  `i2s_std_set_gpio`：`is_input = role==SLAVE`，slave 通道 BCK/WS 接 `s_rx_*_sig`
- 寄存器：`nuttx/arch/xtensa/src/esp32s3/hardware/esp32s3_i2s.h`
  （RXEOF_NUM 公式 L1839-1845；SIG_LOOPBACK L429-439；INT_RAW bit0/1/2/3）
- 信号映射：`nuttx/arch/xtensa/src/esp32s3/hardware/esp32s3_gpio_sigmap.h`
  （I2S0I_BCK_IN_IDX=26 / I2S0I_WS_IN_IDX=27 / I2S0I_SD_IN_IDX=25）
- 工作日志（含更早的 ES8311 静音根因等历史）：`VOICE_DEBUG.md`
- 原理图：`CHD-ESP32-S3-BOX_SCH_V2.0.pdf`；码片手册：ES7210/ES8311/NS4150B/MSM381A

## 10. 关键教训（沉淀）

1. **RXEOF_NUM 不是字节数**，单位是 16-bit 字（TRM 公式）——"看起来对其实错"的典型。
2. **RX slave 的 BCK/WS 来自引脚输入回环**（IDF 官方），不是 sig_loopback 内部供给。
3. **完成信号用 DMA in_suc_eof**（官方驱动），轮询 I2S_RX_DONE 易被 EOF 阈值误导。
4. **确定性数据 = 数据源确定**（DIN 固定），软件层只是放大了确定性——先分"通路"和"内容"两层。
5. **诊断打印要能定位断点**：超时打印 INT_RAW/DMA_ST/RXEOF/buf，一次运行可判别时钟/中断/数据三层。

---

## 11. 2026-08-20 补充：根因定位与修复（RX 回归 master，含实测诊断）

### 11.1 新固件实测诊断（修复④ + 竞态修复后，仍超时）

```
[Voice] RX TIMEOUT: INT_RAW=0x02 DMA_IN_ST=0x00 RXEOF_NUM=0x7cf TX_CONF=0x08089204 RX_CONF=0x...
```

解码：
- `INT_RAW=0x02`：只有 TX_DONE（静音 DMA 完成），**无 TX_HUNG** → TX 健康、续命有效。
- `DMA_IN_ST=0x00`：3s 内 RX DMA **一个字节都没收到**。
- `TX_CONF=0x08089204`：bit27 SIG_LOOPBACK=1、bit2 TX_START=1、bit13 TX_STOP_EN=0 → TX 侧全部正确。
- 结论：**RX slave 模式下 RX 模块完全收不到数据**（时钟/数据都没进来）。

### 11.2 根因（关键推理链）

1. **SIG_LOOPBACK 只共享 WS/BCK 时钟，不直连 TX→RX 数据**（`ai_voice.c` i2sloop 注释已写死；TRM 原文也只说 sharing the same WS and BCK signals）。
   → RX 数据**永远来自 DIN 引脚**（GPIO16 ← ES7210 SDOUT1）。
2. **RX slave 的时钟路径在本自定义 HAL 里始终没打通**：修复①-④（slave + 引脚回环 + SIG_LOOPBACK + 续命）全部 0 字节超时。
3. **RX=master 是实测可用的**：VOICE_DEBUG 阶段5 的 i2sloop"收到真实数据（峰值 18369/32767）"其实**不是回环数据，而是 RX=master 时 ES7210 在 DIN 上输出的真实 ADC 数据**——那就是成功的麦克风采集。团队被官方驱动注释（"force RX as slave"）带偏，改 slave 后 5 轮全超时。
4. 原理图核实（CHD-ESP32-S3-BOX_SCH_V2.0.pdf）：ES7210 SDOUT1(pin11) → R25(10Ω) → ADC_SDOUT_BUF → R96(0Ω) → GPIO16，DIN 通路硬件无问题。
5. 本板 TX master 时钟只驱动码片（GPIO17/45 由 TX 矩阵输出），RX master 内部时钟（rx_clkm，同源 PLL160M 同分频）不输出到引脚 → **无"双 master 引脚冲突"**。

### 11.3 修复（nuttx/arch/xtensa/src/esp32s3/hal_i2s.c）

- `configure_i2s_registers()`：RX_SLAVE_MOD 清 0（**RX 回归 master**），SIG_LOOPBACK 保持 1（阶段5 实测配置）。
- `hal_i2s_read()`：每次读前防御性清 RX_SLAVE_MOD + rx_update；保留 RXEOF 公式（2×(n+1) 字节）+ DMA in_suc_eof 等待 + 静音续命（码片仍需持续 MCLK/BCLK/WS）。
- `hal_i2s_write_read_sync()`：同样清 RX_SLAVE_MOD。
- 超时诊断拆成 3 行打印（上次 RX_CONF 被 80 列串口截断成 `0xb` 无法解码），并加 TX_CONF/RX_CONF。

### 11.4 验证（烧录后）

```bash
plant voice loop 3
```

- 成功且说话时指纹/统计变化 → RX master + ES7210 通路通，收尾（调相位/增益）。
- 仍超时 → 贴新的 `RX TIMEOUT #1/#2/#3` 三行：重点看 `RX_CONF` bit2(RX_START) 是否=1、bit3(RX_SLAVE_MOD) 是否=0。
- 读数成功但数据是"移位的周期垃圾" → RX master 采样相位与码片 BCLK 错位，再调采样沿。

### 11.5 新增教训

6. **官方驱动的"强制 RX slave"不是普适真理**：它成立的前提是 TX/RX 共享同一对时钟引脚且都由 master 驱动。本板 TX 独占总线引脚、RX 只采样 DIN 时，**RX master 完全可行且有实测证据**（阶段5 峰值 18369）。
7. **回环测试的"真实数据"要分清来源**：SIG_LOOPBACK 不转数据，i2sloop 收到的"匹配数据"实为 DIN 上的码片输出——这反而证明了 RX master + ES7210 通路是通的。
8. **诊断打印必须防串口截断**：一行超 80 列会被终端截断（RX_CONF 从 `0x0000000b` 截成 `0xb`），拆行打印。

---

## 12. 2026-08-20 补充：RX master 修复生效 + 帧长不匹配根因（录音 8 倍时长）

### 12.1 RX=master 实测成功

烧录 RX=master 固件后 `plant voice loop 3`：
- 读取耗时 0 ticks → **8~9 ticks**（每次 read 真正等满 4000 字节）
- 指纹实时变化（不再恒定 1370994）
- 非零 56%、峰值 32768（有信号但削顶）→ **RX 数据通路打通**

### 12.2 新根因：I2S 总线帧长与 ES7210 输出不匹配（mictest 原始数据铁证）

`plant voice mictest 2` 原始采样（chan0-only 存储流 24000 int16/s）：
- 槽0(MIC1)：`0000 0000 0000 2c01 0000 0000 0000 0000` → 数据在 **idx%8==3**
- 槽1(MIC2)：`0000 800f 0000 0000 ...` → 数据在 **idx%8==1**

即每个通道的数据只占存储流的 **1/8 → 实际音频仅 3000 采样/秒** → 录音 72000 采样耗时 24.36s（=期望 3s 的 8 倍）。

**根因**：总线帧 = BCLK 768kHz ÷ WS 24kHz = **32 bit/帧**（2 槽×16bit），但 ES7210 配置 REG12=0x02（TDM 4 通道）需要 **64 bit/帧** —— 帧长不匹配，码片数据被挤到 1/8 位置。而参考驱动 esp_codec_dev 的逻辑是 **≥3 麦才开 TDM**，本板 2 麦（MIC1+MIC2）应配 **REG12=0x00（标准 I2S 立体声）**。es7210.c 旧注释"必须 TDM"是 MCLK=12.288MHz 时代的错误结论（当时的"确定性垃圾"实为 RXEOF 残留缓冲假象）。

### 12.3 修复（本轮）

| 文件 | 改动 |
|------|------|
| `es7210.c` | REG12 `0x02`→`0x00`（2 麦标准 I2S，关闭 TDM）；MIC PGA 增益 18dB→9dB（实测 18dB 削顶 32768） |
| `hal_i2s.c` | RX_TDM_CTRL 4 槽→**2 槽**（匹配 32bit 总线帧）；RX master 保留 |
| `ai_voice.c` | 提取逻辑从"1/8 抽相位"改为**全量提取**（chan0-only 存储流即 MIC1 连续 24kHz）；相位扫描保留作诊断（新增"连续数据/帧长仍不匹配"判读） |

### 12.4 验证

```bash
plant voice loop 3
```
预期：录音耗时 ≈ 3s（不再 24s）；指纹实时变化；峰值回落（9dB 增益）。

若仍稀疏（打印"帧长仍不匹配"）→ 总线帧长/码片格式还需调整（BCLK 或 REG12 再验证）。

### 12.5 附：`;1;1;120;120;1;0x` 输入行乱码

非音频线索：音频走 I2S/DMA 不经串口。为录音 24s 等待期间终端输入缓冲（粘贴/按键）的回显残留（像被截断的 ESC 转义序列）。录音时长修好后自然消失。

---

## 13. 2026-08-20 补充：最终根因——总线帧长（手册 + IDF 公式 + 小智源码三方对照）

### 13.1 过程

- REG12=0x00（关 TDM）+ 9dB 增益生效，录音缩到 1.84s，但**仍稀疏**（"帧长仍不匹配"），mictest 数据变成**成对出现**（`0028 002e`、`0009 1009`），存储率 47.5kHz。
- 用户质疑"看过手册对比过吗"→ 完整重读 ES7210 手册 + IDF 公式 + 小智源码。

### 13.2 三方对照结论（全部一致）

| 来源 | 关键事实 |
|------|---------|
| **ES7210 手册 §3/§5** | 从模式 LRCK/SCLK 由外部供给；TDM 4 通道 = **每 LRCK 周期 64 bit**（Figure 2e：Ch1,Ch3 高半 + Ch2,Ch4 低半）；SDOUT 在 SCLK **下降沿**输出；支持 256Fs |
| **IDF i2s_tdm_calculate_clock** | `bclk = fs × total_slot × slot_bits` = 24000×4×16 = **1.536MHz**；mclk = fs×256 = 6.144MHz；`ws_width(AUTO) = total_slot×slot_bits/2 = 32`；msb_shift=1；left_align=false |
| **小智 xiaozhi esp-box-3**（本板实机可用） | ES7210 4 麦全选（TDM REG12=0x02）；MCLK 6.144MHz / BCLK 1.536MHz / 64bit 帧；RX TDM 4 槽 |

**根因**：旧配置 BCLK=768kHz（32 bit/帧）只有码片 TDM 需求（64 bit/帧）的**一半** → 帧长错位 → 数据稀疏 1/8、成对出现（RX 按 2× 码片位率采样）。手册里"REG12=0x00 治本"的方向也错了——根子在总线帧长，不是 REG12。

### 13.3 修复（本轮）

| 文件 | 改动 |
|------|------|
| `hal_i2s.c` | **BCLK_DIV 8→4**（BCLK 768kHz→1.536MHz）；TX/RX `ws_width`、`half_sample_bits` 15→31（64 bit 帧，WS 24kHz）；RX `left_align` 清 0（默认 1，与小智 false 对齐）；RX 4 槽全开 |
| `es7210.c` | REG12 改回 **0x02（TDM）**（与小智 4 麦配置一致）；增益保持 9dB |
| `ai_voice.c` | 槽检测 8 相位→**4 槽**（idx%4），按检测到的槽每 4 采样提取（该槽 24kHz 连续） |

### 13.4 验证

```bash
plant voice loop 3
```
预期：录音耗时 ≈ 3s；槽检测显示"←槽有信号"；指纹实时变化；峰值正常不削顶。
同时验证 `plant voice tone 1000` 仍出声（确认 64bit 帧下 ES8311 DAC 正常）。

### 13.5 新增教训

9. **I2S 总线帧长 = BCLK ÷ WS，必须 ≥ 码片单帧输出位宽**。TDM 4ch×16bit 需要 64 bit/帧 → BCLK ≥ 64×Fs。先算帧长再写寄存器，别只看 BCLK/WS 各自频率。
10. **对照三份权威**：码片手册（帧结构/时钟模式）+ 驱动公式（IDF `i2s_tdm_calculate_clock`）+ 同板型实机源码（xiaozhi）。三者一致才能定案。
11. **数据"成对出现 + 稀疏"是帧长不匹配的典型指纹**：RX 采样率与码片位率错倍 → 每个码片 bit 被读多次/少读。

---

## 14. 2026-08-20 补充：RX master 帧长实测 = 32bit/48kHz（提取步长 4→2）

### 14.1 实测（BCLK 1.536MHz + ws_width=32 + REG12=0x02 后）

```
[Voice] 数据槽检测: idx%4==0 (非零 265/500) ←槽有信号
[Voice] 录音耗时≈6650 ms (got=72000)     ← 期望 3000ms
[Voice] 播放吱吱声（音调偏高）
mictest: 实际RX采样率≈47524 Hz（48k int16/s），数据稀疏于 idx%2 一半
```

### 14.2 推理定案

- **RX master 实际帧 = 32bit / WS 48kHz**（存储流 48k int16/s，chan0-only 每帧 1 int16）——`rx_tdm_ws_width=32` 未使 RX master 帧变成 64bit（master 自产时钟的 WS 周期机制与预期不同）。
- 码片 TDM 64bit 帧（24kHz）被 RX 拆成**两半交替进 chan0** → 流形如 `[Ch1, Ch3, Ch1, Ch3...]` → 麦克风数据在 **idx%2 的一半**。
- 旧提取 `idx%4==0`（每 4 取 1）只拿到一半麦克风数据 → **12kHz 有效却按 24kHz 标称** → 音调翻倍（吱吱声）+ 录音 2 倍时长（6.65s）。

### 14.3 修复

`ai_voice.c`：槽检测 4 槽 → **2 槽**（idx%2），提取步长 4 → **2**（每 2 采样取检测到的槽 → 24kHz 连续）。

### 14.4 验证

```bash
plant voice loop 3
```
预期：录音 ≈ 3s；播放音调正常（无吱吱声）；指纹实时变化。
同时 `plant voice tone 1000` 确认 ES8311 DAC 在 64bit 帧下仍正常。

### 14.5 新教训

12. **RX master 的 WS/帧长与配置可能不符，必须以实测存储率反推帧结构**：存储率 = 每帧存储 int16 数 × WS 频率，先用 mictest 实测再定提取步长，不要假设 ws_width 一定生效。
13. **音调翻倍（吱吱声）= 提取率只有真实数据率一半**：72000 采样 / 实测耗时 可反推有效数据率（72000/6.65s ≈ 10.8kHz ≈ 24kHz/2），与"按 24kHz 标称"矛盾时先修提取步长。

---

## 15. 2026-08-20 补充：全面审计——RX master 数据不可用（位偏移 + 稀疏），回归 RX slave

### 15.1 用户质疑"对标小智未经验证" → 全面扒数据链路

引脚/信号/使能全表 + 全部寄存器配置见对话记录。mictest 原始数据暴露 RX master 两个硬伤：

**铁证 1 — 12bit 位偏移**：槽0 数据 `6000 1000 7000 d000 ...` 全部为 0x1000 倍数（低 12 位恒零，仅 4bit 有效信号）→ 位对齐错误，播放必然是吱吱噪声，与提取率无关。

**铁证 2 — 1/8 稀疏（3000Hz）**：BCLK 1.536MHz + ws_width=32 后依然：数据在 idx%16∈{6,7}，mictest 实测存储率 47524 Hz（RX master 帧=32bit/WS 48kHz，与配置 64bit/24kHz 不符）→ RX master 采样帧长/相位与码片不同步。

**结论**：RX master 是本问题的核心（位偏移 + 帧长错位都是它造成的）；参考实现（IDF/xiaozhi）用 RX slave，位对齐硬件保证。前两次 RX slave 失败于 768kHz/32bit 错误总线；现在总线已修正（1.536MHz/64bit），第三次回归 RX slave。

### 15.2 本轮改动

`hal_i2s.c`：configure_i2s_registers 恢复 `RX_SLAVE_MOD=1`（slave）；hal_i2s_read / write_read_sync 移除 master 强制清除。其余（BCLK 1.536MHz、ws_width 32、4 槽、REG12=0x02 TDM、9dB、RXEOF、DMA、续命、诊断）不变。

### 15.3 验证（重要：两种测试都做）

```bash
# 1. 直接录音回放
plant voice loop 3
# 2. 决定性隔离测试：物理短接 GPIO15(DOUT) ↔ GPIO16(DIN)，然后
plant voice i2sloop    # TX 发 ramp，RX 应收到完全相同数据
```
- i2sloop 匹配率高 → RX 通路正常 → 问题在 ES7210 侧（SDOUT/时钟/配置）
- i2sloop 仍是噪声/稀疏 → RX 通路本身有问题（时钟回环/位对齐）

### 15.4 新教训

14. **"有数据"≠"数据可用"**：非零率/峰值只说明有信号，必须看原始采样位结构（低 12 位恒零 = 位偏移）。
15. **不要盲信"对标 xiaozhi"**：参考配置只在参考的总线/时钟下成立；RX master 与 slave 的位对齐行为完全不同，slave 失败不能证明 slave 方案错，可能是总线配置错。

---

## 16. 2026-08-20 补充：读 MCU TRM（esp32-s3_technical_reference_manual_cn.pdf，I2S 章 983 页起）——从机时钟硬性要求

用户指出应翻 MCU 数据手册而非参考驱动。TRM 28.6/28.10 关键条款：

### 16.1 TRM 权威结论

1. **28.6：从机模式必须 `fI2Sn_RX_CLK >= 8 × fBCK`**（硬性要求！）
   - BCK=1.536MHz → RX 模块时钟必须 **≥ 12.288MHz**
   - 旧配置 rx_clkm=6.144MHz 只有 **4×BCK，违反 ≥8×** → RX slave 收不到数据（此前 0 字节的寄存器级根因）
2. **28.10.1.1：`WS 周期 = 2 × (HALF_SAMPLE_BITS+1)` 个 BCK** → 我们配 31+1=32 → WS=64 BCK=24kHz ✓（帧长配置本身正确）
3. **28.10.2.3：chan_bits/LEFT_ALIGN 决定 16bit 存储窗口从线上数据截取的位置** → 12bit 位偏移的寄存器级解释（RX master 采样错位 + 24-bit ADC 通道位宽不匹配）

### 16.2 修复（hal_i2s.c）

- **RX 模块时钟独立提升到 12.288MHz**（`RX_MCLK_DIV_NUM=13, X=47`，160MHz/(13+1/48)）；TX/MCLK 输出保持 6.144MHz 给码片（两时钟域独立）
- RX slave（上轮已切）+ BCLK 1.536MHz + 64bit 帧 + 4 槽 TDM + REG12=0x02 + 9dB + RXEOF + DMA + 续命 + 诊断，全部保留

### 16.3 验证

```bash
plant voice loop 3        # 应成功：RX slave 首次有数据
plant voice mictest 2     # 看原始采样：位结构应正常（不再 0x1000 倍数）
```
若成功且数据干净 → 收尾（增益/采样率）；若仍 0 字节 → 诊断三行（INT_RAW/RX_CONF/TX_CONF）定位 RX slave 时钟/启动；若数据仍稀疏/位偏 → 码片侧（SDOUT 实际输出格式，需 ES7210 User Guide 寄存器表或 DOUT→DIN 短接隔离）。

### 16.4 新教训

16. **MCU 时钟树要求优先于参考驱动**：TRM 的 `≥8×BCK` 是硬件约束，任何参考驱动都不会替你满足它——先查 TRM 时钟树，再对参考实现。
17. **RX/TX 模块时钟独立**：rx_clkm 与 tx_clkm 分开配，从机 RX 的模块时钟按 8×BCK 算，与 MCLK 输出（TX 域）无关。

---

## 17. 2026-08-20 补充：码片寄存器根因（用户提供 ES7210 寄存器定义，逐位对比）

### 17.1 用户提供 ES7210 User Guide 寄存器表后的逐位对比

| 寄存器 | 规格/参考 | 我们的写入 | 结论 |
|--------|-----------|-----------|------|
| **REG08 模式配置** | 默认 **0x10**（bit[7:4]=LRCK_RATE_MODE=1，N×FS TDM 通道系数/对应麦克风数） | **0x00**（阶段 3 整写，把 LRCK_RATE_MODE 清成 0） | ✗ **主因**：TDM 帧通道结构与 RX 4 槽/64bit 帧对不上 → 数据稀疏错位（1/8 密度 3000Hz）+ 吱吱声 |
| **REG01 时钟** | 官方 4 麦 = 0x20（0x3F 清 0x0b+0x15） | 0x34（只清 0x0b，MIC3/4 ADC 时钟仍关） | ✗ 次要：与"4 麦全开 + TDM 4 通道"不一致 |
| REG02 主时钟 | 0xC1 = DLL旁路+倍频+div=1 | 0xC1 | ✓ |
| REG07 OSR | 0x20 | 0x20 | ✓ |
| REG04/05 LRCK 分频 | 仅主模式生效 | 从模式不写 | ✓ |
| REG11 串行格式 | 0x60 = SP_WL=011(16bit)+I2S | 0x60 | ✓ |
| REG12 TDM | SDOUT_MODE=10 = 1×FS TDM(I2S/LJ) | 0x02 | ✓ |

### 17.2 时钟链验算（用户提供公式，确认 24kHz 合法）

```
MCLK 6.144MHz ÷ CLK_ADC_DIV(1) → CLK1=6.144M → DLL旁路 → CLK2=6.144M → 倍频×2 → 内部MCLK=12.288MHz
单速: Fs = 内部MCLK ÷ (16 × OSR 32) = 12.288M ÷ 512 = 24kHz ✓（单速 8~48k 范围内合法）
```

→ 采样率公式无问题，**REG08 LRCK_RATE_MODE=0 才是 TDM 帧结构错乱的元凶**。

### 17.3 修复（本轮）

- `es7210.c`：**REG08 0x00→0x10**（保留 LRCK_RATE_MODE=1）；**REG01 0x34→0x20**（4 麦时钟全开）
- `ai_voice.c`：提取逻辑改为**自适应密度检测**（周期 1/2/4 网格上找非零密度最高的 (周期,相位)，密度显著高于总体 → 稀疏 TDM 槽；否则连续 → 全量）——REG08 修复后码片帧结构会变，固定步长（4→2 都试错过）不再适用

### 17.4 验证

```bash
plant voice loop 3
```
预期：槽检测打印"周期X 相位idx%X==Y"；若吱吱声消失即定位成功。
仍吱吱声 → mictest 看原始采样判断数据是否落在固定槽（4 槽 64bit 帧下 MIC1 应在槽 0）。

### 17.5 新教训

18. **码片寄存器必须逐位对 User Guide**：esp_codec_dev 从模式只清 bit0(MS_MODE)，我们整写 0x00 把 LRCK_RATE_MODE 一起清了——"看起来在配从模式，实际破坏了 TDM 通道结构"。
19. **REG01 的 MIC 时钟开关要按实际使能的麦克风数清**：4 麦全开必须 0x20，只清 0x0b（MIC1/2）会让 MIC3/4 时钟关着。

---

## 18. 2026-08-20 终定：TX 帧长（tot_chan_num）决定 WS 频率——总线帧长三方不一致

### 18.1 clkdump 寄存器实况（全部按意图写入，唯 TX 槽数错）

| 寄存器 | 实测值 | 解码 |
|--------|--------|------|
| TX_CONF1 | 0x6f7de19f | half_sample=31, ws_width=31, bck_div=3(BCLK=1.536M), bits_mod=15, chan_bits=15 ✅ |
| RX_CONF1 | 0x2f7de19f | 同上 ✅ |
| TX_CLKM | N=26 (MCLK=6.144M) | ✅ |
| RX_CLKM | N=13 (RX 模块时钟=12.288M ≥8×BCK) | ✅ |
| **TX_TDM_CTRL** | TOT_CHAN=**1**（2 槽）| ❌ **根因** |
| RX_TDM_CTRL | TOT_CHAN=3（4 槽）| ✅ |

### 18.2 根因（铁证链）

1. **WS 周期 = TX 帧长 = tot_chan_num × chan_bits**：TX 只有 2 槽 → 帧 32bit → WS = 1.536MHz/32 = **48kHz**（mictest 实测存储率 47524Hz 独立证实）
2. RX（4 槽）与码片（TDM 4ch）都按 **64bit/24kHz 帧**工作 → **总线帧长三方不一致**（TX 32bit vs RX/码片 64bit）
3. 码片内部时钟按 Fs=24kHz 配（REG02/REG07），LRCK 实际 48kHz → 周期错位 → 撕裂吱吱声
4. 示波器 GPIO45 读数 40MHz 是 ×1 探笔晶振串扰（VOICE_DEBUG §1 已记），真实值 48kHz

### 18.3 修复

`hal_i2s.c`：**TX tot_chan_num 1→3（4 槽 = 64bit 帧）→ WS=24kHz**；槽 0-1 使能（L/R 给 ES8311），槽 2-3 关闭（SINGLE_DATA=0，不耗 FIFO，播放数据率不变）。
`ai_voice.c`：提取改回**全量**（WS=24kHz 后 chan0-only 存储流 = 24000/s 连续）。

### 18.4 验证

```bash
plant voice loop 3    # 听人声是否正常
plant voice mictest 2 # 看实际RX采样率应≈24000 Hz（不再是 47524）
plant voice clkdump   # TX_TDM_CTRL 应显示 TOT_CHAN=3
plant voice tone 1000 # 确认 ES8311 DAC 在 64bit 帧下仍正常
```

### 18.5 新教训

20. **WS 频率由 TX 帧长（tot_chan_num×chan_bits）决定，不是 ws_width/half_sample**：改 ws_width 无效（clkdump 证实写入正确但 WS 不变）——先查 tot_chan_num 是否与 RX/码片一致。
21. **总线帧长三方（TX/RX/码片）必须一致**：TX 2 槽(32bit) vs RX/码片 4 槽(64bit) → WS 48kHz → 全链路错位。改任何一方都要核对另外两方。

---

## 19. 2026-08-21：时钟"无规则噪声"根因——分数分频抖动（示波器关键观察）

### 19.1 用户示波器观察（关键转折）

- GPIO2 (MCLK) / GPIO17 (BCLK) **看不到干净方波，噪声极大**；之前 12.288MHz 也是"牵强"波形
- 但数据侧 mictest 波形干净 → 码片确实在工作 → **矛盾只能用"时钟有抖动/不规则"解释**

### 19.2 根因：分数分频产生周期抖动（TRM 明示）

- 6.144MHz = 160MHz ÷ (26 + 1/24) → **分数分频**：每周期在 ÷26 / ÷27 间切换 → **3.8% 周期抖动**
- 12.288MHz = 160 ÷ (13 + 1/48) → 同样分数（1.9% 抖动）
- TRM：**"使用小数分频功能可能会产生时钟抖动"**
- 示波器（NORMAL 触发）看到的就是抖动糊掉的无规则波形；码片内部 DLL 兜不住 → 音频时序被污染 → 撕裂声

### 19.3 修复：全整数分频（零抖动）

| 时钟 | 原（分数，抖动） | 新（整数，干净） |
|------|-----------------|-----------------|
| MCLK | 160/(26+1/24) = 6.144MHz | **160/26 = 6.154MHz**（Fs≈24.04kHz，误差 0.16% 可忽略）|
| RX 模块时钟 | 160/(13+1/48) = 12.288MHz | **160/13 = 12.308MHz**（≥8×BCK ✓）|
| BCLK | MCLK/4 | MCLK/4 = 1.539MHz（整数 ✓）|
| WS | BCLK/64 | BCLK/64 = 24.04kHz（整数 ✓）|

TRM 整数分频要求：X=0、Z=0、**Y=1**。

### 19.4 验证

```bash
plant voice loop 3     # 听人声（撕裂声应消失）
plant voice mictest 2  # 数据应仍干净
```
示波器（×10 探笔）：GPIO2 应看到**干净 6.154MHz 方波**（不再噪声）；GPIO17 ≈ 1.539MHz。

### 19.5 新教训

22. **"示波器看不到干净方波"是第一手证据，优先于一切寄存器推理**：数据侧正常但时钟侧脏 → 时钟抖动（分数分频）是首要嫌疑。
23. **音频时钟尽量整数分频**：160MHz 无法整数分出 6.144/12.288MHz，选 6.154/12.308MHz（0.16% 误差）换取零抖动；码片从模式跟随外部 LRCK，误差可忽略。

## 20. 2026-08-21：RX 0 字节 + RX_HUNG 根因——SIG_LOOPBACK 需要 TX 持续喂数据

### 20.1 症状（整数分频后回归）

- `plant voice loop 2` 录音：RX TIMEOUT #1 INT_RAW=0x0f（RX_DONE+TX_DONE+RX_HUNG+TX_HUNG）
  DMA_IN_ST=0x00，buf 全零；#2 TX_CONF=0x08089204（SIG_LOOPBACK=1, STOP_EN=0, TX_START=1）
  RX_CONF=0x0008160c（RX_SLAVE_MOD=1, RX_START=1）；#3 buf 全 0。
- 示波器：GPIO17 BCLK=1.5MHz、GPIO45 WS=24kHz **引脚时钟全干净**——但 RX 模块一个采样都没收到。
- RX_UPDATE 自清零正常 → RX 模块时钟域活着（12.308MHz 整数分频无问题）。

### 20.2 证据链（根因定位）

1. **RX_HUNG = RX 使能后等不到 BCK/WS 边沿即挂起**（TRM 28.x）。DMA_IN_ST=0 + buf 全零
   = RX 模块根本没采样 → 缺的是 **RX 模块输入侧的 BCK/WS**，不是模块时钟、不是 DMA、不是引脚。
2. IDF 权威注释（i2s_ll.h L1123）：`i2s_ll_share_bck_ws` = "share BCK and WS signal for
   **tx module and rx module**" → SIG_LOOPBACK 是从 TX 模块**内部**抽 BCK/WS 给 RX。
3. 该内部抽头在 TX 串行器之后、pad 输出之前。**TX FIFO 空 → 串行器停 → 内部抽头无信号**；
   STOP_EN=0（TRM 28.8.1）只让 pad 输出"保持最后一帧"（引脚仍有 1.5MHz/24kHz，示波器可见），
   回环抽头已经死了。
4. 对比铁证：**成功那次** = 静音续命持续喂 FIFO → RX 正常收数据；**现在** = 删了续命、
   FIFO 空 → RX 0 字节 + RX_HUNG。两个状态唯一结构性差异就是"TX 是否在持续送数据"。
5. IDF 全双工标准做法（i2s_std.c L142 "Share bck and ws signal in full-duplex mode"）：
   share_bck_ws=true **且 TX DMA 永远在跑**（enable 后持续送零）→ FIFO 永不被耗尽。

### 20.3 修复：连续 TX 静音（环形 DMA）

- 静态 1KB 零缓冲 + 单描述符**自回环**（`desc[0].next = &desc[0]`）→ TX DMA 无 CPU 介入
  持续送零，FIFO 永不被耗尽 → SIG_LOOPBACK 内部 BCK/WS 持续有效。
- `hal_i2s_read`：RX_START 前 `tx_silence_loop_start()`，收完/超时后 `tx_silence_loop_stop()`。
- 优于旧版 50ms STOP+重装续命（每周期 15ms 时钟空洞 + STOP 重装干扰 RX DMA）。
- `hal_i2s_write` 入口先 `tx_silence_loop_stop()`（防 TX_DONE 每 5ms 误触发轮询）。
- 诊断新增 `I2S_STATE_REG(0)`（bit0 TX_IDLE）：静音环生效则 TX_IDLE=0。
- DMA load/enable/disable 的 tx 标志严格隔离 OUT/IN 子通道 → 环形 TX 与 RX 录音并行无冲突
  （已验证 esp32s3_dma.c L355-442）。

### 20.4 验证

```bash
plant voice loop 2     # 应正常录 2s（无 RX TIMEOUT，指纹随时间变化）
```

若仍 0 字节：STATE=0x00000001（TX_IDLE=1）→ 环形 DMA 没喂上（查描述符/对齐）；
STATE=0（TX 工作中）但 RX 仍空 → 回退变量二：RX 时钟改回 12.288MHz 分数分频对照。

### 20.5 新教训

24. **"引脚有时钟" ≠ "RX 模块有时钟"**：SIG_LOOPBACK 抽头在 pad 之前；pad 有输出（STOP_EN=0
    保持最后一帧）≠ 内部串行器在跑。判断 RX 时钟要看 RX_HUNG/DMA_IN_ST，不能只看示波器。
25. **全双工 = TX 时钟主 + RX 从，RX 的 BCK/WS 永远来自 TX 内部**（legacy 与新驱动皆如此）；
    TX 一旦"静默"（FIFO 空），RX 从机立刻饿死。录音期间 TX 必须持续送数据（零/静音）。
26. **环形 DMA 链（链尾指链首）是"持续送时钟"的最优解**：无 CPU 介入、无 STOP/重装抖动、
    与 RX 并行（GDMA 同通道号 OUT/IN 子通道独立）。

## 21. 2026-08-21（下）：RX 0 字节的最终定位——完成路径断（drain 竞态 + EOF 设错描述符）

### 21.1 关键诊断事实（全寄存器回读）

- RX 时钟配置完美（RX_CLKM=0x1400000D：div13+PLL160M+active；RX_DIV=0x200：X0Y1Z0）
- 引脚路由正确（IN26=0x91=GPIO17→I2S0I_BCK_IN，IN27=0xAD=GPIO45→I2S0I_WS_IN）
- pad 输入活着（GPIO_IN 读到电平）；SIG_LOOPBACK 置/清两种配置都试过
- **INT_RAW 的 RX_DONE 置位 + buf 有真实数据** → RX 在采样、数据进了 DMA 缓冲
- 但 DMA_IN_ST=0、DMA_FSM=0 → **GDMA in_suc_eof 从未触发 → g_rx_sem 永不 post → 3s 超时**

### 21.2 两个真 bug（都修了）

1. **EOF 设在链外的死描述符上**：`esp32s3_dma_setup` 对 ≤4095B 只生成单描述符链
   （desc[0]→NULL，desc[1] 从未访问）；旧代码 `rx_desc[1].ctrl |= EOF` 无效。
   → 改 desc[0] 也设 EOF（IDF i2s_alloc_dma_desc 对【所有】RX 描述符设 eof=1）。
2. **drain 竞态**：`while(nxsem_trywait(&g_rx_sem)==0)` 在 RX_START【之后】执行，
   若传输快速完成，ISR 的 post 被 drain 吃掉 → 数据已到却等 3s。
   → drain 移到 RX_START【之前】（残留排空必须在启动新传输之前）。

### 21.3 验证（修复后）

- `plant voice rate 3`：**完成=50 超时=0**，指纹 55 次全不同（数据实时变化）
- 速率 ≈48KB/s = **每帧 1 个采样**（非 4 槽满速 192KB/s）——流结构待定
- `plant voice loop 2`：**录音成功**（got=48000、非零 91%、峰值 32767、均值 25 = 真实麦克风）
- 回放：能启动，但**在最后一小块卡死**（见 §25，仍在查）

## 22. 2026-08-21：ISR 里 printf = 死锁（"卡死在 ES7210 init"的真凶）

- 为诊断在 rx_dma_isr 加了 printf 探针 → **NuttX ISR 上下文 printf 会死锁**
  （ISR 里不能等信号量）。IN 中断在 hal_i2s_init 使能后可能伪触发 → printf → 系统冻死，
  表现"卡在 ES7210 init 之后"（实际是紧接着的 hal_i2s_init）。
- **教训 27：ISR 里绝不 printf；诊断用寄存器回读（超时路径），不用打印探针。**

## 23. 2026-08-21：堆预算实证——UI 是最大占用（语音→AI 必须裁 UI）

- malloc 前 heap: free=62760（62.7KB）→ wav(64KB) 都装不下 → **Record failed: -12 (ENOMEM)**
- **UI（LVGL 7 屏 + 中文字体）摘除后：free=189824（185KB）**——镜像 1.43MB→0.93MB
- defconfig：`# CONFIG_PLANT_UI_PANEL is not set` + `# CONFIG_PLANT_UI is not set`
- 用户决策：语音→AI 功能本来就要裁 UI 空间，黑屏可接受
- 附带：record 的 chunk48 从 malloc 4000B 缩到 1000B；g_tx_silence 缩到 256B

## 24. 2026-08-21：码片层与 HAL 时钟不一致（用户点破）

- HAL 输出 MCLK=6.154MHz（160/26 整数分频），码片层仍写着 6.144MHz：
  VA_MCLK_HZ=24000×512=12.288MHz（错，已改 24000×256≈6.154M）、ES7210 打印 "6.144MHz"（已改 6.154）
- 码片从模式跟随外部 BCK/WS，MCLK 0.16% 误差无影响（寄存器值不动，只改常量/打印）

## 25. 2026-08-21：播放"刺啦一声"根因——TX_DONE 残留置位（播放跳块）

- 症状：回放不到一秒的"刺啦"（不是人声），之后系统无响应
- 根因：TX_STOP_EN=0（FIFO 空持续发最后一帧）→ **TX_DONE 中断位残留置位** →
  播放循环每块的轮询"第一眼就看到 TX_DONE"→ 立即判定完成 → **后续所有块从未发出**
- 录音数据另含偶发全幅尖峰（峰值 32768，均值仅 25）——尖峰即"刺啦"成分
- 修复：hal_i2s_write 在 TX_START 之后立即 `putreg32(I2S_TX_DONE_INT_RAW, I2S_INT_RAW_REG)` 清残留，
  轮询只认本次传输的完成
- **播放末尾卡死仍在查**（调试器抓取中，见 §26）

## 26. 2026-08-21：调试工具链 + 当前状态

### 26.1 调试器套件（零额外硬件，ESP32-S3 内置 USB-JTAG）

- `debug/openocd.sh -b` + `debug/debug.sh`（gdb）：断点/单步/bt/寄存器，Keil 风格
- 与 minicom 争用同一 USB 设备（调试时关串口）
- `CONFIG_PLANT_VOICE_AUTOTEST=y`：开机自跑 rate+录音+回放，配合 GDB 无串口交互

### 26.2 运行时工具（无需重编译）

- `plant voice rate [秒]`：速率 + 首块 32 采样 hex + 周期1/2/4 非零密度（流结构一锤定音）
- `plant voice loopback 0|1`：运行时切 SIG_LOOPBACK
- `plant voice diag [秒]`：clkdump + rate 全量
- 首块槽检测已适配：周期1 密度>30% → 连续单声道全收；否则周期4 扫槽

### 26.3 当前状态（2026-08-21 晚）

| 环节 | 状态 |
|------|------|
| 初始化 | ✅ 全通（ISR printf 死锁已除）|
| RX 采样 | ✅ 真实数据（指纹实时变化，50/50 无超时）|
| 数据率 | ~48KB/s（1 采样/帧，非 4 槽满速——结构待定）|
| 录音→WAV | ✅ 2s 完整（非零 91%，均值 25，偶发削顶）|
| 回放 | ⚠️ 启动正常，**末尾卡死**（TX_DONE 修复后重测中）|
| 播放声音 | 此前"刺啦"（TX_DONE 跳块根因已修），待重测 |

### 26.4 待办

1. **播放末尾卡死**：GDB bt 定位（嫌疑：hal_i2s_write 轮询 / DMA OUT 状态 / 时钟）
2. **流结构确认**：rate 的周期密度输出（1槽 vs 4槽）→ 适配录音提取
3. **削顶**：18dB 增益下峰值 32768 → 降增益或限幅
4. **回放音质验证**：16k→24k 重采样 + 立体声帧正确性

## 27. 2026-08-21（深夜）：录音"系统死、喇叭无声"根因——WAV 缓冲堆越界写

### 27.1 症状与排除

- rate 测试 315 读 0 超时（系统健康）；录音心跳显示 got 推进到 32000/48000（tick 5793）
  后输出停止；喇叭无声 → 系统在录音**收尾**时死亡
- 触摸刷屏已删（不是它）；控制台冻结已被心跳证伪（系统当时还活着）

### 27.2 根因（算术铁证）

- 录音循环终止条件 = **24kHz 输入** got < 48000；降采样 `ai_voice_decimate(N)` 输出
  **floor((2N+1)/3)**（1000 采样 → 667，不是精确 666.67）
- 48 块 × 667 = **n16 = 32016 采样**，但 wav 的 16kHz 容量 = **32000 采样**
  （44 + 16000×2×2 = 64044 字节）
- **最后 32 字节写到 malloc(64044) 块之外 → 堆元数据破坏 → 后续 free/malloc
  崩溃 → 系统死、回放无声**

### 27.3 修复

- 循环终止条件改为 **16kHz 输出** n16 < want_out（32000）
- 每块 mi 按剩余输出容量限制：`mi ≤ (3×remain_out - 1)/2` → 降采样输出 ≤ remain_out，
  数学上保证绝不越界

### 27.4 教训

28. **降采样输出 ≠ 输入×2/3 整除**：每块向上取整会累积溢出；任何"恰好等于缓冲
    大小"的写必须留余量或按输出容量封顶。
29. **"系统在收尾时死"优先查缓冲越界**：malloc 块相邻的堆元数据被写坏，症状是
    "跑完再崩"——不是卡在循环里，而是死在之后的 free/malloc。

## 28. 2026-08-21（深夜）：全面硬件排查——"录不到人声"根因链与硬件交接

### 28.1 本轮结论（一句话）
**数字链路、时钟、DMA、ES7210 芯片全部正常（echo 测试证明能拾音）；卡在"说话声信号弱"与"68.7Hz 周期脉冲"两个硬件相关疑点，已输出硬件交接文档 `AUDIO_HANDOVER_HW_20260821.md`。**

### 28.2 关键新证据
1. **`plant voice echo` 决定性测试**：播放 1kHz 同时录音（`hal_i2s_write_read_sync`），RX 过零=388、能量巨大 → **ES7210 拾音正常**。
2. **1.64× 过采样不成立**：rate 实测 190-198KB/s = 4×24kHz（交接文档的"157k 采样/s"是旧固件误读）。
3. **REG40 LDO/VMID 极限环根因确认**：es7210_init 在无 MCLK 状态配置，MCLK 启动后 REG40 bit0 被清（0x43→0x42）→ 输入共模错误 → 确定性满幅噪声（"录不到真声音只有时钟嘶声"的直接原因）。修复：MCLK 启动后 `es7210_restore_analog()` 重写。
4. **68.7Hz 周期脉冲**（每 14.56ms 一组 4-5 连续大值）：软件 11 点中值已消除听感，根源待硬件查（电源纹波/PCB 串扰/ES7210 内部）。
5. **每次 RX 重启的起始瞬态 + 每块末尾噪声突发**：每块丢弃开头 128 + 末尾 160 采样解决。
6. **4 槽流 DC 阶跃/确定性尖峰**：录音改单槽（`hal_i2s_read_slot`，只收 MIC1）绕开。
7. **USB-Serial-JTAG 控制台输出阻塞**：AUTOTEST 输出积压阻塞主线程数小时（驱动层待修：FIFO 满应丢弃而非阻塞）。

### 28.3 软件修复链（已烧录）
```
单槽48k流 → 丢块头128/块尾160 → 3:1平均(16k) → 高通220Hz → 11点中值
→ 低通5kHz → 限幅±4000 → 噪声门80 → WAV
```
静音基线：峰值 472、残留尖峰 0、均值 0。

### 28.4 待硬件验证（详见交接文档 §4/§5）
- 麦克风 VDD≈3.3V、OUT 静态≈0.7V、MICBIAS12≈2.87V
- 示波器 DIN(IO16) 说话 vs 静音 vs 播放 三态对比
- 68.7Hz 脉冲：查 VDDA 纹波 / DIN 波形 / 断开 SDOUT 链路区分

### 28.5 新教训
30. **"有信号"≠"有真实声音"**：确定性峰值（每次相同）+ 满幅 + 不随说话变化 = 系统噪声/极限环；说话时幅度才变化 = 真实拾音。
31. **芯片验证用"自发声回录"**：播放已知音调同时录音（write_read_sync），过零率/能量判断拾音通断，比看统计更决定性。
32. **码片在无 MCLK 状态初始化会留隐患**：MCLK 启动后必须重配模拟电源（REG40 LDO/VMID）——本板实测 REG40 0x43→0x42。
33. **软件滤波能掩盖但治不了硬件噪声**：68.7Hz 脉冲被 11 点中值消掉，但根源（电源/串扰）必须硬件定位，否则换环境/换板会复发。

---

## 29. NSNet2 神经网络降噪移植（2026-08-24）

### 29.1 目标与决策
- 用户判定：手工 DSP（高通/中值/低通/噪声门）对"语音频段宽带噪声"无效，必须上神经网络 NS。
- RNNoise（纯 C 浮点 GRU）实测在 ESP32-S3 240MHz 无 SIMD 下**卡死**（CPU 过载）→ 弃用。
- 选 esp_sr NSNet2（方案 A，xiaozhi 同款，esp_nn 优化算子，S3 实时）。

### 29.2 关键认知（符号级逆向结论）
1. **esp_sr v2.1.1 的高层 `esp_ns.h`（ns_create/ns_process）不是神经网络**：其实现
   （libesp_audio_processor.a 的 esp_ns.c.obj）引用的是 WebRtcNs_*（传统 WebRTC 谱减法）。
   真正的神经网络 NS 是 **NSNet 接口**：`esp_nsnet_handle_from_name("nsnet2")` →
   `esp_nsn_iface_t`（create/get_samp_chunksize/process/get_samp_rate/destroy），在 libnsnet.a。
2. **模型加载不走文件系统**：nsnet2 的 `model_create` 反汇编确认：
   - `get_model_base_path()==NULL` 时走**内存数据路径**（srmodel_load 的 data 指针）；
   - 模型文件（nsnet2_data/index/_MODEL_INFO_，huffman 压缩）用 pack_model.py 打包成
     srmodels.bin（337950B），以 const 数组嵌入固件，`srmodel_load()` 内存解析；
   - model_create 用 sprintf 拼文件名与 `model_data[i]->files[j]` strcmp 匹配，
     取 `data[j]` 指针 → hufzip 解压 → dl_lib 张量加载到 PSRAM。
3. **依赖闭包**（NSNet2 最小集）：libnsnet.a + libdl_lib.a + libhufzip.a +
   libc_speech_features.a（esp_kiss_fft* 在 c_speech_features，esp_dsp_dot_int16 在 dl_lib）。
   libesp_audio_processor.a / libesp_audio_front_end.a **不需要**（省 4MB 库）。
4. **需 NuttX shim 的 ESP-IDF 接口**：heap_caps_malloc/calloc/free、esp_log_write/timestamp、
   Cache_Start_DCache_Preload/Done、xQueueCreateMutex/vQueueDelete/xQueueSemaphoreTake/
   xQueueGenericSend（FreeRTOS 互斥）、cJSON。全部在 `esp_sr_shim.c` 提供。

### 29.3 移植改动清单
| 文件 | 内容 |
|------|------|
| `esp_sr/lib/libhufzip.a` | 新增拷贝（模型 huffman 解压，原来漏拷） |
| `esp_sr/lib/libvadnet.a` | 新增拷贝（VAD 后续用） |
| `esp_sr/model_data/srmodels_data.c/h` | nsnet2 模型 C 数组（337950B，pack_model.py 生成） |
| `esp_sr/esp_sr_shim.c` | heap_caps/esp_log/Cache/FreeRTOS mutex/dotproduct_int16 兜底 |
| `esp_sr/srmodel_shim.c/h` | srmodel_load 内存解析 + set/get_model_base_path + filter/exists |
| `esp_sr/cjson/cJSON.c/h` | 拷自 ESP-IDF components/json（nsnet 解析 _MODEL_INFO_ 用） |
| `ai_module/ai_voice/ai_ns.c/h` | 重写：esp_nsn_iface 接口，帧大小运行时查询 |
| `ai_module/ai_voice/ai_voice.c` | NS 流水线 480→ai_ns_frame_size() 动态适配 |
| `Makefile` | flat build 下 `all::` 规则用 `ar -M addlib` 把 esp_sr .a 并入 libapps.a |
| defconfig | `CONFIG_ESP32S3_SPIRAM_COMMON_HEAP=y`（PSRAM 进统一 heap，模型大块落 PSRAM） |

### 29.4 RAM 规划（用户强调：UI 还要占 RAM，不许臃肿）
- nsnet2 模型 337KB + 中间张量由 dl_lib 经 heap_caps 分配，SPIRAM_COMMON_HEAP 下大块
  自然落 PSRAM（8MB），内部 RAM 只留帧缓冲（1KB 级）。
- 固件增量 ≈ 模型 338KB + 库代码（esp_nn 优化算子），内部 RAM 增量小。

### 29.5 待验证
- [x] 链接通过、`nm nuttx` 确认 esp_nsnet_handle_from_name / nsnet2 权重在固件内（8/24 下：`esp_nsnet_handle_from_name`@0x42053ba4、`esp_nsnet2_quantized`@0x3c0f81b8、`g_srmodels_nsnet2_bin`@0x3c0f9ef2 均在）
- [ ] 板级：`plant voice echo` 与录音，对比降噪前后（nsnet 开启 vs 关闭）噪声底与语音清晰度（**被录音初始化阶段系统级卡死阻塞，见 §30**）
- [ ] 实时性：单帧 process 耗时（应 < 30ms/帧，esp_nn 优化）
- [x] 帧大小实测打印（ai_ns: chunksize=512, samp_rate=16000，见 VOICE_DEBUG.md §已完成）
- [ ] PSRAM 占用确认（free 看 heap，模型落 SPIRAM region）

---

## 30. 2026-08-24（下）：录音初始化阶段系统级卡死——静态分析结论（接 VOICE_DEBUG.md）

### 30.1 本次完成（无需板子的部分）

| 项 | 结论 |
|----|------|
| esp_sr `.iram1` 段布局审查 | **无冲突**。`legacy_sections.ld` L79 已含 `*(.iram1 .iram1.*)`（在 `.iram0.text` 开头）；实测 esp_sr `.iram1`（libdl_lib.a 的 `esp32s3_dsp.S.obj` + libc_speech_features.a 的 FFT 汇编）落在 **0x40374400~0x403787xx**，为合法可执行 IRAM（iram0_0_seg 上限 0x403cc700，余量充足）；重定位全部段内相对，无 ESP-IDF 绝对地址假设 |
| esp_sr 静态初始化 | 4 个库均无 `.init_array/.ctors/.dtors` → 链接即静态，启动期零行为 |
| I2C 传输超时 | `esp32s3_i2c.c` `i2c_sem_waitdone` = `nxsem_tickwait_uninterruptible(ESP32S3_I2CTIMEOTICKS)`，`CONFIG_ESP32S3_I2CTIMEOMS=500` → 单条消息 500ms 超时返回 -ETIMEDOUT，**不会永久死等**（前提：tick 中断活着） |
| `esp32s3_dma_request` / `esp32s3_setup_irq` | 仅短暂互斥/临界区，无长阻塞 |
| hal_i2s_init 全路径 | sem/gpio/regs/dma-request/tx-irq/rx-irq/dma-eof/done 全部为寄存器操作 + 短暂临界区；hal_i2s_start_tx_clock→hal_i2s_write 轮询带 3s 超时 |

### 30.2 关键新证据：无 NS 版同样卡死（debug/ 日志复盘）

- `debug/rec_ns2.txt`（09:59）与 `debug/rec_ns_t1.txt`（10:04）——**esp_sr libs 10:07 才拷贝，这两版不可能链入 libnsnet.a**，日志也无任何 ai_ns/esp_sr 输出 → 是**无 NS 版**。
- 两者初始化**全部成功**（ES8311→ES7210→hal_i2s→restore_analog），然后**确定性卡在录音循环第 3 块**（最后一行均为 `[Voice] 块@got=1424 峰值=32767 满幅首位置=513/508`）。
- 与 09:33/09:39 同族构建（rec_2047/rec_stable1）**成功完成**对比 → 卡死点随构建漂移，无 NS 版也有。
- → **"库链接静态影响"（VOICE_DEBUG §待办7）基本被反证**；esp_sr 只是改变触发位置/概率。

### 30.3 头号嫌疑：控制台（USB-Serial-JTAG）输出阻塞 = "假卡死"

```
printf → syslog ring(196B) → uart xmit buffer(4096B) → USB-Serial-JTAG FIFO(64B) → 主机
```
- host 端不读（minicom 停/ModemManager 探测复位 USB/端点挂起）→ FIFO 满 → SERIAL_IN_EMPTY 中断停 → xmit 满 → **printf 阻塞 NSH 主线程** → 表现"系统级死锁"。
- 代码自证：`ai_voice.c` 多处 `#if 0` 注释"诊断打印已关闭：USB-Serial-JTAG 输出积压会阻塞主线程（假卡死）"。
- 驱动层"FIFO 满丢字符不阻塞"修复仍未做（§28.2.7 待修项）。

### 30.4 交付物

1. **`debug/freeze_diag.py`** — GDB 卡死取证：halt → PC/bt → **tick 存活测试**（读 g_system_ticks（脚本按符号名解析地址，resume 2s 再读））→ I2S0/GDMA 寄存器 → 当前任务 → 自动分类 A（控制台阻塞）/B（任务死锁）/C（真死机）/D（崩溃）。**这是区分"假卡死 vs 真死机"的决定性工具**，用法见 VOICE_DEBUG.md §四。
2. **二分开关 `make PLANT_NO_NS=1`** — 完全移除 esp_sr/NSNet2 的无 NS 版（⚠️ 切换后需 touch NS 相关源强制重编，见 VOICE_DEBUG.md §四）。
3. **固件备份**：`debug/fw_bak/nuttx_NS_diag.bin`（NS 诊断版 1499824B）、`debug/fw_bak/nuttx_noNS_bisect.bin`（无 NS 二分版 976496B）。

### 30.5 下一步（需板子）

1. 确认 host 无 ModemManager 干扰（`systemctl is-active ModemManager`）。
2. 烧 `nuttx/nuttx.bin`（NS 诊断版）→ `plant voice rec 2` → 记录最后一行（`[I2S] init:` 标记）。
3. 卡死后：关 minicom → `openocd.sh -b` → `freeze_diag.py` → 按 A/B/C/D 分类继续（A→修 usbserial 驱动/查 ModemManager；C/D→按 PC/bt/寄存器定位；卡在 I2S 标记→查对应函数）。

