# -*- coding: utf-8 -*-
from fastapi import APIRouter, Header, HTTPException, Query
from pydantic import BaseModel

from .. import db, utils
from ..services import plant_ops
from . import either, resolve_plant

router = APIRouter(prefix="/tasks", tags=["任务"])
growth_router = APIRouter(tags=["成长"])


class TaskAdd(BaseModel):
    plant_id: str = None
    content: str
    date: str = None


@router.get("/today")
def today_tasks(plant_id: str = Query(None),
                authorization: str = Header(None),
                x_device_token: str = Header(None)):
    claims, kind = either(authorization, x_device_token)
    plant = resolve_plant(claims, kind, plant_id)
    tasks = plant_ops.ensure_today_tasks(plant["id"])
    return {"ok": True, "plant_id": plant["id"], "date": utils.bj_now()[1],
            "tasks": tasks}


@router.post("")
def add_task(body: TaskAdd, authorization: str = Header(None)):
    claims, _ = either(authorization, None)
    plant = resolve_plant(claims, "user", body.plant_id)
    date = body.date or utils.bj_now()[1]
    if not (body.content or "").strip():
        raise HTTPException(400, "任务内容不能为空")
    tid = utils.new_id("t")
    db.exe("INSERT INTO tasks(id,plant_id,date,template_id,content,source,"
           " status,growth_delta) VALUES(?,?,?,?,?,?,?,?)",
           (tid, plant["id"], date, None, body.content.strip(), "manual",
            "open", 5))
    return {"ok": True, "task": db.one("SELECT * FROM tasks WHERE id=?",
                                       (tid,))}


@router.post("/{task_id}/complete")
def complete_task(task_id: str, authorization: str = Header(None),
                  x_device_token: str = Header(None)):
    claims, kind = either(authorization, x_device_token)
    by = claims["sub"]
    task, rc = plant_ops.complete_task(task_id, by)
    if not task:
        raise HTTPException(404, "任务不存在")
    return {"ok": True, "idempotent": rc == "already_done", "task": task}


@growth_router.get("/growth")
def growth(plant_id: str = Query(None), authorization: str = Header(None),
           x_device_token: str = Header(None)):
    claims, kind = either(authorization, x_device_token)
    plant = resolve_plant(claims, kind, plant_id)
    g = plant_ops.growth_summary(plant["id"], claims["sub"])
    return {"ok": True, "plant_id": plant["id"], "growth": g}