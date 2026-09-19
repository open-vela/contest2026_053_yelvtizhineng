# 摄像头链路交接文档（2026-08-28 修订）

> **状态：管脚/字节序/预热已修（白墙能白、轮廓可见）；剩"曝光/白平衡振荡"——
> 已按 calc_sysclk 修正 PCLK 时序（pclk_div 8→10 @20MHz XCLK）+ 预热 40 帧，待板级验证**
> 本文档总结所有改动的文件、代码逻辑、数据链路，供下一位开发者快速接手。
> 详细调试历程见同目录 `CAMERA_DEBUG_LOG.md`（2.1 ~ 2.39 节）。

---

## 1. 一句话现状

OV3660 传感器配置**已完全生效**（verify 全 OK：RGB565 160×120、寄存器写入正确），
DMA 能收到 38400 字节帧（`frame done: 38400 bytes`），VSYNC 中断工作正常。

**花屏定位为三个叠加问题，前两个已修复，第三个（本次）已修复待验证：**

1. **帧对齐（已修复，2026-08-27）**：旧 capture 任意相位 re-arm DMA 链 → 窗口错位。
   已改为 GDMA EOF ISR 每帧边界 re-arm（官方 esp_cam_ctlr_dvp 模型）。
2. **字节序（已修复，2026-08-27）**：OV3660 0x4300=0x61 先发高字节，LVGL 要小端。
   已加软件交换（`plant cam swap on|off`，默认 ON）。
3. **"暗+绿"（2026-08-28 修复）**：**官方默认寄存器序列没抄全**——
   - 缺结尾 `{0x5001, 0x83}`（AWB/色彩矩阵/SDE 开启）→ AWB 从未生效 → 白墙偏绿
   - 缺 gamma 表 13 个点（0x5800-0x583d 部分缺失，默认 0x00）→ 高光被压黑 → 发暗
   - 默认序列尾部误加 JPEG 专用 `{0x3002} {0x3006} {0x471c,0x50}`（应为 0x471c=0xd0）
   → 传感器停留在 JPEG 相关状态
   已整体替换为官方 215 条默认序列（逐条 diff 核对）。
4. **帧不稳定（2026-08-28 修复，待验证）**：每次 `plant cam capture/reg` 都会
   `camera_init()` → probe → **重初始化 LEDC XMCLK** → 传感器 AEC/AWB 被反复
   打断，帧在"暗↔亮、绿↔红↔蓝"间振荡（实测抓到过 `ff ff` 白帧，证明配置已对，
   只是被下次命令打断）。已改：XMCLK 只配一次（board 层幂等）+ probe 只做一次
   （apps 层幂等）+ 首配稳定延时 300ms→1s。

---

## 2. 涉及文件清单（按数据流顺序）

| 文件 | 作用 | 关键内容 |
|---|---|---|
| `nuttx/boards/xtensa/esp32s3/esp32s3-box/src/esp32s3_board_camera.c` | board 层：XMCLK + SCCB | LEDC PWM 20MHz 输出到 **IO39**；SCCB 走 I2C0（SDA=IO8, SCL=IO18）|
| `nuttx/arch/xtensa/src/esp32s3/esp32s3_cam_dvp.c` | **DMA 收帧驱动（核心）** | LCD_CAM CAM 输入模式 + GDMA RX，见 §3/§4 |
| `nuttx/arch/xtensa/src/esp32s3/esp32s3_cam_dvp.h` | 驱动接口 | init/start/stop/capture/diag/get_frame/set_swap/get_swap |
| `apps/plant-companion/components/camera_capture/camera_capture.c` | 传感器寄存器配置 + 帧采集中间层 | 官方 ov3660 序列 + RGB565 配置 + `camera_capture_frame()` |
| `apps/plant-companion/main/app_main.c` | NSH `plant cam` 命令 | probe/cfg/rgb565/reg/capture/swap/diag |
| `apps/plant-companion/ui/screens/screen_camera.c` | UI 拍照页 | LVGL 显示（零拷贝引用 DMA rxbuf）+ 缩放适配取景框 |
| `vendor/espressif/boards/esp32s3/esp32s3-box/configs/openvela/defconfig` | 配置 | `CONFIG_LV_MEM_SIZE_KILOBYTES=96`、`CONFIG_ESP32S3_CAM_DVP=y` |

---

## 3. 数据链路（逻辑流）

```
OV3660 传感器
  │  SCCB(I2C0: IO8/IO18) 配置寄存器 ← camera_config_rgb565_qvga()
  │  XMCLK(LEDC: IO39) 20MHz 时钟
  │  DVP 8bit 输出（D0-D7=IO9/10/11/12/13/21/38/40, PCLK=14, HREF=41, VSYNC=42）
  ▼
LCD_CAM 外设 CAM 输入模式（esp32s3_cam_dvp.c hw_init）
  │  cam_ctrl: clk_sel=3(PLL160M), div=8(→20M), vsync_filter_thres=4,
  │            **vs_eof_en=1**（VSYNC 触发 EOF = 帧完成，官方 dvp_spi_lcd 模式）
  │  cam_ctrl1: bytelen=3839, vsync_filter_en=1, 8bit, 不 swap
  ▼
GDMA RX channel（esp32s3_dma_request, burst=true）
  │  线性链 10 desc × 3840B = 38400B（一行 320B，12 行/desc，行对齐）
  │  bytelen EOF 数据驱动 + cam_vsync 中断标记帧边界
  ▼
g_cam_rxbuf[38400]（静态，DRAM）
  │  ISR: GDMA in_suc_eof → block_done++ **+ 每帧边界 re-arm DMA 链**（核心修复）
  │       LCD_CAM cam_vsync → vsync_count++ / frame_pending=1
  ▼
esp32s3_cam_dvp_capture()：对齐 → 掩蔽 EOF IRQ → 等下一 EOF → 冻结 DMA
  → 诊断打印（head + 3 行像素，raw/swap 双解码）→ 软件字节交换 → sink(整帧)
  ▼
camera_capture_frame()：sink 拷入调用方 buf（cmd 用 rxbuf 或 UI 用 rxbuf）
  ▼
LCD 显示：LVGL lv_image 零拷贝引用 rxbuf（RGB565 160×120，缩放适配取景框）
```

---

## 4. 当前 esp32s3_cam_dvp.c 的关键实现（最终状态）

### 4.1 引脚（CHQ ESP32-S3-BOX V2.0 —— 2026-08-28 已按原理图 J4 + IDF 可用版修正！）
```
D0-D7(Y2-Y9) = IO12,10,9,11,13,21,38,40   ← 必须按此顺序接 CAM_DATA_IN0-7！
PCLK=14, HREF=41, VSYNC=42, XMCLK=39（board 层 LEDC）
SCCB: SDA=IO8, SCL=IO18（I2C0）
```
> ✅ **原理图（J4 FPC24 连接器 + R104-R115 串阻）逐脚确认**：
> IO12→DVP_Y2、IO10→Y3、IO9→Y4、IO11→Y5、IO13→Y6、IO21→Y7、
> IO38→Y8、IO40→Y9、IO14→PCLK、IO41→HREF、IO42→VSYNC、IO39→XMCLK。
> Y2=sensor D2=位0 … Y9=sensor D9=位7，必须按位序接 CAM_DATA_IN0-7。
> CAM_RESET 经 R53(10K) 上拉 2V8、CAM_PWDN 经 R101(10K) 接地 —— 均不可软件控制。
> ⚠️ **旧文档"IO9,10,11,12,13,21,38,40"是错的**（Y2/Y4/Y5 接错位 → 每字节位乱序
> → 颜色全是垃圾，纯白/纯黑帧例外）。权威依据 = 原理图 J4 + IDF 可用版
> `esp32s3/plant-companion/components/camera_capture/camera_capture.c`。

### 4.2 关键宏
```c
#define CAM_FRAME_W/H  160/120          // RGB565
#define CAM_RX_CHUNK   3840             // 行对齐 node（12 行，320B/行）
#define CAM_RX_DESC_NUM 10              // 38400B = 一帧
#define CAM_RX_BUF_SIZE (3840*10)       // 38400
```

### 4.3 hw_init（CAM 外设）
- `vs_eof_en=1`：**VSYNC 触发 EOF = 帧完成**（官方 dvp_spi_lcd 模式，勿改回 0）
- `cam_byte_order=0`：官方明确 8 位数据**不支持**硬件 byte swap
- bytelen=3839（兼容保留；vs_eof=1 时实际由 VSYNC 定 EOF）

### 4.4 GDMA EOF ISR（本次核心修复：每帧边界 re-arm）
```c
ISR(in_suc_eof):
  block_done++;
  DMA stop(INLINK_STOP) → in_rst 脉冲 → 重载 link 地址 → start(INLINK_START)
  // 纯寄存器写（inline，IRAM 安全），不碰 LCD_CAM：
  // cam_reset 会清 LC_DMA_INT_ENA（见 2.29），afifo_reset 会丢 FIFO 连续性
```
- 效果：DMA 每个窗口 = [上一帧边界 → 本帧边界] = **恰好一帧，帧头对齐 rxbuf[0]**
- 这是官方 `esp_cam_ctlr_dvp_start_trans()`（每帧 stop→reset→start）的 NuttX 移植

### 4.5 capture 逻辑（最新，对齐 + 掩蔽 + 取帧）
```c
1. 对齐：等一个 EOF（ISR 活跃，re-arm 在帧边界发生）→ 保证掩蔽时 DMA 正在采完整帧
2. 掩蔽：critical section 内清 DMA_IN_SUC_EOF_CH0_INT_ENA + 清 pending raw/st
         → ISR 不再 re-arm，帧不会被覆盖
3. 等待：轮询 DMA_IN_INT_RAW 的 suc_eof 置位（掩蔽后第一个帧边界）
         → 此时 DMA 停在帧边界，rxbuf = 完整一帧
4. 冻结：esp32s3_dma_disable（保险）
5. 诊断 + 交换：打印 head + row0/1/60 像素（raw/swap 双解码）→
   g_cam_byte_swap 开则软件字节交换 → sink(rxbuf, 38400)
6. 恢复：cam_dvp_arm() + cam_dvp_enable_ints()（cam_reset 会清 INT_ENA，必须重开）
```

### 4.6 字节序：OV3660 RGB565 输出**大端（高字节在前）** —— `plant cam swap` 默认 ON
```c
// 🔴 2026-08-29 彩条实锤（`plant cam bar`，0x503D=0x80 标准 8 色条）：
// 标准色条 [白黄青绿品红红蓝黑] 仅 SWP（字节交换）解码吻合 → 高字节先发 = 大端。
// 2.38"小端"结论错误：白墙 0xFFFF / 捂黑 0x0000 字节交换对称，测不出字节序；
// 2.21 datasheet "高字节先发"从一开始就是对的。R/B 分量序正常（无需额外换位）。
g_cam_byte_swap = true;    // 默认交换（esp32s3_cam_dvp_set_swap() 可切换）
```

### 4.7 中断
- GDMA `ESP32S3_IRQ_DMA_IN_CH0+chan`：in_suc_eof → block_done++ + **re-arm 链**
- LCD_CAM `ESP32S3_IRQ_LCD_CAM`：cam_vsync → vsync_count++ + frame_pending=1
  （cam_dvp_arm 的 cam_reset 会清 INT_ENA，capture/start 后经 cam_dvp_enable_ints
  重新使能两个中断）

---

## 5. 已确认"正常"的部分（不要重复排查）

1. **传感器寄存器写入生效**：`plant cam reg` verify 9 项全 OK
   （X_OUT=160, Y_OUT=120, 0x501f=0x01 RGB, 0x4300=0x61 RGB565, binning=0x01, PLL=0x08）
2. **SD 卡**：`plant sd status` 挂载 /mnt/sd 正常
3. **plant/触摸屏/UI**：heap 121KB 时正常（rxbuf 38400 静态，勿再贪大）
4. **VSYNC 中断**：`vsync=24→234` 正常增长
5. **EOF 中断**：block_done 正常增长
6. **DMA 帧接收**：`frame done: 38400 bytes` 能收到整帧

---

## 6. 花屏根因与本次修复（重点）

### 6.1 旧逻辑为什么必然花屏
```
旧：start/capture 末尾 在"任意相位" re-arm 线性链
    DMA 窗口 = [arm → 下一个 VSYNC-EOF]
    → 窗口起点取决于 arm 时刻（随机），终点固定（帧边界）
    → 捕获内容 = 跨两个传感器帧的碎片窗口，每次 capture 相位都不同
    → 显示 = 每次不同的"彩色混乱/横带"；白墙也因窗口错位不是白色
    且"等 2 个 EOF 丢弃首帧"无效：线性链在 EOF/链尾停止后，第 2 个 EOF
    不会带来新数据（缓冲内容在首 EOF 后不再变化）。
```

### 6.2 修复后为什么对齐
```
新：GDMA EOF ISR 在每个帧边界 re-arm（DMA-only，µs 级延迟）
    DMA 窗口 = [边界k → 边界k+1] = 恰好一帧（帧头 = rxbuf[0]）
    capture 掩蔽 IRQ 后取帧：帧静止、完整、对齐
    首帧（start/arm 后）是部分窗口，其 EOF 处 ISR re-arm 后自愈
```

### 6.3 字节序修复依据
- 手册 Table 7-15（用户实查）：0x4300=0x61 → Byte0（先发）={r[4:0],g[5:3]} 高字节，
  Byte1（后发）={g[2:0],b[4:0]} 低字节
- LVGL v9 RGB565 = 标准小端：低字节 {g[2:0],b[4:0]} 在前
- 因此 DMA 缓冲（先高后低）需软件交换；`plant cam swap off` 可对照验证
- 注：白墙（0xFFFF 对称）无法区分字节序，要用**彩色物体**验证
  （绿叶：swap 开显示绿，swap 关显示紫红/蓝）

---

## 7. 官方参考（必看，别再走弯路）

| 参考 | 路径 | 关键 |
|---|---|---|
| **IDF dvp_spi_lcd 例程** | `esp32s3/esp-idf/examples/peripherals/camera/dvp_spi_lcd/main/dvp_spi_lcd_main.c` | **摄像头→LCD 完整逻辑**：vs_eof=1、8位不硬件 swap、帧完成→直接 draw_bitmap |
| 新驱动 esp_cam_ctlr_dvp | `esp32s3/esp-idf/components/esp_driver_cam/dvp/src/esp_cam_ctlr_dvp_cam.c` | **start_trans 每帧重新 load 链**（ISR 里 stop→reset→start）→ 本次 ISR re-arm 的直接模板 |
| cam_hal_init | `esp32s3/esp-idf/components/hal/cam_hal.c:69` | **`cam_ll_enable_vsync_generate_eof(hw,1)`**（vs_eof=1 权威）|
| esp32-camera 旧库（**勿再参考 vs_eof=0**）| `esp32s3/plant-companion/managed_components/espressif__esp32-camera/` | 用 vs_eof=0，与官方摄像头→LCD 不符 |
| 用户自建 IDF 版（**验证可用的**）| `esp32s3/plant-companion/components/camera_capture/camera_capture.c` | 用 esp_camera_init（JPEG 1600×1200 存 PSRAM），引脚同 CHD |

**IDF 版"清晰"的真因**：FRAMESIZE_UXGA=1600×1200（分辨率），不是 JPEG 格式。
OpenVela 无 JPEG 解码器 + PSRAM 坏 → 只能 RGB565 160×120 直显。

---

## 8. 验证命令速查（板级验证顺序）

```bash
plant sd status          # 挂载 SD（/mnt/sd）
plant cam probe          # 探测 PID=0x3660
plant cam rgb565         # 配置 RGB565 160×120（verify 含 0x5001/0x5800/0x583d/0x471c）
plant cam reg            # 读关键寄存器（尺寸/格式/曝光/增益/AWB/PLL）
plant cam swap           # 显示当前字节交换开关
plant cam capture        # 拍一帧 → 诊断打印 + 显示 + 落盘 /mnt/sd/photo.rgb
plant cam swap off       # 若颜色 R/B 反了，关掉交换再 capture 对照
plant cam diag           # 分层硬件诊断（含 L0b 帧率测量）
```

**capture 诊断输出怎么看**：
```
[Cam-DVP] frame head: xx xx xx xx ...          # 原始 16 字节
[Cam-DVP] row 0 @0: [le|swp r.. g.. b.. | r.. g.. b..] ...   # 3 行 × 4 像素
```
- 白墙：两种解码都应接近 r31 g63 b31（白）；若仍暗/绿 → 看 `plant cam reg`
  的 AEC(0x3500-3503)/增益(0x350a/b)/AWB(0x5001/0x3400-3404) 状态
- 绿叶：**swp（第二个三元组）应显示绿色**（g 大 r/b 小）；若 le 是绿的而 swp 不是
  → 说明不需要交换，`plant cam swap off`
- head 全 00 → DMA 没数据（查 PCLK/数据线）；有结构但乱 → 帧对齐/时序问题

**`plant cam reg` 关键判读**：
- 0x5001：应 = 0xa3（AWB 0x01 + scale 0x20 + 色彩矩阵/SDE 0x82）——AWB 必须开，
  否则白墙偏绿
- 0x5800=0x0c / 0x583d=0xcf：gamma 表必须写全，否则高光被压黑（发暗）
- 0x471c：应 = 0xd0（非 JPEG 状态；0x50 是 JPEG 模式）
- 0x3503：bit0/bit1 = 0 表示 AEC/AGC 自动；0x3500-3502 曝光值、0x350a/b 增益
  用于判断"暗"是曝光问题还是场景问题

---

## 9. 下一步（板级验证后按需）

1. **烧录后先跑 `plant cam rgb565`**：看 verify 是否 9+4 项全 OK
   （新增 0x5001=0xa3、0x5800、0x583d、0x471c=0xd0）
2. **拍白墙**：应显示白色（验证 AWB/gamma 修复）
3. **拍绿叶/彩色物体**：验证字节序（swap 默认开，看 swp 解码是否绿色）
4. 若仍暗：`plant cam reg` 看 AEC/AGC 是否自动（0x3503）、曝光/增益值；
   `plant cam diag` 看 L0b 帧率（帧率异常 → PLL/时序问题）
5. 若仍偏色：`plant cam reg` 看 AWB 增益（0x3400-3404）与 0x5001 状态
6. 帧率/预览（可选）：ISR re-arm 已就绪，可加后台线程连续 capture 做实时预览
7. AI 识别：RGB565 帧暂不能直接喂 MiMo，需服务端转 JPEG 或后续加编码器

---

## 10. 大坑记录（血泪教训）

1. **vs_eof=0 + bytelen 是 esp32-camera 旧库模式，官方摄像头→LCD 用 vs_eof=1**
   ——之前用了 vs_eof=0 导致帧错位/断带
2. **8 位数据不支持硬件 byte swap**（官方 `byte swap is not supported when
   cam_data_width is 8`）；字节序问题只能软件交换（`plant cam swap`）
3. **cam_dvp_arm 的 cam_reset 会清 LCD_CAM INT_ENA** ——arm/reset 后必须重使能
   中断（cam_dvp_enable_ints）
4. **rxbuf 不能贪大**（65536 饿死 heap 导致 plant 命令失效），用 38400（一帧）
5. **单核轮询 + ISR 计数**：用"两次读取比较"检测事件会因中断时序永远错过，
   用 ISR 置标志（frame_pending）最可靠
6. **LVGL 池 96KB + 页面用完销毁**：解决 calc_cols NULL panic（ui_app.c 已改
   lv_obj_delete 旧页）
7. **不要用 periph_module_enable 使能 LCD_CAM 时钟**（曾卡死），直接写 SYSTEM 寄存器
8. **PSRAM 是坏的**（NuttX 侧 psram_get_available_size=0），别指望它放帧缓冲
9. **（本次新增）DMA 链必须每帧边界 re-arm**（ISR 里，µs 级延迟）：
   主循环里"等 EOF 再 arm"有 2ms 轮询延迟 = 帧的 4%~40%，必丢帧头；
   任意相位 arm = 窗口错位 = 花色。这是花屏主因。
10. **（本次新增）掩蔽 EOF IRQ 后再取帧**：否则 ISR 的 re-arm 会在你读取时
    用下一帧覆盖 rxbuf → 撕裂帧。掩蔽 → 等 raw EOF → 冻结 → 读取 → 恢复。
- **预览架构（2.43 已回退）**：直写 LCD 预览实测全黑 → 回退 lv_image 预览
  （LVGL 定时器喂帧 + invalidate）。capture 末尾不 re-arm 保留。
  直写全黑原因待查（见 CAMERA_DEBUG_LOG.md 2.43）。

---

## 给新会话/新开发者的一句话交接（2026-08-28 深夜版）

**目标**：OV3660 动态视频预览（手机相机式取景）在 ST7796 480×320 LCD 上平滑、清晰、不花屏。

**已确认为真的事实（别再重复验证）**：
- **OV3660 输出 RGB565 160×120 大端（高字节先发），swap 必须 ON**（2.49 彩条实锤：
  `plant cam bar` 标准 8 色条仅 SWP 解码吻合；2.38"小端"是白墙/捂黑对称性误判）。
  0x4300=0x61 的 R/B 分量序正常（红=红、蓝=蓝），无需额外换位。
  传感器配置已生效（verify 14 项全 OK，官方 215 条默认序列，含 AWB 0x5001、gamma 表、pclk_div 10）。
- st7789 非 3WIRE 路径已加字节交换（面板大端）；**直接写 LCD（LCDDEVIO_PUTAREA 逐行）颜色正确**
  （`plant img test` 四象限、单拍白墙白色轮廓可见）——**这是当前唯一"画面正确"的显示路径**。
  **2026-08-28 深夜已修**：`lcd_put_rgb565` 每行分块 ≤320 像素再 PUTAREA（320+160 / 320+94），
  **彻底消除直写路径的 rowbuff_be(640B) 越界**（map 实证越界区 = shutdown_handlers +
  g_cam_quiet + g_cam_byte_swap + g_cam_rxbuf[0..300]；见 CAMERA_DEBUG_LOG 2.47）。
  ⚠️ LVGL 自身全宽多行 flush 仍会越界（约束下未动 st7789.c）——UI 运行期 swap/quiet
  仍可能被破坏，正式预览方案必须让 LVGL 不刷取景框区域。
- capture 末尾不 re-arm（DMA 停在帧边界，rxbuf 稳定）；ioctl 层有互斥锁；
  GDMA ISR 每帧边界 re-arm 是对齐正确的前提。
- 本 LVGL 版本 **lv_image 渲染 RGB565 缩放有 bug**（实测：缩放上半错乱、对象删除后区域不重绘）
  → 摄像头帧不要走 lv_image。

**视频预览失败的三种现象（按出现顺序）**：
1. lv_image 预览 → 随机彩色长条从上往下刷（LVGL partial-render 条带 + lv_image 渲染 bug + 帧变化）
2. 直写 LCD 预览（后台线程 capture→lcd_put_rgb565）→ 全黑（子线程 capture/写屏链路断，原因未定位）
3. 期间 swap 标志被 st7789 rowbuff_be(640B) 越界随机改写（g_cam_byte_swap 0x3fcb9931 相邻被覆盖，
   日志铁证：set_swap(false) 后 capture 打印 swap=on）——LVGL 全宽 flush 走 putarea
   `stride==row_size` 分支 → wrram(rows*row_size,0,1) 一次性整区交换 → 越界横扫 .bss

**硬约束**：LCD 驱动（st7789.c）与 defconfig 的 XRES/YRES **保持原样不动**（用户要求，曾改坏过）；
编译/烧录由用户执行，调试依赖串口日志与 `plant cam capture/preview/img test/swap/diag` 命令。

**建议下一步（新会话的首选实验）**：用串口命令定位直写预览全黑断点 ——
`plant cam preview 20`（NSH 里直写，非 UI 线程）是否出画面？
- **注意首帧延迟 ~3s**（配置 1s AEC 收敛 + capture 首次 40 帧预热），期间黑屏是正常等待；
  命令已增强：每 5 帧打印帧头、每帧打印耗时 ms、LCD 写失败即报错退出。
- 若 NSH 直写出画面 → 问题在 UI 线程/预览线程上下文（线程栈、优先级、与 LVGL 并发、首帧时序）
- 若 NSH 直写也黑 → 问题在 capture 或写屏链路本身（capture 是否返回错误、quiet 日志、EOF 超时）
- **先跑一次 `plant img test` 确认 LCD 直写链路本身 OK**（分块修复后应仍四象限正确），
  再跑 `plant cam capture` 看 frame head/像素统计判断 rxbuf 数据是否正常。
同时可关注 `plant cam capture` 的诊断输出（frame head/像素统计）判断 rxbuf 数据是否正常。
**视频要平滑的关键**：一帧 capture(\~30ms)+缩放+写屏(\~10ms) ≈ 25 FPS 上限；
避免 LVGL 与预览线程同时写 LCD 取景框区域（ioctl 锁已串行化，但取景框内不要放 LVGL 对象
且 LVGL 全宽 flush 有 rowbuff_be 越界隐患——越界本身不修的话，预览区域要避开整行 480 宽 flush，
或接受在 LCD 侧修复）。
**实测写屏耗时**：10MHz SPI 下 414 宽行 ≈ 0.66ms/行 → 取景框 184 行 ≈ 120ms+ 帧（≈6FPS）。
要达 ~25FPS 需提升 CONFIG_LCD_ST7789_FREQUENCY（defconfig，非 XRES/YRES，可议）或缩小取景框。
