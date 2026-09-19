# 植小伴 · 架构设计（产品架构 / 目录架构 / 代码架构）

> 配套文档：《UI_SPEC.md》= 界面需求唯一事实来源；本文档 = 如何把界面需求落成代码。
> 目标：在现有「NSH 命令驱动 + 驱动组件」基础上，演进为「UI 优先的儿童交互产品」。

---

## 1. 产品架构（Product Architecture）

### 1.1 产品定位与用户

- **产品**：植小伴 — 面向小朋友的 AI 植物伙伴（虫洞嵌入式端，ESP32-S3-BOX 3.5" 触摸屏）。
- **用户**：3-8 岁儿童（可能不识字）+ 家长（配置 WiFi、查看详情）。
- **核心主张**：拟人化（植物有名字/心情/健康分）、语音优先（说话最自然）、游戏化（任务打卡/成就）、可视化（进度环/趋势图）。

### 1.2 信息架构（IA）

```
植小伴
├── ① 首页（植物状态总览）        ← 开机首屏 / Tab1
│   ├── 🪴 拍一拍 → ② AI拍照 → ③ AI诊断结果
│   └── 🎙️ 说话  → ⑥ 语音对话（全屏二级页）
├── ④ 数据（实时监测 + 7天趋势）  ← Tab2
├── ⑤ 任务（养护打卡 + 成就）     ← Tab3
└── ⑦ 日记（时光记录时间线）      ← Tab4
```

- **全局骨架**：4-Tab 底部导航（首页/数据/任务/日记）贯穿 ①④⑤⑦；②③⑥ 为全屏二级页（带 ← 返回/顶部返回）。
- **导航规则**：二级页不显示 4-Tab（或按原型 ⑥ 保留扩展导航）；返回一律回首页。

### 1.3 功能模块与数据流（逻辑视图）

```
┌────────────────────────────────────────────────────────────┐
│                        UI 层（LVGL v9）                      │
│  screen_home │ screen_camera │ screen_diagnose │ screen_data │
│  screen_tasks │ screen_voice │ screen_diary                  │
│  widgets: topbar / bottomnav / progress_ring / metric_card / │
│           chart / voice_wave / chip / dialog                 │
└───────────────┬──────────────────────────┬──────────────────┘
                │ UI事件(触摸/语音)          │ 状态订阅(observer/事件)
┌───────────────▼──────────────────────────▼──────────────────┐
│                    业务服务层（services）                     │
│  plant_state (档案/心情/健康分)   task_service (任务/成就)     │
│  sensor_service (轮询缓存)       diary_service (日记/照片)    │
│  ai_service (识别/诊断/聊天封装)  voice_service (录音/播放流)  │
└───────┬──────────────────┬────────────────────┬─────────────┘
        │ Modbus           │ MiMo HTTP          │ I2S/I2C
┌───────▼──────────────────▼────────────────────▼─────────────┐
│                    驱动组件层（components）                    │
│  sensor_driver(RS485)  ai_module(ai_common/chat/image/engine)│
│  camera_capture(OV3660) voice_agent(ES8311/ES7210)            │
│  ui_panel(lcd_st7796)  gesture_sensor  system_monitor(battery)│
└──────────────────────────────────────────────────────────────┘
```

**数据流方向**：
- **下行（感知→UI）**：Modbus 轮询 → `soil_data_s` 缓存（`sensor_service`）→ 订阅者（数据页/首页）刷新 label/chart。
- **上行（UI→AI→UI）**：触摸「拍一拍」→ `ai_service` 拍照上传 → MiMo 返回 JSON → 解析为 `ai_result_s` → ③ 诊断页渲染。
- **语音闭环**：`voice_service` 录音（ES7210）→ MiMo 全模态 → 文本上屏 + TTS 播放（ES8311）。

### 1.4 功能需求清单（FR，按优先级）

| ID | 功能 | 屏幕 | 优先级 | 依赖 |
|----|------|------|--------|------|
| FR-01 | 开机首屏：植物档案 + 健康分进度环 + 4项数据 + 状态栏 | ① | P0 | plant_state, sensor_service |
| FR-02 | 4-Tab 全局导航 | ①④⑤⑦ | P0 | widgets/bottomnav |
| FR-03 | 传感器实时数据 + 阈值评语（本地规则，离线可用） | ④ | P0 | sensor_service |
| FR-04 | 7天水分趋势折线图 | ④ | P1 | 历史环缓冲(NVS/littlefs) |
| FR-05 | 每日任务列表 + 完成打卡 + 成就徽章 | ⑤ | P1 | task_service |
| FR-06 | 拍照识别：取景预览 → MiMo 图像识别 | ②③ | P1 | camera_capture, ai_image |
| FR-07 | 识别结果页：名称/拉丁名/匹配度/标签/警告/建议 | ③ | P1 | ai_image JSON |
| FR-08 | 语音对话：录音→AI→TTS→文本气泡 | ⑥ | P1 | voice_agent, ai_voice |
| FR-09 | 成长日记时间线（事件+照片） | ⑦ | P2 | diary_service, SD |
| FR-10 | 任务完成 → 自动写入日记事件 | ⑤⑦ | P2 | task→diary 联动 |
| FR-11 | 电池/时间状态栏；🌵 心情随数据变化 | ①④ | P2 | battery_monitor, plant_state |
| FR-12 | 中文 UI 字体子集 + emoji 图片资源 | 全局 | P0 | §UI_SPEC 4.3 |

---

## 2. 目录架构（Directory Architecture）

### 2.1 现状（现状是「命令驱动」的平面结构）

```
apps/plant-companion/
├── main/app_main.c            # NSH 命令分发（plant wifi/ai/soil/ota...）
├── ai_module/                 # MiMo API（ai_common/ai_chat/ai_image/ai_engine/ai_voice）
├── communication/             # wifi_manager（cloud_client 仅头文件）
├── components/                # 硬件驱动组件
│   ├── ui_panel/              # 仅 lcd_st7796.c（lcd_init 空实现）← 需重写
│   ├── sensor_driver/         # RS485 Modbus 土壤传感器
│   ├── camera_capture/        # OV3660 DVP
│   ├── gesture_sensor/        # ICM-42607 IMU
│   ├── voice_agent/           # ES8311 + ES7210
│   └── system_monitor/        # battery + sd_card
├── hal/                       # HAL 头文件
└── ota/                       # OTA A/B 升级
```

**问题**：UI 只有 `lcd_init()` 占位；业务逻辑（任务/日记/植物档案/服务编排）不存在；一切靠 NSH 命令行手动触发，无法支撑触摸交互产品。

### 2.2 目标目录（新增 ui/ 与 services/，扩写 components/ui_panel）

```
apps/plant-companion/
├── main/
│   ├── app_main.c             # 入口：解析参数 → 启动 NSH 命令 OR 启动 UI 产品模式
│   └── app_task.c/h           # （新增）产品模式任务编排：ui/sensor/ai/voice 任务创建
├── ui/                        # ★ 新增：LVGL UI 层（本阶段核心）
│   ├── ui_app.c/h             # ui 任务入口：lv_init → 主题/资源加载 → 屏幕管理 → lv_timer_handler 循环
│   ├── screens/
│   │   ├── screen_home.c/h        # ① 主界面
│   │   ├── screen_camera.c/h      # ② AI拍照（含摄像头直写模式）
│   │   ├── screen_diagnose.c/h    # ③ AI诊断结果
│   │   ├── screen_data.c/h        # ④ 传感器数据
│   │   ├── screen_tasks.c/h       # ⑤ 今日任务
│   │   ├── screen_voice.c/h       # ⑥ 语音对话
│   │   └── screen_diary.c/h       # ⑦ 成长日记
│   ├── widgets/               # 复用控件
│   │   ├── widget_topbar.c/h      # 状态栏（时间/电池/天气）
│   │   ├── widget_bottomnav.c/h   # 4-Tab 导航
│   │   ├── widget_card.c/h        # 圆角卡片容器
│   │   ├── widget_progress_ring.c/h  # 健康分进度环（lv_arc 封装）
│   │   ├── widget_metric_card.c/h    # 数据小卡（emoji+数值+单位+评语）
│   │   ├── widget_chart.c/h         # 7天趋势折线图（lv_chart 封装）
│   │   ├── widget_voice_wave.c/h    # 语音波形动画
│   │   └── widget_chip.c/h          # 标签（#好养 #喜阴）
│   ├── theme/
│   │   ├── theme_plant.c/h     # 设计 token：颜色/圆角/间距/字体（§UI_SPEC 3）
│   │   └── theme_fonts.c       # 中文字体子集注册（plant_zh_16/20/24）
│   └── assets/                 # 图标/表情 PNG→C 数组（.c/.h 生成物）
│       ├── images/             # emoji/图标
│       └── fonts/              # lv_font_conv 生成的中文字体
├── services/                  # ★ 新增：业务服务层（UI 与驱动之间）
│   ├── plant_state.c/h        # 植物档案：名字/品种/种植日期/天数/心情/健康分
│   ├── sensor_service.c/h     # Modbus 轮询任务 + 最新值缓存 + 评语规则 + 历史环
│   ├── task_service.c/h       # 每日任务生成/完成/成就
│   ├── diary_service.c/h      # 日记事件读写（littlefs/SD）+ 照片
│   ├── ai_service.c/h         # ai_engine 的异步封装（拍照识别/诊断/聊天回调）
│   └── voice_service.c/h      # 录音→AI→TTS 状态机
├── communication/             # （保持）wifi_manager + cloud_client
├── ai_module/                 # （保持）MiMo API
├── components/                # （保持驱动）+ ui_panel 扩写
│   └── ui_panel/
│       ├── lcd_st7796.c/h     # LCD runtime（保留，补充 flush/backlight 控制）
│       ├── lv_port_disp.c/h   # ★ 新增：LVGL display driver（flush_cb → lcd）
│       └── lv_port_indev.c/h  # ★ 新增：LVGL input driver（GT911 → indev）
├── hal/                       # （保持）
├── ota/                       # （保持）
└── docs/                      # ★ 新增：UI_SPEC.md / ARCHITECTURE.md（本文档）
```

**关键取舍**：
- **ui/ 与 services/ 分离**：UI 只依赖 services 的接口（头文件），不直接碰驱动；驱动组件保持"可独立测试"。
- **components/ui_panel 只做移植层**（disp/indev/flush），不写业务屏——与 LVGL 官方 port 惯例一致。
- **screens 一屏一文件**：7 屏可独立开发/裁剪（Kconfig 可逐屏开关）。
- **widgets 可复用**：topbar/bottomnav 全局复用，metric_card 用于首页+数据页。

### 2.3 构建接入（Makefile 增改）

```
# 新增（对应目录架构）：
CSRCS += ui/ui_app.c
CSRCS += ui/screens/screen_home.c ... （逐屏，可被 CONFIG 裁剪）
CSRCS += ui/widgets/widget_*.c
CSRCS += ui/theme/theme_plant.c ui/theme/theme_fonts.c
CSRCS += services/plant_state.c services/sensor_service.c ...
CSRCS += components/ui_panel/lv_port_disp.c components/ui_panel/lv_port_indev.c
```

### 2.4 Kconfig 增改

```
config PLANT_UI          # 复用/改名 PLANT_UI_PANEL → PLANT_UI（总开关）
config PLANT_UI_SCREEN_HOME / _DATA / _TASKS / _DIARY / _CAMERA / _DIAGNOSE / _VOICE
                        # 逐屏裁剪（默认 y）
config PLANT_TASK_SERVICE / PLANT_DIARY_SERVICE / PLANT_PLANT_STATE
                        # 业务服务开关
config PLANT_UI_FONT_ZH  # 中文子集字体（default y）
```

---

## 3. 代码架构（Code Architecture）

### 3.1 分层与依赖规则

```
┌──────────────┐
│ main(app_main)│ ← 只负责启动/命令分发，不写业务
├──────────────┤
│ ui/          │ ← 依赖 services/*.h、theme、widgets、LVGL；禁止直接调驱动
├──────────────┤
│ services/    │ ← 依赖 components/*.h、ai_module/*.h；无 LVGL 依赖（可单测）
├──────────────┤
│ components/  │ ← 硬件驱动；无业务
└──────────────┘
```

> 依赖方向严格单向：main → ui → services → components。禁止 services 反向依赖 ui（回调用函数指针注入，如 `sensor_service_set_on_update(cb)`，由 ui 层注册）。

### 3.2 线程模型（NuttX 任务）

| 任务 | 职责 | 周期 | 优先级 | 与 UI 通信 |
|---|---|---|---|---|
| `ui_task` | `lv_timer_handler()` 循环（30fps），处理触摸/动画/刷新 | 33ms | 高(110) | 主 UI 线程 |
| `sensor_task` | Modbus 轮询 7合1 → 写缓存 → 通知订阅者 | 1-5s | 中(100) | observer/`lv_event` 或 msgq → ui 任务刷新 |
| `ai_task` | 拍照/聊天 HTTP 请求（阻塞 15-60s） | 事件驱动 | 低(90) | 完成后 msgq/回调 → 诊断页/对话气泡 |
| `voice_task` | 录音/播放 DMA 流 | 事件驱动 | 中(100) | 状态事件（听/想/说）→ 波形动画/气泡 |
| `camera_task` | ②页期间直写 LCD 帧（LVGL 暂停） | 帧同步 | 高(120) | 进入/退出标志位互斥 |

**互斥要点**：
- 共享数据（`soil_data_s` 缓存、`plant_state_t`、任务/日记列表）用 NuttX `pthread_mutex` 或自旋锁保护；**LVGL 对象只能在 ui_task 线程操作**（外部任务只发事件/消息）。
- 摄像头直写与 LVGL flush 共享帧缓冲：进入②页时 `lv_timer_pause` + 标志位，退出时恢复 + 全屏 invalidate。参考《UI_SPEC》§4.1。

### 3.3 核心数据结构

```c
/* services/plant_state.h */
typedef struct {
  char     name[16];        /* 小绿绿 */
  char     species[24];     /* 绿萝 */
  uint8_t  health_score;    /* 0-100，进度环 */
  int      plant_day;       /* 养花第 N 天 */
  uint8_t  mood;            /* 0=😊 1=🥱 2=😢 */
  bool     need_water;      /* AI 浇水建议 */
} plant_state_t;

/* services/sensor_service.h —— 从 components/sensor_driver 的 soil_data_s 派生 */
typedef struct {
  float moisture;  /* %  水分 */
  float temp;      /* °C */
  float ec;        /* EC */
  float light;     /* lux（如传感器支持） */
  char  moisture_note[12];  /* 我很好/有点干…（阈值规则生成） */
  uint8_t status;  /* 0=OK 1=WARN 2=BAD */
} sensor_view_t;

/* services/ai_service.h —— 识别+诊断结果（对应③页） */
typedef struct {
  char name[32];        /* 绿萝 */
  char latin[48];       /* Epipremnum aureum */
  uint8_t match;        /* 96 */
  char tags[3][16];     /* 好养/净化空气/喜阴 */
  bool issue;           /* 发现小问题 */
  char issue_desc[128]; /* 诊断详情 */
  char advice[256];     /* 养护建议 */
} ai_diagnose_t;

/* services/task_service.h —— 对应⑤页 */
typedef struct {
  char title[32];       /* 看看小绿绿 */
  char time_slot[16];   /* 早上 8:00 */
  bool done;
  uint8_t kind;         /* 0=例行 1=AI建议 2=拍照 3=浇水 */
} task_item_t;

/* services/diary_service.h —— 对应⑦页 */
typedef struct {
  uint32_t ts;
  uint8_t  color;       /* 🟢🟠🔵🟣 */
  char     title[48];   /* 拍了第28天成长照 🌱 */
  char     detail[64];  /* 又长了一片新叶！ */
} diary_entry_t;
```

### 3.4 UI 层关键设计

```c
/* ui/ui_app.h —— ui 任务入口 */
int  ui_app_start(void);          /* 创建 ui_task，lv_init + 主题 + 屏幕管理 */
void ui_app_push_screen(int id);  /* 屏幕切换：home/camera/diagnose/data/tasks/voice/diary */
void ui_app_pop_screen(void);     /* 返回首页 */

/* ui/screens/screen_home.h —— 一屏一接口 */
int screen_home_create(lv_obj_t *parent);
void screen_home_refresh(const sensor_view_t *sv, const plant_state_t *ps);
```

- **屏幕管理**：所有屏对象常驻（`lv_obj_set_hidden` 切换，保持状态）或懒创建；二级页（②③⑥）进出用 push/pop 栈。4-Tab 用 `widget_bottomnav` 广播 Tab 切换事件。
- **数据订阅**：LVGL v9 `LV_USE_OBSERVER`（`lv_observer`）或 `lv_event_send` 自定义事件；`sensor_service` 的 `on_update` 回调由 ui 层注册 → 事件 → 各屏 `*_refresh()`。
- **摄像头直写模式**：`screen_camera.c` 提供 `camera_preview_start()/stop()`，内部管理 LVGL 暂停标志与帧缓冲互斥（详见 UI_SPEC §4.1）。

### 3.5 产品模式 vs 调试模式（app_main 演进）

```
nsh> plant                        # 无参数 → 启动产品模式（UI 首屏）
nsh> plant ui                     # 显式启动 UI
nsh> plant wifi/ai/soil/ota/...   # 保留全部调试命令（NSH 调试不冲突）
```

- `app_main` 增加：无参数（或 `plant ui`）时 `ui_app_start()` 并进入产品模式；其余命令走原有分发。
- **建议**：UI 任务与 NSH 共存于同一固件；调试命令在 UI 运行时仍可用（串口输入），便于现场联调。

---

## 4. 演进路线（Roadmap）

> 状态（2025-08-20）：3A/3B/3C/3D 的 **UI 代码 + 服务层已全部完成并编译通过**
> （nuttx.bin 1.52MB / 2MB 槽位 = 74%，7 屏 + 语音状态字符串确认入固件），
> 待板级验证。服务层已含：sensor_service（轮询/评语/历史环）、plant_state（档案/健康分/心情规则）、
> record_service（任务/日记持久化，/data littlefs 可降级）、ai_service（异步封装）、
> voice_service（录音→AI→TTS 状态机）。剩余：OV3660 帧采集驱动 + MiMo 真实识别接入、
> voice_service 接 MiMo 全模态、成就系统。

### Phase 3A — UI 骨架 ✅（代码完成）
1. ~~`components/ui_panel` 自写 lv_port_disp/indev~~ → **改用 NuttX 官方移植** `lv_nuttx_init()`（`/dev/lcd0` + `/dev/input0`），已确认可用。
2. `ui/theme`：设计 token + 中文字体子集（lv_font_conv 生成 16/20/24px）✅
3. `ui/widgets`：topbar / bottomnav / card ✅
4. `ui_app`：lv_init + 屏幕管理 + ui_task 主循环 + 二级页 push/pop ✅

### Phase 3B — 三屏打通（P0 功能）✅（代码完成）
5. `services/sensor_service`（Modbus 轮询 + 评语规则 + 7天历史环）✅
6. `screen_home`（①）：状态栏 + 快捷入口(拍一拍/说话) + 植物卡 + 进度环 + 4 数据卡 + 4-Tab ✅
7. `screen_data`（④）：2×2 数据卡 + 7天趋势折线图 ✅

### Phase 3C — AI + 语音（P1 功能）🟡（UI + 服务层完成，真实链路待接）
8. `screen_camera`(②) 取景框 + 扫描线动画 + 拍照→诊断页 ✅
9. `screen_diagnose`(③) 名称/匹配度/标签/警告框 ✅
10. `screen_voice`(⑥) 气泡列表 + 🎙️ 说话 + 波形动画 ✅
11. `services/ai_service` 异步封装（后台线程跑 ai_engine，完成回调投递 ui_task）✅
12. `services/voice_service` 状态机（录音→本地回复→TTS 播放，接 MiMo 全模态时替换 local_reply）✅
13. **待做**：OV3660 帧采集驱动直写 LCD + ai_service 传真实 JPEG + MiMo 全模态对话。

### Phase 3D — 游戏化与记录（P2 功能）🟡（UI + 持久化完成，成就待做）
14. `screen_tasks`(⑤) 任务打卡（点击完成切换）✅
15. `screen_diary`(⑦) 时间线（彩色圆点 + 事件）✅
16. `services/plant_state` 植物档案 + 健康分/心情规则（水分/温度/EC 加权）✅
17. `services/record_service` 任务/日记持久化（/data littlefs，可降级仅内存）✅
18. **待做**：成就系统（徽章）+ 任务完成→日记联动 + 照片（SD）。

### Phase 4 — 系统集成（未开始）
15. 24h 全外设压测（摄像头直写 + LVGL 恢复、语音 + AI 并发、OTA 后首屏自检）。
16. 性能：首帧时间、刷新率、内存峰值（`free`）、碎片检查。

---

## 5. 与现有代码的关系（改造边界）

| 现有文件 | 处置 |
|---|---|
| `main/app_main.c` | 保留命令分发，新增产品模式分支 |
| `components/ui_panel/lcd_st7796.c` | 保留 `lcd_init`，补充 flush/backlight 控制；LVGL 移植放新文件 |
| `components/sensor_driver/soil_sensor.c` | 不变；`sensor_service` 包一层轮询+缓存 |
| `ai_module/*` | 不变；`ai_service` 做异步封装 |
| `components/voice_agent/*` | 不变；`voice_service` 编排录音/AI/TTS |
| `components/camera_capture/*` | 不变；`screen_camera` 管理直写模式 |
| `communication/wifi_manager` | 不变；产品模式下若未联网，AI 页显示「需要联网」引导 |

> 原则：**驱动与 AI 底层零改动**，新增 ui/services 两层；逐屏交付、每屏可板级验证。
