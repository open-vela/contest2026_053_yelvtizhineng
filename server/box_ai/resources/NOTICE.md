# BOX 题库源数据与许可

`qa_entries.jsonl.gz` 从 2026-11-04 正在使用的只读 FAQ 快照导出，共 43,406 条。导出只改变序列化和压缩容器，保留条目 ID、问题、答案、示例、来源、引用 URL/修订版本、署名、`CC-BY-SA-4.0` 许可及 `independent_fact_verification` 字段。它是知识资料，不是用户对话或遥测数据库。

许可入口：[Creative Commons Attribution-ShareAlike 4.0](https://creativecommons.org/licenses/by-sa/4.0/)。具体来源和署名以逐条 `attribution`、`references` 为准；本说明不把第三方资料重新声明为本项目原创，也不替代逐条来源记录。该许可说明仅针对题库资料，不改变其他项目代码的许可状态。

条目中的模型复核与独立事实核验是不同标记，导出未提高原有核验等级。返回答案不应被解释为所有内容已获专家验证。`qa_database_schema.json` 保存数据库结构及源快照元数据；脚本按同样归一化、别名和二元字索引重建，数据库二进制摘要可能因 SQLite 版本而不同。
