# 音频链路调试交接文档（给下一个 AI）

> 生成时间：2026-08-21 深夜 · 硬件：ESP32-S3-BOX（CHD，与 xiaozhi esp-box-3 同款）
> 系统：OpenVela/NuttX · 本文件自包含，下一个 AI 从零读起即可接续。
> 配套：`MIC_RX_BUG_REPORT.md`（§1-27 完整历史，含每个 bug 的证据链与教训）

---

## 0. 一句话现状（先看这个）

- ✅ **全链路已跑通**：初始化 → RX 采到真实数据（315 读 0 超时）→ 录音 2s 完整（32000 采样，非零 92%）→ 回放 40 块完整播完，**系统不再死**。
- ❌ **声音不对**：440Hz 音调听成"吱的一声 + 悠远嘶嘶"（音调偏高/变调）；录音回放是嘶声不是人声。
- ❓ **核心未解之谜**：RX 实测 **~157k 采样/s**，而配置计算应为 **96k**（4槽 × 24kHz WS）——**1.64× 过采样**。音调偏高同样 ≈1.64×。**两个独立证据指向：实际帧率 ≈39.25kHz，不是 24kHz。但寄存器回读与示波器（旧固件）都说是 24kHz。**

---

## 1. 硬件与引脚（勿改）

| 信号 | GPIO | 说明 |
|------|------|------|
| I2S0 MCLK | GPIO2 | 160/26 = **6.154MHz**（整数分频，零抖动）|
| I2S0 BCLK | GPIO17 | MCLK/4 = **1.539MHz**（TX 输出 + matrix_in 回环给 RX）|
| I2S0 WS | GPIO45 | BCLK/64 = **24.04kHz**（同上回环）|
| I2S0 DOUT | GPIO15 | TX 数据 → ES8311 DAC |
| I2S0 DIN | GPIO16 | RX 数据 ← ES7210 ADC |
| I2C1 SDA/SCL | IO8/IO18 | **共享总线**：ES8311(0x18)、ES7210(0x23)、GT911 触摸、IMU、摄像头 |
| PA 使能 | GPIO46 | 高电平开功放 |

⚠️ 关键认知：**I2C 是共享总线**，NuttX 每事务有总线锁，串行化不破坏数据——多外设共存没问题。GT911 触摸驱动的刷屏 printf 已删（那是之前"乱码/假卡死"的真凶之一）。

## 2. I2S0 配置（`nuttx/arch/xtensa/src/esp32s3/hal_i2s.c`，自定义 HAL）

| 项 | 值 | 依据 |
|----|-----|------|
| TX | **master**，TDM 4 槽（tot_chan=3），ws_width=32，half_sample=32，16bit，msb_shift=1 | 码片从模式需要 64bit 帧 → WS=24kHz |
| RX | **slave**（RX_SLAVE_MOD=1），TDM 4 槽，chan0-3 全开，ws_width=32，msb_shift=1 | 与 IDF/xiaozhi 对齐 |
| SIG_LOOPBACK | **0**（引脚回环送时钟；置 1 实测无效甚至干扰） | 对照实验定案 |
| TX_STOP_EN | 0（FIFO 空持续发最后一帧，时钟不停）| TRM 28.8.1 |
| RX 模块时钟 | 160/13 = 12.308MHz（≥8×BCK）| TRM 28.6 |
| 时钟源 | PLL160M，X=0 Y=1 Z=0（整数分频）| TRM：整数分频防抖动 |

**RX 完成信号**：GDMA IN 的 in_suc_eof 中断 → g_rx_sem。**不要用 I2S RX_DONE**。
**TX 完成信号**：轮询 I2S TX_DONE（注意 STOP_EN=0 时 TX_DONE 会残留置位——必须 TX_START 后先清再轮询，见 §4 bug 5）。

### 2.1 clkdump 实测寄存器（当前固件，全部正确）

```
TX_CONF(0x24)       = 0x00089204  TX_START=1 STOP_EN=0 SIG_LOOPBACK=0
TX_CLKM_CONF(0x34)  = 0x3400001a  clk_sel=2 active=1 div=26
TX_CLKM_DIV(0x3C)   = 0x00000200  X0Y1Z0（整数）
RX_CLKM_CONF(0x30)  = 0x1400000d  clk_sel=2 active=1 mclk_sel=0 div=13
RX_CLKM_DIV(0x38)   = 0x00000200  X0Y1Z0
TX_CONF1(0x2C)      = 0x6f7de19f  ws_width=31 bck_div=3 bits=15 half=31 chan=15 msb=1
RX_CONF1(0x28)      = 0x2f7de19f  （同 TX，bck_no_dly=0）
TX_TDM_CTRL(0x54)   = 0x00030003  tot_chan=3 chan0-1 使能
RX_TDM_CTRL(0x50)   = 0x0003000f  tot_chan=3 chan0-3 全开
GPIO: MCLK→GPIO2 SIG23, BCLK→GPIO17 SIG22, WS→GPIO45 SIG24, IN_SEL26=0x91(GPIO17) IN_SEL27=0xAD(GPIO45)
```
→ 按此配置：BCK=1.539MHz、**WS=24.04kHz**、4槽流=96k 采样/s=192KB/s。

## 3. 码片配置

### ES7210（ADC，`components/voice_agent/es7210.c`，与 IDF 逐位一致）
| REG | 值 | 含义 |
|-----|-----|------|
| 00 | 0x41 | 复位后 |
| 01 | 0x20 | 麦时钟（4 麦）|
| 02 | 0xc1 | DLL bypass + doubler + div1（slave 必须 doubler=1）|
| 07 | 0x20 | OSR=32 |
| 08 | **0x10** | slave + **LRCK_RATE_MODE=1**（决定 TDM 帧通道结构！）|
| 11 | 0x60 | 16-bit I2S |
| 12 | **0x02** | SDOUT TDM 1×FS（4 麦）|
| 14/15 | 清 bit0-1 | 解除静音（上电默认静音！）|
| 40/4B/4C/41/43 | 0x43/0x00/0x00/0x70/增益 | 模拟电源 + MICBIAS |
| 47-4A | 0x08 | 4 麦 PGA 电源 |

### ES8311（DAC，`components/voice_agent/es8311.c`）
- REG09/0A：0x0c（16bit）+ [1:0]=00（I2S 标准）——**与 IDF `es8311_set_bits_per_sample`/`es8311_config_fmt` 逐位一致**（已核对 xiaozhi managed_components 源码）。
- 时钟系数 pre_div=1（对应 MCLK≈6.144 档，6.154 误差 0.16% 无影响）。

## 4. 已找到并修复的 bug（全部有证据，勿回退）

| # | Bug | 证据 | 修复 |
|---|-----|------|------|
| 1 | RXEOF_NUM 单位是 16bit 字、且写后必须 rx_update | 两次录音字节相同（指纹恒定）| `num=bytes/2-1` + rx_update |
| 2 | RX EOF 设在链外死描述符 | 数据到了但 in_suc_eof 永不触发 | desc[0] 也设 EOF（IDF 全设）|
| 3 | drain 竞态：排空信号量在 RX_START 之后 | 快速传输的完成信号被 drain 吃掉 → 3s 超时但有数据 | drain 移到 RX_START 前 |
| 4 | **ISR 里 printf = 死锁** | "卡死在 ES7210 init 后"（实际是 ISR printf 冻死系统）| 删除；诊断改寄存器回读 |
| 5 | **TX_DONE 残留置位 → 播放跳块** | "刺啦一声不到一秒"——STOP_EN=0 时 TX_DONE 残留，轮询第一眼就"完成"，后续块从未发出 | TX_START 后立即清 TX_DONE 再轮询 |
| 6 | **WAV 堆越界写 → 录音收尾系统死** | 降采样 1000→667 向上取整，48块=32016 > 容量 32000 → 32 字节写穿 malloc 块 → free 崩溃 | 循环按 16kHz 输出容量终止 + mi 按剩余容量封顶 |
| 7 | UI 占堆 62KB→185KB | malloc 前 heap: free=62760 < wav 64044 → -12 | 摘除 UI 编译（语音→AI 本就要裁 UI）|
| 8 | GT911 触摸刷屏 printf | 串口乱码交错，"假卡死" | 删 printf（触摸保留）|
| 9 | 码片层时钟常量 6.144 vs HAL 6.154 | 注释/常量不一致 | 统一 6.154 |
| 10 | **录音后播放失败 `hal_i2s_write=-5`**（plant voice loop 3）| 2026-09-01 RX 预读自动续链引入：录音结束在途 RX apb 仍占 RX DMA，TX 与其并发启动失败；旧修复只置 paused 标志、不等 DMA 停，且 hal_i2s_write 把错误折叠成 -EIO 掩盖根因 | 2026-09-02 三策略融合：`hal_i2s_rx_prefetch_stop()` 置标志后**同步等 RX DMA 空闲**（≤300ms）再发 TX（小智通道生命周期先停后放 + OpenVela 官方驱动单次 DMA 语义 + 植小伴预读链保留录音连续）；`hal_i2s_write` 返回真实 errno，失败带 TX_CONF/TX_CONF1/STATE 寄存器现场 |
| 11 | **播放"完成"却无声**（hal_i2s_write=-5 修复后新现象）| 播放 DMA 全部完成、无错误，但喇叭无声；录音正常（MCLK/BCK/WS/ES7210 均好，共享总线证明时钟在跑）| 2026-09-02 缓存一致性修复：`CONFIG_ESP32S3_SPIRAM_COMMON_HEAP=y` 时官方驱动的 TX 内部缓冲 `calloc` 可能落 PSRAM——PSRAM 经 Cache/MMU 访问，CPU memcpy 写入停在 cache，GDMA 直读 PSRAM 拿到 calloc 旧零值 → "完成但无声"。修复：`esp32s3_i2s.c` TX 启动前 `rom_Cache_WriteBack_Addr`（PSRAM 才刷，DRAM 跳过，行为不变）+ RX 回调前 `Cache_Invalidate_Addr`（对称修复）。对比：WiFi 适配器的 `esp_wifi_internal_malloc` 直接拒绝 PSRAM——本树既定模式就是"DMA 缓冲必须内部 RAM"；小智/IDF 用 `esp_cache_msync`/`MALLOC_CAP_DMA` 同理 |
| 12 | 无声排查诊断（新增）| 播放首块打印数据统计（`[Voice] 播放数据:`）+ `plant voice regs` dump I2S TX 状态与 ES8311 全部关键寄存器，与 小智 esp_codec_dev / OpenVela NuttX 参考值逐项对比 | 见 §11 的验证路径：数据非零 + TX_START=1 → 查 ES8311 寄存器/功放；数据全零 → 查录音/WAV |
| 13 | **TTS 清晰但录音回放全是噪声**（ESP32-S3-WROOM 板）| RX 格式失配：ES7210 REG08=0x10（4 通道 TDM 帧/64bit，旧 4 槽 RX 时代残留）配当前 2 槽/32bit RX（BCLK 只有 64×fs 的一半）→ 槽位错位/混叠 → 24k→16k 降采样拿到错位样本 → 人声变噪声；TTS 清晰证明 TX/DAC/PA 全好 | REG08 改 0x00（2 通道标准帧，与 REG12=0x00 立体声 + 2 槽 RX 一致；小智 esp_codec_dev 从不写 bits[7:4] 即默认 2ch）。验证：`plant voice loop 3` 听人声 + `plant voice diag` 帧率检查（实际耗时≈标称则 24k 正常；≈标称/2 则 RX 仍 48k 需查帧率）+ 过零率（人声<0.15，噪声>0.3）|
| 14 | **（已回退）"ES7210 需 64×FS BCLK（4 槽帧）"尝试** | 改 total_slot=4 + RX_TDM 4 槽 + REG08=0x10 → **RX 0 数据**（TX 仍 2 槽时 WS 变 48kHz，ES7210 锁不上 MCLK/LRCK 比）| ❌ **回退**：2026-09-02 恢复 08-25 验证基线（total_slot=2 + 2 槽 RX + REG08=0x10 + 30dB）。**勿再走 4 槽/TDM 路**——VOICE_WORKLOG 2026-08-25 已实机验证"2 槽 + ES7210 2 麦标准模式"人声可闻（说话峰值 7065-14053）|
| 15 | **回归定案：回到 2026-08-25 验证基线** | 本轮 4 槽改动造成 RX 0 数据回归；REG08=0x00 造成 1/8 稀疏 | 官方驱动 total_slot=2、i2s_rxchannels 保持原生（只存值）、hal_i2s src[2i+slot] 提取、ES7210 REG08=0x10 + REG12=0x00 + 增益 30dB(0x1A)。保留本轮无害增强：RX 预读链 TX 前同步停（修 -5）、真实 errno、tone 分块 static、SD 挂载重试、plant mem 报表、raw 槽位 A/B。剩余工作=底噪优化（08-25 留档：软门/NSNet2）|

## 5. 当前可复现现象（下一个 AI 的起点）

1. **RX 速率 = 314KB/s = 157k 采样/s**（rate 测试，多次复现，0 超时）——**应为 96k**。
2. **440Hz 音调 → "吱的一声 + 悠远嘶嘶"**（音调偏高/变调）。
3. **录音回放 = 嘶声**（非人声）。
4. 录音统计：非零 \~92%，**峰值 32767（削顶）**，均值 \~18-27。
5. 首块原始采样（阈值修复前）显示：相位0 平滑（真音频）、相位1/3 = ±1 噪声（死槽）。
6. 槽检测（|x|>8 阈值）：上次输出"周期1 连续流 (密度 90/95/96/78%)"——**没有死槽**（所有相位都有 >8 的信号）。

## 6. 核心未解之谜（重点！）

**1.64× 出现在两个独立测量：**
- RX 速率 157k/96k = 1.64
- 音调偏高 ≈1.64 → 实际帧率 ≈39.25kHz 而非 24kHz

**但**：寄存器回读算得 WS=24.04kHz；早先示波器测过 24kHz（旧固件，现已还掉）。

**候选解释**（按嫌疑排序）：
1. **ES7210 实际输出帧结构 ≠ 4槽×24kHz**（REG08 LRCK_RATE_MODE / REG12 SDOUT_MODE 的实际生效值可能与预期不同）
2. **RX 过采样**：RX 帧同步与码片输出不同步，每帧多采 ~1.64 个词
3. TX 实际 WS ≠ 24kHz（配置回读对但硬件生效值不同——不太可能，寄存器回读=硬件值）

## 7. 下一个 AI 的实验清单（按此顺序）

**实验 A（零成本，先看上次固件输出）**：跑最新固件（含自相关），看
`帧周期检测(自相关): 最佳lag=N` —— **N=6-7 直接坐实过采样**；N=4 则帧结构对但速率对不上（矛盾指向 ES7210）。

**实验 B（改 ES7210 REG08）**：LRCK_RATE_MODE 0x10 → 0x20 / 0x30，重测 rate。
- 速率跟着变 → 码片帧结构问题，锁定 REG08 语义
- 不变 → 与 REG08 无关

**实验 C（改 TX half_sample/ws_width）**：32 → 16（WS=48k）/ 24（WS=32k），重测 rate。
- 速率按比例变 → WS 由该寄存器控制 → 反推当前真实 WS
- 不变 → WS 被别处锁死

**实验 D（STOP_EN=1 播放对照）**：hal_i2s_write 期间临时 STOP_EN=1（FIFO 空即停，TX_DONE 语义变可靠），播放已知音调对比——排除 TX_DONE 残留造成的播放异常。

**实验 E（ES7210 实际输出验证）**：读回 REG08/12/01 确认写入生效；查 ES7210 手册 REG08 bit[7:4] LRCK_RATE_MODE 对 TDM 帧的确切定义（4 麦应配什么值）。

**实验 F（DAC 侧隔离）**：若上述都指向数据正常，用已知 L/R 测试序列（左声道 +0.5、右声道 -0.5 交替）播放，听是否有声道内容——验证 ES8311 在 64bit 帧下的槽位提取。

## 8. 工具与命令（固件里已内置）

```
plant voice rate [秒]     # RX 速率 + 128采样hex + 自相关帧周期 + 周期1/2/4密度
plant voice diag [秒]     # clkdump（全寄存器）+ rate
plant voice loopback 0|1  # 运行时切 SIG_LOOPBACK（无需重编译）
plant voice tone <freq>   # 播放正弦音调（隔离 TX/DAC 链路）
plant voice loop [秒]     # 录音+回放
plant voice mictest [秒]  # 麦克风自检
plant voice clkdump       # 时钟寄存器全 dump
```
- **AUTOTEST**（defconfig `CONFIG_PLANT_VOICE_AUTOTEST=y`）：开机自动跑 ①440Hz 音调 ②rate ③录音+回放，跑完回 nsh。
- **调试器**：`/home/vboxuser/openvela/debug/`（OpenOCD + GDB **12.1**——GDB 17 的 vMustReplyEmpty 与 OpenOCD 不兼容！）。先 `./openocd.sh -b`（会检查 ModemManager/串口占用）再 `./debug.sh`。
- **UI 已摘除**（`# CONFIG_PLANT_UI_PANEL is not set`）——黑屏正常，语音→AI 本来就要裁 UI。

## 9. 关键文件

| 文件 | 作用 |
|------|------|
| `nuttx/arch/xtensa/src/esp32s3/hal_i2s.c` | **I2S0 自定义 HAL**（时钟/槽/中断/DMA，改动最多）|
| `apps/plant-companion/components/voice_agent/es7210.c` | ADC 配置（REG08/12 是帧结构关键）|
| `apps/plant-companion/components/voice_agent/es8311.c` | DAC 配置 |
| `apps/plant-companion/components/voice_agent/voice_agent.c` | 初始化编排 |
| `apps/plant-companion/ai_module/ai_voice/ai_voice.c` | 录音（槽检测/降采样/越界修复）、播放、rate 测试 |
| `apps/plant-companion/components/voice_agent/MIC_RX_BUG_REPORT.md` | §1-27 完整排查历史 |
| `nuttx/arch/xtensa/src/esp32s3/hardware/esp32s3_i2s.h` | 寄存器位定义（已核与 IDF 一致）|

## 10. 避坑清单（血泪教训）

1. ISR 里绝不 printf（死锁）。
2. TX_DONE/RX_DONE 中断位要分清语义（残留置位/单位陷阱）。
3. NuttX `esp32s3_dma_setup` 只给 TX 设 EOF，RX 要自己补。
4. 降采样输出向上取整会累积越界——按输出容量封顶。
5. 控制台（USB-Serial-JTAG）高输出量会冻结，看起来像"卡死"——用听喇叭/心跳区分。
6. I2C 共享总线安全（有锁），别怀疑它——怀疑驱动的刷屏 printf。
7. 头文件位定义先和 IDF `soc/esp32s3/register/soc/i2s_struct.h` 核对再信。
