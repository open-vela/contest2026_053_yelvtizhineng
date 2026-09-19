#!/usr/bin/env python3
"""
植小伴 · 服务器辅助手（AI 中枢薄版）

职责（用户定案）：只做"收发 + 转格式"的辅助手，不做重系统。
  - 设备分段上传 PCM 录音块 → 服务器攒成 WAV
  - finalize：WAV → MiMo 音频理解（mimo-v2.5）→ 回复文本
  - 回复文本 → MiMo TTS（mimo-v2.5-tts）→ 音频
  - 返回 {text, has_audio} 给设备（文本上屏 + 音频单独端点拉取）
  - /data 数据中转（阶段 C1）：手机/设备 POST 任务/日记/传感器 JSON，
    服务器落盘本地 + GET 供设备按日期拉取（设备端显示 = 阶段 C3）
  - MQTT 代理上云（阶段 E，用户定案：设备零 MQTT 依赖）：服务器把
    诊断结果与 /data 收到的记录用 paho-mqtt 发布到云 broker
    （plant/<device_id>/<kind>）；broker 不可用时进本地队列，
    断线恢复后自动补发（E5），HTTP 全功能不受影响。

端点：
  POST /voice/upload?session=<id>&seq=<n>    body=原始 PCM16 块（16kHz mono）
  POST /voice/finalize?session=<id>          触发：攒WAV→MiMo理解→TTS→返回JSON
  GET  /voice/audio?session=<id>             拉取 TTS 音频（WAV）
  POST /image/analyze                        body=RGB565 160×120 帧 → 诊断文本
  GET  /time                                 北京时间 {unix, date, time}（UTC+8）
  POST /data/<task|diary|sensor|diagnose>    body=记录 JSON → 落盘 + MQTT 发布
  GET  /data/<kind>?date=YYYY-MM-DD&limit=N  拉记录（date 缺省=最近 N 条倒序）
  GET  /data                                 各 kind 落盘计数
  GET  /health                               健康检查（含 mqtt/data 状态）

运行：python3 server_bridge.py [port]   （默认 8000，监听 0.0.0.0）
依赖：pip install requests            （MQTT 可选：pip install paho-mqtt）
MQTT/数据配置见代码内"阶段 C1/E"注释段（环境变量 PLANT_MQTT_HOST 等，
全可缺省——无 paho 库或无 broker 时服务器照常跑，仅云发布降级为队列）。
"""

import base64
import io
import json
import os
import random
import struct
import sys
import tempfile
import threading
import time
from collections import deque
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import urlparse, parse_qs

# ── 配置 ──
API_KEY     = "<在此填入你的 MiMo API Key>"
MIMO_URL    = "https://token-plan-cn.xiaomimimo.com/v1/chat/completions"
MIMO_MODEL  = "mimo-v2.5"       # 音频理解（已实测转写）
TTS_MODEL   = "mimo-v2.5-tts"   # 语音合成（已实测返回 WAV）
TTS_VOICE   = "mimo_default"
RATE        = 16000             # 设备 PCM 采样率（16kHz mono 16bit）

# ── P0-2 服务器侧降噪（2026-09-01 用户拍板：降噪责任上移服务器）──
# 背景：设备端"手动滤波链"（11点中值+软噪声门）把清辅音当噪声抹、词尾当
# 静音削——人声损伤大于降噪收益（用户原话：手动降噪最垃圾）。改为设备裸传
# PCM，服务器负责：noisereduce 频谱门控（稳态噪声）→ 峰值归一化(-3dBFS)。
# 依赖：pip install numpy noisereduce（缺库自动跳过降噪，仅归一化不生效）
NOISE_REDUCE_ENABLE = True
NOISE_REDUCE_DECREASE = 0.7     # 降噪强度 0-1（保守 0.3，防削人声）
NOISE_REDUCE_GAIN_MAX = 20.0    # 归一化最大增益 dB（防纯噪声被放大）

# ⚠️ 测试辅助（2026-09-01，锁定"降噪 vs AI"问题用，验证完删除）：
# finalize 时把降噪前后 PCM 落盘 WAV → tools/server/debug_audio/ 下
#   before_<ts>.wav（设备上传的原始音频）/ after_<ts>.wav（降噪归一化后）
# 听对比：before 清晰 → 问题在 AI/prompt；before 也糊 → 问题在设备滤波链；
#          after 有伪影/削声 → 降噪参数问题。
DEBUG_DUMP_AUDIO = True
_DEBUG_AUDIO_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                "debug_audio")

# ── 阶段 C1：/data 数据中转（2026-09-03 实现）──
# 设备/手机 POST 任务/日记/传感器记录 → 服务器落盘 JSON 文件，设备进页
# GET 按日期拉取（阶段 C3 设备端显示是后续活，本文件只管收/存/发）。
# 文件按 kind+date 组织：data_store/<kind>/<date>.json = {"date":…,"items":[…]}
# 追加式读改写（记录量小；_data_lock 串行化防 ThreadingHTTPServer 并发写坏）。
# 日期统一北京时间（UTC+8，与 /time 同源）——设备无 RTC，靠 /time 得知
# "今天"，服务器侧同源才不会筛错天（HANDOVER §B 为此预留了 date 字段）。
KINDS = ("task", "diary", "sensor", "diagnose")
_DATA_DIR = os.environ.get(
    "PLANT_DATA_DIR",
    os.path.join(os.path.dirname(os.path.abspath(__file__)), "data_store"))
_data_lock = threading.Lock()

# ── 阶段 E：MQTT 云同步（服务器代理上云，2026-09-03 实现）──
# 架构（用户拍板）：设备 → 已有 HTTP 通道 → 本服务器 → paho-mqtt → 云
# broker。设备零 MQTT 依赖（无 PSRAM 下 NuttX mqttc 栈 ~10KB+ 太紧，
# HANDOVER §E 理由）。本文件保证：
#   1) 缺 paho 库 / broker 不通 → 发布进本地队列（E5 离线缓冲），HTTP
#      全功能不受影响；恢复后按序补发（先连上 broker 再排空队列）。
#   2) 队列有界（丢最旧保最新，防传感器洪峰撑爆内存）。
#   3) 断线由 paho loop_start 自动重连（connect_async + reconnect_delay_set）。
# 配置（环境变量优先，全可缺省）：
#   PLANT_MQTT_HOST / PLANT_MQTT_PORT / PLANT_MQTT_USER / PLANT_MQTT_PASS
#   PLANT_DEVICE_ID  设备标识 → 主题 plant/<device_id>/<kind>
#   （本地测试可用 mosquitto：sudo apt install mosquitto 后默认 1883）
MQTT_TOPIC_PREFIX = "plant"
MQTT_HOST = os.environ.get("PLANT_MQTT_HOST", "127.0.0.1")
MQTT_PORT = int(os.environ.get("PLANT_MQTT_PORT", "1883"))
MQTT_USER = os.environ.get("PLANT_MQTT_USER", "")
MQTT_PASS = os.environ.get("PLANT_MQTT_PASS", "")
MQTT_DEVICE_ID = os.environ.get("PLANT_DEVICE_ID", "esp32s3-box-1")
MQTT_QUEUE_MAX = 5000          # 离线队列上限（条）
MQTT_RETRY_SEC = 5             # broker 不通时的重试间隔

_mqtt_lock = threading.Lock()
_mqtt = {
    "client": None,            # paho Client（None = 未初始化/不可用）
    "connected": False,        # on_connect 置位 / on_disconnect 清位
    "outq": deque(),           # 发布队列（含离线缓冲）
    "dropped": 0,              # 队列满丢弃计数
    "disabled": False,         # 缺 paho 库 → 永久停用（只告警一次）
    "warned_at": 0.0,          # broker 连不上告警节流时间戳
}

# 会话存储：session_id -> {"seq": 已收块数, "buf": bytearray, "last": 最后活跃}
_sessions = {}
_lock = threading.Lock()


def get_session(sid):
    with _lock:
        s = _sessions.get(sid)
        if s is None:
            s = {"buf": bytearray(), "seq": -1, "last": time.time()}
            _sessions[sid] = s
        s["last"] = time.time()
        return s


def cleanup_old_sessions(max_age=600):
    now = time.time()
    with _lock:
        for sid in list(_sessions):
            if now - _sessions[sid]["last"] > max_age:
                del _sessions[sid]


def pcm_to_wav(pcm):
    """PCM16 mono → WAV（加 RIFF 头）"""
    n = len(pcm)
    hdr = b"RIFF" + struct.pack("<I", 36 + n) + b"WAVE"
    hdr += b"fmt " + struct.pack("<IHHIIHH", 16, 1, 1, RATE, RATE * 2, 2, 16)
    hdr += b"data" + struct.pack("<I", n)
    return hdr + bytes(pcm)


def pcm_denoise(pcm_bytes):
    """PCM16 mono → [noisereduce 频谱门控 + 峰值归一化] → PCM16。

    返回 (out_bytes, stats)：
      - 缺 numpy/noisereduce → 原样返回, stats=None（降噪失效，链路不破）
      - stats = {before/after peak/rms, gain_db}，供日志对比降噪效果
    取舍（2026-09-01）：
      - stationary=True：整段估计稳态噪声谱（风扇/空调），对语音损伤小；
      - prop_decrease=0.3 保守（1.0 会残留"水声"伪影并削语音细节）；
      - n_fft=512 短窗：保语音瞬态（辅音）；
      - 归一化限增益 20dB：纯噪声段（人声很弱）不会把底噪放大成"嘶声"。
    """
    if not NOISE_REDUCE_ENABLE:
        return pcm_bytes, None

    try:
        import numpy as np
        import noisereduce as nr
    except ImportError as e:
        # ⚠️ 2026-09-02：缺库必须显式警告（此前静默跳过 → 用户以为降噪
        # 生效，实际裸底噪直接喂 MiMo → "录音全是噪声"假象）。
        print(f"[DENOISE] ⚠️ 降噪不可用（{e}）——录音将裸传 MiMo。"
              f"安装：pip install numpy noisereduce")
        return pcm_bytes, None

    n = len(pcm_bytes) // 2
    if n < RATE:
        # ⚠️ 2026-09-02：录音 <1s 噪声谱估计不足跳过降噪——显式提示，
        # 否则用户会以为降噪失效（实际是太短）。
        print(f"[DENOISE] ⚠️ 录音仅 {n/16000:.1f}s <1s，跳过降噪（裸传 MiMo）")
        return pcm_bytes, None

    pcm = np.frombuffer(pcm_bytes, dtype=np.int16).astype(np.float32) / 32768.0
    before_peak = float(np.max(np.abs(pcm)))
    before_rms = float(np.sqrt(np.mean(pcm * pcm)))

    reduced = nr.reduce_noise(
        y=pcm,
        sr=RATE,
        stationary=True,
        prop_decrease=NOISE_REDUCE_DECREASE,
        n_fft=512,
    )

    # 峰值归一化到 -3dBFS（0.707），增益上限 20dB
    peak = float(np.max(np.abs(reduced))) if len(reduced) else 0.0
    gain = 0.7071 / peak if peak > 1e-6 else 1.0
    gmax = 10.0 ** (NOISE_REDUCE_GAIN_MAX / 20.0)
    if gain > gmax:
        gain = gmax

    out = np.clip(reduced * gain, -1.0, 1.0)
    out16 = (out * 32767.0).astype(np.int16)

    stats = {
        "before_peak": before_peak,
        "before_rms": before_rms,
        "after_peak": float(np.max(np.abs(out))),
        "after_rms": float(np.sqrt(np.mean(out * out))),
        "gain_db": 20.0 * __import__("math").log10(gain) if gain > 0 else -99.0,
    }
    return out16.tobytes(), stats


def mimo_audio_understand(wav_bytes, prompt=None):
    """WAV → MiMo 音频理解 → 回复文本"""
    import requests
    if prompt is None:
        # ⚠️ 2026-09-01 prompt 重构（防脑补）：
        # 旧 prompt 强诱导"以植物管家的身份"→ 模型听不清时往植物养护话题
        # 脑补（用户实测："小绿你好我想和你说话" 被转写成 "我想让我的仙人掌
        # 长得更好"——"仙人掌"就是脑补产物）。新 prompt：
        #   1) 先**准确转写**，听不清就说"没听清"不要猜测；
        #   2) 回答身份仍可带植物管家（这是产品定位），但只针对转写内容；
        #   3) 输出不要"语音转写/回答"标记（旧格式混乱）。
        # 同时要求简短：TTS 音频按文本长度生成，回答太长 → 音频几百 KB~MB，
        # 生成慢 → 设备 finalize 超时断开（实测 1.9MB/59s 音频 BrokenPipe）
        prompt = ("请先**准确转写**用户刚才说的话（尽量逐字、不要改写）；"
                  "如果听不清，请直接说'没听清，请再说一遍'，不要猜测或编造。"
                  "然后以植物管家的身份，针对转写内容用中文简短回答"
                  "（2-3 句话，适合语音播报，不要列点、不要寒暄）。"
                  "直接输出转写内容和回答，不要加'语音转写''回答'之类的标记。")
    b64 = base64.b64encode(wav_bytes).decode("ascii")
    payload = {
        "model": MIMO_MODEL,
        "messages": [{
            "role": "user",
            "content": [
                {"type": "input_audio",
                 "input_audio": {"data": f"data:audio/wav;base64,{b64}"}},
                {"type": "text", "text": prompt}
            ]
        }],
        "max_completion_tokens": 1024  # ⚠️ 2026-08-31 修复：300 偶发空文本
        # （reasoning_tokens 吃掉全部预算 → content 为 0 → 回复文本空）
    }
    headers = {"Content-Type": "application/json",
               "api-key": API_KEY,
               "Authorization": f"Bearer {API_KEY}"}
    r = requests.post(MIMO_URL, headers=headers, json=payload, timeout=90)
    j = r.json()
    return j["choices"][0]["message"].get("content", "")


def mimo_tts(text):
    """回复文本 → MiMo TTS → WAV bytes"""
    import requests
    payload = {
        "model": TTS_MODEL,
        "messages": [
            {"role": "user", "content": "请用温柔亲切的植物管家语气朗读"},
            {"role": "assistant", "content": text}
        ],
        "audio": {"voice": TTS_VOICE},
        "max_completion_tokens": 1024
    }
    headers = {"Content-Type": "application/json",
               "api-key": API_KEY,
               "Authorization": f"Bearer {API_KEY}"}
    r = requests.post(MIMO_URL, headers=headers, json=payload, timeout=90)
    j = r.json()
    audio_b64 = (j["choices"][0].get("message") or {}).get("audio", {}).get("data", "")
    return base64.b64decode(audio_b64) if audio_b64 else None


# ═══ 图像识别（3C-2：设备拍照 → 服务器中转 → MiMo 视觉） ═══
# 架构同语音：设备端无 TLS，直连 MiMo(HTTPS 443) 走不通 → 设备把原始
# RGB565 帧传给局域网服务器（明文 HTTP），服务器转 JPEG 后调 MiMo。
# 2026-08-31 实测（PC 端直调）：
#   - mimo-v2.5 + image_url + data:image/jpeg;base64, ✅（image_tokens 计入）
#   - RGB565(LE 小端, swap=ON) 160×120 → PIL 转 JPEG → MiMo 识别 ✅
# 方案取舍（无 PSRAM + DRAM 121KB 硬约束）：
#   - 传 RGB565：设备零新增内存（rxbuf 已静态 38400B），零驱动改动
#     （JPEG 输出需重构 DMA vs_eof=0，§2.24 曾饿死堆，风险高）
#   - 分辨率 160×120：MiMo 实测 64×64 纯色块都能理解，足够识别
#     健康状态/明显病害；320×240 JPEG 的收益 vs 驱动风险不成比例

IMAGE_W = 160
IMAGE_H = 120
IMAGE_PROMPT = ("请仔细观察这张植物图片，以植物管家的身份用中文简短回答"
                "（2-3 句话，适合语音播报）：1) 植物整体健康状态；"
                "2) 如有病害/虫害/黄叶/枯叶请指出；3) 给出养护建议。"
                "不要列点、不要寒暄。")


def rgb565_to_jpeg(rgb565_bytes, width=IMAGE_W, height=IMAGE_H):
    """RGB565（小端 LE，设备 swap=ON 后）→ JPEG bytes。
    纯 Python 实现（设备帧 38400B，无需 PIL 也能跑，但 PIL 更快更稳）。"""
    try:
        from PIL import Image
    except ImportError:
        return None

    n = width * height
    if len(rgb565_bytes) < n * 2:
        return None
    rgb = bytearray(n * 3)
    for i in range(n):
        lo = rgb565_bytes[i * 2]
        hi = rgb565_bytes[i * 2 + 1]
        v = lo | (hi << 8)
        rgb[i * 3]     = ((v >> 11) & 0x1F) << 3
        rgb[i * 3 + 1] = ((v >> 5) & 0x3F) << 2
        rgb[i * 3 + 2] = (v & 0x1F) << 3
    img = Image.frombytes("RGB", (width, height), bytes(rgb))
    import io
    buf = io.BytesIO()
    img.save(buf, format="JPEG", quality=85)
    return buf.getvalue()


def mimo_image_understand(jpeg_bytes):
    """JPEG → MiMo 视觉理解 → 回复文本"""
    import requests
    b64 = base64.b64encode(jpeg_bytes).decode("ascii")
    payload = {
        "model": MIMO_MODEL,
        "messages": [{
            "role": "user",
            "content": [
                {"type": "text", "text": IMAGE_PROMPT},
                {"type": "image_url",
                 "image_url": {"url": f"data:image/jpeg;base64,{b64}"}}
            ]
        }],
        "max_completion_tokens": 1024
    }
    headers = {"Content-Type": "application/json",
               "api-key": API_KEY,
               "Authorization": f"Bearer {API_KEY}"}
    r = requests.post(MIMO_URL, headers=headers, json=payload, timeout=90)
    j = r.json()
    return j["choices"][0]["message"].get("content", "")


# ═══ 阶段 C1+E：/data 数据中转 + MQTT 代理上云（2026-09-03） ═══


def bj_now():
    """北京时间（UTC+8 手工偏移）→ (unix_beijing, "YYYY-MM-DD", "HH:MM")。

    /time 与本文件所有日期同源；设备无 RTC 电池，靠这里校准"今天"
    （HANDOVER §B 根因：每次上电都必须网络校准）。"""
    t = time.time() + 8 * 3600
    g = time.gmtime(t)
    return int(t), time.strftime("%Y-%m-%d", g), time.strftime("%H:%M", g)


def data_counts():
    """各 kind 落盘记录数（/health、GET /data 用）。"""
    counts = {}
    with _data_lock:
        for kind in KINDS:
            d = os.path.join(_DATA_DIR, kind)
            n = 0
            if os.path.isdir(d):
                for fn in os.listdir(d):
                    if fn.endswith(".json"):
                        try:
                            with open(os.path.join(d, fn), "r",
                                      encoding="utf-8") as f:
                                n += len(json.load(f).get("items", []))
                        except Exception:
                            pass
            counts[kind] = n
    return counts


def data_append(kind, record):
    """落盘一条记录（kind 白名单内；record 须 dict）。

    自动补 ts/date（北京时间，缺省才补——上传方带了就尊重，如手机 App
    补写昨天日记）、_id（<date>#<当天序号>）。返回补全后的记录。
    阶段 C 存储格式定案：JSON（字段含日期，客户端与固件都能读写、加字段
    不破坏兼容——对比 record_service 定长结构体 STR_LEN 48 的局限）。"""
    if kind not in KINDS:
        raise ValueError(f"bad kind: {kind}")
    if not isinstance(record, dict):
        raise ValueError("record must be a JSON object")

    now_unix, today, _ = bj_now()
    record.setdefault("ts", now_unix)
    record.setdefault("date", today)

    date = record["date"]
    if not isinstance(date, str) or len(date) != 10:
        raise ValueError(f"bad date: {date!r}")

    with _data_lock:
        d = os.path.join(_DATA_DIR, kind)
        os.makedirs(d, exist_ok=True)
        path = os.path.join(d, f"{date}.json")
        items = []
        if os.path.isfile(path):
            try:
                with open(path, "r", encoding="utf-8") as f:
                    items = json.load(f).get("items", [])
            except Exception:
                items = []  # 文件损坏 → 从空重建（单条记录损坏不拖垮整类）
        record["_id"] = f"{date}#{len(items) + 1}"
        items.append(record)
        with open(path, "w", encoding="utf-8") as f:
            json.dump({"date": date, "items": items}, f,
                      ensure_ascii=False, indent=1)
    return record


def data_query(kind, date=None, limit=100):
    """拉记录：date 给定 → 那天全部；否则跨文件合并倒序（新→旧）取 limit 条。

    文件名即日期（YYYY-MM-DD.json，字典序=时间序），倒序遍历即新日期在前；
    同日内的 items 为插入顺序（追加式，符合"日记按写的时间先后"）。"""
    if kind not in KINDS:
        raise ValueError(f"bad kind: {kind}")
    d = os.path.join(_DATA_DIR, kind)
    if not os.path.isdir(d):
        return []
    with _data_lock:
        if date:
            if not (len(date) == 10 and date.replace("-", "").isdigit()):
                raise ValueError(f"bad date: {date!r}")
            path = os.path.join(d, f"{date}.json")
            if not os.path.isfile(path):
                return []
            try:
                with open(path, "r", encoding="utf-8") as f:
                    return json.load(f).get("items", [])
            except Exception:
                return []
        files = sorted((fn for fn in os.listdir(d) if fn.endswith(".json")),
                       reverse=True)
        out = []
        for fn in files:
            try:
                with open(os.path.join(d, fn), "r", encoding="utf-8") as f:
                    items = json.load(f).get("items", [])
            except Exception:
                continue
            out.extend(reversed(items))
            if len(out) >= limit:
                break
        return out[:limit]


def mqtt_topic(kind):
    """主题：plant/<device_id>/<kind>（HANDOVER §E 主题设计建议）"""
    return f"{MQTT_TOPIC_PREFIX}/{MQTT_DEVICE_ID}/{kind}"


def mqtt_publish(kind, record):
    """入发布队列（先入队、后台线程排空）→ 天然 E5 离线缓冲。

    线程安全（deque + _mqtt_lock）；队列满丢最旧并计数。缺 paho 库时
    disabled 置位后本函数直接返回（不再进队——进程内已无消费方）。"""
    if kind not in KINDS:
        return
    with _mqtt_lock:
        if _mqtt["disabled"]:
            return
        q = _mqtt["outq"]
        if len(q) >= MQTT_QUEUE_MAX:
            q.popleft()
            _mqtt["dropped"] += 1
        q.append((mqtt_topic(kind),
                  json.dumps(record, ensure_ascii=False)))
        client = _mqtt["client"]
        connected = _mqtt["connected"]
    # 客户端已就绪且在线 → 唤醒排空线程（避免最多 0.5s 的延迟感）
    if client is not None and connected:
        _mqtt_wake.set()


def _mqtt_on_connect(client, userdata, flags, rc):
    with _mqtt_lock:
        _mqtt["connected"] = (rc == 0)
        n = len(_mqtt["outq"])
    if rc == 0:
        print(f"[MQTT] 已连接 {MQTT_HOST}:{MQTT_PORT}，"
              f"待补发 {n} 条"
              + ("（离线缓冲）" if n else ""))
        _mqtt_wake.set()          # 唤醒排空线程补发
    else:
        print(f"[MQTT] 连接被拒 rc={rc}（5=未授权/4=密码错/3=不可达），"
              f"数据继续进队列，将自动重试")


def _mqtt_on_disconnect(client, userdata, rc):
    with _mqtt_lock:
        was = _mqtt["connected"]
        _mqtt["connected"] = False
        qlen = len(_mqtt["outq"])
    if was:
        print(f"[MQTT] 断线 rc={rc}（0=主动断开）——进入离线队列模式，"
              f"队列现有 {qlen} 条，自动重连后补发")


def _mqtt_init():
    """初始化 paho 客户端（connect_async + loop_start → 自动重连）。

    返回 True=已初始化 / False=暂不可用（调用方稍后重试）。
    缺 paho 库 → disabled 置位，永久停用。"""
    try:
        import paho.mqtt.client as mqtt
    except ImportError:
        with _mqtt_lock:
            _mqtt["disabled"] = True
        print("[MQTT] ⚠️ 缺 paho-mqtt 库（pip install paho-mqtt）——"
              "云同步停用，数据只落 /data 本地；HTTP 全功能不受影响")
        return False

    # broker 不可用时 paho 内部循环会高频重连并在 stderr 打 ERROR 日志
    # （每次 TCP 失败一条）→ 控制台刷屏。静音 paho 自己的 logger，
    # 连接状态由本文件的 on_connect/on_disconnect/节流告警打印兜底。
    try:
        import logging
        logging.getLogger("paho.mqtt").setLevel(logging.CRITICAL)
    except Exception:
        pass

    cid = f"plant-server-{os.getpid()}-{random.randint(1000, 9999)}"
    try:
        if hasattr(mqtt, "CallbackAPIVersion"):   # paho ≥ 2.0
            client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION1,
                                 client_id=cid)
        else:                                     # paho 1.x
            client = mqtt.Client(client_id=cid)
        if MQTT_USER:
            client.username_pw_set(MQTT_USER, MQTT_PASS or None)
        client.reconnect_delay_set(min_delay=1, max_delay=60)
        client.on_connect = _mqtt_on_connect
        client.on_disconnect = _mqtt_on_disconnect
        client.connect_async(MQTT_HOST, MQTT_PORT, keepalive=30)
        client.loop_start()   # 网络循环在后台线程；断线自动重连
    except Exception as e:
        print(f"[MQTT] 初始化失败: {e}——将每 {MQTT_RETRY_SEC}s 重试")
        return False

    with _mqtt_lock:
        _mqtt["client"] = client
    print(f"[MQTT] 代理上云已启用：{MQTT_HOST}:{MQTT_PORT} → "
          f"{mqtt_topic('<kind>')}（设备零 MQTT 依赖，HANDOVER §E）")
    return True


# 排空线程唤醒事件（连接/入队时 set，避免 0.5s 轮询延迟感）
_mqtt_wake = threading.Event()


def _mqtt_drain():
    """把队列里已连接的发布排空（只在 connected 时发；失败放回队首）。"""
    while True:
        with _mqtt_lock:
            if not _mqtt["connected"]:
                return
            client = _mqtt["client"]
            if client is None or not _mqtt["outq"]:
                return
            topic, payload = _mqtt["outq"].popleft()
        try:
            info = client.publish(topic, payload, qos=0)
            if info.rc != 0:
                raise RuntimeError(f"publish rc={info.rc}")
        except Exception as e:
            with _mqtt_lock:
                _mqtt["outq"].appendleft((topic, payload))
            print(f"[MQTT] 发布失败（放回队首重试）: {e}")
            return


def _mqtt_worker():
    """后台线程：broker 不可用时每 MQTT_RETRY_SEC 尝试初始化；
    在线时（事件或 0.5s 兜底）排空队列。"""
    while True:
        with _mqtt_lock:
            client = _mqtt["client"]
            disabled = _mqtt["disabled"]
        if client is None and not disabled:
            if not _mqtt_init():
                with _mqtt_lock:
                    now = time.time()
                    if now - _mqtt["warned_at"] > 60:
                        _mqtt["warned_at"] = now
                        print(f"[MQTT] broker {MQTT_HOST}:{MQTT_PORT} 连不上"
                              f"——发布进离线队列（最多 {MQTT_QUEUE_MAX} 条），"
                              f"HTTP 正常；起 broker 后自动补发")
                _mqtt_wake.wait(timeout=MQTT_RETRY_SEC)
                continue
        _mqtt_drain()
        _mqtt_wake.wait(timeout=0.5)


def mqtt_start():
    """main() 调用：启动 MQTT 后台线程（daemon，Ctrl-C 随进程退出）。"""
    t = threading.Thread(target=_mqtt_worker, daemon=True,
                         name="mqtt-worker")
    t.start()


# ═══ HTTP Handler ═══


class Handler(BaseHTTPRequestHandler):
    def log_message(self, fmt, *args):
        sys.stdout.write("[%s] %s\n" % (time.strftime("%H:%M:%S"), fmt % args))

    def _send_json(self, obj, code=200):
        body = json.dumps(obj, ensure_ascii=False).encode("utf-8")
        self.send_response(code)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def _read_body(self):
        n = int(self.headers.get("Content-Length", 0))
        return self.rfile.read(n) if n > 0 else b""

    def do_GET(self):
        u = urlparse(self.path)
        q = parse_qs(u.query)
        sid = q.get("session", [""])[0]

        if u.path == "/health":
            # ⚠️ 2026-09-03：扩展健康检查——mqtt 状态（代理上云/E）+ data
            # 落盘计数（阶段 C1），方便 curl 一眼确认两端是否就绪。
            with _mqtt_lock:
                mq_ready = _mqtt["client"] is not None
                mq = {
                    "enabled": mq_ready and not _mqtt["disabled"],
                    "connected": _mqtt["connected"],
                    "queued": len(_mqtt["outq"]),
                    "dropped": _mqtt["dropped"],
                    "broker": (f"{MQTT_HOST}:{MQTT_PORT}" if mq_ready
                               else None),
                    "topic_prefix": mqtt_topic("<kind>"),
                }
            self._send_json({"ok": True, "sessions": len(_sessions),
                             "data": data_counts(), "mqtt": mq})
        elif u.path == "/time":
            # 顶栏实时时间（阶段 B）：返回北京时间（UTC+8，手工偏移，
            # 不引 pytz/tzdata 依赖）。设备无 RTC 电池，掉电时间丢，
            # 每次上电必须从服务器校准（HANDOVER §B 根因）。
            # unix = 北京时间 unix 秒（已偏移），设备据此**本地走时**：
            #   抓一次后 = unix + (tick差值)/TICK_PER_SEC 推算，每小时才校准一次，
            #   顶栏每分钟自然跳，不依赖轮询节奏（方案 F，2026-09-01）。
            # date 供阶段 C（任务/日记按日期筛选）使用。
            unix, date, tm = bj_now()
            self._send_json({"unix": unix, "date": date, "time": tm})
        elif u.path == "/voice/audio":
            # ⚠️ 2026-08-31 修复：设备端用 GET 拉取 TTS 音频
            # （sb_http_get_to_file → "GET /voice/audio"），但端点误写在
            # do_POST → 404 {"error":"not found"} → 设备把 JSON 当 WAV 存盘
            # → play_file 无 RIFF 头返回 -EINVAL → 喇叭无声。
            if not sid or sid not in _sessions:
                self._send_json({"error": "session not found"}, 404)
                return
            s = _sessions[sid]
            audio = s.get("audio")
            if not audio:
                self._send_json({"error": "no audio ready"}, 404)
                return
            self.send_response(200)
            self.send_header("Content-Type", "audio/wav")
            self.send_header("Content-Length", str(len(audio)))
            self.send_header("Connection", "close")
            self.end_headers()

            # ⚠️ 2026-08-31 增强：分块发送 + 进度日志。设备端下载中途
            # 断开时（BrokenPipe/ConnectionReset），直接看到"已发送 x/y B"：
            #  x≈0   → 设备端刚连上就断（fopen/早期 recv 失败）
            #  0<x<y → 设备端接收/写盘中途中断
            #  x==y  → 发送完成，问题在设备端播放侧
            total = len(audio)
            sent = 0
            try:
                for i in range(0, total, 16384):
                    self.wfile.write(audio[i:i + 16384])
                    sent = i + min(16384, total - i)
            except (BrokenPipeError, ConnectionResetError) as e:
                print(f"[AUDIO] 发送中断 {type(e).__name__}: "
                      f"已发送 {sent}/{total}B")
                raise
            print(f"[AUDIO] 发送完成 {total}B")
        elif u.path == "/data":
            # 阶段 C1：各 kind 落盘计数（总览）
            self._send_json({"ok": True, "counts": data_counts()})
        elif u.path.startswith("/data/"):
            # 阶段 C1：GET /data/<kind>?date=YYYY-MM-DD&limit=N
            # 设备进页拉取（C3 后续）：任务页 GET /data/task?date=今天；
            # 日记页 GET /data/diary（最近 N 条倒序，UI 自己排时间线）。
            kind = u.path[len("/data/"):]
            if kind not in KINDS:
                self._send_json({"error": "not found"}, 404)
                return
            date = q.get("date", [""])[0] or None
            try:
                limit = max(1, min(int(q.get("limit", ["100"])[0]), 1000))
            except ValueError:
                limit = 100
            try:
                items = data_query(kind, date=date, limit=limit)
            except ValueError as e:
                self._send_json({"error": str(e)}, 400)
                return
            self._send_json({"ok": True, "kind": kind, "date": date,
                             "count": len(items), "items": items})
        else:
            self._send_json({"error": "not found"}, 404)

    def do_POST(self):
        u = urlparse(self.path)
        q = parse_qs(u.query)
        sid = q.get("session", [""])[0]
        body = self._read_body()

        if u.path == "/voice/upload":
            if not sid or not body:
                self._send_json({"error": "missing session or body"}, 400)
                return
            s = get_session(sid)
            seq = int(q.get("seq", [str(s["seq"] + 1)])[0])

            # ⚠️ 2026-08-31 修复：设备重启后 g_session 从 0 重新计数，
            # 会复用服务器上残留的同名 session（seq 停在旧值、buf 混入
            # 旧数据）→ 旧 seq 全部 400 + finalize 把混合音频发给 MiMo
            # → 理解失败返回空文本。设备每次会话第一块必是 seq=0，
            # 以此作为「新会话」标记：强制重置（清 buf、seq 归 -1）。
            if seq == 0:
                if s["seq"] >= 0:
                    print(f"[UPLOAD] session={sid} 检测到新会话，重置旧状态 "
                          f"(旧 seq={s['seq']}, 旧 {len(s['buf'])}B)")
                s["buf"] = bytearray()
                s["seq"] = -1

            if seq != s["seq"] + 1:
                self._send_json({"error": f"seq mismatch: got {seq}, want {s['seq']+1}"}, 400)
                return
            s["buf"] += body
            s["seq"] = seq
            cleanup_old_sessions()
            self._send_json({"ok": True, "received": len(body), "total": len(s["buf"])})

        elif u.path == "/voice/finalize":
            if not sid or sid not in _sessions:
                self._send_json({"error": "session not found"}, 404)
                return
            s = _sessions[sid]
            if len(s["buf"]) < 100:
                self._send_json({"error": "audio too short", "bytes": len(s["buf"])}, 400)
                return

            # ⚠️ P0-2（2026-09-01）：降噪责任上移服务器——设备裸传 PCM，
            # 这里做 noisereduce 降噪 + 归一化后再喂 MiMo。日志打印
            # 处理前后 peak/rms/gain，可观测降噪效果（用户可对比）。
            raw_pcm = bytes(s["buf"])
            denoised, ds = pcm_denoise(raw_pcm)
            if ds is not None:
                print(f"[DENOISE] pcm {len(s['buf'])}B: "
                      f"peak {ds['before_peak']:.3f}→{ds['after_peak']:.3f} "
                      f"rms {ds['before_rms']:.4f}→{ds['after_rms']:.4f} "
                      f"gain {ds['gain_db']:.1f}dB")
                s["buf"] = bytearray(denoised)

            # 测试辅助：落盘降噪前后 WAV（听对比，验证后删）
            if DEBUG_DUMP_AUDIO:
                ts = time.strftime("%m%d_%H%M%S")
                try:
                    os.makedirs(_DEBUG_AUDIO_DIR, exist_ok=True)
                    with open(os.path.join(_DEBUG_AUDIO_DIR,
                                           f"before_{ts}.wav"), "wb") as f:
                        f.write(pcm_to_wav(raw_pcm))
                    with open(os.path.join(_DEBUG_AUDIO_DIR,
                                           f"after_{ts}.wav"), "wb") as f:
                        f.write(pcm_to_wav(bytes(s["buf"])))
                    print(f"[DEBUG] 音频已存 {_DEBUG_AUDIO_DIR}/"
                          f"before_{ts}.wav after_{ts}.wav")
                except Exception as e:
                    print(f"[DEBUG] 落盘失败: {e}")

            wav = pcm_to_wav(s["buf"])
            print(f"[FINALIZE] session={sid} pcm={len(s['buf'])}B wav={len(wav)}B")

            # 1. MiMo 音频理解 → 文本
            try:
                reply_text = mimo_audio_understand(wav)
            except Exception as e:
                print(f"[FINALIZE] 音频理解失败: {e}")
                self._send_json({"error": f"mimo understand failed: {e}"}, 500)
                return
            print(f"[FINALIZE] 回复文本: {reply_text[:200]}")

            # 2. 文本 → TTS → 音频（存 session，设备经 /voice/audio 拉取）
            has_audio = False
            try:
                tts_wav = mimo_tts(reply_text)
                if tts_wav:
                    s["audio"] = tts_wav
                    s["audio_ts"] = time.time()
                    has_audio = True
                    print(f"[FINALIZE] TTS 音频: {len(tts_wav)}B")
            except Exception as e:
                print(f"[FINALIZE] TTS 失败(仅回文本): {e}")

            # 3. 响应只回文本 + has_audio（音频单独端点拉取，设备不用大缓冲）
            self._send_json({
                "ok": True,
                "text": reply_text,
                "has_audio": has_audio
            })

        elif u.path == "/image/analyze":
            # 3C-2 图像识别：设备 POST RGB565 160×120 原始帧 → 服务器
            # 转 JPEG → MiMo 视觉 → 文本（设备无 TLS，走服务器中转）
            if not body:
                self._send_json({"error": "empty body"}, 400)
                return
            if len(body) < IMAGE_W * IMAGE_H * 2:
                self._send_json({"error": "frame too small",
                                 "bytes": len(body)}, 400)
                return
            print(f"[IMAGE] 收到 RGB565 {len(body)}B，转 JPEG 并请求 MiMo...")
            jpeg = rgb565_to_jpeg(body[:IMAGE_W * IMAGE_H * 2])
            if jpeg is None:
                self._send_json({"error": "PIL not installed or bad frame"},
                                500)
                return
            print(f"[IMAGE] JPEG {len(jpeg)}B，请求 MiMo...")
            try:
                text = mimo_image_understand(jpeg)
            except Exception as e:
                print(f"[IMAGE] MiMo 图像理解失败: {e}")
                self._send_json({"error": f"mimo image failed: {e}"}, 500)
                return
            print(f"[IMAGE] 回复文本: {text[:200]}")

            # E3：AI 诊断结果上云（服务器 paho 代理；同时落盘 /data 本地
            # 备份）。broker 不可用 → mqtt_publish 进离线队列，不影响识别。
            try:
                diag = {"text": text, "jpeg_bytes": len(jpeg)}
                stored = data_append("diagnose", diag)
                mqtt_publish("diagnose", stored)
                print(f"[IMAGE] 诊断已落盘 + 上云队列 "
                      f"({stored['_id']})")
            except Exception as e:
                print(f"[IMAGE] 诊断记录落盘/上云失败(不影响识别): {e}")

            self._send_json({"ok": True, "text": text})

        elif u.path.startswith("/data/"):
            # 阶段 C1：POST /data/<kind>，body = 记录 JSON（dict）。
            # 来源：手机 App 写日记/任务上传（C4，客户端侧后续做）、
            # 设备传感器周期上报（E2，未来设备加推送）、服务器定时任务
            # （C2，未来）。收下即：落盘本地 + MQTT 发布上云（E4/E2）。
            kind = u.path[len("/data/"):]
            if kind not in KINDS:
                self._send_json({"error": "not found"}, 404)
                return
            if not body:
                self._send_json({"error": "empty body"}, 400)
                return
            try:
                record = json.loads(body.decode("utf-8"))
            except Exception as e:
                self._send_json({"error": f"bad json: {e}"}, 400)
                return
            try:
                stored = data_append(kind, record)
            except ValueError as e:
                self._send_json({"error": str(e)}, 400)
                return
            mqtt_publish(kind, stored)
            print(f"[DATA] {kind} {stored['date']} "
                  f"#{stored['_id'].split('#')[-1]} → 落盘 + 上云队列")
            self._send_json({"ok": True, "record": stored})

        else:
            self._send_json({"error": "not found"}, 404)


def main():
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 8000

    # 阶段 E：MQTT 后台线程先启动——即使 broker 未起，发布也先进离线
    # 队列（E5），broker 一上线即自动重连补发，不会丢首轮数据。
    mqtt_start()

    srv = ThreadingHTTPServer(("0.0.0.0", port), Handler)
    print(f"植小伴服务器辅助手启动：http://0.0.0.0:{port}")
    print(f"  语音：POST /voice/upload?session=<id>&seq=<n>  (body=PCM16 块)")
    print(f"  语音：POST /voice/finalize?session=<id>        (→ 文本+TTS)")
    print(f"  图像：POST /image/analyze                     (RGB565 帧→诊断)")
    print(f"  时间：GET  /time                               (北京时间)")
    print(f"  数据：POST/GET /data/<task|diary|sensor|diagnose>（阶段 C1）")
    print(f"  健康：GET  /health                             (含 mqtt/data 状态)")
    print(f"  MQTT：{MQTT_HOST}:{MQTT_PORT} → "
          f"{mqtt_topic('<kind>')}（阶段 E；缺库/无 broker 自动降级为队列）")
    print(f"  落盘：{_DATA_DIR}")
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        print("\n停止")
        srv.shutdown()


if __name__ == "__main__":
    main()
