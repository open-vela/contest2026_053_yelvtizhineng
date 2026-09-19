# 图像识别链路（3C-2：拍照 → MiMo 真实诊断）需求与实现记录

## 1. 架构定案（遵循 CLAUDE_SYSTEM.md §12/§13）

用户拍板的架构（薄客户端 + 服务器 AI 中枢）：

```
设备（ESP32-S3-WROOM-1，薄客户端）
  采集：OV3660 rxbuf = RGB565 160×120（38.4KB 静态 DRAM，零拷贝）
  传输：raw RGB565 直传服务器（HTTP POST，零编码、零新增内存）
  渲染：LCD 直写预览（RGB565，LVGL 绕过）+ 诊断页显示服务器返回文本
        │
        │ HTTP（明文局域网）
        ▼
服务器 AI 中枢（PC 端 Python）
  收 RGB565 → PIL 转 JPEG（quality 85）→ MiMo mimo-v2.5（image_url）
  → 返回诊断文本 → 设备诊断页上屏
```

关键取舍（数据说话）：
- **设备端不输出 JPEG**：OV3660 有内置 JPEG 编码器，但切 JPEG 模式需
  重构 DMA（vs_eof=0 数据驱动），§2.22-2.24 历史曾饿死堆（rxbuf 65536
  → heap 88.8KB 系统崩）。raw RGB565 直传 = 零新增内存 + 零驱动风险。
- **传输量不是瓶颈**：语音 TTS 音频 800KB 都传过，51KB(base64) vs
  30KB(JPEG) 对局域网是零头；且 MiMo 实测 64×64 纯色块都能理解，
  160×120 植物照片足够识别健康状态/明显病害。
- **服务器转换已验证**：RGB565(LE, swap=ON) → PIL → JPEG → MiMo，
  3/3 次返回非空诊断文本（偶发空是 MiMo 瞬时抖动，非系统性问题）。

## 2. 协议

```
POST /image/analyze
  body = RGB565 160×120 原始帧（38400B，小端 LE，设备 swap=ON 后格式）
  响应 = {"ok": true, "text": "<MiMo 诊断文本>"}
```

## 3. 代码改动

### 服务器端（tools/server/server_bridge.py）
- `rgb565_to_jpeg()`：RGB565(LE) → PIL RGB → JPEG（quality 85）
- `mimo_image_understand()`：JPEG base64 → MiMo image_url → 文本
- `do_POST /image/analyze`：收帧 → 转 JPEG → MiMo → 回 {ok,text}
- 实测：RGB565 38400B → JPEG ~1.5-3KB → MiMo 诊断 ✅

### 设备端
- `services/server_bridge.c`：新增 `server_bridge_image_analyze()`：
  POST /image/analyze → 解析 text（复用 sb_http_post + sb_json_get_string）；
  超时 60s（服务器端 MiMo 视觉 10-30s，比语音 180s 更快失败反馈）
- `services/ai_service.c`：ai_worker 优先服务器中转（有帧时），
  成功填 `result.server_text`；失败降级本地 ai_engine（离线演示）；
  worker 栈加大到 8KB（同 voice_worker 经验）；
  **零拷贝借用模式**（borrowed=true：不 malloc 帧，调用方保证存活）
- `services/ai_service.h`：result 加 `server_text[1024]`；
  `ai_service_analyze_async` 加 `borrowed` 参数
- `ui/screens/screen_camera.c`：
  - `cam_on_shoot`：冻结预览 → **零拷贝**取 rxbuf 帧（freeze 后预览
    线程不再 capture，rxbuf 静态 DRAM 定格）→ borrowed 提交异步诊断
  - `cam_ai_done`（worker 线程）：**只写共享缓冲 s_ai_result +
    s_ai_pending，不碰 LVGL**（照抄 voice 页成熟模式，消除跨线程
    LVGL 竞态 → 花屏/卡死风险）
  - `frame_timer_cb`（ui_task 500ms）：消费 pending → 按换行拆
    "问题描述/建议" → 诊断页上屏
  - `cam_on_delete`：清 pending + 删定时器 + 置空（UAF 防护，
    voice 页 PANIC 教训）
- `components/camera_capture/camera_capture.h`：暴露帧尺寸宏
  CAM_PREVIEW_W/H/SIZE（与 .c 的 CAM_FRAME_* 一致防漂移）
- `Makefile`：server_bridge.c 从 AI_VOICE 移到 AI_MODULE（图像识别
  也依赖它，不再只属于语音）

## 4. 验证（实机）

1. `plant voice server <ip>`（配置服务器地址，同语音）
2. UI 进拍照页（预览正常 = RGB565 直显）
3. 点「拍照」→ 状态"识别中…" → 服务器日志：
   `[IMAGE] 收到 RGB565 38400B，转 JPEG 并请求 MiMo...`
   `[IMAGE] 回复文本: <非空中文诊断>`
4. 设备自动进诊断页显示真实 MiMo 结果（非演示"绿萝"数据）

失败排查：
- 服务器无日志 → 设备没配置 server / WiFi 断
- `[IMAGE] ... 400 frame too small` → 帧不是 38400B
- 诊断页显示"绿萝"演示数据 → server_bridge_image_analyze 失败
  （设备日志 `[Bridge] image analyze HTTP failed: -X`），检查
  服务器是否在跑、地址端口对不对
- 空文本偶发 → MiMo 瞬时抖动，重拍一次

## 5. 遗留

- MiMo 对抽象色块图（非真实植物）会提示"无法判断"——实机拍真实
  植物才有诊断价值
- 诊断页显示：MiMo 文本按第一个 \n 拆"问题描述/建议"，无换行时
  全部进描述、建议用默认文案
- 未来可加：拍完照片存 SD（/mnt/sd/photo.rgb 已有落盘逻辑）

## 6. 修订记录（2026-08-31 晚五：跨线程/资源/超时三项硬伤修复）

用户质询后逐项核查修复：

| 问题 | 风险 | 修复 |
|---|---|---|
| cam_ai_done 在 worker 线程直接操作 LVGL | 跨线程竞态 → 花屏/卡死 | 只写共享缓冲，ui_task 定时器上屏（照抄 voice 页模式）|
| malloc(38400) 拷贝帧 | 无 PSRAM 堆仅 ~17KB → 必失败 | 零拷贝借用 rxbuf（freeze 后稳定，静态 DRAM 不清零）|
| 图像识别用 180s 超时 | 服务器不可达时"识别中"挂 3 分钟 | 收紧到 60s |
| 页面销毁时 timer 未清 | use-after-free（voice 页 PANIC 教训）| cam_on_delete 清 pending + 删 timer + 置空 |
| 本地 ai_engine 兜底 malloc 大块 | 堆不足 | 有 NULL 保护 fallback mock，安全 |

稳定性实测（PC 端 5 连测）：RGB565→JPEG→MiMo **5/5 返回非空诊断**，
内容合理（绿植+黄叶均被识别）。偶发空文本 = MiMo 瞬时抖动，非系统问题。

## 7. 修订记录（2026-08-31 晚六：拍照后杀摄像头进程，用户要求）

用户质询："诊断结果还要存在，你不应该把摄像头进程去掉/杀掉吗"

**改动**（ui/screens/screen_camera.c）：
- `cam_on_shoot`：拍照 = 定格帧 + **立即杀摄像头进程**
  - 顺序：take()（freeze，让预览线程退出 capture 进入空转）→
    取帧 → 提交诊断（borrowed 零拷贝）→ camera_preview_stop()
    （join 立即返回，freeze 后线程只 usleep 20ms）→ s_cam_stopped=true
  - rxbuf 静态 DRAM stop 不清零 → worker 借用读取安全
- `frame_timer_cb`：页面重新可见时——若 s_cam_stopped（拍照杀过）→
  camera_preview_start 重启预览；否则 resume（未拍照返回场景）
- `cam_on_delete` / `screen_camera_create`：重置 s_cam_stopped +
  清 s_ai_pending（防残留跳页：上次 worker 未完成时销毁页面，
  结果写入 static 缓冲但 timer 已删，下次进页面必须先清）

**验证点**（实机）：
- 拍照 → "识别中…" → 诊断页出现（进程已杀，无 10s join 等待）
- 诊断页返回 → 拍照页预览自动重启（camera_preview_start）
- 拍照后立即返回主页 → 再进拍照页 → 不自动跳诊断页（pending 已清）

## 8. 修订记录（2026-08-31 晚七：诊断页残留摄像头画面，用户反馈）

现象：拍照进诊断页后，取景框区域仍显示摄像头拍摄画面（蓝色模糊植物图），
把诊断结果挡住。

根因：取景框区域是摄像头「直写 LCD」（lcd_put_rgb565 → ioctl PUTAREA，
绕过 LVGL）画的。LVGL 不认为该区域脏 → 诊断页 push（叠加子对象，
非 lv_screen_load）后，LVGL 不重绘取景框区 → 残留。

修复（ui/screens/screen_camera.c frame_timer_cb）：创建诊断页并 push 后，
`lv_obj_invalidate(diag)` + `lv_obj_invalidate(lv_screen_active())` 强制
LVGL 下一帧（30fps 主循环）全量重绘诊断页 —— 其全屏背景
（bg_opa=COVER）物理覆盖取景框残留。
注：未用 lcd_put_rgb565 填色清屏（需 76KB 填充缓冲，无 PSRAM 挤爆
.bss）；LVGL 全屏重绘是零额外内存的正解。

验证：拍照 → 诊断页背景干净（浅蓝渐变），无摄像头残留；返回拍照页
预览正常重启。

## 9. 修订记录（2026-08-31 晚八：诊断页残留——用户指正"ioctl 直写与 LVGL 非同一逻辑"）

用户关键指正：摄像头画面走 **ioctl 直写 LCD**（LCDDEVIO_PUTAREA，绕过
LVGL 渲染管线），与 LVGL 是**两套逻辑**——LVGL 不认为取景框区域脏，
靠 `lv_obj_invalidate` 让 LVGL 重绘**不可靠**（依赖 LVGL 对隐藏页/叠加
子对象的重绘行为，且 HIDDEN 对象的 invalidate 会被 LVGL 忽略）。

**正解（物理填充）**：与摄像头**同通道**覆盖——拍照 worker 上传完成后
（frame_timer_cb 消费 pending 时），把 rxbuf 填成诊断页背景色
（0xeff6ff → RGB565 LE），再 `lcd_put_rgb565` 直写取景框区域。
- ioctl 物理写屏，100% 必效（不依赖 LVGL 重绘时序/优化）
- 复用 rxbuf（此时 worker 已结束，不再借用）→ 零额外内存
- lcd_put_rgb565 逐行分块 ≤320px，不触发 st7789 rowbuff_be 越界
- 保留 lv_obj_invalidate 双保险（覆盖取景框外的残留）

时序安全：返回拍照页 → camera_preview_start 重启预览 → DMA 重写 rxbuf
为新帧；下次拍照上传新帧，无背景色残留污染。

验证：拍照 → 诊断页背景干净（浅蓝渐变），无摄像头残留；返回拍照页
预览正常；再次拍照上传的是新帧（非背景色）。

## 10. 修订记录（2026-08-31 晚九：填充未生效 → 重排顺序 + 加诊断打印）

用户实测：填充后诊断页仍残留摄像头画面（"显示屏还在"）。
修正：
1. **重排顺序**：物理填充移到 screen_diagnose_create 之前（原在 push
   之后，可能被 LVGL 重绘覆盖或时序错乱）
2. **加 3 处诊断打印**（下次实测可定位断点）：
   - `[Cam] 诊断结果到达，准备上屏 (len=N)` — pending 消费
   - `[Cam] 取景框已物理填充背景色` — lcd_put_rgb565 填充执行
   - `[Cam] 填充失败: rxbuf 不可用` — rxbuf 空
3. 确认流程：worker 完成 → cam_ai_done 写 pending → frame_timer_cb
   消费 → 物理填充取景框 → 创建诊断页 → push → invalidate 双保险

验证判断：
- 若填充打印出现且诊断页干净 → 修复生效
- 若填充打印出现但残留仍在 → lcd_put_rgb565 的 ioctl 问题（需查驱动）
- 若填充未打印 → frame_timer_cb 未调度/pending 未消费（查 timer）
