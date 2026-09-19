#!/usr/bin/env python3
"""
MiMo 音频接口发现实验（Phase 1.0，PC 端）

目的：确认 mimo-v2.5 的音频收发格式，决定服务器辅助手（Phase 6/1.1）如何转发：
  1. 音频输入：messages content 里 {"type":"input_audio",
     "input_audio":{"data":"<base64>","format":"wav"}} 是否被接受？
  2. 音频输出：回复里是否带 output_audio（base64 TTS）？
  3. 文本：是否随回复返回？

用法：python3 test_mimo_audio.py
需要：pip install requests numpy（numpy 只用于生成正弦波，可换成纯 python）
"""

import base64
import json
import math
import struct
import sys

API_KEY  = "<在此填入你的 MiMo API Key>"
API_URL  = "https://token-plan-cn.xiaomimimo.com/v1/chat/completions"
MODEL    = "mimo-v2.5"          # 全模态模型（-pro 无图片/音频全模态，勿用）

RATE     = 16000                # 16kHz
SECONDS  = 2                    # 2 秒测试音
PROMPT   = "这是一段2秒16kHz正弦波测试音频。请告诉我你听到了什么，并回复一句简短的植物问候。"

def make_wav():
    """生成 2s 16kHz mono 16bit 正弦波 WAV（440Hz + 880Hz 混合，能听出音调）"""
    n = RATE * SECONDS
    pcm = bytearray()
    for i in range(n):
        t = i / RATE
        # 440Hz + 880Hz 混合，音量 0.3，避免削波
        s = 0.3 * (math.sin(2 * math.pi * 440 * t) + 0.5 * math.sin(2 * math.pi * 880 * t))
        v = int(s * 32767)
        pcm += struct.pack("<h", v)

    data_len = len(pcm)
    header = b"RIFF" + struct.pack("<I", 36 + data_len) + b"WAVE"
    header += b"fmt " + struct.pack("<IHHIIHH", 16, 1, 1, RATE, RATE * 2, 2, 16)
    header += b"data" + struct.pack("<I", data_len)
    return header + bytes(pcm)

def send_audio():
    wav = make_wav()
    b64 = base64.b64encode(wav).decode("ascii")
    print(f"[INFO] 测试音频：{SECONDS}s 16kHz mono WAV ({len(wav)}B → base64 {len(b64)}B)")

    payload = {
        "model": MODEL,
        "messages": [{
            "role": "user",
            "content": [
                {"type": "text", "text": PROMPT},
                {"type": "input_audio",
                 "input_audio": {"data": b64, "format": "wav"}}
            ]
        }],
        "max_tokens": 300
    }

    headers = {"Content-Type": "application/json",
               "Authorization": f"Bearer {API_KEY}"}

    print(f"[INFO] POST {API_URL} (model={MODEL})")
    print(f"[INFO] Payload: {len(json.dumps(payload))} bytes")

    import requests
    try:
        resp = requests.post(API_URL, headers=headers, json=payload, timeout=60)
        status = resp.status_code
        body = resp.text
    except Exception as e:
        print(f"[RESULT] ❌ 网络异常：{e}")
        return

    print(f"[INFO] HTTP {status}")
    print(f"[RESPONSE] {body[:1500]}")

    # ── 判读 ──
    try:
        j = json.loads(body)
        if "choices" in j and j["choices"]:
            msg = j["choices"][0].get("message", {})
            text = msg.get("content", "") or ""
            print(f"\n[RESULT] ✅ 音频输入被接受（HTTP 200）")
            print(f"[RESULT] 回复文本（{len(text)} 字）：{text[:300]}")

            # 音频输出：OpenAI 格式是 message.output_audio.data
            oa = msg.get("output_audio", {})
            if oa and oa.get("data"):
                audio_b64 = oa["data"]
                fmt = oa.get("format", "wav")
                print(f"[RESULT] ✅ 回复带音频 output_audio（format={fmt}, base64 {len(audio_b64)}B）")
                # 存下来供服务器/人工试听
                with open("/tmp/mimo_reply_audio.bin", "wb") as f:
                    f.write(base64.b64decode(audio_b64))
                print(f"[RESULT] 已存 /tmp/mimo_reply_audio.bin（可试听）")
            else:
                print(f"[RESULT] ⚠️ 回复无 output_audio（可能只返回文本）")
        elif "error" in j:
            err = j["error"].get("message", "")
            print(f"\n[RESULT] ❌ MiMo 报错：{err[:300]}")
            if "input_audio" in err.lower() or "audio" in err.lower():
                print(f"[RESULT] 提示：input_audio 格式可能不被接受，需查 MiMo 音频 API 文档")
        else:
            print(f"\n[RESULT] ⚠️ 未知响应格式")
    except Exception as e:
        print(f"\n[RESULT] ⚠️ 响应解析失败：{e}")

if __name__ == "__main__":
    send_audio()
