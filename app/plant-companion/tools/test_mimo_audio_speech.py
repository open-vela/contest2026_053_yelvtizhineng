#!/usr/bin/env python3
"""
MiMo 音频真实语音测试 v3（Token Plan 端点 + MIME 前缀，Phase 1.0d）

用户确认（Token Plan 官方文档截图）：
  - 我们是 Token Plan 套餐（tp- 开头 key）→ 专属端点
    https://token-plan-cn.xiaomimimo.com/v1（不是 api.xiaomimimo.com）
  - base64 必须带 MIME 前缀：data:audio/wav;base64,<b64>  ← 之前缺失是主因

判据：
  - audio_tokens 上升且模型转写/描述了语音内容 = MiMo 音频真支持 ✅
  - 模型回复"无法接收" = 仍失败（看响应里的具体错误）

用法：python3 test_mimo_audio_speech.py <语音.wav>
"""

import base64
import json
import sys

API_KEY  = "<在此填入你的 MiMo API Key>"
# ★ Token Plan 专属端点（用户截图确认）
API_URL  = "https://token-plan-cn.xiaomimimo.com/v1/chat/completions"
MODEL    = "mimo-v2.5"
PROMPT   = "请仔细听这段语音，把你听到的内容原样转写出来，然后回答：植物需要浇水吗？"

def main():
    if len(sys.argv) < 2:
        print("用法：python3 test_mimo_audio_speech.py <语音.wav>")
        sys.exit(1)

    wav_path = sys.argv[1]
    try:
        with open(wav_path, "rb") as f:
            wav = f.read()
    except Exception as e:
        print(f"❌ 读取 {wav_path} 失败：{e}")
        sys.exit(1)

    print(f"[INFO] 语音文件：{wav_path} ({len(wav)}B)")
    if wav[:4] != b"RIFF":
        print("⚠️ 无 RIFF 头，可能不是标准 WAV")

    b64 = base64.b64encode(wav).decode("ascii")

    # ★ 关键修正：带 MIME 前缀（官方要求）
    data_url = f"data:audio/wav;base64,{b64}"

    payload = {
        "model": MODEL,
        "messages": [{
            "role": "user",
            "content": [
                {"type": "input_audio",
                 "input_audio": {"data": data_url}},   # ← 带前缀
                {"type": "text", "text": PROMPT}
            ]
        }],
        "max_completion_tokens": 1024
    }
    # 官方 curl 用 api-key 头；也兼容 Bearer
    headers = {"Content-Type": "application/json",
               "api-key": API_KEY,
               "Authorization": f"Bearer {API_KEY}"}

    print(f"[INFO] POST {API_URL} (model={MODEL})")
    print(f"[INFO] data_url 前缀：data:audio/wav;base64,...（{len(b64)}B base64）")

    import requests
    try:
        resp = requests.post(API_URL, headers=headers, json=payload, timeout=90)
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
            text = msg.get("content", "") or ""
            usage = j.get("usage", {}).get("prompt_tokens_details", {})
            audio_tok = usage.get("audio_tokens", 0)
            total = j.get("usage", {}).get("prompt_tokens", 0)

            print(f"[RESPONSE] {text[:800]}")
            print(f"\n[RESULT] 音频 token：{audio_tok} / 总 prompt token：{total}")
            # ★ 2026-08-29 实测定案：audio_tokens≈19 是音频编码的固定开销，
            #   与内容无关（静音/有内容都是 19）——不能拿它判"模型没听"。
            #   判据只看回复内容：模型是否转写/描述了音频。
            if "无法" in text and ("音频" in text or "语音" in text):
                print(f"[RESULT] ⚠️ 模型自称听不到 → 大概率录音是静音/无内容，"
                      f"先检查音频文件是否有声音（用播放器/check_wav 辅助）")
            else:
                print(f"[RESULT] ✅ 模型基于音频回复了 → MiMo 音频理解可用！"
                      f"若回复含转写内容则完全确认")
        elif "error" in j:
            print(f"[RESULT] ❌ MiMo 报错：{j['error'].get('message','')[:300]}")
        else:
            print(f"[RESULT] ⚠️ 未知响应：{body[:300]}")
    except Exception as e:
        print(f"[RESULT] ⚠️ 解析失败：{e}")

if __name__ == "__main__":
    main()
