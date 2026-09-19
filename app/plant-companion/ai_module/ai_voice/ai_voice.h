/****************************************************************************
 * apps/plant-companion/ai_module/ai_voice/ai_voice.h
 *
 * Voice agent: microphone capture (ES7210) + speaker playback (ES8311)
 * over the ESP32-S3 I2S0 bus, plus a WAV codec for the AI pipeline.
 *
 * Sample format: 16-bit signed PCM, stereo.
 * Native rate: 24000 Hz (matches xiaozhi BOX-3 已验证配置).
 * The AI stage (mimo full-modal) receives 16 kHz WAV; a resampler
 * is applied before encoding when the capture rate is 24 kHz.
 ****************************************************************************/

#ifndef __APPS_PLANT_COMPANION_AI_VOICE_H
#define __APPS_PLANT_COMPANION_AI_VOICE_H

#include <stdint.h>
#include <stddef.h>

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define AI_VOICE_SAMPLE_RATE     24000   /* xiaozhi BOX-3 已验证配置 */
#define AI_VOICE_CHANNELS        2       /* stereo */
#define AI_VOICE_BITS_PER_SAMPLE 16

/* AI-facing rate: speech models expect 16 kHz */
#define AI_VOICE_AI_RATE         16000

/* Max capture length for one utterance (seconds at 16 kHz) */
#define AI_VOICE_MAX_SECONDS     5
#define AI_VOICE_PCM16K_MAX      (AI_VOICE_AI_RATE * 2 * AI_VOICE_MAX_SECONDS)

/* Full WAV buffer: header + 16 kHz PCM (allocated at runtime) */
#define AI_VOICE_WAV_MAX         (44 + AI_VOICE_PCM16K_MAX)

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

/* Init I2C (codecs) + I2S0 (data). Safe to call multiple times. */

int ai_voice_init(void);

/* Record 'seconds' of microphone audio, downsample to 16 kHz and encode
 * as a WAV file in 'wav'.  Returns the WAV size in bytes, or a negative
 * errno.  'wav' must hold at least AI_VOICE_WAV_MAX bytes.
 */

int ai_voice_record(FAR uint8_t *wav, size_t wav_size, int seconds);

/* 流式录音（语音链路用，设备零大缓冲）：
 * 阻塞录音，每 ~100ms 把 16kHz mono PCM 块回调给 cb（不攒 WAV）。
 * 内部能量 VAD：持续静音 silent_blocks×100ms 后提前结束（0=禁用）。
 * 返回录音时长（毫秒）或负 errno。
 */

typedef void (*ai_voice_stream_cb_t)(FAR const int16_t *pcm16,
                                     size_t samples, void *arg);

int ai_voice_stream_record(ai_voice_stream_cb_t cb, void *arg,
                           int max_seconds, int silent_blocks);

/* 取消正在进行的流式录音（微信式随放随停：再点按键即停） */

void ai_voice_stream_cancel(void);

/* 清取消标志（播放/录音循环起算点）。
 * ⚠️ 2026-09-11：TTS 下载 → 播放之间清一次，避免「下载期间随便点了
 * 一下」把刚要开始的播放按 0 tick 掐掉（表现为喇叭完全没声）。 */

void ai_voice_stream_reset_cancel(void);

/* ⚠️ 2026-08-31 微信式「再点=说完」：结束录音（保留已录内容，正常发送），
 * 区别于 ai_voice_stream_cancel（取消=丢弃）。录音循环看到 finish 提前
 * break，返回已录音时长，voice_worker 照常 finalize 发给 AI。 */

void ai_voice_stream_finish(void);

/* Play a WAV file (16-bit PCM, any rate) through the speaker. */

int ai_voice_play(FAR const uint8_t *wav, size_t size);

/* Play a WAV file from storage (TTS audio can be hundreds of KB —
 * streamed in small chunks, never loaded whole into RAM). */

int ai_voice_play_file(FAR const char *wav_path);

/* ── 流式播放（边下边播，2026-09-16 时延修复）──────────────────────
 * 旧路径是「整条 TTS 音频先落 SD，再 fopen 播放」：795KB 的音频在 SD
 * 上只有 23.5KB/s，光下载就要 34s（同一文件不写卡 4.6s）——写卡把下载
 * 拖住了，是「说完话等 43s 才出声」的最大来源。
 * 新路径把网络收到的字节直接喂 I2S：
 *   begin()  重置解析状态 + 初始化 I2S
 *   push()   可任意切分（RIFF 头允许跨包）；返回 0=继续，-ECANCELED=
 *            用户点了停止（调用方据此中断下载），其它负值=音频格式坏
 *   end()    flush DMA + 打印播放统计
 * 采样率仍从 RIFF fmt 解析：16k 走 3:2 插值、24k 直通（同 play_file）。
 * ⚠️ 流式无法 seek，只能顺序解析（服务器 TTS 的 WAV 就是 fmt 在前、
 * data 在后）。内部 6.4KB 静态暂存同时充当起播前预缓冲（~200ms@16k）。 */

int ai_voice_stream_play_begin(void);
int ai_voice_stream_play_push(FAR const uint8_t *data, size_t len);
int ai_voice_stream_play_end(void);

/* 播放/录音循环是否收到过取消（voice_worker 用来区分「用户点了停止」
 * 和「真的失败了」—— 否则用户一停，兜底逻辑反而会把整条音频重下一遍）。 */

int ai_voice_stream_cancel_requested(void);

/* Play a sine tone (testing the DAC path without a file). */

int ai_voice_play_tone(uint32_t freq_hz, int ms);

/* Build a WAV header over 'pcm' (16 kHz mono 16-bit).  Returns size. */

int ai_voice_wav_encode(FAR uint8_t *wav, FAR const int16_t *pcm,
                        uint32_t pcm_bytes);

/* Decimate 48 kHz -> 16 kHz (boxing average of 3 samples). */

uint32_t ai_voice_decimate(FAR int16_t *out, FAR const int16_t *in48,
                           uint32_t in_samples);

/* Diagnostics: split the I2C and I2S links to isolate failures. */

int ai_voice_i2c_test(void);

#endif /* __APPS_PLANT_COMPANION_AI_VOICE_H */
