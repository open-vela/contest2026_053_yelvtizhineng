# -*- coding: utf-8 -*-
"""演示数据：1 演示账号 + 1 设备 + 1 植物 + 30 天传感器 + 日记/任务/徽章。
幂等：重复调用不产生重复数据。"""
import random
import time

from .. import config, db, utils
from . import plant_ops

DEMO_PHONE = config.DEMO_PHONE = "13800000001"
DEMO_SN = "ZXB-DEMO-0001"


def _ts(days_ago, hour):
    d = utils.date_add(utils.bj_now()[1], -days_ago)
    return "%s %02d:%02d:%02d" % (d, hour, (days_ago * 37) % 60, 0)


def ensure_demo(verbose=False):
    """确保演示账号/设备/植物/数据存在。"""
    db.init_db()
    plant_ops.ensure_templates()
    now = utils.bj_now()
    user = db.one("SELECT * FROM users WHERE phone=?", (DEMO_PHONE,))
    if not user:
        uid = utils.new_id("u")
        db.exe("INSERT INTO users(id,phone,nickname,role,demo,created_at) "
               "VALUES(?,?,?,?,?,?)",
               (uid, DEMO_PHONE, "小明", "parent", 1, now[0]))
        user = db.one("SELECT * FROM users WHERE id=?", (uid,))
    uid = user["id"]
    dev = db.one("SELECT * FROM devices WHERE sn=?", (DEMO_SN,))
    if not dev:
        from .. import security
        did = utils.new_id("d")
        token = security.create_token(did, "device")
        db.exe("INSERT INTO devices(id,sn,model,fw_version,owner_user_id,token,"
               " status,last_seen_at,created_at) VALUES(?,?,?,?,?,?,?,?,?)",
               (did, DEMO_SN, "CHD-ESP32-S3-Box", "v1.2.3", uid, token,
                "online", now[0], now[0]))
        dev = db.one("SELECT * FROM devices WHERE id=?", (did,))
    plant = db.one("SELECT * FROM plants WHERE device_id=?", (dev["id"],))
    if not plant:
        pid = utils.new_id("pl")
        db.exe("INSERT INTO plants(id,user_id,device_id,species,name,started_at,"
               " location,health_score,mood,health_level,updated_at,created_at)"
               " VALUES(?,?,?,?,?,?,?,?,?,?,?,?)",
               (pid, uid, dev["id"], "绿萝", "小绿绿", utils.date_add(
                   now[1], -28), "客厅窗台", 80, "😊", "良好", now[0], now[0]))
        plant = db.one("SELECT * FROM plants WHERE id=?", (pid,))

    # 30 天传感器：每 3 小时一条（240 条）
    cnt = db.one("SELECT COUNT(*) n FROM telemetry WHERE plant_id=?",
                 (plant["id"],))["n"]
    if cnt < 100:
        rnd = random.Random(7)
        moist = 58.0
        for i in range(240):
            days_ago = (239 - i) // 8
            hour = (i % 8) * 3
            ts = _ts(days_ago, hour)
            h = hour
            light = 300 + (2500 if 8 <= h <= 17 else 60) + rnd.uniform(-150, 150)
            moist = max(18.0, min(82.0, moist + rnd.uniform(-2.5, 2.5) +
                        (3.0 if moist < 40 else -1.2 if moist > 72 else 0)))
            rec = {"moisture": round(moist, 1),
                   "temp": round(23.5 + rnd.uniform(-2, 2), 1),
                   "light": round(max(0, light), 0),
                   "ec": round(1.3 + rnd.uniform(-0.35, 0.35), 2)}
            db.exe("INSERT OR IGNORE INTO telemetry(id,device_id,plant_id,ts,"
                   " moisture,temp,light,ec,source) VALUES(?,?,?,?,?,?,?,?,?)",
                   (utils.new_id("tm"), dev["id"], plant["id"], ts,
                    rec["moisture"], rec["temp"], rec["light"], rec["ec"],
                    "device"))
        if verbose:
            print("[seed] telemetry seeded")

    # 日记事件（幂等：按 title 判重）
    ev_count = db.one("SELECT COUNT(*) n FROM events WHERE plant_id=?",
                      (plant["id"],))["n"]
    if ev_count < 20:
        def add_if_missing(title, days_ago, hour, etype, delta, summary,
                           source="app"):
            if db.one("SELECT id FROM events WHERE plant_id=? AND title=?",
                      (plant["id"], title)):
                return
            plant_ops.add_event(plant["id"], etype, title=title,
                                summary=summary, source=source,
                                event_ts=_ts(days_ago, hour), delta=delta,
                                user_id=uid)
        for d in range(0, 24, 2):
            add_if_missing("浇水打卡（第 %d 天）" % (28 - d), d, 9, "water",
                           2, "土壤有点干，给小绿浇了 200ml 水💧")
        add_if_missing("第 1 张成长照", 27, 10, "photo", 5, "拍下小绿第一天到家🌱")
        add_if_missing("第 14 天成长照", 14, 10, "photo", 5, "长出了第 3 片新叶！")
        add_if_missing("第 21 天成长照", 7, 10, "photo", 5, "叶子更绿更精神啦")
        add_if_missing("里程碑：长出第 5 片新叶", 3, 18, "milestone", 20,
                       "里程碑事件！从买回来到现在已经 5 片叶子了🎉")
        add_if_missing("AI 体检示例", 1, 12, "diagnose", 0,
                       "整体状态不错😊 记得按时浇水、多晒太阳哦。")
        if verbose:
            print("[seed] events seeded")

    # 近 6 天每天完成一个任务（成长日记好看 + 周打卡数据）
    done = db.one("SELECT COUNT(*) n FROM tasks WHERE plant_id=? AND "
                  "status='done'", (plant["id"],))["n"]
    if done < 5:
        tpls = db.q("SELECT * FROM task_templates")
        for i in range(1, 7):
            d = utils.date_add(now[1], -i)
            tpl = tpls[(i - 1) % len(tpls)]
            if db.one("SELECT id FROM tasks WHERE plant_id=? AND date=?",
                      (plant["id"], d)):
                continue
            tid = utils.new_id("t")
            db.exe("INSERT INTO tasks(id,plant_id,date,template_id,content,"
                   " source,status,completed_by,completed_at,growth_delta) "
                   "VALUES(?,?,?,?,?,?,?,?,?,?)",
                   (tid, plant["id"], d, tpl["id"], tpl["name"], "template",
                    "done", uid, d + " 19:00:00", tpl["growth_delta"]))
            if not db.one("SELECT id FROM events WHERE plant_id=? AND type=? "
                          "AND event_ts LIKE ?",
                          (plant["id"], "task", d + "%")):
                plant_ops.add_event(plant["id"], "task",
                                    title="完成任务：" + tpl["name"],
                                    summary="「" + tpl["name"] +
                                    "」打卡完成，真棒！", source="app",
                                    event_ts=d + " 19:00:00",
                                    delta=tpl["growth_delta"], user_id=uid)
        if verbose:
            print("[seed] past tasks seeded")

    plant_ops.ensure_today_tasks(plant["id"])
    plant_ops.award_badges(plant["id"], uid)
    # 用最后一条遥测校准健康分
    last = db.one("SELECT * FROM telemetry WHERE plant_id=? ORDER BY ts DESC "
                  "LIMIT 1", (plant["id"],))
    if last:
        plant_ops.apply_telemetry(plant["id"], last)
    return {"user_id": uid, "device_id": dev["id"], "plant_id": plant["id"]}