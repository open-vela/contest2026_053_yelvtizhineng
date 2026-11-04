#!/usr/bin/env python3
"""
WAV 文件格式诊断（Phase 1.0e 辅助）

确认音频文件是不是"标准 16kHz mono PCM16 WAV"——MiMo 解析失败最常见原因
就是文件实际是 MP3/AAC/m4a 但后缀是 .wav。

用法：python3 check_wav.py <文件>
"""

import struct
import sys

def main():
    if len(sys.argv) < 2:
        print("用法：python3 check_wav.py <音频文件>")
        sys.exit(1)

    path = sys.argv[1]
    with open(path, "rb") as f:
        data = f.read()

    print(f"[INFO] 文件：{path} ({len(data)}B)")

    # ── 判 RIFF/WAVE ──
    if data[:4] == b"RIFF":
        print("[FMT ] ✅ RIFF 头存在（WAV 容器）")
    else:
        print(f"[FMT ] ❌ 无 RIFF 头 → 不是标准 WAV，实际是：{data[:4]!r}")
        print(f"       若为 ID3/MP3/.... → 这是 MP3/AAC 伪装成 wav，需先转 PCM")
        return

    if data[8:12] == b"WAVE":
        print("[FMT ] ✅ WAVE 标识存在")
    else:
        print(f"[FMT ] ❌ 无 WAVE 标识（{data[8:12]!r}），可能损坏")
        return

    # ── 遍历 chunk 找 fmt ──
    pos = 12
    while pos + 8 <= len(data):
        cid = data[pos:pos + 4]
        csize = struct.unpack("<I", data[pos + 4:pos + 8])[0]
        if cid == b"fmt ":
            fmt_tag, ch, rate, byterate, blk_align, bits = struct.unpack(
                "<HHIIHH", data[pos + 8:pos + 24])
            print(f"[FMT ] ✅ fmt chunk: tag={fmt_tag}(1=PCM) ch={ch} rate={rate}Hz bits={bits}")
            print(f"       byterate={byterate} block_align={blk_align}")
            if fmt_tag == 1:
                print(f"[FMT ] ✅ 编码=PCM（无损，MiMo 可解析）")
            else:
                print(f"[FMT ] ⚠️ 编码 tag={fmt_tag}（≠1）→ 可能是压缩格式（MP3=0x55/85, ADPCM=2, A-law=6, u-law=7）→ MiMo 按 wav 解析可能失败")
            if rate == 16000 and ch == 1 and bits == 16:
                print(f"[FMT ] ✅ 16kHz mono 16bit = MiMo 最稳格式，可以直接测")
            else:
                print(f"[FMT ] ⚠️ 非 16k/mono/16bit，建议 ffmpeg 转：")
                print(f"       ffmpeg -i {path} -ar 16000 -ac 1 -c:a pcm_s16le out.wav")
            return
        pos += 8 + csize + (csize & 1)

    print("[FMT ] ❌ 未找到 fmt chunk，文件可能损坏或不是 WAV")

if __name__ == "__main__":
    main()
