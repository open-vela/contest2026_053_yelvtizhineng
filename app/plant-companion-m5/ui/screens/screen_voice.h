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
bool screen_voice_is_active(void);
void screen_voice_visibility_tick(void);
void screen_voice_set_system(int online,int battery);
void screen_voice_set_care(bool care);
void screen_voice_audit(void);
/* Explicit, idle-only 8s visual injection; no audio/input/session credit. */
int screen_voice_preview_request(int state);
int screen_voice_prepare_service(void);

/** 追加一条 AI 回复气泡（文本） */
void screen_voice_add_ai_reply(lv_obj_t *scr, const char *text);

/** 追加一条用户气泡（文本） */
void screen_voice_add_user_msg(lv_obj_t *scr, const char *text);

#ifdef __cplusplus
}
#endif

#endif /* __PLANT_UI_SCREEN_VOICE_H */
