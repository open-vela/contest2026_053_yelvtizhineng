# -*- coding: utf-8 -*-
"""植小伴 · 服务器管理台（网页版后端）。

设计原则（2026-09-11 第一版）：
- **只加不改**：不动任何现有路由与表结构。管理台走 /admin 页面与
  /api/v1/admin/* 接口，设备链路（/devices、/voice、/image/analyze）零影响。
- **只读为主**：除查询外只有两个动作——「重跑一次 AI 诊断」（排查用）与
  「下载数据库备份」，都不删改业务数据。
- **鉴权独立**：口令来自环境变量 PLANT_ADMIN_PASSWORD，签发 role=admin 的
  签名 token 放 HttpOnly Cookie，与设备 token、家长账号完全隔离。
  云端部署前务必：改口令、挂 HTTPS、按需加 IP 白名单（见 README 说明）。
"""
import os
import shutil
import sqlite3
import time

from fastapi import (APIRouter, Body, Cookie, Depends, Header, HTTPException,
                     Query, Request, Response)
from fastapi.responses import FileResponse

from . import config, db, security, utils
from .services import media_store

router = APIRouter(prefix="/admin", tags=["管理台"])

DEFAULT_ADMIN_PASSWORD = os.environ.get("PLANT_ADMIN_PASSWORD", "请设置 PLANT_ADMIN_PASSWORD")
ADMIN_PASSWORD = os.environ.get("PLANT_ADMIN_PASSWORD", DEFAULT_ADMIN_PASSWORD)
COOKIE_NAME = "plant_admin"
SERVER_START = time.time()
LOG_DIR = os.path.join(config.DATA_DIR, "logs")


# ── 小工具 ────────────────────────────────────────────────────────────
def _now():
    return utils.bj_now()


def _count(sql, params=()):
    row = db.one(sql, params)
    return int(list(row.values())[0]) if row else 0


def _online(dev):
    """沿用设备模块同一套判定（OFFLINE_AFTER_S 内有一次心跳即在线）。"""
    if not dev or not dev.get("last_seen_at"):
        return False
    try:
        last = time.mktime(time.strptime(dev["last_seen_at"],
                                         "%Y-%m-%d %H:%M:%S"))
        return (time.time() - last) <= config.OFFLINE_AFTER_S
    except Exception:
        return False


def _abs_path(p):
    """历史数据里可能存着别的机器的绝对路径，统一交给 media_store 处理。"""
    return media_store.resolve_path(p) or ""


def _media_url(mid):
    return ("/api/v1/media/%s/file" % mid) if mid else None


def _dir_size(path):
    total = 0
    if not os.path.isdir(path):
        return 0
    for root, _dirs, files in os.walk(path):
        for f in files:
            try:
                total += os.path.getsize(os.path.join(root, f))
            except OSError:
                pass
    return total


def _downsample(rows, max_points):
    if len(rows) <= max_points:
        return rows
    step = len(rows) // max_points + 1
    out = rows[::step]
    if out and out[-1] is not rows[-1]:
        out.append(rows[-1])
    return out


def _auth_token(request, cookie, authorization):
    if authorization:
        tok = authorization
        if tok.lower().startswith("bearer "):
            tok = tok[7:]
        return tok.strip()
    if cookie:
        return cookie
    return (request.query_params.get("token") or "").strip()


def require_admin(request: Request, plant_admin: str = Cookie(None),
                  authorization: str = Header(None)):
    """管理台统一鉴权：Cookie / Authorization: Bearer / ?token= 皆可。"""
    tok = _auth_token(request, plant_admin, authorization)
    claims = security.verify_token(tok) if tok else None
    if not claims or claims.get("role") != "admin":
        raise HTTPException(401, "请先登录管理台")
    return claims


# ── 登录 ──────────────────────────────────────────────────────────────
@router.post("/login")
def admin_login(response: Response, body: dict = Body(...)):
    pwd = str(body.get("password") or "")
    if not pwd or pwd != ADMIN_PASSWORD:
        time.sleep(0.4)          # 放慢暴力尝试
        raise HTTPException(401, "口令不正确")
    token = security.create_token("admin", "admin")
    response.set_cookie(COOKIE_NAME, token, httponly=True, samesite="lax",
                        max_age=config.TOKEN_TTL_DAYS * 86400, path="/")
    return {"ok": True, "role": "admin"}


@router.post("/logout")
def admin_logout(response: Response):
    response.delete_cookie(COOKIE_NAME, path="/")
    return {"ok": True}


@router.get("/me")
def admin_me(request: Request, plant_admin: str = Cookie(None),
             authorization: str = Header(None)):
    tok = _auth_token(request, plant_admin, authorization)
    claims = security.verify_token(tok) if tok else None
    return {"ok": True,
            "logged_in": bool(claims and claims.get("role") == "admin"),
            "password_is_default": ADMIN_PASSWORD == DEFAULT_ADMIN_PASSWORD,
            "server_time": _now()[0]}


# ── 服务重启（管理台按钮） ────────────────────────────────────────────
@router.post("/service/restart")
def service_restart(request: Request, claims=Depends(require_admin)):
    """重启服务器进程：先回响应，再由游离看护进程「杀旧起新」。"""
    from .services import service_ctl

    ok, msg = service_ctl.restart_later(host=request.url.hostname,
                                        port=request.url.port,
                                        who=str(claims.get("sub") or "admin"))
    if not ok:
        raise HTTPException(500, msg)
    return {"ok": True, "restarting": True, "pid": os.getpid(),
            "message": msg, "wait_s": 15}


# ── 总览 ──────────────────────────────────────────────────────────────
@router.get("/overview")
def overview(_=Depends(require_admin)):
    from .services.ai_gateway import gateway

    today = _now()[1]
    du = shutil.disk_usage(config.DATA_DIR)
    devices = db.q("SELECT * FROM devices ORDER BY created_at")
    for d in devices:
        d["online"] = _online(d)
        d["plant_n"] = _count("SELECT COUNT(*) n FROM plants WHERE device_id=?",
                              (d["id"],))
    plants = db.q("SELECT id,name,species,health_score,mood,health_level,"
                  "device_id FROM plants ORDER BY created_at")
    recent = db.q("SELECT e.id,e.type,e.title,e.summary,e.event_ts,e.source,"
                  "e.media_id,p.name plant_name FROM events e "
                  "LEFT JOIN plants p ON p.id=e.plant_id "
                  "ORDER BY e.event_ts DESC LIMIT 12")
    last_tel = db.q("SELECT * FROM telemetry ORDER BY ts DESC LIMIT 1")
    return {
        "ok": True,
        # 在线判定策略（管理台文案直接用，避免前端写死数字跟 config 漂移）
        "device_policy": {
            "heartbeat_s": config.DEVICE_HEARTBEAT_S,
            "offline_after_s": config.OFFLINE_AFTER_S,
        },
        "service": {
            "version": "0.1.0",
            "ai_mode": gateway.mode,
            "provider": gateway.provider,
            "model": gateway.model,
            "db_path": config.DB_PATH,
            "media_dir": config.MEDIA_DIR,
            "data_bytes": _dir_size(config.DATA_DIR),
            "disk_total": du.total,
            "disk_free": du.free,
            "db_bytes": os.path.getsize(config.DB_PATH)
            if os.path.exists(config.DB_PATH) else 0,
            "uptime_s": int(time.time() - SERVER_START),
            "pid": os.getpid(),
            "server_time": _now()[0],
            "admin_password_is_default":
                ADMIN_PASSWORD == DEFAULT_ADMIN_PASSWORD,
        },
        "counts": {
            "devices": _count("SELECT COUNT(*) n FROM devices"),
            "devices_online": _count("SELECT COUNT(*) n FROM devices") and
            sum(1 for d in devices if d["online"]),
            "plants": _count("SELECT COUNT(*) n FROM plants"),
            "users": _count("SELECT COUNT(*) n FROM users"),
            "events": _count("SELECT COUNT(*) n FROM events"),
            "media": _count("SELECT COUNT(*) n FROM media"),
            "telemetry": _count("SELECT COUNT(*) n FROM telemetry"),
            "messages": _count("SELECT COUNT(*) n FROM messages"),
            "ai_jobs": _count("SELECT COUNT(*) n FROM ai_jobs"),
        },
        "today": {
            "ai_jobs": _count("SELECT COUNT(*) n FROM ai_jobs WHERE "
                              "created_at LIKE ?", (today + "%",)),
            "ai_quota": config.AI_DAILY_QUOTA,
            "tasks": _count("SELECT COUNT(*) n FROM tasks WHERE date=?",
                            (today,)),
            "tasks_done": _count("SELECT COUNT(*) n FROM tasks WHERE date=? "
                                 "AND status='done'", (today,)),
            "events": _count("SELECT COUNT(*) n FROM events WHERE event_ts "
                             "LIKE ?", (today + "%",)),
        },
        "devices": devices,
        "plants": plants,
        "recent_events": recent,
        "latest_telemetry": last_tel[0] if last_tel else None,
    }


# ── 设备 ──────────────────────────────────────────────────────────────
@router.get("/devices")
def list_devices(_=Depends(require_admin)):
    rows = db.q("SELECT * FROM devices ORDER BY created_at")
    for d in rows:
        d["online"] = _online(d)
        d["plants"] = db.q("SELECT id,name,species,health_score FROM plants "
                           "WHERE device_id=?", (d["id"],))
        d["last_telemetry"] = db.one(
            "SELECT * FROM telemetry WHERE device_id=? ORDER BY ts DESC "
            "LIMIT 1", (d["id"],))
        d["telemetry_n"] = _count("SELECT COUNT(*) n FROM telemetry WHERE "
                                  "device_id=?", (d["id"],))
        d["event_n"] = _count("SELECT COUNT(*) n FROM events WHERE device_id=?",
                              (d["id"],))
    return {"ok": True, "devices": rows}


# ── 植物 ──────────────────────────────────────────────────────────────
@router.get("/plants")
def list_plants(_=Depends(require_admin)):
    rows = db.q("SELECT * FROM plants ORDER BY created_at")
    for p in rows:
        p["device"] = db.one("SELECT id,sn,model,fw_version,last_seen_at "
                             "FROM devices WHERE id=?", (p["device_id"],))
        p["growth"] = _count("SELECT COALESCE(SUM(delta),0) n FROM growth_logs "
                             "WHERE plant_id=?", (p["id"],))
        p["badge_n"] = _count("SELECT COUNT(*) n FROM badges WHERE plant_id=?",
                              (p["id"],))
        p["event_n"] = _count("SELECT COUNT(*) n FROM events WHERE plant_id=?",
                              (p["id"],))
        p["media_n"] = _count("SELECT COUNT(*) n FROM media WHERE plant_id=?",
                              (p["id"],))
        p["msg_n"] = _count("SELECT COUNT(*) n FROM messages m JOIN "
                            "conversations c ON c.id=m.conversation_id "
                            "WHERE c.plant_id=?", (p["id"],))
        p["last_telemetry"] = db.one("SELECT * FROM telemetry WHERE plant_id=? "
                                     "ORDER BY ts DESC LIMIT 1", (p["id"],))
        p["tasks_done"] = _count("SELECT COUNT(*) n FROM tasks WHERE plant_id=? "
                                 "AND status='done'", (p["id"],))
        p["tasks_total"] = _count("SELECT COUNT(*) n FROM tasks WHERE "
                                  "plant_id=?", (p["id"],))
    return {"ok": True, "plants": rows}


@router.get("/plants/{plant_id}")
def plant_detail(plant_id: str, days: int = Query(7, ge=1, le=90),
                 _=Depends(require_admin)):
    p = db.one("SELECT * FROM plants WHERE id=?", (plant_id,))
    if not p:
        raise HTTPException(404, "植物不存在")
    since = utils.date_add(_now()[1], -(days - 1)) + " 00:00:00"
    tel = db.q("SELECT ts,moisture,temp,light,ec,ph,salt,nitrogen,phosphorus,"
               "potassium,source FROM telemetry "
               "WHERE plant_id=? AND ts>=? ORDER BY ts", (plant_id, since))
    evs = db.q("SELECT * FROM events WHERE plant_id=? ORDER BY event_ts DESC "
               "LIMIT 50", (plant_id,))
    for e in evs:
        e["media_url"] = _media_url(e["media_id"])
    return {"ok": True, "plant": p,
            "device": db.one("SELECT * FROM devices WHERE id=?",
                             (p["device_id"],)),
            "telemetry": tel,
            "events": evs,
            "tasks": db.q("SELECT * FROM tasks WHERE plant_id=? ORDER BY date "
                          "DESC, template_id LIMIT 40", (plant_id,)),
            "badges": db.q("SELECT * FROM badges WHERE plant_id=? ORDER BY "
                           "awarded_at", (plant_id,)),
            "growth": db.q("SELECT * FROM growth_logs WHERE plant_id=? ORDER BY "
                           "created_at DESC LIMIT 30", (plant_id,)),
            "reports": db.q("SELECT * FROM reports WHERE plant_id=? ORDER BY "
                            "created_at DESC LIMIT 5", (plant_id,))}


# ── 遥测 ──────────────────────────────────────────────────────────────
@router.get("/telemetry")
def telemetry_series(plant_id: str = Query(""), days: int = Query(7, ge=1,
                                                                  le=90),
                     max_points: int = Query(600, ge=50, le=5000),
                     _=Depends(require_admin)):
    since = utils.date_add(_now()[1], -(days - 1)) + " 00:00:00"
    sql = ("SELECT ts,moisture,temp,light,ec,ph,salt,nitrogen,phosphorus,"
           "potassium,source FROM telemetry "
           "WHERE ts>=?")
    params = [since]
    if plant_id:
        sql += " AND plant_id=?"
        params.append(plant_id)
    sql += " ORDER BY ts"
    rows = db.q(sql, tuple(params))
    return {"ok": True, "days": days, "total": len(rows),
            "rows": _downsample(rows, max_points)}


# ── 任务 / 事件 ───────────────────────────────────────────────────────
@router.get("/tasks")
def list_tasks(date: str = Query(""), _=Depends(require_admin)):
    d = date or _now()[1]
    rows = db.q("SELECT t.*, p.name plant_name FROM tasks t LEFT JOIN plants p "
                "ON p.id=t.plant_id WHERE t.date=? "
                "ORDER BY t.status, p.name, t.template_id", (d,))
    return {"ok": True, "date": d, "tasks": rows,
            "templates": db.q("SELECT * FROM task_templates")}


@router.get("/events")
def list_events(plant_id: str = Query(""), days: int = Query(30, ge=1,
                                                             le=365),
                limit: int = Query(100, ge=1, le=500),
                _=Depends(require_admin)):
    since = utils.date_add(_now()[1], -(days - 1)) + " 00:00:00"
    sql = ("SELECT e.*, p.name plant_name FROM events e LEFT JOIN plants p "
           "ON p.id=e.plant_id WHERE e.event_ts>=?")
    params = [since]
    if plant_id:
        sql += " AND e.plant_id=?"
        params.append(plant_id)
    sql += " ORDER BY e.event_ts DESC LIMIT ?"
    params.append(limit)
    rows = db.q(sql, tuple(params))
    for r in rows:
        r["media_url"] = _media_url(r["media_id"])
    return {"ok": True, "events": rows}


# ── 对话与语音 ────────────────────────────────────────────────────────
@router.get("/conversations")
def list_conversations(_=Depends(require_admin)):
    rows = db.q(
        "SELECT c.*, p.name plant_name, "
        "(SELECT COUNT(*) FROM messages m WHERE m.conversation_id=c.id) msg_n, "
        "(SELECT MAX(ts) FROM messages m WHERE m.conversation_id=c.id) last_ts, "
        "(SELECT text FROM messages m WHERE m.conversation_id=c.id "
        " ORDER BY m.rowid DESC LIMIT 1) last_text "
        "FROM conversations c LEFT JOIN plants p ON p.id=c.plant_id "
        "ORDER BY c.updated_at DESC")
    return {"ok": True, "conversations": rows}


@router.get("/conversations/{cid}")
def conversation_detail(cid: str, _=Depends(require_admin)):
    c = db.one("SELECT * FROM conversations WHERE id=?", (cid,))
    if not c:
        raise HTTPException(404, "会话不存在")
    msgs = db.q("SELECT * FROM messages WHERE conversation_id=? "
                "ORDER BY rowid", (cid,))
    for m in msgs:
        m["audio_url"] = _media_url(m.get("audio_media_id"))
    return {"ok": True, "conversation": c, "messages": msgs}


# ── 媒体库 ────────────────────────────────────────────────────────────
@router.get("/media")
def list_media(kind: str = Query(""), plant_id: str = Query(""),
               days: int = Query(30, ge=1, le=3650),
               limit: int = Query(200, ge=1, le=1000),
               _=Depends(require_admin)):
    since = utils.date_add(_now()[1], -(days - 1)) + " 00:00:00"
    sql = ("SELECT m.*, p.name plant_name FROM media m LEFT JOIN plants p "
           "ON p.id=m.plant_id WHERE m.created_at>=?")
    params = [since]
    if kind:
        sql += " AND m.kind=?"
        params.append(kind)
    if plant_id:
        sql += " AND m.plant_id=?"
        params.append(plant_id)
    sql += " ORDER BY m.created_at DESC LIMIT ?"
    params.append(limit)
    rows = db.q(sql, tuple(params))
    total = 0
    for r in rows:
        r["url"] = _media_url(r["id"])
        r["bytes"] = (os.path.getsize(_abs_path(r["path"]))
                      if r.get("path") and os.path.exists(_abs_path(r["path"]))
                      else 0)
        total += r["bytes"]
    return {"ok": True, "media": rows, "bytes": total}


# ── AI 调试 ───────────────────────────────────────────────────────────
@router.get("/ai_jobs")
def list_ai_jobs(status: str = Query(""), limit: int = Query(100, ge=1,
                                                             le=500),
                 _=Depends(require_admin)):
    sql = ("SELECT id,job_type,status,provider,model,request_source,"
           "input_ref,error,created_at,length(COALESCE(structured_json,'')) "
           "AS sj_len FROM ai_jobs")
    params = []
    if status:
        sql += " WHERE status=?"
        params.append(status)
    sql += " ORDER BY created_at DESC, rowid DESC LIMIT ?"
    params.append(limit)
    jobs = db.q(sql, tuple(params))
    return {"ok": True, "jobs": jobs,
            "stats": {
                "total": _count("SELECT COUNT(*) n FROM ai_jobs"),
                "ok": _count("SELECT COUNT(*) n FROM ai_jobs WHERE "
                             "status='ok'"),
                "empty": _count("SELECT COUNT(*) n FROM ai_jobs WHERE "
                                "status='empty'"),
                "error": _count("SELECT COUNT(*) n FROM ai_jobs WHERE "
                                "status='error'"),
                "today": _count("SELECT COUNT(*) n FROM ai_jobs WHERE "
                                "created_at LIKE ?", (_now()[1] + "%",)),
            }}


@router.get("/ai_jobs/{job_id}")
def ai_job_detail(job_id: str, _=Depends(require_admin)):
    job = db.one("SELECT * FROM ai_jobs WHERE id=?", (job_id,))
    if not job:
        raise HTTPException(404, "任务不存在")
    job["media_url"] = None
    if job.get("input_ref"):
        m = db.one("SELECT id,kind,path,created_at FROM media WHERE id=?",
                   (job["input_ref"],))
        if m:
            job["media"] = m
            job["media_url"] = _media_url(m["id"])
    return {"ok": True, "job": job}


@router.post("/ai_jobs/{job_id}/rerun")
def ai_job_rerun(job_id: str, _=Depends(require_admin)):
    """对同一条输入重跑诊断（排查"识别失败"用；会写一条新 ai_jobs 与体检事件）。"""
    job = db.one("SELECT * FROM ai_jobs WHERE id=?", (job_id,))
    if not job:
        raise HTTPException(404, "任务不存在")
    if job["job_type"] != "diagnose":
        raise HTTPException(400, "只有图像诊断可以重跑")
    media = db.one("SELECT * FROM media WHERE id=?", (job["input_ref"],))
    if not media:
        raise HTTPException(404, "原始图片记录已不存在")
    plant = db.one("SELECT * FROM plants WHERE id=?", (media["plant_id"],)) \
        or db.one("SELECT * FROM plants ORDER BY created_at LIMIT 1")
    if not plant:
        raise HTTPException(400, "还没有植物，无法重跑")
    from .routers.ai import run_diagnose
    res = run_diagnose(media, plant, job["request_source"] or "user")
    return {"ok": True, "new_job_id": res.get("diagnose_id"),
            "recognized": res.get("recognized"),
            "result": res.get("result")}


# ── 日志 ──────────────────────────────────────────────────────────────
def _log_candidates():
    out = []
    for d in (LOG_DIR, config.BASE_DIR):
        if not os.path.isdir(d):
            continue
        for name in sorted(os.listdir(d)):
            if name.endswith(".log") and name not in out:
                out.append(name)
    return out


def _log_path(name):
    for d in (LOG_DIR, config.BASE_DIR):
        p = os.path.join(d, name)
        if os.path.isfile(p):
            return p
    return None


def _tail(path, n):
    with open(path, "rb") as f:
        f.seek(0, os.SEEK_END)
        size = f.tell()
        data = b""
        block = 16384
        while size > 0 and data.count(b"\n") <= n:
            step = min(block, size)
            size -= step
            f.seek(size)
            data = f.read(step) + data
    lines = data.decode("utf-8", "replace").splitlines()
    return lines[-n:]


@router.get("/logs")
def read_logs(lines: int = Query(200, ge=10, le=5000),
              file: str = Query(""), _=Depends(require_admin)):
    files = _log_candidates()
    name = file or ("server.log" if "server.log" in files
                    else (files[0] if files else ""))
    if not name:
        return {"ok": True, "files": [], "file": "", "lines": [],
                "note": "还没有日志文件"}
    path = _log_path(name)
    if not path:
        raise HTTPException(404, "日志文件不存在: %s" % name)
    st = os.stat(path)
    return {"ok": True, "files": files, "file": name,
            "size": st.st_size,
            "mtime": time.strftime("%Y-%m-%d %H:%M:%S",
                                   time.localtime(st.st_mtime)),
            "lines": _tail(path, lines)}


# ── 运维：数据库在线备份 ──────────────────────────────────────────────
def _prune_backups(keep=5):
    try:
        items = [f for f in os.listdir(LOG_DIR)
                 if f.startswith("plant-backup-") and f.endswith(".db")]
        items.sort()
        for f in items[:-keep]:
            os.remove(os.path.join(LOG_DIR, f))
    except OSError:
        pass


# ── 自动执行层：执行器看板（2026-09-12）──────────────────────────────
#   管理台能看全站执行器状态、手动补一次浇水/补光、临时关掉"自动"。
#   注意：这里下发的是指令，真正动作要等板卡下一次心跳（约 60 秒）来取。
@router.get("/actuators")
def actuators_overview(_=Depends(require_admin)):
    from .services import actuator_ops
    return {"ok": True,
            "actuators": actuator_ops.all_actuators(),
            "jobs": actuator_ops.recent_jobs(30),
            "kinds": [{"key": k, "label": v[0]}
                      for k, v in actuator_ops.KINDS.items()]}


@router.post("/actuators/{actuator_id}/run")
def actuator_run(actuator_id: str, body: dict = Body(None),
                 _=Depends(require_admin)):
    from .services import actuator_ops
    body = body or {}
    a = db.one("SELECT * FROM actuators WHERE id=?", (actuator_id,))
    if not a:
        raise HTTPException(404, "没有这个执行器")
    job = actuator_ops.trigger(a, "run", body.get("duration_s") or None,
                               body.get("reason") or "管理台手动下发",
                               source="admin")
    return {"ok": True, "job": job,
            "note": "已排队，板卡下次心跳取走后执行"}


@router.post("/actuators/{actuator_id}/mode")
def actuator_mode(actuator_id: str, body: dict = Body(None),
                  _=Depends(require_admin)):
    body = body or {}
    mode = str(body.get("mode") or "")
    if mode not in ("auto", "off", "manual"):
        raise HTTPException(400, "mode 只能是 auto / off / manual")
    a = db.one("SELECT * FROM actuators WHERE id=?", (actuator_id,))
    if not a:
        raise HTTPException(404, "没有这个执行器")
    db.exe("UPDATE actuators SET mode=?, updated_at=? WHERE id=?",
           (mode, _now()[0], actuator_id))
    return {"ok": True, "actuator": db.one("SELECT * FROM actuators WHERE id=?",
                                           (actuator_id,))}


# ── 会员订阅看板（2026-09-12）────────────────────────────────────────
@router.get("/subscriptions")
def subscriptions_overview(_=Depends(require_admin)):
    from .services import billing_ops
    rows = billing_ops.all_subscriptions()
    for r in rows:
        r["days_left"] = billing_ops._days_left(r.get("expires_at"))
        r["plan_name"] = billing_ops.plan_of(r.get("plan"))["name"]
    total = sum(1 for r in rows if r.get("status") == "active" and r["days_left"] >= 0)
    mrr = sum(billing_ops.plan_of(r.get("plan"))["price_cents"] for r in rows
              if r.get("status") == "active" and r["days_left"] >= 0)
    return {"ok": True, "subscriptions": rows, "active": total,
            "mrr_cents": mrr, "plans": billing_ops.PLANS,
            "addons": billing_ops.ADDONS,
            "deposit_cents": billing_ops.DEPOSIT_CENTS,
            "enforce": config.BILLING_ENFORCE}


@router.get("/backup/db")
def backup_db(_=Depends(require_admin)):
    os.makedirs(LOG_DIR, exist_ok=True)
    stamp = time.strftime("%Y%m%d-%H%M%S")
    dst_path = os.path.join(LOG_DIR, "plant-backup-%s.db" % stamp)
    src = sqlite3.connect(config.DB_PATH)
    dst = sqlite3.connect(dst_path)
    with dst:
        src.backup(dst)
    dst.close()
    src.close()
    _prune_backups()
    return FileResponse(dst_path, media_type="application/octet-stream",
                        filename=os.path.basename(dst_path))
