/****************************************************************************
 * apps/plant-companion/components/camera_capture/camera_capture.h
 *
 * OV3660 camera — SCCB probe + JPEG 配置 + 帧采集
 ****************************************************************************/

#ifndef __APPS_PLANT_COMPANION_CAMERA_CAPTURE_H
#define __APPS_PLANT_COMPANION_CAMERA_CAPTURE_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C"
{
#endif

/**
 * 探测 OV3660（SCCB 读 PID 0x3660）+ 初始化 XMCLK
 * @return 0 成功；负 errno 失败
 */
int camera_init(void);
int camera_power_idle(void);
#ifdef CONFIG_PLANT_CAMERA_GC0308
/* Caller releases ownership only after restoring shared UART/soil pins. */
bool camera_boot_prepare_busy(void);
int camera_boot_prepare_claim(void);
void camera_boot_prepare_release(void);
int camera_boot_prepare(unsigned budget_ms,int *shutdown_rc);
#endif

/**
 * 配置 OV3660 输出 JPEG 320×240（官方寄存器序列）
 * 之后 DVP 数据线开始输出 JPEG 帧流，等待 camera_capture_frame() 收帧。
 * @return 0 成功；负 errno 失败
 */
int camera_config_jpeg_qvga(void);
int camera_config_jpeg_wh(uint16_t w, uint16_t h);
int camera_jpeg_capture(const uint8_t **frame, size_t *len_out);
int camera_jpeg_capture_to_sd(const char *path, size_t *len_out);

/**
 * 配置 OV3660 输出【指定尺寸】的 RGB565（完整初始化序列：软复位 + 格式 +
 * 帧尺寸 + PLL + AEC/AWB 收敛等待）。只应在首次配置时调用。
 * @param w,h 目标尺寸（上限 640×480）
 * @return 0 成功；负 errno 失败
 */
int camera_config_rgb565_wh(uint16_t w, uint16_t h);

/**
 * 兼容包装：等价于 camera_config_rgb565_wh(CAM_PREVIEW_W, CAM_PREVIEW_H)
 * @return 0 成功；负 errno 失败
 */
int camera_config_rgb565_qvga(void);

/**
 * 切到【预览】模式（160×120）。预览可以糊、必须快，所以分辨率压低。
 * 首次调用 = 完整配置；已在该模式时立即返回。
 * @return 0 成功；负 errno 失败
 */
int camera_mode_preview(void);

/**
 * 切到【拍照】模式（320×240）：上传服务器识别的图（2×2 binning，亮部不过曝）。
 * 只改输出窗口 + PLL（复用已收敛的 AEC/AWB），约 0.15s。
 * ⚠️ 调用前必须先 camera_preview_stop()，不允许并发取帧。
 * @return 0 成功；负 errno 失败
 */
int camera_mode_photo(void);

/**
 * 在【拍照】模式下抓一帧高清图（零拷贝，不 malloc）。
 * 调用前先 camera_mode_photo()。返回指针指向驱动内部帧缓冲，在下次
 * capture / 切模式前一直有效（AI worker 以 borrowed 方式借用上传）。
 * 原始帧同时落盘 /mnt/sd/photo.rgb，便于电脑端核对。
 * @param frame   输出：帧缓冲（CAM_PHOTO_W×CAM_PHOTO_H RGB565 小端）
 * @param len_out 输出：帧字节数（= CAM_PHOTO_SIZE）
 * @return 0 成功；负 errno 失败
 */
int camera_photo_capture(const uint8_t **frame, size_t *len_out);

/**
 * 2026-09-09 M2（土壤↔拍照引脚互斥）：完整关闭摄像头硬件，放行 IO42/40。
 *   1) esp32s3_cam_dvp_stop()：停 CAM/DMA（若预览线程已停则幂等无操作）；
 *   2) SCCB 软复位 OV3660（0x3008=0x82）→ 传感器回冷启动默认态，不再
 *      推挽驱动 DVP 输出 → IO40(Y9)/IO42(VSYNC) 可安全切回 UART0。
 * 调用方随后需 esp32s3_uart0_reclaim_pins() + 恢复 soil 轮询。
 * @return 0 成功；SCCB 复位失败返回负 errno（引脚仍可切回，不阻塞）
 */
int camera_shutdown(void);

/**
 * 读 OV3660 关键寄存器（输出尺寸/格式），验证实际配置
 * @return 0 成功；负 errno 失败
 */
int camera_dump_regs(void);

/**
 * 官方 esp32-camera OV3660 色彩矩阵饱和度等级，level=-4..+4。
 * 直接改 0x5381..0x538B（可读写、可回读），不走 0x5588 自动饱和度位。
 */
int camera_set_saturation(int level);

/**
 * 官方 esp32-camera OV3660 AEC 目标亮度等级，level=-5..+5。
 * 负值整体压暗，用来防止 640x480 亮部红通道先饱和。
 */
int camera_set_ae_level(int level);

/* v171：取景最大曝光上限（行，0=恢复默认）。界面用于调参。 */
int camera_set_preview_ae_max_lines(int lines);

/* v172：单寄存器读写（调参） */
int camera_peek_reg(int reg, uint8_t *val);
int camera_poke_reg(int reg, int val);

/* 恢复芯片默认 gamma，或回到本工程当前的自定义 gamma。 */
int camera_set_gamma_default(void);
int camera_set_gamma_local(void);

/* SDE 总开关：关闭后保留 CMX/scale/AWB，只去掉 contrast/saturation。 */
int camera_set_sde(bool on);

/* 高光去饱和补丁开关。正式链路默认关闭，只作为诊断 A/B 使用。 */
void camera_set_highlight_desat(bool on);

/**
 * 捕获一帧 JPEG（阶段一目标；DMA 收帧）
 * @param buf  输出缓冲（>= max_size 字节）
 * @param max_size 缓冲容量
 * @param out_len 实际 JPEG 长度
 * @return 0 成功；负 errno 失败
 */
int camera_capture_frame(uint8_t *buf, size_t max_size, size_t *out_len);

/**
 * 传感器彩条测试图案（0x503d bit0）验证字节序 / R/B 分量序。
 * 打印 8 条标准色 × 4 种解码（LE/SWP/LE_RB/SWP_RB），
 * 与标准色 [白黄青绿品红蓝黑] 吻合的那列 = 需要做的变换。
 * 结束后自动关彩条、恢复实时画面。
 * @return 0 成功；负 errno 失败
 */
int camera_colorbar_test(void);

/**
 * 直接写 LCD（绕过 LVGL 渲染器）：软件最近邻缩放 RGB565 帧 → LCDDEVIO_PUTAREA。
 * 参考官方 dvp_spi_lcd 的 draw_bitmap 模式。fb 为标准 LE RGB565
 * （capture swap=on 后即此格式）。
 * @param fb 源帧 (sw×sh×2)
 * @param dx,dy,dw,dh 目标区域（LCD 坐标）
 * @return 0 成功；负 errno 失败
 */

/* 预览快路径：关掉逐像素锐化以提帧率（单帧拍照保持默认锐化）。 */
void lcd_put_rgb565_set_sharpen(bool on);
int lcd_put_rgb565(const uint8_t *fb, int sw, int sh,
                   int dx, int dy, int dw, int dh);
/* 1:1 transport for a standard little-endian RGB565 frame.  The LCD driver
 * performs the final panel byte swap through LCDDEVIO_PUTAREA, matching the
 * already-working LVGL display path. */
int lcd_put_rgb565_raw(const uint8_t *fb, int sw, int rows,
                       int dx, int dy);

/* Same as lcd_put_rgb565_raw(), except the pixel bytes are already in the
 * panel byte order and LCDDEVIO_PUTAREA_PRESWAPPED can skip an in-place
 * full-frame swap in PSRAM. */

int lcd_put_rgb565_raw_preswapped(const uint8_t *fb, int sw, int rows,
                                  int dx, int dy);
int lcd_put_rgb565_cover_be(const uint8_t *fb, int sw, int sh,
                            int dx, int dy, int dw, int dh);
int lcd_put_rgb565_cover(const uint8_t *fb, int sw, int sh,
                         int dx, int dy, int dw, int dh);
int lcd_put_rgb565_cover_fast(const uint8_t *fb, int sw, int sh,
                         int dx, int dy, int dw, int dh);

/**
 * 动态预览（参考官方 dvp_spi_lcd）：后台线程连续 capture → 软件缩放 →
 * 直接写 LCD 区域。拍照 = camera_preview_take() 冻结（画面定格），
 * 退出页面 = camera_preview_stop()。
 */
int camera_preview_start(int x, int y, int w, int h);
int camera_preview_start_hd(int x, int y, int w, int h);
int camera_preview_start_rgb(int x, int y, int w, int h);
void camera_preview_set_color_mode(int mode);
int camera_preview_get_color_mode(void);
void camera_preview_set_yuv_hardware(bool on);
bool camera_preview_get_yuv_hardware(void);
void camera_preview_set_sensor_format(bool yuv);
bool camera_preview_get_sensor_format(void);
void camera_preview_set_verify(bool on);
void camera_preview_set_diag(bool on);
bool camera_preview_get_diag(void);
void camera_preview_set_hold(bool on);
void camera_preview_set_min_interval_ms(int ms);
int camera_preview_set_min_interval_ms_get(void);

/* v166：取景控件「让位」蒙版（形状=圆角矩形：r=w/2 即圆形，r=0 即直角矩形）。
 * UI 线程建页时注册，预览线程 start 时取快照；相机写屏时把控件
 * 覆盖区让出来（控件像素由 LVGL 自己画）。 */
void camera_preview_mask_clear(void);
int  camera_preview_mask_add(int x, int y, int w, int h, int r);
int camera_preview_get_jpeg(const uint8_t **jpeg, size_t *len);

/* 连续单色帧计数：>=3 说明镜头大概率被挡（或传感器异常），
 * 快门据此直接提示，不浪费一次上传识别。 */

unsigned camera_preview_mono_streak(void);

/* 快门专用：2048x1536 单拍 + TJpgDec 严格解码验证 + 失败重抓（最多
 * max_tries 次）。成功把解码结果定格到取景框 (x,y,w,h) 并返回原始
 * 2048 字节；调用前必须先 camera_preview_stop()。 */

int camera_hd_jpeg_capture_verified(const uint8_t **jpeg, size_t *len,
                                    int x, int y, int w, int h,
                                    int max_tries);
int camera_preview_take(void);
int camera_preview_resume(void);
/* Wait for producer idle before motion; ticket0 means no running preview.
 * Resume returns1 only for this unchanged owner,0 for no/stale ownership. */
int camera_preview_motion_pause(unsigned *ticket);
int camera_preview_motion_resume(unsigned ticket);
int camera_preview_wait_new_frame(uint32_t before,unsigned timeout_ms);
int camera_preview_stop(void);

/* 预览线程是否在跑（相机页打开中）。网络自愈用它避让相机活动。 */
bool camera_preview_is_running(void);

/* 帧尺寸（与 camera_capture.c / esp32s3_cam_dvp.c 保持一致，勿漂移）
 *
 * 预览 320×240：直写 LCD 大取景框，按目标宽高比居中裁剪，避免拉伸。
 * 拍照 640×480：上传服务器做 AI 识别，配合高光去饱和；若粉块明显再降回
 * 320×240（2×2 binning）。 */

#define CAM_PREVIEW_W      320
#define CAM_PREVIEW_H      240
#define CAM_PREVIEW_BPP    2
#define CAM_PREVIEW_SIZE   (CAM_PREVIEW_W * CAM_PREVIEW_H * CAM_PREVIEW_BPP)

#define CAM_PHOTO_W        640
#define CAM_PHOTO_H        480
#define CAM_PHOTO_BPP      2
#define CAM_PHOTO_SIZE     (CAM_PHOTO_W * CAM_PHOTO_H * CAM_PHOTO_BPP)

/**
 * 返回最新完整预览帧（320×240 RGB565 小端，双缓冲之一）。
 * 供 LVGL 渲染线程引用；帧索引变化即新帧。
 */
const uint8_t *camera_preview_get_frame(void);
/* Single UI consumer: atomically replace its retained RGB frame. The returned
 * bytes remain immutable until the next acquire or release. Call on UI only,
 * outside rendering; release after the image is removed and preview stopped.
 * Producer conversion never holds the metadata mutex or waits for rendering. */
const uint8_t *camera_preview_acquire_frame(uint32_t *sequence);
void camera_preview_release_frame(void);

int camera_preview_get_frame_index(void);
/* Lifetime counter for rate windows spanning a preview restart. */
uint32_t camera_preview_get_total_frames(void);

#ifdef CONFIG_ARCH_BOARD_M5STACK_CORES3
int camera_preview_snapshot(const uint8_t **frame, size_t *len, unsigned timeout_ms);
int camera_preview_snapshot_test(const uint8_t **frame,size_t *len,unsigned timeout_ms,bool boost);
/* Differential test: official esp_image_effects object vs the Q14 lookup
 * implementation on the same completed frame, with AWB disabled. */
int camera_verify_official_convert(uint32_t *mismatches, uint32_t *pixels);
#endif

#ifdef __cplusplus
}
#endif

#endif /* __APPS_PLANT_COMPANION_CAMERA_CAPTURE_H */
