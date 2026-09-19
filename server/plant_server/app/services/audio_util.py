# -*- coding: utf-8 -*-
"""音频小工具：把 TTS 出来的 24kHz WAV 降到 16kHz。

2026-09-16 语音时延修复：MiMo TTS 固定输出 24kHz/16bit/单声道（实测：
传 sample_rate=16000 / speed 都被忽略，字节数一个没少），而板卡 I2S 跑
24kHz —— 也就是说 1 秒语音要在网线上跑 48KB。板卡那边本来就有 16kHz
→ 24kHz 的插值播放路径（见 ai_voice_play_file），所以服务器先降到 16kHz
（32KB/s）是纯赚：传输量直接砍掉 1/3，音质是语音级别够用。

刻意不依赖 numpy/audioop：云端是 Python 3.10（有 audioop），本机开发是
Python 3.14（audioop 已被移除），两边都要能跑。
"""
import array
import struct
import sys


def _find_chunk(data, want, start=12):
    """在 RIFF 里顺序找 chunk（返回 (payload_off, size)）。"""
    off = start
    n = len(data)
    while off + 8 <= n:
        cid = data[off:off + 4]
        size = struct.unpack("<I", data[off + 4:off + 8])[0]
        if cid == want:
            return off + 8, size
        off += 8 + size + (size & 1)
    return None, 0


def parse_wav(data):
    """解析 WAV：返回 (采样率, 声道数, 位深, data 偏移, data 长度)，失败 None。"""
    if len(data) < 44 or data[:4] != b"RIFF" or data[8:12] != b"WAVE":
        return None
    foff, fsize = _find_chunk(data, b"fmt ")
    if foff is None or fsize < 16:
        return None
    doff, dsize = _find_chunk(data, b"data")
    if doff is None:
        return None
    fmt, ch, rate = struct.unpack("<HHI", data[foff:foff + 8])
    bits = struct.unpack("<H", data[foff + 14:foff + 16])[0]
    if fmt != 1 or bits != 16 or ch < 1:
        return None
    if doff + dsize > len(data):
        dsize = len(data) - doff
    return rate, ch, bits, doff, dsize


def _wav_header(rate, channels, data_bytes):
    block = channels * 2
    return (b"RIFF" + struct.pack("<I", 36 + data_bytes) + b"WAVE"
            + b"fmt " + struct.pack("<IHHIIHH", 16, 1, channels, rate,
                                    rate * block, block, 16)
            + b"data" + struct.pack("<I", data_bytes))


def _resample_pcm16(pcm, src_rate, dst_rate, channels):
    """16bit PCM 线性插值重采样；降采样时先做 3 点均值抗混叠。"""
    n_in = len(pcm) // 2
    frames_in = n_in // channels
    if frames_in <= 1:
        return pcm
    frames_out = max(1, int(frames_in * dst_rate / float(src_rate)))
    src = array.array("h")
    src.frombytes(pcm[:frames_in * channels * 2])

    out = array.array("h", bytes(frames_out * channels * 2))
    step = src_rate / float(dst_rate)
    down = dst_rate < src_rate
    last = frames_in - 1

    for i in range(frames_out):
        pos = i * step
        i0 = int(pos)
        frac = pos - i0
        if i0 > last:
            i0 = last
        if down:
            lo = (i0 - 1) if i0 > 0 else 0
            hi = (i0 + 1) if i0 < last else last
            o = i * channels
            a = lo * channels
            b = i0 * channels
            c = hi * channels
            for k in range(channels):
                v = (src[a + k] + src[b + k] + src[c + k]) / 3.0
                out[o + k] = int(v + (0.5 if v >= 0 else -0.5))
        else:
            hi = (i0 + 1) if i0 < last else last
            o = i * channels
            a = i0 * channels
            c = hi * channels
            for k in range(channels):
                v = src[a + k] + (src[c + k] - src[a + k]) * frac
                out[o + k] = int(v + (0.5 if v >= 0 else -0.5))
    return out.tobytes()


def wav_to_rate(data, target=16000):
    """把 WAV 重采样到 target Hz。不是 16bit PCM WAV、或本来就够低
    （采样率 ≤ target）→ 原样返回：宁可大一点，也不要把音频弄坏。"""
    info = parse_wav(data)
    if info is None:
        return data
    rate, ch, _bits, doff, dsize = info
    if rate <= target or dsize <= 0:
        return data
    pcm = _resample_pcm16(data[doff:doff + dsize], rate, target, ch)
    return _wav_header(target, ch, len(pcm)) + pcm
