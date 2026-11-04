# -*- coding: utf-8 -*-
"""路由公共：鉴权解析与权限校验。"""
import time

from fastapi import HTTPException

from .. import db, security, utils

# 设备在线判定：板卡固件没有 heartbeat 调用（见 server_bridge.c），只有开机注册
# 时才写一次 last_seen_at，于是管理台常年显示“离线”。这里改成**设备只要说话就
# 算在线**：任何带有效 X-Device-Token 的请求都会刷新 last_seen_at，写库按设备
# 节流 5 秒一次，避免语音分片（一次会话几十个包）把库写爆。
_TOUCH_AT = {}
TOUCH_THROTTLE_S = 5.0


def mark_device_seen(device_id):
    if not device_id:
        return
    now = time.time()
    if now - _TOUCH_AT.get(device_id, 0) < TOUCH_THROTTLE_S:
        return
    _TOUCH_AT[device_id] = now
    try:
        db.exe("UPDATE devices SET status='online', last_seen_at=? WHERE id=?",
               (utils.bj_now()[0], device_id))
    except Exception:
        pass


def touch_device_token(x_device_token):
    """有设备凭证就先认设备（老协议桥也带这个头，但以前没人看）。"""
    if not x_device_token:
        return None
    claims = security.verify_token(x_device_token)
    if not claims or claims.get("role") != "device":
        return None
    mark_device_seen(claims.get("sub"))
    return claims.get("sub")

def _claims_user(authorization):
    if not authorization:
        raise HTTPException(401, "请先登录")
    token = authorization[7:] if authorization.lower().startswith("bearer ") \
        else authorization
    claims = security.verify_token(token)
    if not claims or claims.get("role") != "user":
        raise HTTPException(401, "登录已失效，请重新登录")
    return claims

def _claims_device(x_device_token):
    if not x_device_token:
        raise HTTPException(401, "缺少设备凭证 X-Device-Token")
    claims = security.verify_token(x_device_token)
    if not claims or claims.get("role") != "device":
        raise HTTPException(401, "设备凭证无效")
    return claims

def either(authorization=None, x_device_token=None):
    """App 用户或设备都可用；返回 (claims, kind)。"""
    if authorization:
        return _claims_user(authorization), "user"
    if x_device_token:
        claims = _claims_device(x_device_token)
        mark_device_seen(claims.get("sub"))
        return claims, "device"
    raise HTTPException(401, "缺少凭证")

def auto_plant_for_device(device_id):
    """给还没有植物的设备自动建一盆（多终端：新板卡刷完即可用）。

    2026-09-21 双板卡实测：第二块板卡注册成功后一直没有植物，拍照上传
    拿到 404 且服务器在板卡还在发 153KB 图时就断连接，板卡侧表现为
    -107（Not connected）连试 8 次后降级。设备侧自动建一盆、不绑用户，
    App 里绑定设备时自动认领，最省事。
    """
    dev = db.one("SELECT * FROM devices WHERE id=?", (device_id,))
    pid = utils.new_id("pl")
    now = utils.bj_now()
    db.exe("INSERT INTO plants(id,user_id,device_id,species,name,started_at,"
           "location,health_score,mood,health_level,updated_at,created_at)"
           " VALUES(?,?,?,?,?,?,?,?,?,?,?,?)",
           (pid, (dev["owner_user_id"] if dev else None) or None, device_id,
            "待识别", "新植物", now[1], "客厅", 80, "😊", "良好",
            now[0], now[0]))
    try:
        from ..services import plant_ops
        plant_ops.ensure_today_tasks(pid)
    except Exception as e:
        print("[plant] ensure_today_tasks skipped: %s" % e)
    print("[plant] auto-created %s for device %s (multi-terminal onboarding)"
          % (pid, device_id))
    return db.one("SELECT * FROM plants WHERE id=?", (pid,))


def resolve_plant(claims, kind, plant_id=None, optional=False):
    """按用户/设备解析可访问的 plant；plant_id 缺省取第一盆。

    optional=True：连一盆植物都没有时返回 None 而不是报错 —— 给"聊天"用。
    2026-09-12 用户要求聊天不限于植物，新用户还没添加植物也该能问东问西，
    此时以 /ai/chat 为例会走"没有植物上下文"的通用问答。注意：显式传了
    plant_id 却查不到 / 无权访问，照样报错，不会静默降级。
    """
    if plant_id:
        p = db.one("SELECT * FROM plants WHERE id=?", (plant_id,))
        if not p:
            raise HTTPException(404, "植物不存在")
        if kind == "user":
            if p["user_id"] != claims["sub"]:
                raise HTTPException(403, "无权访问该植物")
        else:
            d = db.one("SELECT * FROM devices WHERE id=?", (claims["sub"],))
            if not d or (d["owner_user_id"] and p["user_id"] != d["owner_user_id"]
                         and p["device_id"] != d["id"]):
                raise HTTPException(403, "设备无权访问该植物")
        return p
    if kind == "user":
        p = db.one("SELECT * FROM plants WHERE user_id=? ORDER BY created_at "
                   "LIMIT 1", (claims["sub"],))
    else:
        p = db.one("SELECT * FROM plants WHERE device_id=? ORDER BY created_at "
                   "LIMIT 1", (claims["sub"],))
        if not p:
            d = db.one("SELECT * FROM devices WHERE id=?", (claims["sub"],))
            if d and d["owner_user_id"]:
                p = db.one("SELECT * FROM plants WHERE user_id=? "
                           "ORDER BY created_at LIMIT 1", (d["owner_user_id"],))
    if not p:
        if optional:
            return None
        if kind == "device":
            # 多终端：新板卡还没有植物时自动建一盆，别让拍照/遥测一直 404
            return auto_plant_for_device(claims["sub"])
        raise HTTPException(404, "还没有植物，请先在 App 添加或绑定设备")
    return p
