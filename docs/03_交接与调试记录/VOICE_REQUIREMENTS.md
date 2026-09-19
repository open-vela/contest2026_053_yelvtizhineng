# ⑥ 说话功能需求文档（语音对话：流式录音→AI→流式TTS→文本字幕）

> 状态：**需求梳理 + 流式架构定案**（2026-08-29）。目标读者：接下来实现"说话功能"的开发者。
> 配套历史文档：`components/voice_agent/AUDIO_HANDOVER_NEXT_AI.md`（音频链路调试交接，
> 含 I2S 过采样根因）、`ai_module/VOICE_WORKLOG.md`、`ai_module/ai_voice/VOICE_DEBUG.md`。
> 摄像头同款工作流参考：`CAMERA_HANDOVER.md`（结构/判读方式可模仿）。
> 系统资源/调度/内存预算：`CLAUDE_SYSTEM.md`（堆 ~112KB 是硬约束）。

---

## 0. 一句话目标

**点击首页「🎙️ 说话」→ 语音对话页 → 点「说话」→ DMA 流式录音（ES7210 麦克风）
→ 流式发送给 MiMo 全模态 → 回复文本上屏（"和小绿聊天"气泡）+ 回复语音
DMA 流式播放（ES8311 喇叭）→ 拟人化语音聊天。**

**核心原则（用户定案，2026-08-29）：不整段缓冲音频 + 设备零编解码。**
- 录音侧：`ai_voice.c` 已是 **DMA 100ms 分块**（PCM16，3.2KB/块）——**边录边发**，
  不再攒 160KB WAV，**不设备端 OPUS 编码**。
- TTS 侧：回复音频**流式播放**（边收边播，缓冲 1-2 块）；30s 语音 ≈ 960KB
  永不整段落内存。
- 字幕：全模态模型返回文本（用户确认）→ 回复文本直接上屏气泡。

**架构定案（2026-08-29 修订，推翻 OPUS 方案）：设备薄客户端 + PCM 流式直传 + WS 全双工 + 服务器辅助手。**
- **为什么不用 OPUS**（用户批评，我复盘）：OPUS 省带宽，但我们的瓶颈是**内存不是带宽**。
  设备端 OPUS 编码器需要一次性 23-26KB 大块分配，而当前堆 `largest≈10KB` → **物理分配不出**，
  为省一个 WiFi 不在乎的带宽（16kHz PCM=256kbps ≪ WiFi 带宽）去花 40KB 内存 = 方向反了。
  **正确的取舍是用带宽换内存**（用户一直坚持的方向）。
- **新方案**：
  ```
  设备（薄，全部小块缓冲）：
    DMA 采 100ms PCM16（3.2KB/块，双缓冲 6.4KB 静态）
      → WS 上行逐块发（边录边发）→ 服务器
    ← WS 下行 TTS PCM 块 → hal_i2s_write 直接播（缓冲 1-2 块）
    ← WS 下行文本 → 气泡
  服务器辅助手（攒包，Python）：
    收 PCM 块 → 边收边攒成 WAV（服务器内存/磁盘无限）
      → 完整 WAV → MiMo 音频理解（mimo-v2.5 单模型，✅ 已实测转写语音）
      → MiMo 回复文本 → 调 TTS（mimo-v2.5-tts，待验证）
      → 文本 + TTS 音频回设备
  ```
- **关键洞察**：MiMo 要"完整 WAV 文件"**不代表设备要攒**——设备边录边发，**服务器负责攒**。
  设备端永远只有 1-2 块缓冲，无大块分配。
- **✅ 2026-08-29 实测定案**：mimo-v2.5（Token Plan 端点）**音频理解确认可用**
  （完整转写"喂你好测试测试你好"并回答）；audio_tokens≈19 是固定开销与内容无关。
  无需独立 ASR。
- **内存预算（DRAM，重算）**：
  | 项 | 大小 | 类型 |
  |---|---|---|
  | DMA RX 双缓冲 | 6.4KB | 静态（不占堆）|
  | WS 发送/接收缓冲 | 各 4-8KB | 堆小块 |
  | 网络环冲 | 6-12KB | 堆小块 |
  | 传统 NS / VAD（可选）| 5-10KB / 3-5KB | 堆小块 |
  | **堆总计** | **~25-40KB，全部小块（每块 <10KB）** | **当前 largest=10KB 即可运行 ✅** |
- **参考**：小智的 WS 全双工 + 文本随流架构（流式模型对），但**编码器取舍不盲从**——
  小智用 OPUS 是因为它有 PSRAM 装编解码器；我们无 PSRAM、内存更紧，**更该用 PCM 直传**。

---

## 1. 现状盘点（2026-08-29，已核实）

### 1.1 硬件（勿改）

| 信号 | GPIO | 说明 |
|---|---|---|
| I2S0 MCLK | IO2 | 160/26 = 6.154MHz 整数分频 |
| I2S0 BCLK | IO17 | MCLK/4 = 1.539MHz（TX 输出 + 回环给 RX）|
| I2S0 WS | IO45 | BCLK/64 = 24.04kHz |
| I2S0 DOUT | IO15 | TX → ES8311 DAC |
| I2S0 DIN | IO16 | RX ← ES7210 ADC |
| I2C1 SDA/SCL | IO8/IO18 | 共享总线：ES8311(0x18)、ES7210(0x23)、GT911、IMU、摄像头 |
| PA 使能 | IO46 | 高电平开功放（NS4150B）|

### 1.2 软件链路现状（每层能干什么）

```
UI（screen_voice.c）
  └─ 🎙️ 说话按钮 → voice_service_talk(3)
      └─ voice_service.c（状态机 IDLE/LISTENING/THINKING/SPEAKING）
          ├─ 录音：ai_voice_record() → 16kHz WAV（真实可用 ✅）
          ├─ 回复：local_reply() ← 本地规则（用土壤传感器数据，离线占位 ⚠️ 非 AI）
          ├─ 文本回调 → UI 气泡上屏 ✅
          └─ 播放：ai_voice_play(wav) ← **回放用户自己的录音当 TTS 占位** ⚠️ 假 TTS
              └─ ai_voice.c → voice_agent → ES8311 播放
```

### 1.3 已验证正常（✅，勿重复排查）

- `plant voice i2ctest`：ES8311(0x18) / ES7210(0x23) I2C 通
- `plant voice tone <freq>`：播放测试音（DAC 链路基本通）
- `plant voice rec [sec]`：录音 2s 完整（32000 采样、非零率高）、WAV 结构正确
- `plant voice loop [sec]`：录音→回放全链路不崩
- UI 语音页：气泡/波形动画/状态文字/🎙️ 按钮/跨线程消息环形缓冲全通
- MiMo HTTP 文本链路（ai_common + ai_engine）：`plant ai advise` 可跑

### 1.4 ❌ 核心遗留问题（**P0 前置，必须先修**）

| # | 问题 | 证据 | 影响 |
|---|---|---|---|
| 1 | **I2S RX 过采样 ~1.64×**（实测 ~157k 采样/s，应为 96k）| AUDIO_HANDOVER_NEXT_AI.md §0：寄存器回读/示波器说 24kHz，但 RX 数据量 1.64× → 实际帧率 ~39.25kHz | **音调偏高 ~1.64×、录音回放是嘶声**——语音体验的根本障碍 |
| 2 | TTS 是假的（回放录音占位）| voice_service.c:146-149 | 用户听到的是自己声音，不是 AI 回复 |
| 3 | AI 回复是本地规则，非 MiMo | voice_service.c:55-102 | 无法真正"聊天" |
| 4 | `ai_common_http_post` 走 **HTTP 明文**（无 TLS）| ai_common.h 注释 | 音频/文本明文传输；HTTPS 需 mbedtls（TODO）|
| 5 | 录音缓冲 160KB（AI_VOICE_WAV_MAX=44+16000×2×5）| ai_voice.h | 堆紧张时 malloc 可能失败（rec 命令已有 heap 打印）|

---

## 2. 目标数据流（流式版，改造后）

```
点「说话」→ voice_service_talk()
  → LISTENING：ai_voice DMA 100ms chunk 连续采（9.6KB/块，已有）
      ├─ chunk →（待定传输方式）→ MiMo 全模态   ← Step 0 验证后定 A/B/C
      └─ 本地环形缓冲 ≤2~3s 兜底（防网络抖动丢字）
  → THINKING：MiMo 回复（文本 + 音频）
      ├─ 文本 → 气泡上屏（"和小绿聊天"字幕，用户确认模型能返回文本）
      └─ 音频 → 流式 → hal_i2s_write 逐块播放（缓冲 1-2 块）
              → 或落 SD 再播（若接口一次返回大音频）
  → SPEAKING：ES8311 播放中
  → IDLE
```

**⚠️ 唯一大未知（必须先验证，见 §5.1）**：MiMo 音频接口的**传输方式**——
JSON base64？multipart？WebSocket/SSE 流式？用户有 API key，**先在 PC 上用
curl/python 验证**（摄像头彩条同款方法论：先验证再写代码）。

---

## 3. 相关文件清单（改动范围）

| 文件 | 现状 | 需要的改动 |
|---|---|---|
| `nuttx/arch/xtensa/src/esp32s3/hal_i2s.c` | 自定义 HAL | **P0：修 RX 过采样 1.64×**（AUDIO_HANDOVER_NEXT_AI.md 有根因线索）|
| `components/voice_agent/voice_agent.c/h` | 仅 init | 大概率不动（外设层已通）|
| `ai_module/ai_voice/ai_voice.c/h` | record/play/wav/tone | 可能需要：录音数据导出（raw PCM 给 AI）、播放任意采样率、TTS 缓冲管理 |
| `ai_module/ai_common.c/h` | HTTP JSON POST | 可能加：音频上传/下载、base64、HTTPS(TLS) |
| `services/voice_service.c/h` | 状态机 + 本地规则 + 假 TTS | **核心改造**：local_reply → 真实 AI 调用；假 TTS → 真 TTS 播放 |
| `ui/screens/screen_voice.c` | 气泡/波形/状态 | 小改：状态文字、错误提示、可能加"重听/停止" |
| `main/app_main.c` | voice 命令 | 可加诊断命令（如 `plant voice ai` 单测 AI 语音链路）|
| `docs/*` | — | 完成后更新本文件状态勾选 |

---

## 4. 需求拆解（按优先级，可勾选）

### P0 —— 流式链路验证（前置，按新架构：PCM 直传 + 服务器攒包）

- [ ] **P0-1 修/确认 I2S RX 过采样 1.64×**：`plant voice tone 440` 听感 440Hz、
      `plant voice rec` 采样数与时长严格对应。用户已听到自己说话声（loop 通），
      但音调是否准需 `tone 440` 复核。
- [x] **P0-2 【PC 端】MiMo 音频接口发现实验（✅ 已通过，2026-08-29 实测定案）**：
      API key + 真人语音 WAV（16kHz mono PCM）→ 实测：
      ① 音频输入：`input_audio` + **`data:audio/wav;base64,` MIME 前缀** ✅ 被接受；
      ② 模型 **mimo-v2.5**（Token Plan 端点 `token-plan-cn.xiaomimimo.com/v1`）
         **完整转写语音内容**（"喂你好测试测试你好"）并回答 → **音频理解确认可用**；
      ③ 教训：audio_tokens≈19 是固定开销与内容无关（静音/有内容都是 19）；
         之前"模型听不到"的真因是**录音静音**（虚拟机没开声音输入），不是 API 问题。
      → **结论：MiMo 单模型（mimo-v2.5）听+懂+答，无需独立 ASR。**
- [x] **P0-2b 【PC 端】MiMo TTS 验证（✅ 已通过，2026-08-29）**：
      `mimo-v2.5-tts`（Token Plan 端点）合成"小绿绿"语音成功，返回 WAV（284KB）。
      用法：合成文本放 **assistant** message；风格指令放 **user**；`audio: {voice: "mimo_default"}`；
      返回音频在 `choices[0].message.audio.data`（base64）。
      → **语音链路全通：录音 + 音频理解(mimo-v2.5) + 文本 + TTS(mimo-v2.5-tts)，全 MiMo 生态。**
- [ ] **P0-3 服务器辅助手**：收设备 PCM 块 → 攒 WAV → MiMo（音频理解）；MiMo 回复文本 →
      调 TTS → 音频回设备（Python，与设备并行开发）。
- [ ] **P0-4 设备侧流式改造**：ai_voice 100ms DMA chunk → WS 直发（**零 OPUS 编码**）；
      删除 AI_VOICE_WAV_MAX(160KB) 与 OPUS 依赖；堆预算全部小块。

### P1 —— 完整说话体验（PCM 流式 + WS 全双工）

- [ ] **P1-1 发送链路**：DMA 100ms PCM 块 → WS 上行（边录边发）。
- [ ] **P1-2 接收链路**：WS 下行 TTS PCM 块 → hal_i2s_write 逐块播放（缓冲 1-2 帧）。
- [ ] **P1-3 字幕上屏**：回复文本 → 气泡（全模态模型可返回文本，用户确认）。
- [ ] **P1-4 voice_service 状态机流式化**：LISTENING=边录边发；THINKING=等回复；
      SPEAKING=边收边播；多轮上下文（messages 保留）。
- [ ] **P1-5 UI 打磨**：波形接录音音量、气泡自动滚动、播放可停止、错误提示、断网提示。

### P2 —— 进阶（可选）

- [ ] **P2-1 连续对话**：一轮结束后自动回到"点一下开始说话"，保留上下文
      （多轮 messages）。
- [ ] **P2-2 唤醒词/打断**：说"小绿小绿"唤醒；说话中可打断。
- [ ] **P2-3 语音+图像多模态**：拍照页和语音页打通（"看看这盆花"→ 拍一张+语音）。
- [ ] **P2-4 HTTPS**：ai_common 走 TLS（mbedtls），音频数据不再明文。

---

## 5. 关键技术风险与前置实验

### 5.1 MiMo 音频接口发现实验（P0-2，最高优先，PC 端做）

现状只有文本 `chat/completions`（ai_common，裸 socket HTTP POST）。语音需要先确认
**传输方式**（用户 2026-08-29 确认：未验证过，模型能返回文本+声音）：

1. 用 `plant cam` 同款方法论：**先在 PC 上 curl/python 验证，再写固件代码**。
   准备：API key（defconfig CONFIG_PLANT_AI_API_KEY）+ PC 生成一段 2s 16kHz WAV，
   base64 编码。
2. 试 A（OpenAI 兼容）：`messages` content 数组带
   `{"type":"input_audio","input_audio":{"data":"<b64>","format":"wav"}}`；
   回复若带 `output_audio` + 文本 → 方案 A（JSON base64）成立。
3. 试 B：multipart/form-data 上传 WAV 文件 → 方案 B。
4. 试 C：`"stream":true`（SSE）是否返回流式音频块 → 方案 C（真流式 TTS）。
5. **输出结论：A / B / C**，写回本文件与 ai_common 设计。

### 5.2 I2S 过采样（P0-1）

AUDIO_HANDOVER_NEXT_AI.md §0 已有两个独立证据（采样数 1.64× + 音调 1.64×）。
用户已听到自己说话声（loop 通）——用 `tone 440` 复核音调是否已准；未准则按该文档线索定位。

### 5.3 内存预算（流式后大幅缓解）

- 录音：160KB WAV 缓冲 → **删除**；改 100ms DMA chunk 直发 + 环形缓冲 ≤2-3s
  （64-96KB，堆内可行；或 static）。
- TTS：流式播放缓冲 1-2 块（几十 KB）；**整段 960KB 不放内存**（落 SD 或流式）。
- 上行 base64：2s WAV ≈ 64KB 原始 → base64 ≈ 86KB JSON —— 若方案 A，请求体
  仍偏大，需分块/压缩（OPUS/PCM16 降采样）或缩短录音；若方案 C（WS/OPUS）最省。

### 5.4 线程与回调约束（沿用现有模型，勿破坏）

- voice_service 回调在 voice 线程触发 → UI 只写环形缓冲，ui_task 定时器上屏
  （screen_voice.c 现有模式 ✅）。
- 流式接收/播放也在 voice worker 内（阻塞循环），不阻塞 ui_task。
- 不要在工作线程里直接操作 LVGL 对象。

---

## 6. 验证步骤（实现后按序跑）

```
1. plant voice i2ctest        # I2C 通
2. plant voice tone 440       # 听感 440Hz（P0-1 过采样修复的判据）
3. plant voice rec 3          # 采样数 = 3s×16000（严格对应）
4. plant voice loop 3         # 回放清晰可辨（不是嘶声）
5. PC 端 curl 验证 MiMo 音频格式（§5.1）
6. plant voice ai             # （新增命令）录音→MiMo→文本，串口打印回复
7. 进 UI 语音页：点说话 → 气泡出现 AI 文本 + 喇叭播放 TTS 语音
8. 断 WiFi 再点说话 → 明确错误提示，不卡死
```

---

## 7. 验收标准（完成定义）

- [ ] 点「说话」→ 录音 → **真实 MiMo 回复** 上屏为气泡
- [ ] AI 回复 **以语音播放**（不是回放用户录音）
- [ ] 音调准确、无嘶声（P0-1 修复生效）
- [ ] 一轮结束回到空闲态，可连续点
- [ ] 无网/失败有明确提示，不卡 UI

---

## 8. 随放随停 + VAD 修复记录（2026-08-31）

### 8.1 取消链路断裂（真 bug，实锤）

现象目标：微信式「再点按键 = 取消当前会话」。

根因：`voice_service_talk()` 的 busy 分支只设置了 voice_service 自己的
`g_cancel`，但录音循环（`ai_voice_stream_record`）检查的是 ai_voice 层的
`g_stream_cancel`（由 `ai_voice_stream_cancel()` 设置）——**此前没有任何代码
调用 `ai_voice_stream_cancel()`**。结果：

- 录音中再点按键 → `g_cancel=true` 但录音循环看不到 → 继续录满（最长 5s）；
- TTS 播放中再点按键 → 播放不中断；
- 取消检查只在录音结束后才执行，最多延迟 5 秒，chunk 也已上传。

修复（`voice_service.c` + `ai_voice.c`）：

1. `talk()` busy 分支：`g_cancel = true;` **并调用 `ai_voice_stream_cancel()`**
   —— 同时设两个标志，立即中断正在进行的录音/播放 while 循环；
2. `ai_voice_play_file()` 播放循环每块检查 `g_stream_cancel` → 播放中也随放随停；
3. worker 三处补查 `g_cancel`（会话完整性）：
   - `session_begin` 网络等待后、录音前（防 stream_record 开头重置
     `g_stream_cancel=false` 把取消标志清掉的时序漏洞）；
   - 录音结束后（已有）；
   - `finalize` 网络等待返回后（结果丢弃，不上屏不播放）；
4. 每个退出路径统一复位 `g_busy=false; g_cancel=false;`（防卡 busy）。

服务器侧：未 finalize 的孤儿 session 由 `cleanup_old_sessions(600s)` 回收，
不会污染下次会话（每次 session id 独立）。

### 8.2 VAD 静音阈值错 1000 倍（真 bug）

现象：说话结束不自动停，总是录满 5 秒（此前实测「录音 5000ms」）。

根因：`ai_voice_stream_record` 里 `sq/out_n` 是**均方（mean-square）**，
不是 RMS。静音基线 RMS≈284 → mean-square≈284²=80656。代码却写成
`rms < 300*300/1000 = 90`（笔误多除了 1000）→ 只有 RMS<9.5 才判静音
→ 静音永不触发。

修复：`msq < 300*300`（=90000，对应 RMS<300），变量名 `rms`→`msq`
避免误导。说话时 mean-square 远大于 90000，静音时 80656<90000 ✓。

### 8.3 验证清单（重刷后）

```
1. 说话 2 秒内再点按键 → 立即停止录音（不再等 5s），串口无「发送给服务器」
2. 说完一句话停顿 >1.5s → 自动停（不再固定 5s），有「录音 xxxx ms」
3. TTS 播放中再点按键 → 播放立即停，回到 IDLE
4. 一轮结束后可立即再点（不卡 busy）
```

---

## 9. PANIC 根因 + 交互语义修正（2026-08-31 晚）

### 9.1 PANIC（xptcode=28, PC=0x42053845）：LVGL use-after-free

现象：语音页连续操作后崩溃。System.map 反查：
`PC=0x42053845 → get_selector_style_prop+0x65`（lv_obj_style.c），
`A0=0x42053B84 → lv_obj_get_style_prop+0x44` —— 崩溃在 **LVGL 样式查询**，
访问无效地址 VADDR=0xb8e480c8（垃圾指针）。

根因：**screen_voice 的 UI 定时器泄漏（use-after-free）**：
- `screen_voice_create()` 每次进语音页 `lv_timer_create(voice_ui_timer_cb, 500)`
  —— 从不删除；
- 返回时 `ui_app_pop_screen()` → `lv_obj_delete(scr)` 销毁页面对象，
  但 timer 还活着，static `s_status`/`s_wave_bars`/`s_msg_list` 指向
  **已释放内存**；
- 再次进入语音页又建一个新 timer → 多个 timer 同时跑，旧的每 500ms
  访问已释放对象 → `get_selector_style_prop` 读垃圾指针 → PANIC。

**screen_camera 早有 `cam_on_delete`（LV_EVENT_DELETE 删 timer）修复，
voice 页漏了**。

修复（`ui/screens/screen_voice.c`）：
1. 新增 `voice_on_delete`（LV_EVENT_DELETE）：删 timer + 置空全部 static 指针
   （s_msg_list/s_status/s_wave_bars）+ 清待处理队列；
2. `s_ui_timer` 保存 timer 句柄，`lv_obj_add_event_cb(scr, voice_on_delete,
   LV_EVENT_DELETE, NULL)` 注册；
3. 页面销毁同时 `voice_service_abort()`（新接口）：设 g_cancel +
   中断录音/播放循环，worker 尽快退出，不浪费服务器请求。

### 9.2 交互语义修正（用户核心诉求：「再点=说完」不是「取消」）

用户原话：「我点一下开始说话，再点就是录制完成应该让ai给我回复了」。

旧实现错误：`talk()` busy 分支无脑 `g_cancel=true` + stream_cancel →
再点 = 取消丢弃 → 用户反复点（串口 5 次「已取消，丢弃本次录音」），
AI 永远不回复。

新实现（`voice_service.c` + `ai_voice.c`）——「再点」按当前状态分派：
| 状态 | 再点行为 | 实现 |
|------|---------|------|
| LISTENING（录音中） | **说完**：结束录音，已录内容发给 AI | `ai_voice_stream_finish()`（新 API：g_stream_finish，录音循环 break 但返回正时长） |
| SPEAKING（播放中） | 停止播放（随放随停） | `ai_voice_stream_cancel()` |
| THINKING / 窗口期 | 取消，丢弃结果 | `g_cancel=true` + cancel |

要点：
- `g_stream_finish` 与 `g_stream_cancel` 分离：说完保留数据，取消丢弃；
- worker 录音后**不再**因 g_cancel 丢弃（说完正常发送），只保留
  ms<300 太短静默放弃 + abort（页面销毁）丢弃两个例外；
- 录音前窗口期 g_cancel 检查保留（防 session_begin 网络等待期间
  取消标志被 stream_record 开头重置的时序漏洞）。

### 9.3 重刷验证清单（含 9.1/9.2）

```
1. 进语音页 → 返回 → 再进 → 返回，反复 10 次 → 不崩溃（原 UAF 场景）
2. 点 🎙️ 说话 → 再点 → 串口「录音 xxxx ms，发送给服务器」→ AI 文本气泡
   + TTS 喇叭出声（不再显示「已取消，丢弃」）
3. 录音中再点 → 立即结束并发送（不等 5s / 不丢弃）
4. TTS 播放中再点 → 播放立即停
5. AI 思考中再点 → 结果丢弃，回 IDLE
6. 语音页按返回（会话进行中）→ 立即停止，回首页不崩溃
```

---

## 10. 服务器 seq 校验修复（2026-08-31 晚二）

现象：设备「录音 3169 ms，发送给服务器...」后，服务器日志：
```
POST /voice/upload?session=0&seq=0..23  → 全部 400
POST /voice/upload?session=0&seq=24..   → 200
FINALIZE session=0 pcm=85688B  → 回复文本:（空）→ TTS 46124B
```

根因：**设备重启后 g_session 从 0 重新计数，复用了服务器上残留的
同名 session**（前一次测试 session=0 停在 seq=23、buf 还有旧数据）：

1. 设备端 `session_begin()`：`g_session = (g_session + 1)`，重启归零 →
   新会话 session=0 与服务器旧 session=0 撞 id；
2. 服务器 `if seq != s["seq"]+1 → 400`：设备从 seq=0 传，服务器期望
   seq=24 → seq 0~23 全被拒；
3. 服务器 buf = 旧 24 块(76800B) + 新 3 块(9600B) ≈ 85688B（日志吻合）
   → **混合两次录音的 PCM 发给 MiMo → 理解失败返回空文本**。

修复（`tools/server/server_bridge.py`）：设备每次会话第一块必是 seq=0，
以此作为新会话标记——`seq==0` 且该 session 已有数据时强制重置
（清 buf、seq 归 -1），旧状态不再污染新会话。模拟验证：残留
seq=23 的会话，新会话 32 块全部正常接收（102400B）。

重测：重启设备 → plant voice server → 说话 → 服务器日志应显示
`[UPLOAD] session=0 检测到新会话，重置旧状态` 且从 seq=0 起全部 200，
FINALIZE 回复文本非空。

---

## 11. 喇叭无声根因：has_audio 布尔解析 + GET /voice/audio 端点缺失（2026-08-31 晚三）

现象：服务器日志显示回复文本非空、TTS 音频 890924B 生成，但设备喇叭
无声音，且服务器日志**没有 GET /voice/audio 请求**（服务器记录所有请求）。

复现与证据（PC 端直调 MiMo API）：
- 同款 payload + 16kHz mono 真实语音 → 返回完整转写+回复 ✅（格式无误）
- 同款 payload + 纯静音 → 返回「抱歉听不到」（**非空**）→ 空文本 ≠ 静音导致
- `max_completion_tokens=300` 3 次：第 3 次 content 空（`comp=300 reasoning=299`，
  推理 token 吃满预算 → content 为 0）→ **300 会偶发空文本**（1024 从不空）

根因（两处叠加，都要修）：
1. **设备端 has_audio 解析失败**（services/server_bridge.c）：服务器返回
   JSON 布尔 `"has_audio": true`（无引号），而 `sb_json_get_string` 只支持
   字符串值（要求 `"key":"..."`）→ 对布尔返回 -EINVAL → has_audio 误判为
   false → 设备跳过下载和播放。**服务器日志无 GET /voice/audio 正是此证**。
2. **服务器端端点方法错**（tools/server/server_bridge.py）：`/voice/audio`
   写在 do_POST 里，但设备 fetch_audio 用 **GET**（sb_http_get_to_file 发
   GET）→ do_GET 没有该端点 → 404 → 即使 has_audio 修好，设备也会把 404
   的 JSON body 当 WAV 存盘 → play_file 无 RIFF 头失败。

修复：
- 设备端：`server_bridge_finalize` 单独做布尔解析（strstr `"has_audio"`
  → 跳过空白冒号 → 匹配 `true`）；`sb_http_get_to_file` 增加 HTTP 状态码
  校验（非 200 不写盘、删文件、返回 -EIO），状态码解析与写文件共用同一
  recv 循环（避免吃掉含 RIFF 头的第一包）；voice_worker 打印
  `ai_voice_play_file` 失败返回值（此前错误被吞）。
- 服务器端：`/voice/audio` 从 do_POST 移到 do_GET（设备用 GET 拉取）；
  `mimo_audio_understand` 的 max_completion_tokens 300 → 1024（防偶发空文本，
  prompt 已要求简短回答，回复不会变长）。

验证：服务器 `GET /voice/audio?session=9` 返回 200 + 原样 WAV 字节 ✅
（本地模拟：上传 2 块 → 塞 audio → GET 200 → 404 分支正确）。
设备端需重编译烧录（server_bridge.c + voice_service.c）。

重测：说话 → 服务器日志应出现 `GET /voice/audio?session=N` → 200，
设备日志应显示 `TTS 音频 x B，播放...` + `播放 /mnt/sd/tts_tmp.wav:
24000 Hz` + `文件播放完成`，喇叭出声。

---

## 12. 语音链路全通 ✅（2026-08-31 晚四）

用户实测确认：**录音 → 服务器 → MiMo 理解 → 文本 → TTS → 设备喇叭播放，
全链路打通**（尽管发送较慢）。此前各修复已验证：

1. seq==0 会话重置（服务器）→ 无 400 风暴
2. max_completion_tokens 300→1024（服务器）→ 无偶发空文本
3. has_audio 布尔解析（设备 server_bridge.c）→ 设备真正拉取音频
4. /voice/audio 移入 do_GET（服务器）→ GET 返回 200
5. recv 缓冲 NUL 终止 + 状态行 hex 诊断（设备）→ 状态码解析稳定
6. 分块发送 + [AUDIO] 进度日志（服务器）→ 断点可定位

已知遗留（下一阶段可处理）：
- **发送慢**：TTS 音频 ~800KB 经局域网下载，加上 finalize 里 MiMo
  理解+TTS 串行（各 10-30s），整轮 ~1 分钟。优化方向：音频压缩
  （opus/mp3）、理解与 TTS 并行、或设备边下边播（流式）。
- 设备端在下载大音频时曾出现 RST（ConnectionReset），疑似资源/时序
  问题，最新代码加了 recv errno 打印，尚未复测到该场景。

测试代码已清理：tools/server/diag_mimo_audio.py 删除。
