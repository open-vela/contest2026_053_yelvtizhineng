/****************************************************************************
 * hal_cam_ll.h — Camera XMCLK init using LEDC PWM
 *
 * Bit positions VERIFIED against NuttX native hardware/esp32s3_ledc.h
 * (2026-08-07). No dependencies on esp-hal-3rdparty or IDF headers.
 *
 * Formula (IDF ledc_calculate_divisor):
 *   div_param = ((src_freq << 8) + rounding) / (freq_hz * 2^duty_res)
 *   For XTAL=40MHz, target=20MHz, duty_res=1:
 *     div_param = ((40_000_000 << 8) + 20_000_000) / (20_000_000 * 2)
 *              = 256
 *   CLK_DIV is Q10.8 fixed-point: 256 = 1.0 (int=1, frac=0)
 *   f_out = 40MHz / (1.0 * 2) = 20MHz ✓
 *
 * USAGE:
 *   cam_ll_xmclk_init(20000000);  // configure LEDC for 20MHz on IO39
 *
 * DEPENDS ON: xtensa.h (getreg32/putreg32/modifyreg32)
 ****************************************************************************/

#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "xtensa.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ── SYSTEM clock enable / reset ─────────────────────────────────── */

#define SYSTEM_CLK_EN0    0x600C0018
#define SYSTEM_RST_EN0    0x600C0020
#define LEDC_CLK_BIT      (1u << 11)   /* CLK_EN0[11] */

/* ── LEDC peripheral base (ESP32-S3 TRM) ─────────────────────────── */

#define DR_REG_LEDC_BASE  0x60019000

/* Register offsets */
#define LEDC_CH0_CONF0    (DR_REG_LEDC_BASE + 0x00)
#define LEDC_CH0_HPOINT   (DR_REG_LEDC_BASE + 0x04)
#define LEDC_CH0_DUTY     (DR_REG_LEDC_BASE + 0x08)
#define LEDC_CH0_CONF1    (DR_REG_LEDC_BASE + 0x0C)
#define LEDC_TIMER0_CONF  (DR_REG_LEDC_BASE + 0xA0)
#define LEDC_CONF         (DR_REG_LEDC_BASE + 0xD0)

/* ── LEDC timer conf register (TIMER0_CONF at +0xA0) ─────────────
 * Bit positions VERIFIED from hardware/esp32s3_ledc.h:
 *   DUTY_RES  bits [3:0]   (LEDC_TIMER0_DUTY_RES)
 *   CLK_DIV   bits [21:4]  (LEDC_CLK_DIV_TIMER0)
 *   PAUSE     bit 22       (LEDC_TIMER0_PAUSE)
 *   RST       bit 23       (LEDC_TIMER0_RST)
 *   PARA_UP   bit 25       (LEDC_TIMER0_PARA_UP)
 *
 * NOTE: Previous code had DUTY_RES at bit 26 (WRONG) and CLK_DIV at bit 8
 * (WRONG). C bitfields on Xtensa GCC pack LSB-first, so DUTY_RES:4 is at
 * bits 3:0, not 29:26 like the IDF struct comment implies.
 */

#define LEDC_DUTY_RES_S    0
#define LEDC_CLK_DIV_S     4
#define LEDC_TIMER_PAUSE   (1u << 22)
#define LEDC_TIMER_RST     (1u << 23)
#define LEDC_TIMER_PARA_UP (1u << 25)

/* ── LEDC channel conf0 register (CH0_CONF0 at +0x00) ─────────────
 *   SIG_OUT_EN bit 2  (LEDC_SIG_OUT_EN_CH0)
 *   PARA_UP    bit 4  (LEDC_PARA_UP_CH0)
 */

#define LEDC_SIG_OUT_EN   (1u << 2)
#define LEDC_CH_PARA_UP   (1u << 4)

/* ── LEDC global conf register (at +0xD0) ─────────────────────────
 *   CLK_EN     bit 31
 *   APB_CLK_SEL bits [1:0] → 3 = XTAL 40MHz
 */

#define LEDC_CLK_EN       (1u << 31)

/* ── XMCLK init (IDF xclk.c approach) ────────────────────────────── */

static inline void cam_ll_xmclk_init(uint32_t xclk_freq_hz)
{
    (void)xclk_freq_hz;

    /* Enable LEDC peripheral clock + release reset */
    modifyreg32(SYSTEM_CLK_EN0, 0, LEDC_CLK_BIT);
    modifyreg32(SYSTEM_RST_EN0, LEDC_CLK_BIT, 0);

    /* Global: XTAL source (APB_CLK_SEL=3), enable clock */
    putreg32((3u << 0) | LEDC_CLK_EN, LEDC_CONF);

    /* Timer 0: duty_res=1 (bits 3:0), clk_div=256 (bits 21:4)
     * CLK_DIV is Q10.8 fixed-point: 256 = 1.0 divider */
    putreg32(1u                        /* DUTY_RES = 1 */
           | (256u << LEDC_CLK_DIV_S), /* CLK_DIV  = 256 (Q10.8 = 1.0) */
             LEDC_TIMER0_CONF);
    modifyreg32(LEDC_TIMER0_CONF, 0, LEDC_TIMER_PARA_UP);  /* latch */
    modifyreg32(LEDC_TIMER0_CONF, 0, LEDC_TIMER_RST);       /* reset pulse */
    modifyreg32(LEDC_TIMER0_CONF, LEDC_TIMER_RST, 0);       /* release */

    /* Channel 0: timer_sel=0, sig_out_en (IDF order: params→latch→start) */
    putreg32(LEDC_SIG_OUT_EN, LEDC_CH0_CONF0);  /* no latch yet */
    putreg32(0, LEDC_CH0_HPOINT);                /* hpoint = 0 */
    putreg32(1u << 4, LEDC_CH0_DUTY);            /* duty = 1 (50%) */
    modifyreg32(LEDC_CH0_CONF0, 0, LEDC_CH_PARA_UP);  /* latch */

    /* Start duty cycle (DUTY_START at bit 31 of conf1) */
    putreg32(1u << 31, LEDC_CH0_CONF1);
}

#ifdef __cplusplus
}
#endif
