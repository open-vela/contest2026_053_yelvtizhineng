/****************************************************************************
 * apps/plant-companion/ui/assets/fonts/zh_font.h
 *
 * 中文字体统一加载层（CLAUDE_SYSTEM.md §14）
 *
 * 两级字体策略（2026-09-02 修复 SD 整体切换回归后）：
 *   - 主字体 = flash 子集（lv_font_plant_zh_16/20/24，XIP 直读零拷贝，
 *     含 ASCII + 250 个静态 UI 汉字）→ 时间/数字/静态文案不碰 SD；
 *   - fallback = SD 全量字库（/mnt/sd/fonts/plant_zh_*.bin，GB2312 6763 字，
 *     自定义 lv_font_t 流式二分 + LRU 缓存 ≤10KB）→ 子集外动态汉字兜底。
 *
 * 设计：UI 只经 zh_font_get() 拿字体，不直接引用字体符号 —— 数据源切换
 * 不碰业务代码；zh_font_check_text() 供缺字诊断（plant font status）。
 ****************************************************************************/

#ifndef __PLANT_UI_ASSETS_FONTS_ZH_FONT_H
#define __PLANT_UI_ASSETS_FONTS_ZH_FONT_H

#include <lvgl/lvgl.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C"
{
#endif

/* 数据源类型（zh_font_src 语义：SD 兜底是否可用） */

typedef enum
{
  ZH_SRC_FLASH_SUBSET = 0,   /* 仅 flash 子集（250 汉字 + ASCII，SD 缺失/拔卡） */
  ZH_SRC_SD_FULL = 1         /* flash 子集主字体 + SD 全量 GB2312 兜底 */
} zh_font_src_t;

/** 初始化（幂等，可反复调用重试）。检测 SD 全量字库 → 挂上主字体 fallback；
 *  缺失回退仅 flash 子集（等价阶段 A）。 */
void zh_font_init(void);

/** 绑定当前任务为字体归属任务（= UI 线程）并初始化：必须由 UI 线程在
 *  lv_init 之后调用一次。之后其它任务（NSH 的 plant font status/trace/sd）
 *  调 zh_font_init() 一律直接返回——SD 字库句柄按任务组隔离，跨任务重开
 *  会把 UI 的 fallback 链换成本任务的 FILE*，实测直接 panic。 */
void zh_font_bind_ui(void);

/** UI 上下文重试挂 SD 全量字库（SD 挂载晚于 UI 启动）：只能由归属任务调用，
 *  返回 true = SD 兜底已就绪。最多重试 12 次后放弃（不再打日志）。 */
bool zh_font_retry_ui(void);

/** 当前字体策略（SD 兜底是否就绪） */
zh_font_src_t zh_font_src(void);

/** 统一字体入口：16/20/24 → lv_font_t*（flash 主字体，含 SD fallback）；
 *  不支持的字号返回 NULL。 */
const lv_font_t *zh_font_get(int size_px);

/** 当前策略下 size_px 字体可覆盖字形数（SD 兜底就绪 ≈6763；否则 ≈250） */
int zh_font_count(int size_px);

/** 单字符是否有字形（lv_font_get_glyph_dsc 封装；false = 显示方框） */
bool zh_font_has_glyph(const lv_font_t *font, uint32_t letter);

/** 缺字诊断：统计 text 中在 size_px 字体下无字形的字符数（方框数）。
 *  0 = 全覆盖；text 为 NULL 返回 0。 */
int zh_font_check_text(int size_px, const char *text);

/** 调试（2026-09-10）：逐字符打印字形解析结果（主字体 / SD兜底 /
 *  符号emoji / 缺=方框），用于把屏幕上的方框定位到具体字与层级。 */
void zh_font_trace_text(int size_px, const char *text);

/** 调试：打印某码点在 SD 兜底里的完整查找过程（fseek/fread/errno）。 */
void zh_font_diag_sd(uint32_t cp);

/** 调试：取文本首个码点（UTF-8）。 */
uint32_t zh_font_utf8_first(const char *text);

/** 文本净化：把"设备渲染不出字形"的字符剔掉，避免屏幕上出现一串方框。
 *  适用于服务器/模型回传的任意文本（AI 回复、识别结果、语音转写）。
 *  规则：变体选择符/零宽连接符等"没有独立字形"的修饰符直接丢；
 *  其余字符在字体链（flash 子集 + SD 全量汉字 + 符号 + emoji）里查不到字形就丢。
 *  注意：SD 兜底句柄归 ui_task，本函数只能在 UI 上下文调用。 */
void zh_text_sanitize(char *dst, size_t dstsz, const char *src);

/** 同上，直接给 label 设文本（内部先净化）。 */
void zh_label_set_text_safe(lv_obj_t *label, const char *text);

#ifdef __cplusplus
}
#endif

#endif /* __PLANT_UI_ASSETS_FONTS_ZH_FONT_H */
