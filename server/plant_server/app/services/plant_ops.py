# -*- coding: utf-8 -*-
"""植物域业务：健康分、事件/日记、成长值、徽章、任务。全部在服务器计算。"""
from .. import db, utils

TEMPLATE_DEFS = [
    ("water", "给小绿浇水打卡", 5),
    ("sun", "搬到窗边晒太阳", 5),
    ("photo", "拍一张今日成长照", 5),
    ("ask", "问问 AI 一个问题", 5),
]


def ensure_templates():
    for kind, name, delta in TEMPLATE_DEFS:
        if not db.one("SELECT id FROM task_templates WHERE kind=?", (kind,)):
            db.exe("INSERT INTO task_templates(id,name,kind,growth_delta)"
                   " VALUES(?,?,?,?)",
                   (utils.new_id("tpl"), name, kind, delta))


# ── 健康分（服务器规则，V1 简化）──
def telemetry_health(rec):
    score = 80.0
    m = rec.get("moisture")
    t = rec.get("temp")
    l = rec.get("light")
    e = rec.get("ec")
    if m is not None:
        if m < 20 or m > 90:
            score -= 15
        elif not (40 <= m <= 70):
            score -= 6
    if t is not None:
        if not (18 <= t <= 28):
            score -= 6
    if l is not None:
        if l < 800:
            score -= 6
        elif l < 1500:
            score -= 3
    if e is not None:
        if not (1.0 <= e <= 2.0):
            score -= 5
    return int(max(20, min(99, score)))


def update_plant_health(plant_id, score, source="telemetry"):
    mood, level = utils.mood_of(score)
    db.exe("UPDATE plants SET health_score=?, mood=?, health_level=?, updated_at=?"
           " WHERE id=?",
           (score, mood, level, utils.bj_now()[0], plant_id))
    return score, mood, level


def apply_telemetry(plant_id, rec):
    score = telemetry_health(rec)
    return update_plant_health(plant_id, score)


def get_health(plant_id):
    p = db.one("SELECT * FROM plants WHERE id=?", (plant_id,))
    if not p:
        return None
    last = db.one(
        "SELECT * FROM telemetry WHERE plant_id=? ORDER BY ts DESC LIMIT 1",
        (plant_id,))
    out = dict(p)
    out["last_telemetry"] = last
    return out


# ── 成长值 / 事件 / 徽章 ──
def growth_add(user_id, plant_id, delta, reason, event_id=None):
    if not delta:
        return
    db.exe("INSERT INTO growth_logs(id,user_id,plant_id,delta,reason,event_id,"
           " created_at) VALUES(?,?,?,?,?,?,?)",
           (utils.new_id("g"), user_id, plant_id, delta, reason, event_id,
            utils.bj_now()[0]))


def add_event(plant_id, etype, title=None, summary="", media_id=None,
              source="server", event_ts=None, delta=0, user_id=None,
              device_id=None, reason=None):
    now = utils.bj_now()
    eid = utils.new_id("ev")
    ts = event_ts or now[0]
    db.exe(
        "INSERT INTO events(id,plant_id,user_id,device_id,type,title,summary,"
        " media_id,growth_delta,source,event_ts,created_at)"
        " VALUES(?,?,?,?,?,?,?,?,?,?,?,?)",
        (eid, plant_id, user_id, device_id, etype, title or etype,
         summary, media_id, delta, source, ts, now[0]))
    if delta or reason:
        growth_add(user_id, plant_id, delta, reason or title or etype, eid)
    award_badges(plant_id, user_id)
    return db.one("SELECT * FROM events WHERE id=?", (eid,))


def _count(plant_id, etype):
    return db.one(
        "SELECT COUNT(*) n FROM events WHERE plant_id=? AND type=?",
        (plant_id, etype))["n"]


def award_badges(plant_id, user_id):
    rules = [
        ("first_photo", "第一次拍照", _count(plant_id, "photo") >= 1),
        ("photo5", "摄影小能手", _count(plant_id, "photo") >= 5),
        ("water10", "浇水小达人", _count(plant_id, "water") >= 10),
        ("first_task", "第一次完成任务", _count(plant_id, "task") >= 1),
        ("milestone1", "见证成长", _count(plant_id, "milestone") >= 1),
    ]
    plant = db.one("SELECT user_id FROM plants WHERE id=?", (plant_id,))
    uid = user_id or (plant or {}).get("user_id")
    for key, name, ok in rules:
        if not ok:
            continue
        if db.one("SELECT id FROM badges WHERE plant_id=? AND badge_key=?",
                  (plant_id, key)):
            continue
        db.exe("INSERT INTO badges(id,user_id,plant_id,badge_key,name,"
               " awarded_at) VALUES(?,?,?,?,?,?)",
               (utils.new_id("bd"), uid, plant_id, key, name,
                utils.bj_now()[0]))
        add_event(plant_id, "badge", title="获得徽章：" + name,
                  summary="解锁「" + name + "」徽章！继续加油🏆",
                  source="server", delta=10, user_id=uid)


# ── 任务 ──
def ensure_today_tasks(plant_id):
    today = utils.bj_now()[1]
    for tpl in db.q("SELECT * FROM task_templates"):
        if db.one("SELECT id FROM tasks WHERE plant_id=? AND date=? AND "
                  "template_id=?", (plant_id, today, tpl["id"])):
            continue
        db.exe("INSERT INTO tasks(id,plant_id,date,template_id,content,source,"
               " status,growth_delta) VALUES(?,?,?,?,?,?,?,?)",
               (utils.new_id("t"), plant_id, today, tpl["id"], tpl["name"],
                "template", "open", tpl["growth_delta"]))
    return db.q("SELECT * FROM tasks WHERE plant_id=? AND date=? ORDER BY "
                "template_id", (plant_id, today))


def today_tasks(plant_id):
    return db.q("SELECT * FROM tasks WHERE plant_id=? AND date=?"
                " ORDER BY status DESC, template_id",
                (plant_id, utils.bj_now()[1]))


def complete_task(task_id, by):
    task = db.one("SELECT * FROM tasks WHERE id=?", (task_id,))
    if not task:
        return None, "任务不存在"
    if task["status"] == "done":
        return task, "already_done"
    db.exe("UPDATE tasks SET status='done', completed_by=?, completed_at=?"
           " WHERE id=?",
           (by, utils.bj_now()[0], task_id))
    plant = db.one("SELECT user_id FROM plants WHERE id=?",
                   (task["plant_id"],))
    add_event(task["plant_id"], "task", title="完成任务：" + task["content"],
              summary="「" + task["content"] + "」打卡完成，真棒！",
              source="device" if by and by.startswith("d_") else "app",
              delta=task["growth_delta"],
              user_id=(plant or {}).get("user_id"),
              device_id=by if by and by.startswith("d_") else None)
    return db.one("SELECT * FROM tasks WHERE id=?", (task_id,)), "ok"


def growth_summary(plant_id, user_id):
    total = db.one("SELECT COALESCE(SUM(delta),0) s FROM growth_logs "
                   "WHERE plant_id=?", (plant_id,))["s"]
    logs = db.q("SELECT * FROM growth_logs WHERE plant_id=? "
                "ORDER BY created_at DESC LIMIT 50", (plant_id,))
    badges = db.q("SELECT * FROM badges WHERE plant_id=? ORDER BY awarded_at",
                  (plant_id,))
    return {"total_growth": total, "logs": logs, "badges": badges}


# ── 健康周报（V1 最小版）──
def weekly_report(plant_id):
    now = utils.bj_now()
    since = utils.date_add(now[1], -6) + " 00:00:00"
    rows = db.q("SELECT * FROM telemetry WHERE plant_id=? AND ts>=?",
                (plant_id, since))
    agg = {"moisture": {}, "temp": {}, "light": {}, "ec": {}}
    for k in agg:
        vals = [r[k] for r in rows if r.get(k) is not None]
        if vals:
            agg[k] = {"avg": round(sum(vals) / len(vals), 1),
                      "min": round(min(vals), 1), "max": round(max(vals), 1),
                      "n": len(vals)}
    ev = db.q("SELECT type, COUNT(*) n FROM events WHERE plant_id=? AND "
              "event_ts>=? GROUP BY type", (plant_id, since))
    done_days = db.one("SELECT COUNT(DISTINCT date) n FROM tasks WHERE "
                       "plant_id=? AND status='done' AND completed_at>=?",
                       (plant_id, since))["n"]
    plant = db.one("SELECT * FROM plants WHERE id=?", (plant_id,))
    p = (plant or {}).get("name", "小绿")
    parts = ["%s 这一周" % p]
    if agg.get("moisture"):
        parts.append("土壤水分平均 %s%%（%s-%s）" % (
            agg["moisture"]["avg"], agg["moisture"]["min"],
            agg["moisture"]["max"]))
    if agg.get("temp"):
        parts.append("温度平均 %s°C" % agg["temp"]["avg"])
    if agg.get("light"):
        parts.append("光照平均 %s" % agg["light"]["avg"])
    if agg.get("ec"):
        parts.append("EC 平均 %s" % agg["ec"]["avg"])
    parts.append("完成养护任务 %s 天" % done_days)
    summary = "；".join(parts) + "。继续加油，它越来越健康啦🌱"
    rid = utils.new_id("rp")
    db.exe("INSERT INTO reports(id,plant_id,period_start,summary,summary_json,"
           " created_at) VALUES(?,?,?,?,?,?)",
           (rid, plant_id, since.split(" ")[0], summary,
            json_dumps({"agg": agg, "events": ev, "done_days": done_days}),
            now[0]))
    return db.one("SELECT * FROM reports WHERE id=?", (rid,))


def json_dumps(obj):
    import json
    return json.dumps(obj, ensure_ascii=False)