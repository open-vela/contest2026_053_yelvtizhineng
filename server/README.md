# 植小伴服务器端（2026-11-04 源码整理版）

本次源码从运行服务器只读采集，涵盖主业务 API、手机网页/PWA、管理台、BOX 语音与图像 AI 和JSON 土壤 MQTT 网关。代码、部署映射及验证范围见 [部署指南](../docs/server/部署指南.md)、[来源与验证](../docs/server/来源与验证.md)。

| 目录 | 内容 | 运行目录 / 端口 |
| --- | --- | --- |
| plant_server | FastAPI、账号/设备/植物/任务/日记/科普与图鉴、PWA、管理台 | /opt/plant_server / 8011 |
| box_ai | BOX 独立 AI、标准流式语音、快速图像识别、按设备绑定的土壤读取 | /opt/plant_box_voice_20261031 / 9012（本机） |
| soil_gateway | ESP32 节点的 JSON MQTT → HTTP 网关 | /opt/plant_soil |
| deploy | systemd 和环境变量示例、反向代理示例 | 需按自己的部署填写 |
| tools | 题库重建、首次运行数据准备、源码自测 | 不连接线上环境 |
| tests | 学习/图鉴、MQTT 网关及整体 API 回归 | 临时目录和测试数据 |

## 本地运行主 API

在仓库根目录执行（Python 3.11 或 3.12）：

```bash
python3 -m venv .venv-server
. .venv-server/bin/activate
python -m pip install -r server/requirements.txt
python server/tools/prepare_runtime.py
export PLANT_AI_MODE=mock
export PLANT_TOKEN_SECRET="$(python -c 'import secrets; print(secrets.token_hex(32))')"
cd server/plant_server
python run_server.py --host 127.0.0.1 --port 8011
```

Windows 可激活 `.venv-server/Scripts/Activate.ps1`，用 `$env:PLANT_AI_MODE='mock'` 和 `$env:PLANT_TOKEN_SECRET` 设置同名环境变量。

浏览器打开 `http://127.0.0.1:8011/mobile/`、`/admin/`、`/docs`。本地默认演示验证码为 `123456`；这不是线上验证码。主服务无模型密钥时支持 mock；BOX 独立服务按原版设计要求真实模型和设备白名单，不能把 mock 运行宣称为真实模型验证。

## 题库与边界

BOX 使用的 43,406 条条目以 [压缩源数据](box_ai/resources/qa_entries.jsonl.gz) 提供，由 `python server/tools/build_box_qa_snapshot.py` 重建只读 SQLite。保留每条来源、引用版本、署名、许可和独立事实核验标记，见 [题库来源说明](box_ai/resources/NOTICE.md)。不提交 133 MB 的运行时数据库。主服务 1,248 条题库由运行准备脚本复制到新建数据目录，不覆盖已有数据。

本目录没有包含线上用户数据库、实际设备绑定、密钥、环境文件、媒体上传、日志、音频缓存或模型权重。展会介绍视频和历史 APK 是发布素材，需按自己的发布流程配置；原仓已有 APK 本次未更新。M5 视觉场景记忆的可选模型及依赖见部署指南。

## 自测

```bash
python -m pip install -r server/requirements-test.txt
python server/tools/prepare_runtime.py
python server/tests/test_learning.py
PYTHONPATH=server/soil_gateway python -m unittest discover -s server/tests -p test_mqtt_soil_gateway.py -v
python server/tests/test_release.py
```

测试使用临时数据库和本机 HTTP，不使用线上用户数据，不调用付费模型。生产部署前替换配置模板中的占位值；管理台包含本机服务和 MQTT 账号管理能力，其权限需求与公开 API 的反向代理边界见部署指南。
