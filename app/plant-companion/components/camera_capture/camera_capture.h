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

/**
 * 配置 OV3660 输出 JPEG 320×240（官方寄存器序列）
 * 之后 DVP 数据线开始输出 JPEG 帧流，等待 camera_capture_frame() 收帧。
 * @return 0 成功；负 errno 失败
 */
int camera_config_jpeg_qvga(void);

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

/**
 * 动态预览（参考官方 dvp_spi_lcd）：后台线程连续 capture → 软件缩放 →
 * 直接写 LCD 区域。拍照 = camera_preview_take() 冻结（画面定格），
 * 退出页面 = camera_preview_stop()。
 */
int camera_preview_start(int x, int y, int w, int h);
int camera_preview_take(void);
int camera_preview_resume(void);
int camera_preview_stop(void);

/* 帧尺寸（与 camera_capture.c / esp32s3_cam_dvp.c 保持一致，勿漂移）
 *
 * 预览 160×120：直写 LCD 取景框（414×184），快字优先，模糊可接受。
 * 拍照 320×240：上传服务器做 AI 识别。2026-09-10 从 640×480 降下来：
 *   640×480 是逐像素输出（不 binning），传感器 AWB 给红 1.63× 增益 → 亮部
 *   红先撞顶、绿还有余量 → 粉色块 + 过曝（实测撞顶 5.70%）。320×240 走
 *   2×2 binning，亮部被平均后偏中性（撞顶约 1.5%），像素数仍是预览的 4 倍。 */

#define CAM_PREVIEW_W      160
#define CAM_PREVIEW_H      120
#define CAM_PREVIEW_BPP    2
#define CAM_PREVIEW_SIZE   (CAM_PREVIEW_W * CAM_PREVIEW_H * CAM_PREVIEW_BPP)

#define CAM_PHOTO_W        320
#define CAM_PHOTO_H        240
#define CAM_PHOTO_BPP      2
#define CAM_PHOTO_SIZE     (CAM_PHOTO_W * CAM_PHOTO_H * CAM_PHOTO_BPP)

/**
 * 返回最新完整预览帧（160×120 RGB565 小端，双缓冲之一）。
 * 供 LVGL 渲染线程引用；帧索引变化即新帧。
 */
const uint8_t *camera_preview_get_frame(void);
int camera_preview_get_frame_index(void);

#ifdef __cplusplus
}
#endif

#endif /* __APPS_PLANT_COMPANION_CAMERA_CAPTURE_H */
