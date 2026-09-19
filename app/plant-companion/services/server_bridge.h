/****************************************************************************
 * apps/plant-companion/services/server_bridge.h
 *
 * 服务器大脑客户端（设备侧）——语音/图像/上报/同步统一走新服务器
 * （plant_server，FastAPI，前缀 /api/v1，设备凭证 X-Device-Token）。
 *
 * 协议 v2 概览：
 *   POST /api/v1/devices/register（SN→device_token，幂等）
 *   POST /api/v1/voice/session|upload|finalize / GET /audio（会话制 PCM+TTS）
 *   POST /api/v1/media/image?fmt=raw565 → media_id
 *   POST /api/v1/ai/diagnose {media_id} → 结构化 result
 *   POST /api/v1/telemetry/put（单条传感器）
 *   GET  /api/v1/time
 *   POST /api/v1/devices/heartbeat（60s 心跳：保活 + 取服务器配置）
 *
 * 依赖：裸 socket（明文 HTTP，与 ai_common 同模式但 host/port 可配）
 ****************************************************************************/

#ifndef __PLANT_SERVICES_SERVER_BRIDGE_H
#define __PLANT_SERVICES_SERVER_BRIDGE_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C"
{
#endif

/* 配置服务器地址（设备通过 WiFi 连局域网内的服务器辅助手） */

int server_bridge_set_server(const char *host, uint16_t port);

/* 设备身份（参赛演示版：SN 即凭证）。server_bridge_set_server 成功后
 * 会自动向服务器注册并保存 device_token（无需额外调用）；也可手动重登。 */

int server_bridge_device_login(void);

/* 查询当前服务器地址与设备凭证（调试打印用） */

const char *server_bridge_host(void);
const char *server_bridge_device_token(void);

/* 是否已配置且已注册（服务器地址 + 设备凭证都在）——周期任务据此决定
 * 要不要真的发网络请求（未联网时不发，避免白跑 + 刷错误日志）。 */

int server_bridge_configured(void);

/* 3C-2/B 实时时间：GET /time → 北京时间（UTC+8）。
 *   unix_beijing 填北京时间 unix 秒（已偏移，设备据此本地走时：
 *   抓一次后 = unix + (tick差值)/TICK_PER_SEC 推算，每小时校准一次，
 *   顶栏每分钟自然跳——方案 F，2026-09-01）；
 *   date_out 填 "YYYY-MM-DD"（阶段 C 任务/日记按日期筛选用），
 *   time_out 填 "HH:MM"。
 * 返回 0 成功；负 errno 失败（-ENOTCONN=未配置服务器）。 */

int server_bridge_get_time(int64_t *unix_beijing,
                           char *date_out, size_t date_size,
                           char *time_out, size_t time_size);

/* 服务器配置版本号：每次 server_bridge_set_server 递增。时间 worker
 * 检测到变化立即重新抓取（"配置即抓"，避免等轮询周期——解决
 * "连上网了却没时间"的时间差缺陷）。 */

int server_bridge_config_epoch(void);

/* 开始一轮会话（语音上传）：向服务器建会话。
 * 返回 0 成功；负 errno 失败（-ENOTCONN=未配置服务器）。 */

int server_bridge_session_begin(void);

/* 上传一块 PCM16（16kHz mono）到当前会话，seq 自动递增 */

int server_bridge_upload(const int16_t *pcm, size_t samples);

/* 结束会话：服务器攒 WAV → MiMo 理解 + TTS → 返回
 *   text_out 填回复文本（<=text_size 字节）
 *   has_audio 输出：服务器是否有 TTS 音频可下载
 * 返回 0 成功；负 errno 失败 */

int server_bridge_finalize(char *text_out, size_t text_size,
                           bool *has_audio);

/* 下载 TTS 音频到文件（GET /voice/audio，边收边写 filepath，
 * 4KB 块缓冲——TTS 音频可达数百 KB，不整段进内存）。
 * 返回写入字节数；负 errno 失败。 */

int server_bridge_fetch_audio(const char *filepath);

/* 按服务器路径直接下载到文件（调试：plant voice get <path> <file>）。
 * 例："/api/v1/media/md_xxx/file" -> "/mnt/sd/tts_test.wav"。
 * 返回写入字节数；负 errno 失败。 */

int server_bridge_fetch_path(const char *path, const char *filepath);

/* 当前语音会话的 TTS 音频路径（"/api/v1/voice/audio?session_id=..."）。
 * 返回 0 成功；-ENOTCONN = 还没建会话 / 没配服务器。 */

int server_bridge_voice_audio_path(char *buf, size_t cap);

/* 流式下载回调：每收到一块响应体就调一次（不落盘）。
 * 返回 0 继续；返回 <0 → 立即中止整条下载（用于「用户点了停止」）。
 * ⚠️ 回调在调用线程（语音 worker）里同步执行，别做耗时操作。 */

typedef int (*server_bridge_stream_cb_t)(const uint8_t *data, size_t len,
                                         void *arg);

/* 2026-09-16 时延修复：流式 GET —— 边下边播的地基。
 *
 * 旧路径是「先整条下载到 SD，再打开播放」：实测 795KB 的 TTS 音频在
 * SD 上只有 23.5KB/s（写卡把下载拖住），光下载就要 34s；同一文件不写
 * 卡只用 4.6s。所以播放这条线不能再碰 SD。
 *
 * stall_timeout_ms 是「多久收不到新数据才算断」（不是总时长 —— 回复
 * 本身可能几十秒）；max_bytes 是防御性上限，超了按失败处理。
 * 返回交付给回调的总字节数；负 errno 失败。 */

int server_bridge_get_stream(const char *path,
                             server_bridge_stream_cb_t cb, void *arg,
                             int stall_timeout_ms, size_t max_bytes);

/* ⚠️ 旧协议已废弃（直连服务器旧辅助手 /image/analyze）。下面两个为协议 v2：
 *   ① POST /api/v1/media/image（raw565 38400B）→ media_id
 *   ② POST /api/v1/ai/diagnose → 结构化 result（名称/健康分/问题/建议） */

/* 图像诊断结构化结果（设备屏显与日志共用） */

struct sb_diag_result_s
{
  bool  ok;                 /* 服务器返回有效诊断 */
  char  name[64];           /* 植物俗名（UTF-8） */
  char  latin[96];          /* 拉丁名（可为空） */
  int   match;              /* 匹配度 0-100；<0=未知 */
  int   health_score;       /* 健康分 0-100；<0=未知 */
  char  issue[288];         /* 发现的问题（多行 \n 连接，空=没发现问题） */
  char  advice[384];        /* 养护建议（多行 \n 连接，可为空） */
  char  summary[288];       /* 一句话总结 */
};

/* 协议 v2 图像诊断（两步：media/image → ai/diagnose），结构化返回。 */

int server_bridge_image_diagnose(const uint8_t *frame, size_t frame_len,
                                 struct sb_diag_result_s *out);

/* 兼容壳：把结构化诊断拼成一段文本（换行分段，UI 旧逻辑可直接显示） */

int server_bridge_image_analyze(const uint8_t *frame, size_t frame_len,
                                char *text_out, size_t text_size);

/* 传感器单条上报：POST /api/v1/telemetry/put（土壤 8 参数）。
 *   moisture % / temp ℃ / ec μS/cm / ph / salt / 氮 / 磷 / 钾 (mg/kg)
 * 服务器按 (device, ts) 幂等；返回 0 成功；负 errno 失败
 * （-ENOTCONN = 还没配置服务器或还没注册）。 */

int server_bridge_report_telemetry(float moisture, float temp, float ec,
                                   float ph, float salt, float nitrogen,
                                   float phosphorus, float potassium);

/* 心跳保活：POST /api/v1/devices/heartbeat（空 JSON 体，自动带
 * X-Device-Token）。服务器据此刷新 last_seen_at → 管理台在线判定；
 * 响应 config.heartbeat_s 回填 interval_s_out（服务器可远程调心跳周期）。
 * 未配置服务器或尚未注册（无 token）返回 -ENOTCONN，不发请求。
 * 返回 0 成功；负 errno 失败。 */

int server_bridge_heartbeat(int *interval_s_out);

/* 服务器下发的传感器上报周期（秒）；0 = 未拿到 */
int server_bridge_upload_interval_s(void);


/* ── 执行器（自动执行层，2026-09-12）──────────────────────────────────────
 * 板卡侧三步协议（服务器见 plant_server app/routers/actuator.py）：
 *   ① POST /api/v1/actuators/capabilities   声明板卡自带哪几路（幂等）
 *   ② GET  /api/v1/actuators/pending        取服务器排队的指令
 *   ③ POST /api/v1/actuators/jobs/{id}/ack  执行完回执（服务器写成长日记）
 * 三个请求都自动带 X-Device-Token；未配置服务器/还没注册返回 -ENOTCONN，
 * 不发请求。上层驱动见 services/actuator_service.c。
 */

/* 声明用：一路执行器的类型与展示名 */

struct sb_act_cap_s
{
  const char *kind;   /* water / light / fan / feeder / heat（对齐服务器 KINDS） */
  const char *name;   /* 展示名（UTF-8），App/管理台直接显示 */
};

/* 服务器下发的单条指令 */

struct sb_act_job_s
{
  char job_id[40];        /* "aj_xxx"：回执时原样带回 */
  char actuator_id[40];   /* "ac_xxx" */
  char kind[16];          /* water / light / … */
  char action[20];        /* run（手动）/ auto（自动规则） */
  int  duration_s;        /* 持续秒数（服务器已填默认值） */
  char reason[72];        /* 下发原因（可在 App/管理台/日记里显示） */
};

/* 声明板卡自带哪几路执行器（幂等，可反复调）。
 * 返回 0 成功；负 errno 失败（-ENOTCONN=未配置服务器/无 token）。 */

int server_bridge_declare_capabilities(const struct sb_act_cap_s *caps, int n);

/* 取待执行指令。out 是调用方给的数组（最多 max 条），n_out 填实际条数。
 * 返回 0 成功（n_out 可能为 0，表示暂时没活）；负 errno 失败。
 * 响应缓冲 1.5KB 静态：不要在多线程里并发调用 server_bridge 系列函数。 */

int server_bridge_fetch_pending(struct sb_act_job_s *out, int max, int *n_out);

/* 执行完回执。ok=0 表示执行失败（服务器会记成失败并保持执行器告警态）。
 * detail 只放可读的 ASCII 摘要（服务器截前 200 字符写进日记）。
 * 返回 0 成功；负 errno 失败。 */

int server_bridge_ack_job(const char *job_id, int ok, const char *detail);

#ifdef __cplusplus
}
#endif

#endif /* __PLANT_SERVICES_SERVER_BRIDGE_H */
