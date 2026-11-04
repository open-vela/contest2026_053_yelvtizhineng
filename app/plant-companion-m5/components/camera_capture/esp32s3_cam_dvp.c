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
 * - CoreS3 camera module generates its own XCLK
 * - DMA RX chain covers one RGB565 frame (10 x 3840B desc = 38400B)
 * - cam_vs_eof_en = 1: VSYNC generates in_suc_eof = frame complete
 * - the GDMA EOF ISR re-arms the chain at every frame boundary (the
 * official start_trans() per-frame model): each DMA window is exactly
 * one frame, aligned to rxbuf[0] (this fixed the "pattern" misalignment)
 * - capture() masks the EOF IRQ, waits for the next EOF, freezes the
 * DMA and reads the stable frame; optional software RGB565 byte swap
 * (sensor sends high byte first, LVGL wants little-endian)
 * - GDMA RX channel IRQ (independent of LCD_CAM peripheral IRQ); the
 * LCD_CAM IRQ is used for the VSYNC frame-boundary counter (this
 * board's LCD is SPI-based, so ESP32S3_IRQ_LCD_CAM is free)
 *
 * [DIAG] Camera bring-up. Keep until capture is fully working.
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <debug.h>
#include <pthread.h>

#include <nuttx/irq.h>
#include <nuttx/spinlock.h>
#include <nuttx/kmalloc.h>
#include <nuttx/arch.h>
#include <nuttx/sched.h>
#include <nuttx/signal.h>

#include "xtensa.h"
#include "esp32s3_gpio.h"
#include "esp32s3_dma.h"
#include "esp32s3_irq.h"
#include "esp32s3_spiram.h"
#include "hardware/esp32s3_soc.h"
#include "rom/cache.h"

/* 2026-09-09: ROM cache helper used by the I2S PSRAM DMA path; not declared
 * in the esp32s3 rom/cache.h header.
 */

extern int rom_cache_writeback_addr(uint32_t addr, uint32_t size)
  __asm__("rom_Cache_WriteBack_Addr");
extern int cache_invalidate_addr(uint32_t addr, uint32_t size)
  __asm__("Cache_Invalidate_Addr");
#include "hardware/esp32s3_system.h"
#include "hardware/esp32s3_lcd_cam.h"
#include "hardware/esp32s3_dma.h"

#ifdef CONFIG_ESP32S3_CAM_DVP

/****************************************************************************
 * Private Definitions
 ****************************************************************************/

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#ifdef CONFIG_ARCH_BOARD_M5STACK_CORES3

/* CoreS3 GC0308 DVP pin mapping. */

#define CAM_DVP_PCLK_PIN   45
#define CAM_DVP_HREF_PIN   38
#define CAM_DVP_VSYNC_PIN  46

#define CAM_DVP_D0_PIN     39
#define CAM_DVP_D1_PIN     40
#define CAM_DVP_D2_PIN     41
#define CAM_DVP_D3_PIN     42
#define CAM_DVP_D4_PIN     15
#define CAM_DVP_D5_PIN     16
#define CAM_DVP_D6_PIN     48
#define CAM_DVP_D7_PIN     47

#  define CAM_DVP_VSYNC_INVERT 1
#  define CAM_DVP_EXTERNAL_XTAL 1

#else

/* Original ESP32-S3-BOX DVP pin mapping. */

#define CAM_DVP_PCLK_PIN   14
#define CAM_DVP_HREF_PIN   41
#define CAM_DVP_VSYNC_PIN  42

#define CAM_DVP_D0_PIN     12
#define CAM_DVP_D1_PIN     10
#define CAM_DVP_D2_PIN      9
#define CAM_DVP_D3_PIN     11
#define CAM_DVP_D4_PIN     13
#define CAM_DVP_D5_PIN     21
#define CAM_DVP_D6_PIN     38
#define CAM_DVP_D7_PIN     40

#  define CAM_DVP_VSYNC_INVERT 1
#  define CAM_DVP_EXTERNAL_XTAL 0

#endif

/* CAM signal indices (soc/gpio_sig_map.h) */

#define SIG_CAM_PCLK       149   /* CAM_PCLK_IDX */
#define SIG_CAM_H_ENABLE   150   /* CAM_H_ENABLE_IDX (HREF) */
#define SIG_CAM_V_SYNC     152   /* CAM_V_SYNC_IDX (VSYNC) */

/* XMCLK: NOT configured here. The board layer (esp32s3_board_camera.c)
 * drives IO39 with LEDC PWM at 20MHz via board_camera_xmclk_init() (called
 * from board_camera_sccb_probe -> camera_init).
 */

/* Full-frame capture (vs_eof=1), one RGB565 160x120 frame per DMA window:
 * - descriptor chain covers exactly one frame (10 x 3840B desc,
 * line-aligned)
 * - cam_vs_eof_en = 1: VSYNC triggers in_suc_eof = frame complete
 * - GDMA EOF ISR re-arms the chain at every frame boundary (official
 * esp_cam_ctlr_dvp_start_trans() model)
 */

/* 2026-09-10: Frame size changed to [runtime selectable]: preview 160x120 /
 * capture 640x480. DMA links and dual PSRAM buffers are allocated based on
 * maximum frame size; when vs_eof_en=1, VSYNC serves as frame boundary.
 * boundary (EOF prematurely ends this transfer), so the chain being longer
 * than the current frame is harmless — DMA is not used at all gets
 * truncated by EOF. Switching modes only changes how many bytes the CPU
 * reads back (g_frame_size).
 */

#define CAM_FRAME_BPP     2
#define CAM_MAX_W         640
#define CAM_MAX_H         480
#define CAM_JPEG_MAX_W    2048
#define CAM_JPEG_MAX_H    1536
/* CAM_MAX_SIZE = 640 x 480 x 2 = 614400; CAM_LINE_BYTES = 640 x 2 = 1280.
 * CAM_RX_CHUNK is 3 lines at 640w / 12 lines at 160w (both 32-byte aligned).
 * CAM_RX_DESC_NUM >= ceil(614400 / 4032) = 153.
 */

#define CAM_MAX_SIZE      (CAM_MAX_W * CAM_MAX_H * CAM_FRAME_BPP)
#define CAM_JPEG_BUF_SIZE (1 * 1024 * 1024)
#define CAM_LINE_BYTES    (CAM_MAX_W * CAM_FRAME_BPP)
#define CAM_RX_CHUNK      3840
#define CAM_JPEG_CHUNK    1024
#define CAM_JPEG_DESC_NUM ((CAM_JPEG_BUF_SIZE + CAM_JPEG_CHUNK - 1) / \
                           CAM_JPEG_CHUNK)
#define CAM_RX_DESC_NUM   ((CAM_JPEG_BUF_SIZE + CAM_RX_CHUNK - 1) / CAM_RX_CHUNK)
#define CAM_RX_BUF_SIZE   CAM_JPEG_BUF_SIZE

/* NuttX subtracts the configured 64-byte external-memory block from the
 * 4 KiB descriptor limit, so a PSRAM descriptor carries at most 4032 bytes.
 */
#define CAM_M5_RX_NODE_BYTES 4032
#define CAM_RGB_DESC_NUM \
  ((CAM_MAX_SIZE + CAM_M5_RX_NODE_BYTES - 1) / CAM_M5_RX_NODE_BYTES)

/* Official esp_cam_ctlr_dvp_ext model: two internal DMA halves and a
 * continuous descriptor ring.  The CAM byte counter raises EOF at each half;
 * software copies the completed half into the full frame while DMA fills the
 * peer.  153600 % 7680 == 0, so QVGA completes in exactly 20 chunks.
 */
#define CAM_M5_CHUNK_BYTES 7680u
#define CAM_M5_DMA_BYTES   (CAM_M5_CHUNK_BYTES * 2u)

/* AEC/AGC/AWB convergence warm-up frame count.
 *
 * Zenn's proof of root cause for 'NuttX ESP32-S3 camera all green': OV
 * series sensors' Auto-exposure/gain/white balance need **dozens of frames**
 * to converge. Cold frames (first few frames after STREAMON) will be "dark +
 * green tint". Official method = idle 24 frames to discard, then capture,
 * zero post-processing to achieve natural color. We tested that the G
 * channel slowly dropped from 50 to 42 (AWB still converging), whereas
 * capture only waits 2 frames to grab a frame → it always grabs a 'cold
 * frame'. OV3660 measured convergence is slower, warm-up takes 40 frames
 * (~1.2s @34FPS / ~2.3s @17.8FPS).
 */

#define CAM_WARMUP_FRAMES 40

/****************************************************************************
 * Private Types
 ****************************************************************************/

struct esp32s3_cam_s
{
  int          dma_channel;          /* NuttX GDMA channel */
  bool         initialized;          /* init done once (idempotent) */
  bool         started;
  bool         warmed_up;            /* AEC/AWB warm-up done only once (reused for preview) */
  struct esp32s3_dmadesc_s dmadesc[CAM_JPEG_DESC_NUM];
#ifdef CONFIG_ARCH_BOARD_M5STACK_CORES3
  struct esp32s3_dmadesc_s dmadesc_rgb[2][CAM_RGB_DESC_NUM];
#endif

  /* completed frames (ISR-only inc; vs_eof=1: one per VSYNC EOF) */

  volatile uint32_t block_done;
  volatile uint32_t jpeg_chunks;     /* JPEG nodes completed in this capture */
  volatile uint32_t vsync_count;     /* VSYNC pulses seen (LCD_CAM ISR) */
  volatile uint32_t frame_pending;   /* VSYNC seen since capture start */
  volatile uint32_t vsync_chunks[8]; /* jpeg_chunks snapshot at each VSYNC */
  volatile uint32_t vsync_marks;     /* total VSYNC chunk marks recorded */
  volatile uint32_t short_frames;    /* RGB 丢字节短帧（被丢弃未上屏） */
};

/****************************************************************************
 * Private Data
 ****************************************************************************/

static struct esp32s3_cam_s g_cam;

/* DMA RX + shadow buffers (38400B each) relocated to reserved top of PSRAM
 * since 2026-09-09. (esp32s3_psram_static_alloc), no longer occupying
 * internal DRAM: internal DRAM is occupied by WiFi (esp_wifi can only
 * allocate internally) can lead to ic_ebuf_alloc returning NULL when
 * combined with other static exhaustion, Data frame cannot be sent. The DMA
 * chain itself still uses internal descriptors; rxbuf/shadow data resides in
 * PSRAM Directly accessible via GDMA (consistent with I2S recording PSRAM
 * path); CPU reads back after cache operation. failure (cam_dvp_sync_rxbuf).
 * Allocation is done once in cam_dvp_dma_init().
 */

static uint8_t *g_cam_rxbuf;    /* RGB565 frame or variable-length JPEG */
static uint8_t *g_cam_shadow;   /* RGB565 preview snapshot/shadow */
#ifdef CONFIG_ARCH_BOARD_M5STACK_CORES3
static uint8_t g_m5_dma_buf[CAM_M5_DMA_BYTES] __attribute__((aligned(64)));
static volatile bool g_m5_official_mode;
static volatile uint8_t g_m5_half_active;
static volatile uint8_t g_m5_half_ready;
static volatile uint32_t g_m5_half_done;
static bool g_m5_official_sensor;
static pthread_t g_m5_worker;
static volatile bool g_m5_worker_run;
static volatile uint32_t g_m5_frame_done;
static volatile uint8_t g_m5_frame_ready;
static bool g_m5_worker_started;
#define CAM_M5_FRAME_BUFFERS 4
static int cam_dvp_m5_start_chunked(void);
static void cam_dvp_m5_stop_chunked(void);
static void *cam_dvp_m5_worker_thread(void *arg);
#endif
static bool     g_cam_jpeg_mode;
static bool g_cam_yuv_input; /* true: capture variable-length JPEG */
static bool     g_jpeg_vsync_aligned;

/* Preview stream mode switch: after esp32s3_cam_dvp_set_stream(true),
 * capture proceeds with rolling snapshots
 */

static bool g_stream_mode = false;
static bool g_stream_armed = false;

/* RGB565 双缓冲（v62）：两条描述符链 A/B，各指向 rxbuf 前半/后半。
 * EOF ISR 在帧边界切换到另一条链——上一帧所在的半区在下一帧写完之前
 * 保持静止，消费方 memcpy 不再和 DMA 赛跑（v61 单缓冲下 640x480 每帧
 * 后半都被下一帧覆盖 -> 画面抖动 + 油膜花纹）。 */

static volatile uint8_t g_ready_half;   /* 最近完成帧所在半区 (0/1) */
static uint8_t g_active_half;           /* 当前 DMA 正在填的半区 */
static uint32_t g_rgb_ndesc;            /* 每条 RGB 链的描述符数 (0=回退单链) */
static volatile uint32_t g_frame_bytes[2];
static uint8_t *g_last_frame = NULL;   /* get_frame returns frame: stream=shadow */

/* Software RGB565 byte swap, default ON.
 *
 * 2026-08-29 corrected (definitively, by sensor color bar): OV3660 RGB565
 * (0x4300=0x61) outputs BIG-ENDIAN (high byte first). PROOF = `plant cam
 * bar` (0x503D=0x80 standard 8-color bar): captured bars match [white yellow
 * cyan green magenta red blue black] ONLY in the byte-swapped (SWP) decode;
 * the little-endian decode is garbage. The earlier "LITTLE-ENDIAN"
 * conclusion (debug log 2.38) rested on ambiguous tests: a WHITE WALL is
 * 0xFFFF and a COVERED lens is ~0x0000 — both are byte-swap symmetric, and
 * contours only prove luminance structure, so neither could detect byte
 * order. The original datasheet reading "high byte first" (debug log 2.21)
 * was right all along. Toggle at runtime with `plant cam swap on|off`.
 */

static bool g_cam_byte_swap = true;
static bool g_stream_direct;    /* 免拷贝流模式：直接交出 rxbuf 就绪半帧 */

/* Silent mode (for preview): disables per-frame capture logging to avoid
 * screen flicker and slowdown.
 */

static bool g_cam_quiet = false;

/* Current frame geometry (runtime switch) */

static uint32_t g_frame_w = 160;
static uint32_t g_frame_h = 120;
static uint32_t g_frame_size = 160 * 120 * CAM_FRAME_BPP;

#define CAM_LOG(...) \
  do { if (!g_cam_quiet) printf(__VA_ARGS__); } while (0)

/****************************************************************************
 * Name: cam_dvp_sync_rxbuf
 *
 * Description:
 * The rxbuf on PSRAM is written directly by GDMA (bypassing CPU cache).
 * After DMA completes a frame,
 * CPU must invalidate that cache line before reading, otherwise CPU will
 * read stale cached data
 * (Same processing as I2S recording). Skipped when internal SRAM pointer is
 * used, behavior identical to original.
 ****************************************************************************/

static void cam_dvp_sync_rxbuf(void)
{
  if (esp32s3_ptr_extram(g_cam_rxbuf))
    {
      cache_invalidate_addr((uint32_t)g_cam_rxbuf,
                            g_cam_jpeg_mode ? CAM_RX_BUF_SIZE : g_frame_size);
    }
}

/* Invalidate one RGB frame range (dual-buffer half). */

static void cam_dvp_sync_rgb_range(const uint8_t *base)
{
  if (esp32s3_ptr_extram(base))
    {
      cache_invalidate_addr((uint32_t)base, g_frame_size);
    }
}

/****************************************************************************
 * Name: cam_dvp_sync_jpeg_range
 *
 * Description:
 *   Invalidate the CPU cache for a byte range that GDMA has just written to
 *   PSRAM.  JPEG marker scanning happens while DMA is still running, so only
 *   the newly-arrived range is invalidated instead of the full 2 MiB buffer.
 ****************************************************************************/

static void cam_dvp_sync_jpeg_range(uint32_t start, uint32_t end)
{
  uintptr_t addr;
  size_t len;

  if (!esp32s3_ptr_extram(g_cam_rxbuf) || end <= start)
    {
      return;
    }

  addr = (uintptr_t)(g_cam_rxbuf + start) & ~((uintptr_t)31);
  len = (size_t)(end - (uint32_t)(addr - (uintptr_t)g_cam_rxbuf));
  len = (len + 31U) & ~((size_t)31);
  cache_invalidate_addr((uint32_t)addr, len);
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
 * [raw_le ...] = interpretation WITHOUT swap (wrong for OV3660: it is
 * BIG-ENDIAN, high byte first — see 2.49 color bar proof) [swp ...] =
 * interpretation WITH swap (CORRECT) White bar: swp should be near-white.
 */

static void cam_diag_pixels(const uint8_t *fp, int off)
{
  int p;
  int row = off / CAM_LINE_BYTES;

  printf("[Cam-DVP] row %d @%d:", row, off);
  for (p = 0; p < 4; p++)
    {
      uint16_t raw_le = fp[off + p * 2] |
                        (uint16_t)(fp[off + p * 2 + 1] << 8);
      uint16_t swp    = fp[off + p * 2 + 1] |
                        (uint16_t)(fp[off + p * 2] << 8);

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

#ifdef CONFIG_ARCH_BOARD_M5STACK_CORES3
static void cam_dvp_m5_converter_config(void)
{
  uint32_t val = 0;

  if (g_cam_yuv_input)
    {
      /* LCD_CAM hardware YUV422 -> RGB565, BT.601 limited range.
       * cam_conv_trans_mode = 0 (YUV->RGB), yuv_mode = 0 (4:2:2),
       * yuv2yuv_mode = 3, 8-bit input, converter enabled. */
      val |= (3 << LCD_CAM_CAM_CONV_YUV2YUV_MODE_S);
      val |= LCD_CAM_CAM_CONV_MODE_8BITS_ON_M;
      /* GC0308 YUYV is full-range.  Set both input and output conversion
       * ranges explicitly instead of relying on reset defaults. */
      val |= LCD_CAM_CAM_CONV_DATA_IN_MODE_M;
      val |= LCD_CAM_CAM_CONV_DATA_OUT_MODE_M;
    }
  else
    {
      /* Native RGB565 input must bypass the YUV converter. */
      val |= LCD_CAM_CAM_CONV_BYPASS_M;
    }

  cam_dvp_putreg(LCD_CAM_CAM_RGB_YUV_REG, val);
}
#endif

/* Per-channel GDMA register accessor (register sets are 0xC8 apart). */

static uint32_t cam_gdma_reg(uint32_t base_ch0, int ch)
{
  return base_ch0 + (uint32_t)ch * GDMA_REG_OFFSET;
}

/****************************************************************************
 * Name: cam_dvp_gpio_config
 *
 * CoreS3 supplies the sensor clock on the camera module, so this function
 * only routes DVP data and sync pins.
 ****************************************************************************/

static void cam_dvp_gpio_config(void)
{
  /* DVP data pins -> CAM_DATA_IN0..IN7.
   *
   * GC0308 D0..D7 must map to CAM_DATA_IN0..IN7 in order.
   */

  /* Sensor D0 -> CAM_DATA_IN0_IDX (bit 0) */

  esp32s3_configgpio(CAM_DVP_D0_PIN, INPUT);
  esp32s3_gpio_matrix_in(CAM_DVP_D0_PIN, 133, 0);

  /* Sensor D1 -> CAM_DATA_IN1_IDX (bit 1) */

  esp32s3_configgpio(CAM_DVP_D1_PIN, INPUT);
  esp32s3_gpio_matrix_in(CAM_DVP_D1_PIN, 134, 0);

  /* Sensor D2 -> CAM_DATA_IN2_IDX (bit 2) */

  esp32s3_configgpio(CAM_DVP_D2_PIN, INPUT);
  esp32s3_gpio_matrix_in(CAM_DVP_D2_PIN, 135, 0);

  /* Sensor D3 -> CAM_DATA_IN3_IDX (bit 3) */

  esp32s3_configgpio(CAM_DVP_D3_PIN, INPUT);
  esp32s3_gpio_matrix_in(CAM_DVP_D3_PIN, 136, 0);

  /* Sensor D4 -> CAM_DATA_IN4_IDX (bit 4) */

  esp32s3_configgpio(CAM_DVP_D4_PIN, INPUT);
  esp32s3_gpio_matrix_in(CAM_DVP_D4_PIN, 137, 0);

  /* Sensor D5 -> CAM_DATA_IN5_IDX (bit 5) */

  esp32s3_configgpio(CAM_DVP_D5_PIN, INPUT);
  esp32s3_gpio_matrix_in(CAM_DVP_D5_PIN, 138, 0);

  /* Sensor D6 -> CAM_DATA_IN6_IDX (bit 6) */

  esp32s3_configgpio(CAM_DVP_D6_PIN, INPUT);
  esp32s3_gpio_matrix_in(CAM_DVP_D6_PIN, 139, 0);

  /* Sensor D7 -> CAM_DATA_IN7_IDX (bit 7) */

  esp32s3_configgpio(CAM_DVP_D7_PIN, INPUT);
  esp32s3_gpio_matrix_in(CAM_DVP_D7_PIN, 140, 0);

  esp32s3_configgpio(CAM_DVP_PCLK_PIN, INPUT);
  esp32s3_gpio_matrix_in(CAM_DVP_PCLK_PIN, SIG_CAM_PCLK, 0);

  esp32s3_configgpio(CAM_DVP_HREF_PIN, INPUT);
  esp32s3_gpio_matrix_in(CAM_DVP_HREF_PIN, SIG_CAM_H_ENABLE, 0);

  esp32s3_configgpio(CAM_DVP_VSYNC_PIN, INPUT);
  esp32s3_gpio_matrix_in(CAM_DVP_VSYNC_PIN, SIG_CAM_V_SYNC,
                         CAM_DVP_VSYNC_INVERT);

  lcdinfo("DVP pins routed to LCD_CAM CAM input\n");
}

/****************************************************************************
 * Name: cam_dvp_hw_init
 ****************************************************************************/

static void cam_dvp_hw_init(void)
{
  uint32_t regval;

  /* Enable LCD_CAM peripheral clock DIRECTLY via SYSTEM register. This
   * board's LCD uses SPI (CONFIG_ESP32S3_LCD=n) so esp32s3_lcd.c is not
   * compiled and nobody enables the LCD_CAM clock. We bypass
   * periph_module_enable() (its spinlock path is suspect in this build) and
   * set the clock-enable + clear-reset bits directly.
   */

  modifyreg32(SYSTEM_PERIP_CLK_EN1_REG, 0, SYSTEM_LCD_CAM_CLK_EN);

  /* Match cam_ll_reset_register(): assert and release the peripheral reset.
   * Merely forcing the reset bit low preserves stale CAM/JDMA state. */
  modifyreg32(SYSTEM_PERIP_RST_EN1_REG, 0, SYSTEM_LCD_CAM_RST);
  up_udelay(1);
  modifyreg32(SYSTEM_PERIP_RST_EN1_REG, SYSTEM_LCD_CAM_RST, 0);

  /* Whole-register reset, exactly like IDF ll_cam_config():
   * LCD_CAM.cam_ctrl.val = 0; LCD_CAM.cam_ctrl1.val = 0;
   * LCD_CAM.cam_rgb_yuv.val = 0; This avoids stale bits (e.g. cam_start,
   * bytelen) left by previous read-modify-write passes.
   */

  cam_dvp_putreg(LCD_CAM_CAM_CTRL_REG, 0);
  cam_dvp_putreg(LCD_CAM_CAM_CTRL1_REG, 0);
#if CAM_DVP_EXTERNAL_XTAL
  cam_dvp_m5_converter_config();
#else
  cam_dvp_putreg(LCD_CAM_CAM_RGB_YUV_REG, 0);
#endif

  regval = 0;

#if CAM_DVP_EXTERNAL_XTAL
  /* ESP-IDF 5.5.1 CAM_CLK_SRC_DEFAULT is PLL_D2, which maps to
   * cam_clk_sel=2 in cam_ll_select_clk_src().  External XTAL only disables
   * the output-clock divider; it does not clear the controller clock source. */
  regval |= (2 << LCD_CAM_CAM_CLK_SEL_S);
#else
  /* Original BOX path is unchanged: PLL160M / 8 supplies 20 MHz XCLK. */
  regval |= (8 << LCD_CAM_CAM_CLKM_DIV_NUM_S);
  regval |= (3 << LCD_CAM_CAM_CLK_SEL_S);
#endif

#if !CAM_DVP_EXTERNAL_XTAL
  regval |= (4 << LCD_CAM_CAM_VSYNC_FILTER_THRES_S);
#endif
  /* RGB565 uses VSYNC as the frame boundary.  JPEG must instead accumulate
   * fixed-size DMA nodes and stop from software when the EOI marker arrives;
   * setting vs_eof=1 for JPEG cuts the compressed byte stream at VSYNC.
   */

  if (!g_cam_jpeg_mode)
    {
      regval |= LCD_CAM_CAM_VS_EOF_EN_M;
    }
  /* cam_byte_order=0: official rejects byte swap for 8-bit data ("byte swap
   * is not supported when cam_data_width is 8")
   */

  cam_dvp_putreg(LCD_CAM_CAM_CTRL_REG, regval);

  /* cam_ctrl1 (IDF order):
   *   cam_rec_data_bytelen = 0 (VSYNC is the frame EOF source)
   *   cam_line_int_num = 0
   *   cam_clk_inv = 0 (PCLK not inverted)
   *   cam_vsync_filter_en = 0
   *   cam_2byte_en = 0 (8-bit data)
   *   cam_de_inv / hsync_inv / vsync_inv = 0
   *   cam_vh_de_mode_en = 0
   */

  regval = 0;

  /* ESP-IDF cam_hal_init() leaves cam_rec_data_bytelen at zero and relies on
   * VSYNC-generated EOF. A nonzero receive length is an alternate EOF source
   * and must not be mixed with VSYNC-generated EOF.
   */

  /* 2026-09-27 (v54d): only gate the receiver on VSYNC in RGB565 mode.
   * OV3660's VSYNC is dead/erratic in JPEG mode (0 edges in 300 ms diag
   * windows while HREF bursts normally); with the filter enabled the
   * receiver discards ALL data until VSYNC passes the filter -> GDMA
   * writes zero bytes -> chunks=0 captures.  The JPEG capture algorithm
   * is vsync-free (SOI..EOI pair scan), so the filter buys nothing. */

  cam_dvp_putreg(LCD_CAM_CAM_CTRL1_REG, regval);

  printf("[Cam-DVP] CAM hw configured (8-bit DVP, %s, "
         "vs_eof=%d, bytelen=%d)\n",
         CAM_DVP_EXTERNAL_XTAL ? "external XTAL" : "BOX PLL160M/8",
         g_cam_jpeg_mode ? 0 : 1,
         g_cam_jpeg_mode ? CAM_JPEG_CHUNK - 1 : 0);
}

/* Configure one complete RX transaction for `half`, exactly like
 * esp_cam_ctlr_dvp_dma_start(): rebuild a single descriptor chain over the
 * target frame buffer and write it back before GDMA starts.
 */

static void cam_dvp_setup_rgb_chain(int half)
{
  struct esp32s3_cam_s *priv = &g_cam;
  uint8_t *buffer = g_cam_rxbuf + (size_t)half * g_frame_size;
  uint32_t nd = 0;
#ifdef CONFIG_ARCH_BOARD_M5STACK_CORES3
  struct esp32s3_dmadesc_s *chain = priv->dmadesc_rgb[half];
#else
  struct esp32s3_dmadesc_s *chain = priv->dmadesc;
#endif

  esp32s3_dma_setup(chain, CAM_RGB_DESC_NUM, buffer,
                    g_frame_size, false, priv->dma_channel);

  while (nd + 1 < CAM_RGB_DESC_NUM && chain[nd].next != NULL)
    {
      nd++;
    }

  g_rgb_ndesc = nd + 1;
  rom_cache_writeback_addr((uint32_t)chain,
                           g_rgb_ndesc * sizeof(chain[0]));
}

/****************************************************************************
 * Name: cam_dvp_prepare_dma
 *
 * Description:
 *   Bind the current receive mode to the GDMA descriptor chain.
 *
 *   RGB/YUV follows esp_cam_ctlr_dvp: one single-chain transaction per
 *   frame buffer.  JPEG keeps 1024-byte nodes and software EOI scanning.
 ****************************************************************************/

static void cam_dvp_prepare_dma(void)
{
  struct esp32s3_cam_s *priv = &g_cam;
  uint32_t i;

  if (!g_cam_jpeg_mode)
    {
      cam_dvp_setup_rgb_chain(0);
#ifdef CONFIG_ARCH_BOARD_M5STACK_CORES3
      cam_dvp_setup_rgb_chain(1);
#endif
      CAM_LOG("[Cam-DVP] RGB frame chain: %u desc for %uB\n",
              (unsigned)g_rgb_ndesc, (unsigned)g_frame_size);

      return;
    }

  for (i = 0; i < CAM_JPEG_DESC_NUM; i++)
    {
      uint32_t offset = i * CAM_JPEG_CHUNK;
      uint32_t remain = CAM_RX_BUF_SIZE - offset;
      uint32_t len = remain > CAM_JPEG_CHUNK ? CAM_JPEG_CHUNK : remain;

      priv->dmadesc[i].ctrl = ESP32S3_DMA_CTRL_OWN |
                              (len << ESP32S3_DMA_CTRL_BUFLEN_S);
      priv->dmadesc[i].pbuf = g_cam_rxbuf + offset;
      priv->dmadesc[i].next = (i + 1 < CAM_JPEG_DESC_NUM) ?
                              &priv->dmadesc[i + 1] : NULL;
    }

  /* Descriptor chain lives in cached DRAM (g_cam BSS).  Without a
   * writeback the GDMA reads STALE lines (zeros from boot) -> OWN=0 ->
   * channel never fetches -> chunks=0 forever on the cold first capture.
   * Once the JPEG scan/SD traffic later sweeps the cache, the real
   * descriptors leak out and captures mysteriously start working -
   * exactly the "second session always works" pattern seen 2026-09-26. */

  rom_cache_writeback_addr((uint32_t)priv->dmadesc,
                           sizeof(priv->dmadesc));

  CAM_LOG("[Cam-DVP] JPEG DMA chain: %u desc x %uB\n",
          CAM_JPEG_DESC_NUM, CAM_JPEG_CHUNK);
}

/****************************************************************************
 * Name: cam_dvp_dma_init
 ****************************************************************************/

static int cam_dvp_dma_init(void)
{
  struct esp32s3_cam_s *priv = &g_cam;

  /* burst=true -> esp32s3_dma_setup() uses ESP32S3_DMA_BUFLEN_MAX_4B_ALIGNED
   * (4092) per descriptor, exactly matching LCD_CAM_DMA_NODE_BUFFER_MAX_SIZE
   * in IDF's esp32-camera. With burst=false the descriptor would be 4095
   * bytes while bytelen is 4091 -> EOF lands 3 bytes early, corrupting the
   * stream.
   */

  /* RX 优先级拉满：LCD TX 是 10，相机 RX 原来是 1，预览抓帧期间
   * LCD 刷屏/WiFi 突发会饿死相机 GDMA -> AFIFO 溢出 -> JPEG 中段丢
   * 字节（~50% 坏帧的根因，2026-09-28 静模式 3/3 完好证实）。 */
  priv->dma_channel = esp32s3_dma_request(ESP32S3_DMA_PERIPH_LCDCAM,
                                          10, 15, true);
  if (priv->dma_channel < 0)
    {
      printf("[Cam-DVP] GDMA RX alloc FAILED\n");
      return -ENODEV;
    }

  printf("[Cam-DVP] dma_channel=%d\n", priv->dma_channel);

  /* Match IDF gdma_config_transfer()/gdma_apply_strategy() for a PSRAM RX
   * transaction: 64-byte external-memory blocks and descriptor owner checks.
   * The 16-byte default causes AFIFO underrun at CoreS3 frame rates and leaves
   * whole descriptor tails unwritten (observed as horizontal colour bands).
   */
  esp32s3_dma_set_ext_memblk(priv->dma_channel, false,
                             ESP32S3_DMA_EXT_MEMBLK_64B);
#ifndef CONFIG_ARCH_BOARD_M5STACK_CORES3
  modifyreg32(cam_gdma_reg(DMA_IN_CONF1_CH0_REG, priv->dma_channel), 0,
              DMA_IN_CHECK_OWNER_CH0_M);
#endif

#ifdef CONFIG_ESP32S3_SPIRAM_COMMON_HEAP
  g_cam_rxbuf = esp32s3_psram_static_alloc(CAM_RX_BUF_SIZE);
  g_cam_shadow = esp32s3_psram_static_alloc(CAM_MAX_SIZE);
#else
  static uint8_t rxbuf_fallback[CAM_MAX_SIZE]
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
      rom_cache_writeback_addr((uint32_t)g_cam_rxbuf, CAM_RX_BUF_SIZE);
      /* Drop the write-back-only zero lines as well.  If they remain dirty,
       * a later eviction can overwrite a JPEG that GDMA has just written into
       * the same PSRAM range. */

      cache_invalidate_addr((uint32_t)g_cam_rxbuf, CAM_RX_BUF_SIZE);
    }

  printf("[Cam-DVP] dma_setup call (%s)...\n",
         g_cam_jpeg_mode ? "JPEG 1024B nodes" : "RGB565 line-aligned nodes");
  cam_dvp_prepare_dma();

  /* Linear chain (official dvp_spi_lcd + esp_cam_ctlr_dvp): the GDMA EOF ISR
   * re-loads the chain at every frame boundary (per-frame start), so the DMA
   * always captures exactly one frame per VSYNC window.
   */

  printf("[Cam-DVP] dma_setup done (%s)\n",
         g_cam_jpeg_mode ? "JPEG node counters" :
                           "RGB565 VSYNC frame chain");

  return OK;
}

/****************************************************************************
 * Name: cam_dvp_gdma_isr
 *
 * GDMA RX channel interrupt: in_suc_eof fires once per frame (vs_eof=1,
 * VSYNC-driven). ISR does NOT touch NuttX semaphores/syslog (IRAM-safe):
 * it increments a volatile counter and RE-ARMS the DMA chain at the frame
 * boundary, exactly like the official driver's per-frame start_trans().
 *
 * Re-arm is DMA-only (stop -> in_rst -> reload link -> start), inline
 * register writes so the ISR stays IRAM-safe. We deliberately do NOT
 * pulse cam_reset / cam_afifo_reset here: cam_reset would clear the
 * LCD_CAM INT_ENA (debug log 2.29) and afifo_reset would drop FIFO
 * continuity; the CAM peripheral keeps streaming on its own once started.
 *
 * This per-frame re-arm is the key fix for the "garbled colors" (garbled
 * colors):
 * previously the chain was only re-armed from capture() at an arbitrary
 * phase, so the DMA window [arm -> next VSYNC] was a partial frame that
 * changed every capture. Re-arming at every VSYNC EOF makes each window
 * exactly one frame, aligned to the frame start.
 ****************************************************************************/

#ifdef CONFIG_ARCH_BOARD_M5STACK_CORES3
static void IRAM_ATTR cam_dvp_m5_start_frame(uint8_t half);
#endif

static int IRAM_ATTR cam_dvp_gdma_isr(int irq, void *context, void *arg)
{
  struct esp32s3_cam_s *priv = &g_cam;
  int ch = priv->dma_channel;
  uint32_t st;

  st = cam_dvp_getreg(cam_gdma_reg(DMA_IN_INT_ST_CH0_REG, ch));
  if (st != 0)
    {
      cam_dvp_putreg(cam_gdma_reg(DMA_IN_INT_CLR_CH0_REG, ch), st);
    }

  if (st & DMA_IN_SUC_EOF_CH0_INT_ST_M)
    {
      if (g_cam_jpeg_mode)
        {
          /* JPEG uses data-driven EOF: each 1024-byte node completes once.
           * GDMA advances to the next owned descriptor automatically.
           */

          priv->jpeg_chunks++;
          return 0;
        }

      {
#ifdef CONFIG_ARCH_BOARD_M5STACK_CORES3
        if (g_m5_official_mode)
          {
            uint8_t finished = g_m5_half_active;

            g_m5_half_ready = finished;
            g_m5_half_active = finished ^ 1u;
            g_m5_half_done++;
            return 0;
          }
#endif

        uint8_t finished = g_active_half;
        uint8_t next = finished ^ 1;
        uint32_t got = 0;
        uint32_t di;

        /* Official transaction handoff: the completed buffer is published
         * first, then the channel is stopped, reset, and started on the
         * peer buffer.  Do not merely rewrite LINK/START on the old chain.
         */

#ifdef CONFIG_ARCH_BOARD_M5STACK_CORES3
        for (di = 0; di < g_rgb_ndesc; di++)
          {
            got += (priv->dmadesc_rgb[finished][di].ctrl >>
                    ESP32S3_DMA_CTRL_DATALEN_S) &
                   ESP32S3_DMA_CTRL_DATALEN_V;
          }
#else
        for (di = 0; di < g_rgb_ndesc; di++)
          {
            got += (priv->dmadesc[di].ctrl >>
                    ESP32S3_DMA_CTRL_DATALEN_S) &
                   ESP32S3_DMA_CTRL_DATALEN_V;
          }
#endif

        g_ready_half = finished;
        g_active_half = next;
        g_last_frame = g_cam_rxbuf + (size_t)finished * g_frame_size;
        g_frame_bytes[finished] = got ? got : g_frame_size;

        if (!g_stream_mode)
          {
            uint32_t regval;

            /* Match the official per-transaction model: a completed frame
             * belongs to the caller, so stop both the CAM receiver and the
             * GDMA until the next buffer is explicitly requested. */

            regval = cam_dvp_getreg(LCD_CAM_CAM_CTRL1_REG);
            regval &= ~LCD_CAM_CAM_START_M;
            cam_dvp_putreg(LCD_CAM_CAM_CTRL1_REG, regval);
            esp32s3_dma_disable(ch, false);
            g_stream_armed = false;
          }
        else
          {
#ifdef CONFIG_ARCH_BOARD_M5STACK_CORES3
            cam_dvp_m5_start_frame(next);
#else
            esp32s3_dma_disable(ch, false);
            cam_dvp_setup_rgb_chain(next);
            esp32s3_dma_load(priv->dmadesc, ch, false);
            esp32s3_dma_enable(ch, false);
#endif
          }

        priv->block_done++;
      }
    }

  return 0;
}

/****************************************************************************
 * Name: cam_dvp_vsync_isr
 *
 * LCD_CAM peripheral interrupt used only for VSYNC diagnostics. Frame
 * boundaries are handled by VSYNC-generated GDMA RX EOF in the GDMA ISR.
 ****************************************************************************/

#ifdef CONFIG_ARCH_BOARD_M5STACK_CORES3
static void IRAM_ATTR cam_dvp_m5_start_frame(uint8_t half)
{
  struct esp32s3_cam_s *priv = &g_cam;
  struct esp32s3_dmadesc_s *chain = priv->dmadesc_rgb[half];
  uint32_t offset = 0;
  uint32_t nd = 0;

  /* Match esp_cam_ctlr_dvp_start_trans(): reset and restart only the GDMA
   * transaction with the peer frame buffer. The CAM receiver is continuous;
   * stopping/resetting it here changes the HREF/data phase and produces
   * horizontal stripe corruption.
   */

  while (offset < g_frame_size && nd < CAM_RGB_DESC_NUM)
    {
      uint32_t data_len = g_frame_size - offset;
      uint32_t buf_len;

      if (data_len > CAM_M5_RX_NODE_BYTES)
        {
          data_len = CAM_M5_RX_NODE_BYTES;
        }

      /* PSRAM GDMA descriptors must end on the configured 64-byte block. */

      buf_len = (data_len + 63u) & ~63u;
      chain[nd].ctrl = ESP32S3_DMA_CTRL_OWN |
                       (buf_len << ESP32S3_DMA_CTRL_BUFLEN_S);
      chain[nd].pbuf = g_cam_rxbuf + (size_t)half * g_frame_size + offset;
      chain[nd].next = &chain[nd + 1];
      offset += data_len;
      nd++;
    }

  chain[nd - 1].next = NULL;
  g_rgb_ndesc = nd;
  rom_cache_writeback_addr((uint32_t)chain, nd * sizeof(chain[0]));

  esp32s3_dma_disable(priv->dma_channel, false);
  esp32s3_dma_load(chain, priv->dma_channel, false);
  esp32s3_dma_enable(priv->dma_channel, false);
  g_active_half = half;
}
#endif

static int IRAM_ATTR cam_dvp_vsync_isr(int irq, void *context, void *arg)
{
  struct esp32s3_cam_s *priv = &g_cam;
  uint32_t st;

  st = cam_dvp_getreg(LCD_CAM_LC_DMA_INT_ST_REG);
  if (st & LCD_CAM_CAM_VSYNC_INT_ST_M)
    {
      cam_dvp_putreg(LCD_CAM_LC_DMA_INT_CLR_REG,
                     LCD_CAM_CAM_VSYNC_INT_ST_M);
      priv->vsync_chunks[priv->vsync_marks & 7u] = priv->jpeg_chunks;
      priv->vsync_marks++;
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

  /* 3. Select the EOF source.  Match IDF cam_hal_init(): the receive byte
   * limit stays zero and VSYNC is the only frame boundary.  Programming
   * 3839 here cuts the stream every 3840 bytes and produces horizontal
   * colour bands. */

  regval = cam_dvp_getreg(LCD_CAM_CAM_CTRL_REG);
  regval &= ~LCD_CAM_CAM_VS_EOF_EN_M;
  if (!g_cam_jpeg_mode)
    {
      regval |= LCD_CAM_CAM_VS_EOF_EN_M;
    }

  cam_dvp_putreg(LCD_CAM_CAM_CTRL_REG, regval);

  regval = cam_dvp_getreg(LCD_CAM_CAM_CTRL1_REG);
  regval &= ~LCD_CAM_CAM_REC_DATA_BYTELEN_M;
  cam_dvp_putreg(LCD_CAM_CAM_CTRL1_REG, regval);

  /* 4. Load chain (in_rst pulse + link addr) and start DMA (link start) */

  cam_dvp_prepare_dma();
  g_active_half = 0;
  g_ready_half = 0;
  g_frame_bytes[0] = g_frame_size;
  g_frame_bytes[1] = g_frame_size;
#ifdef CONFIG_ARCH_BOARD_M5STACK_CORES3
  priv->frame_pending = 0;
  esp32s3_dma_load(priv->dmadesc_rgb[0], priv->dma_channel, false);
#else
  esp32s3_dma_load(priv->dmadesc, priv->dma_channel, false);
#endif
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

uint32_t esp32s3_cam_dvp_get_blockdone(void)
{
  return g_cam.block_done;
}

uint32_t esp32s3_cam_dvp_get_vsync(void)
{
  return g_cam.vsync_count;
}

void esp32s3_cam_dvp_set_yuv_input(bool on)
{
  g_cam_yuv_input = on;
}

int esp32s3_cam_dvp_init(void)
{
  struct esp32s3_cam_s *priv = &g_cam;
  uint32_t regval;
  int ret;

  /* Idempotent: only initialize once. Repeated calls (e.g. each
   * camera_capture_frame) must NOT re-request DMA channels or re-memalign
   * the frame buffer - that leaks channels/memory.
   */

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

  /* GDMA RX channel interrupt only (peripheral IRQ 66 + chan). LCD_CAM
   * peripheral IRQ is NOT used -> no conflict with LCD driver. Must map the
   * peripheral IRQ (esp32s3_setup_irq) BEFORE irq_attach + up_enable_irq,
   * otherwise g_irqmap[irq] is IRQ_UNMAPPED and up_enable_irq writes a
   * garbage CPUINT -> hangs.
   */

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
   * free.
   */

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

int esp32s3_cam_dvp_reinit_hw(void)
{
  struct esp32s3_cam_s *priv = &g_cam;

  if (!priv->initialized)
    {
      return esp32s3_cam_dvp_init();
    }

  /* Cold-boot recovery (2026-09-27, v54): re-run the full hardware
   * configuration (GPIO matrix, LCD_CAM peripheral reset+config, GDMA
   * descriptor chain rebuild) WITHOUT re-requesting the DMA channel or
   * re-attaching interrupts.  The POR-first JPEG capture delivers zero
   * DMA bytes (chunks=0, vsync dead); the only verified recovery is the
   * CLI cold-boot order: sensor streams RGB565 first, then JPEG config,
   * then a full hw config run (what a first-ever dvp_init does there).
   */

  esp32s3_cam_dvp_stop();
  cam_dvp_gpio_config();
  cam_dvp_hw_init();
  cam_dvp_prepare_dma();
  cam_dvp_enable_ints();

  printf("[Cam-DVP] hw reinit done\n");

  return OK;
}

int esp32s3_cam_dvp_start(void)
{
  struct esp32s3_cam_s *priv = &g_cam;

  /* 2026-09-18: Before each stream start, reclaim DVP pins from UART0.
   *
   * When leaving capture page, apps side calls esp32s3_uart0_reclaim_pins()
   * to reclaim IO42(TX)/IO40(RX) returned to UART0 (needed for soil sensor),
   * and esp32s3_cam_dvp_init() carries the initialized guard, won't run
   * cam_dvp_gpio_config() again when entering the photo page the second
   * time. As a result: the second time and Previously, entering the photo
   * capture page, VSYNC(IO42) was constantly held by UART0's TX, preventing
   * detection of frame boundaries, capture waits 10-second timeout, preview
   * thread exits, viewfinder goes blank, only rebooting board can
   * restoration (user feedback: "works initially after restart, fails after
   * running for a while").
   *
   * GPIO matrix is last-writer-wins, rerouting once here is sufficient; only
   * touches pins, Does not disturb DMA or interrupts, can be safely called
   * repeatedly.
   */

  cam_dvp_gpio_config();

#ifdef CONFIG_ARCH_BOARD_M5STACK_CORES3
  /* Format can be changed between preview sessions.  Re-apply YUV->RGB or
   * native-RGB bypass before the receiver is re-armed. */
  cam_dvp_m5_converter_config();
#endif

  if (priv->started)
    {
      return OK;
    }

#ifdef CONFIG_ARCH_BOARD_M5STACK_CORES3
  if (g_m5_official_sensor && !g_cam_jpeg_mode)
    {
      if (g_m5_worker_started)
        {
          priv->started = true;
          return OK;
        }

      g_m5_frame_done = 0;
      g_m5_frame_ready = 0;
      if (cam_dvp_m5_start_chunked() < 0)
        {
          return -EIO;
        }

      g_m5_worker_run = true;
      if (pthread_create(&g_m5_worker, NULL,
                         cam_dvp_m5_worker_thread, NULL) != 0)
        {
          g_m5_worker_run = false;
          cam_dvp_m5_stop_chunked();
          return -EIO;
        }

      g_m5_worker_started = true;
      priv->started = true;
      CAM_LOG("[Cam-DVP] official chunk worker started\n");
      return OK;
    }
#endif

  /* arm the DMA chain + CAM once (full start sequence); cam_reset inside arm
   * clears the INT_ENAs, so re-enable both interrupts afterwards
   */

  cam_dvp_arm();
  cam_dvp_enable_ints();

  priv->started = true;
  CAM_LOG("[Cam-DVP] capture started\n");

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

#ifdef CONFIG_ARCH_BOARD_M5STACK_CORES3
  if (g_m5_official_mode)
    {
      if (g_m5_worker_started)
        {
          g_m5_worker_run = false;
          pthread_join(g_m5_worker, NULL);
          g_m5_worker_started = false;
        }

      cam_dvp_m5_stop_chunked();
      priv->started = false;
      return OK;
    }
#endif

  /* Mask the GDMA EOF interrupt first: the ISR re-arms the chain at every
   * frame boundary and would otherwise restart a disabled DMA.
   */

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

  /* Stop VSYNC marks too.  They are capture-session counters; leaving the
   * interrupt enabled pollutes the next JPEG window with stale chunk counts.
   */

  regval = cam_dvp_getreg(LCD_CAM_LC_DMA_INT_ENA_REG);
  regval &= ~LCD_CAM_CAM_VSYNC_INT_ENA_M;
  cam_dvp_putreg(LCD_CAM_LC_DMA_INT_ENA_REG, regval);

  lcdinfo("cam_dvp: capture stopped\n");

  return OK;
}

/****************************************************************************
 * Name: cam_dvp_capture_jpeg
 *
 * Description:
 *   Capture a complete JPEG byte stream from the camera.
 *
 *   With cam_vs_eof_en=0 the CAM peripheral raises in_suc_eof after every
 *   4092 received bytes.  The ISR increments jpeg_chunks, while this task
 *   scans the newly-written portion for FF D8 (SOI) and FF D9 (EOI).  DMA is
 *   stopped as soon as EOI appears, which freezes the frame before the next
 *   JPEG can overwrite it.
 ****************************************************************************/

static int cam_dvp_capture_jpeg(void (*sink)(const uint8_t *data,
                                             size_t len, void *arg),
                                void *sink_arg,
                                volatile bool *eoi_out)
{
  struct esp32s3_cam_s *priv = &g_cam;
  uint32_t chunks;
  uint32_t valid = 0;
  uint32_t scanned = 0;
  uint32_t scan_pos = 0;
  uint32_t soi = UINT32_MAX;
  uint32_t eoi = 0;
  uint32_t loop;

  priv->jpeg_chunks = 0;

  CAM_LOG("[Cam-DVP] JPEG capture enter: chunks=%u\n",
          (unsigned)priv->jpeg_chunks);

  /* 3 s is already >10 frame periods at 1024x768.  The old 20 s loop made a
   * single missed frame look like a frozen camera and blocked the preview
   * thread for far too long. */

  for (loop = 0; loop < 3000; loop++)
    {
      chunks = priv->jpeg_chunks;
      valid = chunks > (CAM_RX_BUF_SIZE / CAM_JPEG_CHUNK) ?
              CAM_RX_BUF_SIZE : chunks * CAM_JPEG_CHUNK;

      if (valid > scanned)
        {
          uint32_t i;

          cam_dvp_sync_jpeg_range(scanned, valid);
          i = scan_pos;

          if (soi == UINT32_MAX)
            {
              for (; i + 1 < valid; i++)
                {
                  if (g_cam_rxbuf[i] == 0xff && g_cam_rxbuf[i + 1] == 0xd8)
                    {
                      soi = i;
                      i += 2;
                      break;
                    }
                }
            }

          if (soi != UINT32_MAX)
            {
              if (i < soi + 2)
                {
                  i = soi + 2;
                }

              for (; i + 1 < valid; i++)
                {
                  if (g_cam_rxbuf[i] == 0xff && g_cam_rxbuf[i + 1] == 0xd9)
                    {
                      eoi = i + 2;
                      break;
                    }
                }
            }

          scan_pos = valid > 0 ? valid - 1 : 0;
          scanned = valid;

          if (eoi > soi && (eoi - soi) > 256)
            {
              size_t len = eoi - soi;

              esp32s3_dma_disable(priv->dma_channel, false);
              g_stream_armed = false;
              cam_dvp_sync_jpeg_range(soi, eoi);

              if (g_cam_rxbuf[soi] != 0xff || g_cam_rxbuf[soi + 1] != 0xd8 ||
                  g_cam_rxbuf[eoi - 2] != 0xff ||
                  g_cam_rxbuf[eoi - 1] != 0xd9)
                {
                  CAM_LOG("[Cam-DVP] JPEG marker verification failed\n");
                  return -EIO;
                }

              g_last_frame = g_cam_rxbuf + soi;
              if (sink != NULL)
                {
                  sink(g_last_frame, len, sink_arg);
                }

              if (eoi_out != NULL)
                {
                  *eoi_out = true;
                }

              CAM_LOG("[Cam-DVP] JPEG frame done: SOI=%u EOI=%u len=%u "
                      "chunks=%u\n",
                      (unsigned)soi, (unsigned)eoi, (unsigned)len,
                      (unsigned)priv->jpeg_chunks);
              return (int)len;
            }
        }

      if (valid >= CAM_RX_BUF_SIZE)
        {
          CAM_LOG("[Cam-DVP] JPEG buffer full without complete frame: "
                  "SOI=%u chunks=%u\n",
                  (unsigned)soi, (unsigned)priv->jpeg_chunks);
          return -ENODATA;
        }

      nxsig_usleep(1000);
    }

  CAM_LOG("[Cam-DVP] JPEG capture timeout: chunks=%u SOI=%u\n",
          (unsigned)priv->jpeg_chunks, (unsigned)soi);
  return -ETIMEDOUT;
}

/* Stable JPEG capture path: let the sensor produce whole VSYNC-delimited
 * frames into the PSRAM chain without touching the buffer from the CPU, then
 * stop DMA, invalidate the cache and scan the now-static bytes for SOI/EOI.
 *
 * The old live-scan path raced the DMA and the PSRAM cache: it could see a
 * transient ``FFD9`` and return a mixed/stale tail.  Waiting for three VSYNCs
 * guarantees at least one complete frame even when capture starts mid-frame.
 */

static int cam_dvp_capture_jpeg_stable(void (*sink)(const uint8_t *data,
                                                    size_t len, void *arg),
                                       void *sink_arg,
                                       volatile bool *eoi_out)
{
  struct esp32s3_cam_s *priv = &g_cam;
  uint32_t chunks;
  uint32_t valid;
  uint32_t i;
  uint32_t soi = UINT32_MAX;
  uint32_t eoi = 0;
  uint32_t last_chunks = UINT32_MAX;
  uint32_t last_scanned = 0;
  int quiet_polls = 0;
  int poll;
  bool have_pair = false;

  priv->jpeg_chunks = 0;
  priv->vsync_marks = 0;

  /* Vsync-independent rolling capture (2026-09-26, v52).
   *
   * The LCD_CAM cam_vsync interrupt proved unreliable in JPEG modes
   * (fires ~once per 20 frames; pin routing, INT_ENA and GPIO matrix all
   * verified OK in diag L7 - root cause not isolated).  The old
   * "wait 2 vsync marks + vsync window" logic then filled the 1 MiB
   * chain to the last descriptor, the DMA stopped, and the stale window
   * [c0, c1] rejected every SOI..EOI pair -> -ENODATA "no EOI" on both
   * the UI preview and the CLI single-shot.
   *
   * A complete SOI..EOI pair in the byte stream IS one whole frame by
   * JPEG semantics (the sensor emits FFD8..FFD9 atomically per frame),
   * so vsync marks are not needed at all: poll until one complete pair
   * sits comfortably behind the DMA write pointer, then freeze + scan.
   */

  for (poll = 0; poll < 350; poll++)   /* 350 x 10 ms = 3.5 s max */
    {
      nxsig_usleep(10000);

      chunks = priv->jpeg_chunks;
      if (chunks == last_chunks)
        {
          quiet_polls++;

          /* 2026-09-26: OV3660 JPEG output is bursty (HREF-gated bursts,
           * ~1 burst/8.5 ms observed, frame period up to ~1 s with long
           * AEC exposure).  200 ms of no data proved NOTHING - the first
           * two attempts after arm got 0 bytes and attempt 3 pulled a
           * full 46 KB frame.  Only give up after 1 s of true silence. */

          if (chunks < 4 && quiet_polls > 120)
            {
              break;    /* 1.2 s with zero bytes: sensor/DMA path dead */
            }
        }
      else
        {
          quiet_polls = 0;
          last_chunks = chunks;
        }

      if (chunks < 8)
        {
          continue;
        }

      /* Rescan only when >= 8 new nodes landed (or stream went quiet). */

      if ((uint32_t)(chunks - last_scanned) < 8 && quiet_polls <= 10)
        {
          continue;
        }

      last_scanned = chunks;

      valid = chunks > (CAM_RX_BUF_SIZE / CAM_JPEG_CHUNK) ?
              CAM_RX_BUF_SIZE : chunks * CAM_JPEG_CHUNK;

      cam_dvp_sync_jpeg_range(0, valid);

      /* Keep the LAST complete SOI..EOI pair (the freshest full frame). */

      i = 0;
      soi = UINT32_MAX;
      eoi = 0;
      while (i + 1 < valid)
        {
          uint32_t next_soi = UINT32_MAX;
          uint32_t next_eoi = 0;
          uint32_t k;

          for (k = i; k + 1 < valid; k++)
            {
              if (g_cam_rxbuf[k] == 0xff && g_cam_rxbuf[k + 1] == 0xd8)
                {
                  next_soi = k;
                  break;
                }
            }

          if (next_soi == UINT32_MAX)
            {
              break;
            }

          for (k = next_soi + 2; k + 1 < valid; k++)
            {
              if (g_cam_rxbuf[k] == 0xff && g_cam_rxbuf[k + 1] == 0xd9)
                {
                  next_eoi = k + 2;
                  break;
                }
            }

          if (next_eoi == 0)
            {
              break;
            }

          if ((next_eoi - next_soi) > 1024)
            {
              soi = next_soi;
              eoi = next_eoi;
            }

          i = next_eoi;
        }

      if (soi == UINT32_MAX)
        {
          continue;     /* no complete frame yet */
        }

      /* Accept when the DMA has moved >= 4 nodes past the pair (the pair
       * can no longer be disturbed), or when the stream has gone quiet
       * (chain full / sensor stalled) - the bytes are stable either way. */

      if (eoi + 4 * CAM_JPEG_CHUNK <= valid || quiet_polls > 10 ||
          chunks >= (CAM_RX_BUF_SIZE / CAM_JPEG_CHUNK))
        {
          have_pair = true;
          break;
        }
    }

  esp32s3_dma_disable(priv->dma_channel, false);
  g_stream_armed = false;

  if (!have_pair)
    {
      int ff = 0;
      uint32_t z;
      uint32_t dump = priv->jpeg_chunks * CAM_JPEG_CHUNK;

      if (dump > 4096)
        {
          dump = 4096;
        }

      if (dump > 0)
        {
          cam_dvp_sync_jpeg_range(0, dump);
          for (z = 0; z < dump; z += 64)
            {
              if (g_cam_rxbuf[z] == 0xff)
                {
                  ff++;
                }
            }
        }

      printf("[Cam-DVP] stable JPEG no complete frame: chunks=%u "
             "ff=%d head=%02x %02x %02x %02x started=%d\n",
             (unsigned)priv->jpeg_chunks, ff,
             dump > 0 ? g_cam_rxbuf[0] : 0, dump > 1 ? g_cam_rxbuf[1] : 0,
             dump > 2 ? g_cam_rxbuf[2] : 0, dump > 3 ? g_cam_rxbuf[3] : 0,
             (int)priv->started);
      return -ENODATA;
    }

  /* DMA frozen: one final sync + verify the chosen pair survived. */

  cam_dvp_sync_jpeg_range(soi, eoi);
  if (g_cam_rxbuf[soi] != 0xff || g_cam_rxbuf[soi + 1] != 0xd8 ||
      g_cam_rxbuf[eoi - 2] != 0xff || g_cam_rxbuf[eoi - 1] != 0xd9)
    {
      printf("[Cam-DVP] stable JPEG pair tore after freeze (%u..%u)\n",
             (unsigned)soi, (unsigned)eoi);
      return -ENODATA;
    }

  g_last_frame = g_cam_rxbuf + soi;
  if (sink != NULL)
    {
      sink(g_last_frame, eoi - soi, sink_arg);
    }

  if (eoi_out != NULL)
    {
      *eoi_out = true;
    }

  g_jpeg_vsync_aligned = true;

  CAM_LOG("[Cam-DVP] stable JPEG: SOI=%u EOI=%u len=%u chunks=%u\n",
          (unsigned)soi, (unsigned)eoi, (unsigned)(eoi - soi),
          (unsigned)chunks);
  return (int)(eoi - soi);
}

#ifdef CONFIG_ARCH_BOARD_M5STACK_CORES3
static void cam_dvp_m5_build_ring(void)
{
  struct esp32s3_cam_s *priv = &g_cam;
  uint32_t index = 0;
  int half;

  for (half = 0; half < 2; half++)
    {
      uint32_t offset = 0;
      int node;

      for (node = 0; node < 2; node++)
        {
          uint32_t len = node == 0 ? 4092u : 3588u;

          priv->dmadesc[index].ctrl = ESP32S3_DMA_CTRL_OWN |
            (len << ESP32S3_DMA_CTRL_BUFLEN_S);
          if (node == 1)
            {
              priv->dmadesc[index].ctrl |= ESP32S3_DMA_CTRL_EOF;
            }

          priv->dmadesc[index].pbuf =
            g_m5_dma_buf + half * CAM_M5_CHUNK_BYTES + offset;
          priv->dmadesc[index].next = &priv->dmadesc[index + 1];
          offset += len;
          index++;
        }
    }

  priv->dmadesc[index - 1].next = &priv->dmadesc[0];
  rom_cache_writeback_addr((uint32_t)priv->dmadesc,
                           index * sizeof(priv->dmadesc[0]));
}

static int cam_dvp_m5_start_chunked(void)
{
  struct esp32s3_cam_s *priv = &g_cam;
  uint32_t regval;

  g_m5_half_active = 0;
  g_m5_half_ready = 0;
  g_m5_half_done = 0;
  g_m5_official_mode = true;

  regval = cam_dvp_getreg(LCD_CAM_CAM_CTRL1_REG);
  regval &= ~LCD_CAM_CAM_START_M;
  cam_dvp_putreg(LCD_CAM_CAM_CTRL1_REG, regval);

  regval = cam_dvp_getreg(LCD_CAM_CAM_CTRL1_REG);
  regval |= LCD_CAM_CAM_RESET_M | LCD_CAM_CAM_AFIFO_RESET_M;
  cam_dvp_putreg(LCD_CAM_CAM_CTRL1_REG, regval);
  regval &= ~(LCD_CAM_CAM_RESET_M | LCD_CAM_CAM_AFIFO_RESET_M);
  cam_dvp_putreg(LCD_CAM_CAM_CTRL1_REG, regval);

  regval = cam_dvp_getreg(LCD_CAM_CAM_CTRL_REG);
  regval &= ~LCD_CAM_CAM_VS_EOF_EN_M;
  cam_dvp_putreg(LCD_CAM_CAM_CTRL_REG, regval);

  regval = cam_dvp_getreg(LCD_CAM_CAM_CTRL1_REG);
  regval &= ~LCD_CAM_CAM_REC_DATA_BYTELEN_M;
  regval |= (CAM_M5_CHUNK_BYTES - 1u) <<
            LCD_CAM_CAM_REC_DATA_BYTELEN_S;
  cam_dvp_putreg(LCD_CAM_CAM_CTRL1_REG, regval);

  cam_dvp_putreg(cam_gdma_reg(DMA_IN_INT_CLR_CH0_REG,
                              priv->dma_channel), 0xffffffffu);
  modifyreg32(cam_gdma_reg(DMA_IN_CONF1_CH0_REG, priv->dma_channel),
              DMA_IN_CHECK_OWNER_CH0_M, 0);

  cam_dvp_m5_build_ring();
  esp32s3_dma_disable(priv->dma_channel, false);
  esp32s3_dma_load(priv->dmadesc, priv->dma_channel, false);
  esp32s3_dma_enable(priv->dma_channel, false);

  regval = cam_dvp_getreg(LCD_CAM_CAM_CTRL_REG);
  regval |= LCD_CAM_CAM_UPDATE_REG_M;
  cam_dvp_putreg(LCD_CAM_CAM_CTRL_REG, regval);

  cam_dvp_enable_ints();

  regval = cam_dvp_getreg(LCD_CAM_CAM_CTRL1_REG);
  regval |= LCD_CAM_CAM_START_M;
  cam_dvp_putreg(LCD_CAM_CAM_CTRL1_REG, regval);
  return OK;
}

static void cam_dvp_m5_stop_chunked(void)
{
  struct esp32s3_cam_s *priv = &g_cam;
  uint32_t regval;

  regval = cam_dvp_getreg(LCD_CAM_CAM_CTRL1_REG);
  regval &= ~LCD_CAM_CAM_START_M;
  cam_dvp_putreg(LCD_CAM_CAM_CTRL1_REG, regval);
  esp32s3_dma_disable(priv->dma_channel, false);
  g_m5_official_mode = false;
}

static void *cam_dvp_m5_worker_thread(void *arg)
{
  struct esp32s3_cam_s *priv = &g_cam;
  uint32_t last_vsync = priv->vsync_count;
  uint32_t last_half = g_m5_half_done;
  uint32_t received = 0;
  uint8_t write_index = 0;
  bool active = false;

  (void)arg;

  while (g_m5_worker_run)
    {
      uint32_t vsync = priv->vsync_count;
      uint32_t half_done = g_m5_half_done;

      if (vsync != last_vsync)
        {
          last_vsync = vsync;
          if (active && received != g_frame_size)
            {
              write_index = (write_index + 1u) % CAM_M5_FRAME_BUFFERS;
            }

          active = true;
          received = 0;
        }

      if (half_done != last_half)
        {
          uint8_t half;

          if (!active)
            {
              active = true;
              received = 0;
            }

          half = g_m5_half_ready;
          if (received + CAM_M5_CHUNK_BYTES <= g_frame_size)
            {
              memcpy(g_cam_rxbuf + (size_t)write_index * g_frame_size +
                       received,
                     g_m5_dma_buf + half * CAM_M5_CHUNK_BYTES,
                     CAM_M5_CHUNK_BYTES);
            }

          received += CAM_M5_CHUNK_BYTES;
          last_half = half_done;

          if (received == g_frame_size)
            {
              g_m5_frame_ready = write_index;
              g_m5_frame_done++;
              write_index = (write_index + 1u) % CAM_M5_FRAME_BUFFERS;
              active = false;
              received = 0;
            }
        }

      up_udelay(50);
      sched_yield();
    }

  return NULL;
}

static int cam_dvp_m5_capture_chunked(void (*sink)(const uint8_t *data,
                                                   size_t len,
                                                   void *arg),
                                      void *sink_arg,
                                      volatile bool *eoi_out)
{
  uint32_t seen;
  uint32_t stall = 0;

  if (!g_m5_official_sensor || g_cam_jpeg_mode ||
      (g_frame_size % CAM_M5_CHUNK_BYTES) != 0)
    {
      return -EINVAL;
    }

  seen = g_m5_frame_done;
  while (g_m5_frame_done == seen)
    {
      nxsig_usleep(1000);
      if (++stall > 10000)
        {
          CAM_LOG("[Cam-DVP] chunk frame timeout\n");
          return -ETIMEDOUT;
        }
    }

  g_last_frame = g_cam_rxbuf + (size_t)g_m5_frame_ready * g_frame_size;

  if (sink != NULL)
    {
      sink(g_last_frame, g_frame_size, sink_arg);
    }

  if (eoi_out != NULL)
    {
      *eoi_out = true;
    }

  return (int)g_frame_size;
}
#endif

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

  if (g_cam_jpeg_mode)
    {
      return cam_dvp_capture_jpeg_stable(sink, sink_arg, eoi_out);
    }

#ifdef CONFIG_ARCH_BOARD_M5STACK_CORES3
  if (g_m5_official_sensor)
    {
      return cam_dvp_m5_capture_chunked(sink, sink_arg, eoi_out);
    }
#endif

  CAM_LOG("[Cam-DVP] capture enter (vs_eof=1): block_done=%u vsync=%u\n",
          priv->block_done, priv->vsync_count);

  /* Warm-up (first time only): AEC/AGC/AWB needs dozens of frames to
   * converge. DMA must keep running, During warm-up, ISR continuously
   * re-arms, rxbuf gets overwritten (we read only during frame capture).
   * warm-up only occurs during the first capture — continuous preview
   * reuses the converged state.
   */

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
                  nxsig_usleep(2000);
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
      CAM_LOG("[Cam-DVP] warmup done (%d frames, AEC converged)\n",
              CAM_WARMUP_FRAMES);
    }

  /* Preview stream mode: rolling capture + snapshot. Wait for the next EOF
   * (frame just completed), immediately rxbuf copied to shadow (and
   * byte-swapped on shadow), DMA does not stop, ISR continuous re-arming —
   * next capture requires no re-arm/realignment, only waiting for one frame
   * period; When LCD reads shadow, DMA is writing rxbuf, no interference, no
   * tearing.
   */

  if (g_stream_mode)
    {
      if (!g_stream_armed)
        {
          cam_dvp_arm();
          cam_dvp_enable_ints();
          g_stream_armed = true;

          /* The first window after arming is a half-frame: digest one EOF
           * first, then all subsequent are full frames
           */

          seen = priv->block_done;
          stall = 0;
          while (priv->block_done == seen)
            {
              nxsig_usleep(2000);
              stall++;
              if (stall > 5000)
                {
                  CAM_LOG("[Cam-DVP] stream arm align timeout\n");
                  return -ETIMEDOUT;
                }
            }
        }

      {
        int skips = 0;

        for (;;)
          {
            uint8_t half;
            const uint8_t *src;

            seen = priv->block_done;
            stall = 0;
            while (priv->block_done == seen)
              {
                up_udelay(150);
                stall++;
                if ((stall % 2000) == 0)
                  {
                    CAM_LOG("[Cam-DVP] stream frame wait... stall=%u\n",
                            stall);
                  }

                if (stall > 80000)
                  {
                    CAM_LOG("[Cam-DVP] stream frame timeout\n");
                    return -ETIMEDOUT;
                  }
                }

            /* 双缓冲：DMA 正在写另一半区，这一半在下一帧写完前保持静
             * 止，memcpy 不再有被覆盖的竞态。快照 ready_half 防二次
             * EOF 翻动。 */

            /* Let the GDMA flush the FIFO tail before sampling the
             * descriptor lengths. */

            nxsig_usleep(3000);

            half = g_ready_half;
            src = g_cam_rxbuf + half * g_frame_size;

            /* 帧完整性（v66）：AFIFO 欠载丢字节时，VSYNC EOF 到达而
             * 链未写满（尾段描述符 datalen 仍为 0），帧内容整体错位，
             * 上屏即"大片油膜一闪"。校验链 datalen 合计 == 帧字节数，
             * 不足则丢弃等下一帧（透明重试，不上报失败）。 */

#ifndef CONFIG_ARCH_BOARD_M5STACK_CORES3
            /* Descriptor LENGTH write-back is not reliable enough on the
             * CoreS3 PSRAM RX path to reject the frame; VSYNC already bounds
             * the transaction.  Keep the checked path for the BOX board. */
            if (g_rgb_ndesc > 0)
              {
                uint32_t got = g_frame_bytes[half];

                if (got < g_frame_size &&
                    got >= (uint32_t)((uint64_t)g_frame_size * 95ULL / 100ULL))
                  {
                    /* GDMA may lag the CAM EOF by a few lines.  Accept a
                     * >=95% frame and pad the missing tail so the stream
                     * keeps ~20fps instead of rejecting every frame. */

                    memset((uint8_t *)src + got, 0,
                           (size_t)(g_frame_size - got));
                    got = g_frame_size;
                  }

                if (got < g_frame_size)
                  {
                    priv->short_frames++;
                    /* v185：短帧永不显示。尾部描述符 datalen=0 时帧底
                     * 是上一帧残留 → 上屏即"底部一大宽条油膜、闪一下
                     * 消失"。旧逻辑跳 3 次后 show anyway 正是元凶。
                     * 宁可掉帧也不上坏帧；连续短帧过多则放弃本次
                     * capture，绝不把残帧交给显示。 */
                    if (++skips <= 30)
                      {
                        printf("[Cam-DVP] short frame %u<%u, skip #%d "
                               "(total %u)\n",
                               (unsigned)got, (unsigned)g_frame_size,
                               skips, (unsigned)priv->short_frames);
                        continue;
                      }

                    CAM_LOG("[Cam-DVP] short frame storm %u<%u, drop "
                            "this capture (total %u)\n",
                            (unsigned)got, (unsigned)g_frame_size,
                            (unsigned)priv->short_frames);
                    return -EIO;
                  }
              }
#endif

            cam_dvp_sync_rgb_range(src);
            if (g_stream_direct)
              {
                /* 免拷贝：直接交出就绪半帧（保留传感器原始字节序，由
                 * 消费端在采样时交换）。DMA 写另一半期间本半帧静止，
                 * 消费方须在一个帧周期内读完。CPU 不写 rxbuf，无脏行
                 * 回写风险。 */
                g_last_frame = (uint8_t *)src;
              }
            else
              {
                memcpy(g_cam_shadow, src, g_frame_size);
              }
            break;
          }
      }

      if (!g_stream_direct)
        {
          if (g_cam_byte_swap)
            {
              cam_swap_rgb565(g_cam_shadow, g_frame_size);
            }

          g_last_frame = g_cam_shadow;
        }

      if (eoi_out != NULL)
        {
          *eoi_out = true;
        }

      CAM_LOG("[Cam-DVP] stream frame done: %u bytes\n",
              (unsigned)g_frame_size);
      return (int)g_frame_size;
    }

  /* Official transaction model used by esp_cam_ctlr_dvp: the CAM/GDMA pair
   * is started at a VSYNC boundary and the completed transaction is handed
   * to the caller.  The ISR stops the receiver at EOF, so the caller owns the
   * buffer until it explicitly starts the next transaction.
   */

  cam_dvp_enable_ints();

  seen = priv->vsync_count;
  stall = 0;
  while (priv->vsync_count == seen)
    {
      nxsig_usleep(1000);
      if (++stall > 10000)
        {
          CAM_LOG("[Cam-DVP] VSYNC start timeout\n");
          return -ETIMEDOUT;
        }
    }

  seen = priv->block_done;
  cam_dvp_arm();
  cam_dvp_enable_ints();
  g_stream_armed = true;

  stall = 0;
  while (priv->block_done == seen)
    {
      nxsig_usleep(1000);
      if (++stall > 10000)
        {
          CAM_LOG("[Cam-DVP] frame EOF timeout\n");
          esp32s3_dma_disable(priv->dma_channel, false);
          g_stream_armed = false;
          return -ETIMEDOUT;
        }
    }

  g_stream_armed = false;
  g_last_frame = g_cam_rxbuf + (size_t)g_ready_half * g_frame_size;
  cam_dvp_sync_rgb_range(g_last_frame);

  cam_dvp_sync_rgb_range(g_last_frame);

  /* 5. Diagnostics on the RAW frame (before any swap):
   * [xxxx|yyyy r..g..b.. | r..g..b..] = pixel value as little-endian vs
   * swapped; the first RGB triplet is what LVGL shows WITHOUT swap (CORRECT
   * for OV3660), the second is what it shows WITH swap.
   */

  CAM_LOG("[Cam-DVP] frame done: %u bytes (swap=%s)\n",
         (unsigned)g_frame_size, g_cam_byte_swap ? "on" : "off");

  if (sink != NULL)
    {
      int k;
      const uint8_t *fp = g_last_frame;

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
   * little-endian), then hand the frame to the sink.
   */

  if (g_cam_byte_swap)
    {
      cam_swap_rgb565(g_last_frame, g_frame_size);
    }

  /* get_frame() must point to [this specific frame captured]. Fixed on
   * 2026-09-10: This line was originally Indentation error in
   * g_cam_byte_swap branch — once byte swap is disabled (`plant cam swap`
   * or default changed elsewhere), g_last_frame retains previous value
   * (likely for preview shadow), the caller gets the previous frame/another
   * buffer by calling get_frame(): desaturated on A, but upload read from B,
   * resulting in "changes appeared to have no effect".
   */

  if (sink != NULL)
    {
      sink(g_last_frame, g_frame_size, sink_arg);
    }

  /* 6. End [no re-arm]: DMA stops at frame boundary, rxbuf remains stable.
   * this is key for tear-free preview — if cam_dvp_arm() restarts DMA
   * here, the next frame immediately overwrites rxbuf, LVGL reads half-new
   * half-old during rendering → random stripes. The next capture start
   * will re-arm.
   */

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
   * use block_done (GDMA EOF = 1/frame) for the real rate.
   */

  if (priv->initialized)
    {
      uint32_t v0 = priv->block_done;

      nxsig_usleep(1000000);
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

  printf("L1 GPIO: PCLK(%d)=%d HREF(%d)=%d VSYNC(%d)=%d "
         "D0(%d)=%d D7(%d)=%d\n",
         CAM_DVP_PCLK_PIN, esp32s3_gpioread(CAM_DVP_PCLK_PIN),
         CAM_DVP_HREF_PIN, esp32s3_gpioread(CAM_DVP_HREF_PIN),
         CAM_DVP_VSYNC_PIN, esp32s3_gpioread(CAM_DVP_VSYNC_PIN),
         CAM_DVP_D0_PIN, esp32s3_gpioread(CAM_DVP_D0_PIN),
         CAM_DVP_D7_PIN, esp32s3_gpioread(CAM_DVP_D7_PIN));

  /* PCLK toggle test: 20 samples over ~2ms - any change = clock running */

    {
      int s;
      int pclk_prev = -1;
      int pclk_changes = 0;

      for (s = 0; s < 20; s++)
        {
          int p = esp32s3_gpioread(CAM_DVP_PCLK_PIN);

          if (pclk_prev >= 0 && p != pclk_prev)
            {
              pclk_changes++;
            }

          pclk_prev = p;
          up_udelay(100);
        }

      printf("L1 PCLK toggles in 2ms: %d/20 samples\n", pclk_changes);
    }

  /* L1b: VSYNC/HREF edge counter over 300 ms - settles whether the sensor
   * actually drives frame/line syncs (static reads always sample VSYNC=1
   * and are useless for this).
   */

    {
      int s;
      int vs_prev = -1;
      int hr_prev = -1;
      int vs_changes = 0;
      int hr_changes = 0;
      uint32_t irq_vs_begin = priv->vsync_count;

      for (s = 0; s < 3000; s++)
        {
          int v = esp32s3_gpioread(CAM_DVP_VSYNC_PIN);
          int h = esp32s3_gpioread(CAM_DVP_HREF_PIN);

          if (vs_prev >= 0 && v != vs_prev)
            {
              vs_changes++;
            }

          if (hr_prev >= 0 && h != hr_prev)
            {
              hr_changes++;
            }

          vs_prev = v;
          hr_prev = h;
          up_udelay(100);
        }

      printf("L1b VSYNC edges in 300ms: %d, HREF edges: %d, IRQ VSYNC: %u\n",
             vs_changes, hr_changes,
             (unsigned)(priv->vsync_count - irq_vs_begin));
    }

  /* All 8 data pins + HREF + VSYNC raw sample for the selected board. */

  printf("L1 D0-7(%d,%d,%d,%d,%d,%d,%d,%d)=%d%d%d%d%d%d%d%d "
         "HREF(%d)=%d VSYNC(%d)=%d\n",
         CAM_DVP_D0_PIN, CAM_DVP_D1_PIN, CAM_DVP_D2_PIN, CAM_DVP_D3_PIN,
         CAM_DVP_D4_PIN, CAM_DVP_D5_PIN, CAM_DVP_D6_PIN, CAM_DVP_D7_PIN,
         esp32s3_gpioread(CAM_DVP_D0_PIN), esp32s3_gpioread(CAM_DVP_D1_PIN),
         esp32s3_gpioread(CAM_DVP_D2_PIN), esp32s3_gpioread(CAM_DVP_D3_PIN),
         esp32s3_gpioread(CAM_DVP_D4_PIN), esp32s3_gpioread(CAM_DVP_D5_PIN),
         esp32s3_gpioread(CAM_DVP_D6_PIN), esp32s3_gpioread(CAM_DVP_D7_PIN),
         CAM_DVP_HREF_PIN, esp32s3_gpioread(CAM_DVP_HREF_PIN),
         CAM_DVP_VSYNC_PIN, esp32s3_gpioread(CAM_DVP_VSYNC_PIN));

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

  /* L7: live forensics ? is the VSYNC interrupt still enabled, and does
   * IO42 still belong to the camera (GPIO matrix input + output-enable)?
   */

  {
    uint32_t ena;
    uint32_t gpio_ena1;
    uint32_t func152;

    ena = cam_dvp_getreg(LCD_CAM_LC_DMA_INT_ENA_REG);
    gpio_ena1 = getreg32(0x60004000 + 0x002c);
    func152 = getreg32(0x60004000 + 0x0154 + 152 * 4);

    printf("L7 LC_DMA_INT_ENA=0x%08lx (cam_vsync_ena=%d) vsync=%u "
           "chunks=%u marks=%u started=%d\n",
           (unsigned long)ena, (ena >> 2) & 1, (unsigned)priv->vsync_count,
           (unsigned)priv->jpeg_chunks, (unsigned)priv->vsync_marks,
           (int)priv->started);
    printf("L7 GPIO_ENABLE1=0x%08lx (io42_out=%d io40_out=%d) "
           "FUNC152=0x%08lx (gpio=%lu inv=%d matrix=%d)\n",
           (unsigned long)gpio_ena1, (gpio_ena1 >> 10) & 1,
           (gpio_ena1 >> 8) & 1, (unsigned long)func152,
           (unsigned long)(func152 & 0x3f), (func152 >> 6) & 1,
           (func152 >> 7) & 1);
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

uint32_t esp32s3_cam_dvp_get_short_frames(void)
{
  return g_cam.short_frames;
}

uint8_t *esp32s3_cam_dvp_get_shadow(void)
{
  return g_cam_shadow;
}

size_t esp32s3_cam_dvp_get_shadow_size(void)
{
  return CAM_MAX_SIZE;
}

void esp32s3_cam_dvp_invalidate(const void *data, size_t len)
{
  uintptr_t addr;

  if (data == NULL || len == 0 || !esp32s3_ptr_extram(data))
    {
      return;
    }

  addr = (uintptr_t)data & ~((uintptr_t)31);
  len = (len + 31U + ((uintptr_t)data - addr)) & ~((size_t)31);
  cache_invalidate_addr((uint32_t)addr, len);
}

/****************************************************************************
 * Name: esp32s3_cam_dvp_set_framesize
 *
 * Runtime frame geometry switching (preview 160x120 / capture 640x480).
 *
 * DMA chain and rxbuf/shadow are allocated with CAM_MAX_SIZE, and
 * vs_eof_en=1 makes VSYNC
 * becomes a frame boundary (EOF prematurely ends transfer), so here there is
 * no need to reconfigure the DMA chain — only change
 * CPU read-back length and write-back length. The caller is responsible for
 * switching the sensor to the same resolution first (via SCCB),
 * And ensure capture is stopped before calling (no concurrent frame
 * grabbing).
 *
 * ⚠️ Do NOT clear warmed_up here: both modes use the same sensor window
 * (start/end both 0,0→2079,1547), only output scaling differs, scene
 * average brightness unchanged
 * → AEC/AGC/AWB remains effective, no need to re-run the 40-frame warm-up
 * (otherwise, on every capture, every return
 * Preview will all wait about an extra 9 seconds). Switching timing
 * stability is handled by two inherent aspects of the freeze path.
 * Frame period alignment (see the two-step align + post-mask EOF in
 * capture).
 *
 * Input Parameters:
 * w, h - target pixel dimensions (<= CAM_MAX_W / CAM_MAX_H)
 *
 * Returned Value: 0 success; negative errno for failure
 ****************************************************************************/

int esp32s3_cam_dvp_set_framesize(uint32_t w, uint32_t h)
{
  uint32_t max_w = g_cam_jpeg_mode ? CAM_JPEG_MAX_W : CAM_MAX_W;
  uint32_t max_h = g_cam_jpeg_mode ? CAM_JPEG_MAX_H : CAM_MAX_H;

  if (w == 0 || h == 0 || w > max_w || h > max_h)
    {
      printf("[Cam-DVP] set_framesize %ux%u out of range (max %dx%d)\n",
             (unsigned)w, (unsigned)h, (unsigned)max_w, (unsigned)max_h);
      return -EINVAL;
    }

  g_frame_w = w;
  g_frame_h = h;
  g_frame_size = g_cam_jpeg_mode ? CAM_RX_BUF_SIZE :
                 (w * h * CAM_FRAME_BPP);

  printf("[Cam-DVP] framesize -> %ux%u (%s, buffer %u bytes)\n",
         (unsigned)w, (unsigned)h,
         g_cam_jpeg_mode ? "JPEG" : "RGB565",
         (unsigned)g_frame_size);
  return OK;
}

void esp32s3_cam_dvp_set_jpeg(bool on)
{
  g_jpeg_vsync_aligned = false;
  g_cam_jpeg_mode = on;
  printf("[Cam-DVP] jpeg mode %s\n", on ? "ON" : "OFF");
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

void esp32s3_cam_dvp_set_stream_direct(bool on)
{
  g_stream_direct = on;
}

void esp32s3_cam_dvp_set_official_yuv(bool on)
{
#ifdef CONFIG_ARCH_BOARD_M5STACK_CORES3
  g_m5_official_sensor = on;
  printf("[Cam-DVP] official chunk receiver %s\n", on ? "ON" : "OFF");
#else
  (void)on;
#endif
}

/****************************************************************************
 * Name: esp32s3_cam_dvp_set_quiet
 *
 * Silent mode: disable capture's per-frame logging (used by the preview
 * thread, to avoid flooding the log and slowing things down).
 ****************************************************************************/

void esp32s3_cam_dvp_set_quiet(bool quiet)
{
  g_cam_quiet = quiet;
}

/****************************************************************************
 * Name: esp32s3_cam_dvp_set_stream / esp32s3_cam_dvp_get_stream
 *
 * Preview stream mode switch. When enabled, capture() follows the "rolling
 * snapshot" path: DMA continuous acquisition,
 * Frame complete then copied to shadow (no freeze, no re-arm), preview wait
 * reduced from ~2 frame periods
 * reduces to about one frame period; LCD/UI reads from shadow while DMA
 * writes to rxbuf, without mutual interference.
 * Single-frame paths like photo/color bars remain disabled (freeze mode, for
 * stable, zero-copy frames).
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
