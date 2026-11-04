/****************************************************************************
 * apps/plant-companion/services/voice_service.h
 *
 * 语音业务服务：录音 → AI 全模态 → TTS 播放 状态机
 *
 * 设计（ARCHITECTURE.md §3.2）：
 *   - voice_task 后台执行录音（ES7210）与播放（ES8311），不阻塞 ui_task
 *   - 状态事件通过回调通知 UI（波形动画/气泡/状态文字）
 *   - 3C-2 阶段：录音+播放真实链路可独立验证；AI 对话文本先用本地
 *     规则生成（接入 MiMo 全模态时替换 ai_respond 实现）
 ****************************************************************************/

#ifndef __PLANT_SERVICES_VOICE_SERVICE_H
#define __PLANT_SERVICES_VOICE_SERVICE_H

#ifdef __cplusplus
extern "C"
{
#endif

#include <stdbool.h>
#include <stdint.h>
enum voice_ui_error {VOICE_UI_OK,VOICE_UI_NETWORK,VOICE_UI_SERVICE,VOICE_UI_MIC,VOICE_UI_NO_SPEECH};
struct voice_ui_snapshot {int state,error;uint32_t generation;bool busy,dialogue,cancelling,capture_ready,playback_active;};
void voice_service_ui_snapshot(struct voice_ui_snapshot *out);

/* 语音状态（UI 驱动波形/气泡用） */

#define VOICE_STATE_IDLE      0   /* 空闲 */
#define VOICE_STATE_LISTENING 1   /* 正在听你说... */
#define VOICE_STATE_THINKING  2   /* AI 思考中... */
#define VOICE_STATE_SPEAKING  3   /* 正在说话... */

typedef void (*voice_state_cb_t)(void *user_data, int state);
typedef void (*voice_text_cb_t)(void *user_data, const char *text,
                                bool from_user);

/**
 * 启动语音服务（初始化 voice_agent/ai_voice）
 * @param state_cb 状态回调（voice 线程触发；UI 需投递到 ui_task）
 * @param text_cb  文本回调（用户语音识别文本 / AI 回复文本）
 * @param user_data 透传
 * @return 0 成功；负值失败
 */
int voice_service_start(voice_state_cb_t state_cb,
                        voice_text_cb_t text_cb, void *user_data);
/* Replace the current streamed answer with its completed text, not a new turn. */
void voice_service_set_text_update_cb(voice_text_cb_t update_cb);

/** 停止并释放 */
void voice_service_stop(void);

/**
 * 触发一轮对话：录音 N 秒 → AI 回复 → TTS 播放
 * @param seconds 录音时长（1-5）
 * @return 0 已开始；负值失败
 */
int voice_service_talk(int seconds);
/* Wake once, then listen again after each reply; 30s without speech exits. */
int voice_service_dialogue_start(void);
/* Real keyword only: local acknowledgement, then immediate RX. */
int voice_service_wake_start(void);
bool voice_service_dialogue_active(void);
void voice_service_dialogue_poll(bool blocked); /* UI thread */
int voice_service_say(const char *text);
/* Explicit, nonblocking synthetic QA/camera/flow diagnostic. Stop via abort. */
int voice_service_fixture_start(unsigned program);
/* Diagnostic only: -1 read;0 original scheduling;110 experimental, idle only. */
int voice_service_priority_diag(int value);

/* Fixed-duration talk session for microphone/server diagnostics.
 * silent_blocks=0 disables VAD auto-stop. */

int voice_service_talk_ex(int seconds, int silent_blocks);

/** 当前状态 */
int voice_service_get_state(void);

/** 是否有一轮会话正在跑（控制台 plant voice talk 回归用） */
int voice_service_busy(void);

/**
 * ⚠️ 2026-08-31 立即中止进行中的会话（页面销毁时调用）：
 * 停止录音 / 停止播放 / 丢弃结果，worker 尽快退出。
 * 与 talk() 的「再点=说完」不同，这是强制取消（返回页面场景）。
 */
void voice_service_abort(void);

/**
 * ⚠️ 2026-09-16 会话看门狗（"界面永远停在聆听"的最终保险）：
 * 由 ui_task 的 1 秒定时器周期调用。任何阶段超过硬上限（录音 20s /
 * 思考 110s / 播放 240s）就强制清会话回空闲 —— 用户再点一次即可重来，
 * 不必重启板卡。放在 ui_task 是因为它最稳且界面就在这里。
 */
void voice_service_watchdog_poll(void);

#ifdef __cplusplus
}
#endif

#endif /* __PLANT_SERVICES_VOICE_SERVICE_H */
