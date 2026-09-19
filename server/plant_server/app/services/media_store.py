# -*- coding: utf-8 -*-
"""媒体落盘：图像(raw565/jpeg→jpeg+thumb)、音频(wav)。"""
import io
import os
import re

from .. import config, db, utils

# Windows 盘符路径：在 Linux 上 os.path.isabs() 会误判成相对路径
_WIN_ABS = re.compile(r"^[A-Za-z]:/")


def rgb565_to_pil(rgb565, width=config.IMAGE_W, height=config.IMAGE_H):
    """RGB565(小端, 设备 swap=ON 后格式) → PIL RGB Image。"""
    from PIL import Image
    n = width * height
    if len(rgb565) < n * 2:
        raise ValueError("raw565 长度不足: %d != %d" % (len(rgb565), n * 2))
    rgb = bytearray(n * 3)
    for i in range(n):
        lo = rgb565[i * 2]
        hi = rgb565[i * 2 + 1]
        v = lo | (hi << 8)
        rgb[i * 3] = ((v >> 11) & 0x1F) << 3
        rgb[i * 3 + 1] = ((v >> 5) & 0x3F) << 2
        rgb[i * 3 + 2] = (v & 0x1F) << 3
    return Image.frombytes("RGB", (width, height), bytes(rgb))


def _save_jpeg(img, path, quality=85):
    buf = io.BytesIO()
    img.save(buf, format="JPEG", quality=quality)
    with open(path, "wb") as f:
        f.write(buf.getvalue())
    return buf.getvalue()


def save_image(owner_user_id, plant_id, data, fmt, w=None, h=None):
    """fmt=raw565|jpeg。返回 media dict。

    raw565 支持 160x120（预览）、320x240（拍照）与 640x480（旧高清，兼容）
    三种尺寸，由调用方显式传 w/h（见 routers/media.py:resolve_raw565_dims）。
    """
    from PIL import Image
    now = utils.bj_now()
    mid = utils.new_id("md")
    day = now[1].replace("-", "")
    sub = "image"
    base = os.path.join(config.MEDIA_DIR, sub, day)
    os.makedirs(base, exist_ok=True)

    if fmt == "raw565":
        img = rgb565_to_pil(data, w or config.IMAGE_W, h or config.IMAGE_H)
        jpg_path = os.path.join(base, mid + ".jpg")
        jpeg_bytes = _save_jpeg(img, jpg_path)
        raw_path = os.path.join(config.MEDIA_DIR, "raw", day, mid + ".565")
        os.makedirs(os.path.dirname(raw_path), exist_ok=True)
        with open(raw_path, "wb") as f:
            f.write(data)
        thumb = img.copy()
        thumb.thumbnail((160, 120))
        thumb_path = os.path.join(base, mid + "_thumb.jpg")
        _save_jpeg(thumb, thumb_path)
        meta = "raw565->jpeg %dx%d" % (img.width, img.height)
    elif fmt == "jpeg":
        img = Image.open(io.BytesIO(data))
        img.load()
        jpg_path = os.path.join(base, mid + ".jpg")
        jpeg_bytes = _save_jpeg(img, jpg_path)
        thumb = img.copy()
        thumb.thumbnail((160, 120))
        thumb_path = os.path.join(base, mid + "_thumb.jpg")
        _save_jpeg(thumb, thumb_path)
        meta = "jpeg"
    else:
        raise ValueError("不支持的图像格式: " + str(fmt))

    db.exe(
        "INSERT INTO media(id, plant_id, owner_user_id, kind, fmt, path, thumb_path, meta, created_at)"
        " VALUES(?,?,?,?,?,?,?,?,?)",
        (mid, plant_id, owner_user_id, "image", fmt,
         _rel(jpg_path), _rel(thumb_path), meta,
         now[0]))
    return db.one("SELECT * FROM media WHERE id=?", (mid,))


def save_audio(owner_user_id, plant_id, wav_bytes, meta=None):
    now = utils.bj_now()
    mid = utils.new_id("md")
    day = now[1].replace("-", "")
    base = os.path.join(config.MEDIA_DIR, "audio", day)
    os.makedirs(base, exist_ok=True)
    path = os.path.join(base, mid + ".wav")
    with open(path, "wb") as f:
        f.write(wav_bytes)
    db.exe(
        "INSERT INTO media(id, plant_id, owner_user_id, kind, fmt, path, thumb_path, meta, created_at)"
        " VALUES(?,?,?,?,?,?,?,?,?)",
        (mid, plant_id, owner_user_id, "audio", "wav",
         _rel(path), "", meta or "voice", now[0]))
    return db.one("SELECT * FROM media WHERE id=?", (mid,))


def _rel(p):
    """入库统一存相对 BASE_DIR 的路径：换目录、换系统（Windows/Linux）都能用。"""
    try:
        return os.path.relpath(p, config.BASE_DIR).replace("\\", "/")
    except ValueError:
        return p.replace("\\", "/")


def resolve_path(p):
    """把库里存的路径还原成本机绝对路径。

    - 新记录存的是相对 BASE_DIR 的路径 → 直接拼；
    - 历史记录可能是别的机器上的绝对路径（例如从 Windows 搬到 Linux），
      就按 data/media/... 的尾巴在本地重新定位。
    """
    if not p:
        return None
    q = str(p).replace("\\", "/")
    foreign = bool(_WIN_ABS.match(q))
    if not foreign and not os.path.isabs(q):
        return os.path.join(config.BASE_DIR, q)
    if os.path.exists(q):
        return q
    i = q.find("/media/")
    if i >= 0:
        cand = os.path.join(config.MEDIA_DIR, q[i + len("/media/"):])
        if os.path.exists(cand):
            return cand
    return q


def media_path(media_id):
    row = db.one("SELECT * FROM media WHERE id=?", (media_id,))
    if not row or not row.get("path"):
        return None
    return resolve_path(row["path"])
