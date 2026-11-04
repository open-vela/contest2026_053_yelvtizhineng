#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
parse_photo_rgb.py — PC 端解析 OV3660 拍到的 RGB565 帧文件 photo.rgb

用法:
    python3 parse_photo_rgb.py <photo.rgb> [--png out.png] [--show]

用途（与设备端 `plant cam capture` 的诊断输出逐项对比）:
    1. 打印前 16 字节 —— 与设备端 "[Cam-DVP] frame head:" 比对
    2. 打印 0/320/16000 偏移的像素双解码 [le|swp r g b | r g b]
       —— 与设备端 "row 0/1/50" 输出比对（le = 小端不交换 = 正确）
    3. 通道统计（min/max/avg, R/G/B）—— 判断是"暗/溢出/偏色"
    4. 存成 PNG（PIL）或 BMP —— 肉眼看是不是正常图像

判定:
    - PC 端 LE 解码出正常图像（白墙白、轮廓清晰）
      → 数据链路正确，问题在设备端显示/解析（LVGL 缩放/字节序）
    - PC 端也是彩色垃圾 → 数据本身错（管脚/格式/时序）
    - PC 端 LE 与设备端 row 输出不一致 → 设备端解析/打印有 bug
"""

import sys
import struct
import argparse

W, H = 160, 120
FRAME = W * H * 2  # 38400


def rgb565_le(p):
    """小端（低字节在前）—— OV3660 实际输出, 与设备端 swap=off 一致"""
    v = p[0] | (p[1] << 8)
    return ((v >> 11) & 0x1f, (v >> 5) & 0x3f, v & 0x1f)


def rgb565_be(p):
    """大端（高字节在前, 交换后）—— 与设备端 swap=on 一致"""
    v = (p[0] << 8) | p[1]
    return ((v >> 11) & 0x1f, (v >> 5) & 0x3f, v & 0x1f)


def to8(t):
    r, g, b = t
    return (r << 3, g << 2, b << 3)


def main():
    ap = argparse.ArgumentParser(description="Parse OV3660 RGB565 frame file")
    ap.add_argument("file", help="photo.rgb (38400 bytes)")
    ap.add_argument("--png", default=None, help="save PNG path")
    ap.add_argument("--show", action="store_true", help="show PNG (needs GUI)")
    args = ap.parse_args()

    data = open(args.file, "rb").read()
    print(f"file size: {len(data)} bytes (expect {FRAME})")
    if len(data) < FRAME:
        print("ERROR: file too small")
        sys.exit(1)

    # 1) 前 16 字节, 与设备端 frame head 对比
    head = " ".join(f"{b:02x}" for b in data[:16])
    print(f"frame head: {head}")

    # 2) 与设备端 row 0/1/50 同格式的像素解码
    def dump(off):
        row = off // (W * 2)
        out = f"row {row} @{off}:"
        for k in range(4):
            p = data[off + k * 2: off + k * 2 + 2]
            le = rgb565_le(p)
            be = rgb565_be(p)
            out += (f" [{le[0]:04x}|{be[0]:04x} r{le[0]:02d} g{le[1]:02d} "
                    f"b{le[2]:02d} | r{be[0]:02d} g{be[1]:02d} b{be[2]:02d}]")
        print(out)

    dump(0)
    dump(320)
    dump(16000)

    # 3) 通道统计（LE 与 BE 各算一遍）
    for name, dec in (("LE(no-swap=正确)", rgb565_le), ("BE(swap=错误)", rgb565_be)):
        rs, gs, bs = [], [], []
        for off in range(0, FRAME, 2):
            r, g, b = dec(data[off:off + 2])
            rs.append(r)
            gs.append(g)
            bs.append(b)
        print(f"[{name}] R min={min(rs)} max={max(rs)} avg={sum(rs)/len(rs):.1f} "
              f"| G min={min(gs)} max={max(gs)} avg={sum(gs)/len(gs):.1f} "
              f"| B min={min(bs)} max={max(bs)} avg={sum(bs)/len(bs):.1f}")
        nz = sum(1 for r, g, b in zip(rs, gs, bs) if r or g or b)
        print(f"[{name}] non-zero pixels: {nz}/{W*H} "
              f"({100.0*nz/(W*H):.1f}%)  distinct16: "
              f"{len(set(data[off] | (data[off+1]<<8) for off in range(0, FRAME, 2)))}")

    # 4) 存 PNG（LE 解码）
    img = bytearray()
    for off in range(0, FRAME, 2):
        img += bytes(to8(rgb565_le(data[off:off + 2])))
    try:
        from PIL import Image
        im = Image.frombytes("RGB", (W, H), bytes(img))
        out = args.png or "photo_le.png"
        im.save(out)
        print(f"saved LE image -> {out}")
        im2 = Image.frombytes("RGB", (W, H), b"".join(
            bytes(to8(rgb565_be(data[o:o + 2]))) for o in range(0, FRAME, 2)))
        im2.save("photo_be.png")
        print("saved BE image -> photo_be.png")
        if args.show:
            im.show()
    except ImportError:
        # 纯 Python BMP 兜底
        with open("photo_le.bmp", "wb") as f:
            row_size = (W * 3 + 3) & ~3
            f.write(b"BM")
            f.write(struct.pack("<IHHI", 54 + row_size * H, 0, 0, 54))
            f.write(struct.pack("<IiiHHIIiiII", 40, W, H, 1, 24, 0,
                                row_size * H, 2835, 2835, 0, 0))
            for y in range(H - 1, -1, -1):
                row = bytes(img[(y * W) * 3: (y * W + W) * 3])
                f.write(row + b"\x00" * (row_size - W * 3))
        print("saved LE image -> photo_le.bmp (PIL not available)")


if __name__ == "__main__":
    main()
