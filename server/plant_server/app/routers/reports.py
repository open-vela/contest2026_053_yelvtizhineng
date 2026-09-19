# -*- coding: utf-8 -*-
from fastapi import APIRouter, Header

from .. import db
from ..services import plant_ops
from . import either, resolve_plant

router = APIRouter(prefix="/plants", tags=["健康报告"])


@router.post("/{plant_id}/reports/weekly")
def make_weekly(plant_id: str, authorization: str = Header(None),
                x_device_token: str = Header(None)):
    claims, kind = either(authorization, x_device_token)
    plant = resolve_plant(claims, kind, plant_id)
    rep = plant_ops.weekly_report(plant["id"])
    return {"ok": True, "report": rep}


@router.get("/{plant_id}/reports")
def list_reports(plant_id: str, authorization: str = Header(None),
                 x_device_token: str = Header(None)):
    claims, kind = either(authorization, x_device_token)
    plant = resolve_plant(claims, kind, plant_id)
    rows = db.q("SELECT * FROM reports WHERE plant_id=? ORDER BY created_at "
                "DESC LIMIT 20", (plant["id"],))
    return {"ok": True, "reports": rows}