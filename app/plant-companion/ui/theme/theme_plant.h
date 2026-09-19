/****************************************************************************
 * apps/plant-companion/ui/theme/theme_plant.h
 *
 * 植小伴设计系统 —— 设计 Token（颜色/圆角/间距/字体）
 * 2026-09-10 起对齐《嵌入式 UI V3（大字版）》设计稿：
 *   - 取消全局状态栏（V3 每页自绘顶部行）
 *   - 底部 Tab 由 64px 降为 52px
 *   - 字号四档：12（辅助标签）/16（正文）/19（标题）/25（数值大字）
 ****************************************************************************/

#ifndef __PLANT_UI_THEME_PLANT_H
#define __PLANT_UI_THEME_PLANT_H

#include <lvgl/lvgl.h>

#ifdef __cplusplus
extern "C"
{
#endif

/****************************************************************************
 * 色彩规范（V3 每页一个主题色）
 ****************************************************************************/

#define TP_CLR_GREEN            0x16a34a   /* 主绿 */
#define TP_CLR_GREEN_DARK       0x166534   /* 深绿（标题字） */
#define TP_CLR_GREEN_DEEP       0x14532d   /* 更深绿 */
#define TP_CLR_GREEN_LIGHT      0x4ade80   /* 亮绿（进度环/图标底） */
#define TP_CLR_BLUE             0x2563eb   /* 数据蓝 */
#define TP_CLR_BLUE_LIGHT       0x60a5fa
#define TP_CLR_AMBER            0xf59e0b   /* 任务黄 */
#define TP_CLR_AMBER_DARK       0xd97706
#define TP_CLR_AMBER_TEXT       0x92400e   /* 黄底上的字 */
#define TP_CLR_PINK             0xec4899   /* 语音粉 */
#define TP_CLR_PINK_DARK        0xdb2777
#define TP_CLR_PINK_TEXT        0xbe185d
#define TP_CLR_RED              0xef4444
#define TP_CLR_PURPLE           0x7c3aed
#define TP_CLR_BROWN_TEXT       0x78350f

#define TP_CLR_TEXT_MAIN        0x1e293b   /* 主文字 */
#define TP_CLR_TEXT_SUB         0x6b7280   /* 次要文字 */
#define TP_CLR_TEXT_MUTED       0x9ca3af   /* 最弱文字（未选中 Tab/时间） */
#define TP_CLR_TEXT_ON_PRIMARY  0xffffff   /* 主题色上的文字 */
#define TP_CLR_CARD_BG          0xffffff   /* 卡片底色 */
#define TP_CLR_LINE             0xe5e7eb   /* 分隔线/浅底 */

#define TP_CLR_STATE_OK         0x22c55e   /* 状态好 */
#define TP_CLR_STATE_WARN       0xf59e0b   /* 状态警示 */
#define TP_CLR_STATE_BAD        0xef4444   /* 状态差 */

/* 每页背景渐变（V3 设计稿取色） */

#define TP_CLR_HOME_BG_A        0xdcfce7
#define TP_CLR_HOME_BG_B        0xecfdf5
#define TP_CLR_CAMERA_BG        0x0f172a   /* 拍照页深底 */
#define TP_CLR_RESULT_BG_A      0xf0fdf4
#define TP_CLR_RESULT_BG_B      0xdcfce7
#define TP_CLR_DATA_BG_A        0xeff6ff
#define TP_CLR_DATA_BG_B        0xf0fdf4
#define TP_CLR_TASK_BG_A        0xfef3c7
#define TP_CLR_TASK_BG_B        0xfffbeb
#define TP_CLR_VOICE_BG_A       0xfdf2f8
#define TP_CLR_VOICE_BG_B       0xfce7f3
#define TP_CLR_DIARY_BG_A       0xecfdf5
#define TP_CLR_DIARY_BG_B       0xf0fdf4

/* 底部 Tab 底色（每页同色系淡底） */

#define TP_CLR_NAV_BG_GREEN     0xf0fdf4
#define TP_CLR_NAV_BG_BLUE      0xeff6ff
#define TP_CLR_NAV_BG_AMBER     0xfffbeb
#define TP_CLR_NAV_BG_PINK      0xfdf2f8

/* 兼容旧名（历史代码引用） */

#define TP_CLR_PRIMARY_GREEN_DARK   TP_CLR_GREEN
#define TP_CLR_PRIMARY_GREEN_LIGHT  TP_CLR_GREEN_LIGHT
#define TP_CLR_DATA_BLUE            TP_CLR_BLUE
#define TP_CLR_TASK_YELLOW          TP_CLR_AMBER
#define TP_CLR_VOICE_PINK           TP_CLR_PINK
#define TP_CLR_BG_GRAD_A            TP_CLR_HOME_BG_A
#define TP_CLR_BG_GRAD_B            TP_CLR_HOME_BG_B
#define TP_CLR_TOPBAR_BG            TP_CLR_CARD_BG
#define TP_CLR_NAV_BG               TP_CLR_NAV_BG_GREEN
#define TP_CLR_NAV_ACTIVE           TP_CLR_GREEN
#define TP_CLR_NAV_INACTIVE         TP_CLR_TEXT_MUTED

/****************************************************************************
 * 圆角 / 间距 / 尺寸
 ****************************************************************************/

#define TP_RADIUS_CARD          16         /* 大卡片圆角（V3 hero 卡 16） */
#define TP_RADIUS_SMALL         12         /* 小卡片/按钮圆角 */
#define TP_RADIUS_TILE          14         /* 大按钮圆角 */
#define TP_PAD_PAGE             12         /* 页面左右边距（V3 一律 12） */
#define TP_PAD_CARD             12         /* 卡片内边距 */
#define TP_GAP_CARD             6          /* 卡片间距 */
#define TP_TOP_BAR_H            32         /* V3 页面顶部行高度（自绘，非状态栏） */
#define TP_NAV_BAR_H            52         /* 底部 Tab 高度（V3：52） */
#define TP_CONTENT_W            480        /* 屏幕宽 */
#define TP_CONTENT_H            (320 - TP_NAV_BAR_H)   /* 有 Tab 页的内容区高 = 268 */
#define TP_TOUCH_MIN            48         /* 触摸命中区 ≥48px */

/****************************************************************************
 * 字体（theme_plant_init 时经 zh_font_get 解析：flash 子集主字体 + SD 兜底）
 *
 * V3 四档（flash 子集，含静态 UI 全部用字 + ASCII）：
 *   12px 辅助标签 / Tab 文字
 *   16px 正文（任务名、诊断描述、对话气泡…）
 *   19px 卡片标题、页面主标题（植物名…）
 *   25px 数值大字（62% / 24°C / 1200lux）
 ****************************************************************************/

extern const lv_font_t lv_font_plant_zh_12;
extern const lv_font_t lv_font_plant_zh_16;
extern const lv_font_t lv_font_plant_zh_19;
extern const lv_font_t lv_font_plant_zh_25;

extern const lv_font_t *g_font_caption;   /* 12px */
extern const lv_font_t *g_font_body;      /* 16px */
extern const lv_font_t *g_font_title;     /* 19px */
extern const lv_font_t *g_font_value;     /* 25px */

#define TP_FONT_CAPTION (g_font_caption)
#define TP_FONT_BODY    (g_font_body)
#define TP_FONT_TITLE   (g_font_title)
#define TP_FONT_VALUE   (g_font_value)
#define TP_FONT_BIG     (g_font_value)   /* 兼容旧名 */

/* 图标字体（单色 emoji 子集，OpenMoji）。
 * ⚠ 只用于"纯图标 label"；中英混排/带文字的行请用 TP_FONT_* —— 主字体
 *   的 fallback 链（montserrat 符号 → emoji）会自动兜底，不会出方块。 */

extern const lv_font_t lv_font_emoji_16;
extern const lv_font_t lv_font_emoji_20;
extern const lv_font_t lv_font_emoji_24;
extern const lv_font_t lv_font_emoji_32;

#define TP_ICON_S        (&lv_font_emoji_16)   /* ~15-16px 小图标 */
#define TP_ICON_M        (&lv_font_emoji_20)   /* 20px Tab 图标 */
#define TP_ICON_L        (&lv_font_emoji_24)   /* 22-24px 卡片图标 */
#define TP_ICON_XL       (&lv_font_emoji_32)   /* 32px 植物头像 */

/****************************************************************************
 * 全局样式（theme_plant.c 中初始化，ui_app_start 时调用一次）
 ****************************************************************************/

void theme_plant_init(void);

extern lv_style_t theme_style_card;        /* 白底圆角卡片（默认阴影） */
extern lv_style_t theme_style_card_small;  /* 小卡片 */
extern lv_style_t theme_style_text_main;   /* 主文字 */
extern lv_style_t theme_style_text_sub;    /* 次要文字 */
extern lv_style_t theme_style_text_big;    /* 大数字 */
extern lv_style_t theme_style_btn_primary; /* 主按钮（绿底白字圆角） */

/** LVGL v9 按钮文字辅助（v9 的 lv_button 是纯容器，需手动加 label）。 */
void theme_btn_set_text(lv_obj_t *btn, const char *text);

/**
 * 页面背景：整屏垂直渐变 + 铺满（V3 每页不同色）。
 * @param scr 屏幕对象
 * @param c1  渐变起色
 * @param c2  渐变止色
 */
void theme_page_bg(lv_obj_t *scr, uint32_t c1, uint32_t c2);

#ifdef __cplusplus
}
#endif

#endif /* __PLANT_UI_THEME_PLANT_H */
