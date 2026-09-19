#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
fetch_photo_rgb.py — 通过串口把板子 SD 卡上的 photo.rgb 抓回 PC

用法:
    python3 fetch_photo_rgb.py /dev/ttyACM0 [--baud 115200] [--out photo.rgb]

流程:
    1. 确保 SD 已挂载:   plant sd status   （无 FAT 先 mkfatfs /dev/mmcsd1）
    2. plant cam capture             → 板子写 /mnt/sd/photo.rgb
    3. base64enc /mnt/sd/photo.rgb   → 板子以 base64 文本输出（规避控制台 \n 转义）
    4. 本脚本解析 base64 → 存 photo.rgb → 可接 parse_photo_rgb.py 解析

注意: 打开串口时不要拉 DTR/RTS（否则会复位板子）。
"""

import os
import re
import sys
import time
import base64
import termios
import select
import argparse


def open_tty(port):
    fd = os.open(port, os.O_RDWR | os.O_NOCTTY)
    attrs = termios.tcgetattr(fd)
    attrs[0] &= ~(termios.IGNBRK | termios.BRKINT | termios.PARMRK |
                  termios.ISTRIP | termios.INLCR | termios.IGNCR |
                  termios.ICRNL | termios.IXON)
    attrs[1] &= ~termios.OPOST
    attrs[2] &= ~(termios.CSIZE | termios.PARENB)
    attrs[2] |= termios.CS8
    attrs[3] &= ~(termios.ECHO | termios.ECHONL | termios.ICANON |
                  termios.ISIG | termios.IEXTEN)
    termios.tcsetattr(fd, termios.TCSANOW, attrs)
    return fd


def drain(fd, t):
    out = b""
    end = time.time() + t
    while time.time() < end:
        r, _, _ = select.select([fd], [], [], 0.2)
        if r:
            try:
                d = os.read(fd, 65536)
                if not d:
                    break
                out += d
            except OSError:
                break
    return out


def send(fd, cmd, wait=1.5):
    os.write(fd, (cmd + "\r\n").encode())
    time.sleep(0.3)
    return drain(fd, wait)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("port", help="serial port e.g. /dev/ttyACM0")
    ap.add_argument("--out", default="photo.rgb", help="output file")
    args = ap.parse_args()

    fd = open_tty(args.port)
    drain(fd, 0.5)

    print(send(fd, "plant sd status", 2.0).decode("utf-8", "replace"))
    print(send(fd, "plant cam capture", 10.0).decode("utf-8", "replace"))
    print("-- requesting base64 of /mnt/sd/photo.rgb --")
    os.write(fd, b"base64enc /mnt/sd/photo.rgb\r\n")
    time.sleep(1.0)

    buf = b""
    end = time.time() + 30  # 38KB -> ~52KB base64 @115200 ≈ 5s, 留余量
    while time.time() < end:
        r, _, _ = select.select([fd], [], [], 0.5)
        if r:
            try:
                d = os.read(fd, 65536)
                if not d:
                    break
                buf += d
            except OSError:
                break
    os.close(fd)

    txt = buf.decode("utf-8", "replace")
    # 提取 base64 行（命令回显之后、nsh> 提示符之前的连续 base64 字符）
    m = re.search(r"base64enc\s+\S+\s*\r?\n([A-Za-z0-9+/=\r\n]+)", txt)
    if not m:
        print("ERROR: could not find base64 payload in output")
        print(txt[-500:])
        sys.exit(1)
    payload = re.sub(r"[\r\n\s]+", "", m.group(1))
    try:
        raw = base64.b64decode(payload)
    except Exception as e:
        print(f"ERROR: base64 decode failed: {e}")
        print(payload[:80])
        sys.exit(1)

    with open(args.out, "wb") as f:
        f.write(raw)
    print(f"saved {len(raw)} bytes -> {args.out} (expect 38400)")
    if len(raw) != 38400:
        print("WARN: size mismatch — file may be truncated or stale")


if __name__ == "__main__":
    main()
