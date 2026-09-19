# 摄像头调试工作日志（2026-08-26）

> 本文档记录 DVP 摄像头（OV3660）从"完全无数据"到"DMA 收帧 + VSYNC EOF 检测打通"的
> 全部根因、修复与当前状态。**新话题继续前先通读本文档。**

---

## 1. 结论速览

| 项目 | 状态 | 说明 |
|---|---|---|
| 摄像头传感器 | ✅ 工作 | OV3660 SCCB probe ✓，RGB565 320×240 配置生效（寄存器证实） |
| DMA 收帧 | ✅ 打通 | GDMA RX 收到数据，VSYNC EOF 检测成功 |
| 帧落盘 | ✅ 打通 | `/data/photo.rgb` 38400 字节成功写入 |
| LVGL 显示 | ⏳ 下一步 | 需要同步传感器分辨率 + LVGL image 显示 |
| AI 识别 | ⏳ 规划 | 用户倾向服务器调用（省资源） |

**当前机制已验证**：`clk_sel=3`(PLL160M) + `vs_eof=1`(VSYNC 触发 EOF) + 整帧描述符链 + 轮询 desc.ctrl EOF 位 → 帧完成检测。

---

## 2. 关键根因与修复（按发现顺序）

### 2.1 CAM 外设时钟从未使能（最先根因）
- **现象**：DMA 完全 0 数据，`block_done=0`
- **根因**：`CONFIG_ESP32S3_LCD=n`（本板 LCD 用 SPI ST7789），`esp32s3_lcd.c` 未编译，
  **`periph_module_enable(PERIPH_LCD_CAM_MODULE)` 从未被调用** → LCD_CAM 外设无时钟
- **修复**：`cam_dvp_hw_init()` 开头直接写 SYSTEM 寄存器使能时钟：
  ```c
  modifyreg32(SYSTEM_PERIP_CLK_EN1_REG, 0, SYSTEM_LCD_CAM_CLK_EN);
  modifyreg32(SYSTEM_PERIP_RST_EN1_REG, SYSTEM_LCD_CAM_RST, 0);
  ```
- ⚠️ 不要用 `periph_module_enable()`——曾导致开机卡死（原因未完全明确，用直接寄存器写避开）

### 2.2 GDMA IRQ 未映射（卡在 up_enable_irq）
- **现象**：capture 卡在 `irq enabled` 之前
- **根因**：`g_irqmap[irq]` 是 `IRQ_UNMAPPED`，`up_enable_irq` 写垃圾 CPUINT
- **修复**：先 `esp32s3_setup_irq(this_cpu(), ESP32S3_PERIPH_DMA_IN_CH0+chan, prio, flags)`

### 2.3 CAM 内部时钟源错误（clk_sel=2 应为 3）
- **IDF cam_ll.h 权威值表**：
  ```
  cam_clk_sel = 1 → XTAL (40MHz)
  cam_clk_sel = 2 → PLL240M
  cam_clk_sel = 3 → PLL160M   ← 正确（160M/8 = 20MHz，匹配 xclk 20M）
  ```
- **修复**：`clk_sel=3` + `clkm_div_num=8`

### 2.4 EOF 触发机制（bytelen vs VSYNC）
- **错误尝试**：`vs_eof_en=0` + `cam_rec_data_bytelen=4095`（每 4KB 触发 EOF）→ 只收到 8KB
- **正确**（IDF `esp_cam_ctlr_dvp` 方式）：**`vs_eof_en=1`**（VSYNC 触发 EOF 置 desc 位）+
  整帧描述符链 + 轮询 `desc.ctrl & ESP32S3_DMA_CTRL_EOF` 检测帧完成
- **修复后**：`VSYNC EOF detected (desc 0)` ✓

### 2.5 内存不足（150KB 帧缓冲装不下）
- **现象**：320×240 帧缓冲（150KB）`memalign` 失败（堆 186KB 但碎片/WiFi占用）
- **当前对策**：DMA 缓冲临时用 160×120（37KB）验证机制
- **待解决**：传感器同步 160×120 + LVGL 池权衡

### 2.6 XMCLK(IO39) 从未配置输出 —— block_done 恒 0 的真根因（本轮定案）
- **现象**：vs_eof=0+bytelen=4091 后 `block_done` 恒 0，`block timeout, no data`，`total=0`
- **根因（对照 IDF esp32-camera 官方逐行发现）**：
  1. **`cam_dvp_gpio_config()` 只配了输入引脚，`XMCLK=IO39` 从未 `gpio_matrix_out`！**
     OV3660 没有外部时钟 → 不输出任何像素（SCCB 是自定时 I2C，probe/配寄存器都不需要
     时钟 → 完美解释"传感器能配、寄存器回读正确，但 0 像素数据"）
  2. **`CAM_VSYNC_FILTER_THRES`(cam_ctrl bit[3:1]) 被写进 cam_ctrl1 的 bytelen 位**
     （cam_ctrl1 bit[3:1] 属于 REC_DATA_BYTELEN 低 4 位 → 污染 EOF 计数）
  3. cam_ctrl/cam_ctrl1 未整体清零（官方 `val=0` 后逐位设，我们 read-modify-write 残留旧位）
  4. start 缺官方完整序列（cam_start=0 → cam_reset → afifo_reset → in_rst → bytelen → link → start）
  5. **desc 大小 4095（非 burst）vs bytelen 4091 错位 3 字节**——`esp32s3_dma_request`
     第 4 参 burst 改 true 后 dma_setup 用 4092，与官方 `LCD_CAM_DMA_NODE_BUFFER_MAX_SIZE=4092` 一致
- **修复（全部对齐官方 esp32-camera ll_cam.c / IDF dvp_spi_lcd）**：
  - gpio_config 加 `esp32s3_configgpio(39, OUTPUT)` + `esp32s3_gpio_matrix_out(39, 149, false, false)`
  - hw_init：cam_ctrl/cam_ctrl1/rgb_yuv 整体写 0 后逐位设；THRES 写回 cam_ctrl；**vs_eof_en=1**
    （帧级 EOF，对标 IDF `cam_hal_init` 明确 `cam_ll_enable_vsync_generate_eof(hw, 1)`）
  - 新增 `cam_dvp_arm()` = 官方 `ll_cam_start()` 完整序列；capture = 跳过首帧 + 取下一完整帧
    （对标 isx012.c "skip first invalid frame"）+ 每帧重新 arm（对标 dvp_spi_lcd 每帧 start_trans）
- **官方权威事实（esp32-camera ll_cam.c / private_include/ll_cam.h）**：
  - `LCD_CAM_DMA_NODE_BUFFER_MAX_SIZE = 4092`，`cam_rec_data_bytelen = 4092-1 = 4091`
  - `cam_clk_sel=3`（注释"3:no clock"是错的，实际 3=PLL160M；div_num=160M/20M=8）
  - `cam_ctrl.cam_update=1` 在最后；`cam_ctrl1.cam_start=1` 在 update 之后
  - `cam_vsync_filter_thres=4` 在 **cam_ctrl**；`cam_vsync_filter_en=1` 在 **cam_ctrl1**
  - GDMA 描述符 size 12bit 最大 4095，但 4B 对齐后 LLDESC_MAX=4092

### 2.7 capture 命令传 NULL 缓冲 bug
- `cmd_cam capture` 曾传 `dummy=NULL,0` 给 `camera_capture_frame`（要求 buf>=38400）→ 必失败
- 修复：cmd 内 malloc(38400) 帧缓冲，capture 后 free

### 2.8 帧数据全 0 —— 传感器寄存器序列不完整 + 用了 JPEG 的 PLL/TIMING
- **现象**：XMCLK 修复后 DMA 收帧机制全通（block_done 每帧+1，38400 字节捕获），
  但 `frame head` 全 00 → DMA 收到的是 memset 的初始值，像素没进 CAM
  （vs_eof=1 的 EOF 由 VSYNC 触发，与数据量无关 → 0 数据也能"收帧"）
- **根因（对照官方 ov3660_settings.h 逐条 diff）**：
  1. 默认序列后半段与官方完全不同：官方 `0x5000=0xa7`（ISP 全使能），我们 `0x5000=0x06`
     （ISP 几乎全关 → 输出黑）；官方含 0x4002/0x4003/0x4005、0x538x 色彩矩阵、
     0x580x 伽马、0x548x gamma 表、0x3a0f-0x3a1f AEC 表——我们全是旧值/缺失
  2. PLL 用了 JPEG 320×240 的（CTRL2=0x10, CTRL3=0x02, PCLK_RATIO=4）：
     官方 RGB565 QQVGA 是 `set_pll(false,8,1,0,false,0,true,8)`
     → CTRL0=0x00, CTRL1=8, CTRL2=0x11, CTRL3=0x00, PCLK_RATIO=8（32M/8M）
  3. TIMING 寄存器带 JPEG 位：官方 RGB565 binning 是 0x3820=0x01, 0x3821=0x01，
     我们 0x3820=0x41, 0x3821=0x21（0x20 是 JPEG 压缩位，RGB565 不能要）
- **修复**：默认序列整体替换为官方 178 条；PLL/TIMING 对齐官方 RGB565 分支

### 2.9 XMCLK 必须用 LEDC（board 层），不能用 CAM_CLK 信号（用户提醒后确认）
- **现象**：上一版我们 `esp32s3_gpio_matrix_out(39, CAM_CLK_IDX=149)` 输出 XMCLK，
  但官方板级代码（`esp32s3-board/src/esp32s3_board_camera.c`）早就实现了正确方案：
  **LEDC PWM 20MHz** 输出到 IO39（`XMCLK_SIG_IDX=73` = LEDC_LS_SIG_OUT0_IDX）
- **关键**：`board_camera_xmclk_init()` 在 `camera_init()`(probe) 时已调用（配置 LEDC 时钟）；
  我们驱动再 `gpio_matrix_out(39, 149)` 会把 IO39 从 LEDC **抢走**，传感器时钟反而死掉
- **修复**：`esp32s3_cam_dvp.c` 的 gpio_config **完全不碰 IO39**，XMCLK 完全交给 board 层 LEDC
- **教训**：NuttX 板级已有现成方案，动手前应先查 `boards/xtensa/esp32s3/esp32s3-box/src/`
  （`esp32s3_board_camera.c` + `hal_cam_ll.h` 是官方移植的 CAM HAL，含 XMCLK/SCCB）
- **用户提示**：用户记得 NuttX 有"摄像头帧实时传 LCD"的官方逻辑——即 IDF dvp_spi_lcd
  例程架构（摄像头 DMA → 内存 → LCD），我们正在对标该架构

### 2.10 LCD 花屏（绿/紫像素 + 黑竖条）—— 有像素了但帧结构错位
- **现象**：官方寄存器序列 + LEDC XMCLK 修复后，取景框**有画面了**（传感器真正出像素），
  但显示花屏：绿色/紫色像素交替 + 黑色竖条
- **分析**：
  1. 绿色(0x07E0)↔紫色(0xE007)交替 = RGB565 **字节序错误**的典型特征
  2. 黑色竖条 = 帧数据从错误行开始（VSYNC 对齐问题）或每行字节数与显示假设不匹配
  3. 官方差异：esp32-camera `cam_init()` 设 `vsync_invert = true`，
     我们之前 `gpio_matrix_in(42, 152, 0)` **不反相** → EOF 可能触发在帧中间
- **已修复**：VSYNC 改为反相（`gpio_matrix_in(42, 152, 1)`），对齐官方
- **诊断增强**：capture 后打印 line0 像素分析（zero-px 数 + green-swap-score）
- **注意**：后续 `plant cam reg` 证明花屏**真正的根因是寄存器写入未生效**（见 2.11）

### 2.11 传感器寄存器写入未生效 —— 软复位后没延时（用户读回数据实锤）
- **现象**：`plant cam reg` 读回全部是默认值，与我们写入的完全不同：
  | 寄存器 | 写入 | 读回 |
  |---|---|---|
  | 0x3808/09 (X_OUT) | 160 (0x00A0) | **0x0800=2048**（默认 QXGA） |
  | 0x380a/0b (Y_OUT) | 120 (0x0078) | **0x0600=1536** |
  | 0x501f (格式) | 0x01 | **0x03** |
  | 0x4300 (RGB565) | 0x61 | **0xf8** |
  | 0x3820 (binning) | 0x01 | **0x40** |
  | 0x3821 | 0x01 | **0x00** |
  → 传感器一直在默认 QXGA 2048×1536 非 RGB565 状态，DMA 收到的是默认大帧的一小片
- **根因（对照官方 ov3660_settings.h 开头）**：
  官方默认序列第一条 = `{0x3008, 0x82}` 软复位，**随后 `{REG_DLY, 10}` 延时 10ms**，
  再写 `{0x3008, 0x42}`（复位第二阶段）、`{0x302c, 0xc3}`（DRIVE_CAPABILITY）、
  `{0x4740, 0x21}`（CLOCK_POL_CONTROL = PCLK 极性！）。
  我们把软复位拆到函数开头且**无延时** → 后续 178 条写入全被复位窗口吞掉
- **修复（用户要求"直接照抄官方"）**：
  - 默认序列开头照抄官方 8 条（软复位 + REG_DLY 10ms + 0x42 + 0x302c + 0x4740）
  - `ov3660_write_regs` 支持 REG_DLY 延时
  - `ov3660_set_framesize` 完整照抄官方 set_framesize + set_image_options + set_pll
    （含 binning 判断、total/offset 分支、PLL 三分支）
  - 配置后加**写后读回验证**（verify 打印，确认寄存器真的写进去）
- **教训**：官方序列的**开头几条（复位时序）和尾部（ISP/色彩）一样重要**，
  不能只抄中间；写后读回是判断"配置是否生效"的唯一可靠方法


- **现象**：官方寄存器序列 + LEDC XMCLK 修复后，取景框**有画面了**（传感器真正出像素），
  但显示花屏：绿色/紫色像素交替 + 黑色竖条
- **分析**：
  1. 绿色(0x07E0)↔紫色(0xE007)交替 = RGB565 **字节序错误**的典型特征
  2. 黑色竖条 = 帧数据从错误行开始（VSYNC 对齐问题）或每行字节数与显示假设不匹配
  3. 官方差异：esp32-camera `cam_init()` 设 `vsync_invert = true`，
     我们之前 `gpio_matrix_in(42, 152, 0)` **不反相** → EOF 可能触发在帧中间
- **已修复**：VSYNC 改为反相（`gpio_matrix_in(42, 152, 1)`），对齐官方
- **诊断增强**：capture 后打印 line0 像素分析（zero-px 数 + green-swap-score），
  一次烧录同时区分"字节序错"还是"行结构错"
- **待验证**：
  1. `plant cam reg` 确认传感器实际输出尺寸（0x3808/0x380A 应为 160/120）
  2. capture 的 line0 analysis：swap-score>0 = 字节序对（问题在行结构）；
     swap-score<0 = 需要 cam_byte_order=1；zeros 多 = 黑条来源


- **现象**：上一版我们 `esp32s3_gpio_matrix_out(39, CAM_CLK_IDX=149)` 输出 XMCLK，
  但官方板级代码（`esp32s3-board/src/esp32s3_board_camera.c`）早就实现了正确方案：
  **LEDC PWM 20MHz** 输出到 IO39（`XMCLK_SIG_IDX=73` = LEDC_LS_SIG_OUT0_IDX）
- **关键**：`board_camera_xmclk_init()` 在 `camera_init()`(probe) 时已调用（配置 LEDC 时钟）；
  我们驱动再 `gpio_matrix_out(39, 149)` 会把 IO39 从 LEDC **抢走**，传感器时钟反而死掉
- **修复**：`esp32s3_cam_dvp.c` 的 gpio_config **完全不碰 IO39**，XMCLK 完全交给 board 层 LEDC
- **教训**：NuttX 板级已有现成方案，动手前应先查 `boards/xtensa/esp32s3/esp32s3-box/src/`
  （`esp32s3_board_camera.c` + `hal_cam_ll.h` 是官方移植的 CAM HAL，含 XMCLK/SCCB）
- **用户提示**：用户记得 NuttX 有"摄像头帧实时传 LCD"的官方逻辑——即 IDF dvp_spi_lcd
  例程架构（摄像头 DMA → 内存 → LCD），我们正在对标该架构

- **现象**：XMCLK 修复后 DMA 收帧机制全通（block_done 每帧+1，38400 字节捕获），
  但 `frame head` 全 00 → **DMA 收到的是 memset 的初始值，像素根本没进 CAM**
  （vs_eof=1 的 EOF 由 VSYNC 触发，与数据量无关 → 0 数据也能"收帧"）
- **根因（对照官方 ov3660_settings.h 逐条 diff）**：
  1. **默认序列只有 188 条，官方是 178 条且后半段完全不同**：
     - 官方 `0x5000=0xa7`（ISP 全使能），我们 `0x5000=0x06`（ISP 几乎全关 → 输出黑）
     - 官方含 0x4002/0x4003/0x4005（ISP 行处理）、0x538x（色彩矩阵）、0x580x（伽马）、
       0x548x（gamma 表）、0x3a0f-0x3a1f（AEC 表）——我们全是旧值/缺失
  2. **PLL 用了 JPEG 320×240 的**（SC_PLLS_CTRL2=0x10, CTRL3=0x02, PCLK_RATIO=4）：
     官方 RGB565 QQVGA(160×120) 是 `set_pll(false, 8, 1, 0, false, 0, true, 8)`
     → CTRL0=0x00, CTRL1=8, **CTRL2=0x11**, **CTRL3=0x00**, **PCLK_RATIO=8**
     （32MHz SYSCLK / 8MHz PCLK）
  3. **TIMING 寄存器用了 JPEG 位**：官方 RGB565 binning 是 `0x3820=0x01, 0x3821=0x01`，
     我们 `0x3820=0x41, 0x3821=0x21`（0x21 的 0x20 是 JPEG 压缩位，RGB565 不能要；
     0x41 的 0x40 是 no-binning 标志与 binning 冲突）
- **修复**：默认序列整体替换为官方 178 条；PLL/TIMING 对齐官方 RGB565 分支
- **官方权威事实（ov3660_settings.h）**：
  - `ratio_table[0]`(4x3): start(0,0) end(2079,1547) total(2300,1564) offset(16,6)，
    binning 时 total_y/2+1=783, offset(8,2)
  - `set_image_options`: binning→reg20|=0x01, reg21|=0x01, 0x4514=0xaa, 0x4520=0x0b,
    X/Y_INC=0x31；JPEG 才 reg21|=0x20
  - `set_framesize` 非 JPEG 且 <QVGA: `set_pll(false,8,1,0,false,0,true,8)`

---

## 3. 当前代码状态

### 3.1 摄像头配置（camera_capture.c）
- `camera_config_jpeg_qvga()`：JPEG 320×240（旧，AI 用）
- `camera_config_rgb565_qvga()`：RGB565 320×240（新增，LCD 直显用）
  - `FORMAT_CTRL(0x501f)=0x01`（RGB）
  - `FORMAT_CTRL00(0x4300)=0x61`（RGB565 BGR）
- `camera_dump_regs()`：读 OV3660 关键寄存器（尺寸/格式验证）

### 3.2 DMA 收帧（esp32s3_cam_dvp.c）
- `CAM_FRAME_W/H` = **160×120**（37.5KB，内存预算内）
- `CAM_RX_CHUNK=4092`（GDMA 节点最大 4B 对齐值），10 desc 线性链覆盖 40920B
- **`vs_eof_en=1`**（VSYNC 帧级 EOF）+ 跳过首帧 + 每帧重新 `cam_dvp_arm()`（官方 ll_cam_start 序列）
- **XMCLK=IO39 由 board 层 LEDC 驱动**（`board_camera_xmclk_init` 20MHz，probe 时调用）；
  本驱动不碰 IO39（曾用 `gpio_matrix_out(39, CAM_CLK_IDX)` 抢走 LEDC → 传感器时钟死）
- `esp32s3_dma_request(..., burst=true)` → desc=4092 与官方 `LCD_CAM_DMA_NODE_BUFFER_MAX_SIZE` 一致
- GPIO 路由（DVP 引脚 → CAM_DATA_IN0-7 / PCLK / HREF / VSYNC；XMCLK 走 LEDC）

### 3.3 测试命令（app_main.c cmd_cam）
- `plant cam probe`：SCCB 探测
- `plant cam rgb565`：配 RGB565
- `plant cam reg`：读寄存器
- `plant cam capture`：收帧 → 写 `/data/photo.rgb`

---

## 4. 下一步（建议顺序）

1. **传感器同步 160×120**：改 `ov3660_set_framesize_qvga` 或新增 `camera_config_rgb565_qqvga()`
   （输出尺寸寄存器 0x3808/0x380A 设 160/120），使 DMA 收满整帧 37KB
2. **LVGL 显示照片**：screen_camera 加 `lv_image`，用 `lv_image_dsc_t` 引用 RGB565 帧缓冲
   （零拷贝），拍照后显示
3. **预览**（可选）：连续帧 → 更新 lv_image（需后台线程 + 帧缓冲复用）
4. **AI 服务器**：WiFi 上传 `/data/photo.rgb`（或转 JPEG）→ 服务器识别 → 返回结果
5. **内存权衡**：LVGL 池 64KB + 帧缓冲 37KB 的余量确认（堆 186KB）

---

## 5. 大坑提醒

1. **`cam_update` 位在 cam_ctrl（0x4），不在 cam_ctrl1（0x8）**——写错位置配置不生效
2. **不要用 `periph_module_enable` 使能 LCD_CAM 时钟**（曾卡死），用直接寄存器写
3. **clk_sel 值表**：1=XTAL, 2=PLL240M, 3=PLL160M（不是 2=CLK160！）
4. **XMCLK 必须配输出**（`gpio_matrix_out(39, CAM_CLK_IDX=149)`）——OV3660 没时钟就 0 像素，
   SCCB 却能正常工作，这是最隐蔽的坑
5. **寄存器位归属**：`vsync_filter_thres`/`vs_eof_en`/`update`/`clk_sel`/`div_num` 都在 **cam_ctrl**；
   `start`/`vsync_filter_en`/`bytelen`/`clk_inv` 在 **cam_ctrl1**——写错寄存器位会污染相邻字段
6. **desc 大小必须 4B 对齐 4092**，与 bytelen 匹配（burst=true）；4095 会错位 3 字节
7. **vs_eof_en=1 才是整帧模式**（VSYNC 触发 EOF），且每帧要重新 arm（线性链）
8. **改 LVGL 池大小必须清 lvgl 的 .o 重编**（`find apps/graphics/lvgl -name "*.o" -delete`）
9. **帧缓冲 150KB 在 186KB 堆可能分配失败**（碎片），用 160×120(37KB) 更稳
10. **capture 命令必须传 ≥38400B 缓冲**，传 NULL/0 会被 camera_capture_frame 拒绝（-EINVAL）

### 2.12 里程碑：verify 全 OK + 像素非零 —— 但颜色偏色/panic
- **结果**：软复位延时修复后 `plant cam capture` 输出：
  - verify **9 项全 OK**（0x3809=0xa0, 0x501f=0x01, 0x4300=0x61, 0x3820/21=0x01, 0x303b=0x08）
    → **寄存器写入生效，RGB565 160×120 binning 配置成功**
  - frame head 非零：`01 20 01 20 ... 0a 80 0a e0 ...` → **有真实像素了**
  - 像素值分析：`0a 80`=0x0a80(R=1,G=20,B=0 绿), `0a e0`=0x0ae0(绿),
    `01 20`=0x2001(R=4,G=0,B=1 偏红黑)
- **问题 1（颜色）**：LVGL 期望 RGB565 小端，传感器 0x4300=0x61 是 BGR 字节序 →
  试 cam_byte_order=1 结果全 0（见 2.14），已回退 0，颜色待软件 swap
- **问题 2（panic）**：多次 capture 后 `[PANIC-DUMP] xptcode=29 PC=400570e8
  CAUSE=0x1d (LoadTLBMiss @addr 0)` → 空指针。PC 在 ROM 无符号。疑似 heap 碎片
  或 littlefs 写满（fopen/fwrite 路径）

### 2.13 DRAM 溢出 + UI 卡死 —— 静态缓冲超限（内存重构）
- **现象**：把 rxbuf(40920) + s_cam_frame(38400) + cmd fb(38400) 全改静态后
  **DRAM 溢出 112 字节**（`section .dram0.data will not fit`）；此前 malloc(38400)
  失败 → UI 卡死（堆耗尽）
- **重构**：
  - rxbuf 静态 40920 → **38400**（= CAM_FRAME_SIZE，dma_setup 自动 10 desc：9×4092+1572）
  - **删掉 s_cam_frame**：新增 `esp32s3_cam_dvp_get_frame()` 导出 rxbuf，
    screen_camera 直接引用 DMA 缓冲做 LVGL 源（零拷贝，省 38KB）
  - cmd fb 改回 malloc（rxbuf/s_cam_frame 静态化后堆有空间）
- **教训**：静态缓冲总和不能超过 DRAM 剩余；帧数据尽量复用 DMA rxbuf

### 2.14 byte_order=1 产生全 0 帧（经验记录）
- 设置 `cam_byte_order=1` 后 capture 收到全 0 帧（160 zero-px）——理论上 byte_order
  只交换字节不应影响数据有无，但实测全 0。已回退 byte_order=0（保持有数据的基线），
  颜色问题留待软件 swap 处理
- **待验证**：byte_order=0 + 静态 rxbuf 后是否恢复非零帧（应恢复 `01 20 0a 80...`）

### 2.15 白色墙壁显示暗色偏色 —— 缺 set_ae_level(0)（曝光未初始化）
- **现象**：用户拍**白色墙壁**，但 frame head 显示：
  `0a 80`=0x800a(R16 G0 B10 紫红)、`0a e0`=0xe00a(R28 G0 B10 亮紫红)、
  `01 20`=0x2001(R4 G0 B1 暗红黑) —— **G 通道完全为 0，图像极暗**
  白色墙壁不可能 G=0 → 曝光严重不足 + 偏色
- **根因（对照官方 ov3660.c reset() 第 185-199 行）**：
  官方默认序列后必调 **`set_ae_level(sensor, 0)`**（写 6 个曝光目标寄存器
  0x3a0f/0x3a10/0x3a1b/0x3a1e/0x3a11/0x3a1f），我们漏了这一步
- **修复**：新增 `ov3660_set_ae_level0()` 照抄官方（target=55）：
  - 0x3a0f=59, 0x3a10=50, 0x3a1b=59, 0x3a1e=50, 0x3a11=118, 0x3a1f=25
  - RGB565 和 JPEG 配置都调用
- **教训**：官方 reset() 流程 = 软复位 100ms → 默认序列 → **set_ae_level** → 100ms，
  每一步都不能省；曝光目标不设画面必然暗/偏色

### 2.16 白色不白、黑边框清楚 —— 曾误判为字节序问题（实际是 vs_eof=1 帧错位）
- 现象：白墙显示暗绿棕、黑边框清楚；row60 0x5ace swap 后 ≈ 白色
- 曾误判为 RGB565 字节序反 → 加软件 swap；后经 2.18 证实是 vs_eof=1
  帧起始偏移假象，软件 swap 已删除，改 vs_eof=0 官方模式

### 2.17 UI panic（calc_cols NULL）—— LVGL 池耗尽 + 页面常驻（用户要求重构）
- **现象**：进入 Tasks 页后 `[PANIC-DUMP] xptcode=29 PC=4207d22d CAUSE=0x1d`
  → addr2line 定位 `calc_cols` lv_grid.c:330（LVGL 网格布局）→ 空指针
- **根因 1**：LVGL 池 64KB，Tasks 页创建到 81%（free=12KB），bottomnav 的
  grid 布局内存分配失败 → calc_cols 解引用 NULL
- **根因 2（架构）**：页面**懒加载但常驻**（g_screens[] 保存所有已建页面，
  切换只 HIDDEN/SHOW），LVGL 池越用越多
- **修复（用户要求"离开即销毁"）**：
  1. `LV_MEM_SIZE_KILOBYTES` 64 → **96**（defconfig 显式设置）
  2. `ui_nav_cb`：切换 Tab 时 **lv_obj_delete 销毁除 home 外旧页**，置 NULL，
     下次进入重新创建
  3. `ui_app_pop_screen`：二级页 pop 时 **lv_obj_delete 销毁**
  4. 刷新逻辑加 NULL 保护（页面销毁后 g_screens[i]=NULL 不再访问）
- **预期**：内存只占 home + 当前页，LVGL 池不再耗尽



- **结果**：软复位延时修复后 `plant cam capture` 输出：
  - verify **9 项全 OK**（0x3809=0xa0, 0x501f=0x01, 0x4300=0x61, 0x3820/21=0x01, 0x303b=0x08）
    → **寄存器写入生效，RGB565 160×120 binning 配置成功**
  - frame head 非零：`01 20 01 20 ... 0a 80 0a e0 ...` → **有真实像素了**
  - 像素值分析：`0a 80`=0x0a80(R=1,G=20,B=0 绿), `0a e0`=0x0ae0(绿),
    `01 20`=0x2001(R=4,G=0,B=1 偏红黑)
- **问题 1（颜色）**：LVGL 期望 RGB565 小端，传感器 0x4300=0x61 是 BGR 字节序 →
  已加 `cam_byte_order=1`（swap），待验证绿色是否不再显示为紫色
- **问题 2（panic）**：多次 capture 后 `[PANIC-DUMP] xptcode=29 PC=400570e8
  CAUSE=0x1d (LoadTLBMiss @addr 0)` → 空指针。PC 在 ROM 无符号。疑似 heap 碎片
  或 littlefs 写满（fopen/fwrite 路径），待查



### 2.18 为什么 IDF 版正常、OpenVela 费劲 —— vs_eof=0 才是官方模式（关键转折）
- **用户提示**：`/home/vboxuser/esp32s3/plant-companion/components/camera_capture`（IDF 版）
  是**绝对正常**的参考实现！它调用 `esp_camera_init`（esp32-camera 官方库）
- **对比发现关键差异**：
  | 项 | IDF 版（正常） | 我们之前 |
  |---|---|---|
  | EOF 模式 | **vs_eof_en=0** + bytelen=4091 | vs_eof=1（VSYNC EOF）|
  | 字节序 | swap_data=0（不交换） | 软件 swap |
  | 引脚 | 同 CHD 标准 | 同（确认正确）|
  | 寄存器 | 官方 ov3660 | 已照抄 ✓ |
- **根因**：vs_eof=1 时 VSYNC 触发 EOF 在**帧中间任意时刻** → 帧数据起始偏移
  1 字节 → **看起来像字节序反了**（row60 5ace→ce5a），实际是帧错位！
  软件 swap 只是碰巧"摆正"了偏移数据，不是真正修好
- **修复（彻底照搬官方）**：
  1. hw_init：vs_eof_en=**0**（EOF 由 bytelen 驱动，官方 ll_cam_config 一致）
  2. **删除软件 swap**（swap_data=0，字节序天然正确）
  3. capture：按字节累计收满 38400（vs_eof=0 时 EOF 每 4092 字节）
- **教训**：用户一直说"直接照搬官方"——官方 esp32-camera 用 vs_eof=0+bytelen，
  我们却自创 vs_eof=1 模式，导致帧错位假象。**先看官方正常实现再动手**

### 2.19 DMA 线性链走完即停 + 4092 非行宽整数倍 —— 照搬官方行对齐 node + 环形链
- **现象**：vs_eof=0 后 capture 卡在 `total=32736`（= 8×4092）超时：
  **DMA 线性链（last next=NULL）走完 38400B 就停止**，不再产生 EOF
- **根因（官方 ll_cam_calc_rgb_dma 没照搬）**：
  1. 官方 node 大小**与行宽对齐**：RGB565 160px = 320B/行 → node=**3840**
     （12 行），38400/3840 = **10 整除**，无余数错位
  2. 官方 DMA **持续流**（每帧重载或环形），不会走完即停
  3. 4092 不是 320 的整数倍 → 帧边界错位
- **修复（彻底照搬）**：
  - `CAM_RX_CHUNK` 4092 → **3840**（12 行对齐，10 node 整除）
  - dma_setup 后**手动把 last desc 指回 first** → **环形链**，DMA 持续跑
  - capture：记录环位 start_idx=(block%10)*3840，收满 38400 后**旋转
    rxbuf**（分块左移 k 次）使帧对齐到 rxbuf[0]
- **预期**：不再卡 32736；环形链持续收数据，10 EOF = 一帧，行对齐 + 字节序正确

### 2.20 环形链旋转竞态 —— DMA 持续写覆盖旋转中的 rxbuf
- **现象**：环形链 + 行对齐后不再卡 32736（`frame aligned at idx=3840`），
  但 frame head 全 00、屏幕"很乱的花色"
- **根因**：capture 收集满 38400 后**立即旋转 rxbuf**，但**环形 DMA 持续写**，
  旋转 memcpy 期间 DMA 覆盖刚整理好的数据 → 帧错乱/开头全 0
- **修复**：
  1. 旋转前 `esp32s3_dma_disable` **冻结 DMA**（rxbuf 静止）
  2. 旋转 + sink 发送后 `cam_dvp_arm()` **重启环形链**
- **预期**：冻结后 frame head 非零、图像正常（白墙白、黑框黑）

### 2.21 手册定案：0x4300=0x61 需要字节交换（用户查 OV3660 Table 7-15）
- **用户提供手册精确定义（Table 7-15）**：
  - 0x4300=0x61 = Bit[7:4]=0x6(RGB565) + Bit[3:0]=0x1(Sequence)
  - **Byte0 = {r[4:0], g[5:3]}**（高字节，先发）
  - **Byte1 = {g[2:0], b[4:0]}**（低字节，后发）
- **LVGL RGB565 小端期望**：低地址=低字节 {g[2:0],b[4:0]}，高地址=高字节 {r,g}
- **矛盾**：传感器先发高字节 Byte0 → DMA 存 rxbuf[0]（低地址）= 高字节 →
  LVGL 小端把高字节当低字节读 → **R/B 互换**（白墙显示暗紫红）
- **定案**：**0x61 + 软件字节交换**（每像素 2 字节 swap）= 正确组合
  （2.16 加过、2.18 误删；现帧结构修好后恢复）
- **其他寄存器核对手册**：0x501F=0x01 RGB 路径 ✓；0x5000=0xA7 ISP 全开
  （LENC/Gamma/BPC/WPC/CIP，官方序列一致）；0x5001=0x01 SDE 关+AWB 开 ✓

### 2.22 转 JPEG 模式（用户要求 + 官方 vs_eof=0 本就是 JPEG 设计）
- **用户指出**：vs_eof=0 + bytelen 是官方 esp32-camera 的 **JPEG 模式**设计
  （RGB565 全帧用 vs_eof=1）。我们用 vs_eof=0 收 RGB565 帧边界错位 → 全 0
- **IDF 版清晰真因**：FRAMESIZE_UXGA=1600×1200 + JPEG 存 PSRAM，
  不是 JPEG 格式本身；OpenVela PSRAM 坏（交接文档 psram_get_available_size=0）
  → 用户选 SD 卡存照片
- **PSRAM 确认坏**：HANDOVER_BOOT_CRASH.md 记录 NuttX 侧 PSRAM 不可用，
  当前固件 rxbuf 全静态 DRAM、照片存 SD 卡，**完全不依赖 PSRAM** ✓
- **改动**：
  1. 传感器配 JPEG 320×240（camera_config_jpeg_qvga，官方序列已有）
  2. DMA: vs_eof=0 + bytelen=4091，rxbuf 65536（16×4092 环形）
  3. capture: 冻结 DMA → 找 EOI(0xFFD9) → 回找 SOI(0xFFD8) → 送完整 JPEG
  4. 落盘 /mnt/sd/photo.jpg（原始 JPEG 流，电脑直接可看）
  5. cmd_cam capture 用 jpeg 配置 + 96KB 缓冲
- **预期**：SD 卡 photo.jpg 是完整可查看的 JPEG 照片

### 2.23 流式写 SD（用户从堆栈角度指出）
- **用户观点**：应从内存（堆栈）角度把图片**流式传输到 SD 卡**，
  不要整帧收进内存再写
- **改动**：capture 改为**逐块流式消费**：
  - DMA 环形链每完成 4092B 块 → sink 立即写 SD（fwrite 追加）
  - 块内 + 跨块检测 SOI(0xFFD8) 开始、EOI(0xFFD9) 结束
  - 跳过第一帧（流对齐）
- **内存优势**：峰值 = rxbuf + 1 块（≈70KB），非整帧（160KB）；
  JPEG 帧大小无上限（边收边写）
- **落盘**：/mnt/sd/photo.jpg（原始 JPEG 流）

### 2.24 rxbuf 65536 饿死堆 -> plant/触摸屏失效（已修复）
- **现象**：上一版 rxbuf 65536 后 `plant: command not found` + 触摸屏不工作
- **根因**：bss 285KB + LVGL 96KB 池 → heap 跌到 **88.8KB**（_sheap=0x3fcd7bb0）
  → UI/plant 启动失败（命令没注册）
- **修复**：
  - rxbuf 65536 → **32768**（8×4092，流式只需几块缓冲）
  - cmd_cam 改 NULL 缓冲（流式直接写 SD，免 98304 堆分配）
  - heap 恢复 **121.5KB**（_sheap=0x3fccfb70）
- **教训**：静态缓冲（rxbuf/LVGL 池）直接吃掉 DRAM → 堆缩小；
  流式模式下 rxbuf 只需容纳消费滞后，不要贪大

### 2.25 UI 拍照路径冲突（rgb565 配置 + JPEG 流式 capture）
- **现象**：UI 自动/按钮拍照走 screen_camera 的 cam_on_shoot，仍配 RGB565，
  但 capture 已改 JPEG 流式（找 SOI/EOI）→ RGB565 数据无 SOI → total=0 超时
- **修复**：UI cam_on_shoot 改为 camera_config_jpeg_qvga + NULL 缓冲流式存 SD，
  状态栏提示"已存 SD: photo.jpg"
- **注意**：LVGL 无 JPEG 解码器，UI 不再显示预览，只提示保存成功

### 2.26 最终方案定案：RGB565 160×120 LCD 直显
- **硬约束确认**：无 JPEG 解码器（LCD 无法显示 JPEG）+ PSRAM 坏（无法放高
  分辨率缓冲）+ DRAM 只剩 121KB
- **用户需求**：LCD 看到图片 + 最省事 + bug 最少 + 内存少
- **定案**：RGB565 160×120 直显（唯一满足全部要求的方案）：
  - 零拷贝 DMA→rxbuf→LVGL，无解码器/格式转换
  - rxbuf 37.5KB 静态，无解码缓冲
  - vs_eof=0 + 行对齐 3840 块 + 环形链 + 冻结 DMA + rotate + 字节交换
- **JPEG 存 SD 用途**：AI 服务器（电脑/服务端解码），不是给 LCD
- **分辨率限制**：160×120（PSRAM 坏 + DRAM 不足，320×240=150KB 装不下）

### 2.27 水平断带根因：vs_eof=0 EOF 数据驱动不帧对齐 → 加 VSYNC 帧边界
- **现象**：拍纯黑也有水平断带（紫/深红/绿横线），frame head 全 00
- **根因**：vs_eof=0 + bytelen 的 EOF 每 3840 字节（数据驱动），**与帧
  边界（VSYNC）无关**！收集 10 块 = 38400 字节可能跨两个 VSYNC 帧 → 行错位
- **官方做法（ll_cam_vsync_isr）**：用 **LCD_CAM VSYNC 中断标记帧边界**，
  bytelen EOF 只搬数据
- **修复**：
  1. 注册 ESP32S3_IRQ_LCD_CAM 外设中断（esp32s3_lcd.c 未编译，IRQ 空闲）
  2. ISR 检测 cam_vsync_int_st → vsync_count++（IRAM-safe）
  3. 使能 LCD_CAM_LC_DMA_INT_ENA 的 cam_vsync_int_ena
  4. capture：先等 VSYNC（帧边界）→ 从 VSYNC 后 block 收集 10 块 → 冻结
     → rotate → swap → sink
- **预期**：帧从 VSYNC 边界开始，不再跨帧错位 → 断带消失

### 2.28 完整理解官方架构（用户要求整体看）—— 乒乓半帧 + VSYNC 帧完成
- **用户批评**：一直零散找根因，没有整体看官方实现
- **完整读完官方**（ll_cam.c 623 行 + cam_hal.c 819 行）后彻底修正：
  | 官方（cam_task 状态机） | 我之前（错） |
  |---|---|
  | bytelen = **半帧-1**（乒乓） | 整帧-1 |
  | **VSYNC = 帧完成** → stop → 取帧 | VSYNC = 帧开始 |
  | EOF 半帧 → 数据落位；每帧重新 ll_cam_start | 收集 10 块当一帧 |
  | DMA 链环形（last→first）| 有 ✓ |
- **修正**：
  - bytelen = CAM_HALF_BYTES-1 = 19199（半帧）
  - DMA 链 = 2 半帧（half0=desc0-4, half1=desc5-9）环形
  - capture：EOF 计数（数据自动落位）→ **VSYNC 冻结 DMA** → swap → sink
    → 重新 arm
- **预期**：帧边界 = VSYNC（帧完成），EOF 半帧落位 → 不再跨帧错位

### 2.29 VSYNC ISR 不触发：cam_dvp_arm 的 cam_reset 清掉 INT_ENA
- **现象**：vsync_count 涨到 233（ISR 触发过）但 capture 循环检测不到
  frame_done → half 涨到 465 超时
- **根因**：start() 调 cam_dvp_arm()，其 cam_reset 脉冲**复位 LCD_CAM
  外设 → 清掉 LCD_CAM_LC_DMA_INT_ENA 的 cam_vsync_int_ena**！
  init 里使能的 VSYNC 中断在 start 后被 reset 清除 → capture 期间 ISR
  不触发（vsync=233 是残留/部分触发）
- **修复**：capture 的 initial VSYNC 后**重新使能 cam_vsync_int_ena**
- **教训**：cam_reset 会复位整个 LCD_CAM 外设（含中断使能），
  arm/reset 后必须重使能需要的 INT_ENA（官方 ll_cam_start 后也重配）

### 2.30 VSYNC 检测时序 bug：计数比较永远错过（frame_pending 修复）
- **现象**：vsync 中断正常（vsync 24→234 涨）、EOF 正常（half 46→466，
  2×vsync 增速），但 frame_done 从未触发 → 10s 超时
- **根因**：单核系统里 ISR 只在中断窗口（如 up_udelay 的 tick）触发。
  主循环 `vs_now = vsync_count` 读到"已更新"的值 → `vsync_count != vs_now`
  永远 false！VSYNC 总是在 udelay 期间到达，醒来后 count 已变，比较失效
- **修复**：ISR 置 **frame_pending=1** 标志，capture 检查并清零
  （边沿检测，不依赖"读取期间变化"）
- **教训**：单核 + 轮询 + ISR 计数，用"计数比较两次读取"检测事件会
  因中断时序永远错过；用 ISR 置标志 + 主循环查标志最可靠

### 2.31 最终定案：照搬官方 dvp_spi_lcd 例程（vs_eof=1 + 不 swap + 线性链）
- **用户问**：官方例程有没有合适的顺序/逻辑演示？→ 有：dvp_spi_lcd
- **完整读完 dvp_spi_lcd + 新驱动 esp_cam_ctlr_dvp 后定案**：
  | 官方（摄像头→LCD）| 我之前（错）|
  |---|---|
  | **vs_eof=1**（VSYNC EOF=帧完成）| vs_eof=0 bytelen |
  | **8 位数据不 swap**（byte_swap 8bit 不支持）| 加字节 swap |
  | **线性链 + 每帧重新 load** | 环形链 |
  | 帧完成→直接显示 | 半帧乒乓 |
- **修正**：vs_eof=1 + 去 swap + 线性链 + capture 等 2 帧取第 2 帧
- **教训**：一直参考 esp32-camera 旧库（vs_eof=0），实际官方摄像头→LCD
  用的是新驱动 esp_cam_ctlr_dvp（vs_eof=1）；字节 swap 对 8 位数据是错的

### 2.32 花屏真因定案：DMA 窗口错位（主因）+ 字节序（次因）—— ISR 每帧 re-arm + 软件交换
- **现象**：vs_eof=1 + 线性链后仍"时不时彩色混乱"、白墙不白
- **根因 1（主因，帧错位）**：旧 capture 只在调用时（任意相位）re-arm 线性链，
  DMA 窗口 = [arm → 下一 VSYNC] = **部分帧**；且每次 capture 相位随机 →
  每次拍到的都是不同偏移的碎片。"等 2 个 EOF 丢弃首帧"无效：线性链在
  首 EOF 处已停（vs_eof=1 的 EOF 终止传输），缓冲在首 EOF 后不再变化
- **根因 2（次因，颜色）**：OV3660 0x4300=0x61 先发高字节 {r,g[5:3]}，
  LVGL 要小端 → R/B 换位 + G 分裂（绿→紫红）。硬件 cam_byte_order 8 位无效
- **修复（对齐官方 esp_cam_ctlr_dvp 每帧 start_trans）**：
  1. **GDMA EOF ISR 每帧边界 re-arm**（stop → in_rst → 重载 link → start，
     inline 寄存器写，IRAM 安全；不碰 cam_reset/afifo_reset）→ 每个 DMA
     窗口恰好一帧，帧头 = rxbuf[0]
  2. capture：对齐（等 1 EOF）→ 掩蔽 EOF IRQ（防 ISR 覆盖帧）→ 等 raw EOF
     （DMA 停在帧边界）→ 冻结 → 诊断打印（raw/swap 双解码）→ 软件字节交换
     → sink → 恢复 arm + 重使能中断
  3. `plant cam swap on|off` 运行时切换字节交换（默认 ON，按手册 Table 7-15）
- **验证**：白墙应白（验证帧对齐）；绿叶应绿（验证字节序，看 swp 三元组）

### 2.33 "暗+绿"根因：官方默认序列没抄全（缺 AWB/gamma，混入 JPEG 尾）
- **现象（用户实机）**：帧对齐修复后仍"暗+绿"——白墙在 LCD 上偏暗、绿色主导
  （swap 解码 R≈1 G≈20 B≈0；raw 解码 R≈16-24 G≈0 B≈10），且多次 capture
  数值不稳（01 40 → 0a c0 → 0a 80）
- **根因（对照官方 ov3660_settings.h sensor_default_regs 逐条 diff）**：
  1. **结尾缺 {0x5001, 0x83}**（"turn color matrix, awb and SDE"）→ AWB 从未
     开启 → 白墙偏绿。官方默认序列以 0x5001=0x83 结尾，我们完全没写 0x5001
  2. **gamma 表缺 13 个点**：0x5800/0x5802/0x5803/0x5804/0x580b/0x5811/0x5817/
     0x581c/0x581d/0x581f/0x5820/0x5823/0x583d（官方有完整 0x5800-0x583d 曲线，
     我们缺位默认 0x00 → 高光被压黑 → 白墙发暗）
  3. **默认序列尾部误加 JPEG 专用寄存器** {0x3002,0x00} {0x3006,0xff}
     {0x471c,0x50}（官方这些只在 sensor_fmt_jpeg 里；默认序列应为 0x471c=0xd0）
     → 传感器停留在 JPEG 相关状态
- **修复**：默认序列整体替换为官方 215 条（含 0x5001=0x83、完整 gamma、
  0x471c=0xd0）；verify 增加 0x5001=0xa3(0x83|scale 0x20)、0x5800、0x583d、
  0x471c 读回校验；`plant cam reg` 增加曝光/增益/AWB/PLL 寄存器读取；
  `plant cam diag` 增加帧率测量（L0b）
- **教训**：上一轮"照抄官方"只抄了中间段，头部（复位时序）和尾部
  （ISP/AWB/gamma）各缺/错；必须整表 diff，不能凭印象

### 2.34 帧在"暗↔亮、绿↔红↔蓝"间振荡 —— 每次命令都重初始化 XMCLK 打断收敛
- **现象**：默认序列对齐官方后（verify 13 项全 OK），白墙画面仍不稳定：
  连续 capture 抓到 `46 ee`(亮绿) → `00 20`(近黑) → `04 a8`(中绿) →
  `ff ff`(白！) → 又变 `5a 25`(青)/`81 f9`(红)/`32 ce`(黄)，且
  AEC 曝光值在 784(满)/392(半) 间跳
- **根因**：**每次 `plant cam capture/reg` 都调用 camera_init() →
  board_camera_sccb_probe() → board_camera_xmclk_init()**，把 LEDC
  XMCLK 定时器/通道重新配置一遍（reset 脉冲）→ 传感器时钟/PL
  L/AEC/AWB 被反复打断 → 永远收敛不了。某次 capture 抓到 `ff ff`
  证明收敛后白墙确实是白的（配置已正确），只是被下次命令打断
- **修复**：
  1. `esp32s3_board_camera.c`：board_camera_xmclk_init() 加 static
     s_ready，LEDC 只配一次（幂等）
  2. `camera_capture.c` camera_init()：static s_probed，SCCB probe 只做
     一次（避免每次 capture 重跑 probe/唤醒）
  3. 首次配置稳定延时 300ms → 1000ms（AEC/AWB 收敛时间；screen_camera.c
     同步）
- **教训**：探测/时钟初始化必须幂等；硬件命令反复重配时钟 = 自找不收敛

### 2.35 收敛确认 + 预热修复（对标官方/NuttX 先例）
- **现象**：XMCLK 幂等修复后，白墙帧仍偏色且在"黄↔青"间漂移
  （46ee→5a45→5a4d），G 通道 50→42 缓慢下降 = AWB 仍在收敛途中；
  捂黑近黑、白墙偏亮 = 曝光链路已正常，剩纯色偏问题
- **对标**：Zenn《ESP32-S3 に NuttX を載せてカメラを撮ったら緑一色だった》
  （NuttX+ESP32-S3+OV2640，症状/平台完全一致）实证：
  - 绿被覆真因 = **AEC/AWB 未收敛的冷帧**（数十帧才收敛，头几帧必暗+绿）
  - 官方做法 = **空转 24 帧丢弃后再取帧**，零后处理即自然色
    （实测冷帧 G=38.7 → 收敛后 G=16.8，R/G 0.07→0.34）
  - 另有：OV2640 RGB565 是 LE 输出（LBYTE_FIRST），BE 解码全红 ——
    字节序先例，OV3660 待收敛后实测确认
- **修复**：capture 对齐后加 **CAM_WARMUP_FRAMES=24 帧预热**（ISR 持续
  re-arm，缓冲被连续覆盖不读取），再掩蔽取帧；每帧 29ms → 预热 ~0.7s

### 2.36 真凶实锤：DVP 数据管脚位序错乱（对照 IDF 可用版逐脚核对）
- **用户提示**："是不是有一处管脚没用上啊，对标一下 IDF 的 plant，人家的摄像头
  为啥没那么费劲"
- **对照** `esp32s3/plant-companion/components/camera_capture/camera_capture.c`
  （IDF 可用版，JPEG 清晰）：
  pin_d0=12 pin_d1=10 pin_d2=9 pin_d3=11 pin_d4=13 pin_d5=21 pin_d6=38 pin_d7=40
  （esp32-camera 的 pin_dN → CAM_DATA_INN，即 传感器D2→位0 … D9→位7）
- **我们的错误**（esp32s3_cam_dvp.c 旧表）：
  GPIO9→IN0（传感器D4）、GPIO11→IN2（传感器D5）、GPIO12→IN3（传感器D2）
  → **D2/D4/D5 接错位，收到的每字节都是位乱序**
- **为什么症状如此诡异**：纯白像素 = 全 1 字节，任何位置换仍是 0xFF →
  抓到过 `ff ff` 白帧；暗像素 ≈ 0 不受影响 → 捂黑近黑；但任何带纹理/阴影/
  颜色的像素被位乱序 → "彩色垃圾"，且随 AWB/曝光漂移 → "时不时彩色混乱"。
  这就是寄存器怎么调都不对的根本原因（数据在进 CAM 前就乱了）
- **修复**：data_pins 改为 Y2(12)→IN0、Y3(10)→IN1、Y4(9)→IN2、Y5(11)→IN3、
  Y6(13)→IN4、Y7(21)→IN5、Y8(38)→IN6、Y9(40)→IN7
- **教训**：管脚映射必须以"能出图的版本"为准逐脚核对，不能凭文档/口头确认；
  "位序错乱"的症状是"纯色/暗场正常、纹理彩色垃圾"，要会识别

### 2.37 原理图 J4 实锤：修正后的管脚映射与原理图逐脚一致
- 用户提供板级原理图（J4 FPC24 连接器 + R104-R115 10Ω 串阻）：
  IO12→DVP_Y2、IO10→Y3、IO9→Y4、IO11→Y5、IO13→Y6、IO21→Y7、IO38→Y8、
  IO40→Y9、IO14→PCLK、IO41→HREF、IO42→VSYNC、IO39→XMCLK
- 与 2.36 修正后的驱动 data_pins 完全一致（Y2→位0 … Y9→位7）；
  旧映射（9,10,11,12,13,21,38,40）确认错误（Y2/Y4/Y5 错位）
- CAM_RESET 上拉 2V8（R53 10K）、CAM_PWDN 接地（R101 10K）→ 均不可软件控制，
  与 IDF 版 pin_reset/pin_pwdn=-1 一致

### 2.38 字节序实锤：OV3660 RGB565 是小端（低字节在前）—— swap 默认改 OFF
- **用户实测**（管脚修正 + 预热后）：
  - swap=ON：白墙暗绿、**捂黑→一堆蓝色**（低位被当高位读的典型症状）
  - **swap=OFF：白墙变白、能看清物体轮廓** ✓
- **结论**：OV3660 0x4300=0x61 输出**低字节在前**（LBYTE_FIRST，与 OV2640 一致，
  见 Zenn NuttX 文章）；2.21 节"高字节先发"的手册解读是错的（与管脚错误同源）
- **修复**：g_cam_byte_swap 默认 true → **false**；诊断注释同步反转
- **残余现象**："彩色模糊态" —— 疑似 AWB 收敛余量/低分辨率放大，待多帧稳定观察

### 2.39 曝光/白平衡振荡 —— PCLK 时序 1.25× 偏快（20MHz XCLK vs 16MHz 调优）
- **现象**：swap=off 后白墙能白、轮廓可见（图像基本成了），但连续 5 拍帧值
  仍大幅振荡（灰→暗红→绿→杂→暗棕），24 帧预热不够
- **根因（calc_sysclk 精确计算）**：官方 PLL 值 tuned for **16MHz XCLK**，
  本板 XMCLK=20MHz → PCLK = 160M/2/8 = **10MHz**（应为 8MHz），帧率 1.25×
  偏快 → AEC 按行积分曝光的时序错位 → 曝光/白平衡环路振荡不收敛
- **修复**：
  1. QQVGA 分支 pclk_div 8→**10** → PCLK = 160M/2/10 = **8MHz**（与官方调优一致）；
     QVGA 分支 4→5（未用，顺手对齐）
  2. verify 增加 0x3824=0x0a 读回
  3. 预热 24→**40 帧**（OV3660 收敛更慢）
  4. diag L0b 帧率改用 block_done（vsync 每帧触发 ~2.3 次不可靠）
- **待验证**：固定场景连续多拍是否稳定（白墙恒定白色）

### 2.40 "纯彩色/波纹" = 疑似 ISP 定点溢出（255→0 回绕）—— 待诊断确认
- **用户反馈**：swap=off 后白墙能白、轮廓可见，但仍"纯彩色 + 波纹"，
  疑似像素值溢出（255→0 回绕）
- **评估**：有道理。传感器 ISP 内部定点运算（AWB 增益 × 色彩矩阵），若
  PCLK 时序错 → AWB 统计错乱 → 增益极端 → 乘法结果超 8 位被回绕 →
  亮部变暗/彩色波纹。回绕后的值确实可以是"暗值"（如 382&0xFF=126），
  与实测"帧值偏暗但彩色"相符
- **诊断增强**：`plant cam reg` 增加读 AWB 工作增益 0x5186-0x518e、
  色彩矩阵 0x5381-0x538b、测试图案 0x503d（排除彩条）
- **判定**：若 0x5186-0x518e 出现极端值（近 0xFF）→ 溢出实锤 → 改固定
  AWB 增益兜底；若正常 → 排除 AWB 溢出，查显示/缩放路径

### 2.41 重大转折：颜色问题的根因在 **LCD 显示链路**（面板大端 vs LVGL 小端），不是摄像头！
- **用户实测 `plant img test`**（板载生成红/绿/蓝/白四象限色块，走与摄像头完全相同
  的 lv_image 显示路径）：
  预期 左上红/右上绿/左下蓝/右下白 → 实际 **左上蓝/右上红/左下绿/右下白**
- **逐位分析**：显示 = 每像素 16 位值被字节交换（红 0xF800→0x00F8≈蓝、
  绿 0x07E0→0xE007≈红、蓝 0x001F→0x1F00≈绿）——与现象完全吻合
- **根因**：ST7796 面板 RAMCTL=0xF0 为**大端**（IDF 注释 "Use big endian by
  default"；驱动内部 st7789_fill 也是高字节先发），而 **st7789_wrram 非 3WIRE
  路径把 LVGL 的小端缓冲逐字节直发**（低字节先发）→ 每个像素字节序反了
- **对照**：驱动 3WIRE 路径本来就有字节交换（rowbuff[j]=buff[j+1]）；
  IDF 例程明确 "because SPI LCD is big-endian, we need to swap the RGB bytes
  order" —— 非 3WIRE 路径漏了这一步
- **修复**：st7789_wrram 非 3WIRE 路径补字节交换（进 rowbuff 后发送）
- **推论**：之前摄像头"白墙能白、彩色全错"正是因为白墙 R=G=B 字节交换不变，
  彩色全变；修复后**UI 与摄像头颜色一起变正确**

### 2.42 定案：显示用"直接写 LCD"（官方 dvp_spi_lcd 模式），弃用 LVGL lv_image 渲染
- **用户实测**：`plant img test` 直接写 LCD（软件最近邻缩放 + LCDDEVIO_PUTAREA）
  → **四象限颜色正确、无上半乱、无残留遮挡** —— LCD 链路完全正常！
- **结论**：之前"彩色/上半乱/蓝白遮挡"全是**本 LVGL 版本 lv_image 渲染 RGB565 的 bug**
  （缩放采样错乱 + 对象删除后区域不重绘）。字节交换修复（2.41）本身是对的
- **新方案（= 官方 dvp_spi_lcd 例程）**：帧完成 → 直接画 LCD（IDF 的
  draw_bitmap ↔ NuttX 的 LCDDEVIO_PUTAREA），不经过 LVGL 渲染器
- **新增 `plant cam preview [N]`**：连续 capture → 软件缩放 → 写取景框区域
  （33,63,414,184）= 动态预览雏形；预热改"仅首次"（预览复用收敛状态）
- **下一步**：预览线程化 + 集成拍照页（UI 只画取景框边框，画面直接写）

### 2.43 尝试"直写 LCD 预览"导致全黑 —— 已回退，保留 lv_image 预览
- **尝试**（弃 lv_image，取景框清空 LVGL 对象，预览线程直写 LCD）：
  用户实测 **全黑，一张图都看不到**（连之前的花屏都没有了）
- **回退**：恢复 lv_image 预览（s_cam_image + preview_timer_cb 80ms 喂帧），
  capture 末尾不 re-arm 保留（rxbuf 稳定）；`plant cam preview` 区域回 (33,63,414,184)
- **待查**：直写方案全黑原因（预览线程 capture 失败退出？/open-ioctl-close 与
  LVGL flush 并发卡 SPI？/取景框无 LVGL 对象时 LVGL 首帧不刷该区？）
  恢复 lv_image 后先确认画面/条纹状态，再决定下一步

### 2.44 swap 矛盾定案：LCD 侧保持不动，摄像头侧取帧前强制 swap=off
- **现象**：日志 `rgb565 byte swap OFF`（set_swap(false) 已执行）→ `frame done:
  swap=on`；预览线程 set_quiet(true) 但逐帧日志全打印（quiet 也失效）→
  **g_cam_byte_swap / g_cam_quiet 被内存覆盖**（map 显示二者与 st7789
  rowbuff_be 缓冲相邻，rowbuff_be 整区写入会越界改写它们）
- **用户要求**：LCD 本体是通的，**不改 LCD 驱动/配置**（st7789.c、defconfig
  保持原样，rowbuff_be 越界暂不处理），只解决 swap 矛盾
- **定案（摄像头侧防御，2 层）**：
  1. `esp32s3_cam_dvp_capture` 取帧前（DMA 冻结后）**无条件
     `g_cam_byte_swap = false`** —— 无论标志被谁改写，本帧都以 OV3660
     正确的 LE 字节序出帧；`plant cam swap on` 命令只影响诊断显示
  2. 预览循环**每次 capture 前** `set_swap(false)`（与驱动内复位双保险）
- **效果**：任何情况下出帧字节序正确 → 取景框颜色不再随机错乱；
  swap 标志被覆盖的问题不再影响画面（LCD 越界本身留给后续处理）

### 2.45 回退确认 + "全花"补充根因（XRES=480 会让 LVGL 变 320×480）
- **全花根因**：`CONFIG_LCD_LANDSCAPE=y` 时 getvideoinfo 给 LVGL 的
  宽=ST7789_YRES。defconfig 若 XRES=480（YRES 默认 320）→ LVGL 320×480，
  物理屏 480×320 → 显示整体错乱（全花）。**正常固件必须是 XRES=320,
  YRES=480**（用户旧固件 map 中 rowbuff_be=640B=ST7789_XRES×2 即 XRES=320
  的铁证）→ 已恢复 defconfig/.config = XRES=320, YRES=480
- **set_swap 刷屏**：set_swap() 内部有 printf，放预览循环里每帧打印刷爆串口
  → 已移回循环外（仅线程启动时一次）；swap 由驱动 capture 取帧前强制复位
- **当前定案**：
  - LCD 驱动/配置：恢复 XRES=320/YRES=480（用户正常固件一致），st7789.c 原样
  - swap 矛盾：esp32s3_cam_dvp_capture step 4.5 取帧前无条件
    `g_cam_byte_swap = false`（不打印、不刷屏，任何情况出帧字节序正确）
  - 预览：lv_image 版本（上一对话回退状态）

### 2.46 定案（2026-08-28 晚）：回退到 15:32 状态，单张可清、视频未解
- **最终回退**：撤销 2.44/2.45 的全部 LCD 与摄像头侧改动 ——
  defconfig/.config 还原 XRES=480/YRES=320（原始值）、st7789.c 原样、
  esp32s3_cam_dvp.c 删掉 capture 取帧前强制 swap 复位。
  保留：capture 末尾不 re-arm、lv_image 预览（15:32 状态）、
  st7789 字节交换、ioctl 互斥锁、`plant cam preview`(33,63,414,184)
- **单张照片 → 清晰显示 LCD（已通的路径）**：
  `plant img test` / `plant cam capture` → capture 一帧（rxbuf 稳定）→
  **软件最近邻缩放 + LCDDEVIO_PUTAREA 逐行直写**（lcd_put_rgb565，
  每行一次 ioctl，size=行宽×2 ≤ 640 不越界）→ 颜色正确、无花屏。
  **关键：绕过 LVGL 渲染器**（本 LVGL 版本 lv_image 渲染 RGB565 缩放有 bug）。
  拍照页 = camera_preview_take() 冻结当前帧（定格在 LCD RAM）
- **视频（动态预览）不行的现象清单**（按时间顺序出现的所有形态）：
  1. lv_image 预览：**随机彩色长条从上往下刷**（约 30-40% 屏幕）——
     LVGL partial render 把失效区按水平条带从上往下重绘，lv_image 渲染
     RGB565 缩放 bug + 帧数据在渲染中变化 → 条带内容随机错乱
  2. 直写 LCD 预览（取景框清空 LVGL 对象）：**全黑，一张图都看不到**——
     预览线程 capture 或写屏链路失败/提前退出（lcd_put_rgb565 单行 PUTAREA
     本身不越界，嫌疑在子线程 capture 失败或首帧时序）
  3. 期间还出现过：swap 标志被 st7789 rowbuff_be 越界随机改写
     （g_cam_byte_swap 0x3fcb9931 与 640B 的 rowbuff_be 相邻，LVGL 全宽
     flush 走 putarea `stride==row_size` 分支 wrram(rows*row_size,0,1)
     一次性交换整区字节 → 越界横扫 .bss → swap 随机 on/off、quiet 失效）
  4. 配置改错（XRES=480/YRES=320 而 CONFIG_LCD_LANDSCAPE=y）→
     getvideoinfo 给 LVGL 宽=ST7789_YRES=320 → **整个 UI 全花**
  5. 直写预览 + rowbuff_be 越界叠加 → 单帧 90KB 越界 → **LCD/系统全崩**
- **约束（用户明确）**：LCD 驱动/配置（st7789.c、defconfig XRES/YRES）
  保持原样不动；编译/烧录由用户负责；调试靠串口日志

### 2.47 rowbuff_be 越界实锤（map 量化）+ 调用方规避修复（2026-08-28 深夜）
- **map 实证（nuttx.map）**：rowbuff_be = 0x3fcb96a0，size 0x280（**640B**）；
  其正后方依次是：
  ```
  0x3fcb9920  shutdown_handlers  0x10   (esp32s3_systemreset.o —— OTA 复位回调表！)
  0x3fcb9930  g_cam_quiet        0x1
  0x3fcb9931  g_cam_byte_swap    0x1
  0x3fcb9940  g_cam_rxbuf        0x9600 (38400B，摄像头帧缓冲)
  ```
- **越界量**：横屏 480 宽一行 = 960B → 单行 PUTAREA 越界 320B/行
  （img test 每行都扫掉 shutdown_handlers+quiet+swap+rxbuf[0..300]）；
  414 宽预览行 = 828B → 越界 188B/行；LVGL 全宽 24 行 flush = 23040B →
  越界 \~22.4KB（覆盖到 g_cam_rxbuf 前 \~22KB）。
- **为什么"画面正常却内存被毁"**：wrram 交换循环先把整块（含越界区）写完，
  再 SPI_SNDBLOCK 从同一地址读回 → 面板数据自洽（显示正确）；
  越界**写**却真实破坏了 .bss。这就是 swap 随机 on/off（set_swap(false)
  后 capture 打印 swap=on）、quiet 失效、"帧头被毁导致首行乱"的机制。
- **修复（调用方规避，未动 st7789.c/defconfig）**：
  `lcd_put_rgb565` 每行**分块 ≤320 像素（640B）**再 PUTAREA（320+160 /
  320+94），wrram 交换循环不再越界。img test / preview / img show 全部走
  此函数 → 直写路径从此不再破坏任何 .bss。
- **注意**：LVGL 自身的 flush（lv_nuttx_lcd.c → PUTAREA 全宽多行）仍会越界
  —— 这是 UI 运行期 swap/quiet 被破坏的来源。约束下不修 st7789.c 的话，
  正式预览方案必须让 LVGL 不刷取景框区域（取景框内不放 LVGL 对象 +
  不失效该区），或在 UI 侧对 flush 做裁剪/暂停（待 NSH 实验后定）。
- **`plant cam preview` 增强**：每 5 帧打印帧头、每帧打印耗时 ms、
  LCD 写失败即报错退出、开头提示首帧 ~3s（配置 1s + 预热 40 帧）。
  **首帧前 ~3s 是正常等待，不是黑屏故障。**

### 2.48 决定性实验成功 + R/B 分量序悬案（0x4300=0x61 官方注释 "BGR"）
- **NSH 实测（用户，2026-08-28）**：
  - `plant cam capture`：14 项 verify 全 OK；帧头非零有结构（`64 93 6c b3...`）；
    **取景框出现轮廓**（"能看到轮廓、不垃圾"）→ **rxbuf 数据正常 + 直写链路正常，
    彩屏垃圾 = 100% lv_image 渲染 bug，实锤**。
  - `plant cam preview 20`：每帧 260~280ms（≈3.7FPS）。瓶颈 = 写屏：414 宽行
    分块后 368 次 ioctl，10MHz SPI ≈ 200ms/帧；capture ≈ 70ms（对齐等 1 帧 +
    取帧等 1 帧 = 2 个帧周期）。
- **新发现（R/B 分量序从未验证）**：官方 esp32-camera `ov3660_settings.h:290`
  `{FORMAT_CTRL00, 0x61}, // RGB565 (BGR)` —— 与我们的 0x61 一致，但注释是
  **BGR**：16 位字内分量顺序可能是 B,G,R 而非标准 R,G,B。字节序（LE、swap OFF）
  已实锤（2.38），但 **R/B 分量序从未被验证**——白墙 R=G=B、轮廓=亮度，都测不出
  R/B 互换。官方 cam_hal.c `swap_data=0` 也只证明不做字节交换。
  若 0x61 真为 BGR，则小端解码后红↔蓝互换（红色物体显示成蓝）。
- **判定方法（新增 `plant cam bar`）**：开 0x503d bit0 彩条测试图案 → 传感器输出
  已知 8 条标准色（白/黄/青/绿/品红/红/蓝/黑），与场景/曝光/白平衡无关 →
  采样每条中心 → 打印 4 种解码（LE 当前路径 / SWP / LE_RB / SWP_RB），
  哪列吻合标准色即需哪种变换。跑完自动关彩条恢复实时画面。
- **注意**：绿/青/品红/白/黑对 R/B 交换部分或完全不变，判读靠 **红条和蓝条**
  （红条应显示红、蓝条应显示蓝）。

### 2.49 🔴 重大翻案：OV3660 是**大端**（高字节先发），swap 必须 ON —— 彩条实锤
- **`plant cam bar` 修正后实测（0x503D=0x80 标准 8 色条，写后读回 0x80 确认生效）**：
  ```
  bar0 白  (31,63,31) | bar1 黄 (31,31,3) | bar2 青 (3,63,29) | bar3 绿 (2,63,0)
  bar4 品红(29,0,31)  | bar5 红 (28,0,2) | bar6 蓝 (0,0,28)  | bar7 黑 (0,1,0)
  ```
  **只有 SWP（字节交换）列与标准 8 色条完全吻合；LE 列全错** →
  OV3660 RGB565 输出**高字节先发（大端）**，**swap 必须 ON**；
  且 SWP 解码就是标准 RGB565（红=红、蓝=蓝）→ **R/B 分量序正确，无需额外处理**。
- **为什么 2.38"小端 LE、swap OFF"是错的**：当时靠"白墙变白、捂黑发蓝"判定。
  白墙 0xFFFF 和捂黑 ~0x0000 都是**字节交换对称**的（FFFF↔FFFF、0000↔0000），
  根本测不出字节序；"轮廓可见"只证明亮度结构。2.21 的 datasheet 读数
  （"Byte0={r,g[5:3]} 高字节先发"）**从一开始就是对的**。
- **改动**：`esp32s3_cam_dvp.c` g_cam_byte_swap 默认 **false→true**；
  `plant cam capture/preview`、`camera_preview_loop` 强制 set_swap(**true**)；
  `plant cam bar` 临时 swap=off 收原始字节（双解码判读）后恢复 swap=on。
- **影响**：直写显示的颜色从此正确（此前 LE 解码是字节序反的，植物/台灯色偏）。
  lv_image 路线不受影响（仍是渲染 bug 彩屏）。
- **遗留（视频"彩色地带往下刷"）**：与颜色无关，是 10MHz SPI 逐行写屏 ~200ms
  肉眼可见的涂画过程 + 帧间内容差异；需要提 CONFIG_LCD_ST7789_FREQUENCY
  （defconfig，非 XRES/YRES）或缩小取景框。UI 模式 LVGL 全宽 flush 的
  rowbuff_be 越界仍会随机改写 swap 标志（NSH 无 UI 不受影响）。

### 2.50 ✅ 里程碑：彩色地带消失（swap=ON 硬件验证通过）
- **用户实机确认（2026-08-29）**：swap=ON 烧录后，视频（NSH preview 与
  UI 拍照页）**"没有彩色地带了"**。
- **结论**：此前视频的"彩色地带一直往下刷"主因是**字节序错乱的颜色在
  逐行涂画**（swap 反了 → 每帧都是错色，交界处尤其刺眼），不是纯速度问题。
  swap=ON 修正后颜色正确，涂画过程不再表现为"彩色地带"（剩余只是 10MHz
  写屏的慢刷新感，用户决定暂不提频、不做激进优化）。
- **当前可用状态**：`plant cam capture`（单帧直写取景框）、`plant cam preview N`
  （连续直写）、UI 拍照页（lv_image，源数据已正确）——颜色正确、无花屏；
  帧率 ~3.7FPS 待用户后续决定是否提 CONFIG_LCD_ST7789_FREQUENCY。

### 2.51 UI 拍照页改直写预览（= plant cam preview 逻辑）+ LVGL 越界根治
- **lv_nuttx_lcd.c flush_cb 分块修复**：一次 PUTAREA 列宽 >320px 时，st7789
  wrram 整块交换越界横扫 .bss（swap 标志被覆写 → "彩色丝往下刷"的根源之一）。
  现 flush_cb 按列分块 ≤320px 并带整区 stride（驱动走行-by-行分支，每行交换
  ≤640B 不越界）——**LVGL 任何全宽 flush 从此不再破坏 .bss**（顺带保住
  shutdown_handlers/OTA 复位表、g_cam_rxbuf 帧头）。
- **screen_camera.c 重写（拍照页）**：
  - 弃用 lv_image + preview_timer（渲染 bug + 每 80ms 全图失效 → 彩色丝）；
  - `camera_preview_start(33,63,414,184)` 启动后台线程：capture → 
    `lcd_put_rgb565` 直写取景框内部，**画面与 3px 绿框严丝合缝**；
  - 取景框内部不放任何 LVGL 对象（不失效该区 → LVGL 不重绘盖掉画面）；
  - 删除占位图标/扫描线/重叠提示（干净）；
  - 右上角帧计数小字（调试辅助，只失效自身区域）；
  - 拍照 = take() 冻结（**必须冻结：否则线程直写会撕坏盖在拍照页上层的
    诊断页内容**）；从诊断页返回时 frame_timer 检测到本页可见 → 自动 resume。
- **camera_preview_loop 直写 LCD**：capture 后调用 lcd_put_rgb565(rxbuf,
  160,120, x,y,w,h)，目标区域由 camera_preview_start 参数传入（此前被
  (void) 忽略）。

### 2.52 ✅ UI 拍照页直写预览实机通过（用户确认"通了"）
- **用户实机确认（2026-08-29）**：拍照页画面填满绿框、颜色正确、
  **无彩色丝往下刷**。直写预览 + swap=ON + flush_cb 分块三项合体生效。
- **用户点评**："我就知道是 swap 的原因" —— 2.21 datasheet 读数
  （高字节先发）与用户直觉一致；2.38"小端"误判由 2.49 彩条翻案。
- **当前完整可用路径**：
  - 拍照页（UI）：camera_preview_start(33,63,414,184) 直写取景框，
    严丝合缝；拍照=冻结定格，返回自动续拍；帧计数右上角。
  - NSH：`plant cam capture`（单帧直写显示）/ `plant cam preview N` /
    `plant cam bar`（彩条诊断）。
- **遗留**：帧率 ~3.7FPS（10MHz SPI 写屏）——用户决定暂不提频，不激进。


