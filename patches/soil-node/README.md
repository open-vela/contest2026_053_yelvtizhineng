# 无线土壤节点公共仓补丁（待单独提 PR）

比赛要求作品代码在队伍仓维护；NuttX 等公共仓修改应 fork 对应仓后向 `dev-ai-contest-2026` 提 PR，由组委会审核。本目录保存待审依赖和本地复现补丁，**不代表这些修改已合入公共仓，也不代替公共仓 PR**。本次队伍仓更新不包含向 NuttX/apps 等公共仓创建 PR。

| 文件 | 目标 | 内容 |
| --- | --- | --- |
| nuttx.patch | open-vela/nuttx | SoftAP 配置初始化及 carrier、UART、GPIO5、自动启动、mbedTLS 构建等 |
| apps.patch | open-vela/nuttx-apps | MQTT-C 复用 ESP HAL 的 mbedTLS 和当前源码的构建方式 |
| esp-hal.patch | ESP HAL | mbedTLS 平台时间函数、锁初始值及 ESP32 Wi-Fi 配置兼容 |
| baselines.json | 核对清单 | 官方基线提交及每个改动文件的前后 SHA256 |

已逐文件确认：本次涉及的 10 个 NuttX 和 3 个 apps 原文件，与 2026-11-04 查询到的官方 `dev-ai-contest-2026` 对应内容一致。补丁据此重新生成；源码包中构建机本地快照提交仍保留在 source_versions.json 作来源证据，不能当作官方可下载提交。

未收录原源码包的自动生成 `apps/Kconfig`（含构建机绝对路径）和 cJSON 下载代理改动（本固件未启用 cJSON）。应用通过 manifest 映射发现，Kconfig 由构建系统生成。板配置通过独立 linkfile 引入。

原源码包只收录了一处 HAL 时间函数修改。本次重新核对构建机，补充 `clk_ctrl_os.c`、`modem_clock.c` 中的 `SP_UNLOCKED` 以及 `sdkconfig.h` 对 `CONFIG_ESP32_WIFI` 的兼容分支；四处修改均与构建机一致。HAL 内 mbedTLS 子模块已有 SDK 自带补丁，构建时仍需按 SDK 规则处理，不重复打包为产品修改。

这些补丁用于复现现有样机。正式公共仓 PR 前还需将通用修复与产品专用修改拆开，尤其 MQTT Kconfig 对 SOIL_NODE 的依赖、board 自启动、GPIO 分配和“补丁失败即已应用”的旧兼容处理，应重新整理成适合公共仓的方案；不能直接把整包当作已经通过上游规范审查的修改。
