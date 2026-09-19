# -*- coding: utf-8 -*-
import os

from fastapi import APIRouter, Header, HTTPException, Query, Request, Response

from .. import config, db
from ..services import media_store
from . import either, resolve_plant

router = APIRouter(prefix="/media", tags=["媒体"])


def resolve_raw565_dims(data_len, w=None, h=None):
    """定 raw565 的宽高：优先用设备显式传的 w/h，否则按字节数匹配已知尺寸。

    设备端预览 160x120 / 拍照 320x240 走同一个上传接口，长度不同 → 尺寸必须
    跟着变，否则会把拍照帧按 160x120 解码，得到一堆斜条，大模型
    更认不出来。
    """
    if w and h:
        if (w, h) not in config.RAW565_DIMS:
            raise HTTPException(400, "不支持的 raw565 尺寸 %dx%d（支持 %s）"
                                % (w, h, config.RAW565_DIMS))
        expect = w * h * 2
        if data_len != expect:
            raise HTTPException(400, "raw565 %dx%d 长度应为 %d，实际 %d"
                                % (w, h, expect, data_len))
        return w, h

    for (cw, ch) in config.RAW565_DIMS:
        if cw * ch * 2 == data_len:
            return cw, ch

    raise HTTPException(400, "raw565 长度 %d 无法匹配已知尺寸 %s"
                        % (data_len, config.RAW565_DIMS))


@router.post("/image")
async def upload_image(request: Request,
                       fmt: str = Query("raw565"),
                       w: int = Query(None),
                       h: int = Query(None),
                       plant_id: str = Query(None),
                       authorization: str = Header(None),
                       x_device_token: str = Header(None)):
    claims, kind = either(authorization, x_device_token)
    plant = resolve_plant(claims, kind, plant_id)
    data = await request.body()
    if not data:
        raise HTTPException(400, "空数据")
    dims = None
    if fmt == "raw565":
        dims = resolve_raw565_dims(len(data), w, h)
    elif fmt != "jpeg":
        raise HTTPException(400, "fmt 只支持 raw565|jpeg")
    try:
        media = media_store.save_image(claims["sub"], plant["id"], data, fmt,
                                       w=(dims[0] if dims else None),
                                       h=(dims[1] if dims else None))
    except Exception as e:
        raise HTTPException(400, "图片保存失败: %s" % e)
    return {"ok": True, "media": media,
            "size": ("%dx%d" % dims) if dims else "jpeg"}


@router.get("")
def list_media(plant_id: str = Query(None), limit: int = Query(12),
               authorization: str = Header(None)):
    """我名下的照片（最近的在前）。发帖配图、理赔补材料都从这里挑，
    只返回自己的，避免借接口翻别人的相册。"""
    claims, _kind = either(authorization, None)
    limit = max(1, min(50, limit))
    if plant_id:
        rows = db.q("SELECT id,plant_id,created_at FROM media WHERE "
                    "owner_user_id=? AND kind='image' AND plant_id=? "
                    "ORDER BY created_at DESC LIMIT ?",
                    (claims["sub"], plant_id, limit))
    else:
        rows = db.q("SELECT id,plant_id,created_at FROM media WHERE "
                    "owner_user_id=? AND kind='image' ORDER BY created_at DESC "
                    "LIMIT ?", (claims["sub"], limit))
    return {"ok": True, "media": [
        {"id": r["id"], "plant_id": r["plant_id"], "created_at": r["created_at"],
         "url": "/api/v1/media/%s/file" % r["id"],
         "thumb": "/api/v1/media/%s/file" % r["id"]} for r in rows]}


@router.get("/{media_id}/file")
def media_file(media_id: str):
    p = media_store.media_path(media_id)
    if not p or not os.path.exists(p):
        raise HTTPException(404, "媒体不存在")
    with open(p, "rb") as f:
        return Response(f.read(), media_type="application/octet-stream")
