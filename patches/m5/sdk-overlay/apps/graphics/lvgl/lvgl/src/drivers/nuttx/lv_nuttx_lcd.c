/**
 * @file lv_nuttx_lcd.c
 *
 */

/*********************
 *      INCLUDES
 *********************/

#include "lv_nuttx_lcd.h"

#if LV_USE_NUTTX

#if LV_USE_NUTTX_LCD

#include <sys/ioctl.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <debug.h>
#include <errno.h>
#include <fcntl.h>
#include <nuttx/lcd/lcd_dev.h>

#include "../../../lvgl.h"
#include "../../lvgl_private.h"

#ifdef CONFIG_ARCH_BOARD_M5STACK_CORES3
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdbool.h>
extern uint64_t esp32s3_rt_timer_time_us(void);

/* Default-off wall time around the existing synchronous PUTAREA ioctl.
 * No pixels, buffer pointers or display commands are logged. */
struct lcd_perf_token_s {bool enabled;uint32_t epoch;uint64_t started;};
struct lcd_perf_stats_s
{
    uint64_t pixels,usec,max_usec;
    uint32_t calls,small,errors,clock_backwards,max_pixels;
};
static pthread_mutex_t g_lcd_perf_lock=PTHREAD_MUTEX_INITIALIZER;
static bool g_lcd_perf_enabled;
static uint32_t g_lcd_perf_epoch;
static struct lcd_perf_stats_s g_lcd_perf_stats;

void lv_nuttx_lcd_perf_trace(bool enabled)
{
    pthread_mutex_lock(&g_lcd_perf_lock);
    g_lcd_perf_enabled=enabled;g_lcd_perf_epoch++;
    if(enabled)g_lcd_perf_stats=(struct lcd_perf_stats_s){0};
    pthread_mutex_unlock(&g_lcd_perf_lock);
}

static struct lcd_perf_token_s lcd_perf_begin(void)
{
    struct lcd_perf_token_s token={0};
    pthread_mutex_lock(&g_lcd_perf_lock);
    token.enabled=g_lcd_perf_enabled;token.epoch=g_lcd_perf_epoch;
    pthread_mutex_unlock(&g_lcd_perf_lock);
    if(token.enabled)token.started=esp32s3_rt_timer_time_us();
    return token;
}

static void lcd_perf_end(struct lcd_perf_token_s token,uint32_t pixels,int rc)
{
    if(!token.enabled)return;
    uint64_t ended=esp32s3_rt_timer_time_us();
    pthread_mutex_lock(&g_lcd_perf_lock);
    if(g_lcd_perf_enabled && token.epoch==g_lcd_perf_epoch)
    {
        g_lcd_perf_stats.calls++;
        g_lcd_perf_stats.pixels+=pixels;
        if(pixels>g_lcd_perf_stats.max_pixels)g_lcd_perf_stats.max_pixels=pixels;
        if(pixels<=2048)g_lcd_perf_stats.small++;
        if(rc<0)g_lcd_perf_stats.errors++;
        if(ended<token.started)g_lcd_perf_stats.clock_backwards++;
        else
        {
            uint64_t elapsed=ended-token.started;
            g_lcd_perf_stats.usec+=elapsed;
            if(elapsed>g_lcd_perf_stats.max_usec)g_lcd_perf_stats.max_usec=elapsed;
        }
    }
    pthread_mutex_unlock(&g_lcd_perf_lock);
}

void lv_nuttx_lcd_perf_dump(void)
{
    pthread_mutex_lock(&g_lcd_perf_lock);
    struct lcd_perf_stats_s stats=g_lcd_perf_stats;
    bool enabled=g_lcd_perf_enabled;uint32_t epoch=g_lcd_perf_epoch;
    pthread_mutex_unlock(&g_lcd_perf_lock);
    printf("[LCDPerf] enabled=%d epoch=%u calls=%u pixels=%llu us=%llu max_us=%llu small=%u max_pixels=%u errors=%u clock_backwards=%u\n",
           enabled,epoch,stats.calls,(unsigned long long)stats.pixels,
           (unsigned long long)stats.usec,(unsigned long long)stats.max_usec,
           stats.small,stats.max_pixels,stats.errors,stats.clock_backwards);
}
#endif


/*********************
 *      DEFINES
 *********************/

/**********************
 *      TYPEDEFS
 **********************/

typedef struct {
    /* fd should be defined at the beginning */
    int fd;
    lv_display_t * disp;
    struct lcddev_area_s area;
    struct lcddev_area_align_s align_info;
#ifdef CONFIG_ARCH_BOARD_M5STACK_CORES3
    pthread_t tx_thread;
    pthread_mutex_t tx_lock;
    pthread_cond_t tx_job_cond,tx_done_cond;
    bool tx_created,tx_busy,tx_stop;
    struct lcddev_area_s tx_area;
    uint8_t * owned_draw_buf_1,* owned_draw_buf_2;
#endif
} lv_nuttx_lcd_t;

/**********************
 *  STATIC PROTOTYPES
 **********************/

static int32_t align_round_up(int32_t v, uint16_t align);
static void rounder_cb(lv_event_t * e);
static void flush_cb(lv_display_t * disp, const lv_area_t * area_p,
                     uint8_t * color_p);
static lv_display_t * lcd_init(int fd, int hor_res, int ver_res);
static void display_release_cb(lv_event_t * e);

#ifdef CONFIG_ARCH_BOARD_M5STACK_CORES3
/* Match the official queued display's render/transfer overlap without
 * bypassing the board's internal-SRAM staging. LVGL permits one outstanding
 * flush: render the other partial buffer, then wait before submitting it.
 * Only the UI thread calls LVGL, including its flush-wait callback.
 */
/* Use different conditions for different predicates. NuttX's cond_wait
 * releases the mutex before its shared-sem wait; a higher-priority worker
 * must not re-wait on the UI completion semaphore and consume its credit.
 * Each condition now has exactly one waiting thread. */
static _Atomic bool g_lcd_async_enabled=true;
static _Atomic unsigned g_lcd_async_workers,g_lcd_async_queued,g_lcd_async_sync;

void lv_nuttx_lcd_async_enable(bool enabled)
{
    atomic_store(&g_lcd_async_enabled,enabled);
}

void lv_nuttx_lcd_async_dump(void)
{
    printf("[LCDAsync] enabled=%d workers=%u queued=%u sync=%u\n",
           atomic_load(&g_lcd_async_enabled),atomic_load(&g_lcd_async_workers),
           atomic_load(&g_lcd_async_queued),atomic_load(&g_lcd_async_sync));
}

static void lcd_transfer(lv_nuttx_lcd_t * lcd,struct lcddev_area_s * area)
{
    struct lcd_perf_token_s perf=lcd_perf_begin();
    int rc=ioctl(lcd->fd,LCDDEVIO_PUTAREA,(unsigned long)area);
    uint32_t pixels=(uint32_t)(area->col_end-area->col_start+1)*
                    (uint32_t)(area->row_end-area->row_start+1);
    lcd_perf_end(perf,pixels,rc);
}

static void * lcd_tx_worker(void * arg)
{
    lv_nuttx_lcd_t * lcd=arg;
    pthread_mutex_lock(&lcd->tx_lock);
    for(;;) {
        while(!lcd->tx_busy && !lcd->tx_stop)
            pthread_cond_wait(&lcd->tx_job_cond,&lcd->tx_lock);
        if(lcd->tx_stop && !lcd->tx_busy)break;
        struct lcddev_area_s area=lcd->tx_area;
        pthread_mutex_unlock(&lcd->tx_lock);
        lcd_transfer(lcd,&area);
        pthread_mutex_lock(&lcd->tx_lock);
        lcd->tx_busy=false;
        pthread_cond_broadcast(&lcd->tx_done_cond);
    }
    pthread_mutex_unlock(&lcd->tx_lock);
    return NULL;
}

static void lcd_tx_wait(lv_display_t * disp)
{
    lv_nuttx_lcd_t * lcd=lv_display_get_driver_data(disp);
    pthread_mutex_lock(&lcd->tx_lock);
    while(lcd->tx_busy)pthread_cond_wait(&lcd->tx_done_cond,&lcd->tx_lock);
    pthread_mutex_unlock(&lcd->tx_lock);
    /* The actual LVGL wait_for_flushing clears disp->flushing after this
     * callback. No worker-thread LVGL access or volatile-flag spinning. */
}

static bool lcd_tx_init(lv_nuttx_lcd_t * lcd)
{
    if(pthread_mutex_init(&lcd->tx_lock,NULL)!=0)return false;
    if(pthread_cond_init(&lcd->tx_job_cond,NULL)!=0) {
        pthread_mutex_destroy(&lcd->tx_lock);return false;
    }
    if(pthread_cond_init(&lcd->tx_done_cond,NULL)!=0) {
        pthread_cond_destroy(&lcd->tx_job_cond);
        pthread_mutex_destroy(&lcd->tx_lock);return false;
    }
    pthread_attr_t attr;
    int rc=pthread_attr_init(&attr);
    if(rc==0) {
        rc=pthread_attr_setstacksize(&attr,3072);
        /* CoreS3 uses one CPU, UI priority100 and RR interval200ms. Wake
         * this short transfer task above UI so it arms DMA before UI draws
         * the other buffer; its semaphore wait then lets rendering proceed.
         * Preserve camera/wake/audio policy and all interrupt priorities. */
        struct sched_param priority={.sched_priority=101};
        if(rc==0)rc=pthread_attr_setinheritsched(&attr,PTHREAD_EXPLICIT_SCHED);
        if(rc==0)rc=pthread_attr_setschedpolicy(&attr,SCHED_RR);
        if(rc==0)rc=pthread_attr_setschedparam(&attr,&priority);
        if(rc==0)rc=pthread_create(&lcd->tx_thread,&attr,lcd_tx_worker,lcd);
        pthread_attr_destroy(&attr);
    }
    if(rc!=0) {
        pthread_cond_destroy(&lcd->tx_done_cond);
        pthread_cond_destroy(&lcd->tx_job_cond);
        pthread_mutex_destroy(&lcd->tx_lock);return false;
    }
    lcd->tx_created=true;
    atomic_fetch_add(&g_lcd_async_workers,1);
    return true;
}

static void lcd_tx_destroy(lv_nuttx_lcd_t * lcd)
{
    if(!lcd->tx_created)return;
    pthread_mutex_lock(&lcd->tx_lock);
    lcd->tx_stop=true;
    pthread_cond_broadcast(&lcd->tx_job_cond);
    pthread_mutex_unlock(&lcd->tx_lock);
    pthread_join(lcd->tx_thread,NULL);
    lcd->tx_created=false;
    pthread_cond_destroy(&lcd->tx_done_cond);
        pthread_cond_destroy(&lcd->tx_job_cond);
    pthread_mutex_destroy(&lcd->tx_lock);
    atomic_fetch_sub(&g_lcd_async_workers,1);
}
#endif


/**********************
 *  STATIC VARIABLES
 **********************/

/**********************
 *      MACROS
 **********************/

/**********************
 *   GLOBAL FUNCTIONS
 **********************/

lv_display_t * lv_nuttx_lcd_create(const char * dev_path)
{
    struct fb_videoinfo_s vinfo;
    struct lcd_planeinfo_s pinfo;
    lv_display_t * disp;
    int fd;
    int ret;

    LV_ASSERT_NULL(dev_path);

    LV_LOG_USER("lcd %s opening", dev_path);
    fd = open(dev_path, 0);
    if(fd < 0) {
        perror("Error: cannot open lcd device");
        return NULL;
    }

    LV_LOG_USER("lcd %s open success", dev_path);

    ret = ioctl(fd, LCDDEVIO_GETVIDEOINFO,
                (unsigned long)((uintptr_t)&vinfo));
    if(ret < 0) {
        perror("Error: ioctl(LCDDEVIO_GETVIDEOINFO) failed");
        close(fd);
        return NULL;
    }

    ret = ioctl(fd, LCDDEVIO_GETPLANEINFO,
                (unsigned long)((uintptr_t)&pinfo));
    if(ret < 0) {
        perror("ERROR: ioctl(LCDDEVIO_GETPLANEINFO) failed");
        close(fd);
        return NULL;
    }

    disp = lcd_init(fd, vinfo.xres, vinfo.yres);
    if(disp == NULL) {
        close(fd);
    }

    return disp;
}

/**********************
 *   STATIC FUNCTIONS
 **********************/

static int32_t align_round_up(int32_t v, uint16_t align)
{
    return (v + align - 1) & ~(align - 1);
}

static void rounder_cb(lv_event_t * e)
{
    lv_nuttx_lcd_t * lcd = lv_event_get_user_data(e);
    lv_area_t * area = lv_event_get_param(e);
    struct lcddev_area_align_s * align_info = &lcd->align_info;
    int32_t w;
    int32_t h;

    area->x1 &= ~(align_info->col_start_align - 1);
    area->y1 &= ~(align_info->row_start_align - 1);

    w = align_round_up(lv_area_get_width(area), align_info->width_align);
    h = align_round_up(lv_area_get_height(area), align_info->height_align);

    area->x2 = area->x1 + w - 1;
    area->y2 = area->y1 + h - 1;
}

static void flush_cb(lv_display_t * disp, const lv_area_t * area_p,
                     uint8_t * color_p)
{
    lv_nuttx_lcd_t * lcd=disp->driver_data;
    lcd->area.row_start=area_p->y1;
    lcd->area.row_end=area_p->y2;
    lcd->area.col_start=area_p->x1;
    lcd->area.col_end=area_p->x2;
    lcd->area.data=color_p;
#ifdef CONFIG_ARCH_BOARD_M5STACK_CORES3
    if(lcd->tx_created && atomic_load(&g_lcd_async_enabled)) {
        pthread_mutex_lock(&lcd->tx_lock);
        /* LVGL waits before another flush, including the previous last
         * rectangle. Keep a defensive wait rather than overwrite a job. */
        while(lcd->tx_busy)pthread_cond_wait(&lcd->tx_done_cond,&lcd->tx_lock);
        lcd->tx_area=lcd->area;
        lcd->tx_busy=true;
        atomic_fetch_add(&g_lcd_async_queued,1);
        pthread_cond_broadcast(&lcd->tx_job_cond);
        pthread_mutex_unlock(&lcd->tx_lock);
        return;
    }
    atomic_fetch_add(&g_lcd_async_sync,1);
    lcd_transfer(lcd,&lcd->area);
#else
    ioctl(lcd->fd,LCDDEVIO_PUTAREA,(unsigned long)&lcd->area);
#endif
    lv_display_flush_ready(disp);
}

static lv_display_t * lcd_init(int fd, int hor_res, int ver_res)
{
    uint8_t * draw_buf = NULL;
    uint8_t * draw_buf_2 = NULL;
    lv_nuttx_lcd_t * lcd = lv_malloc_zeroed(sizeof(lv_nuttx_lcd_t));
    LV_ASSERT_MALLOC(lcd);
    if(lcd == NULL) {
        LV_LOG_ERROR("lv_nuttx_lcd_t malloc failed");
        return NULL;
    }

    lv_display_t * disp = lv_display_create(hor_res, ver_res);
    if(disp == NULL) {
        lv_free(lcd);
        return NULL;
    }

    uint32_t px_size = lv_color_format_get_size(lv_display_get_color_format(disp));
#if LV_NUTTX_LCD_BUFFER_COUNT > 0
    uint32_t buf_size = hor_res * ver_res * px_size;
    lv_display_render_mode_t render_mode = LV_DISPLAY_RENDER_MODE_FULL;
#else
    uint32_t buf_size = hor_res * LV_NUTTX_LCD_BUFFER_SIZE * px_size;
    lv_display_render_mode_t render_mode = LV_DISPLAY_RENDER_MODE_PARTIAL;
#endif

    draw_buf = lv_malloc(buf_size);
    if(draw_buf == NULL) {
        LV_LOG_ERROR("display draw_buf malloc failed");
        lv_free(lcd);
        return NULL;
    }

#if LV_NUTTX_LCD_BUFFER_COUNT == 2
    draw_buf_2 = lv_malloc(buf_size);
    if(draw_buf_2 == NULL) {
        LV_LOG_ERROR("display draw_buf_2 malloc failed");
        lv_free(lcd);
        lv_free(draw_buf);
        return NULL;
    }
#endif

    lcd->fd = fd;
    if(ioctl(fd, LCDDEVIO_GETAREAALIGN, &lcd->align_info) < 0) {
        perror("Error: ioctl(LCDDEVIO_GETAREAALIGN) failed");
    }

#if defined(CONFIG_ARCH_BOARD_M5STACK_CORES3) && LV_NUTTX_LCD_BUFFER_COUNT == 0
    /* Two partial buffers, not two full screens. Allocation failure keeps
     * the established synchronous single-buffer path. */
    draw_buf_2=lv_malloc(buf_size);
    if(draw_buf_2) {
        if(lcd_tx_init(lcd)) {
            lcd->owned_draw_buf_2=draw_buf_2;
            lv_display_set_flush_wait_cb(disp,lcd_tx_wait);
        }
        else {lv_free(draw_buf_2);draw_buf_2=NULL;}
    }
    printf("[LCDAsync] init worker=%d partial_bytes=%u\n",lcd->tx_created,(unsigned)buf_size);
#endif
#ifdef CONFIG_ARCH_BOARD_M5STACK_CORES3
    lcd->owned_draw_buf_1=draw_buf;
    lcd->owned_draw_buf_2=draw_buf_2;
#endif
    lcd->disp = disp;
    lv_display_set_buffers(lcd->disp, draw_buf, draw_buf_2, buf_size, render_mode);
    lv_display_set_flush_cb(lcd->disp, flush_cb);
    lv_display_add_event_cb(lcd->disp, rounder_cb, LV_EVENT_INVALIDATE_AREA, lcd);
    lv_display_add_event_cb(lcd->disp, display_release_cb, LV_EVENT_DELETE, lcd->disp);
    lv_display_set_driver_data(lcd->disp, lcd);

    return lcd->disp;
}

static void display_release_cb(lv_event_t * e)
{
    lv_display_t * disp = (lv_display_t *) lv_event_get_user_data(e);
    lv_nuttx_lcd_t * dsc = lv_display_get_driver_data(disp);
    if(dsc) {
#ifdef CONFIG_ARCH_BOARD_M5STACK_CORES3
        /* Join before freeing buffers, display state or closing the FD. */
        lcd_tx_destroy(dsc);
        lv_display_set_flush_wait_cb(disp,NULL);
        lv_free(dsc->owned_draw_buf_1);
        dsc->owned_draw_buf_1=NULL;
        lv_free(dsc->owned_draw_buf_2);
        dsc->owned_draw_buf_2=NULL;
        /* lv_display_set_buffers uses embedded _static_buf descriptors;
         * only their pixel storage is owned by this port. */
        disp->buf_1=NULL;
        disp->buf_2=NULL;
#endif
        lv_display_set_driver_data(disp, NULL);
        lv_display_set_flush_cb(disp, NULL);

        /* clear display buffer */
        if(disp->buf_1) {
            lv_free(disp->buf_1);
            disp->buf_1 = NULL;
        }
        if(disp->buf_2) {
            lv_free(disp->buf_2);
            disp->buf_2 = NULL;
        }

        /* close device fb */
        if(dsc->fd >= 0) {
            close(dsc->fd);
            dsc->fd = -1;
        }
        lv_free(dsc);
        LV_LOG_USER("Done");
    }
}
#endif /*LV_USE_NUTTX_LCD*/

#endif /* LV_USE_NUTTX*/
