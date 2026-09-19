/****************************************************************************
 * arch/xtensa/src/esp32s3/esp32s3_cam_dvp.c
 *
 * ESP32-S3 LCD_CAM peripheral in CAM (input) mode: capture DVP 8-bit
 * parallel camera frames (OV3660 RGB565) via GDMA RX.
 *
 * Written NuttX-style, reusing the same GDMA + LCD_CAM infrastructure as
 * esp32s3_lcd.c (LCD is the same peripheral in output mode).
 *
 * Capture model (full-frame, modeled on IDF dvp_spi_lcd + esp_cam_ctlr_dvp):
 *   - XMCLK(IO39) drives the sensor clock (board-layer LEDC, 20MHz)
 *   - DMA RX chain covers one RGB565 frame (10 x 3840B desc = 38400B)
 *   - cam_vs_eof_en = 1: VSYNC generates in_suc_eof = frame complete
 *   - the GDMA EOF ISR re-arms the chain at every frame boundary (the
 *     official start_trans() per-frame model): each DMA window is exactly
 *     one frame, aligned to rxbuf[0] (this fixed the "花色" misalignment)
 *   - capture() masks the EOF IRQ, waits for the next EOF, freezes the
 *     DMA and reads the stable frame; optional software RGB565 byte swap
 *     (sensor sends high byte first, LVGL wants little-endian)
 *   - GDMA RX channel IRQ (independent of LCD_CAM peripheral IRQ); the
 *     LCD_CAM IRQ is used for the VSYNC frame-boundary counter (this
 *     board's LCD is SPI-based, so ESP32S3_IRQ_LCD_CAM is free)
 *
 * [DIAG] Camera bring-up. Keep until capture is fully working.
 ****************************************************************************/

#include <nuttx/config.h>

#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <debug.h>

#include <nuttx/irq.h>
#include <nuttx/spinlock.h>
#include <nuttx/kmalloc.h>
#include <nuttx/arch.h>
#include <nuttx/sched.h>

#include "xtensa.h"
#include "esp32s3_gpio.h"
#include "esp32s3_dma.h"
#include "esp32s3_irq.h"
#include "esp32s3_spiram.h"
#include "hardware/esp32s3_soc.h"
#include "rom/cache.h"

/* 2026-09-09: ROM cache helper used by the I2S PSRAM DMA path; not
 * declared in the esp32s3 rom/cache.h header. */

extern int rom_Cache_WriteBack_Addr(uint32_t addr, uint32_t size);
#include "hardware/esp32s3_system.h"
#include "hardware/esp32s3_lcd_cam.h"
#include "hardware/esp32s3_dma.h"

#ifdef CONFIG_ESP32S3_CAM_DVP

/****************************************************************************
 * Private Definitions
 ****************************************************************************/

/* DVP pin mapping (CHQ ESP32-S3-BOX V2.0, corrected 2026-08-28 to match
 * the WORKING IDF reference esp32-camera config):
 *   Y2=IO12 Y3=IO10 Y4=IO9 Y5=IO11 Y6=IO13 Y7=IO21 Y8=IO38 Y9=IO40
 *   PCLK=IO14 HREF=IO41 VSYNC=IO42 XMCLK=IO39
 * Y2..Y9 are the sensor's 8-bit data (D2..D9) and must land on
 * CAM_DATA_IN0..IN7 IN ORDER (Y2->bit0 ... Y9->bit7).  The old mapping
 * had Y2/Y4/Y5 swapped -> bit-scrambled bytes -> garbage colors.
 */

#define CAM_DVP_PCLK_PIN   14
#define CAM_DVP_HREF_PIN   41
#define CAM_DVP_VSYNC_PIN  42

/* CAM signal indices (soc/gpio_sig_map.h) */

#define SIG_CAM_PCLK       149   /* CAM_PCLK_IDX */
#define SIG_CAM_H_ENABLE   150   /* CAM_H_ENABLE_IDX (HREF) */
#define SIG_CAM_V_SYNC     152   /* CAM_V_SYNC_IDX (VSYNC) */

/* XMCLK: NOT configured here.  The board layer (esp32s3_board_camera.c)
 * drives IO39 with LEDC PWM at 20MHz via board_camera_xmclk_init()
 * (called from board_camera_sccb_probe -> camera_init). */

/* Full-frame capture (vs_eof=1), one RGB565 160x120 frame per DMA window:
 *  - descriptor chain covers exactly one frame (10 x 3840B desc, line-aligned)
 *  - cam_vs_eof_en = 1: VSYNC triggers in_suc_eof = frame complete
 *  - GDMA EOF ISR re-arms the chain at every frame boundary (official
 *    esp_cam_ctlr_dvp_start_trans() model) */

/* 2026-09-10: 帧尺寸改为【运行时可选】：预览 160x120 / 拍照 640x480。
 * DMA 链与两块 PSRAM 缓冲一律按最大帧分配；vs_eof_en=1 时 VSYNC 即帧
 * 边界（EOF 提前结束本次搬运），所以链比当前帧长无害 —— DMA 根本用不
 * 完就被 EOF 截断。切换模式只改 CPU 回读多少字节（g_frame_size）。 */

#define CAM_FRAME_BPP     2
#define CAM_MAX_W         640
#define CAM_MAX_H         480
#define CAM_MAX_SIZE      (CAM_MAX_W * CAM_MAX_H * CAM_FRAME_BPP)  /* 614400 */

#define CAM_LINE_BYTES    (CAM_MAX_W * CAM_FRAME_BPP)  /* 1280 */
#define CAM_RX_CHUNK      3840   /* 3 lines @640w / 12 lines @160w：都对齐 */
#define CAM_RX_DESC_NUM   160    /* >= ceil(614400 / 4032) = 153 */
#define CAM_RX_BUF_SIZE   CAM_MAX_SIZE

/* AEC/AGC/AWB 收敛预热帧数。
 *
 * Zenn「NuttX ESP32-S3 摄像头绿一色」根因实证：OV 系列传感器的
 * 自动曝光/增益/白平衡需要**数十帧**才收敛，冷帧（STREAMON 后头几帧）
 * 必然是"暗 + 绿被覆"。官方做法 = 空转 24 帧丢弃后再取帧，零后处理
 * 即得自然色。我们曾实测 G 通道从 50 缓慢降到 42（AWB 仍在收敛途中），
 * 而 capture 只等 2 帧就抓帧 → 抓到的一直是"冷帧"。OV3660 实测收敛更慢，
 * 预热取 40 帧（~1.2s @34FPS / ~2.3s @17.8FPS）。 */

#define CAM_WARMUP_FRAMES 40

/****************************************************************************
 * Private Types
 ****************************************************************************/

struct esp32s3_cam_s
{
  int          dma_channel;          /* NuttX GDMA channel */
  bool         initialized;          /* init done once (idempotent) */
  bool         started;
  bool         warmed_up;            /* AEC/AWB 预热只做一次（预览复用） */
  struct esp32s3_dmadesc_s dmadesc[CAM_RX_DESC_NUM];
  volatile uint32_t block_done;      /* completed frames (ISR-only inc;
                                      * vs_eof=1: one per VSYNC EOF) */
  volatile uint32_t vsync_count;     /* VSYNC pulses seen (LCD_CAM ISR) */
  volatile uint32_t frame_pending;   /* VSYNC seen since capture start */
};

/****************************************************************************
 * Private Data
 ****************************************************************************/

static struct esp32s3_cam_s g_cam;

/* DMA RX + shadow 缓冲（各 38400B）2026-09-09 起改放 PSRAM 顶部保留区
 * （esp32s3_psram_static_alloc），不再占用内部 DRAM：内部 DRAM 被 WiFi
 * (esp_wifi 仅能内部分配) 与其它静态耗尽后会导致 ic_ebuf_alloc NULL /
 * 数据帧发不出。DMA 链本身仍走内部描述符；rxbuf/shadow 数据在 PSRAM
 * 经 GDMA 可直达（与 I2S 录音 PSRAM 路径一致），CPU 读回前做 cache
 * 失效（cam_dvp_sync_rxbuf）。分配在 cam_dvp_dma_init() 一次性完成。 */

static uint8_t *g_cam_rxbuf;
static uint8_t *g_cam_shadow;

/* 预览流模式开关：esp32s3_cam_dvp_set_stream(true) 后 capture 走滚动快照 */

static bool g_stream_mode = false;
static bool g_stream_armed = false;
static uint8_t *g_last_frame = NULL;   /* get_frame 返回帧：stream=shadow */

/* Software RGB565 byte swap, default ON.
 *
 * 2026-08-29 corrected (definitively, by sensor color bar): OV3660 RGB565
 * (0x4300=0x61) outputs BIG-ENDIAN (high byte first).  PROOF = `plant cam
 * bar` (0x503D=0x80 standard 8-color bar): captured bars match
 * [white yellow cyan green magenta red blue black] ONLY in the byte-swapped
 * (SWP) decode; the little-endian decode is garbage.  The earlier
 * "LITTLE-ENDIAN" conclusion (debug log 2.38) rested on ambiguous tests:
 * a WHITE WALL is 0xFFFF and a COVERED lens is ~0x0000 — both are
 * byte-swap symmetric, and contours only prove luminance structure, so
 * neither could detect byte order.  The original datasheet reading
 * "high byte first" (debug log 2.21) was right all along.
 * Toggle at runtime with `plant cam swap on|off`. */

static bool g_cam_byte_swap = true;

/* 静默模式（预览用）：关闭 capture 的逐帧日志，避免刷屏拖慢 */

static bool g_cam_quiet = false;

/* 当前帧几何（运行时切换） */

static uint32_t g_frame_w = 160;
static uint32_t g_frame_h = 120;
static uint32_t g_frame_size = 160 * 120 * CAM_FRAME_BPP;

#define CAM_LOG(...) \
  do { if (!g_cam_quiet) printf(__VA_ARGS__); } while (0)

/****************************************************************************
 * Name: cam_dvp_sync_rxbuf
 *
 * Description:
 *   PSRAM 上的 rxbuf 由 GDMA 直接写入（绕过 CPU cache）。DMA 完成一帧后、
 *   CPU 读取前必须先作废该段 cache 行，否则 CPU 会读到旧缓存数据
 *   （I2S 录音同款处理）。内部 SRAM 指针时跳过，行为与原先完全一致。
 *
 ****************************************************************************/

static void cam_dvp_sync_rxbuf(void)
{
  if (esp32s3_ptr_extram(g_cam_rxbuf))
    {
      Cache_Invalidate_Addr((uint32_t)g_cam_rxbuf, g_frame_size);
    }
}

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static void cam_swap_rgb565(uint8_t *buf, size_t len)
{
  size_t i;

  for (i = 0; i + 1 < len; i += 2)
    {
      uint8_t t = buf[i];

      buf[i]     = buf[i + 1];
      buf[i + 1] = t;
    }
}

/* Print a few pixels at byte offset `off` decoded BOTH ways as RGB565:
 *   [raw_le ...] = interpretation WITHOUT swap (wrong for OV3660: it is
 *                  BIG-ENDIAN, high byte first — see 2.49 color bar proof)
 *   [swp ...]    = interpretation WITH swap (CORRECT)
 * White bar: swp should be near-white. */

static void cam_diag_pixels(const uint8_t *fp, int off)
{
  int p;
  int row = off / CAM_LINE_BYTES;

  printf("[Cam-DVP] row %d @%d:", row, off);
  for (p = 0; p < 4; p++)
    {
      uint16_t raw_le = fp[off + p * 2] | (uint16_t)(fp[off + p * 2 + 1] << 8);
      uint16_t swp    = fp[off + p * 2 + 1] | (uint16_t)(fp[off + p * 2] << 8);

      printf(" [%04x|%04x r%02u g%02u b%02u | r%02u g%02u b%02u]",
             raw_le, swp,
             (raw_le >> 11) & 0x1f, (raw_le >> 5) & 0x3f, raw_le & 0x1f,
             (swp >> 11) & 0x1f, (swp >> 5) & 0x3f, swp & 0x1f);
    }

  printf("\n");
}

static inline uint32_t cam_dvp_getreg(uint32_t reg)
{
  return getreg32(reg);
}

static inline void cam_dvp_putreg(uint32_t reg, uint32_t val)
{
  putreg32(val, reg);
}

/* Per-channel GDMA register accessor (register sets are 0xC8 apart). */

static uint32_t cam_gdma_reg(uint32_t base_ch0, int ch)
{
  return base_ch0 + (uint32_t)ch * GDMA_REG_OFFSET;
}

/****************************************************************************
 * Name: cam_dvp_gpio_config
 *
 * XMCLK(IO39) is driven by the board layer via LEDC PWM 20MHz
 * (board_camera_xmclk_init, called from camera_init -> probe).  We must
 * NOT touch IO39 here, otherwise the sensor clock dies.
 ****************************************************************************/

static void cam_dvp_gpio_config(void)
{
  /* DVP data pins -> CAM_DATA_IN0..IN7.
   *
   * IMPORTANT (2026-08-28): mapping corrected to match the WORKING IDF
   * reference (esp32s3/plant-companion/components/camera_capture/):
   *   pin_d0=12 pin_d1=10 pin_d2=9 pin_d3=11 pin_d4=13 pin_d5=21
   *   pin_d6=38 pin_d7=40  (esp32-camera pin_dN -> CAM_DATA_INN)
   * i.e. sensor D2(Y2)->bit0, D3(Y3)->bit1, D4(Y4)->bit2, D5(Y5)->bit3,
   *      D6(Y6)->bit4, D7(Y7)->bit5, D8(Y8)->bit6, D9(Y9)->bit7.
   * The previous mapping fed D2/D4/D5 to the wrong bit positions -> every
   * received byte was bit-scrambled -> RGB565 colors were garbage while
   * pure-white pixels (all-1 bits) still survived as 0xFFFF (the "ff ff"
   * frames we saw) and dark pixels stayed ~0.  That exactly matches the
   * observed "colorful chaos" that no register fix could cure. */

  static const struct
  {
    int gpio;
    int sig;
  } data_pins[8] =
  {
    { 12, 133 },  /* Y2 (sensor D2) -> CAM_DATA_IN0_IDX (bit 0) */
    { 10, 134 },  /* Y3 (sensor D3) -> CAM_DATA_IN1_IDX (bit 1) */
    { 9,  135 },  /* Y4 (sensor D4) -> CAM_DATA_IN2_IDX (bit 2) */
    { 11, 136 },  /* Y5 (sensor D5) -> CAM_DATA_IN3_IDX (bit 3) */
    { 13, 137 },  /* Y6 (sensor D6) -> CAM_DATA_IN4_IDX (bit 4) */
    { 21, 138 },  /* Y7 (sensor D7) -> CAM_DATA_IN5_IDX (bit 5) */
    { 38, 139 },  /* Y8 (sensor D8) -> CAM_DATA_IN6_IDX (bit 6) */
    { 40, 140 },  /* Y9 (sensor D9) -> CAM_DATA_IN7_IDX (bit 7) */
  };
  int i;

  for (i = 0; i < 8; i++)
    {
      esp32s3_configgpio(data_pins[i].gpio, INPUT);
      esp32s3_gpio_matrix_in(data_pins[i].gpio, data_pins[i].sig, 0);
    }

  esp32s3_configgpio(CAM_DVP_PCLK_PIN, INPUT);
  esp32s3_gpio_matrix_in(CAM_DVP_PCLK_PIN, SIG_CAM_PCLK, 0);

  esp32s3_configgpio(CAM_DVP_HREF_PIN, INPUT);
  esp32s3_gpio_matrix_in(CAM_DVP_HREF_PIN, SIG_CAM_H_ENABLE, 0);

  esp32s3_configgpio(CAM_DVP_VSYNC_PIN, INPUT);
  esp32s3_gpio_matrix_in(CAM_DVP_VSYNC_PIN, SIG_CAM_V_SYNC, 1);

  /* XMCLK(IO39) is NOT touched here: board layer drives it with LEDC PWM
   * 20MHz (board_camera_xmclk_init via camera_init/probe).
   *
   * VSYNC inverted (last arg = 1): esp32-camera cam_init() sets
   * cam_obj->vsync_invert = true; OV3660 drives VSYNC low during the
   * active frame, inverting aligns CAM EOF with the real frame start. */

  lcdinfo("DVP pins routed to LCD_CAM CAM input (XMCLK=IO39 via LEDC)\n");
}

/****************************************************************************
 * Name: cam_dvp_hw_init
 ****************************************************************************/

static void cam_dvp_hw_init(void)
{
  uint32_t regval;

  /* Enable LCD_CAM peripheral clock DIRECTLY via SYSTEM register.
   * This board's LCD uses SPI (CONFIG_ESP32S3_LCD=n) so esp32s3_lcd.c is
   * not compiled and nobody enables the LCD_CAM clock.  We bypass
   * periph_module_enable() (its spinlock path is suspect in this build)
   * and set the clock-enable + clear-reset bits directly. */

  modifyreg32(SYSTEM_PERIP_CLK_EN1_REG, 0, SYSTEM_LCD_CAM_CLK_EN);
  modifyreg32(SYSTEM_PERIP_RST_EN1_REG, SYSTEM_LCD_CAM_RST, 0);

  /* Whole-register reset, exactly like IDF ll_cam_config():
   *   LCD_CAM.cam_ctrl.val  = 0;
   *   LCD_CAM.cam_ctrl1.val = 0;
   *   LCD_CAM.cam_rgb_yuv.val = 0;
   * This avoids stale bits (e.g. cam_start, bytelen) left by previous
   * read-modify-write passes. */

  cam_dvp_putreg(LCD_CAM_CAM_CTRL_REG, 0);
  cam_dvp_putreg(LCD_CAM_CAM_CTRL1_REG, 0);
  cam_dvp_putreg(LCD_CAM_CAM_RGB_YUV_REG, 0);

  /* cam_ctrl (IDF order):
   *   cam_clkm_div_b/a = 0
   *   cam_clkm_div_num = 160M / 20M = 8   (xclk = PLL160M/8)
   *   cam_clk_sel      = 3                (PLL160M source)
   *   cam_stop_en      = 0
   *   cam_vsync_filter_thres = 4
   *   cam_update       = 0 (armed later in start)
   *   cam_byte_order   = 0, cam_bit_order = 0
   *   cam_line_int_en  = 0
   *   cam_vs_eof_en    = 0  (EOF by cam_rec_data_bytelen, like esp32-camera
   *                          ll_cam_config.  vs_eof=1 fires mid-frame,
   *                          data starts at wrong offset -> looked like
   *                          a byte-order bug!)
   */

  regval = 0;
  regval |= (8 << LCD_CAM_CAM_CLKM_DIV_NUM_S);   /* 160M/8 = 20M XMCLK */
  regval |= (3 << LCD_CAM_CAM_CLK_SEL_S);        /* PLL160M */
  regval |= (4 << LCD_CAM_CAM_VSYNC_FILTER_THRES_S);
  regval |= (1 << LCD_CAM_CAM_VS_EOF_EN_S);      /* vs_eof=1: VSYNC EOF =
                                                  * frame complete (official
                                                  * dvp_spi_lcd cam_hal_init:
                                                  * cam_ll_enable_vsync_generate
                                                  * _eof(hw,1)) */
  /* cam_byte_order=0: official rejects byte swap for 8-bit data
   * ("byte swap is not supported when cam_data_width is 8") */
  cam_dvp_putreg(LCD_CAM_CAM_CTRL_REG, regval);

  /* cam_ctrl1 (IDF order):
   *   cam_rec_data_bytelen = 3839 (compat only; vs_eof=1 makes VSYNC the
   *                                           EOF source)
   *   cam_line_int_num = 0
   *   cam_clk_inv = 0 (PCLK not inverted)
   *   cam_vsync_filter_en = 1
   *   cam_2byte_en = 0 (8-bit data)
   *   cam_de_inv / hsync_inv / vsync_inv = 0
   *   cam_vh_de_mode_en = 0
   */

  regval = 0;
  regval |= ((CAM_RX_CHUNK - 1) << LCD_CAM_CAM_REC_DATA_BYTELEN_S);
  regval |= (1 << LCD_CAM_CAM_VSYNC_FILTER_EN_S);
  cam_dvp_putreg(LCD_CAM_CAM_CTRL1_REG, regval);

  printf("[Cam-DVP] CAM hw configured (8-bit DVP, PLL160M/8, "
         "vs_eof=1, bytelen=%d compat)\n", CAM_RX_CHUNK - 1);
}

/****************************************************************************
 * Name: cam_dvp_dma_init
 ****************************************************************************/

static int cam_dvp_dma_init(void)
{
  struct esp32s3_cam_s *priv = &g_cam;

  /* burst=true -> esp32s3_dma_setup() uses ESP32S3_DMA_BUFLEN_MAX_4B_ALIGNED
   * (4092) per descriptor, exactly matching LCD_CAM_DMA_NODE_BUFFER_MAX_SIZE
   * in IDF's esp32-camera.  With burst=false the descriptor would be 4095
   * bytes while bytelen is 4091 -> EOF lands 3 bytes early, corrupting the
   * stream. */

  priv->dma_channel = esp32s3_dma_request(ESP32S3_DMA_PERIPH_LCDCAM,
                                          10, 1, true);
  if (priv->dma_channel < 0)
    {
      printf("[Cam-DVP] GDMA RX alloc FAILED\n");
      return -ENODEV;
    }

  printf("[Cam-DVP] dma_channel=%d\n", priv->dma_channel);

#ifdef CONFIG_ESP32S3_SPIRAM_COMMON_HEAP
  g_cam_rxbuf = esp32s3_psram_static_alloc(CAM_RX_BUF_SIZE);
  g_cam_shadow = esp32s3_psram_static_alloc(CAM_MAX_SIZE);
#else
  static uint8_t rxbuf_fallback[CAM_RX_BUF_SIZE]
    __attribute__((aligned(64)));
  static uint8_t shadow_fallback[160 * 120 * CAM_FRAME_BPP]
    __attribute__((aligned(64)));

  g_cam_rxbuf = rxbuf_fallback;
  g_cam_shadow = shadow_fallback;
#endif
  if (g_cam_rxbuf == NULL || g_cam_shadow == NULL)
    {
      printf("[Cam-DVP] psram carve alloc FAILED\n");
      return -ENOMEM;
    }

  printf("[Cam-DVP] rxbuf=%p size=%u (psram carve)\n",
         g_cam_rxbuf, CAM_RX_BUF_SIZE);
  memset(g_cam_rxbuf, 0, CAM_RX_BUF_SIZE);
  if (esp32s3_ptr_extram(g_cam_rxbuf))
    {
      rom_Cache_WriteBack_Addr((uint32_t)g_cam_rxbuf, CAM_RX_BUF_SIZE);
    }

  printf("[Cam-DVP] dma_setup call (%u desc x %uB)...\n",
         CAM_RX_DESC_NUM, CAM_RX_CHUNK);
  esp32s3_dma_setup(priv->dmadesc, CAM_RX_DESC_NUM,
                    g_cam_rxbuf, CAM_RX_BUF_SIZE,
                    false, priv->dma_channel);

  /* Linear chain (official dvp_spi_lcd + esp_cam_ctlr_dvp): the GDMA EOF
   * ISR re-loads the chain at every frame boundary (per-frame start),
   * so the DMA always captures exactly one frame per VSYNC window. */

  printf("[Cam-DVP] dma_setup done (linear chain, ISR re-arm per frame)\n");

  return OK;
}

/****************************************************************************
 * Name: cam_dvp_gdma_isr
 *
 * GDMA RX channel interrupt: in_suc_eof fires once per frame (vs_eof=1,
 * VSYNC-driven).  ISR does NOT touch NuttX semaphores/syslog (IRAM-safe):
 * it increments a volatile counter and RE-ARMS the DMA chain at the frame
 * boundary, exactly like the official driver's per-frame start_trans().
 *
 * Re-arm is DMA-only (stop -> in_rst -> reload link -> start), inline
 * register writes so the ISR stays IRAM-safe.  We deliberately do NOT
 * pulse cam_reset / cam_afifo_reset here: cam_reset would clear the
 * LCD_CAM INT_ENA (debug log 2.29) and afifo_reset would drop FIFO
 * continuity; the CAM peripheral keeps streaming on its own once started.
 *
 * This per-frame re-arm is the key fix for the "花色" (garbled colors):
 * previously the chain was only re-armed from capture() at an arbitrary
 * phase, so the DMA window [arm -> next VSYNC] was a partial frame that
 * changed every capture.  Re-arming at every VSYNC EOF makes each window
 * exactly one frame, aligned to the frame start.
 ****************************************************************************/

static int IRAM_ATTR cam_dvp_gdma_isr(int irq, void *context, void *arg)
{
  struct esp32s3_cam_s *priv = &g_cam;
  int ch = priv->dma_channel;
  uint32_t st;
  uint32_t regval;

  st = cam_dvp_getreg(cam_gdma_reg(DMA_IN_INT_ST_CH0_REG, ch));
  if (st != 0)
    {
      cam_dvp_putreg(cam_gdma_reg(DMA_IN_INT_CLR_CH0_REG, ch), st);
    }

  if (st & DMA_IN_SUC_EOF_CH0_INT_ST_M)
    {
      priv->block_done++;

      /* 1. stop DMA RX (self-clearing bit) */
      regval = cam_dvp_getreg(cam_gdma_reg(DMA_IN_LINK_CH0_REG, ch));
      regval |= DMA_INLINK_STOP_CH0_M;
      cam_dvp_putreg(cam_gdma_reg(DMA_IN_LINK_CH0_REG, ch), regval);

      /* 2. reset RX FSM + FIFO pointer (in_rst pulse) */
      regval = cam_dvp_getreg(cam_gdma_reg(DMA_IN_CONF0_CH0_REG, ch));
      regval |= DMA_IN_RST_CH0_M;
      cam_dvp_putreg(cam_gdma_reg(DMA_IN_CONF0_CH0_REG, ch), regval);
      regval &= ~DMA_IN_RST_CH0_M;
      cam_dvp_putreg(cam_gdma_reg(DMA_IN_CONF0_CH0_REG, ch), regval);

      /* 3. reload the descriptor link base (preserve other LINK bits) */
      regval = cam_dvp_getreg(cam_gdma_reg(DMA_IN_LINK_CH0_REG, ch));
      regval &= ~DMA_INLINK_ADDR_CH0;
      regval |= (uint32_t)priv->dmadesc & DMA_INLINK_ADDR_CH0;
      cam_dvp_putreg(cam_gdma_reg(DMA_IN_LINK_CH0_REG, ch), regval);

      /* 4. start RX again */
      regval = cam_dvp_getreg(cam_gdma_reg(DMA_IN_LINK_CH0_REG, ch));
      regval |= DMA_INLINK_START_CH0_M;
      cam_dvp_putreg(cam_gdma_reg(DMA_IN_LINK_CH0_REG, ch), regval);
    }

  return 0;
}

/****************************************************************************
 * Name: cam_dvp_vsync_isr
 *
 * LCD_CAM peripheral interrupt: cam_vsync marks frame boundaries.  With
 * vs_eof=0 the DMA EOF is data-driven (every 3840B) and does NOT align
 * to frames - without a VSYNC marker, a 38400B collection can span two
 * frames -> horizontal banding.  ISR only clears the flag and counts.
 ****************************************************************************/

static int IRAM_ATTR cam_dvp_vsync_isr(int irq, void *context, void *arg)
{
  struct esp32s3_cam_s *priv = &g_cam;
  uint32_t st;

  st = cam_dvp_getreg(LCD_CAM_LC_DMA_INT_ST_REG);
  if (st & LCD_CAM_CAM_VSYNC_INT_ST_M)
    {
      cam_dvp_putreg(LCD_CAM_LC_DMA_INT_CLR_REG,
                     LCD_CAM_CAM_VSYNC_INT_ST_M);
      priv->vsync_count++;
      priv->frame_pending = 1;
    }

  return 0;
}

/****************************************************************************
 * Name: cam_dvp_arm
 *
 * Re-arm the DMA chain + CAM start.  Follows esp32-camera ll_cam_start()
 * exactly (IDF-proven sequence).  Called on first start and after every
 * completed frame (per-frame re-arm, like the dvp_spi_lcd example).
 ****************************************************************************/

static void cam_dvp_arm(void)
{
  struct esp32s3_cam_s *priv = &g_cam;
  uint32_t regval;

  /* 1. cam_start = 0 (clear leftover start bit) */

  regval = cam_dvp_getreg(LCD_CAM_CAM_CTRL1_REG);
  regval &= ~LCD_CAM_CAM_START_M;
  cam_dvp_putreg(LCD_CAM_CAM_CTRL1_REG, regval);

  /* 2. CAM + AFIFO reset pulses (WO bits) */

  regval = cam_dvp_getreg(LCD_CAM_CAM_CTRL1_REG);
  regval |= LCD_CAM_CAM_RESET_M;
  cam_dvp_putreg(LCD_CAM_CAM_CTRL1_REG, regval);
  regval &= ~LCD_CAM_CAM_RESET_M;
  cam_dvp_putreg(LCD_CAM_CAM_CTRL1_REG, regval);

  regval |= LCD_CAM_CAM_AFIFO_RESET_M;
  cam_dvp_putreg(LCD_CAM_CAM_CTRL1_REG, regval);
  regval &= ~LCD_CAM_CAM_AFIFO_RESET_M;
  cam_dvp_putreg(LCD_CAM_CAM_CTRL1_REG, regval);

  /* 3. Set bytelen (kept for compatibility; EOF driven by VSYNC) */

  regval = cam_dvp_getreg(LCD_CAM_CAM_CTRL1_REG);
  regval &= ~LCD_CAM_CAM_REC_DATA_BYTELEN_M;
  regval |= ((CAM_RX_CHUNK - 1) << LCD_CAM_CAM_REC_DATA_BYTELEN_S);
  cam_dvp_putreg(LCD_CAM_CAM_CTRL1_REG, regval);

  /* 4. Load chain (in_rst pulse + link addr) and start DMA (link start) */

  esp32s3_dma_load(priv->dmadesc, priv->dma_channel, false);
  esp32s3_dma_enable(priv->dma_channel, false);

  /* 5. cam_ctrl.cam_update = 1 (update lives in cam_ctrl) */

  regval = cam_dvp_getreg(LCD_CAM_CAM_CTRL_REG);
  regval |= LCD_CAM_CAM_UPDATE_REG_M;
  cam_dvp_putreg(LCD_CAM_CAM_CTRL_REG, regval);

  /* 6. cam_ctrl1.cam_start = 1 */

  regval = cam_dvp_getreg(LCD_CAM_CAM_CTRL1_REG);
  regval |= LCD_CAM_CAM_START_M;
  cam_dvp_putreg(LCD_CAM_CAM_CTRL1_REG, regval);
}

/****************************************************************************
 * Name: cam_dvp_enable_ints
 *
 * Re-enable the two capture interrupts.  cam_dvp_arm() pulses cam_reset,
 * which resets the whole LCD_CAM peripheral and therefore clears
 * LC_DMA_INT_ENA (debug log 2.29) — call this after every cam_dvp_arm().
 ****************************************************************************/

static void cam_dvp_enable_ints(void)
{
  struct esp32s3_cam_s *priv = &g_cam;
  uint32_t regval;

  /* GDMA RX in_suc_eof interrupt (per-frame re-arm driver) */

  regval = cam_dvp_getreg(cam_gdma_reg(DMA_IN_INT_ENA_CH0_REG,
                                       priv->dma_channel));
  regval |= DMA_IN_SUC_EOF_CH0_INT_ENA_M;
  cam_dvp_putreg(cam_gdma_reg(DMA_IN_INT_ENA_CH0_REG, priv->dma_channel),
                 regval);

  /* LCD_CAM cam_vsync interrupt (frame boundary diagnostics) */

  regval = cam_dvp_getreg(LCD_CAM_LC_DMA_INT_ENA_REG);
  regval |= LCD_CAM_CAM_VSYNC_INT_ENA_M;
  cam_dvp_putreg(LCD_CAM_LC_DMA_INT_ENA_REG, regval);
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int esp32s3_cam_dvp_init(void)
{
  struct esp32s3_cam_s *priv = &g_cam;
  uint32_t regval;
  int ret;

  /* Idempotent: only initialize once.  Repeated calls (e.g. each
   * camera_capture_frame) must NOT re-request DMA channels or
   * re-memalign the frame buffer - that leaks channels/memory. */

  if (priv->initialized)
    {
      return OK;
    }

  memset(priv, 0, sizeof(*priv));

  cam_dvp_gpio_config();
  cam_dvp_hw_init();

  ret = cam_dvp_dma_init();
  if (ret < 0)
    {
      return ret;
    }

  /* GDMA RX channel interrupt only (peripheral IRQ 66 + chan).
   * LCD_CAM peripheral IRQ is NOT used -> no conflict with LCD driver.
   * Must map the peripheral IRQ (esp32s3_setup_irq) BEFORE irq_attach +
   * up_enable_irq, otherwise g_irqmap[irq] is IRQ_UNMAPPED and
   * up_enable_irq writes a garbage CPUINT -> hangs. */

  printf("[Cam-DVP] setup_irq call (periph=%d)...\n",
         ESP32S3_PERIPH_DMA_IN_CH0 + priv->dma_channel);
  esp32s3_setup_irq(this_cpu(),
                    ESP32S3_PERIPH_DMA_IN_CH0 + priv->dma_channel,
                    ESP32S3_INT_PRIO_DEF,
                    ESP32S3_CPUINT_LEVEL);
  printf("[Cam-DVP] setup_irq done\n");

  printf("[Cam-DVP] irq_attach call (irq=%d)...\n",
         ESP32S3_IRQ_DMA_IN_CH0 + priv->dma_channel);
  ret = irq_attach(ESP32S3_IRQ_DMA_IN_CH0 + priv->dma_channel,
                   cam_dvp_gdma_isr, NULL);
  printf("[Cam-DVP] irq_attach done (ret=%d)\n", ret);
  if (ret < 0)
    {
      printf("[Cam-DVP] gdma irq attach FAILED (%d)\n", ret);
      return ret;
    }

  up_enable_irq(ESP32S3_IRQ_DMA_IN_CH0 + priv->dma_channel);
  printf("[Cam-DVP] irq enabled\n");

  /* Enable GDMA in_suc_eof interrupt */

  regval = cam_dvp_getreg(cam_gdma_reg(DMA_IN_INT_ENA_CH0_REG,
                                       priv->dma_channel));
  regval |= DMA_IN_SUC_EOF_CH0_INT_ENA_M;
  cam_dvp_putreg(cam_gdma_reg(DMA_IN_INT_ENA_CH0_REG, priv->dma_channel),
                 regval);

  printf("[Cam-DVP] init done: irq=%d\n",
         ESP32S3_IRQ_DMA_IN_CH0 + priv->dma_channel);

  /* LCD_CAM peripheral VSYNC interrupt: marks frame boundaries (needed
   * because vs_eof=0 DMA EOFs are data-driven, not frame-aligned).
   * esp32s3_lcd.c is NOT compiled (CONFIG_ESP32S3_LCD=n) so this IRQ is
   * free. */

  printf("[Cam-DVP] vsync irq setup (periph=%d)...\n",
         ESP32S3_PERIPH_LCD_CAM);
  esp32s3_setup_irq(this_cpu(),
                    ESP32S3_PERIPH_LCD_CAM,
                    ESP32S3_INT_PRIO_DEF,
                    ESP32S3_CPUINT_LEVEL);
  ret = irq_attach(ESP32S3_IRQ_LCD_CAM, cam_dvp_vsync_isr, NULL);
  printf("[Cam-DVP] vsync irq_attach done (ret=%d)\n", ret);
  if (ret < 0)
    {
      printf("[Cam-DVP] vsync irq attach FAILED (%d)\n", ret);
      return ret;
    }

  up_enable_irq(ESP32S3_IRQ_LCD_CAM);

  /* Enable cam_vsync interrupt in LCD_CAM */

  regval = cam_dvp_getreg(LCD_CAM_LC_DMA_INT_ENA_REG);
  regval |= LCD_CAM_CAM_VSYNC_INT_ENA_M;
  cam_dvp_putreg(LCD_CAM_LC_DMA_INT_ENA_REG, regval);
  printf("[Cam-DVP] vsync irq enabled\n");

  priv->initialized = true;
  lcdinfo("cam_dvp: DVP capture initialized\n");

  return OK;
}

int esp32s3_cam_dvp_start(void)
{
  struct esp32s3_cam_s *priv = &g_cam;

  /* 2026-09-18：每次开流前把 DVP 引脚从 UART0 手里抢回来。
   *
   * 离拍照页时 apps 侧会调 esp32s3_uart0_reclaim_pins() 把 IO42(TX)/IO40(RX)
   * 交还给 UART0（土壤传感器要用），而 esp32s3_cam_dvp_init() 带 initialized
   * 守卫、不会在第二次进拍照页时再跑 cam_dvp_gpio_config()。结果：第二次及
   * 以后进拍照页，VSYNC(IO42) 被 UART0 的 TX 一直拉着，收不到帧边界，
   * capture 等满 10 秒超时、预览线程退出、取景框一片空白，只有重启板卡才能
   * 恢复（用户实测："刚重启是好的，跑一会就不行了"）。
   *
   * GPIO matrix 是 last-writer-wins，这里重路由一次即可；只碰引脚，
   * 不动 DMA 与中断，可安全反复调用。 */

  cam_dvp_gpio_config();

  if (priv->started)
    {
      return OK;
    }

  /* arm the DMA chain + CAM once (full start sequence); cam_reset inside
   * arm clears the INT_ENAs, so re-enable both interrupts afterwards */

  cam_dvp_arm();
  cam_dvp_enable_ints();

  priv->started = true;
  printf("[Cam-DVP] capture started\n");

  return OK;
}

int esp32s3_cam_dvp_stop(void)
{
  struct esp32s3_cam_s *priv = &g_cam;
  uint32_t regval;

  if (!priv->started)
    {
      return OK;
    }

  /* Mask the GDMA EOF interrupt first: the ISR re-arms the chain at every
   * frame boundary and would otherwise restart a disabled DMA. */

  regval = cam_dvp_getreg(cam_gdma_reg(DMA_IN_INT_ENA_CH0_REG,
                                       priv->dma_channel));
  regval &= ~DMA_IN_SUC_EOF_CH0_INT_ENA_M;
  cam_dvp_putreg(cam_gdma_reg(DMA_IN_INT_ENA_CH0_REG, priv->dma_channel),
                 regval);

  /* cam_ll_stop(): cam_ctrl1.cam_start = 0; cam_ctrl.cam_update = 1 */

  regval = cam_dvp_getreg(LCD_CAM_CAM_CTRL1_REG);
  regval &= ~LCD_CAM_CAM_START_M;
  cam_dvp_putreg(LCD_CAM_CAM_CTRL1_REG, regval);

  regval = cam_dvp_getreg(LCD_CAM_CAM_CTRL_REG);
  regval |= LCD_CAM_CAM_UPDATE_REG_M;
  cam_dvp_putreg(LCD_CAM_CAM_CTRL_REG, regval);

  esp32s3_dma_disable(priv->dma_channel, false);
  priv->started = false;
  g_stream_armed = false;

  lcdinfo("cam_dvp: capture stopped\n");

  return OK;
}

/****************************************************************************
 * Name: esp32s3_cam_dvp_capture
 *
 * Capture one complete RGB565 frame (vs_eof=1: VSYNC-driven EOF = frame
 * complete).  The GDMA ISR re-arms the chain at every frame boundary, so
 * the DMA always captures exactly one aligned frame per VSYNC.
 *
 * Flow:
 *   1. align  — wait for one EOF with the ISR active (guarantees the DMA
 *               is mid-capture of a complete frame when we mask below)
 *   2. mask   — disable the GDMA EOF interrupt + clear pending bits, so
 *               the ISR cannot re-arm (and overwrite) the frame we read
 *   3. wait   — for the next (post-mask) EOF; the DMA stops at this frame
 *               boundary with one complete frame in rxbuf
 *   4. freeze — esp32s3_dma_disable (already stopped at EOF, safety)
 *   5. sink   — optional RGB565 byte swap, then hand frame to the sink
 *   6. re-arm — cam_dvp_arm() + re-enable both interrupts for next capture
 *
 * Input Parameters:
 *   sink     - frame callback (called once with g_frame_size bytes)
 *   sink_arg - opaque arg passed to sink
 *   eoi_out  - set to true when the frame is delivered (may be NULL)
 *
 * Returned Value: g_frame_size on success; negated errno on failure.
 ****************************************************************************/

int esp32s3_cam_dvp_capture(void (*sink)(const uint8_t *data, size_t len,
                                          void *arg),
                            void *sink_arg,
                            volatile bool *eoi_out)
{
  struct esp32s3_cam_s *priv = &g_cam;
  int ch = priv->dma_channel;
  irqstate_t flags;
  uint32_t seen;
  uint32_t stall = 0;
  uint32_t regval;
  uint32_t raww;

  if (sink == NULL && eoi_out == NULL)
    {
      return -EINVAL;
    }

  CAM_LOG("[Cam-DVP] capture enter (vs_eof=1): block_done=%u vsync=%u\n",
          priv->block_done, priv->vsync_count);

  /* 预热（仅首次）：AEC/AGC/AWB 需要几十帧收敛。DMA 必须滚动着跑，
   * 预热期间 ISR 持续 re-arm，rxbuf 被反复覆盖（我们到抓帧才读）。
   * 预热只在第一次 capture 发生 —— 连续预览复用已收敛状态。 */

  if (!priv->warmed_up)
    {
      cam_dvp_arm();
      cam_dvp_enable_ints();
      g_stream_armed = true;

      {
        int w;

        for (w = 0; w < CAM_WARMUP_FRAMES; w++)
          {
            seen = priv->block_done;
            stall = 0;
            while (priv->block_done == seen)
              {
                up_udelay(2000);
                stall++;
                if (stall > 5000)
                  {
                    CAM_LOG("[Cam-DVP] warmup timeout\n");
                    return -ETIMEDOUT;
                  }
              }
          }
      }

      priv->warmed_up = true;
      CAM_LOG("[Cam-DVP] warmup done (%d frames skipped, AEC/AWB converged)\n",
              CAM_WARMUP_FRAMES);
    }

  /* 预览流模式：滚动采集 + 快照。等待下一次 EOF（帧刚完成），立即把
   * rxbuf 复制进 shadow（并在 shadow 上做字节交换），DMA 不停止、ISR
   * 继续 re-arm —— 下一次 capture 无需重新 arm/对齐，每帧只等一个帧周期；
   * LCD 读 shadow 时 DMA 正在写 rxbuf，互不打扰、不撕裂。 */

  if (g_stream_mode)
    {
      if (!g_stream_armed)
        {
          cam_dvp_arm();
          cam_dvp_enable_ints();
          g_stream_armed = true;

          /* arm 后的首个窗口是半帧：先消化一个 EOF，之后全是整帧 */

          seen = priv->block_done;
          stall = 0;
          while (priv->block_done == seen)
            {
              up_udelay(2000);
              stall++;
              if (stall > 5000)
                {
                  CAM_LOG("[Cam-DVP] stream arm align timeout\n");
                  return -ETIMEDOUT;
                }
            }
        }

      seen = priv->block_done;
      stall = 0;
      while (priv->block_done == seen)
        {
          up_udelay(150);
          stall++;
          if ((stall % 2000) == 0)
            {
              CAM_LOG("[Cam-DVP] stream frame wait... stall=%u\n", stall);
            }

          if (stall > 80000)
            {
              CAM_LOG("[Cam-DVP] stream frame timeout\n");
              return -ETIMEDOUT;
            }
        }

      cam_dvp_sync_rxbuf();
      memcpy(g_cam_shadow, g_cam_rxbuf, g_frame_size);
      if (g_cam_byte_swap)
        {
          cam_swap_rgb565(g_cam_shadow, g_frame_size);
        }

      g_last_frame = g_cam_shadow;

      if (eoi_out != NULL)
        {
          *eoi_out = true;
        }

      CAM_LOG("[Cam-DVP] stream frame done: %u bytes\n",
              (unsigned)g_frame_size);
      return (int)g_frame_size;
    }

  /* === 冻结模式（拍照/彩条/单帧）：抓完即停 DMA，保证帧稳定 === */

  if (!g_stream_armed)
    {
      cam_dvp_arm();
      cam_dvp_enable_ints();
      g_stream_armed = true;

      /* arm 后首个窗口是半帧：等它自愈的 EOF（对齐） */

      seen = priv->block_done;
      stall = 0;
      while (priv->block_done == seen)
        {
          up_udelay(2000);
          stall++;
          if ((stall % 500) == 0)
            {
              CAM_LOG("[Cam-DVP] align wait... stall=%u\n", stall);
            }

          if (stall > 5000)
            {
              CAM_LOG("[Cam-DVP] align timeout, no EOF\n");
              return -ETIMEDOUT;
            }
        }

      CAM_LOG("[Cam-DVP] aligned (block_done=%u)\n", priv->block_done);
    }

  /* 2. Mask the GDMA EOF interrupt and clear any pending EOF, so no ISR
   *    re-arm can overwrite the frame while we read it. */

  flags = enter_critical_section();

  regval = cam_dvp_getreg(cam_gdma_reg(DMA_IN_INT_ENA_CH0_REG, ch));
  regval &= ~DMA_IN_SUC_EOF_CH0_INT_ENA_M;
  cam_dvp_putreg(cam_gdma_reg(DMA_IN_INT_ENA_CH0_REG, ch), regval);

  cam_dvp_putreg(cam_gdma_reg(DMA_IN_INT_CLR_CH0_REG, ch),
                 DMA_IN_SUC_EOF_CH0_INT_ST_M);

  leave_critical_section(flags);

  /* 3. Wait for the NEXT (post-mask) EOF: the DMA stops at this frame
   *    boundary with one complete frame in rxbuf (it was capturing from
   *    the last ISR re-arm at the previous boundary). */

  stall = 0;
  for (;;)
    {
      raww = cam_dvp_getreg(cam_gdma_reg(DMA_IN_INT_RAW_CH0_REG, ch));
      if (raww & DMA_IN_SUC_EOF_CH0_INT_ST_M)
        {
          break;
        }

      up_udelay(2000);
      stall++;
      if ((stall % 500) == 0)
        {
          CAM_LOG("[Cam-DVP] waiting frame EOF... stall=%u\n", stall);
        }

      if (stall > 5000)
        {
          CAM_LOG("[Cam-DVP] frame EOF timeout\n");

          /* Restore: re-arm + re-enable interrupts so the next capture
           * can retry (the EOF IRQ is still masked here). */

          cam_dvp_arm();
          cam_dvp_enable_ints();
          g_stream_armed = true;
          return -ETIMEDOUT;
        }
    }

  cam_dvp_putreg(cam_gdma_reg(DMA_IN_INT_CLR_CH0_REG, ch),
                 DMA_IN_SUC_EOF_CH0_INT_ST_M);

  /* 4. Freeze the DMA (it stopped at the EOF; belt & suspenders) */

  esp32s3_dma_disable(priv->dma_channel, false);
  g_stream_armed = false;

  /* PSRAM 上 DMA 直写的帧：CPU 读前先作废 cache（内部 SRAM 时为空操作） */

  cam_dvp_sync_rxbuf();

  /* 5. Diagnostics on the RAW frame (before any swap):
   *    [xxxx|yyyy r..g..b.. | r..g..b..] = pixel value as little-endian
   *    vs swapped; the first RGB triplet is what LVGL shows WITHOUT swap
   *    (CORRECT for OV3660), the second is what it shows WITH swap. */

  CAM_LOG("[Cam-DVP] frame done: %u bytes (swap=%s)\n",
         (unsigned)g_frame_size, g_cam_byte_swap ? "on" : "off");

  if (sink != NULL)
    {
      int k;
      const uint8_t *fp = g_cam_rxbuf;

      CAM_LOG("[Cam-DVP] frame head:");
      for (k = 0; k < 16; k++)
        {
          printf(" %02x", fp[k]);
        }

      printf("\n");

      cam_diag_pixels(fp, 0);
      cam_diag_pixels(fp, 320);
      cam_diag_pixels(fp, 16000);
    }

  /* Optional RGB565 byte swap (sensor sends high byte first, LVGL wants
   * little-endian), then hand the frame to the sink. */

  if (g_cam_byte_swap)
    {
      cam_swap_rgb565(g_cam_rxbuf, g_frame_size);
    }

  /* get_frame() 必须指向【本次抓到的这一帧】。2026-09-10 修：这行原来被
   * 误缩进在 g_cam_byte_swap 分支里 —— 一旦字节交换关掉（`plant cam swap`
   * 或将来别处改默认），g_last_frame 会停留在上一次的值（很可能是预览用的
   * shadow），调用方去 get_frame() 拿到的是【上一帧/另一块缓冲】：
   * 去饱和改在 A 上、上传读的是 B，表现成"改了跟没改一样"。 */

  g_last_frame = g_cam_rxbuf;

  if (sink != NULL)
    {
      sink(g_cam_rxbuf, g_frame_size, sink_arg);
    }

  /* 6. 末尾【不再 re-arm】：DMA 停在帧边界，rxbuf 保持稳定。
   *    这是预览不撕裂的关键 —— 若这里 cam_dvp_arm() 重新启动 DMA，
   *    下一帧会立刻覆盖 rxbuf，LVGL 渲染时读到半新半旧 → 随机条纹。
   *    下次 capture 开头会重新 arm。 */

  if (eoi_out != NULL)
    {
      *eoi_out = true;
    }

  CAM_LOG("[Cam-DVP] capture done\n");
  return (int)g_frame_size;
}

/****************************************************************************
 * Name: esp32s3_cam_dvp_diag
 *
 * Layered hardware diagnosis.  Prints the raw register state of every
 * link in the capture chain so a single call pinpoints which layer is
 * broken (camera output / GPIO routing / CAM peripheral / clock / GDMA).
 *
 * Layers:
 *   L1 camera: PCLK/VSYNC/HREF GPIO input levels (static sample)
 *   L2 CAM ctrl: cam_ctrl / cam_ctrl1 (mode, start, bytelen)
 *   L3 CAM clock: lcd_clock (clk_en, cam_clk_sel)
 *   L4 CAM int: lc_dma_int_raw (cam_vsync/hs raw bits = VSYNC arriving?)
 *   L5 GDMA: int_raw/int_st, conf0, periph sel, state, dscr
 ****************************************************************************/

void esp32s3_cam_dvp_diag(void)
{
  struct esp32s3_cam_s *priv = &g_cam;
  int ch = priv->dma_channel;
  uint32_t v;

  printf("\n===== CAM-DVP LAYER DIAG =====\n");

  printf("L0 swap: rgb565 byte swap = %s\n",
         g_cam_byte_swap ? "ON" : "OFF");

  /* L0b: frame rate (block_done delta over 1s; needs init (capture) first).
   * NOTE: vsync_count fires ~2.3x per frame (OV3660 VSYNC edges + filter),
   * use block_done (GDMA EOF = 1/frame) for the real rate. */

  if (priv->initialized)
    {
      uint32_t v0 = priv->block_done;

      up_udelay(1000000);
      printf("L0b frame rate: %u FPS (block_done %u -> %u, vsync %u)\n",
             priv->block_done - v0, v0, priv->block_done,
             priv->vsync_count);
    }
  else
    {
      printf("L0b frame rate: not initialized (run 'plant cam capture' "
             "once first)\n");
    }

  /* L1: camera GPIO levels (static; PCLK toggles fast so sample a few) */

  printf("L1 GPIO: PCLK(14)=%d HREF(41)=%d VSYNC(42)=%d "
         "D0(12)=%d D7(40)=%d\n",
         esp32s3_gpioread(14), esp32s3_gpioread(41), esp32s3_gpioread(42),
         esp32s3_gpioread(12), esp32s3_gpioread(40));

  /* PCLK toggle test: 20 samples over ~2ms - any change = clock running */

  {
    int s;
    int pclk_prev = -1;
    int pclk_changes = 0;

    for (s = 0; s < 20; s++)
      {
        int p = esp32s3_gpioread(14);

        if (pclk_prev >= 0 && p != pclk_prev)
          {
            pclk_changes++;
          }

        pclk_prev = p;
        up_udelay(100);
      }

    printf("L1 PCLK toggles in 2ms: %d/20 samples\n", pclk_changes);
  }

  /* All 8 data pins + HREF + VSYNC raw sample (sensor output check;
   * D0-D7 = Y2-Y9 = IO12,10,9,11,13,21,38,40 in bit order) */

  printf("L1 D0-7(12,10,9,11,13,21,38,40)=%d%d%d%d%d%d%d%d "
         "HREF=%d VSYNC=%d\n",
         esp32s3_gpioread(12), esp32s3_gpioread(10), esp32s3_gpioread(9),
         esp32s3_gpioread(11), esp32s3_gpioread(13), esp32s3_gpioread(21),
         esp32s3_gpioread(38), esp32s3_gpioread(40),
         esp32s3_gpioread(41), esp32s3_gpioread(42));

  /* L2: CAM control registers */

  v = cam_dvp_getreg(LCD_CAM_CAM_CTRL_REG);
  printf("L2 cam_ctrl  = 0x%08x (stop_en=%d vs_eof_en=%d clk_sel=%d)\n",
         v, (v >> 0) & 1, (v >> 8) & 1, (v >> 29) & 3);

  v = cam_dvp_getreg(LCD_CAM_CAM_CTRL1_REG);
  printf("L2 cam_ctrl1 = 0x%08x (start=%d 2byte=%d bytelen=%d)\n",
         v, (v >> 29) & 1, (v >> 24) & 1, v & 0xffff);

  /* L3: clock */

  v = cam_dvp_getreg(LCD_CAM_LCD_CLOCK_REG);
  printf("L3 lcd_clock = 0x%08x (clk_en=%d clk_sel=%d div_num=%d)\n",
         v, (v >> 31) & 1, (v >> 29) & 3, (v >> 9) & 0xff);

  /* L4: CAM interrupt raw (did VSYNC arrive?) */

  v = cam_dvp_getreg(LCD_CAM_LC_DMA_INT_RAW_REG);
  printf("L4 int_raw   = 0x%08x (lcd_vsync=%d cam_vsync=%d cam_hs=%d)\n",
         v, (v >> 0) & 1, (v >> 2) & 1, (v >> 3) & 1);

  /* L5: GDMA RX channel state */

  if (ch >= 0)
    {
      printf("L5 DMA chan=%d\n", ch);
      v = cam_dvp_getreg(cam_gdma_reg(DMA_IN_INT_RAW_CH0_REG, ch));
      printf("L5 in_int_raw = 0x%08x (suc_eof=%d err_eof=%d done=%d)\n",
             v, (v >> 1) & 1, (v >> 2) & 1, (v >> 0) & 1);
      v = cam_dvp_getreg(cam_gdma_reg(DMA_IN_CONF0_CH0_REG, ch));
      printf("L5 in_conf0   = 0x%08x (mem_trans_en=%d burst=%d)\n",
             v, (v >> 0) & 1, (v >> 5) & 1);
      v = cam_dvp_getreg(cam_gdma_reg(DMA_IN_PERI_SEL_CH0_REG, ch));
      printf("L5 in_peri_sel= 0x%08x (%d)\n", v, v & 0x1f);
      v = cam_dvp_getreg(cam_gdma_reg(DMA_IN_STATE_CH0_REG, ch));
      printf("L5 in_state   = 0x%08x\n", v);
      v = cam_dvp_getreg(cam_gdma_reg(DMA_IN_LINK_CH0_REG, ch));
      printf("L5 in_link    = 0x%08x (start=%d stop=%d)\n",
             v, (v >> 22) & 1, (v >> 21) & 1);
    }
  else
    {
      printf("L5 DMA chan not allocated\n");
    }

  /* L6: per-descriptor status (did DMA actually write bytes?) */

  if (priv->initialized)
    {
      int i;

      printf("L6 desc ctrl (buf_len=%d):\n", ESP32S3_DMA_CTRL_BUFLEN_V);
      for (i = 0; i < CAM_RX_DESC_NUM; i++)
        {
          uint32_t c = priv->dmadesc[i].ctrl;

          printf("L6  desc[%d] ctrl=0x%08x owner=%d eof=%d err=%d "
                 "datalen=%d\n",
                 i, c, (c >> 31) & 1, (c >> 30) & 1, (c >> 28) & 1,
                 (c >> ESP32S3_DMA_CTRL_DATALEN_S) &
                 ESP32S3_DMA_CTRL_DATALEN_V);
        }
    }
  else
    {
      printf("L6 not initialized yet\n");
    }

  printf("===== DIAG END =====\n");
}

/****************************************************************************
 * Name: esp32s3_cam_dvp_get_frame
 *
 * Return the internal DMA RX buffer (static, CAM_RX_BUF_SIZE bytes).
 * The latest captured frame lives in its first g_frame_size bytes.
 * Used by the UI to display the frame without a second buffer copy.
 ****************************************************************************/

uint8_t *esp32s3_cam_dvp_get_frame(void)
{
  return g_last_frame != NULL ? g_last_frame : g_cam_rxbuf;
}

/****************************************************************************
 * Name: esp32s3_cam_dvp_set_framesize
 *
 * 运行时切换帧几何（预览 160x120 / 拍照 640x480）。
 *
 * DMA 链与 rxbuf/shadow 已按 CAM_MAX_SIZE 分配，且 vs_eof_en=1 让 VSYNC
 * 成为帧边界（EOF 提前结束搬运），因此这里【不需要】重编 DMA 链 —— 只改
 * CPU 回读长度与写回长度。调用方负责先把传感器切到同一分辨率（SCCB），
 * 并在调用前确保 capture 已停止（无并发取帧）。
 *
 * ⚠️ 这里【不】清 warmed_up：两种模式用的是同一个传感器窗口
 * （start/end 均为 0,0→2079,1547），只有输出缩放不同，场景平均亮度不变
 * → AEC/AGC/AWB 依旧有效，无需重跑 40 帧预热（否则每次拍照、每次返回
 * 预览都要多等约 9 秒）。切换后的时序稳定交给 freeze 路径自带的两个
 * 帧周期对齐（见 capture 的 align + post-mask EOF 两步）。
 *
 * Input Parameters:
 *   w, h - 目标像素尺寸（<= CAM_MAX_W / CAM_MAX_H）
 *
 * Returned Value: 0 成功；负 errno 失败
 ****************************************************************************/

int esp32s3_cam_dvp_set_framesize(uint32_t w, uint32_t h)
{
  if (w == 0 || h == 0 || w > CAM_MAX_W || h > CAM_MAX_H)
    {
      printf("[Cam-DVP] set_framesize %ux%u out of range (max %dx%d)\n",
             (unsigned)w, (unsigned)h, CAM_MAX_W, CAM_MAX_H);
      return -EINVAL;
    }

  g_frame_w = w;
  g_frame_h = h;
  g_frame_size = w * h * CAM_FRAME_BPP;

  printf("[Cam-DVP] framesize -> %ux%u (%u bytes)\n",
         (unsigned)w, (unsigned)h, (unsigned)g_frame_size);
  return OK;
}

/****************************************************************************
 * Name: esp32s3_cam_dvp_get_framesize
 ****************************************************************************/

void esp32s3_cam_dvp_get_framesize(uint32_t *w, uint32_t *h)
{
  if (w != NULL) *w = g_frame_w;
  if (h != NULL) *h = g_frame_h;
}

/****************************************************************************
 * Name: esp32s3_cam_dvp_set_swap / esp32s3_cam_dvp_get_swap
 *
 * Runtime toggle for the software RGB565 byte swap (`plant cam swap`).
 * Default ON (OV3660 0x4300=0x61 sends high byte first; LVGL wants
 * little-endian).  Flip to OFF if a capture ever shows swapped R/B.
 ****************************************************************************/

void esp32s3_cam_dvp_set_swap(bool on)
{
  g_cam_byte_swap = on;
  printf("[Cam-DVP] rgb565 byte swap %s\n", on ? "ON" : "OFF");
}

bool esp32s3_cam_dvp_get_swap(void)
{
  return g_cam_byte_swap;
}

/****************************************************************************
 * Name: esp32s3_cam_dvp_set_quiet
 *
 * 静默模式：关闭 capture 的逐帧日志（预览线程用，避免刷屏拖慢）。
 ****************************************************************************/

void esp32s3_cam_dvp_set_quiet(bool quiet)
{
  g_cam_quiet = quiet;
}


/****************************************************************************
 * Name: esp32s3_cam_dvp_set_stream / esp32s3_cam_dvp_get_stream
 *
 * 预览流模式开关。开启后 capture() 走"滚动快照"路径：DMA 连续采集，
 * 帧完成即复制到 shadow（不冻结、不重新 arm），预览等待从约 2 个帧周期
 * 降到约 1 个帧周期；LCD/UI 读 shadow 时 DMA 在写 rxbuf，互不打扰。
 * 拍照/彩条等单帧路径保持关闭（冻结模式，帧稳定零拷贝）。
 ****************************************************************************/

void esp32s3_cam_dvp_set_stream(bool on)
{
  if (g_stream_mode == on)
    {
      return;
    }

  g_stream_mode = on;
  g_stream_armed = false;
  g_last_frame = on ? g_cam_shadow : g_cam_rxbuf;
  printf("[Cam-DVP] stream mode %s\n", on ? "ON (rolling snapshot)" : "OFF");
}

bool esp32s3_cam_dvp_get_stream(void)
{
  return g_stream_mode;
}
#endif /* CONFIG_ESP32S3_CAM_DVP */
