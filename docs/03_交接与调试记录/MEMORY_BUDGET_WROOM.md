# WROOM 内存预算与资源调度策略（工业级）

> 硬件：**ESP32-S3-WROOM（512KB SRAM，无 PSRAM）**
> 约束：LVGL UI + 相机 + WiFi + 语音 + SD + 土壤传感器同固件，静态区已吃掉
> 大部分 SRAM，系统堆只有 ~100KB。本文给出工程化的内存预算与调度纪律。

## 1. SRAM 静态预算（链接期定死，不占堆）

| 子系统 | 大小 | 性质 | 调度策略 |
|---|---|---|---|
| LVGL 池 `work_mem_int` | **80KB**（勿再减：64KB 实测 UI 渲染 PANIC）| 静态池 | UI 专用；**禁止**再降（64KB 触发 `lv_draw_sw_blend_color_to_rgb565` 内存破坏）|
| 相机 DVP RX 缓冲 `g_cam_rxbuf` | 37.5KB | 静态（**勿改 malloc**！见下）| 160×120 RGB565 = 38400B **正好一帧，最小尺寸**；命令触发使用 |
| 网络 IOB `g_iob_buffer` | **12.2KB**（124→60 个，原 25.2KB）| 静态 | 语音流量轻（WAV 分块/TTS 下载），60 个足够；再减需实测 |
| WiFi `g_wlan_priv` 等 | ~13KB + 动态 | 静态+连接期堆 | **惰性初始化**：只在 `plant wifi` 首次连接时才 `esp_wifi_init` |
| LCD 写缓冲 | 9KB | 静态 | 必需 |
| 语音静态（RX FIFO/分块/音调…）| ~14KB（tone 9.6→4.8KB）| 静态 | 语音链路**全静态化**；DMA apb ≤2044B（堆容纳上限）|
| 系统堆 | ~121KB（原 103KB）| 动态 | 见 §3 健康阈值；预计 free≈21KB ✅ |

⚠️ **相机缓冲的历史教训**：曾改为 `memalign(64, 38400)` 按需分配 → UI+相机同时期堆耗尽
→ `malloc(38400)` 失败 → **UI 冻结**。故相机缓冲保持链接期静态是设计使然；
要省这 37.5KB 只能**构建期关相机**，不要改回动态分配。

## 2. 资源调度纪律（运行期）

1. **大缓冲静态化**：实时/DMA 路径（音频、相机、LCD）的固定缓冲一律 static，
   绝不让堆承载 >4KB 的周期分配。
2. **重外设惰性初始化**：WiFi（首连才 init）、相机（命令触发）、语音（首用才 init）。
3. **SD 挂载重试**：`sd_card_mount()` 重试 3 次（100ms 间隔）——堆瞬时不足时
   errno=5/19 是假"无卡"，重试即成功（已实现）。
4. **堆健康守卫**：录音/播放入口检查 `mallinfo.fordblks`，<8KB 打印明确警告
   （已实现 `ai_voice_heap_warn`）。
5. **诊断命令**：`plant mem` 输出 heap/LVGL/静态保留/SD 四段 + 健康判据（已升级）。

## 3. 系统堆健康阈值（WROOM）

| heap free | largest | 判定 | 预期行为 |
|---|---|---|---|
| ≥ 16KB | ≥ 8KB | ✅ OK | SD 挂载、音频 apb、FAT I/O 全部安全 |
| 8~16KB | ≥ 4KB | ⚠️ 警戒 | 大 malloc（>8KB）可能失败，功能基本可用 |
| < 8KB | < 4KB | 🔴 危险 | SD/FAT/音频 apb 偶发失败（errno=5/19、tone -12 的根源）|

**已做的堆释放**：IOB 124→60（-13KB）、tone 100ms→50ms 分块（-4.8KB）、音频 apb
≤2044B（适配堆容纳上限，修 RX 0 数据）。静态总量 281.2→263.5KB，系统堆预计
`free≈21KB/largest≈19KB`（✅）。

## 4. 构建期产品模式（要省内存时按需裁剪）

| 场景 | 裁剪 | 回收 |
|---|---|---|
| 语音优先（当前调试）| `# CONFIG_PLANT_CAMERA_CAPTURE is not set` | 37.5KB 静态 |
| 不需要网络 | `# CONFIG_PLANT_WIFI_MANAGER is not set` + IOB 减半 | ~30KB+ |
| 不需要 UI | `# CONFIG_PLANT_UI is not set`（LVGL 池整个消失）| 80KB |
| 土壤传感器不需要 | `# CONFIG_PLANT_SOIL_SENSOR is not set` | 少量 |

原则：**功能按产品阶段裁剪，而不是运行时把缓冲在堆/静态之间来回倒腾**——
512KB SRAM 的工业铁律是"静态预算 + 惰性初始化 + 阈值守卫"。

## 5. 更新记录
- 2026-09-02（本轮）：LVGL 80→64KB 触发 UI 渲染 PANIC → **回退 80KB（勿再减）**；
  相机 37.5KB = 一帧 RGB565 最小尺寸（160×120×2，勿改）；IOB 124→60（-13KB）；
  tone 减半（-4.8KB）；音频 apb ≤2044B（修 apb_alloc(4092) 在 3.2KB 堆必挂导致的
  RX 0 数据）；静态总量 281.2→263.5KB。
- 2026-09-02（上轮）：tone 分块 static；SD 挂载重试 3 次；音频入口堆警告；
  `plant mem` 升级为四段健康报表；音频 apb 尺寸适配。
- 2026-09-02（纠误）：**"64KB 池 → UI 渲染 PANIC" 系误诊**——真因是 SD 全量字库
  `zh_font.c` 的 `zh_sd_get_glyph_bitmap` 违反 LVGL v9 字体契约（忽略 dbuf、返回裸
  缓存指针 → letter 层把裸指针当 `lv_draw_buf_t` 解析 → mask_buf=垃圾 → blend
  LoadProhibited）。64KB 时代崩溃 = 该次开机 SD 恰好挂载（SD 字库启用）触发；80KB
  "修复" = SD 未挂载走 flash 子集字体（契约正确）→ 不崩，与池大小无关。已修复
  zh_font（解码 A4→A8 进 dbuf 并返回 dbuf）。**池 80KB 结论保留但理由待重验**：
  待 UI 全 7 屏 + 字库验证稳定后，可再试 64KB 实验（省 16KB 静态→堆），勿在
  验证前动。
