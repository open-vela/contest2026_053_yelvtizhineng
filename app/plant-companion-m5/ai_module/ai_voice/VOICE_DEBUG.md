# ai_voice 模块排查记录

## 测试命令

| 命令 | 功能 | 状态 |
|------|------|------|
| `plant voice i2ctest` | I2C 通信测试（验证码片）| ✅ 通过 |
| `plant voice es8311` | ES8311 寄存器诊断 | ✅ REG09=0xcc（16-bit Standard）|
| `plant voice patest` | GPIO46 PA 测试 | ✅ 输出寄存器翻转正常 |
| `plant voice tone 1000` | 喇叭测试音（1kHz，500ms）| ❌ 无声音 |
| `plant voice rec 3` | 麦克风录音（3秒）| ❌ 0 样本 |
| `plant voice loop 2` | 录音+回放（2秒）| ❌ RX 0 样本 |

## tone 无声音排查

### 已确认正常
- I2C：ES8311 读写成功
- ES8311 寄存器：REG09=0xcc（16-bit Standard），REG32=0xe6（70%音量），REG31=0x00（非静音）
- GPIO46(PA)：HIGH（功放使能）
- I2S TX DMA：tx_done_int_raw 触发（DMA 发送完成）
- I2S_STATE=0x00000000（TX_IDLE=0，TX 在工作）
- 波形数据：30000/32767 = 92% 振幅，1000Hz，500ms

### 未确认（需要示波器）
- GPIO17(BCK) 是否有 768kHz 方波？
- GPIO45(WS) 是否有 24kHz 方波？
- GPIO15(DOUT) 发送时是否有 I2S 数据波形？
- ES8311 OUTP/OUTN 是否有模拟输出？
- NS4150B INP/INN 是否有模拟输入？

## rec 0 样本排查

### 已确认正常
- RX DMA：rx_done_int_raw 触发（DMA 完成中断）
- hal_i2s_read 返回 OK（不是超时 -110，不是 ENOSPC -28）
- I2C：ES7210 读写成功

### 未确认（需要示波器）
- GPIO16(DIN) 在 I2S 接收时是否有数据？
- ES7210 DOUT 是否有 I2S 数据输出？
- I2S RX 数据格式是否匹配 ES7210 配置？

## 关键发现

### 1. SIG_LOOPBACK 只共享时钟，不直连数据
- `I2S_SIG_LOOPBACK` 让 TX 和 RX 共享 BCK/WS，但 RX 数据来自 DIN 引脚
- 回环测试（i2sloop）只能验证 BCK/WS 时钟，不能验证数据通路

### 2. RX 需要 TX 先跑（BCK/WS 由 TX 产生）
- ESP32-S3 I2S master 模式下，BCK/WS 由 TX 通道产生
- RX 必须在 TX 启动后才能接收数据
- hal_i2s_read 里已实现：先启动 TX 静音，再启动 RX

### 3. xiaozhi-esp32 用的是 TDM 模式（不是 Standard）
- xiaozhi 的输入用 I2S_TDM_CONFIG（4 通道），不是 Standard
- 我们用 Standard 模式，可能和 ES7210 的输出格式不匹配
- 下一步：改 RX 为 TDM 模式，或查 ES7210 的 DOUT 格式

## 下一步行动

### 最高优先级
1. **示波器验证 I2S 信号**（GPIO2/17/45/15）
2. **改 RX 为 TDM 模式**（匹配 xiaozhi 的配置）
3. **验证 ES8311 DAC 模拟输出**（OUTP/OUTN 引脚）

### 备选方案
- 改用 NuttX I2S 驱动（CONFIG_ESP32S3_I2S=y）
- 用 ESP-IDF I2S HAL（已知能在 BOX-3 上工作）

---

# 2026-08-24 NSNet2 神经网络降噪移植 + 系统级卡死排查（当前进行中）

## 背景与目标
- 用户判定手工 DSP 滤波无效，必须神经网络 NS。
- RNNoise（纯 C GRU）实测卡死弃用 → 选 **esp_sr NSNet2**（xiaozhi 同款，esp_nn 优化）。
- 移植主体**已完成并验证**：模型加载、PSRAM 分配、create 全部成功。
- **当前阻塞：`plant voice rec 2` 在录音初始化阶段系统级卡死（死锁），未定位。**

## 已完成（已验证通过的部分）

### 1. esp_sr NSNet2 集成（全部成功）
- 库：`components/voice_agent/esp_sr/lib/`（libnsnet.a + libdl_lib.a + libhufzip.a + libc_speech_features.a）
- NuttX 适配：`esp_sr_shim.c`（heap_caps→PSRAM heap、FreeRTOS mutex shim）、`srmodel_shim.c`（srmodel_load 内存解析）
- 模型：nsnet2（337KB）打包 srmodels.bin → `esp_sr/model_data/srmodels_data.c` **嵌入固件**（const 数组，DROM）
- 接口：`ai_ns.c` 用 `esp_nsn_iface_t`（esp_nsnet_handle_from_name → create → process）
- **实测成功日志**：
  ```
  srmodel: 1 model(s) loaded (nsnet2)
  ai_ns: handle=0x3c0f80e0 create=0x42053f24 process=0x42054f24 chunksz=0x42053c80
  ai_ns: calling nsnet2 create...
  [esp_sr] PSRAM not mapped, map 8MB @0x3d000000...
  [esp_sr] PSRAM vaddr: 0x3d000000-0x3d800000 self-test: OK
  [esp_sr] PSRAM heap ready: 0x3d000000 (8192 KB)
  ai_ns: create returned 0x3d000190
  ai_ns: get_samp_chunksize... → chunksize=512
  ai_ns: get_samp_rate... → samp_rate=16000
  ```

### 2. 关键坑（已解决）
| 坑 | 原因 | 修复 |
|----|------|------|
| 高层 `esp_ns.h`（ns_create/ns_process）不是神经网络 | 其实是 WebRTC 谱减法 | 改用 `esp_nsn_iface_t`（NSNet2） |
| NSNet2 create 返回 NULL | **srmodel_list_t 结构体布局错误**：esp_sr 库按 ESP_PLATFORM 编译，含 partition(8)/mmap_handle(12) 字段 → num 在偏移 16、model_data 在 20；我少 8 字节导致 model_create 读到垃圾 | 结构体补 partition/mmap_handle 占位 |
| PSRAM 区间为 0（0x3e000000-0x3e000000） | NuttX `mmu_valid_space()` 返回 0（MMU 表未建 PSRAM 条目）→ PSRAM 从未映射 | 手动 `cache_dbus_mmu_set(MMU_ACCESS_SPIRAM, 0x3d000000, 0, 64, 128, 0)` 映射 8MB（flash DROM 0x3c000000-0x3d000000 之后） |
| create 时模型加载卡死（假象） | esp_sr 库内部 ESP_LOGI 大量输出 → USB-Serial-JTAG FIFO 满 → printf 阻塞 | `esp_sr_log_silence()`（esp_log_set_vprintf 置 no-op） |

## 当前卡死问题（未解决，核心）

### 现象
- `plant voice rec 2` → 初始化阶段**系统级死锁**（无输出、无响应、不恢复）。
- **卡死位置随编译漂移**：曾卡在 `ai_ns: nsnet2 ready` 打印、`record loop start` 打印、ES8311 打印、ES7210 打印、hal_i2s_init——每次编译不同位置。
- **打印截断**（stdout 64B 缓冲 + 卡死未 flush 的假象）。
- **无 NS 版（esp_sr 未链接）早期录音正常**（echo/rec 能跑，仅偶发 RD#18 卡）。

### 最后一次实测日志（卡死点）
```
[Voice] malloc前 heap: free=158856 largest=158168
[Voice] heap: free=94808 largest=94120 used=136936
[ES8311] DAC SDP MUTE cleared (REG09=0x0c)
[VoiceAgent] ES8311 done, ticks=1744
[ES7210] 模拟电源验证: REG40=   ← 卡死（printf 部分显示，之后无任何输出）
```

### 已排除
1. **NSH 栈溢出**：`CONFIG_SYSTEM_NSH_STACKSIZE=32768`（原 4096）已加，ES8311 完整跑过（之前卡 ES8311）→ 栈不是当前根因。
2. **USB console 缓冲**：xmit 64→4096；fflush 已全删（fflush 每行→USB 传输频繁→主机跟不上→假卡死）；txready 恢复原版。
3. **GPIO19 LED 事故**：GPIO19 是 USB_D-！配置成输出会断开 USB（已回退，**严禁再碰 GPIO19/20**）。
4. **esp_sr 库 bss**：~10KB，不构成内存压力。

### 未排除（当前嫌疑，按优先级）
1. **卡死点在 hal_i2s_init（ES7210 printf 后）**：已加分段诊断 `[I2S] init: sem/gpio/regs/dma-request/tx-irq/rx-irq/dma-eof/done`——**下一次实测看停在哪个标记**。
2. **esp_sr 库链接的静态影响**（内存布局/.iram1 段/中断向量）：卡死发生在 ai_ns_init（NS 执行）**之前**（voice_agent_init 阶段）→ **与库的执行无关，怀疑库的链接/段布局**。重点检查 `esp32s3_dsp.S.obj` 的 `.iram1` 段是否被 NuttX 链接脚本正确处理。
3. **I2C 总线（ES7210 读 REG40/4B/4C）卡死**：`hal_i2c_read_reg` 是否在 I2C 驱动层死等。
4. **中断系统被破坏**：USB 输出（txint）、RX DMA（in_suc_eof）、I2C 中断同时依赖中断——如果中断被禁/配置破坏，全系统死锁。

### 待办（新话题接续）
1. 跑最新固件（hal_i2s 分段诊断版）`plant voice rec 2`，**记录 `[I2S] init:` 停在哪个标记** → 定位 hal_i2s_init 内部卡点。**✅ 固件已就绪**（`nuttx/nuttx.bin` 与 `debug/fw_bak/nuttx_NS_diag.bin`，含分段诊断 + esp_sr 全量，8/24 14:29 重新编译验证通过）。
2. 若卡在 `[I2S] init: gpio` 之前 → 检查 configure_gpio（esp32s3_configgpio/gpio_matrix 是否死等）。**✅ 已静态审查：configure_gpio 全为寄存器写 + gpio_matrix_out/in，无等待循环；esp32s3_configgpio 对 19/20 引脚有 USB pad 保护，且本代码不碰 19/20。**
3. 若卡在 dma-request → 检查 `esp32s3_dma_request`。**✅ 已静态审查：nxmutex_lock 仅保护通道分配（毫秒级），无死等。**
4. 若卡在 irq → 检查 `esp32s3_setup_irq`。**✅ 已静态审查：enter_critical_section 短暂临界区 + esp32s3_alloc_cpuint，资源不足返回错误，无死等。**
5. 若 `[I2S] init: done` 出现但仍卡 → 卡在 `hal_i2s_start_tx_clock` 或之后。**⚠️ hal_i2s_start_tx_clock → hal_i2s_write：TX_DONE 轮询带 3s 超时 + 1s 预警打印，不会无限死等。**
6. **并行排查 esp_sr 库段布局**：`nm nuttx` 看 esp32s3_dsp.S.obj 的 .iram1 段地址、与 NuttX IRAM 段是否冲突；检查链接脚本（legacy_sections.ld）是否包含 .iram1。**✅ 已完成（见下方 §2026-08-24 静态分析结论）：无冲突。**
7. **验证"库链接影响"二分**：临时注释 Makefile 的 ESP_SR_LIBS 合并 + ai_ns 调用（改回无 NS 版），若录音恢复 → 确认为库静态影响；再逐步加回。**✅ 已做成编译开关 `make PLANT_NO_NS=1`（见 §2026-08-24），且发现反证：无 NS 版同样卡死过（rec_ns2/rec_ns_t1，got=1424）。**
8. **I2C 驱动超时**：检查 esp32s3_i2c 是否有传输超时（若无 → 加超时防止死等）。**✅ 已确认：`i2c_sem_waitdone` 用 `nxsem_tickwait_uninterruptible(ESP32S3_I2CTIMEOTICKS)`，`CONFIG_ESP32S3_I2CTIMEOMS=500` → 单条消息 500ms 超时返回 -ETIMEDOUT；I2C 不会永久死等（唯一前提：系统 tick 中断活着）。**

---

# 2026-08-24（实机）根因确认与阶段性结论

## 一、设备层 100% 调通（铁证）

`plant voice rec 2` / AUTOTEST 实机日志（从 xmit 缓冲区读出的完整序列）：
```
[ES8311] DAC SDP MUTE cleared (REG09=0x0c)          ✓
[VoiceAgent] ES8311 done
[ES7210] 模拟电源验证: REG40=0x43(期望0x43) ...     ✓
[ES7210] init OK: Slave, I2S Std, 16-bit, 24kHz
[VoiceAgent] ES7210 done
[I2S] init: sem/gpio/regs/dma-request/tx-irq/rx-irq/dma-eof/done ✓ 全过
[VoiceAgent] hal_i2s_init done / TX clock / restore_analog REG40=0x43 ✓
```
**ES8311/ES7210/I2S 初始化从未卡住**——控制台显示"卡在 [ES8311] DAC"是 **USB-Serial-JTAG 控制台 TX 堵住**（独立问题，输出积压在 xmit 缓冲区，系统继续运行）。**无 NS 版（AUTOTEST）录音 2s + 回放完整成功**（播放完成、回到 nsh）→ **录音链路全通**。

## 二、本次修复（均已实机验证）

| 修复 | 效果 |
|------|------|
| `xtensa_saveusercontext.c` 内联汇编加 `"a2","memory"` clobber + `mov a2,%0` | 消除 `s32i.n a2,a2,0` 往 flash 写 → LoadStoreError 的崩溃（GCC 14.2 触发） |
| `CONFIG_ARCH_INTERRUPTSTACK=4096` | 崩溃信息不再因中断栈溢出而丢失 |
| `CONFIG_MM_DEFAULT_ALIGNMENT=16` | PSRAM 堆 16 字节对齐（尝试解决 NS，未生效但保留） |
| plant-companion Makefile esp_sr 合并加 flock + `ar s` | 修复并行编译损坏 libapps.a（nsh_initialize/g_builtins 未定义） |

## 三、NS（esp_sr NSNet2）环境问题留档（未解决，独立攻关）

**对标 xiaozhi 逐项核对——全部一致**：
- esp_sr 库 4 个 .a（libnsnet/libdl_lib/libhufzip/libc_speech_features）md5 与 xiaozhi 完全相同
- nsnet2 模型（nsnet2_data 337349B）与 xiaozhi 完全相同
- `srmodel_data_t`/`srmodel_list_t` 结构与 xiaozhi `model_path.h` 逐字段一致
- ES8311/ES7210 寄存器配置与 xiaozhi（esp_codec_dev）完全一致

**现象**：NS 模型加载成功（`create` 曾返回 0x3d000190、chunksize=512），但推理时崩溃——**崩点随编译漂移**（旧固件崩在 `process` 第一次调用 `dl_nn_args_t.c:137` 断言；新固件崩在 `create` 过程中）。断言经 `xtensa_user`（用户异常）→ `_assert` → `reset_board` 无限循环（`CONFIG_BOARD_RESET_ON_ASSERT` 因依赖 `BOARDCTL_RESET` 未生效）。

**已排除**：库/模型/结构版本差异、PSRAM 双重管理（主堆不含 PSRAM，shim 私有堆独占 0x3d000000）、对齐 16。

**嫌疑（未验证）**：heap_caps_malloc 语义与 ESP-IDF 差异（失败 fallback 到内部 RAM）、PSRAM DCache 一致性、hufzip 解压路径。**崩点漂移 = 内存破坏类问题**。

**后续攻关方案**：
1. `CONFIG_BOARDCTL_RESET=y` + `CONFIG_BOARD_RESET_ON_ASSERT=1`（崩溃可见/可复位）
2. 断点抓 `model_create`/`dl_nn_args_check` 内部（AUTOTEST 已启用）
3. 对比移植 xiaozhi 的 `model_path.c` 源码（替代 srmodel_shim）消除解析差异

## 四、可用固件

| 固件 | 说明 |
|------|------|
| `debug/fw_bak/nuttx_noNS_working.bin`（946KB） | **无降噪版，录音+回放实机验证通过**（当前可用） |
| `debug/fw_bak/nuttx_NS_diag.bin`（1499KB） | NS 版（含调试标记，NS 推理会崩，仅攻关用） |

---

# 2026-08-24（下）静态分析结论 + 卡死现场取证工具（本轮完成）

## 一、结论速览

| 疑点 | 结论 | 依据 |
|------|------|------|
| esp_sr `.iram1` 段与 NuttX IRAM 冲突 | **排除**（链接层面） | legacy_sections.ld L79 已含 `*(.iram1 .iram1.*)`；esp_sr `.iram1` 实际落在 0x40374400~0x403787xx（合法可执行 IRAM）；重定位全部段内相对（`R_XTENSA_SLOT0_OP .iram1+off`），无绝对地址假设 |
| esp_sr 库有静态构造/初始化器 | **排除** | 4 个库 objdump 均无 `.init_array/.ctors/.dtors/.preinit` |
| **PSRAM 双堆重叠（shim 私有堆 vs NuttX 主堆）** | **排除** | `mmu_valid_space()` 实测返回 0（esp32s3_spiram.c L405）→ NuttX `xtensa_add_region`（esp32s3_allocateheap.c L220-233）加 0 字节 → **主堆不含 PSRAM**；唯一持有者是 shim 私有堆（`mm_initialize("psram", 0x3d000000, 8MB)`）→ 单所有权，无双重分配/堆重叠 |
| **PSRAM MMU 映射尺寸**（shim 手动 map 8MB） | **排除** | `MMU_PAGE_SIZE=0x10000`(64KB, hardware/esp32s3_cache_memory.h L40)；shim `num=0x800000>>16=128` 页 × 64KB = **8MB 全映射** ✓（与 NuttX 自身 esp32s3_spiram.c 同约定 psize=64/num=size>>16；硬件自检 OK） |
| esp_sr 合并挤压 IRAM 溢出 | **排除** | `.iram0.vectors`(0x40374000,1KB) + `.iram0.text`(0x9940=39KB) 结束于 0x4037DE00，iram0_0_seg 上限 0x403cc700，余量充足 |
| I2C 驱动死等 | **排除**（有超时） | 见待办8 |
| esp32s3_dma_request / setup_irq 死等 | **排除** | 仅短暂临界区/互斥 |
| **控制台（USB-Serial-JTAG）输出阻塞** | **头号嫌疑（未排除）** | 见下文"控制台阻塞机制" |
| 录音环路真实挂起（I2S/DMA 层） | **未排除** | 无 NS 版在 got=1424 确定性卡死（rec_ns2/rec_ns_t1），需 freeze_diag 定案 |

## 二、关键新证据：无 NS 版同样卡死（二分验证的反证）

`debug/` 目录 8/24 上午日志（esp_sr libs 10:07 才拷贝，09:59/10:04 的固件不可能链入 libnsnet.a）：

- **rec_ns2.txt（09:59）**：`plant voice rec 2` — 初始化**全部成功**（ES8311/ES7210/hal_i2s/restore_analog 全打印），录音循环第 3 块后卡死（最后一行 `[Voice] 块@got=1424 峰值=32767 满幅首位置=508`，之后无输出）。
- **rec_ns_t1.txt（10:04）**：`plant voice rec 1` — 同样初始化成功，**同样卡在 got=1424**（`块@got=712` → `块@got=1424` → 无输出）。
- 日志中**无任何 ai_ns/esp_sr 输出**（ai_ns_init 会打印 handle/create/chunksize；失败会打印"NSNet2 init 失败"），且无 `[Voice] NSNet2 init 失败` → **这两个版本未编译/未执行 NS**。

→ 结论：**卡死不限于 NS 版**。文档"无 NS 版早期录音正常（仅偶发 RD#18 卡）"与"卡死位置随编译漂移"互相印证：这是一个**间歇性、随构建漂移**的既有问题，esp_sr 合并只是改变了触发位置/概率。二分验证（待办7）仍需做，但预期是"无 NS 版也可能复现"。

## 三、控制台阻塞机制（头号嫌疑，待 freeze_diag 定案）

输出链路（printf → 假卡死）：
```
printf → syslog ring(196B, CONFIG_SYSLOG_BUFSIZE=196)
       → uart xmit buffer(4096B, esp32s3_usbserial.c)
       → USB-Serial-JTAG HW FIFO(64B)
       → USB 端点 → 主机 cdc_acm → minicom
```
- 阻塞点（**代码级确认，drivers/serial/serial.c，两处无界等待**）：
  1. `uart_putxmitchar()` L319：`nxsem_wait(&dev->xmitsem)` —— xmit buffer 满且 HW 不排空时**永久阻塞**（NSH 主线程 printf 路径）。
  2. `uart_putchars()` L377：`while (!uart_txready(dev)) {}` —— **无界自旋**（ISR/直接写路径）。USB-Serial-JTAG 的 `esp32s3_txready` 读 `SERIAL_IN_EP_DATA_FREE`，host 端不读（minicom 停/ModemManager 拉 DTR 复位 USB/端点挂起）→ FIFO 满 → 该位为 0 → 自旋/阻塞。
- **NuttX 作者自证**（serial.c L281-284 注释）："NOTE: On certain devices, such as USB CDC/ACM, the entire TX buffer may have been emptied in this race condition. In that case, the logic would hang below waiting for space in the TX buffer without this test." —— 作者明确点名 USB CDC/ACM 会挂。
- **代码自证**：`ai_voice.c` L326/L336/L365 的 `#if 0` 注释原文：
  `⚠️ 诊断打印已关闭：USB-Serial-JTAG 输出积压会阻塞主线程（假卡死）`、`心跳...（控制台冻结 ≠ 系统卡死）`。
- **驱动层修复仍未做**（MIC_RX_BUG_REPORT §28.2.7 的"FIFO 满应丢弃而非阻塞"= 待修）。
- 与"卡死位置随编译漂移"吻合：不同构建输出量/时序不同 → 缓冲填满点不同。
- ⚠️ 注意：host 端 ModemManager 会把 ttyACM0 当 3G 猫探测（拉 DTR 复位芯片 → USB 重枚举 → 端点挂起 → 输出阻塞），openocd.sh 已警告过。**测试时确认 `systemctl is-active ModemManager` 为 inactive。**

### 预备修复（【仅当 freeze_diag 判 A（控制台阻塞）后使用】，勿先行改动 kernel）
把 serial.c 两处无界等待改为有界（超时丢字符继续，假卡死 → 系统继续跑）：

```c
/* drivers/serial/serial.c */

/* 1) uart_putxmitchar() L319：nxsem_wait → 有界等待（超时丢该字符） */
-  ret = nxsem_wait(&dev->xmitsem);
+  ret = nxsem_tickwait(&dev->xmitsem, MSEC2TICK(500));   /* 500ms 上限 */

/* 2) uart_putchars() L377：无界自旋 → 有界自旋（超时丢整段，直接返回） */
-      while (!uart_txready(dev))
-        {
-        }
+      {
+        int guard = 0;
+        while (!uart_txready(dev) && ++guard < 500000)
+          {
+          }
+        if (guard >= 500000)
+          {
+            return;                 /* 硬件长时间不可写：丢弃剩余输出 */
+          }
+      }
```
注意：
- 该改动影响**所有**串口设备（不只 USB-Serial-JTAG），但仅在硬件长时间不排空时触发（正常时永不触发），语义="控制台可丢数据但系统不死锁"。
- 必须与 freeze_diag 分类结果配合：判 A 再上；若判 B/C（真死机）此修复无效，回到 hal_i2s/DMA/中断方向。

## 四、新增工具（本次交付）

1. **卡死现场取证 `debug/freeze_diag.py`**（GDB Python，配合 `openocd.sh -b`）：
   **一键方式（推荐）：`./debug/freeze_capture.sh`** —— 自动起 OpenOCD + 跑取证 + 输出存 `debug/freeze_capture_<时间戳>.txt`。
   手动方式：
   ```
   /home/vboxuser/.espressif/tools/xtensa-esp-elf-gdb/12.1_20231023/xtensa-esp-elf-gdb/bin/xtensa-esp32s3-elf-gdb \
       -x debug/freeze_diag.py nuttx/nuttx
   ```
   自动：halt → PC/bt → **tick 存活测试**（读 g_system_ticks（脚本按符号名解析地址，resume 2s 再读））→ I2S0/GDMA 寄存器 → 当前任务 → 分类：
   - **A 控制台阻塞**：tick 活着 + PC 在 usbserial/uart/syslog 路径
   - **B 任务死锁**：tick 活着 + PC 在 nxsem/spinlock 等待
   - **C 真死机**：tick 冻结（timer 中断死）
   - **D 崩溃/内存破坏**：bt 含异常/panic 帧或 PC 在垃圾地址
   一次运行即可区分"假卡死（控制台）"与"真死机"，是当前问题的决定性诊断。

2. **二分验证编译开关 `make PLANT_NO_NS=1`**（apps/plant-companion/Makefile + ai_voice.c 已加 `#ifndef PLANT_NO_NS` 守卫）：
   - 完全移除 ai_ns.c + esp_sr 4 个 shim/cJSON/模型源 + ESP_SR_LIBS 合并 + NS 调用。
   - **⚠️ 使用前必须强制重编 NS 相关源**（NuttX 依赖跟踪不感知 Makefile 变量变化）：
     ```
     cd /home/vboxuser/openvela && source ~/openvela-venv/bin/activate
     touch apps/plant-companion/ai_module/ai_voice/ai_voice.c \
           apps/plant-companion/ai_module/ai_voice/ai_ns.c \
           apps/plant-companion/components/voice_agent/esp_sr/esp_sr_shim.c \
           apps/plant-companion/components/voice_agent/esp_sr/srmodel_shim.c
     make -C nuttx PLANT_NO_NS=1      # → nuttx.bin ≈ 976KB（无 NS）
     make -C nuttx                     # → 恢复正常 NS 版（≈1499KB）
     ```
   - 已实测两版均编译通过：NS 版 1499824B（esp_sr 符号 14 个、诊断串 9 条齐全），无 NS 版 976496B（esp_sr/iram1 符号 0）。
   - 备份：`debug/fw_bak/nuttx_NS_diag.bin`（NS 诊断版）、`debug/fw_bak/nuttx_noNS_bisect.bin`（无 NS 二分版）。

## 五、下一步实测（用户执行，按序）

> **状态（2026-08-24，沙箱侧）**：静态排查阶段已完成（见 §一~§四，10 项排除/嫌疑全部闭环），
> 剩余**实机验证**只能由用户在有板子的环境执行——沙箱容器能力集为空（`capsh`：`Current: =` / `Bounding set =`），
> 无法创建 `/dev/ttyACM0`（mknod 被拒）也不能直通 USB，烧录/取证均需宿主机。
> 固件与工具已全部就绪，按下面 4 步执行即可。

1. **关 ModemManager**：`systemctl is-active ModemManager` → active 则先停。
2. 烧录当前 `nuttx/nuttx.bin`（NS 诊断版）→ `minicom -D /dev/ttyACM0 -b 115200` → `plant voice rec 2`：
   - 记录最后一行（`[I2S] init:` 停在哪个标记 / 或更早的 ES7210 打印）。
3. **卡死后**：关 minicom → `./debug/freeze_capture.sh` → 贴结论（A/B/C/D）。
   - 若 **A（控制台阻塞）** → 修 esp32s3_usbserial 驱动（FIFO 满丢字符不阻塞）或降低输出量；ModemManager 嫌疑先排除。
   - 若 **C/D（真死机/崩溃）** → 按 freeze_diag 的 PC/bt/寄存器继续定位。
   - 若卡在 `[I2S] init: xxx` 标记且 freeze_diag 判 B/C → 查该标记对应函数（见待办2-5）。
4. 二分对照：`make PLANT_NO_NS=1`（见上）→ 烧 `debug/fw_bak/nuttx_noNS_bisect.bin` 或重新编译 → 同命令复测 → 对比卡死位置/是否复现。

## 安全红线（不变）
- 严禁操作 GPIO19/20（USB_D-/D+）。
- 串口输出保持普通 printf，勿加 fflush/setvbuf/自创直写。
- 编译/烧录/验证命令见本文件头部（或根目录 CLAUDE.md）。

## 相关文件
| 文件 | 说明 |
|------|------|
| `apps/plant-companion/components/voice_agent/esp_sr/` | 库 + shim + 模型数据 |
| `apps/plant-companion/ai_module/ai_voice/ai_ns.c/h` | NSNet2 封装 |
| `apps/plant-companion/ai_module/ai_voice/ai_voice.c` | 录音主流程 |
| `apps/plant-companion/components/voice_agent/voice_agent.c` | 码片初始化（卡死区） |
| `nuttx/arch/xtensa/src/esp32s3/hal_i2s.c` | I2S HAL（含分段诊断） |
| `apps/plant-companion/components/voice_agent/MIC_RX_BUG_REPORT.md` | 历史音频问题记录（§29 移植记录） |

### 2026-08-24（实机·刺啦声排查）数据闭环记录
- **现象**：37.5dB 增益后有信号（非零99%）但回放"刺啦"，听不到环境声
- **数据闭环**：rate 380000B/s=190k int16/s；loop 单槽 got=96135/2s=48k/s
- **两种解释**：①WS=48k（4槽×48k=192k）②WS=24k 且 RX 每帧存 8 int16（每槽 32bit）
- **寄存器**：TX/RX_CONF1（half_sample=32/ws_width=32/bits_mod=15/chan_bits=15）、TDM_CTRL（tot_chan=3=4槽）、CLKM（div26/13）、BCK_DIV=3——**与 IDF i2s_ll.h/i2s_hal.c 逐位一致**（已对照）
- **已试无效**：TX_UPDATE 等待自清零、SIG_LOOPBACK 恢复=1（IDF 全双工标准）
- **下一步**：示波器量 GPIO2(MCLK=6.154M)/GPIO17(BCLK=1.539M)/GPIO45(WS=24k) 三频，区分"时钟错"vs"RX帧结构错"；或实验改 RX TDM 槽数

### 2026-08-25（输出链路核查 + 滤波链放宽）结论
- **用户疑"ES8311 输出问题"→ 核查结果：输出侧配置全部正确，非输出问题**：
  - PA 使能 GPIO46=高（`hal_i2s.c` configure_gpio L109-112，`HAL_PA_PIN=46`）✓
  - I2S 引脚 MCLK=2/WS=45/BCLK=17/DOUT=15/DIN=16 —— 与 xiaozhi `main/boards/esp-box-3/config.h` **完全一致** ✓（esp-box-3 即 WS=45；esp-box 一代才用 WS=47）
  - ES8311 寄存器对照 IDF esp_codec_dev 完成：REG09 bit6 DAC SDP MUTE 已清、音量 REG32=0xE6(70%)、REG00 slave、REG01 use_mclk ✓
  - NuttX I2S 位域宏（esp32s3_i2s.h）与 ESP32-S3 硬件布局一致（TX_BITS_MOD[17:13] 等）✓
- **"听不到"真凶嫌疑：录音侧滤波链把声音弄没**（loop = 录→回放，录到零 → 回放无声）：
  - 噪声门 |s|<80 归零：30dB 增益下安静环境底噪若<80 → **整个录音全零**
  - 限幅 ±4000：信号峰值 27415 被削平，动态全无
- **已改**（PLANT_NO_NS 分支）：限幅 ±4000→**±30000**（不削声）、**删除噪声门 80**；保留高通220Hz/11点中值/低通5kHz
- **下一步实机验证**：
  1. `plant voice tone 440` → 应听到 440Hz 纯音（**独立验证输出链路**，不经过麦克风）
  2. `plant voice loop 2` → 应听到环境声（放宽滤波后）
  3. 若 tone 有声而 loop 无声 → 麦克风/录音链路问题；若 tone 也无声 → 回头查 ES8311 硬件/PA

### 2026-08-25（续）诊断版固件——时钟体系定案工具
- **现象**：放宽滤波后 loop 录到 峰值23949/非零99%/均值-669/尖峰>2000 达10460个，回放仍是噪声（用户："依旧是噪声"）；tone 440 播放 log 显示 40×4800B 在 ~1 tick 内完成（TX_DONE 疑似假完成/或 DMA 不等 FIFO 发空）
- **核心怀疑：I2S 时钟体系整体 2 倍速**（MCLK_OUT=12.31M=160/13=RX 分频值，而非 6.15M=160/26=TX 分频值）：
  - 证据①：数据率 190k int16/s（4槽×47.5k）→ WS≈48k 而非 24k
  - 证据②：示波器 GPIO2 实测 ~13M（期望 6.15M）
  - 证据③：mclk_sel(rx_clkm_conf bit29)=0 时 MCLK_OUT 应绑 TX(6.15M)，实测 12.3M → 要么 mclk_sel 写入未生效，要么 TX 时钟域本身 2 倍
  - 后果：码片按 48k 采样率工作，软件按 24k 生成/解析 → 播放音调翻倍、数据错位 → 刺耳"噪声"
- **已加诊断**（hal_i2s.c）：
  - init 后打印 TX_CLKM/RX_CLKM（含 mclk_sel）、TX/RX_CONF1、TX/RX_TDM_CTRL、TX/RX_CONF 读回值
  - hal_i2s_write 结束打印耗时（tone 48000B：50tick=24k体系 / 25tick=48k体系）+ TX_IDLE（数据是否真发完）
- **用户实测**：烧录后 `plant voice tone 440`，贴 `[I2S] regs:` 与 `[Voice] TX done:` 行 + 描述听到的是 440Hz 蜂鸣 / 噪声 / 无声
- **判读**：mclk_sel=1 → 清位失败/被覆盖，改强制写 0 或改用 IDF i2s_ll_mclk_bind_to_tx_clk 流程；若 TX 时钟本身 2 倍 → 查 PLL/时钟源；若确认 48k 体系自洽 → 软件全面改 48k（tone/play/record 采样率）

### 2026-08-25（定案）TX 播放截断根因 = 完成信号用错（"都的一声"）
- **寄存器实况（全部正确，24k 体系坐实）**：
  - TX_CLKM=0x3400001a（clk_sel=2 PLL160M, N=26 → MCLK=6.154M）
  - RX_CLKM=0x1400000d（N=13, **mclk_sel=0** → MCLK_OUT 绑 TX=6.154M ✓）
  - TX/RX_CONF1=0x6f7de19f/0x2f7de19f（ws_width=32、half=32、bits=16、msb_shift=1 → WS=24k ✓）
  - TX_TDM_CTRL=0x30003（4槽/发槽0-1）RX_TDM_CTRL=0x3000f（4槽全开）✓
  - TX_CONF=0x08089200（sig_loopback=1、tdm_en=1、left_align=1）✓
- **真凶**：hal_i2s_write 用 **I2S TX_DONE 作块完成信号 = 错**（TX_DONE 语义=FIFO 空，DMA 启动前即置位 → 每块假完成）→ 播放只发出 FIFO 开头一小段 → tone 48000B "0 ticks 返回 + TX_IDLE=0" → 用户听到"都的一声"
- **修复**：改用 **GDMA OUT_TOTAL_EOF_CH0_INT_ST(bit3)**（官方 esp32s3_i2s.c L2320 同款）+ 函数末尾等 TX_IDLE=1（串行发完）+ 打印真实耗时（tone 48000B → 50ticks=24k / 25ticks=48k）
- **判读**：播放耗时 ~50 ticks → 24k 体系正确，播放完整（440Hz 500ms"嘟——"）；~25 ticks → 48k 体系（软件采样率全面改 48k）；0 ticks 且 TX_IDLE=1 → DMA 数据没进 FIFO（查 DMA 通道/时钟）

### 2026-08-25（定案2）系统卡死 = GDMA OUT/IN 中断共用线 → 中断风暴
- **现象**：`plant voice tone` 卡在 `[Voice] TX: bytes=512` 后无任何返回（多版本复现）
- **排除**：freeze_diag 输出不可信（gdb 12.1 vMustReplyEmpty 读寄存器全 -1，且工具先 halt CPU 造成 tick 冻结假象 → 误判 [C] 真死机，勿采信）
- **机制**（代码版本对照定案）：
  - bash-19（无 OUT_INT_ENA、无 enable_irq(g_tx_irq)）→ 超时正常返回，系统活
  - bash-20（+ enable_irq(g_tx_irq)）→ 卡死
  - bash-23（删 enable_irq 但保留 SET OUT_INT_ENA）→ 仍卡死
  - **根因**：GDMA OUT 与 IN 中断共用 CPU 中断线（up_enable_irq(g_rx_irq) 已开）；TX DMA 完成(TOTAL_EOF) → 触发 RX ISR → 只清 IN 不清 OUT → 立即再触发 → 无限风暴 → tick 冻结
- **修复**：彻底不设 DMA_OUT_INT_ENA（hal_i2s_write 只轮询 TX_IDLE 无需中断）；rx/tx ISR 均改为清 IN+OUT 双中断
- **保持**：一次性大描述符链（16×4095B）+ 等 TX_IDLE + 双兜底（空转6000万次≈1.2s + tick 5s，物理不卡死）

### 2026-08-25（突破）播放链路打通（tone 完整）
- **实测**：`plant voice tone 440` → `TX done: 48000 B in 50 ticks, TX_IDLE=1` + 听到完整 440Hz"嘟——"
- **50 ticks=0.5s=48000B@96kB/s → 24k 体系最终确认**（寄存器 mclk_sel=0、N=26、WS=24k 全部正确）
- 播放链路全通：一次性大链 DMA + 等 TX_IDLE + 无 TX 中断（修复中断风暴）
- **下一步**：`plant voice loop 2` 验证完整 录→回放 链路（record 用 RX IN_SUC_EOF 中断，此前已正常采到数据）

### 2026-08-25（架构决策）迁移 NuttX 官方 esp32s3_i2s 驱动
- **背景**：手写 hal_i2s.c（寄存器直写）重造驱动但丢/错 4 件事：
  ①RX_RESET 清时钟域帧配置 → RX 48k 错位噪声 ②DMA 完成信号用错（I2S TX_DONE=FIFO 空）→ 播放截断 ③GDMA OUT/IN 共用中断线 → 风暴卡死 ④手工滤波对付不了 ES7210 宽带底噪
- **决策**：迁移官方驱动 esp32s3_i2s.c（与 IDF 例程同架构：时钟自动分频、DMA/EOF 中断、全双工 SIG_LOOPBACK、配置重放）
- **关键适配**：
  - 官方驱动只支持 1/2 通道（total_slot=2、32bit 帧）→ **ES7210 改 2 麦标准模式**（REG12=0x00 非 TDM、REG01=0x34 只开 MIC1/2 时钟、MIC3/4 关闭）——xiaozhi 麦数<3 同样配 REG12=0x00
  - hal_i2s.c 重写为官方驱动封装（保持 hal_i2s.h API）：i2s_send/i2s_receive + apb_buffer 同步包装；read_slot 取立体声流左声道=MIC1 24k
  - Kconfig：CONFIG_ESP32S3_I2S0 + MASTER + 16BIT + 24000 + MCLK=2/BCLK=17/WS=45/DIN=16/DOUT=15
  - TX FIFO 空保持时钟：hal_i2s_init 补 STOP_EN=0（官方驱动未配，录音只开 RX 时需要）
  - record 适配：want_samples=24k×s、24k→16k 3:2 线性插值（read_slot 已解出连续单声道）
- **验证命令**：tone 440（完整"嘟——"）+ rec 2（说话 vs 安静差异明显）+ loop 2
- **备选**（若官方驱动 TDM 能力不足或仍有问题）：直接移植 IDF i2s_tdm 驱动代码到 NuttX；或整体改用 IDF 工程（用户意向）

### 2026-08-25（✅ 里程碑）官方驱动迁移成功——链路打通
- **实测**：`plant voice rec 2` ×3 + `loop 2` ×2
  - 均值 -700 → **-5~1**（DC 偏置消除）
  - 安静底噪峰值 300-900、尖峰 0（原固定 11987/46% 大值）
  - 说话峰值 7065-14053（麦克风真实拾音）
  - **用户："有环境声和人声了，就是有点噪声"**
- **根因闭环**：手写 hal_i2s 的 4 类问题（RX_RESET 清配置/DMA 完成信号错/中断风暴/帧错位）全部由官方驱动框架层解决
- **剩余**：底噪优化（软噪声门 / NSNet2 恢复）

# 2026-08-25 全链路复盘（卡点 → 解决 → 为什么能解决）

> 用户要求：把之前卡住的点、如何解决的、为什么能够解决，完整写入工作日志。
> 结论先行：链路已通（能听到环境声和人声），剩余只有底噪优化。

## 一、卡点全景（按时间线）

### 卡点1：录音初始化"系统级卡死"
- **现象**：`plant voice rec 2` 卡在 `[ES8311] DAC` 附近无响应
- **排查**：freeze_diag 判 A/B/C/D；发现是**控制台假卡死**——USB-Serial-JTAG 的
  xmit 缓冲（4096B）/HW FIFO（64B）被大量 printf 灌满，host 不读时
  `nxsem_wait(&dev->xmitsem)` 永久阻塞，任务卡在 printf 而非系统死
- **解决**：控制输出量、用 OpenOCD 读内存取证（gdb 12.1 有 vMustReplyEmpty 问题，
  freeze_diag 结论需打折）
- **为什么能定位**：OpenOCD 读 I2S/ES7210 寄存器证明外设初始化全部成功，
  数据流其实跑到了 NS 处理才崩

### 卡点2："听不到"（增益 + 滤波链互相打架）
- **现象**：18dB 听不到 → 37.5dB 削顶+刺啦 → 30dB 听不到
- **根因**：①增益 18dB 比 xiaozhi（40dB）低 20dB，环境声淹没在底噪里；
  ②滤波链的**噪声门 |s|<80 归零**在安静环境把整个录音清零；**限幅 ±4000**
  把峰值 27415 削平
- **解决**：增益对齐 30dB（折中）；放宽限幅 ±30000；删噪声门
- **为什么**：对照 xiaozhi（40dB 值14）与 MIC_RX §28.3 验证链，参数要随
  增益体系重算，不能沿用低增益时代的门限

### 卡点3："刺啦" + "都的一声"（播放截断）
- **现象**：tone 只响"都的一声"；loop 回放刺啦
- **根因**：hal_i2s_write 用 **I2S TX_DONE 当块完成信号**——TX_DONE 语义是
  "FIFO 空"（DMA 启动前即置位）→ 每块假完成 → 数据只发出 FIFO 开头一小段
- **解决**：改用 GDMA OUT_TOTAL_EOF（raw 轮询）→ 最终重写为**一次性大描述符链
  + 等 TX_IDLE=1**（I2S 硬件真实"串行发完"状态）
- **为什么**：对照官方 esp32s3_i2s.c L2320 用 TOTAL_EOF、IDF i2s 用 DMA EOF，
  而 I2S TX_DONE 从不是传输完成信号

### 卡点4：中断风暴卡死（tick 冻结、连超时都不打印）
- **现象**：tone 卡死、freeze_diag 读寄存器全 -1、tick 冻结
- **根因**：GDMA **OUT 中断与 IN 中断共用 CPU 中断线**（up_enable_irq(g_rx_irq)
  已开）；设置 OUT_INT_ENA 后 TX DMA 完成 → 触发 RX ISR → 只清 IN 不清 OUT →
  立即再触发 → 无限中断风暴 → CPU 卡死在 ISR
- **解决**：彻底不使能 OUT 中断（轮询 TX_IDLE 不需要它）；ISR 清 IN+OUT 双中断
- **为什么**：版本对照定案（bash-19 无 OUT 中断能返回 / bash-20/23 有则卡死），
  中断使能必须与 ISR 的清理动作完整配套

### 卡点5："说话不说话一样"的固定噪声（RX 帧错位）
- **现象**：录音峰值固定 20303/32725、每块 503 位置满幅、46% 大值，与环境无关
- **根因**：hal_i2s_read 每次 **RX_RESET 把时钟域帧配置清回默认**（ws_width/
  half_sample/tot_chan 回 0）→ RX 按 32bit 帧（WS=48k）解析，把 ES7210 的
  64bit/24k 帧拆错位 → 确定性噪声
- **解决**：RX_RESET 后重写 RX_CONF1 + RX_TDM_CTRL + RX_UPDATE（首轮修复）；
  最终由官方驱动彻底解决（每次启动重放配置）
- **为什么**：对照 IDF i2s_hal_tdm_set_rx_slot——IDF 在每次 start 都重写寄存器；
  手写版只在 init 写一次，reset 后即丢

### 卡点6（总根因）：手写寄存器驱动 = 残缺驱动
- 手写 hal_i2s.c 把驱动框架自动处理的 4 件事做错/丢掉：
  ①RX_RESET 后配置不重放 ②DMA 完成信号用错 ③中断生命周期不完整 ④帧结构
  与码片/总线三方不一致
- **最终解决**：**迁移 NuttX 官方 esp32s3_i2s.c 驱动**（与 IDF 例程同架构：
  时钟自动分频、DMA/EOF 中断、全双工 SIG_LOOPBACK、配置重放）
- **配套决策**：官方驱动只支持 1/2 通道 → **ES7210 改 2 麦标准模式**
  （REG12=0x00 非 TDM、REG01=0x34、MIC3/4 关闭）——xiaozhi 麦数<3 同样配
  REG12=0x00，与官方驱动 2 槽原生匹配
- **为什么能解决**：把"通信链路"从自己实现（残缺）拉回与例程同一起跑线；
  之前所有怪异 bug（刺啦/听不到/卡死/固定噪声）都是框架层机制缺失的投影

## 二、为什么官方驱动迁移能一次打通（框架层根因）

| 手写版问题 | 官方驱动机制 |
|---|---|
| RX_RESET 清帧配置 | 每次 start 重写寄存器（i2s_configure/set_datawidth） |
| I2S TX_DONE 假完成 | GDMA OUT_TOTAL_EOF + 完整 ISR 清标志 |
| OUT/IN 中断共用风暴 | 独立 CPUINT + ISR 清 UINT32_MAX |
| 时钟/帧三方不一致 | i2s_set_clock 自动算 MCLK=24k×256=6.144M、BCLK=768k、WS=24k |
| 全双工时钟共享折腾 | 官方 `if (tx_en && rx_en) SIG_LOOPBACK=1` 一行解决 |

## 三、降噪现状与量化依据（2026-08-25 安静环境实测）

`plant voice rec 2`（安静/应急环境）：
- 滤波后安静段峰值：222 / 253 / 256；全段峰值 1072（段1 起始瞬态）
- 均值 3（无 DC）；**残留尖峰>2000 = 0 个**；大值连续段 0
- 对比：说话时峰值 7065 / 10385 / 14053

**依据结论**：
- 手工门限候选：底噪上界 ~300，语音下界 ~7000 → 门限 500 附近有 2 倍以上
  余量（**待 RMS/分位数统计确认，勿拍脑袋**）
- 生产方案：xiaozhi 同款 **NSNet2**（esp-sr AFE）——同硬件 40dB 增益验证过，
  比任何手工门限可靠；之前崩溃（dl_nn_args_t.c:137 断言）是 NuttX 移植问题，
  攻关路径已留档（§三 3 条方案）

## 四、当前状态与下一步
- ✅ 通信链路全通（ES7210 2 麦 → 官方 I2S → 滤波 → ES8311 → 喇叭）
- ✅ 播放/录音/回放实测通过（用户："有环境声和人声了"）
- ⏳ 底噪优化：路径 A 数据驱动门限（先补 RMS/P90 统计）/ 路径 B NSNet2 恢复
- 📌 待办：NSNet2 在 NuttX 的崩溃攻关（dl_nn_args_t.c assert）

### 2026-08-25（路径A）底噪量化 + 软噪声门（有数据依据）
- **实测（安静）**：RMS=284 | P50=59 P90=173 P99=1447 P999=2398；语音峰值 7065~14053
- **门限设计**：软门 500（=P90×3 余量）——|s|<500 衰减 1/4、500-1000 渐变、>1000 全通
  - 覆盖 90%+ 底噪（P90=173）；语音（7000+）完全不受影响；软门不归零防"听不到"
- **附加修复**：RX 栈 UAF——rctx 改 static（官方驱动超时后延迟回调写已释放栈 → 内存损坏 → RX error: 0 + heap -4KB/次泄漏）；修复后 RX 恢复（RX# r=2000 耗时 5 ticks）

### 2026-08-25（定案3）PSRAM 映射破坏 I2S —— NS 版回滚，攻关方向更新
- **决定性实验**：`plant voice mictest 2`（含 PSRAM 提前映射、不含 NS create）→ RX 仍 3s 超时（r=-110）→ **PSRAM 映射本身破坏 I2S DMA**（与 NS create/推理无关）
- **证据**：无 NS 版（无 PSRAM 操作）RX 正常（r=2000/5ticks）；NS 版（PSRAM 映射 0x3d000000 8MB）RX 全超时，cache_suspend/resume 保护无效
- **NuttX 标准 PSRAM 的缺陷**：`psram_get_available_size()` 返回 0（物理检测失败）→ `esp_spiram_init_cache` 映射 0 空间（allocable vaddr 0x3e000000-0x3e000000）→ BOOT_INIT 实际未映射 PSRAM → 只能手写映射（破坏 I2S）
- **回滚**：PLANT_NO_NS=1（无 NS）——37.5dB + 软门版可用（安静 RMS=63/P99=124，说话 RMS=646）
- **NS 攻关新方向（未验证）**：
  1. 修 NuttX `psram_enable`/`psram_get_available_size`（让 BOOT_INIT 启动早期标准映射成功 → 可能不破坏 I2S，因为启动时 cache 未完全激活/流程完整）
  2. PSRAM 映射避开 I2S DMA 影响的地址区（0x3d000000 换成 IRAM0 cache 区 0x40000000 段？）
  3. 模型不用 PSRAM：内部 RAM 装不下 337KB → 模型放 flash XIP（dl_lib 直读，需改 create 分配路径）
- **当前交付**：无 NS 可用固件（nuttx.bin PLANT_NO_NS）+ 全部修复（官方驱动/ES7210 2麦/增益37.5dB/软门/RX 栈 UAF/直方图统计）

### 2026-09-02（tone"吱吱吱嘟嘟"根因 + 修复：TX 窗口化流水线）
- **现象**：`plant voice tone 1000` 听到的是"吱吱吱持续性的嘟嘟"，不是纯音
- **关键排除**：堆健康（free=22KB）、寄存器与 08-25 定案一致（TX_CLKM=0x3400001a
  N=26→MCLK 6.154M、TX_START=1、STOP_EN=0、WS=24k）、ES8311 链路正常
  （R09=0c R32=e6 音量 70%）
- **根因分析**：官方驱动单次 i2s_send ≤ 2044B；旧 hal_i2s_write 逐块
  "排队→等 EOF→HPWORK worker 回调→再排队"——每块之间 DMA 必然空转
  （worker 往返 > FIFO 余量），1kHz 纯音被切成 ~21ms 段、段间掉拍 →
  "吱吱吱嘟嘟"。08-25 验证干净的 tone 是一次性大 DMA 链（无块间隙）；
  09-02 分块 + apb 压 2044B 后音质从未验证过（此前只验证了语音回放
  "有点噪声"——语音掩盖了块洞，纯音暴露无遗）
- **修复**（hal_i2s.c 2026-09-02）：
  ① hal_i2s_write 改**窗口化流水线**：排队 3 块（驱动 container 池=4，
  留 1 给 RX）再等完成 → EOF 后驱动 ISR 直接从 pend 续链，无 worker
  往返空窗；窗口满才阻塞（驱动池满时 i2s_send 自身也阻塞，天然流控）
  ② 新 API hal_i2s_write_async/flush：驱动排队时已 memcpy 拷走数据，
  async 返回即可复用 buf
  ③ ai_voice_play_tone 改**异步排队 + 末尾 flush**：生成下一块时上一块
  仍在播 → 50ms 块边界也无空窗
- **验证命令**：`plant voice tone 1000` → 应听到干净 1kHz 蜂鸣 + 耗时
  ≈50 ticks（500ms@24k）；dump 新增 INT_RAW（TX_HUNG=1=播放期间 DMA
  欠载过，连续播放应 0）
- **遗留**：播放语音（play_file/ai_voice_play）仍逐块阻塞调用（内部已
  窗口化，块边界 ~50ms 一个）——语音"有点噪声"如仍在，下一步把播放
  循环也改 async 流水线

### 2026-09-02（SD"自动不行手动可以"根因 + 修复：预热后台重试）
- **现象**：`plant mem` errno=19(ENODEV) → `plant sd status` errno=22
  (EINVAL 能读卡但非 FAT) → `plant sd mount` 成功。同一 sd_card_mount()
  代码路径，"越试越能通"
- **根因**：非内存（堆 22KB 健康）、非卡坏、非"自动 vs 手动"路径差异——
  是**卡/控制器上电后渐进预热**：三次命令间隔只有用户敲键盘的时间（数秒
  ~数十秒），中间无代码在跑 → mmcsd 首次探测失败后需时间恢复。旧 boot
  自动挂载只重试 3×100ms=0.3s 就放弃 → 开机没挂上就一直没挂上
- **修复**（sd_card.c 2026-09-02）：
  ① sd_card_mount 重试窗口 3×100ms → 5×300ms=1.5s
  ② sd_card_init 失败后起**后台预热线程**（6KB 栈，detached）：每 2s
  重试挂载，最长 ~30s（SD_WARMUP_RETRIES=15），卡热了自动挂上并打印
  "第 N 次挂载成功"
- **注意**：本板无卡检测引脚（CD 硬接 CONST_ZERO）；卡必须在开机前插好，
  SD_CMD=IO0 与 BOOT 键共用（运行中按 BOOT 会拉低 CMD → 通信错误）

### 2026-09-02（UI 开机首帧崩溃真因：SD 字库违反 LVGL v9 字体契约）
- **现象**：SD 开机挂载（预热修复后）→ 首页首帧渲染即 PANIC：
  `xptcode=28 PC=lv_draw_sw_blend_color_to_rgb565+0x? CAUSE=0x1c VADDR=a8c00700`
  （mask 指针垃圾，LoadProhibited）
- **真因**：`ui/assets/fonts/zh_font.c` 的 `zh_sd_get_glyph_bitmap(g_dsc, dbuf)`
  **忽略 dbuf 直接返回裸缓存指针**。LVGL v9 契约（对照 `lv_font_get_bitmap_fmt_txt`）：
  字体必须把字形解码进调用方 `dbuf->data`（A8、stride 行距）并**返回 dbuf**；
  letter 层随后把返回值强转 `lv_draw_buf_t*` 读 `.data` 当 mask → 裸指针的前几字节
  （字形像素）被当指针 → 0xa8c00700 垃圾 → blend 崩
- **为何此前误诊"池必须 80KB"**：64KB 池崩溃时 SD 恰好挂载（SD 字库启用）；80KB
  "修复"时 SD 未挂（flash 子集字体契约正确）→ 不崩。池大小纯属巧合
- **修复**：zh_sd_get_glyph_bitmap 改为按 generate_font_bin.py 的 A4 打包
  （每行 (w+1)/2 字节、高 nibble 在前）逐行解 A8（nibble<<4|nibble）写入 dbuf，
  返回 dbuf。字形 LRU 缓存（15KB）保留（缓存原始 A4，免每次 fseek）
- **验证**：`plant voice tone 1000` + UI 首页/各屏中文正常渲染

### 2026-09-02（RX 双深预读链——"录音一卡一卡/录制长播放短"根因修复）
- **现象**：tone 干净（TX async 已修）；loop 人声"一卡一卡"；diag 3 录音墙钟
  ≈5900ms（标称 3000ms，凑满 48000 采样耗时 ≈2×）
- **根因**：硬件 RX FIFO 仅 64B≈0.67ms；预读链**单深** = 收满 1 个 apb
  （2044B≈21ms）才由 worker 提交下一个 → apb 间 DMA 空窗（worker 往返
  >0.67ms）→ 每 ~21ms 丢一小段 → 数据周期断裂（回放卡）+ 有效吞吐 ~50%
  （凑满 3s 数据花 5.9s 墙钟 → "录制长、播放短"）
- **修复**（hal_i2s.c）：预读链深度 1→2（HAL_RX_PREFETCH_DEPTH=2，A/B 乒乓）：
  - submit 改临界区"窗口检查+预订"（≤2，read_slot 与 worker 回调并发安全）
  - 完成回调递减计数后补一个 → pend 恒有 ≥1 → apb EOF 中断（ISR 微秒级）
    直接装载下一个 → 无 worker 空窗
  - 新增 hal_i2s_rx_prefetch_start() 补满深度（幂等）
- **无缝性保证**（三层，非临界竞态）：①硬件 GDMA 单通道串行 + 驱动 rx.act
  非空保护 → 两 apb 不重叠收数；②EOF ISR 自动续链 → 无空隙（微秒 vs 0.67ms
  底线）；③worker 补交余量 21ms（B 收数时长）vs 亚毫秒 → 30×
- **空间增量**：仅 +1 个在途 apb ≈ +4KB 瞬时堆（段长 2044B、ring FIFO 4KB
  均不变）；container 池 4：录音 RX2+TX0、播放 TX3+RX0，均 ≤4
- **验证**：diag 3 录音墙钟应回 ~3s（不再 5900ms）；loop 人声连贯不卡
