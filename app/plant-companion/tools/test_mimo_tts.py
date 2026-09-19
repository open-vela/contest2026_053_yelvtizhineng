#!/usr/bin/env python3
"""
MiMo TTS 验证实验 v2（按 rholin33/mimotts README 修正）

用法（官方确认）：
  - Endpoint: POST <BASE_URL>/chat/completions（Token Plan = token-plan-cn）
  - 合成文本 → assistant message 的 content
  - 风格指令/音色描述 → user message 的 content
  - 音频参数 → audio
  - 返回音频 → choices[0].message.audio.data

用法：python3 test_mimo_tts.py "要合成的文本"
"""

import base64
import json
import sys

API_KEY  = "<在此填入你的 MiMo API Key>"
# Token Plan 专属端点（用户确认；官方端点对 tp key 返回 401）
TOKENPLAN_URL = "https://token-plan-cn.xiaomimimo.com/v1/chat/completions"

MODEL_TTS = "mimo-v2.5-tts"
VOICE     = "mimo_default"   # 内置音色（README：支持音色选择）

def main():
    text = sys.argv[1] if len(sys.argv) > 1 else "你好，我是小绿绿，你的植物管家。今天天气不错，记得给我浇水哦！"
    print(f"[INFO] 合成文本：{text}")
    print(f"[INFO] 模型：{MODEL_TTS} 音色：{VOICE}")

    payload = {
        "model": MODEL_TTS,
        "messages": [
            # ★ 风格指令/音色描述 → user
            {"role": "user", "content": "请用温柔亲切的植物管家语气朗读"},
            # ★ 合成文本 → assistant（关键修正）
            {"role": "assistant", "content": text}
        ],
        "audio": {"voice": VOICE},
        "max_completion_tokens": 1024
    }
    headers = {"Content-Type": "application/json",
               "api-key": API_KEY,
               "Authorization": f"Bearer {API_KEY}"}

    print(f"[INFO] POST {TOKENPLAN_URL}")
    print(f"[INFO] Payload: {len(json.dumps(payload))} bytes")

    import requests
    try:
        resp = requests.post(TOKENPLAN_URL, headers=headers, json=payload, timeout=90)
        status = resp.status_code
        body = resp.text
    except Exception as e:
        print(f"[RESULT] ❌ 网络异常：{e}")
        return

    print(f"[INFO] HTTP {status}")

    try:
        j = json.loads(body)
        if "choices" in j and j["choices"]:
            msg = j["choices"][0].get("message", {})
            # ★ 返回音频在 message.audio.data（不是 output_audio）
            audio_data = (msg.get("audio") or {}).get("data", "")
            if audio_data:
                print(f"[RESULT] ✅ TTS 成功！base64={len(audio_data)}B")
                raw = base64.b64decode(audio_data)
                # 探测格式（RIFF=wav, ID3/0xFFE0=mp3）
                if raw[:4] == b"RIFF":
                    ext = "wav"
                elif raw[:3] == b"ID3" or (len(raw) > 2 and raw[0] == 0xFF and (raw[1] & 0xE0) == 0xE0):
                    ext = "mp3"
                else:
                    ext = "bin"
                out = f"/tmp/mimo_tts_out.{ext}"
                with open(out, "wb") as f:
                    f.write(raw)
                print(f"[RESULT] 已存 {out}（{len(raw)}B, {ext}）—— 可播放试听")
                print(f"[RESULT] 回复文本：{msg.get('content','(无)')[:200]}")
            else:
                print(f"[RESULT] ⚠️ 无 audio.data，回复内容：{msg.get('content','')[:300]}")
                print(f"[RESULT] 完整 message：{json.dumps(msg, ensure_ascii=False)[:500]}")
        elif "error" in j:
            print(f"[RESULT] ❌ 错误：{j['error'].get('message','')[:300]}")
        else:
            print(f"[RESULT] ⚠️ 未知响应：{body[:300]}")
    except Exception as e:
        print(f"[RESULT] ⚠️ 解析失败：{e}")

if __name__ == "__main__":
    main()
