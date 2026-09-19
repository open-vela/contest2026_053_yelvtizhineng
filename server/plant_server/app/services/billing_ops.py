# -*- coding: utf-8 -*-
"""会员订阅 + 押金：商业模式的落地骨架。

路演讲稿里的商业模式是「设备收押金 99 元 + 订阅 19.9/29.9/49.9 元每月，
满 12 个月退押金」，保险 +9.9 元/月。这里把**状态和权益**做实，
支付通道留成显式的占位（subscribe 直接落库），真上线时：

    1) 把 POST /billing/subscribe 改成"下单 + 调支付"，
    2) 支付回调里调 billing_ops.apply_payment(user_id, plan, months, txn_id)，
    3) 打开 PLANT_BILLING_ENFORCE=1，高级功能（自动执行）就会按套餐卡住。

演示期 PLANT_BILLING_ENFORCE 默认关，不影响现有演示流程。
"""
import datetime

from .. import config, db, utils

# 三档订阅（价格与路演讲稿一致，单位：分）
PLANS = [
    {"key": "basic", "name": "BASIC", "price_cents": 1990, "per": "月",
     "tagline": "够用的智能看护",
     "features": ["土壤 + 环境监测", "AI 养护建议", "植物识别", "成长日记"]},
    {"key": "pro", "name": "PRO", "price_cents": 2990, "per": "月",
     "recommend": True, "tagline": "会自动浇水的那档",
     "features": ["基础版全部", "自动浇水 / 补光", "病虫害诊断", "社区大赛"]},
    {"key": "family", "name": "FAMILY", "price_cents": 4990, "per": "月",
     "tagline": "多设备 + 家庭共享",
     "features": ["专业版全部", "多设备支持", "家庭共享", "绿植保险"]},
]
PLAN_MAP = {p["key"]: p for p in PLANS}
ADDONS = [{"key": "insurance", "name": "绿植保险", "price_cents": 990, "per": "月",
           "tagline": "养死包赔，免费补发新苗",
           "features": ["死亡免费补发同款", "传感器数据自动理赔"]}]
DEPOSIT_CENTS = 9900          # 硬件押金 99 元
DEPOSIT_REFUND_MONTHS = 12    # 满 12 个月退还

# 套餐权益（用于门禁）：谁能用自动执行
PRO_FEATURES = {"auto_execute": ("pro", "family"),
                "insurance": ("family",),
                "multi_device": ("family",)}
_RANK = {"basic": 1, "pro": 2, "family": 3}


def _days_left(expires_at):
    if not expires_at:
        return 0
    try:
        exp = datetime.datetime.strptime(expires_at[:10], "%Y-%m-%d").date()
    except Exception:
        return 0
    return (exp - datetime.date.today()).days


def get(user_id):
    return db.one("SELECT * FROM subscriptions WHERE user_id=?", (user_id,))


def plan_of(key):
    return PLAN_MAP.get(key or "", PLAN_MAP["basic"])


def ensure(user_id, plan="basic"):
    """幂等：没有就建一条（演示期用户注册即送 BASIC）。"""
    s = get(user_id)
    if s:
        return s
    now = utils.bj_now()
    sid = utils.new_id("sub")
    db.exe("INSERT INTO subscriptions(id,user_id,plan,status,started_at,"
           " expires_at,auto_renew,deposit_cents,deposit_status,created_at,"
           " updated_at) VALUES(?,?,?,?,?,?,?,?,?,?,?)",
           (sid, user_id, plan, "active", now[0],
            (datetime.date.today() + datetime.timedelta(days=30)).isoformat(),
            1, 0, "none", now[0], now[0]))
    return get(user_id)


def subscribe(user_id, plan="pro", months=1):
    """开通/续期。真上线时这一步由支付回调触发（见模块 docstring）。"""
    if plan not in PLAN_MAP:
        plan = "pro"
    months = max(1, min(int(months or 1), 36))
    s = ensure(user_id)
    today = datetime.date.today()
    base = today
    # 同一个套餐续费 → 从原到期日往后顺延；换套餐（升级/降级）→ 从今天重新算，
    # 不然"先用免费 BASIC 再升 PRO"会把赠送的天数白白叠上去。
    if s.get("expires_at") and s.get("plan") == plan:
        try:
            cur = datetime.datetime.strptime(s["expires_at"][:10], "%Y-%m-%d").date()
            if cur > today:
                base = cur
        except Exception:
            pass
    exp = base + datetime.timedelta(days=30 * months)
    now = utils.bj_now()[0]
    db.exe("UPDATE subscriptions SET plan=?, status='active', expires_at=?,"
           " auto_renew=1, updated_at=? WHERE user_id=?",
           (plan, exp.isoformat(), now, user_id))
    plant_ops_add_event(user_id, "订阅", "开通 %s 会员（%d 个月）"
                        % (plan_of(plan)["name"], months))
    return get(user_id)


def cancel(user_id):
    s = ensure(user_id)
    db.exe("UPDATE subscriptions SET auto_renew=0, updated_at=? WHERE user_id=?",
           (utils.bj_now()[0], user_id))
    return get(user_id)


def pay_deposit(user_id, cents=DEPOSIT_CENTS):
    """收硬件押金（演示期直接记账，真上线走支付）。"""
    s = ensure(user_id)
    db.exe("UPDATE subscriptions SET deposit_cents=?, deposit_status='held',"
           " updated_at=? WHERE user_id=?", (cents, utils.bj_now()[0], user_id))
    return get(user_id)


def active_plan(user_id):
    s = get(user_id)
    if not s:
        return None
    if s["status"] != "active" or _days_left(s["expires_at"]) < 0:
        return None
    return s["plan"]


def entitled(user_id, feature):
    """这个账号的套餐本身有没有这项权益（不看演示期开关）。App 用它显示"PRO 功能"。"""
    allow = PRO_FEATURES.get(feature)
    if not allow:
        return True
    cur = active_plan(user_id)
    return bool(cur) and cur in allow


def has_feature(user_id, feature):
    """真正的门禁：enforce 关掉时一律放行（演示期默认关，免得演示到一半被拦住）。"""
    if not config.BILLING_ENFORCE:
        return True
    return entitled(user_id, feature)


def summary(user_id):
    s = ensure(user_id)
    days = _days_left(s["expires_at"])
    if days >= 0 and s["status"] == "active":
        state = "active"
    elif days >= 0:
        state = s["status"]
    else:
        state = "expired"
    plan = plan_of(s["plan"])
    return {"subscription": {
        "plan": s["plan"], "plan_name": plan["name"],
        "price_cents": plan["price_cents"], "status": state,
        "started_at": s["started_at"], "expires_at": s["expires_at"],
        "days_left": max(0, days), "auto_renew": bool(s["auto_renew"]),
        "deposit_cents": s["deposit_cents"],
        "deposit_status": s["deposit_status"],
        "deposit_refund_months": DEPOSIT_REFUND_MONTHS,
    }, "plans": PLANS, "addons": ADDONS,
        "enforce": config.BILLING_ENFORCE,
        "note": "演示期开通即生效，未接支付通道"}


def plant_ops_add_event(user_id, title, summary=""):
    """订阅变动记一条，App 的成长日记里也能看到（失败不影响主流程）。"""
    try:
        from . import plant_ops
        p = db.one("SELECT id FROM plants WHERE user_id=? ORDER BY created_at"
                   " LIMIT 1", (user_id,))
        if p:
            plant_ops.add_event(p["id"], "subscribe", title=title,
                                summary=summary, source="server",
                                user_id=user_id)
    except Exception:
        pass


def all_subscriptions():
    """管理台用。"""
    return db.q("SELECT s.*, u.nickname, u.phone FROM subscriptions s"
                " LEFT JOIN users u ON u.id=s.user_id ORDER BY s.updated_at DESC")


def ensure_demo_subscription():
    """演示账号默认是 PRO（而且要能看到"押金已交"），不然演示自动执行会被门禁拦住。"""
    for u in db.q("SELECT id, nickname FROM users WHERE demo=1"):
        s = get(u["id"])
        if not s:
            subscribe(u["id"], "pro", 12)
            pay_deposit(u["id"])
    return True
