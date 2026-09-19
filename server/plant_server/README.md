# 植小伴 · 服务器大脑（参赛演示版 V1.1）

FastAPI 单体（演示期 SQLite + 本地媒体目录），实现《需求规格 V1.0》§7 的 P0 纵向切片：
账号 → 设备 → 数据上报 → 媒体 → AI 编排（诊断/问答/语音）→ 任务/成长 → 双端同步 → 健康周报，
并在 V1.1 补齐 **多用户账号体系、植小伴社区、绿植保险·一键理赔**。

## 快速开始

```bash
pip install -r requirements.txt
python run_server.py --seed          # 写演示数据（1账号+1设备+1植物+30天数据）
python run_server.py --host 0.0.0.0 --port 8000
```

- 交互式 API 文档：http://127.0.0.1:8000/docs
- 演示账号：App 端点 `POST /api/v1/auth/demo`（手机号 13800000001，昵称“小明”）
- 手机验证码登录：固定演示码 `123456`（接口直接返回 dev_code，演示期免短信通道）
- 首次用某手机号登录即自动注册（不需要单独的注册接口），昵称自动取 `植友+后4位`

## 数据与目录

- 数据库：`data/plant.db`（自动建表/迁移）；媒体：`data/media/{raw,image,audio}/日期/`
- 环境变量（可选）：
  - `PLANT_MIMO_KEY`：小米 MiMo 密钥；不设则自动进入 **mock 演示模式**（离线可跑全链路）
  - `PLANT_AI_MODE=auto|mock|mimo`；`PLANT_MIMO_URL/MODEL/TTS_MODEL/TTS_VOICE`
  - `PLANT_DEMO_CODE`：演示固定验证码（默认 123456；阿里云上设为 339703）
  - `PLANT_DEMO_OPEN=0`：关闭「一键演示登录」（正式上线建议关掉）
  - `PLANT_RATE_LIMIT=0`：关闭接口限流（演示压测时用）
  - `PLANT_DB`、`PLANT_DATA_DIR`、`PLANT_TOKEN_SECRET`、`PLANT_UPLOAD_INTERVAL_S`
  - `PLANT_BILLING_ENFORCE=1`：打开**会员门禁**（自动执行等高级功能要 PRO 及以上）。
    演示期默认关，不然演示账号没开会员就点不动自动浇水
  - `PLANT_AUTO_WATER_BELOW`（默认 25，%）、`PLANT_AUTO_WATER_COOLDOWN_MIN`（默认 30 分钟）：
    自动浇水的触发阈值与冷却时间

## 核心接口（前缀 /api/v1）

| 分组 | 接口 |
|------|------|
| 账号 | POST /auth/sms · /auth/login（登录即注册）· /auth/demo · /auth/logout · GET /auth/me · PATCH /auth/me |
| 设备 | POST /devices/register（SN→device_token）· /devices/bind（App 绑定，可带 `plant_id` 指定绑到哪一盆）· /devices/heartbeat · /devices/{id}/status |
| 同步 | GET /sync（设备轮询：时间/配置/任务/健康分/最近事件） |
| 植物 | GET/POST /plants · GET/PATCH /plants/{id} · GET /plants/{id}/telemetry?days=N（图表序列） |
| 上报 | POST /telemetry/put（单条或批量，device_token，按 ts 幂等） |
| 媒体 | POST /media/image?fmt=raw565|jpeg（raw565 38400B / jpeg）· GET /media/{id}/file · GET /media?plant_id=&limit=（只看自己的照片） |
| AI | POST /ai/diagnose（media_id→结构化诊断 JSON）· POST /ai/chat（多轮文字问答，**不限话题**，见下节） |
| 语音 | POST /voice/session · POST /voice/upload?session_id&seq（PCM16 流式）· POST /voice/finalize（→text+audio）· GET /voice/audio |
| 任务/成长 | GET /tasks/today · POST /tasks · POST /tasks/{id}/complete（幂等）· GET /growth |
| 事件/日记 | POST /events · GET /events（时间轴） |
| 周报 | POST /plants/{id}/reports/weekly · GET /plants/{id}/reports |
| 社区 | GET /community/topics · /community/feed?tab=&q=&offset= · /community/leaderboard · /community/posts/{id} · /community/users/{id} |
| 社区（写） | POST /community/posts · /community/posts/{id}/like · /{id}/fav · /{id}/comments · /community/users/{id}/follow |
| 保险 | GET /insurance/summary · /insurance/preview?plant_id= · /insurance/claims · /insurance/claims/{id} |
| 保险（写） | POST /insurance/policies · /insurance/claims |
| 自动执行 | GET /actuators?plant_id=（App 看执行器与规则）· POST /actuators/{id}/mode（自动/关闭）· POST /actuators/{id}/run（立即执行一次） |
| 自动执行（设备） | POST /actuators/capabilities（板卡声明有几路）· GET /actuators/pending（取指令）· POST /actuators/jobs/{id}/ack（回执） |
| 会员 | GET /billing/plans（套餐表，免登录）· /billing/summary · POST /billing/subscribe · /billing/cancel · /billing/deposit |
| 系统 | GET /time · /health · /admin/stats |

## AI 聊天（2026-09-12 起不限话题）

用户要求："反正是接的大模型，不一定非要聊植物，其他问题也能答，提高趣味性"。
落地要点：

- **人设**在 `app/services/ai_gateway.py` 的 `CHAT_SYSTEM`：植小伴（小名小绿绿），
  儿童友好；植物养护是专长，但作业题、科普、故事、笑话、闲聊都要正常答，
  不往植物上硬拐。两条底线：**不编造**（不知道 / 需要实时信息就说不确定）、
  **有安全边界**（危险动作、身体不适、不适宜话题 → 先提醒找爸爸妈妈或老师）。
- **语音同源**：板卡语音走 `AUDIO_PROMPT`，同一套人设，App 打字和对着板子说话风格一致。
- **植物背景**：`/ai/chat` 会把"当前植物名 + 最近一次传感器读数"拼进 system
  （`routers/ai.py` 的 `plant_context()`），所以"它今天状态怎么样"能落到真实数据上；
  **没有绑定植物也能聊**（`resolve_plant(..., optional=True)`，新用户不必先添加植物）。
- **会话归属**：带 `conversation_id` 续聊时校验会话属于当前账号，否则 403
  （此前不校验，拿到别人的会话 id 就能把那 10 条历史塞进模型上下文）。
- **拍照诊断**仍走 `IMG_PROMPT`，属于业务能力，不受聊天放开影响。
- 回归测试：`_tmp/test_chat_general.py`（8 项：趣味话题 / 科普 / 算术 / 养护不退化 /
  0 盆植物可聊 / 跨账号 403 / 错误 plant_id 404），本机与云端通用。

## 大模型故障与降级（2026-09-12）

那天用户报"手机拍照识别不了、板卡语音没上传成功"，实测结论是**上游模型挂了**，
服务器和密钥都是好的 —— 记在这里省下次重新排查：

- 现象：手机拍照 → 502；板卡语音上传一路 200，但 `/voice/finalize` 502。
- 根因：多模态主模型 `mimo-v2.5` 对所有请求（连"你好"）稳定返回
  `HTTP 500 Internal Server Error`，而 `mimo-v2.5-pro`（纯文本）、
  `mimo-v2.5-asr`（转写）、`mimo-v2.5-tts`（合成）都正常。
  即：**密钥有效、上游该模型的路由挂了**。
- 应对（已落地）：
  1. **文本问答自动降级**：主模型返回 5xx / 连接被掐断时，自动换
     `PLANT_MIMO_TEXT_MODEL`（默认 `mimo-v2.5-pro`）重试一次。
     图片/音频请求不降级（备用模型不支持多模态），如实报错。
  2. **语音链路拆成两步**：`ASR 转写（mimo-v2.5-asr，约 0.6s）+ 文本模型作答 + TTS`，
     不再依赖多模态模型 —— 上游抖动时板卡仍能正常对话。
     板卡上屏格式仍是"转写 + 回答"。
  3. **明确提示**：上游故障返回"AI 服务暂时不可用（上游模型故障）"，
     不再和"我们自己出错"混成一句话；故障原文记进 `ai_jobs.error`。
  4. **一键自检**：`GET /api/v1/ai/health`（免登录）逐项探
     文本/看图/听音/合成，回 `{name, model, ok, ms, note}`。
     真打 4 次大模型，按需调；排查"是不是大模型挂了"十秒定位。
- 相关环境变量：`PLANT_MIMO_MODEL`（多模态主模型，默认 mimo-v2.5）、
  `PLANT_MIMO_TEXT_MODEL`（文本兜底，默认 mimo-v2.5-pro）、
  `PLANT_MIMO_ASR_MODEL`（转写，默认 mimo-v2.5-asr）。

## 鉴权约定

- App：`Authorization: Bearer <user_token>`
- 设备：`X-Device-Token: <device_token>`（由 /devices/register 或种子数据获得）
- 两端均无任何大模型密钥；模型只由服务器调用（模型网关 `app/services/ai_gateway.py`）

## 多用户账号（V1.1）

- **登录即注册**：`POST /auth/sms` 拿验证码 → `POST /auth/login`；手机号没见过就自动建号，
  返回 `is_new=true`（App 据此引导填资料）。昵称默认 `植友+后4位`，头像默认 🌱。
- **资料**：`GET /auth/me` 返回脱敏手机号（`138****0001`）与统计（植物/设备/帖子/理赔/关注/粉丝）；
  `PATCH /auth/me` 改昵称（1~16 字）、头像（emoji）、城市、简介。
- **多用户隔离**：所有业务接口都按 token 里的 user_id 过滤，媒体相册只返回自己的照片；
  App 端收到 401 会清本地 token 并跳登录页，**不会**再悄悄用演示账号顶替（那会串号）。
- 限流：验证码 10 次/时/IP、登录 30 次/时/IP、演示登录 20 次/时/IP（防刷 AI 额度）。

## 社区（V1.1）

- 五个流：推荐 / 关注 / 晒图 / 求助 / 排行榜；支持关键词搜索、分页（`offset`）。
- 帖子：正文 ≤500 字、最多 3 张配图（**只能挂自己名下的照片**）、分类（晒图/求助/日常）、话题。
- 互动：点赞、收藏、评论、关注作者；数字全部实时统计，不是写死的。
- 安全：内置违禁词过滤；发帖限流 10 次/时；排行榜按「获赞 + 发帖」排序。
- 演示种子：`app/services/community_seed.py` 会幂等创建 5 位邻居账号 + 7 条帖子 + 真实互动，
  以及演示账号自己一条带真图的帖子（服务启动时自动执行）。

## 绿植保险 · 一键理赔（V1.1，差异化功能）

一句话：**点一下，服务器自己取证、自己预审、当场出结论。**

- **自动承保**：每盆植物自动对应一张保障（保额 ¥49，一盆绿植一年内养死免费补发）。
- **自动取证** `collect_evidence()`：照片张数与最近拍照时间、AI 体检次数与最近健康分、
  传感器记录数与土壤水分区间/长期缺水占比、养护打卡率、设备在线情况。
- **预审打分** `auto_review()`：0~100 分。
  - ≥75 → `auto_pass`：**免人工直接通过**，赔付 ¥49 补发同款
  - ≥50 → `manual`：转人工 `reviewing`
  - <50 → `need_more_info`：直接告诉你还缺什么（每条判定都配中文理由）
- **防刷**：同一盆植物同时只能有一张处理中的单；本保障期已赔付过的再报，强制转人工。
- **演示账号有两盆，正好演两种结果**（见「演示数据种子」）：
  - `小绿绿`（已连卡、证据齐全）→ 86 分 `auto_pass`「证据齐全，免人工审核」→ 接受赔付 → 已补发。
  - `小肉肉`（未绑卡、照片和打卡都偏弱）→ 68 分 `manual`「资料基本齐了，需要人工复核一下」
    → 用「（演示）模拟人工审核通过」→ 接受赔付 → 已补发。

### 演示前必做：重置理赔记录

同一盆植物赔付过一次之后再报案会转人工（防刷），连着演示几遍就演不出「秒过」了。演示前跑：

```bash
python3 scripts/reset_demo_claims.py              # 只清演示账号（推荐）
python3 scripts/reset_demo_claims.py --all        # 清全部账号（慎用）
```

演示前更省事的做法（理赔单 + 卡住的执行指令 + 会员状态，一次全复位）：

```bash
python3 scripts/reset_demo_state.py               # 推荐：演示账号一键复位
```

### 上线前要处理

- `POST /insurance/claims/{id}/advance` 是**演示用**「模拟人工审核通过」按钮，真上线要删掉。
- 赔付目前是直接补发、不走支付通道；接支付/履约系统后替换 `accept_claim` 里的落库逻辑。

## 自动执行层（2026-09-12 · 从"监测"到"守护"的那一环）

一句话：**服务器自动下单，板卡执行，执行完写成长日记。**

- **执行器**（`actuators` 表）：一台设备可以挂多路（水泵/补光灯/风机/喂食器），
  每路有 `mode`（auto 自动 / off 关闭 / manual 只手动）和 `state`（idle/running/error）。
- **自动规则**：遥测进来时判一次——土壤水分低于阈值（默认 25%）且距上次执行超过冷却时间
  （默认 30 分钟）且今天没超过上限（默认 6 次）→ 自动排一条浇水指令。
- **指令队列**（`actuator_jobs` 表）：`pending` 排队 → `sent` 板卡已取走 → `done` 已执行 /
  `failed` 失败 / `expired` 超时作废。来源分三种：`auto` 规则、`app` 用户手动、`admin` 管理台。
- **回执写日记**：执行成功会往成长日记写一条（`water` / `actuator`），并给成长值。
- **App**：「我的 → 自动守护」看每一路的状态、切自动/关闭、点「立即浇水」；
  没绑卡的植物会提示"指令发出去没人执行"。
- **管理台**：「自动执行」页看全站执行器与指令流水，可手动补一次、可临时关掉自动。

### 板卡侧（2026-09-12 已落地：固件实现了这三步）

1. 开机注册后声明能力（幂等，可反复调）：
   ```http
   POST /api/v1/actuators/capabilities    X-Device-Token: <device_token>
   {"actuators": [{"kind": "water", "name": "水泵"}]}   # 有哪几路就声明哪几路
   ```
2. 取指令（固件每 15 秒一次；App 上写的是"60 秒内取走"，留了余量）：
   `GET /api/v1/actuators/pending`，服务器顺手把 `pending` 标成 `sent`。
   （`GET /api/v1/devices/sync` 里的 `pending_actions` 是同一份数据的另一个出口。）
3. 执行完回执：
   ```http
   POST /api/v1/actuators/jobs/{job_id}/ack
   {"ok": true, "detail": "dry-run: no actuator wired"}   # 真接了继电器就是 "gpio12 pulsed"
   ```
   失败就 `ok=false` 带原因，服务器会把执行器标成"需要看看"。
4. 动作就是"开 N 秒"（`duration_s`）。以后要精确到毫升，在 `config_json` 里加
   `flow_ml_per_s` 即可，不用改表。

固件源码在虚拟机 `/home/sdr/plantb/apps/plant-companion`，改了四处：

- `services/actuator_service.c`（新）+ `.h`：执行层本体。开机声明能力；每秒调一次
  `actuator_service_poll()`（挂在 ui_app.c 的时间/心跳 worker 里，**不新开线程**——
  server_bridge 用的是静态响应缓冲，多线程并发调用会互相踩）；内部节流每 15 秒取一次指令；
  动作就是"开 N 秒"：**接了继电器就按 `duration_s` 计时、到点关断再回执；
没接硬件（干跑，当前默认）时取到指令当场完成回执**——App 点「立即浇水」图标就出结果。
最多同时跑两路（浇水 + 补光这类）。
- `services/server_bridge.c/.h`：三个协议函数
  `server_bridge_declare_capabilities / server_bridge_fetch_pending / server_bridge_ack_job`。
- `Kconfig`：`PLANT_ACTUATOR`（默认开）、`PLANT_ACTUATOR_GPIO_WATER`、`PLANT_ACTUATOR_ACTIVE_LOW`。
- `Makefile` / `ui/ui_app.c`：编译项与调用点。

**真机实测（2026-09-12，固件 v1.3.0-srv，COM4）**：

| 场景 | 结果 |
|------|------|
| 开机声明能力 | `[Act] 已向服务器声明 1 路执行器`，服务器建/更新执行器条目 |
| 管理台手动下发 | ≤15 秒内取走并**当场回执**（干跑：未接硬件，立即完成），日记写「管理台浇水完成」 |
| 自动规则触发（水分 15% < 25%） | 取走并回执，日记写「自动浇水完成 · 土壤水分 15% 低于 25%，自动补水」 |
| 同时下发两份（15 秒档） | 干跑模式两路都当场回执；接继电器后才按各自时长计时 |
| 心跳 / 遥测 | 仍是 60 秒一次，不受执行影响 |

**继电器还没接线**，所以现在的回执 detail 是 `dry-run: no actuator wired`——
干跑模式取到指令**当场**完成（约 15 秒内出结果：取指令 15 秒一拍 + 立即回执），
日记照写，页面上能一眼看出没接硬件（不糊弄）。
线接上以后改一个配置就真浇水，代码不用动：

```
CONFIG_PLANT_ACTUATOR_GPIO_WATER=<继电器引脚号>   # 现在 -1 = 未接
CONFIG_PLANT_ACTUATOR_ACTIVE_LOW=y               # 常见继电器模块低电平吸合
```

以后加补光/风机：`actuator_service.c` 的 `g_caps[]` 加一行 + `act_gpio_of()` 给个引脚号 +
Kconfig 加一个引脚配置项即可，**服务器那边不用改**。

## 会员订阅（2026-09-12 · 商业模式的骨架）

路演讲稿里的「押金 99 元 + 订阅 19.9/29.9/49.9」在这里落地成**状态和权益**，支付通道留成显式占位。

- **三档套餐**：BASIC ¥19.9 / PRO ¥29.9 / FAMILY ¥49.9（每月），保险加购 ¥9.9/月，硬件押金 ¥99（满 12 个月退）。
- **权益**：自动执行要 PRO 及以上；绿植保险、多设备要 FAMILY。
  `entitled()` 只看套餐本身，`has_feature()` 才看 `PLANT_BILLING_ENFORCE`——
  演示期门禁默认关，所以 BASIC 账号也能把功能点通，只是 App 上会标"这是 PRO 功能"。
- **续费规则**：同一套餐续费从原到期日**顺延**；换套餐从今天**重新算**（不然"先用免费 BASIC 再升 PRO"会把赠送天数叠上去）。
- **演示账号**默认 PRO（12 个月）+ 押金已交。
- **正式上线要做的两件事**：把 `POST /billing/subscribe` 换成"下单 + 调支付"，
  支付回调里调 `billing_ops.subscribe(user_id, plan, months)`；然后打开 `PLANT_BILLING_ENFORCE=1`。

## 演示数据种子

```bash
python -c "from app.services.seed import ensure_demo; ensure_demo(verbose=True)"
```

重复执行幂等。设备演示凭证在 `data/plant.db` 的 `devices` 表（sn=ZXB-DEMO-0001）。

演示账号（13800000001）除了第一盆 `小绿绿`，还会自动补一盆 `小肉肉`（多肉，`app/services/demo_plants.py`，
随服务启动幂等执行）。这盆**故意不绑设备、证据配在「转人工」档**，用来现场演「材料不够转人工」；
它的照片是复制演示账号已有的真实照片、把拍摄时间挪到 11 天前，不额外造图。

演示账号还会补上「水泵 + 补光灯」两路执行器和两条历史执行记录（`actuator_ops.ensure_demo_actuators()`），
并把会员设成 PRO、押金标记为已交（`billing_ops.ensure_demo_subscription()`）——都随服务启动幂等执行。
板卡以后自己声明能力时会更新同一批执行器条目。

## 与嵌入式/App 的对接说明（对应需求 EM-1..9 / AP-1..9）

1. 设备把 `server_bridge` 客户端指向本服务器即可复用语音/图像上报；直连 MiMo 路径后续在嵌入式代码中关闭。
2. raw565 帧：先 `POST /media/image` 拿 media_id，再 `POST /ai/diagnose`（两步与一次两步皆可）。
3. 语音：设备仍按现状“边录边传”PCM，会话制上传→finalize 返回文本与 TTS。
4. App 端注册/登录后即可拉植物列表、任务、日记与图表；社区与保险走同一套 Bearer token。

## 说明与后续

- 演示期明文 HTTP；正式部署需 HTTPS 与密钥管理（见需求规格 NFR-7）。
- 社区目前只有本地违禁词，没有接内容审核服务；举报、私信、推送尚未实现。
- `/admin/stats` 还没有鉴权；管理台已有设备/遥测/日志/自动执行/会员订阅，还**没有理赔审单界面**
  （"转人工复核"目前只能在 App 里用「模拟人工审核通过」按钮走完）。
- 自动执行**两端都通了**，但继电器/水泵还没接线，板卡目前是"干跑"（回执里写明），
  接线后改 `CONFIG_PLANT_ACTUATOR_GPIO_WATER` 即真执行。
- 订阅没有接支付通道，开通/续费是直接落库；门禁开关 `PLANT_BILLING_ENFORCE` 默认关。
- 待办：OpenAPI 完整契约文件、ER 图、错误码表、多设备首页切换、App 推送占位实现、自动化测试。