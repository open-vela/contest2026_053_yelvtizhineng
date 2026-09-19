# -*- coding: utf-8 -*-
from fastapi import APIRouter, Header, HTTPException, Query
from pydantic import BaseModel

from .. import db, utils
from ..services import plant_ops
from . import either, resolve_plant

router = APIRouter(prefix="/plants", tags=["植物"])


class PlantIn(BaseModel):
    name: str = "小绿绿"
    species: str = "绿萝"
    device_id: str = ""
    location: str = ""


class PlantPatch(BaseModel):
    name: str = None
    species: str = None
    location: str = None
    avatar_media_id: str = None


@router.get("")
def list_plants(authorization: str = Header(None),
                x_device_token: str = Header(None)):
    claims, kind = either(authorization, x_device_token)
    rows = db.q("SELECT * FROM plants WHERE user_id=? ORDER BY created_at",
                (claims["sub"],)) if kind == "user" else db.q(
        "SELECT * FROM plants WHERE device_id=?", (claims["sub"],))
    out = []
    for p in rows:
        g = plant_ops.growth_summary(p["id"], p["user_id"])
        out.append({"id": p["id"], "name": p["name"], "species": p["species"],
                    "health_score": p["health_score"], "mood": p["mood"],
                    "health_level": p["health_level"],
                    "device_id": p["device_id"],
                    "location": p["location"], "started_at": p["started_at"],
                    "avatar_media_id": p["avatar_media_id"],
                    "growth": g["total_growth"], "badges": len(g["badges"])})
    return {"ok": True, "plants": out}


@router.post("")
def create_plant(body: PlantIn, authorization: str = Header(None)):
    claims, _ = either(authorization, None)
    pid = utils.new_id("pl")
    db.exe("INSERT INTO plants(id,user_id,device_id,species,name,started_at,"
           " location,health_score,mood,health_level,updated_at,created_at)"
           " VALUES(?,?,?,?,?,?,?,?,?,?,?,?)",
           (pid, claims["sub"], body.device_id or None, body.species,
            body.name, utils.bj_now()[1], body.location or "客厅",
            80, "😊", "良好", utils.bj_now()[0],
            utils.bj_now()[0]))
    plant_ops.ensure_today_tasks(pid)
    return {"ok": True, "plant": db.one("SELECT * FROM plants WHERE id=?",
                                        (pid,))}


@router.get("/{plant_id}")
def plant_detail(plant_id: str, authorization: str = Header(None),
                 x_device_token: str = Header(None)):
    claims, kind = either(authorization, x_device_token)
    p = resolve_plant(claims, kind, plant_id)
    g = plant_ops.growth_summary(p["id"], p["user_id"])
    last = db.one("SELECT * FROM telemetry WHERE plant_id=? ORDER BY ts DESC "
                  "LIMIT 8", (p["id"],))
    today = plant_ops.today_tasks(p["id"])
    return {"ok": True, "plant": p, "growth": g,
            "recent_telemetry": last,
            "tasks_today": {"open": len([t for t in today if t["status"] == "open"]),
                            "done": len([t for t in today if t["status"] == "done"])}}


@router.patch("/{plant_id}")
def update_plant(plant_id: str, body: PlantPatch,
                 authorization: str = Header(None)):
    claims, _ = either(authorization, None)
    p = resolve_plant(claims, "user", plant_id)
    sets, vals = [], []
    for k in ("name", "species", "location", "avatar_media_id"):
        v = getattr(body, k)
        if v is not None:
            sets.append("%s=?" % k)
            vals.append(v)
    if sets:
        sets.append("updated_at=?")
        vals.append(utils.bj_now()[0])
        vals.append(plant_id)
        db.exe("UPDATE plants SET %s WHERE id=?" % ",".join(sets), vals)
    return {"ok": True, "plant": db.one("SELECT * FROM plants WHERE id=?",
                                        (plant_id,))}

@router.get("/{plant_id}/telemetry")
def plant_telemetry(plant_id: str, days: int = Query(7, ge=1, le=90),
                    authorization: str = Header(None),
                    x_device_token: str = Header(None)):
    claims, kind = either(authorization, x_device_token)
    p = resolve_plant(claims, kind, plant_id)
    since = utils.date_add(utils.bj_now()[1], -days) + " 00:00:00"
    rows = db.q("SELECT * FROM telemetry WHERE plant_id=? AND ts>=? "
                "ORDER BY ts ASC", (p["id"], since))
    rows = rows[-2000:]
    # 按日聚合均值（App/设备 7 天趋势图数据源）
    from collections import OrderedDict
    day = OrderedDict()
    for r in rows:
        d = r["ts"][:10]
        b = day.setdefault(d, {"moisture": [], "temp": [], "light": [], "ec": []})
        for k in ("moisture", "temp", "light", "ec"):
            if r.get(k) is not None:
                b[k].append(r[k])
    series = {"dates": list(day.keys()), "moisture": [], "temp": [],
              "light": [], "ec": []}
    for d, b in day.items():
        for k in ("moisture", "temp", "light", "ec"):
            series[k].append(round(sum(b[k]) / len(b[k]), 1) if b[k] else None)
    return {"ok": True, "plant_id": p["id"], "days": days,
            "raw_count": len(rows), "series": series}