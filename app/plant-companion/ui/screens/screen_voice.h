/****************************************************************************
 * apps/plant-companion/ui/screens/screen_voice.h
 *
 * ⑥ 语音对话 — 和小绿聊天（UI_SPEC ⑥）
 *   波形动画 + 对话气泡 + 🎙️ 说话按钮
 ****************************************************************************/

#ifndef __PLANT_UI_SCREEN_VOICE_H
#define __PLANT_UI_SCREEN_VOICE_H

#include <lvgl/lvgl.h>

#ifdef __cplusplus
extern "C"
{
#endif

/**
 * 创建语音页（全屏二级页）
 * @return 屏幕对象
 */
lv_obj_t *screen_voice_create(void);

/** 追加一条 AI 回复气泡（文本） */
void screen_voice_add_ai_reply(lv_obj_t *scr, const char *text);

/** 追加一条用户气泡（文本） */
void screen_voice_add_user_msg(lv_obj_t *scr, const char *text);

#ifdef __cplusplus
}
#endif

#endif /* __PLANT_UI_SCREEN_VOICE_H */
