# 植小伴 · AI 智能养植伴侣

> 参赛队伍：**叶绿体智能**（北京见壤智能科技有限公司）
> 仓库：`contest2026_053_yelvtizhineng` ｜ 赛道：**AI 硬件产品创新**
> 主控：CHD-ESP32-S3-Box ｜ 系统：**openvela**（NuttX） ｜ 大模型：**小米 MiMo**

---

## 一、作品简介

**植小伴是一台"会看病、会说话"的 AI 养植伴侣。**

市面上的养花监测器大多只做一件事：测出湿度、提醒你浇水，然后就没有然后了——
叶子为什么发黄、要不要施肥、是不是晒狠了，用户还是不知道，最后传感器进抽屉。

植小伴把这件事做成了**完整闭环**：**监测 → 诊断 → 养护任务 → 情感陪伴**。

| 能力 | 说明 |
|------|------|
| **八参数实测** | 一块小板接 RS485 八合一土壤传感器，实时读出 温度 / 水分 / EC / pH / 盐分 / 氮 / 磷 / 钾，三秒刷新一次 |
| **拍照即知** | 板载摄像头取景拍照 → 云端大模型看图 → 返回品种、匹配度、健康分与对症建议 |
| **会说话** | 按住说话、松手就行；回答不仅显示文字，还会**念出来**。说完话到出声 **6~8 秒**（优化前 43 秒） |
| **落成任务** | 诊断结果自动变成"今天该做什么"，设备上点一下确认，成长日记里就多一笔 |
| **有伴** | 手机端社区晒图、点赞、一键理赔——养死了也赔，历史传感器数据就是理赔凭证 |

**亮点**

1. **一颗 ESP32-S3 跑通四路外设**：图形界面（LVGL）、摄像头（DVP）、麦克风与扬声器（双 codec）、八合一土壤传感器（RS485/UART），并在 openvela 上稳定共存
2. **语音链路做了实测优化**：定位到 SD 卡写入只有 ~23.5 KB/s 这个瓶颈，改为**边下边播 + 音频瘦身（24k→16k、只念回答）**，端到端时延从 43 秒压到 6~8 秒
3. **三端齐活**：嵌入式固件 + 云平台 + 手机端 App，另有网页版管理台可运维设备
4. **成本极低**：BOM 不足 50 元

**代码规模**（自研部分）

| 端 | 语言 | 文件 | 行数 |
|----|------|------|------|
| 嵌入式固件 | C | 93 | **31,600** |
| 云平台 | Python | 56 | **6,160** |
| 手机端 App | Java | 2 | **942** |

> 另含约 15 万行第三方代码（乐鑫 esp_sr 语音识别库、cJSON、中文字模），未计入上表。

---

## 二、选题方向

**AI 硬件产品创新**（自定细分方向：智能养植伴侣）

选择理由：
- 有真实且具体的痛点——**80% 的室内绿植死于浇水不当**，而前代产品（Parrot / Edyn / Koubachi / PlantLink）全部阵亡，死因高度一致：**只测不管**
- 适合检验 openvela 的"图形 + AI + 多媒体"三项核心能力：我们三项**全部落地**
- 端云分工天然清晰：实时性与降级放端、大模型与多模态放云

---

## 三、目录结构

```text
contest2026_053_yelvtizhineng/
├── app/
│   └── plant-companion/        嵌入式固件（openvela / NuttX 应用）
│       ├── ui/                 LVGL 界面：首页 / 数据 / 任务 / 日记 / 拍照 / 语音 / 网络 / 升级
│       ├── components/         摄像头 DVP、土壤传感器、LCD、音频 codec、SD、电池监测
│       ├── services/           上云桥、AI 服务、语音服务、记录服务、OTA 服务
│       ├── ai_module/          AI 对话 / 看图 / 语音的端侧编排
│       ├── communication/      WiFi 管理、云客户端
│       ├── ota/                A/B 双槽 OTA（含设计文档与工作记录）
│       └── docs/               架构、摄像头交接、语音要求、内存预算等设计与调试文档
├── server/
│   └── plant_server/           云平台（FastAPI）：设备桥、AI 网关、社区、理赔、计费、管理台
├── mobile/
│   └── plant_android/          手机端 App（Android · Java）：首页、拍照问诊、问答、社区、理赔、会员
├── patches/                    nuttx 侧改动（需单独向 nuttx 仓提 PR，见第六节）
│   ├── nuttx_改动文件/          17 个被改动的 nuttx 源文件（按仓库相对路径存放）
│   ├── nuttx_板级配置/          esp32s3-box 板级配置目录（含我们新增的 openvela/defconfig）
│   └── _diff与清单/             nuttx_changes.diff、改动文件清单
├── docs/                       技术文档（架构与接口 / 编译烧录与部署 / 交接与调试 / 提交与展示）
├── tools/                      编译脚本、串口调试工具
├── logs/                       AI Coding 日志
├── README.md                   本文件
└── contest2026_053_yelvtizhineng.xml   manifest（含一条 <linkfile> 映射）
```

### 关于映射

植小伴嵌入式端是**标准 NuttX 应用**：`app/plant-companion/Make.defs` 里用
`$(APPDIR)/plant-companion` 注册自身，板级配置用 `CONFIG_PLANT_COMPANION=y` 打开。
因此 manifest 中映射为：

```xml
<linkfile src="app/plant-companion" dest="apps/plant-companion"/>
```

`repo sync` 后它会出现在 openvela 工作区的 `apps/plant-companion`，**无需改动任何路径即可编译**。

---

## 四、运行方式

### 4.1 拉取工程

```bash
repo init -u https://github.com/open-vela/contest2026_053_yelvtizhineng \
  -b dev-ai-contest-2026 -m contest2026_053_yelvtizhineng.xml
repo sync -c -j8
```

同步完成后，本仓位于工作区的 `contest2026_053_yelvtizhineng/`；
`app/plant-companion` 会软链到工作区 `apps/plant-companion`。

> Windows 用户提示：`repo` 创建工作区需要创建符号链接，请在 **WSL** 或
> **开启开发者模式**的 Windows（Git Bash）下操作，否则会在
> `.repo/manifests/.git` 处报权限错误。

### 4.2 编译固件

板级配置为 **`esp32s3-box:openvela`**（我们新增的 defconfig 在
`patches/nuttx_板级配置/nuttx/boards/xtensa/esp32s3/esp32s3-box/configs/openvela/defconfig`，
需一并合入 nuttx，见第六节）。

```bash
cd <工作区根目录>
./build.sh esp32s3-box:openvela distclean   # 首次或改过配置后
./build.sh esp32s3-box:openvela -j8
```

产物：`nuttx/nuttx.bin`

### 4.3 烧录

分区表：`nuttx/boards/xtensa/esp32s3/esp32s3-box/configs/openvela/partition-table-2mb-ab.bin`

```bash
esptool.py --chip esp32s3 --port /dev/ttyACM0 --baud 921600 \
  --before default-reset --after hard-reset write-flash \
  --flash-mode dio --flash-freq 80m --flash-size 16MB \
  0x8000  partition-table-2mb-ab.bin \
  0x10000 nuttx.bin \
  0x210000 nuttx.bin
```

> ⚠️ **两个应用槽都要写**。otadata 可能指向 `ota_0` 或 `ota_1`，
> 只写 `0x10000` 会出现"烧录成功但版本号没变"的现象。
> **不要 `erase_flash`，不要写 `0x0`。**

上电后屏幕显示 `=== Firmware V1.3.29 ===` 即成功。

### 4.4 硬件接线

| 外设 | 接法 |
|------|------|
| 土壤传感器 | RS485 转换模块 TTL 侧：TX → 板卡 **GPIO40**，RX → 板卡 **GPIO42**（UART0，9600 8N1，Modbus 从站地址 `0x02`） |
| 摄像头 | 板载 OV3660（DVP） |
| 音频 | 板载 ES7210（采集）+ ES8311（播放），I2S0 16bit/24kHz |
| SD 卡 | 板载 SDMMC（1-bit） |

> ⚠️ **GPIO42 / GPIO40 被摄像头 DVP（VSYNC / Y9）与土壤传感器 UART0 共用，不能同时使用。**
> 固件内已做互斥：进拍照页会暂停传感器轮询并完整关闭摄像头硬件；
> 离开拍照页再把引脚路由回 UART0。详见 `docs/01_架构与接口/02_硬件与接线.md`。

串口控制台走芯片内置 USB 串口。用 Python 打开串口做自动化时，
务必先把 DTR/RTS 置低，否则会触发板卡复位：

```python
import serial
s = serial.Serial()
s.port = "COM23"; s.baudrate = 115200; s.timeout = 0.05
s._dtr_state = False; s._rts_state = False
s.open()
```

### 4.5 云平台

```bash
cd server/plant_server
python -m venv .venv && . .venv/bin/activate
pip install -r requirements.txt
python run_server.py --host 0.0.0.0 --port 8000 --seed
```

配置项通过环境变量注入（`PLANT_AI_MODE=mimo` 等），详见 `docs/02_编译烧录与部署/04_云平台运行与部署.md`。
设备端在 `/mnt/sd/plant.cfg` 里填写 `server_host:server_port` 即可连上。

主要接口域：设备桥（心跳 / 遥测 / 语音 / 看图）、植物与任务、社区、理赔、计费、管理台。

### 4.6 手机端 App

```powershell
cd mobile/plant_android
.\build.ps1     # 需要 JDK 17 + Android SDK（build-tools 34.0.0 / platforms android-35）
```

产物：`build/ZhiXiaoBan.apk`。也可直接用浏览器打开云平台的 `/mobile` 页面（同一套前端）。

### 4.7 ⚠️ 运行前需要你自己配置的项

本仓**不含任何密钥**（已全部清理为占位符），完整跑起来需要填以下几处：

| 配置项 | 位置 | 说明 |
|--------|------|------|
| MiMo API Key | `patches/nuttx_板级配置/.../configs/openvela/defconfig` 的 `CONFIG_PLANT_AI_API_KEY` | 在小米 MiMo 控制台申请；`app/plant-companion/tools/test_mimo_*.py` 里也各有一处 |
| WiFi 账号密码 | `app/plant-companion/components/system_monitor/device_cfg.h` | 默认值已置为 `YOUR_WIFI_PASSWORD`，也可开机后在屏幕上选网输入 |
| 云平台地址 | 设备端 `/mnt/sd/plant.cfg` 的 `server_host`；手机端 `MainActivity.java` 的 `YOUR_SERVER_HOST` | 指向你自己部署的服务器 |
| 管理台口令 | 环境变量 `PLANT_ADMIN_PASSWORD` | 不设置则需显式配置 |

---

---

## 五、AI Coding 使用说明

本作品从需求定义、架构设计、嵌入式开发、云端与 App 实现，到调试与文档撰写，
**全程借助 AI 编程助手协作完成**，具体分工：

- **嵌入式驱动早期攻关**（摄像头 DVP 预览与花屏、LCD 花屏、土壤传感器读取、ES8311/ES7210 音频噪声）：
  由队友使用 **DeepSeek Harness** 在 openvela 工作区（Linux 虚拟机）内完成，共 6 个会话、约 14 万个事件
- **主程序、云平台、手机端与文档**：由本人在 **Codex** 辅助下完成

> **关于 AI Coding 日志（`logs/`）**：本作品主线程全程在 **Codex（桌面版）** 里完成。
> 需要如实说明的是，开发期我们**没有把 AI 工具开在 openvela 工作区内**，也没有留意到日志
> 要在会话结束时由采集钩子自动落盘——一开始的做法是先在本机把开发测试做起来，作品最后
> 整理好才一次性提交到 GitHub；加上**公司网络访问 GitHub 不稳定**，也倾向于把提交集中到
> 最后一次做、减少访问频率。所以官方钩子全程没有触发，`logs/` 到提交时仍然是空的。
>
> 这批日志是我们事后用**官方事件 schema**、从本机 Codex 的**原始会话转录**导出补齐的。
> 官方已发布版的 Codex 适配只能解析 Claude Code 形状的转录（官方仓在途 PR #52 才是针对
> Codex rollout 格式的解析器，尚未合入，且带“会话须在工作区内”的闸门），因此我们按该 PR 的
> 内容规则（只保留 user/assistant 真实对话、丢弃工具前言）实现了等价的格式摊平，并额外保留
> 思考过程与工具调用，**已通过官方防作弊校验脚本 `validate-log.py`（ALL OK，43,563 条事件 / 14 个文件）**。
> 每个会话的源文件 sha256 记录在 `logs/tadycharming/manifest.json`，可逐条核对；
> 详情与限制见 [`logs/README.md`](./logs/README.md)。
>
> 团队**绝不伪造、修改任何日志**。队友 **杨涛** 前期在 Linux 虚拟机的 openvela 工作区内用
> **DeepSeek Harness（dsh）** 做的嵌入式驱动调试会话（6 个会话，原始约 14 万条事件、
> 折算为 8,337 条对话与工具事件）也已一并提交：因为该工具取值不在官方 schema 的
> `tool` 枚举内，无法生成符合 schema 的事件，故放在 `logs/_unsupported_tool_dsh/`
> 作补充材料，并**保留真实 `tool` 取值、不伪装成其它工具**；正式校验集
> （`logs/tadycharming/`）仍是 ✅ ALL OK。该工具能否计入有效工时以组委会结论为准。

### 协作方式

| 环节 | 与 AI 的协作方式 |
|------|-----------------|
| 需求拆解 | 用对话梳理使用场景与验收标准，逐条落成可验证的需求条目 |
| 方案设计 | 让 AI 对比端云职责划分、降级策略与引脚复用方案，产出取舍依据 |
| 编码 | AI 直接读改仓库代码：新写界面页、驱动适配、协议桥接，并同步补 Kconfig / Make.defs |
| 调试 | **最关键的用法**：把串口日志、对象树转储、电平实测贴给 AI，由它定位根因并给出改法 |
| 验证 | 让 AI 写脚本做闭环验证：串口注入点击、拍照后自动比对识别结果、OTA 全流程回归 |
| 文档 | 把调试结论沉淀成交接文档（摄像头、语音、OTA、内存预算等），便于后续接手 |

### 几个真实例子

1. **语音时延 43 秒 → 6~8 秒**：AI 逐段测量链路耗时，定位到 SD 卡写入只有 ~23.5 KB/s，
   提出"边下边播 + 音频瘦身（24k→16k、只念回答）"，改完实测达标
2. **引脚复用冲突**：摄像头 DVP 与土壤传感器 UART0 共用 GPIO42/40，
   AI 设计出"进拍照页关闭摄像头并暂停轮询、离页回收引脚"的互斥序列
3. **OTA "升了不生效"**：AI 从启动日志与 otadata 状态定位到分区表与应用槽位不一致，
   改为**双槽同写**并在文档里写清，避免后人再踩
4. **界面小字看不见**：AI 发现任务页标题标签从未写入文本、子控件吞掉了点击事件，
   两处修复后任务页才真正可用

### AI 在效率上的实际帮助

- 串口日志驱动的"贴日志 → 定位 → 改 → 复测"闭环，把单点问题的平均定位时间从小时级压到分钟级
- 三端代码风格与接口约定由 AI 保持一致，跨端字段对齐几乎不出错
- 文档与代码同步更新，交接成本大幅下降

完整对话日志见 [`logs/`](./logs/)。

---

## 六、nuttx 侧改动（需单独提 PR）

按大赛规则，**公共仓库不在本仓修改**。本作品对 nuttx 的改动共 **17 个文件**，
全部放在 `patches/` 下，并附完整 diff（`patches/_diff与清单/nuttx_changes.diff`）。

改动主题：

| 主题 | 涉及文件 |
|------|---------|
| 摄像头 DVP 驱动与引脚回收 | `arch/xtensa/src/esp32s3/esp32s3_cam_dvp.[ch]` |
| UART0 引脚回收（解决与摄像头复用冲突） | `arch/xtensa/src/esp32s3/esp32s3_lowputc.[ch]`、`Make.defs` |
| 串口写入限时等待（避免"打开串口不读"拖死控制台） | `drivers/serial/serial.c` |
| I2S 异步写与音频链路 | `arch/xtensa/src/esp32s3/hal_i2s.c` |
| 内存/PSRAM 布局 | `esp32s3_allocateheap.c`、`esp32s3_spiram.[ch]` |
| SDMMC、WiFi 适配稳定性 | `esp32s3_sdmmc.c`、`esp32s3_wifi_adapter.c`、`esp32s3_wlan.c` |
| 板级配置与触摸 | `configs/openvela/defconfig`、`configs/lvgl/defconfig`、`esp32s3_board_touchsceen_gt911.c` |
| LCD 驱动 | `drivers/lcd/st7789.c` |
| littlefs | `fs/littlefs/Make.defs` |

---

## 七、已知限制与后续工作

1. **继电器未接入**：养护方案的"自动执行"目前是干跑形态（设备上点确认即完成），
   接上继电器后沿用同一套接口即可真正驱动水泵与补光灯
2. **偶发崩溃**：摄像头 DVP 启动与语音录音启动各出现过一次 `EXCCAUSE=001c` 异常，
   复位可恢复、非必现，正在定位（疑似 DMA 描述符或缓存一致性）
3. **SD 卡 1-bit 模式**：写入带宽受限，语音链路已改为不落卡以绕开
4. **唤醒词未启用**：当前是按键说话，未使用大赛统一唤醒词

---

## 八、致谢

感谢大赛组委会提供的 openvela 平台与小米 MiMo 大模型能力。
