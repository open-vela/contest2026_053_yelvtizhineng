# -*- coding: utf-8 -*-
from fastapi import APIRouter, Header
from pydantic import BaseModel

from .. import db, utils
from ..services import plant_ops
from . import _claims_device

router = APIRouter(prefix="/telemetry", tags=["传感器"])


class TeleIn(BaseModel):
    ts: str = None          # "YYYY-MM-DD HH:MM:SS"，缺省=服务器当前
    moisture: float = None
    temp: float = None
    light: float = None
    ec: float = None
    ph: float = None
    salt: float = None
    nitrogen: float = None
    phosphorus: float = None
    potassium: float = None
    items: list = None      # 批量：[{ts,moisture,temp,light,ec,ph,salt,nitrogen,phosphorus,potassium}, ...]


def _plant_of_device(device_id):
    plant = db.one("SELECT * FROM plants WHERE device_id=?", (device_id,))
    if not plant:
        dev = db.one("SELECT * FROM devices WHERE id=?", (device_id,))
        if dev and dev["owner_user_id"]:
            plant = db.one("SELECT * FROM plants WHERE user_id=? "
                           "ORDER BY created_at LIMIT 1",
                           (dev["owner_user_id"],))
    if not plant:
        # 多终端：新板卡第一包数据可能早于 App 绑定，别丢（自动建一盆）
        from . import auto_plant_for_device
        plant = auto_plant_for_device(device_id)
    return plant


TEL_FIELDS = ("moisture", "temp", "light", "ec", "ph",
              "salt", "nitrogen", "phosphorus", "potassium")


def _insert(device_id, plant_id, ts, rec):
    db.exe("INSERT OR IGNORE INTO telemetry(id,device_id,plant_id,ts,"
           + ",".join(TEL_FIELDS) + ",source) VALUES("
           + ",".join(["?"] * (5 + len(TEL_FIELDS))) + ")",
           (utils.new_id("tm"), device_id, plant_id, ts)
           + tuple(rec.get(k) for k in TEL_FIELDS) + ("device",))


@router.post("/put")
def ingest(body: TeleIn, x_device_token: str = Header(None)):
    """单条或批量上报；同一设备同一 ts 幂等。"""
    claims = _claims_device(x_device_token)
    plant = _plant_of_device(claims["sub"])
    if not plant:
        return {"ok": False, "error": "设备尚未绑定植物", "saved": 0}
    now = utils.bj_now()
    saved = 0
    if body.items:
        items = body.items
    else:
        single = {k: getattr(body, k) for k in TEL_FIELDS}
        single["ts"] = body.ts
        items = [single]
    last = None
    for i, it in enumerate(items):
        ts = (it.get("ts") or now[0]) if it.get("ts") else now[0]
        rec = {k: it.get(k) for k in TEL_FIELDS}
        before = db.one("SELECT id FROM telemetry WHERE device_id=? AND ts=?",
                        (claims["sub"], ts))
        _insert(claims["sub"], plant["id"], ts, rec)
        if not before:
            saved += 1
            last = rec
    # soil-wifi 无心跳：遥测到达即刷新在线时间（管理台 MQTT 页用）
    try:
        db.exe(
            "UPDATE devices SET last_seen_at=?, status='online' "
            "WHERE id=?",
            (utils.bj_now()[0], claims["sub"]),
        )
    except Exception as e:
        print("[telemetry] soil last_seen skip:", e)

    health = None
    if last:
        score, mood, level = plant_ops.apply_telemetry(plant["id"], last)
        health = {"score": score, "mood": mood, "level": level}
    # 自动执行层：土壤水分偏低就自动排队浇一次（有冷却与每日上限，见 actuator_ops）
    fired = []
    try:
        from ..services import actuator_ops
        fired = actuator_ops.auto_check(plant["id"], last)
    except Exception as e:
        print("[actuator] auto_check skipped: %s" % e)
    return {"ok": True, "saved": saved, "plant_id": plant["id"],
            "health": health,
            "auto_actions": len(fired)}


@router.get("/latest")
def telemetry_latest(x_device_token: str = Header(None)):
    """嵌入式拉最新土壤八项（轻量，避免 devices/sync 大包截断）。"""
    claims = _claims_device(x_device_token)
    plant = _plant_of_device(claims["sub"])
    if not plant:
        return {"ok": False, "latest_telemetry": None}
    last = db.one(
        "SELECT ts,moisture,temp,ec,ph,salt,nitrogen,phosphorus,potassium,"
        "source FROM telemetry WHERE plant_id=? ORDER BY ts DESC LIMIT 1",
        (plant["id"],),
    )
    return {"ok": True, "latest_telemetry": last}
