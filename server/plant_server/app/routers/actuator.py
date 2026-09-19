# -*- coding: utf-8 -*-
"""执行器接口（自动执行层）。App / 板卡 / 管理台三个端都走这里。

    App   ：GET /actuators?plant_id= 看状态；POST /actuators/{id}/mode 开关自动；
            POST /actuators/{id}/run 立刻浇一次
    板卡  ：POST /actuators/capabilities 声明有几路；GET /actuators/pending 取指令；
            POST /actuators/jobs/{job_id}/ack 回执
    管理台：见 app/admin.py 的 /admin/actuators*
"""
from fastapi import APIRouter, Header, HTTPException
from pydantic import BaseModel

from .. import config, db, utils
from ..services import actuator_ops, billing_ops
from . import _claims_device, _claims_user, resolve_plant

router = APIRouter(prefix="/actuators", tags=["自动执行"])


class ModeIn(BaseModel):
    mode: str = "auto"          # auto 自动 / off 关闭 / manual 只手动


class RunIn(BaseModel):
    duration_s: int = 0
    reason: str = ""


class CapIn(BaseModel):
    actuators: list = None      # [{"kind":"water","name":"水泵"}, ...]


class AckIn(BaseModel):
    ok: bool = True
    detail: str = ""


# ── 板卡侧（先声明，避免被下面的 /{actuator_id} 吃掉路径）────────────

@router.post("/capabilities")
def declare_capabilities(body: CapIn, x_device_token: str = Header(None)):
    """板卡声明自己带哪几路执行器（幂等，可反复调）。"""
    claims = _claims_device(x_device_token)
    did = claims["sub"]
    dev = db.one("SELECT * FROM devices WHERE id=?", (did,))
    if not dev:
        raise HTTPException(404, "设备不存在")
    plant = db.one("SELECT * FROM plants WHERE device_id=?", (did,))
    made = actuator_ops.ensure_for_device(did, plant["id"] if plant else None,
                                          body.actuators)
    return {"ok": True, "actuators": [{"id": a["id"], "kind": a["kind"],
                                       "name": a["name"], "mode": a["mode"]}
                                      for a in made]}


@router.get("/pending")
def pending(x_device_token: str = Header(None)):
    """板卡轮询取待执行指令。"""
    claims = _claims_device(x_device_token)
    jobs = actuator_ops.pending_for_device(claims["sub"])
    return {"ok": True, "jobs": jobs}


@router.post("/jobs/{job_id}/ack")
def ack_job(job_id: str, body: AckIn, x_device_token: str = Header(None)):
    """板卡执行完回执。"""
    claims = _claims_device(x_device_token)
    job = actuator_ops.ack(job_id, claims["sub"], body.ok, body.detail)
    if not job:
        raise HTTPException(404, "没有这条指令")
    return {"ok": True, "job": job}


# ── App 侧 ────────────────────────────────────────────────────────

@router.get("")
def list_actuators(plant_id: str = "", authorization: str = Header(None)):
    claims = _claims_user(authorization)
    plant = resolve_plant(claims, "user", plant_id or None)
    out = actuator_ops.summary(plant["id"])
    out["device_bound"] = bool(plant.get("device_id"))
    out["can_auto"] = billing_ops.has_feature(claims["sub"], "auto_execute")
    if not out["device_bound"]:
        out["hint"] = "这盆还没绑定板卡，自动执行要等设备接上才会真的动"
    return {"ok": True, **out}


@router.post("/{actuator_id}/mode")
def set_mode(actuator_id: str, body: ModeIn,
             authorization: str = Header(None)):
    claims = _claims_user(authorization)
    a = db.one("SELECT * FROM actuators WHERE id=?", (actuator_id,))
    if not a:
        raise HTTPException(404, "没有这个执行器")
    plant = resolve_plant(claims, "user", a["plant_id"])
    if body.mode not in ("auto", "off", "manual"):
        raise HTTPException(400, "mode 只能是 auto / off / manual")
    if body.mode == "auto" and not billing_ops.has_feature(claims["sub"],
                                                           "auto_execute"):
        raise HTTPException(403, "自动执行是 PRO 及以上会员功能")
    db.exe("UPDATE actuators SET mode=?, updated_at=? WHERE id=?",
           (body.mode, utils.bj_now()[0], actuator_id))
    return {"ok": True, "actuator": db.one("SELECT * FROM actuators WHERE id=?",
                                           (actuator_id,))}


@router.post("/{actuator_id}/run")
def run_now(actuator_id: str, body: RunIn, authorization: str = Header(None)):
    """App 里点「立即浇水」：立刻下发一条指令（不受自动开关影响）。"""
    claims = _claims_user(authorization)
    a = db.one("SELECT * FROM actuators WHERE id=?", (actuator_id,))
    if not a:
        raise HTTPException(404, "没有这个执行器")
    plant = resolve_plant(claims, "user", a["plant_id"])
    if not plant.get("device_id"):
        raise HTTPException(400, "这盆还没绑定板卡，指令没人执行")
    job = actuator_ops.trigger(a, "run", body.duration_s or None,
                               body.reason or "在 App 里手动执行",
                               source="app", actor=claims["sub"])
    return {"ok": True, "job": job,
            "note": "指令已排队，板卡下一次心跳（约 60 秒内）取走后执行"}
