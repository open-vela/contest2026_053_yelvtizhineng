"""Account-scoped learning and atlas; additive tables, no device control."""
import json
import sqlite3
from contextlib import contextmanager
from datetime import date
from pathlib import Path
from typing import Literal

from fastapi import APIRouter, Header, HTTPException, Query
from pydantic import BaseModel, Field

from .. import config, utils
from . import either

router = APIRouter(prefix='/learning', tags=['科普与图鉴'])
CATALOG = json.loads((Path(__file__).resolve().parent.parent / 'resources' / 'growth_catalog.json').read_text(encoding='utf-8'))
SPECIES = {p['id']: p for p in CATALOG['plants']}
ARTICLES = {a['id']: a for a in CATALOG['articles']}
ALIASES = {alias.strip().casefold(): p['id'] for p in SPECIES.values() for alias in p['aliases']}
SCHEMA = """
CREATE TABLE IF NOT EXISTS zx_learning(user_id TEXT NOT NULL, article_id TEXT NOT NULL, completed_at TEXT NOT NULL, PRIMARY KEY(user_id,article_id));
CREATE TABLE IF NOT EXISTS zx_bookmarks(user_id TEXT NOT NULL, article_id TEXT NOT NULL, created_at TEXT NOT NULL, PRIMARY KEY(user_id,article_id));
CREATE TABLE IF NOT EXISTS zx_plant_species(user_id TEXT NOT NULL, plant_id TEXT NOT NULL, species_id TEXT NOT NULL, confirmed_at TEXT NOT NULL, PRIMARY KEY(user_id,plant_id));
CREATE TABLE IF NOT EXISTS zx_observations(id TEXT PRIMARY KEY, user_id TEXT NOT NULL, plant_id TEXT NOT NULL, day TEXT NOT NULL, choice TEXT NOT NULL, note TEXT NOT NULL, created_at TEXT NOT NULL, UNIQUE(user_id,plant_id,day));
CREATE INDEX IF NOT EXISTS zx_obs_owner ON zx_observations(user_id,plant_id,day);
CREATE TABLE IF NOT EXISTS zx_comparisons(id TEXT PRIMARY KEY, user_id TEXT NOT NULL, plant_id TEXT NOT NULL, before_id TEXT NOT NULL, after_id TEXT NOT NULL, note TEXT NOT NULL, created_at TEXT NOT NULL, UNIQUE(user_id,after_id));
CREATE TABLE IF NOT EXISTS zx_reflections(user_id TEXT NOT NULL, plant_id TEXT NOT NULL, note TEXT NOT NULL, updated_at TEXT NOT NULL, PRIMARY KEY(user_id,plant_id));
"""


@contextmanager
def connection():
    con = sqlite3.connect(config.DB_PATH, timeout=15)
    con.row_factory = sqlite3.Row
    try:
        with con:
            yield con
    finally:
        con.close()


def init_learning():
    with connection() as con:
        con.executescript(SCHEMA)


def user_id(authorization):
    claims, kind = either(authorization, None)
    if kind != 'user':
        raise HTTPException(401, '请先登录')
    with connection() as con:
        if not con.execute('SELECT id FROM users WHERE id=?', (claims['sub'],)).fetchone():
            raise HTTPException(401, '请重新登录')
    return claims['sub']


def own_plant(con, uid, pid):
    row = con.execute('SELECT * FROM plants WHERE id=? AND user_id=?', (pid, uid)).fetchone()
    if not row:
        raise HTTPException(404, '植物不存在或无权访问')
    return dict(row)


def event(con, uid, pid, eid, title, summary):
    now = utils.bj_now()[0]
    con.execute('INSERT OR IGNORE INTO events(id,plant_id,user_id,type,title,summary,growth_delta,source,event_ts,created_at) VALUES(?,?,?,?,?,?,0,?,?,?)',
                (eid, pid, uid, 'note', title, summary, 'app', now, now))


def progress(con, uid):
    today = date.fromisoformat(utils.bj_now()[1])
    plants = [dict(p) for p in con.execute('SELECT id,name,species,started_at,created_at FROM plants WHERE user_id=? ORDER BY created_at', (uid,))]
    assigned = {r['plant_id']: r['species_id'] for r in con.execute('SELECT plant_id,species_id FROM zx_plant_species WHERE user_id=?', (uid,))}
    learned = [r[0] for r in con.execute('SELECT article_id FROM zx_learning WHERE user_id=?', (uid,))]
    bookmarks = [r[0] for r in con.execute('SELECT article_id FROM zx_bookmarks WHERE user_id=?', (uid,))]
    observations = [dict(r) for r in con.execute('SELECT * FROM zx_observations WHERE user_id=? ORDER BY created_at DESC', (uid,))]
    comparisons = [dict(r) for r in con.execute('SELECT * FROM zx_comparisons WHERE user_id=?', (uid,))]
    reflections = [dict(r) for r in con.execute('SELECT * FROM zx_reflections WHERE user_id=?', (uid,))]
    for p in plants:
        p['species_id'] = assigned.get(p['id']) or ALIASES.get((p.get('species') or '').strip().casefold())
    entries = []
    for sid, species in SPECIES.items():
        owned = [p for p in plants if p['species_id'] == sid]
        ids = {p['id'] for p in owned}
        dates = []
        for p in owned:
            try:
                dates.append(date.fromisoformat((p.get('started_at') or p.get('created_at') or '')[:10]))
            except ValueError:
                pass
        days = max(1, (today - min(dates)).days + 1) if dates else (1 if owned else 0)
        obs = [o for o in observations if o['plant_id'] in ids]
        observation_days = len({o['day'] for o in obs})
        lessons = len([a for a in learned if a in ARTICLES and sid in ARTICLES[a]['applicable_species']])
        pairs = len([c for c in comparisons if c['plant_id'] in ids])
        reviews = len([r for r in reflections if r['plant_id'] in ids])
        metrics = {'days': days, 'observations': observation_days, 'lessons': lessons, 'comparisons': pairs, 'reflections': reviews}
        thresholds = {1: {}, 2: {'days': 3, 'observations': 2, 'lessons': 1}, 3: {'days': 7, 'observations': 4, 'lessons': 2}, 4: {'days': 14, 'observations': 7, 'lessons': 3, 'comparisons': 1}, 5: {'days': 30, 'observations': 12, 'lessons': 4, 'comparisons': 2, 'reflections': 1}}
        level = 0
        if owned:
            level = 1
            for n in range(2, 6):
                if all(metrics[k] >= v for k, v in thresholds[n].items()):
                    level = n
        entries.append({'id': sid, 'level': level, 'owned': bool(owned), 'plants': owned, 'metrics': metrics,
                        'next': thresholds.get(level + 1, {}), 'observed_today': any(o['day'] == today.isoformat() for o in obs)})
    return {'ok': True, 'version': CATALOG['version'], 'entries': entries, 'plants': plants, 'learned': learned, 'bookmarks': bookmarks, 'today': today.isoformat()}


@router.get('/overview')
def overview(authorization: str = Header(None)):
    uid = user_id(authorization)
    with connection() as con:
        return progress(con, uid)


class QuizIn(BaseModel):
    article_id: str
    answer: int = Field(ge=0, le=10)


@router.post('/complete')
def complete(body: QuizIn, authorization: str = Header(None)):
    uid = user_id(authorization)
    a = ARTICLES.get(body.article_id)
    if not a:
        raise HTTPException(404, '短课不存在')
    if body.answer != a['correct']:
        return {'ok': True, 'correct': False, 'explanation': a['explanation']}
    with connection() as con:
        con.execute('INSERT OR IGNORE INTO zx_learning VALUES(?,?,?)', (uid, a['id'], utils.bj_now()[0]))
        return {'ok': True, 'correct': True, 'explanation': a['explanation'], 'overview': progress(con, uid)}


class BookmarkIn(BaseModel):
    article_id: str
    saved: bool


@router.post('/bookmark')
def bookmark(body: BookmarkIn, authorization: str = Header(None)):
    uid = user_id(authorization)
    if body.article_id not in ARTICLES:
        raise HTTPException(404, '短课不存在')
    with connection() as con:
        if body.saved:
            con.execute('INSERT OR IGNORE INTO zx_bookmarks VALUES(?,?,?)', (uid, body.article_id, utils.bj_now()[0]))
        else:
            con.execute('DELETE FROM zx_bookmarks WHERE user_id=? AND article_id=?', (uid, body.article_id))
        return {'ok': True, 'saved': body.saved}


class AssignIn(BaseModel):
    plant_id: str
    species_id: str


@router.post('/assign')
def assign(body: AssignIn, authorization: str = Header(None)):
    uid = user_id(authorization)
    if body.species_id not in SPECIES:
        raise HTTPException(404, '图鉴条目不存在')
    with connection() as con:
        own_plant(con, uid, body.plant_id)
        con.execute('INSERT INTO zx_plant_species VALUES(?,?,?,?) ON CONFLICT(user_id,plant_id) DO UPDATE SET species_id=excluded.species_id,confirmed_at=excluded.confirmed_at', (uid, body.plant_id, body.species_id, utils.bj_now()[0]))
        return progress(con, uid)


class ObservationIn(BaseModel):
    plant_id: str
    choice: Literal['wet', 'dry', 'unsure', 'no_change', 'new_leaf']
    note: str = Field(default='', max_length=800)


@router.post('/observe')
def observe(body: ObservationIn, authorization: str = Header(None)):
    uid = user_id(authorization)
    now, day, _, _ = utils.bj_now()
    labels = {'wet': '盆土仍湿，今天先不浇', 'dry': '发现需水线索，继续核对', 'unsure': '还拿不准，先留下观察', 'no_change': '今天暂未发现明显变化', 'new_leaf': '我观察到一片新叶'}
    with connection() as con:
        own_plant(con, uid, body.plant_id)
        con.execute('BEGIN IMMEDIATE')
        existing = con.execute('SELECT id FROM zx_observations WHERE user_id=? AND plant_id=? AND day=?', (uid, body.plant_id, day)).fetchone()
        if existing:
            return {'ok': True, 'already_recorded': True, 'overview': progress(con, uid)}
        oid = utils.new_id('obs')
        con.execute('INSERT INTO zx_observations VALUES(?,?,?,?,?,?,?)', (oid, uid, body.plant_id, day, body.choice, body.note.strip(), now))
        event(con, uid, body.plant_id, oid, '成长观察：' + labels[body.choice], body.note.strip() or '用户主动观察并记录。实际养护按植物需要进行。')
        return {'ok': True, 'already_recorded': False, 'overview': progress(con, uid)}


@router.get('/observations')
def observations(plant_id: str = Query(...), authorization: str = Header(None)):
    uid = user_id(authorization)
    with connection() as con:
        own_plant(con, uid, plant_id)
        rows = [dict(r) for r in con.execute('SELECT * FROM zx_observations WHERE user_id=? AND plant_id=? ORDER BY day DESC LIMIT 120', (uid, plant_id))]
        pairs = [dict(r) for r in con.execute('SELECT * FROM zx_comparisons WHERE user_id=? AND plant_id=? ORDER BY created_at DESC', (uid, plant_id))]
        review = con.execute('SELECT note FROM zx_reflections WHERE user_id=? AND plant_id=?', (uid, plant_id)).fetchone()
        return {'ok': True, 'observations': rows, 'comparisons': pairs, 'reflection': review['note'] if review else ''}


class ComparisonIn(BaseModel):
    plant_id: str
    before_id: str
    after_id: str
    note: str = Field(min_length=1, max_length=1000)


@router.post('/compare')
def compare(body: ComparisonIn, authorization: str = Header(None)):
    uid = user_id(authorization)
    if not body.note.strip():
        raise HTTPException(422, '请写下你的观察')
    with connection() as con:
        own_plant(con, uid, body.plant_id)
        con.execute('BEGIN IMMEDIATE')
        records = [con.execute('SELECT * FROM zx_observations WHERE id=? AND user_id=? AND plant_id=?', (oid, uid, body.plant_id)).fetchone() for oid in [body.before_id, body.after_id]]
        if not all(records):
            raise HTTPException(404, '请选择这盆植物的两条有效记录')
        if (date.fromisoformat(records[1]['day']) - date.fromisoformat(records[0]['day'])).days < 3:
            raise HTTPException(400, '请选择先后相隔至少 3 天的两条记录')
        previous = con.execute('SELECT id FROM zx_comparisons WHERE user_id=? AND after_id=?', (uid, body.after_id)).fetchone()
        if previous:
            raise HTTPException(409, '后一条记录已用于一次对照，请等待新的观察')
        cid = utils.new_id('cmp')
        con.execute('INSERT INTO zx_comparisons VALUES(?,?,?,?,?,?,?)', (cid, uid, body.plant_id, body.before_id, body.after_id, body.note.strip(), utils.bj_now()[0]))
        event(con, uid, body.plant_id, cid, '成长对照：' + records[0]['day'] + ' → ' + records[1]['day'], body.note.strip())
        return progress(con, uid)


class ReflectionIn(BaseModel):
    plant_id: str
    note: str = Field(min_length=20, max_length=2000)


@router.post('/reflect')
def reflect(body: ReflectionIn, authorization: str = Header(None)):
    uid = user_id(authorization)
    if len(body.note.strip()) < 20:
        raise HTTPException(422, '再多写一点吧，至少 20 个字')
    with connection() as con:
        own_plant(con, uid, body.plant_id)
        con.execute('INSERT INTO zx_reflections VALUES(?,?,?,?) ON CONFLICT(user_id,plant_id) DO UPDATE SET note=excluded.note,updated_at=excluded.updated_at', (uid, body.plant_id, body.note.strip(), utils.bj_now()[0]))
        eid = 'reflection_' + body.plant_id
        event(con, uid, body.plant_id, eid, '养护复盘：把自己的经验记下来', body.note.strip())
        con.execute('UPDATE events SET summary=?,event_ts=? WHERE id=? AND user_id=?', (body.note.strip(), utils.bj_now()[0], eid, uid))
        return progress(con, uid)
