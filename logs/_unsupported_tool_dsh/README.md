# 队友 DeepSeek Harness（dsh）会话 — 非官方枚举工具

本目录是队员 **杨涛（yang-asjdsad）** 的 AI Coding 会话（前期嵌入式驱动调试），
工具是 **DeepSeek Harness（`@deepseek-ai/dsh`）**，共 **6 个会话、8,337 条事件、9 个文件**。

## 为什么单独放这里

官方 `contest-log-collector` 的事件 schema 把 `tool` 定义为**固定枚举**：

```json
"enum": ["opencode", "claude-code", "codex", "kiro", "mimocode", "cursor"]
```

**`dsh` 不在这个枚举里**，所以这些会话无法生成"符合 schema 的事件"：

- 如果放进 `logs/<login>/` 当成正常成员目录，官方校验脚本会因 `tool` 取值非法而 **❌ FAILED**；
- 如果硬填成上面某一个值（例如 `codex`），那就是**在必填字段上写不实内容**——我们不这么做。

因此这个目录是**补充材料**：

- 目录名以 `_` 开头，明确标注"非标准"；
- 目录里**没有 `manifest.json`**，所以官方 `tools/validate-log.py` 不会把它当成员目录解析
  （校验脚本只认"含 manifest.json 的目录"），`logs/` 的正式校验结果仍是 **✅ ALL OK**；
- 事件里的 `tool` 保留真实值 **`dsh`**，不做任何伪装。

> 关于"该工具是否计入有效工时"，组委会的手册目前支持 Claude Code / AIoT-IDE / OpenCode /
> Codex / MiMo Code / Cursor 六种；我们已把这一情况连同证据反馈组委会，最终以组委会结论为准。
> 无论结论如何，这部分工作记录本身是真实的，所以一并提交备查。

## 数据来源与做法

- **来源**：该工具自带的会话导出包
  `dsh-session-session-<id>.zip`（每个 zip 内含一个 `session.jsonl`）。
  本目录 `yang-asjdsad/sources.json` 里逐条记录：源 zip 文件名、zip 内文件路径、
  **内部 `session.jsonl` 的 sha256 与字节数**，可逐条核对。
- **保留的内容**：`user/message` 的对话正文、`assistant/message` 里的
  思考（→ `thinking`）、回复正文（→ `text`）、工具调用（→ `role=tool` + `tool_name` + `input`）、
  工具结果（→ `role=tool` + `output`）。
- **丢弃的内容**（不含信息损失）：
  - `*-chunks`（`reasoning-chunks` / `assistant/chunk` / `text-chunks` / `tool-call-chunks`）：
    这些是**流式增量**，其内容已被完整的 `assistant/message` 覆盖；
  - `step/*`、`turn/*`、`session/*`、`permission/*`、`sandbox/*`、`approval/*` 等**运行时状态事件**；
  - `<system-reminder>…` 这类**工具前言**（与官方对 codex 会话的处理规则一致）。
- **字段**：`schema_version / session_id / team_id / github_login / tool / seq / ts / role`
  + 内容字段；`seq` 在每个会话内从 0 单调递增；`ts` 由原始毫秒时间戳转成 ISO 8601 UTC。
- 会话跨天时按天拆文件，`seq` 跨文件连续。

**没有伪造、修改、删减任何对话内容**；对原始事件的选取规则如上，且全部写在明处。

## 会话清单

| session | 时间 | 事件数 | 文件 |
|---|---|---|---|
| `c25afd5e-…` | 2026-08-19 | 135 | 1 |
| `6577e57e-…` | 2026-08-27 → 09-01 | 3,171 | 3 |
| `ad70f4f7-…` | 2026-08-27 | 1,935 | 1 |
| `4dc26ba6-…` | 2026-09-01 | 500 | 1 |
| `1598ade2-…` | 2026-09-09 | 680 | 1 |
| `48264a9f-…` | 2026-09-09 → 09-10 | 1,916 | 2 |
| **合计** | | **8,337** | **9** |

> 全部会话的 `cwd` 都是 `/home/vboxuser/openvela`，即 openvela 工作区内
> （官方"工作区闸门"对这批会话是满足的）。

## 目录名说明

目录名用的是该队员的常用 handle `yang-asjdsad`（其 Gitee 账号；邮箱 `yt202066348@163.com`）。
若其 GitHub 用户名与此不同，直接把这一级目录改名即可（本目录不参与 schema 校验，改名不影响校验结果）。