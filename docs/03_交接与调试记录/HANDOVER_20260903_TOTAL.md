# 植小伴（Plant Companion）项目总包交接（2026-09-03）

> 面向接手同事的整体状态汇总。比 HANDOVER_20260831.md 更新（含 09-02/09-03 全部改动）。
> 数据/结论均来自实机验证或工作日志，标注"待烧录验证"的项未上机。

---

## 0. 一句话现状

**语音 + 图像 + 云 三大链路已全通并实机验证**；近期集中解决了：SD 挂载预热、音频"一卡一卡"
（TX/RX DMA 流水线）、开机首帧渲染崩溃（SD 字库违反 LVGL v9 契约）、UI 图标 emoji 缺字。
**当前最大遗留：录音底噪大/人声模糊**——已用数据定位到"服务器降噪算法天花板 + 采集 SNR 低"，
NN 降噪方案（DeepFilterNet3）已验证可行待集成。

---

## 1. 硬件平台与系统架构

### 硬件
- **ESP32-S3-WROOM**（512KB SRAM，**无 PSRAM**——硬约束，NSNet2 类大模型无法设备端运行）
- 16MB flash，A/B OTA（ota_0/ota_1 + otadata）
- 外设：ST7796 LCD 480×320、GT911 触摸、OV3660 相机、ES8311 DAC + ES7210 ADC（**2 物理麦克风**）、
  ICM-42607 IMU、RS485 土壤传感器、SD 卡（SDIO 1-bit）、WiFi
- 音频引脚：MCLK=IO2 BCLK=IO17 WS=IO45 DOUT=IO15(DAC) DIN=IO16(ADC) PA=IO46
- ⚠️ SD_CMD=IO0 = BOOT 键（运行中按 BOOT 会拉低 CMD → SD 出错）；无卡检测引脚，卡须开机前插好

### 架构（用户 2026-08-29 拍板，勿推翻）
```
[设备 = 薄客户端]                  [局域网服务器 = 辅助手]            [云端]
LVGL UI ──▶ voice/image/记录 ──▶  server_bridge.py ──▶ MiMo API（文本/音频/图像/TTS）
传感器/任务/日记 ──────────────▶  /data 端点(JSON 落盘) + paho-mqtt ──▶ MQTT broker
OTA UI ──▶ 检查更新 ──────────▶  OTA 服务器(HTTP 固件) ──▶ 设备 A/B 自升级
```
- 设备**零 MQTT/零 TLS 直连**（NuttX 栈吃紧 + 无 PSRAM），一切经服务器中转
- 服务器地址为 RAM 变量（用户否决持久化），重启后需 `plant voice server <ip>`

### 音频链路（采样率链，已多证据验证正确）
```
采集: ES7210 2 麦 24kHz → I2S RX(双深预读 DMA) → ring FIFO → read_slot(MIC1)
     → 24k→16k 降采样 → (说话按钮: 流式上传 / diag: 存 WAV)
处理: 服务器 noisereduce(降噪) → MiMo 理解 → 文本 + TTS(24k WAV)
播放: 24k → hal_i2s_write_async(窗口化流水线) → ES8311 → PA
```
频率链 24k 采/24k 放/16k 存传，与 xiaozhi esp-box-3 **完全一致**（config.h 同 24000）。

---

## 2. 功能完成度总表

| 模块 | 状态 | 备注 |
|---|---|---|
| 7 屏 UI（首页/数据/拍照/诊断/任务/语音/日记） | ✅ 实机 | LVGL v9，中文字体 SD 全量 + flash 子集兜底 |
| 传感器轮询 + 植物卡/进度环/趋势图 | ✅ 实机 | |
| 土壤传感器 RS485 | ✅ 实机 | |
| 语音全链路（说话→服务器→MiMo→TTS 回播） | ✅ 实机 | 2026-08-31 验证 |
| 图像识别（拍照→服务器→MiMo 诊断） | ✅ 实机 | 2026-08-31 验证 |
| OTA A/B + 自动回退 + **UI 化**（检查更新按钮） | ✅ 实机 | 09-03 验证（含回退） |
| 顶栏实时时间（HTTP 抓取，60s 刷新） | ✅ 实机 | 09-03 验证 |
| 任务/日记本地持久化 + 成就系统 | ✅ 实机 | |
| SD 卡挂载（预热重试 + 后台 30s 重挂） | ✅ 实机 | 09-02 修复后稳定 |
| 服务器 C1 /data 端点 + E MQTT 上云 | ✅ 服务器侧 | 2026-09-03，curl+假 broker 验证 |
| 图标/emoji 字体链（zh→符号→emoji） | 🟡 已编译待烧录 | 09-03 15:32 nuttx.bin |
| 语音唤醒词 + 多轮对话 | ⬜ 未做 | 无 PSRAM，NSNet2 设备端不可行 |
| 照片 SD 入库日记 | ⬜ 未做 | 相机已通，缺存档 UI |
| 植物百科 / 养护知识库 | ⬜ 未做 | 内容型 |
| C2 服务器定时任务 / C3 设备拉取展示 / C4 手机写日记 | ⬜ 未做 | 服务器+设备 |
| E2 设备传感器周期上报 | ⬜ 未做 | |
| TTS 800KB 流式播放（发送慢优化） | ⬜ 未做 | 现整轮 ~1 分钟 |
| 24h 全外设压测 / 稳定性 | ⬜ 未做 | Phase 4 |

---

## 3. 音频专项：现状 + 结论（接手人必读，勿重走弯路）

### 已解决（本阶段）
| 问题 | 根因 | 修复 |
|---|---|---|
| 播放一卡一卡（tone 吱吱吱） | TX 逐块阻塞，块间 DMA 空窗 | hal_i2s_write 窗口化流水线 + async/flush（09-02） |
| 录音一卡一卡 + 录制长播放短 | RX 预读链单深，apb 间空窗丢数据（墙钟 5.9s≈2×） | RX 双深预读链 A/B 乒乓（09-02，墙钟回 ~4.2s） |
| 播放/录音文件头静音 | 录音含开口前环境声 | 需语音起始裁剪（未做，见遗留） |
| 开机首帧渲染崩溃 | SD 字库 get_glyph_bitmap 违反 LVGL v9 契约（返回裸指针） | zh_font 解码 A4→A8 进 dbuf 并返回 dbuf（09-02） |

### 遗留：底噪大 / 人声模糊 / "怪兽声"（核心未解）
**数据定位结论**（勿再怀疑频率/TDM/通道数——已排除）：
1. **频率链正确**：录音墙钟反推实际 24kHz；tone 音高正确；与 xiaozhi 采样率一致
2. **采集链路健康**：麦克风底噪 -54dBFS、无削波、RX 数据完整
3. **真因 = SNR 低 + 降噪算法天花板**：
   - 采集 SNR 仅 ~11dB（安静段峰值 1569 vs 说话 5472；08-25 基线 22dB）——环境
     100-300Hz 轰鸣 + 说话音量/距离
   - 噪声与语音元音（85-250Hz）**同频段重叠**，传统频谱门控（noisereduce）原理性无法分离：
     实测 0.3 参数降 0.2~3.5dB（不可闻），0.75 参数也只降 ~4dB
   - xiaozhi 干净 = 设备端 **NSNet2 神经网络**降噪（我们无 PSRAM 装不下 → 降噪上移服务器）
4. **已验证可行的方向**：服务器端 **DeepFilterNet3（ONNX）** 已在本机跑通现有录音
   （pip install deepfilter-stream，48k 路径 0.9s/3s 音频，各频段降 2.5~6.4dB；
   效果优于 noisereduce 但受样本限制未到理想，需受控采集再评估调参）
   ——模型文件在 `/home/vboxuser/df3/`（denoiser_model.onnx + states，12.9MB）
5. **待办（按序）**：
   - [ ] 受控采集验证：纯环境噪声样本 + 说话样本，测 DF3 真实上限（纯噪应压 15-20dB）
   - [ ] 调 DF3 atten_lim 参数（可混入干声防过抑制）→ 集成替换 `pcm_denoise()`
   - [ ] （体验）录音语音起始检测，自动裁掉开头环境声段
   - [ ] （优化）RX 每块丢头尾 128/160 → 16（当前丢 28.8% 拖慢 40%）
   - [ ] 日志更正：hal_i2s_dump_tx 的 TX_HUNG 注释是错的（播放结束 FIFO 空=正常）

---

## 4. 构建 / 烧录 / 运行

```bash
# 编译（根目录）
cd /home/vboxuser/openvela
source ~/openvela-venv/bin/activate
./build.sh vendor/espressif/boards/esp32s3/esp32s3-box/configs/openvela/
# 产物 nuttx/nuttx.bin（app 镜像）

# 烧录（只改 app 时直接覆盖 0x10000，勿 erase_flash、勿写 0x0）
esptool --chip esp32s3 --port /dev/ttyACM0 --baud 921600 \
  --before default-reset --after hard-reset write-flash \
  --flash-mode dio --flash-freq 80m --flash-size 16MB \
  0x10000 nuttx/nuttx.bin

# 服务器（PC 侧；改动 server_bridge.py 后必须重启）
cd /home/vboxuser/openvela/apps/plant-companion/tools/server
python3 server_bridge.py 8000        # 依赖 numpy noisereduce；MQTT 可选(paho)

# 设备串口
minicom -D /dev/ttyACM0 -b 115200
plant wifi <ssid> <密码>
plant voice server 192.168.3.148     # 重启后必做（RAM 变量）
```

### 常用验证命令
```bash
plant mem                  # 内存四段健康报表（heap/lvgl/static/sd）
plant voice tone 1000      # 喇叭纯音（应干净，耗时≈50ticks/500ms）
plant voice rec 3          # 录音存 /mnt/sd/rec.wav
plant voice loop 3         # 录→回放
plant voice diag 3         # 安静/说话双段判别（安静峰值应 << 说话峰值）
plant voice play <wav>     # 播放任意 SD WAV（09-02 新增）
plant sd mount / status    # SD（已带预热自动重挂）
plant font status ...      # 字体链自检（09-03 新增，待烧录验证）
plant ota check/confirm    # OTA UI 化前的 NSH 途径
```
服务器音频调试辅助：每次 finalize 落盘 `tools/server/debug_audio/before_*.wav / after_*.wav`
（降噪前后原始音频，直接听/频谱分析对比）。

---

## 5. 关键约束与坑（红线，改动前必读）

1. **禁止改 `nuttx/arch/xtensa/src/esp32s3/esp-hal-3rdparty/`**（构建自动克隆）
2. **音频神圣基线（08-25 验证，勿回归）**：官方驱动 total_slot=2 / 2 槽 RX / `src[2i+slot]`
   提取 / ES7210 REG08=0x10 + REG12=0x00 + 30dB 增益。**4-slot TDM 全失败已回退**（板只 2 麦，
   2-slot 已充分利用，与 xiaozhi 数据等价——不要再试 TDM）
3. **LVGL 池 ≥80KB**（64KB 曾渲染 PANIC——注意：该结论现被证伪为 SD 字库 bug 巧合，
   待 UI 稳定后可重试 64KB 省内存实验）
4. **堆红线**：free≥16KB=[OK]，8-16KB=警戒，<8KB=危险（audio/SD/FAT 会失败）。
   不要动 WiFi/TCP 缓冲（曾致 DHCP 坏）；音频 apb 保持 ≤2044B
5. **OTA**：只写非活动槽；勿 write-flash 0x0；勿用 BCH 代理写 MTD（必须 find_mtddriver）
6. **存储分区 0x410000**（勿改回 0x250000——落在 ota_1 内）
7. 服务器地址不持久化（用户否决），勿按"持久化"实现
8. 系统无 PSRAM——任何 >40KB 设备端模型/大缓冲方案先查内存预算
   （详见 docs/MEMORY_BUDGET_WROOM.md）

---

## 6. 代码地图

| 路径 | 内容 |
|---|---|
| `apps/plant-companion/main/app_main.c` | NSH 命令分发（voice/sd/mem/font/ota…） |
| `apps/plant-companion/services/` | voice_service（说话状态机）/ server_bridge.c（HTTP 客户端）/ record_service 等 |
| `apps/plant-companion/components/voice_agent/` | es8311.c / es7210.c / voice_agent.c 码片驱动 |
| `apps/plant-companion/ui/` | screens/widgets/theme + assets/fonts/zh_font.c（字体链核心） |
| `apps/plant-companion/ai_module/` | ai_voice.c（录音/播放/降采样）/ ai_common.c（HTTP） |
| `apps/plant-companion/tools/server/server_bridge.py` | 服务器辅助手（攒 WAV/降噪/MiMo/TTS/MQTT） |
| `nuttx/arch/xtensa/src/esp32s3/hal_i2s.c` | I2S 封装（TX 流水线 / RX 双深预读 / 诊断 dump） |
| `nuttx/arch/xtensa/src/esp32s3/esp32s3_i2s.c` | NuttX 官方 I2S 驱动（勿乱改） |

---

## 7. 文档索引（深挖看这些）

| 文档 | 内容 |
|---|---|
| `docs/HANDOVER_20260831.md` | 阶段式提案 A-F、云同步定案细节 |
| `ai_module/VOICE_WORKLOG.md` + `ai_voice/VOICE_DEBUG.md` | 音频全历程（含 08-25 神圣基线、09-02 修复） |
| `components/voice_agent/MIC_RX_BUG_REPORT.md` | 历史音频 bug 档案 |
| `docs/MEMORY_BUDGET_WROOM.md` | 512KB SRAM 预算/调度策略 |
| `docs/UI_SPEC.md` / `ARCHITECTURE.md` / `IMAGE_REQUIREMENTS.md` / `VOICE_REQUIREMENTS.md` | 各模块规格 |
| `ota/OTA_DESIGN.md` / `OTA_WORKLOG.md` | OTA A/B 机制与坑 |

---

## 8. Git / 备份状态

- apps 仓库分支 `plant-companion-dev`（含 backup commit 137314fbb，09-02 状态）
- nuttx 仓库 detached @ 4af617f
- **gitee 备份（09-02 状态）**：`gitee.com/yang-asjdsad/plant-commut`
  分支 `apps-snapshot-20260902` / `nuttx-snapshot-20260902`（当前工作区含其后改动，未再推）
- ⚠️ 当前未提交改动：hal_i2s.c（RX 双深）、zh_font.c/字体 Makefile（emoji 链）、CLAUDE.md、日志文档
  ——验证稳定后应打新快照
