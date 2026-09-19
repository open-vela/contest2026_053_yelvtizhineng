# -*- coding: utf-8 -*-
"""自动执行层：执行器（水泵/补光灯…）+ 指令队列。

为什么要有这一层：
    「智能养植」前代竞品全挂在"只测不管"——传感器测出来该浇水，然后呢？
    这一层就是把「建议」变成「动作」的那一环：服务器下发指令 → 板卡执行 → 回执。

固件接入约定（预留，照这个实现即可）：
    1) 板卡开机/注册后用 POST /actuators/capabilities 声明带哪几路执行器：
         {"actuators": [{"kind": "water", "name": "水泵"}, {"kind": "light"}]}
       服务器据此建条目，幂等；不声明就一直只有 App 端的样子货。
    2) 板卡按心跳周期取指令：GET /actuators/pending（或 /devices/sync 的
       pending_actions 字段，二选一，内容一样）。
    3) 执行完 POST /actuators/jobs/{job_id}/ack 回执：
         {"ok": true, "detail": "抽水 15 秒"}   —— 失败就 ok=false 带原因。
    4) 指令来源三类：auto（服务器规则，土壤水分低于阈值）、app（用户点一下）、
       admin（管理台）。回执后自动写一条成长日记 events。

执行粒度：现在只有"开 N 秒"（水泵抽 N 秒 ≈ 定量水）。
以后要精确到毫升，在 config_json 里加 flow_ml_per_s 就行，不用改表。
"""
import json

from .. import db, utils
from . import plant_ops

# 执行器种类 → 中文名 + 成长值（做了对植物有益的事，给点成就感）
KINDS = {
    "water": ("浇水", 3),
    "light": ("补光", 2),
    "fan": ("通风", 1),
    "feeder": ("喂食", 3),
    "heat": ("加温", 1),
}
DEFAULT_KIND = "water"

# 默认规则：土壤水分低于 25% 且距上次浇水超过 30 分钟，自动浇 15 秒；一天最多 6 次
DEFAULT_CONFIG = {"moisture_below": 25, "cooldown_min": 30,
                  "duration_s": 15, "daily_max": 6}

# 指令下发后多久没回执算超时（秒）
EXPIRE_AFTER_S = 300


def kind_label(kind):
    return KINDS.get(kind, (kind, 0))[0]


def _cfg(a):
    try:
        c = json.loads(a.get("config_json") or "{}")
    except Exception:
        c = {}
    out = dict(DEFAULT_CONFIG)
    out.update({k: v for k, v in c.items() if v is not None})
    return out


def ensure_for_device(device_id, plant_id, kinds=None):
    """幂等建/修执行器。kinds 为空则按"这台设备有没有绑植物"建一路浇水。"""
    if not kinds:
        kinds = [{"kind": DEFAULT_KIND}]
    made = []
    for k in kinds:
        kind = str(k.get("kind") or DEFAULT_KIND).strip()
        name = str(k.get("name") or kind_label(kind)).strip()
        old = db.one("SELECT * FROM actuators WHERE device_id=? AND kind=?",
                     (device_id, kind))
        now = utils.bj_now()[0]
        if old:
            db.exe("UPDATE actuators SET plant_id=?, name=COALESCE(NULLIF(?,''),name),"
                   " updated_at=? WHERE id=?",
                   (plant_id or old["plant_id"], name, now, old["id"]))
            made.append(db.one("SELECT * FROM actuators WHERE id=?", (old["id"],)))
        else:
            aid = utils.new_id("ac")
            db.exe("INSERT INTO actuators(id,device_id,plant_id,kind,name,mode,"
                   " state,config_json,created_at,updated_at)"
                   " VALUES(?,?,?,?,?,?,?,?,?,?)",
                   (aid, device_id, plant_id, kind, name, "auto", "idle",
                    json.dumps(DEFAULT_CONFIG), now, now))
            made.append(db.one("SELECT * FROM actuators WHERE id=?", (aid,)))
    return made


def ensure_for_plant(plant):
    """这盆植物挂着的设备该有哪些执行器（设备没声明时，默认给一路浇水）。"""
    if not plant or not plant.get("device_id"):
        return []
    return db.q("SELECT * FROM actuators WHERE plant_id=?", (plant["id"],))


def list_for_plant(plant_id):
    return db.q("SELECT * FROM actuators WHERE plant_id=? ORDER BY created_at",
                (plant_id,))


def _today_runs(actuator_id):
    day = utils.bj_now()[1]
    r = db.one("SELECT COUNT(*) n FROM actuator_jobs WHERE actuator_id=?"
               " AND source='auto' AND substr(created_at,1,10)=?", (actuator_id, day))
    return r["n"] if r else 0


def _minutes_since(ts):
    if not ts:
        return 10 ** 6
    try:
        import time
        return (time.time() - time.mktime(time.strptime(ts, "%Y-%m-%d %H:%M:%S"))) / 60.0
    except Exception:
        return 10 ** 6


def trigger(a, action="run", duration_s=None, reason="", source="app", actor=""):
    """下发一条执行指令（排队等板卡来取）。"""
    cfg = _cfg(a)
    dur = int(duration_s if duration_s is not None else cfg.get("duration_s") or 15)
    now = utils.bj_now()[0]
    jid = utils.new_id("aj")
    db.exe("INSERT INTO actuator_jobs(id,actuator_id,device_id,plant_id,kind,"
           " action,duration_s,reason,source,status,created_at)"
           " VALUES(?,?,?,?,?,?,?,?,?,?,?)",
           (jid, a["id"], a["device_id"], a["plant_id"], a["kind"], action,
            dur, reason, source, "pending", now))
    db.exe("UPDATE actuators SET state='running', last_run_by=?, last_run_reason=?,"
           " updated_at=? WHERE id=?", (actor or source, reason, now, a["id"]))
    return db.one("SELECT * FROM actuator_jobs WHERE id=?", (jid,))


def _sweep(device_id=None):
    """收尾：把超时没人取的指令作废，执行器状态回到待命。

    - sent（板卡取走了但一直没回执）超过 EXPIRE_AFTER_S 就作废；
    - pending（排在队里没人取，比如板卡还没实现协议）超过 30 分钟作废，
      否则演示完一次「立即浇水」，执行器会一直挂在"等板卡执行"上。
    """
    where = " AND device_id=?" if device_id else ""
    args = (device_id,) if device_id else ()
    for j in db.q("SELECT * FROM actuator_jobs WHERE status IN ('pending','sent')"
                  + where, args):
        limit = EXPIRE_AFTER_S / 60.0 if j["status"] == "sent" else 30.0
        if _minutes_since(j["created_at"]) > limit:
            db.exe("UPDATE actuator_jobs SET status='expired' WHERE id=?", (j["id"],))
    # 手上没有待执行的指令了，却还挂着"执行中" → 收回待命
    for a in db.q("SELECT * FROM actuators WHERE state='running'" + where, args):
        live = db.one("SELECT id FROM actuator_jobs WHERE actuator_id=? AND"
                      " status IN ('pending','sent')", (a["id"],))
        if not live:
            db.exe("UPDATE actuators SET state='idle' WHERE id=?", (a["id"],))


def pending_for_device(device_id):
    """板卡来取指令：把 pending 标成 sent，返回内容。顺手清理超时的旧指令。"""
    now = utils.bj_now()[0]
    _sweep(device_id)
    rows = db.q("SELECT * FROM actuator_jobs WHERE device_id=? AND status='pending'"
                " ORDER BY created_at LIMIT 5", (device_id,))
    if rows:
        for r in rows:
            db.exe("UPDATE actuator_jobs SET status='sent', picked_at=? WHERE id=?",
                   (now, r["id"]))
    return [{"job_id": r["id"], "actuator_id": r["actuator_id"], "kind": r["kind"],
             "action": r["action"], "duration_s": r["duration_s"],
             "reason": r["reason"] or "", "source": r["source"]} for r in rows]


def ack(job_id, device_id, ok=True, detail=""):
    """设备回执：写回执 + 更新执行器状态 + 记一条成长日记。"""
    job = db.one("SELECT * FROM actuator_jobs WHERE id=? AND device_id=?",
                 (job_id, device_id))
    if not job:
        return None
    now = utils.bj_now()[0]
    db.exe("UPDATE actuator_jobs SET status=?, acked_at=?, result=? WHERE id=?",
           ("done" if ok else "failed", now, str(detail or "")[:200], job_id))
    a = db.one("SELECT * FROM actuators WHERE id=?", (job["actuator_id"],))
    if a:
        db.exe("UPDATE actuators SET state=?, last_run_at=?, updated_at=? WHERE id=?",
               ("idle" if ok else "error", now if ok else a["last_run_at"], now,
                a["id"]))
        label = kind_label(a["kind"])
        who = {"auto": "自动" + label, "app": "手动" + label,
               "admin": "管理台" + label}.get(job["source"], label)
        title = "%s%s" % (who, "完成" if ok else "失败")
        summary = job["reason"] or ("持续 %s 秒" % job["duration_s"])
        if detail:
            summary = "%s · %s" % (summary, detail)
        plant = db.one("SELECT user_id FROM plants WHERE id=?", (job["plant_id"],))
        plant_ops.add_event(job["plant_id"],
                            "water" if a["kind"] == "water" else "actuator",
                            title=title, summary=summary, source="device",
                            delta=(KINDS.get(a["kind"], ("", 0))[1] if ok else 0),
                            reason=(title if ok else None),
                            user_id=(plant or {}).get("user_id"),
                            device_id=device_id)
    return db.one("SELECT * FROM actuator_jobs WHERE id=?", (job_id,))


def auto_check(plant_id, rec):
    """遥测进来之后跑一遍自动规则：水分偏低 → 自动浇水（有冷却和每日上限）。"""
    if not plant_id or not rec:
        return []
    m = rec.get("moisture")
    if m is None:
        return []
    try:
        m = float(m)
    except Exception:
        return []
    fired = []
    for a in db.q("SELECT * FROM actuators WHERE plant_id=? AND kind='water'"
                  " AND mode='auto'", (plant_id,)):
        cfg = _cfg(a)
        thr = float(cfg.get("moisture_below") or 25)
        if m >= thr:
            continue
        if _minutes_since(a["last_run_at"]) < float(cfg.get("cooldown_min") or 30):
            continue
        if _today_runs(a["id"]) >= int(cfg.get("daily_max") or 6):
            continue
        if db.one("SELECT id FROM actuator_jobs WHERE actuator_id=? AND"
                  " status IN ('pending','sent')", (a["id"],)):
            continue
        fired.append(trigger(a, "run",
                             reason="土壤水分 %.0f%% 低于 %d%%，自动补水"
                                    % (m, thr),
                             source="auto"))
    return [j for j in fired if j]


def summary(plant_id):
    """给 App：执行器状态 + 最近几次执行 + 还有几条指令在排队。"""
    plants = db.q("SELECT device_id FROM plants WHERE id=?", (plant_id,))
    if plants and plants[0].get("device_id"):
        _sweep(plants[0]["device_id"])
    acts = []
    for a in list_for_plant(plant_id):
        cfg = _cfg(a)
        waiting = db.one("SELECT id FROM actuator_jobs WHERE actuator_id=? AND"
                         " status IN ('pending','sent')", (a["id"],))
        acts.append({"id": a["id"], "kind": a["kind"], "name": a["name"],
                     "label": kind_label(a["kind"]), "mode": a["mode"],
                     "state": "pending" if waiting else a["state"],
                     "last_run_at": a["last_run_at"],
                     "last_run_reason": a["last_run_reason"],
                     "today_runs": _today_runs(a["id"]),
                     "config": {"moisture_below": cfg.get("moisture_below"),
                                "cooldown_min": cfg.get("cooldown_min"),
                                "duration_s": cfg.get("duration_s"),
                                "daily_max": cfg.get("daily_max")}})
    recent = db.q("SELECT * FROM actuator_jobs WHERE plant_id=?"
                  " ORDER BY created_at DESC LIMIT 6", (plant_id,))
    pending = db.one("SELECT COUNT(*) n FROM actuator_jobs WHERE plant_id=?"
                     " AND status IN ('pending','sent')", (plant_id,))
    return {"plant_id": plant_id, "actuators": acts,
            "pending": pending["n"] if pending else 0,
            "recent": [{"id": r["id"], "kind": r["kind"], "label": kind_label(r["kind"]),
                        "status": r["status"], "source": r["source"],
                        "reason": r["reason"], "duration_s": r["duration_s"],
                        "result": r["result"], "created_at": r["created_at"],
                        "acked_at": r["acked_at"]} for r in recent]}


def ensure_demo_actuators():
    """演示账号的板卡还没刷"执行器协议"的固件，先在 App 里把这一层摆出来。

    和演示植物一样是**幂等**的：只在"这台设备一路执行器都没有"时补，
    板卡以后自己 POST /actuators/capabilities 声明时，会更新同一批条目。
    补的时候顺带写两条今天的执行记录，页面上"最近执行"不至于是空的。
    """
    for u in db.q("SELECT id FROM users WHERE demo=1"):
        plants = db.q("SELECT * FROM plants WHERE user_id=? AND device_id IS NOT NULL"
                      " AND device_id<>''", (u["id"],))
        for p in plants:
            if db.one("SELECT id FROM actuators WHERE device_id=?", (p["device_id"],)):
                continue
            made = ensure_for_device(p["device_id"], p["id"],
                                     [{"kind": "water", "name": "自动水泵"},
                                      {"kind": "light", "name": "补光灯"}])
            now = utils.bj_now()
            for a in made:
                # 30 分钟冷却起点设成"刚刚"，免得服务器一启动就对真实板卡狂发浇水指令
                db.exe("UPDATE actuators SET last_run_at=?, last_run_reason=? WHERE id=?",
                       (now[0], "土壤水分 16% 低于 25%，自动补水", a["id"]))
                if a["kind"] == "water":
                    for i, (src, res, mins) in enumerate(
                            [("auto", "抽水 15 秒 · 自动补水", 22),
                             ("app", "抽水 15 秒 · 在 App 里手动执行", 95)]):
                        jid = utils.new_id("aj")
                        ts = _shift(now[0], -mins * 60)
                        db.exe("INSERT INTO actuator_jobs(id,actuator_id,device_id,"
                               "plant_id,kind,action,duration_s,reason,source,status,"
                               "created_at,picked_at,acked_at,result)"
                               " VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?)",
                               (jid, a["id"], a["device_id"], a["plant_id"],
                                a["kind"], "run", 15,
                                "土壤水分 16% 低于 25%，自动补水" if src == "auto"
                                else "在 App 里手动执行", src, "done", ts, ts, ts, res))
    return True


def _shift(ts, seconds):
    import time as _t
    try:
        base = _t.mktime(_t.strptime(ts, "%Y-%m-%d %H:%M:%S"))
    except Exception:
        return ts
    return _t.strftime("%Y-%m-%d %H:%M:%S", _t.localtime(base + seconds))


def all_actuators():
    """管理台用：所有执行器 + 所属设备/植物。"""
    return db.q("SELECT a.*, d.sn, d.model, p.name plant_name, p.user_id"
                " FROM actuators a LEFT JOIN devices d ON d.id=a.device_id"
                " LEFT JOIN plants p ON p.id=a.plant_id"
                " ORDER BY a.created_at DESC")


def recent_jobs(limit=50):
    return db.q("SELECT j.*, d.sn FROM actuator_jobs j LEFT JOIN devices d"
                " ON d.id=j.device_id ORDER BY j.created_at DESC LIMIT ?", (limit,))
