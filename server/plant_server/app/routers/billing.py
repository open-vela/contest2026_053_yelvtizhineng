# -*- coding: utf-8 -*-
"""会员订阅 / 押金接口。

演示期：POST /billing/subscribe 直接开通（没接支付）。
真上线：把 subscribe 换成"下单 + 支付"，支付成功回调
        billing_ops.subscribe(...) 即可，字段和权益判定都不用改。
"""
from fastapi import APIRouter, Header
from pydantic import BaseModel

from .. import db
from ..services import billing_ops
from . import _claims_user

router = APIRouter(prefix="/billing", tags=["会员订阅"])


class SubIn(BaseModel):
    plan: str = "pro"
    months: int = 1


@router.get("/plans")
def plans():
    """套餐表（不登录也能看，方便 App 在登录页先展示）。"""
    return {"ok": True, "plans": billing_ops.PLANS, "addons": billing_ops.ADDONS,
            "deposit_cents": billing_ops.DEPOSIT_CENTS,
            "deposit_refund_months": billing_ops.DEPOSIT_REFUND_MONTHS}


@router.get("/summary")
def summary(authorization: str = Header(None)):
    claims = _claims_user(authorization)
    j = billing_ops.summary(claims["sub"])
    # 顺带把这盆植物的自动执行权益说清楚，App 好显示"这是 PRO 功能"
    plants = db.q("SELECT id, name FROM plants WHERE user_id=? ORDER BY created_at",
                  (claims["sub"],))
    j["plants"] = plants
    j["entitlements"] = {
        "auto_execute": billing_ops.entitled(claims["sub"], "auto_execute"),
        "insurance": billing_ops.entitled(claims["sub"], "insurance")}
    return {"ok": True, **j}


@router.post("/subscribe")
def subscribe(body: SubIn, authorization: str = Header(None)):
    claims = _claims_user(authorization)
    s = billing_ops.subscribe(claims["sub"], body.plan, body.months)
    return {"ok": True, "subscription": s,
            "note": "演示期直接生效；上线后这里改成支付回调"}


@router.post("/cancel")
def cancel(authorization: str = Header(None)):
    claims = _claims_user(authorization)
    return {"ok": True, "subscription": billing_ops.cancel(claims["sub"])}


@router.post("/deposit")
def deposit(authorization: str = Header(None)):
    """收/退硬件押金（99 元）。演示期直接记账。"""
    claims = _claims_user(authorization)
    return {"ok": True,
            "subscription": billing_ops.pay_deposit(claims["sub"])}
