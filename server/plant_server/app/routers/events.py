# -*- coding: utf-8 -*-
from fastapi import APIRouter, Header, HTTPException, Query
from pydantic import BaseModel

from .. import db
from ..services import plant_ops
from . import either, resolve_plant

router = APIRouter(prefix="/events", tags=["日记事件"])


class EventIn(BaseModel):
    plant_id: str = None
    type: str = "note"
    title: str = None
    summary: str = ""
    media_id: str = None
    event_ts: str = None
    delta: int = 0


@router.post("")
def push_event(body: EventIn, authorization: str = Header(None),
               x_device_token: str = Header(None)):
    claims, kind = either(authorization, x_device_token)
    plant = resolve_plant(claims, kind, body.plant_id)
    delta = body.delta if kind == "user" else 0
    ev = plant_ops.add_event(
        plant["id"], body.type, title=body.title, summary=body.summary,
        media_id=body.media_id, source="app" if kind == "user" else "device",
        event_ts=body.event_ts, delta=delta,
        user_id=claims["sub"] if kind == "user" else
        db.one("SELECT owner_user_id FROM plants WHERE id=?",
               (plant["id"],))["owner_user_id"],
        device_id=None if kind == "user" else claims["sub"])
    return {"ok": True, "event": ev}


@router.get("")
def list_events(plant_id: str = Query(None), days: int = Query(30, le=365),
                limit: int = Query(100, le=500),
                authorization: str = Header(None),
                x_device_token: str = Header(None)):
    claims, kind = either(authorization, x_device_token)
    plant = resolve_plant(claims, kind, plant_id)
    rows = db.q("SELECT * FROM events WHERE plant_id=? ORDER BY event_ts DESC "
                "LIMIT ?", (plant["id"], limit))
    return {"ok": True, "plant_id": plant["id"], "count": len(rows),
            "events": rows}