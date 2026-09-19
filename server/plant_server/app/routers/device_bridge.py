# -*- coding: utf-8 -*-
"""设备旧协议兼容层（板卡 server_bridge 客户端 → 服务器大脑）。

板上固件（plant-companion services/server_bridge.c）按早期“PC 辅助手”
协议通信，无 /api/v1 前缀、无鉴权头、会话用设备自增整数 id：
  GET  /time                     → {unix, date, time}（北京时间，UTC+8）
  POST /voice/upload?session&seq  body=裸 PCM16 块 → 自动建会话
  POST /voice/finalize?session    → {ok, text, has_audio}
  GET  /voice/audio?session       → TTS WAV 文件
  POST /image/analyze             body=RGB565 160×120 / 640×480 → {ok, text}
本层把请求映射到 /api/v1 同一套业务服务（同一 DB/媒体/对话/健康分），
并把会话绑定到种子演示设备（SN=ZXB-DEMO-0001）的植物。

⚠️ 仅演示/内网使用：无鉴权等价于固定设备凭证，正式部署需改为
X-Device-Token 并加 HTTPS。
"""
import os
import threading
import time

from fastapi import (APIRouter, Header, HTTPException, Query, Request,
                     Response)

from .. import config, db, utils
from ..services import media_store
from . import resolve_plant, touch_device_token
from .ai import finalize_voice_session, run_diagnose

router = APIRouter(tags=["设备桥兼容"])

DEMO_SN = "ZXB-DEMO-0001"
# 兼容层也接受高清帧：按 body 长度匹配最大的已知尺寸（配置里已按大→小排列）
FRAME_BYTES = config.IMAGE_W * config.IMAGE_H * 2  # 160*120*2 = 38400（兜底）
_SESS = {}
_SESS_LOCK = threading.Lock()
_SESS_MAX = 64
_SESS_TTL_S = 900


def _demo_plant():
    dev = db.one("SELECT * FROM devices WHERE sn=?", (DEMO_SN,))
    if not dev:
        dev = db.one("SELECT * FROM devices ORDER BY created_at LIMIT 1")
    if not dev:
        raise HTTPException(503, "尚未注册演示设备，请先 /api/v1/devices/register")
    plant = resolve_plant({"sub": dev["id"], "role": "device"}, "device")
    return dev, plant


def _cleanup_locked():
    now = time.time()
    for k in [k for k, v in _SESS.items() if now - v["last"] > _SESS_TTL_S]:
        _SESS.pop(k, None)


def _sess(sid):
    with _SESS_LOCK:
        s = _SESS.get(sid)
        if s is None:
            _cleanup_locked()
            if len(_SESS) >= _SESS_MAX:
                old = min(_SESS, key=lambda k: _SESS[k]["last"])
                _SESS.pop(old, None)
            _, plant = _demo_plant()
            s = {"pcm": bytearray(), "plant_id": plant["id"],
                 "kind": "device", "created": time.time(), "last": time.time(),
                 "audio_media_id": None}
            _SESS[sid] = s
        s["last"] = time.time()
        return s


@router.get("/time")
def bridge_time(x_device_token: str = Header(None)):
    touch_device_token(x_device_token)
    full, date, hms, unix = utils.bj_now()
    return {"unix": unix, "date": date, "time": hms}


@router.post("/voice/upload")
async def bridge_voice_upload(request: Request,
                              session: str = Query(...),
                              seq: int = Query(0),
                              x_device_token: str = Header(None)):
    touch_device_token(x_device_token)
    s = _sess(session)
    chunk = await request.body()
    if chunk:
        with _SESS_LOCK:
            s["pcm"].extend(chunk)
            s["last"] = time.time()
    return {"ok": True, "session": session, "seq": seq,
            "received": len(chunk)}


@router.post("/voice/finalize")
def bridge_voice_finalize(session: str = Query(...),
                          x_device_token: str = Header(None)):
    touch_device_token(x_device_token)
    with _SESS_LOCK:
        s = _SESS.get(session)
    if not s:
        raise HTTPException(404, "会话不存在或已过期")
    payload = finalize_voice_session(s)
    with _SESS_LOCK:
        _SESS[session]["audio_media_id"] = payload["audio_media_id"]
        _SESS[session]["last"] = time.time()
    return payload


@router.get("/voice/audio")
def bridge_voice_audio(session: str = Query(...),
                       x_device_token: str = Header(None)):
    touch_device_token(x_device_token)
    with _SESS_LOCK:
        s = _SESS.get(session)
        mid = (s or {}).get("audio_media_id")
    if not mid:
        raise HTTPException(404, "无 TTS 音频（当前可能为演示模式）")
    p = media_store.media_path(mid)
    if not p or not os.path.exists(p):
        raise HTTPException(404, "音频文件缺失")
    with open(p, "rb") as f:
        return Response(f.read(), media_type="audio/wav")


@router.post("/image/analyze")
async def bridge_image_analyze(request: Request,
                               x_device_token: str = Header(None)):
    touch_device_token(x_device_token)
    data = await request.body()
    dims = None
    for (cw, ch) in config.RAW565_DIMS:
        if len(data) == cw * ch * 2:
            dims = (cw, ch)
            break
    if dims is None:
        raise HTTPException(400, "frame too small: %d" % len(data))
    nbytes = dims[0] * dims[1] * 2
    dev, plant = _demo_plant()
    media = media_store.save_image(dev["id"], plant["id"],
                                   data[:nbytes], "raw565",
                                   w=dims[0], h=dims[1])
    payload = run_diagnose(media, plant, "device")
    res = payload["result"] or {}
    parts = [str(res.get("summary") or "").strip()]
    sugs = [str(x.get("text")).strip()
            for x in (res.get("suggestions") or []) if x.get("text")]
    if sugs:
        parts.append("建议：" + "；".join(sugs))
    text = " ".join(x for x in parts if x).strip() or "体检完成"
    return {"ok": True, "text": text[:600],
            "diagnose_id": payload["diagnose_id"]}
