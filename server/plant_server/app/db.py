# -*- coding: utf-8 -*-
"""SQLite（线程局部连接）。schema 幂等创建。"""
import sqlite3
import threading

from . import config

_local = threading.local()

SCHEMA = """
CREATE TABLE IF NOT EXISTS users(
  id TEXT PRIMARY KEY, phone TEXT UNIQUE, nickname TEXT,
  role TEXT DEFAULT 'parent', demo INTEGER DEFAULT 0, created_at TEXT);

CREATE TABLE IF NOT EXISTS devices(
  id TEXT PRIMARY KEY, sn TEXT UNIQUE, model TEXT, fw_version TEXT,
  owner_user_id TEXT, token TEXT, status TEXT DEFAULT 'offline',
  last_seen_at TEXT, created_at TEXT);

CREATE TABLE IF NOT EXISTS plants(
  id TEXT PRIMARY KEY, user_id TEXT, device_id TEXT, species TEXT,
  name TEXT, avatar_media_id TEXT, started_at TEXT, location TEXT,
  health_score INTEGER DEFAULT 80, mood TEXT DEFAULT '😊',
  health_level TEXT DEFAULT '良好', updated_at TEXT, created_at TEXT);

CREATE TABLE IF NOT EXISTS telemetry(
  id TEXT PRIMARY KEY, device_id TEXT, plant_id TEXT, ts TEXT,
  moisture REAL, temp REAL, light REAL, ec REAL, source TEXT DEFAULT 'device',
  salt REAL, nitrogen REAL, phosphorus REAL, potassium REAL, ph REAL,
  UNIQUE(device_id, ts));

CREATE TABLE IF NOT EXISTS media(
  id TEXT PRIMARY KEY, plant_id TEXT, owner_user_id TEXT,
  kind TEXT, fmt TEXT, path TEXT, thumb_path TEXT, meta TEXT, created_at TEXT);

CREATE TABLE IF NOT EXISTS events(
  id TEXT PRIMARY KEY, plant_id TEXT, user_id TEXT, device_id TEXT,
  type TEXT, title TEXT, summary TEXT, media_id TEXT,
  growth_delta INTEGER DEFAULT 0, source TEXT, event_ts TEXT, created_at TEXT);

CREATE TABLE IF NOT EXISTS task_templates(
  id TEXT PRIMARY KEY, name TEXT, kind TEXT, growth_delta INTEGER DEFAULT 5);

CREATE TABLE IF NOT EXISTS tasks(
  id TEXT PRIMARY KEY, plant_id TEXT, date TEXT, template_id TEXT,
  content TEXT, source TEXT DEFAULT 'template',
  status TEXT DEFAULT 'open', completed_by TEXT, completed_at TEXT,
  growth_delta INTEGER DEFAULT 5,
  UNIQUE(plant_id, date, content));

CREATE TABLE IF NOT EXISTS growth_logs(
  id TEXT PRIMARY KEY, user_id TEXT, plant_id TEXT, delta INTEGER,
  reason TEXT, event_id TEXT, created_at TEXT);

CREATE TABLE IF NOT EXISTS badges(
  id TEXT PRIMARY KEY, user_id TEXT, plant_id TEXT,
  badge_key TEXT, name TEXT, awarded_at TEXT,
  UNIQUE(plant_id, badge_key));

CREATE TABLE IF NOT EXISTS conversations(
  id TEXT PRIMARY KEY, plant_id TEXT, user_id TEXT, device_id TEXT,
  title TEXT, created_at TEXT, updated_at TEXT);

CREATE TABLE IF NOT EXISTS messages(
  id TEXT PRIMARY KEY, conversation_id TEXT, role TEXT, text TEXT,
  audio_media_id TEXT, ts TEXT);

CREATE TABLE IF NOT EXISTS ai_jobs(
  id TEXT PRIMARY KEY, job_type TEXT, status TEXT,
  provider TEXT, model TEXT, request_source TEXT, input_ref TEXT,
  structured_json TEXT, raw_model_text TEXT, error TEXT, created_at TEXT);

CREATE TABLE IF NOT EXISTS reports(
  id TEXT PRIMARY KEY, plant_id TEXT, period_start TEXT,
  summary TEXT, summary_json TEXT, created_at TEXT);

CREATE TABLE IF NOT EXISTS sms_codes(
  phone TEXT PRIMARY KEY, code TEXT, expires_at TEXT, created_at TEXT);

-- ── 社区（2026-09-12）──────────────────────────────────────────────
-- category: share 晒图 / help 求助 / daily 日常；topic 用来挂活动话题
CREATE TABLE IF NOT EXISTS posts(
  id TEXT PRIMARY KEY, user_id TEXT, plant_id TEXT,
  category TEXT DEFAULT 'share', text TEXT, media_ids TEXT, topic TEXT,
  like_count INTEGER DEFAULT 0, comment_count INTEGER DEFAULT 0,
  created_at TEXT);

CREATE TABLE IF NOT EXISTS post_likes(
  post_id TEXT, user_id TEXT, created_at TEXT,
  PRIMARY KEY(post_id, user_id));

CREATE TABLE IF NOT EXISTS post_favs(
  post_id TEXT, user_id TEXT, created_at TEXT,
  PRIMARY KEY(post_id, user_id));

CREATE TABLE IF NOT EXISTS post_comments(
  id TEXT PRIMARY KEY, post_id TEXT, user_id TEXT, text TEXT, created_at TEXT);

CREATE TABLE IF NOT EXISTS user_follows(
  follower_id TEXT, followee_id TEXT, created_at TEXT,
  PRIMARY KEY(follower_id, followee_id));

-- ── 绿植保险与一键理赔（2026-09-12）────────────────────────────────
-- claims.status: submitted 已提交 / approved 审核通过 / manual 转人工
--               / need_more_info 待补材料 / shipped 已补发 / rejected 不通过
CREATE TABLE IF NOT EXISTS policies(
  id TEXT PRIMARY KEY, user_id TEXT, plant_id TEXT, plan TEXT DEFAULT 'demo',
  coverage_cents INTEGER DEFAULT 4900, status TEXT DEFAULT 'active',
  started_at TEXT, expires_at TEXT, created_at TEXT);

CREATE TABLE IF NOT EXISTS claims(
  id TEXT PRIMARY KEY, claim_no TEXT, policy_id TEXT, user_id TEXT,
  plant_id TEXT, reason TEXT, description TEXT, media_ids TEXT,
  evidence_json TEXT, auto_verdict TEXT, auto_score INTEGER,
  auto_note TEXT, status TEXT DEFAULT 'submitted',
  payout_cents INTEGER DEFAULT 0, timeline_json TEXT,
  created_at TEXT, updated_at TEXT);

-- ── 自动执行层 · 执行器（2026-09-12）──────────────────────────────
-- actuators   = 一台设备上的一路执行器（水泵/补光灯/风机/喂食器）
--   mode: auto 自动（服务器按规则下发）/ off 关闭 / manual 只在 App 手动
--   state: idle 待命 / running 执行中 / error 故障
--   config_json: {"moisture_below":25,"cooldown_min":30,"duration_s":15,"daily_max":6}
-- actuator_jobs = 服务器 → 设备 的执行指令，设备执行完回执
--   status: pending 待取 / sent 已下发 / done 已执行 / failed 失败 / expired 超时
CREATE TABLE IF NOT EXISTS actuators(
  id TEXT PRIMARY KEY, device_id TEXT, plant_id TEXT,
  kind TEXT, name TEXT, mode TEXT DEFAULT 'auto', state TEXT DEFAULT 'idle',
  config_json TEXT, last_run_at TEXT, last_run_by TEXT, last_run_reason TEXT,
  created_at TEXT, updated_at TEXT,
  UNIQUE(device_id, kind));

CREATE TABLE IF NOT EXISTS actuator_jobs(
  id TEXT PRIMARY KEY, actuator_id TEXT, device_id TEXT, plant_id TEXT,
  kind TEXT, action TEXT DEFAULT 'run', duration_s INTEGER DEFAULT 0,
  reason TEXT, source TEXT DEFAULT 'app', status TEXT DEFAULT 'pending',
  created_at TEXT, picked_at TEXT, acked_at TEXT, result TEXT);

-- ── 会员订阅 / 押金（2026-09-12）─────────────────────────────────
-- plan: basic 19.9 / pro 29.9 / family 49.9；status: active / expired / cancelled
-- deposit_cents 硬件押金（99 元），满 12 个月退还（deposit_status: held / refunded）
CREATE TABLE IF NOT EXISTS subscriptions(
  id TEXT PRIMARY KEY, user_id TEXT UNIQUE, plan TEXT DEFAULT 'basic',
  status TEXT DEFAULT 'active', started_at TEXT, expires_at TEXT,
  auto_renew INTEGER DEFAULT 1,
  deposit_cents INTEGER DEFAULT 0, deposit_status TEXT DEFAULT 'none',
  created_at TEXT, updated_at TEXT);
"""


def init_db():
    con = sqlite3.connect(config.DB_PATH)
    con.executescript(SCHEMA)

    # 2026-09-11：土壤 8 参数上云——老库补列（新库由 SCHEMA 直接建全）。
    # 板卡现在上报 温度/水分/EC/pH/盐分/氮/磷/钾，原来只存 4 个。
    tcols = [r[1] for r in con.execute("PRAGMA table_info(telemetry)")]
    for col in ("salt", "nitrogen", "phosphorus", "potassium", "ph"):
        if col not in tcols:
            con.execute("ALTER TABLE telemetry ADD COLUMN %s REAL" % col)

    # 2026-09-12：多用户——用户资料（头像 emoji / 城市，社区里要显示）
    ucols = [r[1] for r in con.execute("PRAGMA table_info(users)")]
    for col in ("avatar", "city", "bio"):
        if col not in ucols:
            con.execute("ALTER TABLE users ADD COLUMN %s TEXT" % col)
    con.execute("UPDATE users SET avatar='🌿' WHERE avatar IS NULL")

    cols = [r[1] for r in con.execute("PRAGMA table_info(plants)")]
    if "created_at" not in cols:
        con.execute("ALTER TABLE plants ADD COLUMN created_at TEXT")
    con.execute("UPDATE plants SET created_at=COALESCE(created_at, updated_at, started_at)"
                " WHERE created_at IS NULL")
    con.commit()
    con.close()


def conn():
    c = getattr(_local, "c", None)
    if c is None:
        c = sqlite3.connect(config.DB_PATH, check_same_thread=False)
        c.row_factory = sqlite3.Row
        c.execute("PRAGMA foreign_keys=ON")
        c.execute("PRAGMA journal_mode=WAL")
        _local.c = c
    return c


def q(sql, params=()):
    return [dict(r) for r in conn().execute(sql, params).fetchall()]


def one(sql, params=()):
    r = conn().execute(sql, params).fetchone()
    return dict(r) if r else None


def exe(sql, params=()):
    c = conn()
    cur = c.execute(sql, params)
    c.commit()
    return cur.lastrowid
