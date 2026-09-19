# -*- coding: utf-8 -*-
import time

from fastapi import APIRouter, Header, HTTPException
from pydantic import BaseModel

from .. import config, db, security, utils
from ..services import plant_ops
from . import _claims_device, _claims_user

router = APIRouter(prefix="/devices", tags=["设备"])


class DeviceReg(BaseModel):
    sn: str
    model: str = ""
    fw_version: str = ""


class BindIn(BaseModel):
    sn: str = ""
    device_id: str = ""
    # 可选：把这台设备绑到指定的植物上（多盆时用；不传就自动挑一盆）
    plant_id: str = ""


@router.post("/register")
def register_device(body: DeviceReg):
    sn = body.sn.strip()
    if not sn:
        raise HTTPException(400, "缺少设备序列号 sn")
    old = db.one("SELECT * FROM devices WHERE sn=?", (sn,))
    if old:
        # 已注册：SN 即设备凭证（演示期），幂等返回既有 token，
        # 并顺带刷新型号/固件版本（设备每次联网注册都能取回凭证）。
        db.exe("UPDATE devices SET model=?, fw_version=? WHERE id=?",
               (body.model or old["model"], body.fw_version or old["fw_version"],
                old["id"]))
        token = old["token"] or ""
        claims = security.verify_token(token)
        need_new = claims is None
        if not need_new:
            # 剩余有效期不足（例如把 TTL 从 7 天调长、或旧凭证快到期）也补发
            need_new = (claims.get("exp", 0) - time.time()) < \
                config.DEVICE_TOKEN_REFRESH_S
        if need_new:
            # 凭证失效（服务器换了 PLANT_TOKEN_SECRET）→ 补发一把：
            # 否则设备永远拿着旧签名重连，一直 401 且不会自愈
            # （2026-09-11 阿里云加固后实测踩到）。
            token = security.create_token(old["id"], "device")
            db.exe("UPDATE devices SET token=? WHERE id=?", (token, old["id"]))
            print("[device] token re-issued for sn=%s (left=%s)"
                  % (sn, "expired" if claims is None
                     else "%.1fd" % ((claims.get("exp", 0) - time.time()) / 86400)))
        return {"ok": True, "device": old, "already": True,
                "device_token": token}
    did = utils.new_id("d")
    token = security.create_token(did, "device")
    db.exe("INSERT INTO devices(id,sn,model,fw_version,owner_user_id,token,"
           " status,last_seen_at,created_at) VALUES(?,?,?,?,?,?,?,?,?)",
           (did, sn, body.model, body.fw_version, None, token, "offline",
            None, utils.bj_now()[0]))
    dev = db.one("SELECT id,sn,model,fw_version,status FROM devices WHERE id=?",
                 (did,))
    return {"ok": True, "device": dev, "device_token": token}


@router.post("/bind")
def bind_device(body: BindIn, authorization: str = Header(None)):
    claims = _claims_user(authorization)
    uid = claims["sub"]
    dev = None
    if body.sn:
        dev = db.one("SELECT * FROM devices WHERE sn=?", (body.sn.strip(),))
    elif body.device_id:
        dev = db.one("SELECT * FROM devices WHERE id=?", (body.device_id,))
    if not dev:
        raise HTTPException(404, "设备未注册（先让设备联网注册，或检查 SN）")
    if dev["owner_user_id"] and dev["owner_user_id"] != uid:
        raise HTTPException(403, "设备已绑定其他账号")
    db.exe("UPDATE devices SET owner_user_id=? WHERE id=?", (uid, dev["id"]))
    # 绑到哪一盆：显式指定的 > 这盆设备已有的植物 > 账号里还没绑卡的那盆 > 第一盆 > 新建
    # （以前直接拿"第一盆"，账号有两盆时会把设备挂错地方）
    plant = None
    if body.plant_id:
        plant = db.one("SELECT * FROM plants WHERE id=? AND user_id=?",
                       (body.plant_id, uid))
        if not plant:
            raise HTTPException(404, "要绑的那盆植物不存在")
    else:
        plant = db.one("SELECT * FROM plants WHERE device_id=?",
                       (dev["id"],)) or db.one(
            "SELECT * FROM plants WHERE user_id=? AND "
            "(device_id IS NULL OR device_id='') ORDER BY created_at LIMIT 1",
            (uid,)) or db.one(
            "SELECT * FROM plants WHERE user_id=? ORDER BY created_at LIMIT 1",
            (uid,))
    if not plant:
        pid = utils.new_id("pl")
        db.exe("INSERT INTO plants(id,user_id,device_id,species,name,"
               " started_at,location,health_score,mood,health_level,updated_at)"
               " VALUES(?,?,?,?,?,?,?,?,?,?,?)",
               (pid, uid, dev["id"], "绿萝", "小绿绿",
                utils.bj_now()[1], "客厅窗台", 80, "😊", "良好",
                utils.bj_now()[0]))
        plant = db.one("SELECT * FROM plants WHERE id=?", (pid,))
    else:
        # 一台板卡同一时间只挂一盆：先把别处指向它的摘掉，再指过去
        db.exe("UPDATE plants SET device_id=NULL WHERE device_id=? AND id<>?",
               (dev["id"], plant["id"]))
        db.exe("UPDATE plants SET device_id=? WHERE id=?", (dev["id"],
                                                            plant["id"]))
        plant = db.one("SELECT * FROM plants WHERE id=?", (plant["id"],))
    return {"ok": True, "device": dev, "plant": plant}


def _online(dev):
    if not dev or not dev.get("last_seen_at"):
        return False
    try:
        last = time.mktime(time.strptime(dev["last_seen_at"],
                                         "%Y-%m-%d %H:%M:%S"))
        return time.time() - last <= config.OFFLINE_AFTER_S
    except Exception:
        return False


@router.get("/{device_id}/status")
def device_status(device_id: str, authorization: str = Header(None),
                  x_device_token: str = Header(None)):
    if authorization:
        claims = _claims_user(authorization)
        dev = db.one("SELECT * FROM devices WHERE id=?", (device_id,))
        if not dev:
            raise HTTPException(404, "设备不存在")
        if dev["owner_user_id"] != claims["sub"]:
            raise HTTPException(403, "无权查看该设备")
    else:
        claims = _claims_device(x_device_token)
        dev = db.one("SELECT * FROM devices WHERE id=?", (claims["sub"],))
        if not dev:
            raise HTTPException(404, "设备不存在")
    dev["online"] = _online(dev)
    return {"ok": True, "device": dev}


@router.post("/heartbeat")
def heartbeat(x_device_token: str = Header(None)):
    claims = _claims_device(x_device_token)
    db.exe("UPDATE devices SET status='online', last_seen_at=? WHERE id=?",
           (utils.bj_now()[0], claims["sub"]))
    return {"ok": True, "config": {"upload_interval_s": config.UPLOAD_INTERVAL_S,
                                   "heartbeat_s": config.DEVICE_HEARTBEAT_S}}


@router.get("/sync")
def device_sync(x_device_token: str = Header(None)):
    """设备轮询：时间/配置/今日任务/健康分/最近事件。"""
    claims = _claims_device(x_device_token)
    dev = db.one("SELECT * FROM devices WHERE id=?", (claims["sub"],))
    if not dev:
        raise HTTPException(404, "设备不存在")
    plant = db.one("SELECT * FROM plants WHERE device_id=?",
                   (claims["sub"],))
    if not plant and dev["owner_user_id"]:
        plant = db.one("SELECT * FROM plants WHERE user_id=? "
                       "ORDER BY created_at LIMIT 1", (dev["owner_user_id"],))
    tasks = plant_ops.today_tasks(plant["id"]) if plant else []
    last = db.one("SELECT * FROM telemetry WHERE plant_id=? ORDER BY ts DESC "
                  "LIMIT 1", (plant["id"],)) if plant else None
    evs = db.q("SELECT * FROM events WHERE plant_id=? ORDER BY event_ts DESC "
               "LIMIT 3", (plant["id"],)) if plant else []
    full, date, hms, unix = utils.bj_now()
    return {"ok": True,
            "time": {"unix_beijing": unix, "date": date, "time": hms},
            "config": {"upload_interval_s": config.UPLOAD_INTERVAL_S,
                       "offline_after_s": config.OFFLINE_AFTER_S},
            "plant": None if not plant else {
                "id": plant["id"], "name": plant["name"],
                "health_score": plant["health_score"],
                "mood": plant["mood"], "health_level": plant["health_level"]},
            "tasks": tasks, "recent_events": evs,
            "latest_telemetry_ts": last["ts"] if last else None,
            # 待执行指令（自动浇水/补光…）。板卡可以直接用这个字段，
            # 也可以单独轮询 GET /actuators/pending，两边内容一样。
            "pending_actions": _pending_actions(claims["sub"])}

def _pending_actions(device_id):
    """把排队中的执行指令带给板卡（取走即标 sent，避免重复执行）。"""
    try:
        from ..services import actuator_ops
        return actuator_ops.pending_for_device(device_id)
    except Exception as e:
        print("[actuator] pending skipped: %s" % e)
        return []


# 顶层 /sync 别名（规格 §10：设备轮询 GET /sync）
sync_router = APIRouter(tags=["设备同步"])
sync_router.get("/sync")(device_sync)
