#!/usr/bin/env python3
"""
植小伴 · GB2312 全量中文字库生成器（CLAUDE_SYSTEM.md §14 阶段 B）

用途：根治"缺字方框"——生成 GB2312 一级(3755)+二级(3008)=6763 字的
16/20/24px 灰度位图字库，供设备端自定义 lv_font_t 按需流式读取（字库
落 SD /mnt/sd/fonts/plant_zh_<size>.bin，不进固件——OTA 槽 2MB 上限）。

用法：
  python3 generate_font_bin.py <size_px> [out_dir]
  例：python3 generate_font_bin.py 20 tools/server/fonts

输出格式（设备端 zh_font SD 源按此解析）：
  Header 24B: magic "PLANTZH1" | ver u16=1 | size_px u16 | count u32 |
               index_offset u32（=24+count*16，数据区起点）|
               line_height u16 | base_line u16（LVGL 行高/基线）
  Index: count 项，每项 16B，按 unicode 升序（设备端流式二分，fseek+fread）：
         unicode u32 | offset u32 | size u16 | adv_w u16(1/10px) |
         box_w u8 | box_h u8 | ofs_x i8 | ofs_y i8
  Data: 4bit 灰度位图（LVGL bpp=4 打包：每行 (box_w+1)/2 字节，高 4bit 在前）

依赖：pip install fonttools pillow
源字体：/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc（SC face）
"""

import os
import struct
import sys

from fontTools.ttLib import TTCollection
from PIL import Image, ImageDraw, ImageFont

MAGIC = b"PLANTZH1"
TTC_PATH = "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc"


def find_sc_index(ttc_path):
    """在 TTC 里找 "Noto Sans CJK SC" face 的 index（拿简体中文脸）"""
    tc = TTCollection(ttc_path)
    for i, f in enumerate(tc.fonts):
        name = f["name"].getDebugName(1) or ""
        if "SC" in name:
            return i
    return 0


def gb2312_hanzi(level1_only=False):
    """GB2312 汉字：区 16-55 一级 3755 + 56-87 二级 3008 = 6763。
    level1_only=True → 只取一级 3755（日常覆盖 99.9%，编译进固件用）"""
    chars = []
    q_end = 56 if level1_only else 88
    for q in range(16, q_end):
        for w in range(1, 95):
            try:
                chars.append(bytes([0xa0 + q, 0xa0 + w]).decode("gb2312"))
            except UnicodeDecodeError:
                pass
    return chars


# ⚠️ 2026-09-01 补全角标点：GB2312 符号区（1-9 区）常用标点——此前只生成
# 汉字区，UI/动态文本的全角标点（，。！？、；：""''（）《》——……·）显示方框。
PUNCT_SYMBOLS = "，。！？、；：""''（）《》【】—…·～￥％×÷　"

def gb2312_chars_with_punct(level1_only=False):
    """汉字 + 常用全角标点（UI/动态文本全覆盖）"""
    chars = gb2312_hanzi(level1_only)
    for ch in PUNCT_SYMBOLS:
        if ch not in chars:
            chars.append(ch)
    return chars


def bin_to_c(bin_path, c_path, array_name):
    """把 .bin 转成 C const 数组（编译进固件，zh_font 内置源用）"""
    data = open(bin_path, "rb").read()
    with open(c_path, "w") as f:
        f.write("/* 自动生成（tools/generate_font_bin.py）— GB2312 一级 3755 字 20px\n")
        f.write(" * 编译进固件：动态文本（AI 回复/诊断/日记）全量渲染，零外部依赖 */\n")
        f.write(f"const unsigned char {array_name}[] = {{\n")
        for i in range(0, len(data), 16):
            chunk = data[i:i + 16]
            f.write("  " + ", ".join(f"0x{b:02x}" for b in chunk) + ",\n")
        f.write("};\n")
    print(f"[FONT] C 数组 {c_path}: {len(data)}B -> {array_name}[]")


def main():
    if len(sys.argv) < 2:
        print("用法: python3 generate_font_bin.py <size_px> [out_dir] [--level1] [--c]")
        sys.exit(1)

    size = int(sys.argv[1])
    outdir = sys.argv[2] if len(sys.argv) > 2 and not sys.argv[2].startswith("--") else "."
    level1 = "--level1" in sys.argv
    to_c = "--c" in sys.argv

    idx = find_sc_index(TTC_PATH)
    font = ImageFont.truetype(TTC_PATH, size, index=idx)
    ascent, descent = font.getmetrics()

    chars = gb2312_chars_with_punct(level1)
    print(f"[FONT] {len(chars)} 字 @ {size}px, SC face={idx}, "
          f"ascent={ascent} descent={descent}{'（一级）' if level1 else ''}"
          f"（含标点 {len(PUNCT_SYMBOLS)}）")

    entries = []   # (unicode, off, size, adv, w, h, ofsx, ofsy, packed)
    data = bytearray()
    skip = 0

    for ch in chars:
        bbox = font.getbbox(ch)
        l, t, r, b = bbox
        w, h = r - l, b - t

        if w <= 0 or h <= 0:
            skip += 1
            continue

        adv = int(round(font.getlength(ch) * 10))   # LVGL adv_w 单位 = 1/10px

        img = Image.new("L", (w, h), 0)
        ImageDraw.Draw(img).text((-l, -t), ch, 255, font=font)
        px = img.tobytes()

        # 8bit 灰度 → 4bit（高 4bit 在前，LVGL bpp=4 打包）
        row_bytes = (w + 1) // 2
        packed = bytearray(row_bytes * h)
        for y in range(h):
            for x in range(w):
                v = px[y * w + x] >> 4
                if v > 15:
                    v = 15
                off = y * row_bytes + (x >> 1)
                if x & 1:
                    packed[off] |= v
                else:
                    packed[off] |= v << 4

        # LVGL ofs_y = ascent - 字形底（b = t + h）；现有字体汉字 ofs_y∈[-6,0] 实锤
        #（错误语义 ascent-t 会让字形上移/截断——LVGL y1=(lh-base)-box_h-ofs_y）
        entries.append((ord(ch), len(data), len(packed), adv, w, h,
                        l, ascent - (t + h), bytes(packed)))
        data += packed

    entries.sort(key=lambda e: e[0])
    count = len(entries)
    index_size = count * 16
    header_size = 24  # MAGIC(8)+ver(2)+size(2)+count(4)+idx_off(4)+lh(2)+bl(2)
    out = os.path.join(outdir, f"plant_zh_{size}.bin")

    with open(out, "wb") as f:
        f.write(MAGIC + struct.pack("<HHIIHH", 1, size, count,
                                    header_size + index_size,
                                    ascent + descent, descent))
        for (u, off, sz, adv, w, h, ofsx, ofsy, _) in entries:
            f.write(struct.pack("<IIHHBBbb", u, off, sz, adv, w, h,
                                ofsx, ofsy))
        f.write(data)

    print(f"[FONT] OK: {out}  {header_size + index_size + len(data)}B, "
          f"{count} glyphs (skip {skip} 生僻缺字)")

    if to_c:
        bin_to_c(out, os.path.join(outdir, f"zh_font_builtin_{size}.c"),
                 f"zh_font_builtin_{size}")


if __name__ == "__main__":
    main()
