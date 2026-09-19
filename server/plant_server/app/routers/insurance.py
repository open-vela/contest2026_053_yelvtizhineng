# -*- coding: utf-8 -*-
"""绿植保险 + 一键理赔。

差异化就在这一块：理赔不是"填表 → 等人工 → 三天后回话"，而是
**自动取证 + 秒级预审**。服务器把这盆植物的传感器历史、AI 体检记录、
照片、养护打卡率、设备在线情况一次性拉出来当证据，当场算预审分，
直接给结论和赔付建议。用户只需要：选植物 → 选原因 → 点一下。

状态机（claims.status）：
  submitted 已提交 → auto_pass 免人工（即 approved）
                   → manual   转人工复核（reviewing）
                   → need_more_info 材料不足
  approved → 用户确认补发 → shipped
"""
import json

from fastapi import APIRouter, Header, HTTPException, Query, Request
from pydantic import BaseModel

from .. import db, ratelimit, utils
from ..services import plant_ops
from . import _claims_user

router = APIRouter(prefix="/insurance", tags=["绿植保险"])

PLAN = {
    "key": "demo",
    "name": "植小伴基础保障",
    "desc": "一盆绿植 · 保额 49 元 · 一年内养死免费补发",
    "coverage_cents": 4900,
    "days": 365,
}

REASONS = [
    ("died", "养死了", "🥀"),
    ("wilt", "枯萎 / 烂根", "😵"),
    ("yellow", "叶片发黄", "🍂"),
    ("pest", "虫害", "🐛"),
    ("stunt", "长期不长", "🐌"),
    ("other", "其他情况", "❓"),
]
REASON_NAME = {k: v for (k, v, _i) in REASONS}

# 预审阈值
PASS_SCORE = 75      # 以上：证据齐全，免人工直接通过
MANUAL_SCORE = 50    # 以上：转人工复核；以下：材料不足


class PolicyIn(BaseModel):
    plant_id: str


class ClaimIn(BaseModel):
    plant_id: str
    reason: str = "other"
    description: str = ""
    media_ids: list = []


# ────────────── 保单 ──────────────

def ensure_policy(uid, plant_id):
    """演示期：每盆植物自动带一份保障，不用用户掏钱也不用点投保。
    真上线时这一步改成"下单支付后创建保单"即可，其余逻辑不用动。"""
    p = db.one("SELECT * FROM policies WHERE plant_id=? AND status='active'",
               (plant_id,))
    if p:
        return p
    now = utils.bj_now()
    pid = utils.new_id("pol")
    db.exe("INSERT INTO policies(id,user_id,plant_id,plan,coverage_cents,"
           " status,started_at,expires_at,created_at) VALUES(?,?,?,?,?,?,?,?,?)",
           (pid, uid, plant_id, PLAN["key"], PLAN["coverage_cents"], "active",
            now[1], utils.date_add(now[1], PLAN["days"]), now[0]))
    return db.one("SELECT * FROM policies WHERE id=?", (pid,))


def _policy_out(pol, plant):
    claims = db.q("SELECT id FROM claims WHERE policy_id=? AND status NOT IN "
                  "('rejected')", (pol["id"],))
    return {
        "id": pol["id"], "plan": pol["plan"], "plan_name": PLAN["name"],
        "coverage_cents": pol["coverage_cents"],
        "coverage_text": "¥%.2f" % (pol["coverage_cents"] / 100.0),
        "status": pol["status"], "started_at": pol["started_at"],
        "expires_at": pol["expires_at"],
        "plant_id": pol["plant_id"],
        "plant_name": (plant or {}).get("name") or "我的植物",
        "plant_mood": (plant or {}).get("mood") or "🌿",
        "claimed": len(claims) > 0,
    }


@router.get("/summary")
def summary(authorization: str = Header(None)):
    uid = _claims_user(authorization)["sub"]
    plants = db.q("SELECT * FROM plants WHERE user_id=? ORDER BY created_at",
                  (uid,))
    pols = []
    for p in plants:
        pols.append(_policy_out(ensure_policy(uid, p["id"]), p))
    claims = db.q("SELECT * FROM claims WHERE user_id=? ORDER BY created_at DESC"
                  " LIMIT 20", (uid,))
    active = len([c for c in claims if c["status"] in ("submitted", "reviewing",
                                                       "need_more_info")])
    return {"ok": True, "plan": PLAN, "reasons": [
        {"key": k, "name": n, "icon": i} for (k, n, i) in REASONS],
        "policies": pols,
        "coverage_total_text": "¥%.2f" % (sum(
            p["coverage_cents"] for p in pols) / 100.0),
        "claim_active": active,
        "claims": [_claim_out(c) for c in claims]}


@router.post("/policies")
def create_policy(body: PolicyIn, authorization: str = Header(None)):
    uid = _claims_user(authorization)["sub"]
    plant = db.one("SELECT * FROM plants WHERE id=? AND user_id=?",
                   (body.plant_id, uid))
    if not plant:
        raise HTTPException(404, "植物不存在")
    return {"ok": True, "policy": _policy_out(ensure_policy(uid, plant["id"]),
                                              plant)}


# ────────────── 一键理赔：自动取证 ──────────────

def collect_evidence(plant, plant_id):
    """把这盆植物"到底怎么了"用服务器已有的数据讲清楚。"""
    ev = {}
    ev["plant"] = {"name": plant["name"], "species": plant["species"],
                   "health_score": plant["health_score"],
                   "health_level": plant["health_level"],
                   "started_at": plant["started_at"]}

    tel = db.q("SELECT * FROM telemetry WHERE plant_id=? ORDER BY ts DESC "
               "LIMIT 240", (plant_id,))
    ev["telemetry_count"] = len(tel)
    ev["telemetry_span"] = ([tel[-1]["ts"], tel[0]["ts"]] if tel else [])
    moist = [t["moisture"] for t in tel if t.get("moisture") is not None]
    if moist:
        low = len([m for m in moist if m < 25])
        ev["moisture"] = {"min": round(min(moist), 1), "max": round(max(moist), 1),
                          "avg": round(sum(moist) / len(moist), 1),
                          "low_ratio": round(low / float(len(moist)), 2)}
    temp = [t["temp"] for t in tel if t.get("temp") is not None]
    if temp:
        ev["temp"] = {"min": round(min(temp), 1), "max": round(max(temp), 1)}

    photos = db.q("SELECT id, created_at FROM media WHERE plant_id=? AND "
                  "kind='image' ORDER BY created_at DESC LIMIT 20", (plant_id,))
    ev["photo_count"] = len(photos)
    ev["last_photo_at"] = photos[0]["created_at"] if photos else None
    ev["latest_photo_id"] = photos[0]["id"] if photos else None

    jobs = db.q("SELECT j.created_at, j.status, j.structured_json FROM ai_jobs j"
                " JOIN media m ON m.id = j.input_ref"
                " WHERE m.plant_id=? AND j.job_type='diagnose'"
                " ORDER BY j.created_at DESC LIMIT 5", (plant_id,))
    ev["diagnose_count"] = len(jobs)
    ev["last_diagnose_at"] = jobs[0]["created_at"] if jobs else None
    last_score = None
    for j in jobs:
        try:
            sc = (json.loads(j["structured_json"] or "{}") or {}).get("health_score")
            if sc is not None:
                last_score = int(sc)
                break
        except Exception:
            continue
    ev["last_diagnose_score"] = last_score

    total = db.one("SELECT COUNT(*) n FROM tasks WHERE plant_id=?", (plant_id,))["n"]
    done = db.one("SELECT COUNT(*) n FROM tasks WHERE plant_id=? AND "
                  "status='done'", (plant_id,))["n"]
    ev["task_total"] = total
    ev["task_done"] = done
    ev["task_rate"] = (round(done / float(total), 2) if total else None)

    dev = (db.one("SELECT sn,status,last_seen_at FROM devices WHERE id=?",
                  (plant["device_id"],)) if plant.get("device_id") else None)
    ev["device"] = dev
    return ev


def auto_review(ev, reason):
    """规则化预审：给出分数、结论和"为什么"。分数是给人看的，理由才是关键。"""
    score = 40
    notes = []

    if ev["photo_count"]:
        score += 12
        notes.append("有 %d 张照片可以核验现状" % ev["photo_count"])
        lp = ev.get("last_photo_at") or ""
        if lp[:4] and (utils.bj_now()[1] >= lp[:10]):
            # 最近 7 天内有照片再加分
            days = _days_between(lp[:10], utils.bj_now()[1])
            if days <= 7:
                score += 8
                notes.append("今天刚拍过照片" if days == 0
                             else "最近 %d 天内有拍照记录" % days)
    else:
        score -= 20
        notes.append("没有照片，无法核验植物现状")

    if ev["diagnose_count"]:
        score += 8
        notes.append("有 %d 次 AI 体检记录" % ev["diagnose_count"])
        sc = ev.get("last_diagnose_score")
        if sc is not None:
            if sc < 60:
                score += 12
                notes.append("最近一次 AI 体检健康分只有 %d，状态确实不好" % sc)
            elif sc >= 80:
                score -= 8
                notes.append("最近一次体检健康分 %d，状态还不错，请补充说明" % sc)
    else:
        notes.append("还没有做过 AI 体检（少一份佐证）")

    if ev["telemetry_count"]:
        score += 8
        notes.append("近 30 天有 %d 条传感器数据" % ev["telemetry_count"])
        m = ev.get("moisture")
        if m and m.get("low_ratio", 0) >= 0.3:
            score += 8
            notes.append("有 %d%% 的时间土壤水分低于 25%%，长期缺水" %
                         int(m["low_ratio"] * 100))
    else:
        score -= 10
        notes.append("没有传感器数据，无法判断环境")

    tr = ev.get("task_rate")
    if tr is not None:
        if tr >= 0.5:
            score += 10
            notes.append("养护任务完成率 %d%%，照料是认真的" % int(tr * 100))
        elif tr < 0.2:
            score -= 6
            notes.append("养护任务完成率只有 %d%%，请说明原因" % int(tr * 100))

    score = max(0, min(100, score))
    if score >= PASS_SCORE:
        return score, "auto_pass", "证据齐全，免人工审核", notes
    if score >= MANUAL_SCORE:
        return score, "manual", "资料基本齐了，需要人工复核一下", notes
    return score, "need_more_info", "材料还不够，补一张现状照片会快很多", notes


def _days_between(d1, d2):
    import time as _t
    try:
        a = _t.mktime(_t.strptime(d1, "%Y-%m-%d"))
        b = _t.mktime(_t.strptime(d2, "%Y-%m-%d"))
        return int(abs(b - a) / 86400)
    except Exception:
        return 0


def _claim_out(c):
    try:
        tl = json.loads(c.get("timeline_json") or "[]")
    except Exception:
        tl = []
    try:
        ev = json.loads(c.get("evidence_json") or "{}")
    except Exception:
        ev = {}
    return {
        "id": c["id"], "claim_no": c["claim_no"],
        "plant_id": c["plant_id"], "reason": c["reason"],
        "reason_name": REASON_NAME.get(c["reason"], "其他情况"),
        "description": c["description"], "status": c["status"],
        "auto_verdict": c["auto_verdict"], "auto_score": c["auto_score"],
        "auto_note": c["auto_note"],
        "payout_cents": c["payout_cents"],
        "payout_text": ("¥%.2f" % (c["payout_cents"] / 100.0)) if c["payout_cents"] else "",
        "evidence": ev, "timeline": tl,
        "created_at": c["created_at"], "updated_at": c["updated_at"],
    }


@router.post("/claims")
def create_claim(body: ClaimIn, request: Request,
                 authorization: str = Header(None)):
    """一键理赔：提交即完成取证与预审，返回结论。"""
    uid = _claims_user(authorization)["sub"]
    ratelimit.hit(request, "insurance_claim", 10, 3600)
    plant = db.one("SELECT * FROM plants WHERE id=? AND user_id=?",
                   (body.plant_id, uid))
    if not plant:
        raise HTTPException(404, "植物不存在")
    reason = body.reason if body.reason in REASON_NAME else "other"
    desc = (body.description or "").strip()[:300]
    if reason == "other" and not desc:
        raise HTTPException(400, "选个原因，或者用一句话说说情况")

    policy = ensure_policy(uid, plant["id"])
    pending = db.one("SELECT id,claim_no FROM claims WHERE plant_id=? AND "
                     "status IN ('submitted','reviewing','need_more_info')",
                     (plant["id"],))
    if pending:
        raise HTTPException(400, "这盆植物已经有一张理赔单在处理中（%s）"
                            % pending["claim_no"])
    # 同一保障期内已经赔过（approved/shipped）还能再报，但不再免人工 ——
    # 否则换个原因就能反复拿赔付。
    paid = db.one("SELECT claim_no FROM claims WHERE plant_id=? AND "
                  "status IN ('approved','shipped') ORDER BY created_at DESC"
                  " LIMIT 1", (plant["id"],))

    ev = collect_evidence(plant, plant["id"])
    score, verdict, note, notes = auto_review(ev, reason)
    if paid:
        verdict = "manual"
        note = "本保障期内已经赔付过一次（%s），这次需要人工复核" % paid["claim_no"]
        notes = notes + ["本次不是首次理赔"]

    media_ids = [str(x) for x in (body.media_ids or [])][:3]
    if not media_ids and ev.get("latest_photo_id"):
        # 一键的意思：没传照片就用这盆植物最近那张，用户不用自己翻相册
        media_ids = [ev["latest_photo_id"]]
    for mid in media_ids:
        m = db.one("SELECT id,owner_user_id FROM media WHERE id=?", (mid,))
        if not m or m["owner_user_id"] != uid:
            raise HTTPException(403, "照片不对")

    now = utils.bj_now()
    cid = utils.new_id("cl")
    claim_no = "ZXB%s%04d" % (now[1].replace("-", ""), abs(hash(cid)) % 10000)
    status = {"auto_pass": "approved", "manual": "reviewing",
              "need_more_info": "need_more_info"}[verdict]
    payout = PLAN["coverage_cents"] if status == "approved" else 0
    timeline = [{"ts": now[0], "text": "提交理赔申请"},
                {"ts": now[0], "text": "自动取证完成：%s" % note}]
    if status == "approved":
        timeline.append({"ts": now[0], "text": "预审通过，赔付方案已生成"})

    db.exe("INSERT INTO claims(id,claim_no,policy_id,user_id,plant_id,reason,"
           " description,media_ids,evidence_json,auto_verdict,auto_score,"
           " auto_note,status,payout_cents,timeline_json,created_at,updated_at)"
           " VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)",
           (cid, claim_no, policy["id"], uid, plant["id"], reason, desc,
            json.dumps(media_ids), json.dumps(ev, ensure_ascii=False), verdict,
            score, note, status, payout,
            json.dumps(timeline, ensure_ascii=False), now[0], now[0]))

    plant_ops.add_event(plant["id"], "claim", title="发起理赔（%s）" % claim_no,
                        summary="%s｜%s" % (REASON_NAME.get(reason, ""), note),
                        source="app", delta=0, user_id=uid)
    out = _claim_out(db.one("SELECT * FROM claims WHERE id=?", (cid,)))
    out["notes"] = notes
    return {"ok": True, "claim": out}


@router.get("/claims")
def list_claims(authorization: str = Header(None)):
    uid = _claims_user(authorization)["sub"]
    rows = db.q("SELECT * FROM claims WHERE user_id=? ORDER BY created_at DESC",
                (uid,))
    return {"ok": True, "claims": [_claim_out(c) for c in rows]}


@router.get("/claims/{claim_id}")
def claim_detail(claim_id: str, authorization: str = Header(None)):
    uid = _claims_user(authorization)["sub"]
    c = db.one("SELECT * FROM claims WHERE id=? AND user_id=?", (claim_id, uid))
    if not c:
        raise HTTPException(404, "理赔单不存在")
    return {"ok": True, "claim": _claim_out(c)}


@router.post("/claims/{claim_id}/accept")
def accept_claim(claim_id: str, authorization: str = Header(None)):
    """接受赔付方案 → 进入补发。演示期直接置为已补发。"""
    uid = _claims_user(authorization)["sub"]
    c = db.one("SELECT * FROM claims WHERE id=? AND user_id=?", (claim_id, uid))
    if not c:
        raise HTTPException(404, "理赔单不存在")
    if c["status"] != "approved":
        raise HTTPException(400, "这张理赔单还没通过审核，暂时不能补发")
    now = utils.bj_now()[0]
    tl = json.loads(c["timeline_json"] or "[]")
    tl.append({"ts": now, "text": "用户确认赔付，新苗已安排补发"})
    db.exe("UPDATE claims SET status='shipped', updated_at=?, timeline_json=?"
           " WHERE id=?", (now, json.dumps(tl, ensure_ascii=False), claim_id))
    plant = db.one("SELECT * FROM plants WHERE id=?", (c["plant_id"],))
    if plant:
        plant_ops.add_event(plant["id"], "insurance", title="理赔完成：已补发新苗",
                            summary="理赔单 %s" % c["claim_no"], source="app",
                            delta=0, user_id=uid)
    return {"ok": True, "claim": _claim_out(db.one("SELECT * FROM claims WHERE "
                                                   "id=?", (claim_id,)))}


@router.post("/claims/{claim_id}/advance")
def advance_claim(claim_id: str, authorization: str = Header(None)):
    """演示用：把"转人工复核"的单子模拟成审核通过。
    真上线时这一步由后台客服系统调用，App 里会去掉这个按钮。"""
    uid = _claims_user(authorization)["sub"]
    c = db.one("SELECT * FROM claims WHERE id=? AND user_id=?", (claim_id, uid))
    if not c:
        raise HTTPException(404, "理赔单不存在")
    if c["status"] != "reviewing":
        raise HTTPException(400, "这张单子当前不在人工复核中")
    now = utils.bj_now()[0]
    tl = json.loads(c["timeline_json"] or "[]")
    tl.append({"ts": now, "text": "人工复核通过（演示：模拟审核）"})
    db.exe("UPDATE claims SET status='approved', payout_cents=?, updated_at=?,"
           " timeline_json=? WHERE id=?",
           (PLAN["coverage_cents"], now,
            json.dumps(tl, ensure_ascii=False), claim_id))
    return {"ok": True, "claim": _claim_out(db.one("SELECT * FROM claims WHERE "
                                                   "id=?", (claim_id,)))}


@router.get("/preview")
def preview(plant_id: str = Query(...), authorization: str = Header(None)):
    """提交前先看看服务器手上已经有哪些证据（让"自动取证"看得见）。"""
    uid = _claims_user(authorization)["sub"]
    plant = db.one("SELECT * FROM plants WHERE id=? AND user_id=?",
                   (plant_id, uid))
    if not plant:
        raise HTTPException(404, "植物不存在")
    ev = collect_evidence(plant, plant["id"])
    score, verdict, note, notes = auto_review(ev, "other")
    return {"ok": True, "evidence": ev, "preview_score": score,
            "preview_verdict": verdict, "preview_note": note, "notes": notes}