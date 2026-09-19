# -*- coding: utf-8 -*-
"""账号：验证码登录 + 一键演示登录。

公网（阿里云）部署后这些接口人人可访问，因此：
  - 一键演示登录可用 PLANT_DEMO_OPEN=0 关闭
  - 按 IP 限流，防刷 AI 额度
"""
from fastapi import APIRouter, Header, HTTPException, Request
from pydantic import BaseModel

from .. import config, db, ratelimit, security, utils
from . import _claims_user

router = APIRouter(prefix="/auth", tags=["账号"])

DEMO_PHONE = "13800000001"

# 每 IP 限流：(次数, 窗口秒)
DEMO_LIMIT = (20, 3600)
SMS_LIMIT = (10, 3600)
LOGIN_LIMIT = (30, 3600)


class PhoneIn(BaseModel):
    phone: str


class LoginIn(BaseModel):
    phone: str
    code: str


class ProfileIn(BaseModel):
    nickname: str = None
    avatar: str = None
    city: str = None
    bio: str = None


@router.post("/sms")
def send_sms(body: PhoneIn, request: Request):
    ratelimit.hit(request, "auth_sms", *SMS_LIMIT)
    phone = body.phone.strip()
    if not (phone.isdigit() and 6 <= len(phone) <= 20):
        raise HTTPException(400, u"手机号格式不正确")
    now = utils.bj_now()
    db.exe("INSERT INTO sms_codes(phone,code,expires_at,created_at) "
           "VALUES(?,?,?,?) ON CONFLICT(phone) DO UPDATE SET code=excluded.code,"
           " expires_at=excluded.expires_at",
           (phone, config.DEMO_SMS_CODE, now[0], now[0]))
    out = {"ok": True, "expire_min": 10}
    if config.DEMO_SMS_ECHO:
        # 演示期没有真实短信通道：固定验证码直接回显，便于评审自测
        out["dev_code"] = config.DEMO_SMS_CODE
        out["note"] = u"演示环境固定验证码，正式上线请接入短信通道"
    return out


@router.post("/login")
def login(body: LoginIn, request: Request):
    ratelimit.hit(request, "auth_login", *LOGIN_LIMIT)
    phone = body.phone.strip()
    code = body.code.strip()
    row = db.one("SELECT * FROM sms_codes WHERE phone=?", (phone,))
    ok = (code == config.DEMO_SMS_CODE) or (
        row and row["code"] == code and row["expires_at"] >= utils.bj_now()[0])
    if not ok:
        raise HTTPException(401, u"验证码错误或已过期")
    user = db.one("SELECT * FROM users WHERE phone=?", (phone,))
    is_new = False
    if not user:
        # 手机号 + 验证码：第一次登录就是注册，不再单独做注册接口
        is_new = True
        user_id = utils.new_id("u")
        db.exe("INSERT INTO users(id,phone,nickname,role,demo,avatar,created_at) "
               "VALUES(?,?,?,?,?,?,?)",
               (user_id, phone, u"植友" + phone[-4:], "parent", 0, u"\U0001F331",
                utils.bj_now()[0]))
        user = db.one("SELECT * FROM users WHERE id=?", (user_id,))
    return {"ok": True, "is_new": is_new, "guide_profile": is_new,
            "token": security.create_token(user["id"], "user"),
            "user": _user_out(user)}


@router.post("/demo")
def demo_login(request: Request):
    """一键演示账号：评审用，自动建号 + 建设备 + 建植物。"""
    if not config.DEMO_OPEN:
        raise HTTPException(403, u"演示登录已关闭")
    ratelimit.hit(request, "auth_demo", *DEMO_LIMIT)
    from ..services import seed
    seed.ensure_demo()
    user = db.one("SELECT * FROM users WHERE phone=?", (DEMO_PHONE,))
    if not user:
        raise HTTPException(500, u"演示账号初始化失败")
    return {"ok": True, "is_new": False, "guide_profile": False,
            "token": security.create_token(user["id"], "user"),
            "user": _user_out(user)}


# ── 用户资料（多用户之后"我是谁"这件事要在 App 里看得见、改得动）──

def _user_out(u):
    """对外只给必要字段：手机号做脱敏，不外泄。"""
    phone = u.get("phone") or ""
    masked = (phone[:3] + "****" + phone[-4:]) if len(phone) >= 7 else phone
    return {"id": u["id"], "nickname": u.get("nickname") or u"植友",
            "avatar": u.get("avatar") or u"\U0001F331",
            "city": u.get("city") or "", "bio": u.get("bio") or "",
            "phone_masked": masked, "demo": bool(u.get("demo")),
            "created_at": u.get("created_at")}


@router.get("/me")
def me(authorization: str = Header(None)):
    claims = _claims_user(authorization)
    u = db.one("SELECT * FROM users WHERE id=?", (claims["sub"],))
    if not u:
        raise HTTPException(401, u"账号不存在")
    stats = {
        "plants": db.one("SELECT COUNT(*) n FROM plants WHERE user_id=?",
                         (u["id"],))["n"],
        "devices": db.one("SELECT COUNT(*) n FROM devices WHERE owner_user_id=?",
                          (u["id"],))["n"],
        "posts": db.one("SELECT COUNT(*) n FROM posts WHERE user_id=?",
                        (u["id"],))["n"],
        "claims": db.one("SELECT COUNT(*) n FROM claims WHERE user_id=?",
                         (u["id"],))["n"],
        "following": db.one("SELECT COUNT(*) n FROM user_follows WHERE "
                            "follower_id=?", (u["id"],))["n"],
        "followers": db.one("SELECT COUNT(*) n FROM user_follows WHERE "
                            "followee_id=?", (u["id"],))["n"],
    }
    return {"ok": True, "user": _user_out(u), "stats": stats}


@router.patch("/me")
def update_me(body: ProfileIn, authorization: str = Header(None)):
    claims = _claims_user(authorization)
    sets, args = [], []
    if body.nickname is not None:
        n = body.nickname.strip()
        if not (1 <= len(n) <= 16):
            raise HTTPException(400, u"昵称请填 1~16 个字")
        sets.append("nickname=?"); args.append(n)
    if body.avatar is not None:
        a = body.avatar.strip()[:8] or u"\U0001F331"
        sets.append("avatar=?"); args.append(a)
    if body.city is not None:
        sets.append("city=?"); args.append(body.city.strip()[:12])
    if body.bio is not None:
        sets.append("bio=?"); args.append(body.bio.strip()[:40])
    if not sets:
        raise HTTPException(400, u"没有要改的内容")
    args.append(claims["sub"])
    db.exe("UPDATE users SET " + ", ".join(sets) + " WHERE id=?", tuple(args))
    u = db.one("SELECT * FROM users WHERE id=?", (claims["sub"],))
    return {"ok": True, "user": _user_out(u)}


@router.post("/logout")
def logout():
    return {"ok": True}
