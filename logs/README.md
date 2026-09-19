# logs/ — AI Coding 日志

本目录是**叶绿体智能**（队伍 053，仓 `contest2026_053_yelvtizhineng`）提交的 AI Coding 对话日志，
来源是本机 **Codex（桌面版）** 的真实会话转录，共 **3 个会话、43,563 条事件、14 个文件**。

## 一、目录结构

```text
logs/
└── tadycharming/                                  # 提交人 GitHub 用户名
    ├── manifest.json                              # 会话清单（含每个源文件的 sha256）
    ├── 2026-09-07/codex__01a07a44-….jsonl
    ├── 2026-09-07 … 2026-09-11/codex__01a07b48-….jsonl   (5 个文件)
    └── 2026-09-11 … 2026-09-19/codex__01a08e5f-….jsonl   (8 个文件)
```

| 会话 | 时间 | 事件数 | 文件数 | 这段时间在做什么 |
|------|------|--------|--------|------------------|
| `01a07a44…` | 2026-09-07 | 2,267 | 1 | 早期工程梳理：拉取 apps / nuttx / vendor 仓库、目录整理、编译链打通 |
| `01a07b48…` | 2026-09-07 → 09-11 | 19,065 | 5 | 主线程前段（即"原对话"，后因上下文过长崩掉） |
| `01a08e5f…` | 2026-09-11 → 09-19 | 22,231 | 8 | 主线程后段：接手前段继续做到提交（嵌入式 / 云平台 / App / 文档） |
| **合计** | | **43,563** | **14** | |

> **为什么有的会话拆成多个文件**：会话跨天时，按事件时间戳所属的日期拆到对应日期目录下
> （一天一个文件），**`seq` 在整个会话内保持连续**。官方校验脚本对这种"一个会话分布在多天文件"
> 的形态是明确支持的（按同一 `session_id` 聚合校验 `seq` 与事件数），`manifest.json` 里也逐条列出了
> 每个会话包含哪些文件。

## 二、为什么这批日志不是工具自动落盘的

我们如实说明，原因有两条：

1. **开发期没有在工作区内开 AI，也没留意日志归集这件事。**
   一开始我们的做法是"本地先把开发测试做起来"，作品是最后整理完成后才一次性提交到 GitHub 的。
   官方的自动采集钩子要求 AI 工具在 openvela 工作区（能找到 `.repo/` 的目录树）内运行、
   并在会话结束时落盘——我们全程是在 **Windows 侧的项目目录**
   （`C:\Users\admin\Documents\ChatGPT\植小伴`）里和 AI 协作的，所以钩子没有触发，
   `logs/` 一直到提交时都是空的。

2. **公司网络访问 GitHub 不稳定，我们倾向于把提交集中到最后一次做，降低访问频率。**
   也就没有在开发过程中反复向 GitHub 同步，日志自然也没有随开发进度提交。

这两条是我们的实际情况，**不是**为了规避采集而故意为之。

另外还有一层技术原因：官方工具的 **Codex 适配只认 Claude Code 形状的转录**，
而 Codex（桌面版 / CLI）的会话文件是 `{timestamp, ordinal, type, payload}` 结构，
正文在 `payload` 里；官方已发布版取不到内容，导出结果为 0 条且不报错。
官方仓里的 **在途 PR #52**（`feat(collector): add Codex backfill (CLI + IDE extension + desktop app)`）
才是针对这个格式写的解析器，但它还没合入（作者的 CLA 未签），
并且它带一道"会话 cwd 必须在 `.repo/` 工作区内"的闸门，会把我们这批会话全部过滤掉。
相关证据与复现步骤我们已随问题反馈给组委会。

## 三、这批日志是怎么导出的

**没有修改、删减任何内容**，做的是"把原始转录摊平成官方 schema"这一件事：

- **数据来源**：本机 Codex 的原始会话文件
  `%USERPROFILE%\.codex\sessions\**\rollout-*.jsonl`（桌面版与 CLI、IDE 插件共用同一套 rollout）。
  `manifest.json` 里逐条记录了源文件的**绝对路径 + sha256 + 字节数**，可核对。
- **内容规则**：与官方 PR #52 的 Codex 适配保持一致——
  只保留 `role = user / assistant` 的真实对话，
  丢弃工具前言（`<permissions instructions>` / `<skills_instructions>` /
  `<environment_context>` / `<app-context>`）。
- **额外保留**：`reasoning` → `thinking`、`function_call` → `role=tool` + `input`、
  `function_call_output` → `role=tool` + `output`。
  这几类在官方事件 schema 的字段表里都有（`thinking` / `tool_name` / `input` / `output`），
  Claude Code 的官方采集路径同样会记录它们。
- 事件字段：`schema_version / session_id / team_id / github_login / tool / seq / ts / role`
  + 上述内容字段；`seq` 在每个会话内从 0 单调递增。

## 四、校验

官方防作弊校验脚本（`contest-log-collector/tools/validate-log.py`）对本目录的检查结果：

```text
=== contest-log-upload validation ===
Files checked:  14
Events checked: 43563

✅ ALL OK
```

校验内容包含：事件与 manifest 是否符合官方 JSON Schema、`seq` 是否单调无重号、
manifest 的 `event_count` 与实际行数是否一致（跨天多文件按同一会话聚合）。

## 五、已知限制

1. **只有主线程的会话。** 本机共 11 个候选 rollout，其中 8 个属于本产品的会话已全部收录；
   另外 3 个会话的 cwd 是 `AI_CODING`（一个是问 Codex skills 怎么用，两个是 `reply with ok`
   之类的探测），与本作品无关，**按隐私门控的原则未收录**。
2. **队友的驱动调试会话单独放在补充目录。** 那部分是用 **DeepSeek Harness（dsh）**
   在 Linux 虚拟机的 openvela 工作区内完成的（6 个会话；原始约 14 万条事件，
   折算成对话与工具事件 8,337 条）。该工具的取值不在官方 schema 的 `tool` 枚举内
   （`opencode / claude-code / codex / kiro / mimocode / cursor`），无法生成
   schema-valid 的事件，因此放在 `logs/_unsupported_tool_dsh/` 作补充材料
   （详见下一节），并保留真实的 `tool` 取值，不伪装成其它工具。
3. **官方工作区闸门覆盖不到我们的用法**（会话 cwd 在工作区外），
   这一点也已一并反馈，等待组委会结论。

## 六、附：队友 DeepSeek Harness（dsh）会话

队员 **杨涛** 前期在 Linux 虚拟机的 openvela 工作区（`/home/vboxuser/openvela`）内
用 **DeepSeek Harness（`@deepseek-ai/dsh`）** 做嵌入式驱动调试，共 **6 个会话、8,337 条事件**。

该工具的取值 **不在官方 schema 的 `tool` 枚举内**，无法生成符合 schema 的事件，因此：

- 放在 `logs/_unsupported_tool_dsh/<handle>/` 作为**补充材料**（目录名以 `_` 开头，明确非标准）；
- 该目录**没有 `manifest.json`**，官方校验脚本不会把它当成员目录解析 →
  本目录（`logs/`）的正式校验结果不受影响，仍是 ✅ ALL OK；
- 事件里**保留真实 `tool` 取值 `dsh`**，不伪装成枚举里的其它工具；
- 内容规则与 codex 部分一致：保留对话正文 / 思考 / 工具调用与结果，
  丢弃流式增量事件（`*-chunks`，内容已被 `assistant/message` 覆盖）、运行时状态事件
  （`step/*`、`turn/*`、`permission/*` 等）与 `<system-reminder>` 工具前言；
- 数据来源是该工具自带的会话导出包 `dsh-session-session-*.zip`，
  内部 `session.jsonl` 的 **sha256 / 字节数**记在
  `logs/_unsupported_tool_dsh/<handle>/sources.json`，可逐条核对。

> 该工具是否计入"有效工时"以组委会结论为准（手册目前列出六种工具）。
> 这部分工作记录本身是真实的，故一并提交备查。

## 七、如何自行核对

```bash
# 1) 官方校验（应输出 ✅ ALL OK）
python3 <contest-log-collector>/tools/validate-log.py logs/

# 2) 核对源文件哈希（与本目录 manifest.json 中 source_integrity 对得上）
python3 - <<'PY'
import hashlib, json, pathlib
m = json.loads(pathlib.Path("logs/tadycharming/manifest.json").read_text(encoding="utf-8"))
for s in m["sessions"]:
    for f in s["source_integrity"]["source_files"]:
        p = pathlib.Path(f["path"])
        if p.is_file():
            h = hashlib.sha256(p.read_bytes()).hexdigest()
            print("OK " if h == f["sha256"] else "MISMATCH", p.name)
        else:
            print("missing (仅本机存在)", p.name)
PY

# 3) 终端预览 / HTML 报告（官方渲染工具）
python3 <contest-log-collector>/tools/render-log.py logs/tadycharming/
python3 <contest-log-collector>/tools/render-log.py logs/tadycharming/ --format html --out report.html
```

> 重申：本团队**没有伪造、修改、删减**任何日志内容；
> 上述文件是真实会话转录按官方 schema 的导出结果。