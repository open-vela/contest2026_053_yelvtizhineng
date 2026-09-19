#!/usr/bin/env python3
"""
模拟设备端：分段上传 PCM → 服务器辅助手 → 收文本 + TTS 音频

验证服务器辅助手（tools/server/server_bridge.py）全链路：
  分段 POST PCM 块 → finalize → 服务器调 MiMo 音频理解 + TTS → 回 {text, audio_b64}

用法：
  1. 先启动服务器：python3 tools/server/server_bridge.py 8000
  2. 再跑本脚本：python3 tools/test_server_bridge.py <语音.wav> [服务器地址]
     语音.wav = 16kHz mono PCM（可用 check_wav.py 确认）

设备固件将按此协议实现（同样的 upload/finalize）。
"""

import json
import struct
import sys
import time
import urllib.request

def wav_to_pcm(wav_path):
    """读 WAV，剥离 RIFF 头返回 PCM 数据（假设 16kHz mono 16bit）"""
    with open(wav_path, "rb") as f:
        data = f.read()
    # 找 data chunk
    pos = 12
    while pos + 8 <= len(data):
        cid = data[pos:pos + 4]
        csize = struct.unpack("<I", data[pos + 4:pos + 8])[0]
        if cid == b"data":
            return data[pos + 8:pos + 8 + csize]
        pos += 8 + csize + (csize & 1)
    raise ValueError("无 data chunk")

def main():
    if len(sys.argv) < 2:
        print("用法：python3 test_server_bridge.py <语音.wav> [服务器地址]")
        sys.exit(1)
    wav_path = sys.argv[1]
    base = sys.argv[2] if len(sys.argv) > 2 else "http://127.0.0.1:8000"
    base = base.rstrip("/")

    pcm = wav_to_pcm(wav_path)
    print(f"[INFO] PCM 数据：{len(pcm)}B ({len(pcm)/32000:.1f}s @16kHz)")

    session = f"test-{int(time.time())}"
    chunk_bytes = 3200   # 100ms @16kHz mono 16bit

    # 1. 分段上传（模拟设备 100ms 块）
    seq = 0
    for off in range(0, len(pcm), chunk_bytes):
        chunk = pcm[off:off + chunk_bytes]
        url = f"{base}/voice/upload?session={session}&seq={seq}"
        req = urllib.request.Request(url, data=chunk, method="POST",
                                     headers={"Content-Type": "application/octet-stream"})
        with urllib.request.urlopen(req, timeout=10) as r:
            resp = json.loads(r.read().decode())
        if not resp.get("ok"):
            print(f"❌ 上传块 {seq} 失败：{resp}")
            sys.exit(1)
        seq += 1
        if seq % 10 == 0:
            print(f"[UPLOAD] 已传 {seq} 块 ({off+len(chunk)}B / {len(pcm)}B)")
        time.sleep(0.01)   # 模拟设备录音节奏（可调小加速测试）

    print(f"[UPLOAD] 完成，共 {seq} 块，total={resp.get('total')}B")

    # 2. finalize → 服务器调 MiMo（理解 + TTS），可能 10-30s
    print("[FINALIZE] 等待服务器处理（MiMo 理解 + TTS，约 10-30s）...")
    url = f"{base}/voice/finalize?session={session}"
    req = urllib.request.Request(url, data=b"", method="POST")
    t0 = time.time()
    with urllib.request.urlopen(req, timeout=120) as r:
        result = json.loads(r.read().decode())

    print(f"[FINALIZE] 耗时 {time.time()-t0:.1f}s")
    print(f"\n[RESULT] 回复文本：{result.get('text','')[:500]}")

    if result.get("audio_b64"):
        import base64
        audio = base64.b64decode(result["audio_b64"])
        ext = result.get("audio_ext", "wav")
        out = f"/tmp/device_tts_out.{ext}"
        with open(out, "wb") as f:
            f.write(audio)
        print(f"[RESULT] ✅ TTS 音频已存 {out}（{len(audio)}B）—— 可播放试听")
    else:
        print("[RESULT] ⚠️ 服务器未返回音频（TTS 可能失败，仅文本）")

if __name__ == "__main__":
    main()
