/****************************************************************************
 * apps/plant-companion/ui/ui_app.h
 *
 * 植小伴 UI 入口：lv_init + lv_nuttx_init + 屏幕管理 + ui_task 主循环
 * 复用 NuttX 官方 LVGL 移植（/dev/lcd0 + /dev/input0），无需自写移植层。
 ****************************************************************************/

#ifndef __PLANT_UI_APP_H
#define __PLANT_UI_APP_H

#include <lvgl/lvgl.h>

#ifdef __cplusplus
extern "C"
{
#endif

/**
 * 启动产品模式 UI（阻塞运行，直到退出）。
 * 内部：lv_init → lv_nuttx_init → theme_plant_init → screen_home 首屏
 *       → 进入 lv_timer_handler 主循环。
 * @return 0 正常退出；负值错误
 */
int ui_app_start(void);

/**
 * 停止 UI 主循环（供调试命令/退出使用，可选）。
 */
void ui_app_stop(void);

/**
 * 全屏二级页管理（②③⑥ 拍照/诊断/语音）：
 *   ui_app_push_screen：压入二级页（隐藏 4-Tab，显示二级页）
 *   ui_app_pop_screen ：弹出返回上一级（默认回首页）
 */
void ui_app_push_screen(lv_obj_t *scr);
void ui_app_pop_screen(void);

/**
 * 调试截屏：请求 ui_task 用 LVGL snapshot 把当前屏幕渲染进 buf 并写成 BMP。
 * buf 由调用方分配（建议系统堆/PSRAM），本函数会阻塞等待 ui_task 完成
 * （最多约 10s）。BMP 为 16bpp RGB565（BI_BITFIELDS），行序自上而下。
 * @return 0 成功；负值失败
 */
int ui_app_capture_to_file(const char *path, void *buf, size_t bufsize);

/**
 * 调试截屏转 ASCII（plant capa [x y w h]）：与 ui_app_capture_to_file 相同的
 * LVGL snapshot 流程，但不写文件，而是把 RGB565 缓冲按像素块映射成字符后直接
 * 打印到串口 —— 用于快速目检屏幕真实字形（绕开 SD 大批量写卡死与摄像头视角）。
 * x/y/w/h 为可选窗口（LCD 像素，缺省整屏 480x320，坐标会被钳制到屏内）。
 * @return 0 成功；负值失败
 */
int ui_app_capture_ascii(void *buf, size_t bufsize,
                         int x, int y, int w, int h);

/**
 * 调试对象树转储（plant objs）：请求 ui_task 遍历当前可见屏对象树，
 * 打印每个 label 的几何/字体指针/文本及 UTF-8 码点 —— 用于把屏幕上的
 * "方框/缺字"精确定位到具体 label 与字符。
 * @return 0 成功；负值失败
 */
int ui_app_dump_objects(void);

/** 调试（plant objs miss）：只打印存在无字形字符的
 *  label（带 * 标记与缺字数），用于定位屏幕方框。 */
int ui_app_dump_missing(void);

/**
 * 请求切换 4-Tab 页面（调试：plant nav <home|data|tasks|diary>）。
 * 非阻塞：由 ui_task 定时器实际执行，切换完成约需 <1s。
 * @return 0 接受；-1 参数非法
 */
int ui_app_nav_to(int tab_id);

/**
 * 调试模拟点按（plant tsim <cycles>）：在 ui_task 内用一路额外 LVGL
 * pointer indev 反复"按下/抬起"底部导航 Tab（每次切到下一页），完整走
 * LVGL 输入事件管线，专用于复现"跑一阵后触摸切换失效/卡住"。
 * @return 0 启动；-1 UI/indev 未就绪；-2 已在运行
 */
int ui_app_tsim_start(int cycles);

/** 停止模拟点按循环 */
void ui_app_tsim_stop(void);

/**
 * 调试单点触摸（plant tap <x> <y>）：在屏幕坐标注入一次真实点按。
 * @return 0 已投递；-1 UI 未就绪；-2 上一次点按还没松手
 */
int ui_app_tap_at(int x, int y);

/** 剩余次数；未在运行返回 -1 */
int ui_app_tsim_left(void);

/**
 * 真实触摸诊断（plant tdiag）：打印一行双端计数（GT911 驱动侧
 * polls/down/up/I2C错误 + LVGL 侧真实触摸按下次数）。
 * @param mode 1=每 ~2s 周期打印 0=停止周期打印 2=只打印一次
 */
void ui_app_tdiag_ctl(int mode);

/**
 * 切页内存打点（plant memlog on|off）：开启后每次真正切页完成
 * （延迟删除已冲刷）时由 ui_task 打印一行系统堆余量 + 相对首页基线
 * 增量。默认关；仅调试时按需开启。
 * @param on 1=开 0=关
 */
void ui_app_memlog_ctl(int on);

/**
 * 显示测试图片（plant img show/test）：全屏置顶显示 RGB565 缓冲。
 * 只在 ui_task 定时器上下文操作 LVGL 对象；调用方可从任意任务触发。
 * rgb565 缓冲区须保持有效直到下一次更新（如 rxbuf 静态区）。
 */
void ui_app_show_image(const uint8_t *rgb565, int w, int h);
void ui_app_hide_image(void);

#ifdef __cplusplus
}
#endif

#endif /* __PLANT_UI_APP_H */
