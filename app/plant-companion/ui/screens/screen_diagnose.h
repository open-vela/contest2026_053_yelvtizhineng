/****************************************************************************
 * apps/plant-companion/ui/screens/screen_diagnose.h
 *
 * ③ AI诊断结果 — 病虫害分析（UI_SPEC ③）
 *   植物名称/拉丁名/匹配度/标签 + 警告框 + 建议
 ****************************************************************************/

#ifndef __PLANT_UI_SCREEN_DIAGNOSE_H
#define __PLANT_UI_SCREEN_DIAGNOSE_H

#include <lvgl/lvgl.h>

#ifdef __cplusplus
extern "C"
{
#endif

/**
 * 创建诊断结果页（全屏二级页，无底部 Tab，带 ← 返回）
 * @return 屏幕对象
 */
lv_obj_t *screen_diagnose_create(void);

/**
 * 填充诊断数据（3C 由 ai_service 提供；2026-09-09 起支持自定义提示框标题，
 * 服务器“未识别/失败”回复也照实展示，不再隐藏内容或伪装演示结果）
 */
void screen_diagnose_set(lv_obj_t *scr,
                         const char *name, const char *latin,
                         int match, const char *tags,
                         const char *section_title,
                         bool issue, const char *issue_desc,
                         const char *advice);

#ifdef __cplusplus
}
#endif

#endif /* __PLANT_UI_SCREEN_DIAGNOSE_H */
