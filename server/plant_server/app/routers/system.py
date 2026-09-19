# -*- coding: utf-8 -*-
import os

from fastapi import APIRouter, Header

from .. import config, db, utils
from ..services.ai_gateway import gateway
from . import touch_device_token

router = APIRouter(tags=["系统"])


@router.get("/time")
def get_time(x_device_token: str = Header(None)):
    # 板卡开机/定时对时都带 X-Device-Token，顺手记一次“设备还活着”
    touch_device_token(x_device_token)
    full, date, hms, unix = utils.bj_now()
    return {"ok": True, "unix_beijing": unix, "date": date, "time": hms,
            "zone": "UTC+8"}


@router.get("/health")
def health():
    return {"ok": True, "ai_mode": gateway.mode, "provider": gateway.provider,
            "db": config.DB_PATH, "media_dir": config.MEDIA_DIR,
            "time": utils.bj_now()[0]}


@router.get("/admin/stats")
def admin_stats():
    today = utils.bj_now()[1]
    return {"ok": True,
            "users": db.one("SELECT COUNT(*) n FROM users")["n"],
            "devices": db.one("SELECT COUNT(*) n FROM devices")["n"],
            "plants": db.one("SELECT COUNT(*) n FROM plants")["n"],
            "telemetry": db.one("SELECT COUNT(*) n FROM telemetry")["n"],
            "events": db.one("SELECT COUNT(*) n FROM events")["n"],
            "media": db.one("SELECT COUNT(*) n FROM media")["n"],
            "ai_jobs_today": db.one("SELECT COUNT(*) n FROM ai_jobs WHERE "
                                    "created_at LIKE ?", (today + "%",))["n"],
            "tasks_today": db.one("SELECT COUNT(*) n FROM tasks WHERE date=?",
                                  (today,))["n"]}
