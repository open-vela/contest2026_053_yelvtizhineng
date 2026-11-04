# 植小伴无线土壤传感器

本模块将 RS485 八参数探头接入独立 ESP32-WROOM-32E 节点，使用 openvela/NuttX 完成采集、Wi-Fi 配网、MQTT/TLS 上报及周期深度休眠。它是植小伴系统的数据采集端，所属赛道为 AI 硬件产品创新；此节点本身不运行大模型。

源码来自 2026-11-04 构建机导出的 2026-11-01 Wi-Fi 等待卡死修复版。整理仅调整 `soil_sensor.c` 的两个头文件引用和 Makefile 的头文件搜索目录，以支持 manifest 软链接；其余应用仅规范换行及文件末尾空行，保留源包逻辑。

| 文件 | 职责 |
| --- | --- |
| soil_node_main.c | 启动、配网、采样、上报、休眠调度 |
| soil_sensor.c | RS485 收发、CRC 校验、八参数解析 |
| soil_wifi.c / soil_http.c | STA、DHCP、SoftAP、手机配网页面 |
| soil_config.c / soil_state.c | LittleFS 配置及运行状态 |
| soil_mqtt.c / soil_mqtt_ca.h | MQTT、TLS、CA 与主机名验证 |
| soil_power.c / soil_time.c | 深度休眠及 NTP 校时 |
| soil_cloud_config.h | 本样机云端地址及标识，不含密码 |
| Kconfig / Makefile / Make.defs | NuttX Make 构建入口 |

运行步骤见 [运行指南](../../docs/soil-node/运行指南.md)，协议见 [接口与配置](../../docs/soil-node/接口与配置.md)，公共仓改动见 [补丁说明](../../patches/soil-node/README.md)。本次支持和验证的是 Make 构建；已有 CMakeLists.txt 未验证为可用构建入口。

## AI Coding 使用说明

借助 Codex 完成需求拆解、采集与网络代码、SoftAP/DHCP 排查、Wi-Fi 等待卡死定位和文档整理；用户负责接线、现场配置和实机确认。AI 记录要求及本次日志状态见 [日志说明](../../logs/SOIL_NODE_20261104.md)。测试事实与未完成项目见 [提交检查](../../docs/soil-node/提交检查.md)。
