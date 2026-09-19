# -*- coding: utf-8 -*-
"""社区：帖子 / 点赞 / 收藏 / 评论 / 关注 / 排行榜。

多用户之后的"人和人"那一层。两条边界：
  1. 帖子只能挂自己名下的植物；
  2. 配图只能引用自己名下的媒体（否则就能借帖子 id 把别人相册里的图读出来）。
"""
import json
import time

from fastapi import APIRouter, Header, HTTPException, Query, Request
from pydantic import BaseModel

from .. import db, ratelimit, utils
from . import _claims_user

router = APIRouter(prefix="/community", tags=["社区"])

CATS = ("share", "help", "daily")
CAT_NAME = {"share": "晒图", "help": "求助", "daily": "日常"}
MAX_TEXT = 500
MAX_IMAGES = 3

# 演示期的内容兜底：命中就拒。真上线要接内容审核服务。
BANNED = ("加微信", "代购", "广告位", "赌博", "色情", "办证", "发票", "刷单")

# 首页顶部活动（社区 banner）。点"参加"会把话题词带进发帖框。
HOT_TOPICS = [
    {"key": "best", "icon": "🏆", "title": "最美绿植大赛", "sub": "晒出你的绿植 · 赢种子礼包",
     "tag": "#最美绿植大赛#"},
    {"key": "help", "icon": "🆘", "title": "救救我的植物", "sub": "发帖求助，老园丁来支招",
     "tag": "#救救我的植物#"},
]


def _ago(ts):
    """把 'YYYY-MM-DD HH:MM:SS' 说成'3 小时前'。"""
    if not ts:
        return ""
    try:
        t = time.mktime(time.strptime(ts, "%Y-%m-%d %H:%M:%S"))
    except Exception:
        return ts
    d = int((utils.bj_now()[3] - 8 * 3600) - t)
    if d < 60:
        return "刚刚"
    if d < 3600:
        return "%d 分钟前" % (d // 60)
    if d < 86400:
        return "%d 小时前" % (d // 3600)
    if d < 7 * 86400:
        return "%d 天前" % (d // 86400)
    return ts[5:10]


def _author(uid):
    u = db.one("SELECT id,nickname,avatar,city FROM users WHERE id=?", (uid,))
    if not u:
        return {"id": uid, "nickname": "植友", "avatar": "🌿", "city": ""}
    return {"id": u["id"], "nickname": u["nickname"] or "植友",
            "avatar": u["avatar"] or "🌿", "city": u["city"] or ""}


def _post_out(p, uid):
    ids = []
    try:
        ids = json.loads(p.get("media_ids") or "[]")
    except Exception:
        ids = []
    liked = db.one("SELECT 1 FROM post_likes WHERE post_id=? AND user_id=?",
                   (p["id"], uid))
    faved = db.one("SELECT 1 FROM post_favs WHERE post_id=? AND user_id=?",
                   (p["id"], uid))
    followed = db.one("SELECT 1 FROM user_follows WHERE follower_id=? AND "
                      "followee_id=?", (uid, p["user_id"]))
    return {
        "id": p["id"], "text": p["text"],
        "category": p["category"], "category_name": CAT_NAME.get(p["category"], "日常"),
        "topic": p.get("topic") or "",
        "created_at": p["created_at"], "ago": _ago(p["created_at"]),
        "like_count": p.get("like_count") or 0,
        "comment_count": p.get("comment_count") or 0,
        "liked": bool(liked), "faved": bool(faved),
        "followed": bool(followed) or p["user_id"] == uid,
        "is_mine": p["user_id"] == uid,
        "author": _author(p["user_id"]),
        "images": [{"id": m, "url": "/api/v1/media/%s/file" % m} for m in ids],
    }


def _check_text(text):
    t = (text or "").strip()
    if not t:
        raise HTTPException(400, "说点什么吧")
    if len(t) > MAX_TEXT:
        raise HTTPException(400, "内容太长了（最多 %d 字）" % MAX_TEXT)
    for w in BANNED:
        if w in t:
            raise HTTPException(400, "内容里包含不合适的信息，改一下再发吧")
    return t


def _check_images(uid, ids):
    ids = [str(x) for x in (ids or [])][:MAX_IMAGES]
    for mid in ids:
        m = db.one("SELECT id,owner_user_id,kind FROM media WHERE id=?", (mid,))
        if not m or m["kind"] != "image":
            raise HTTPException(400, "图片不存在")
        if m["owner_user_id"] != uid:
            raise HTTPException(403, "只能用自己的照片")
    return ids


class PostIn(BaseModel):
    text: str
    category: str = "share"
    media_ids: list = []
    plant_id: str = None
    topic: str = ""


class CommentIn(BaseModel):
    text: str


@router.get("/topics")
def topics():
    return {"ok": True, "topics": HOT_TOPICS}


@router.get("/feed")
def feed(tab: str = Query("recommend"), limit: int = Query(20),
         offset: int = Query(0), q: str = Query(None),
         authorization: str = Header(None)):
    uid = _claims_user(authorization)["sub"]
    limit = max(1, min(50, limit))
    offset = max(0, offset)
    kw = (q or "").strip()[:40]
    if kw:
        like = "%" + kw + "%"
        rows = db.q("SELECT * FROM posts WHERE text LIKE ? OR topic LIKE ? "
                    "ORDER BY created_at DESC LIMIT ? OFFSET ?",
                    (like, like, limit, offset))
        return {"ok": True, "tab": "search", "q": kw, "limit": limit,
                "offset": offset, "has_more": len(rows) == limit,
                "posts": [_post_out(p, uid) for p in rows]}
    if tab == "follow":
        rows = db.q("SELECT p.* FROM posts p JOIN user_follows f ON "
                    "f.followee_id=p.user_id WHERE f.follower_id=? "
                    "ORDER BY p.created_at DESC LIMIT ? OFFSET ?",
                    (uid, limit, offset))
    elif tab == "photo":
        rows = db.q("SELECT * FROM posts WHERE category='share' ORDER BY "
                    "created_at DESC LIMIT ? OFFSET ?", (limit, offset))
    elif tab == "help":
        rows = db.q("SELECT * FROM posts WHERE category='help' ORDER BY "
                    "created_at DESC LIMIT ? OFFSET ?", (limit, offset))
    elif tab == "mine":
        rows = db.q("SELECT * FROM posts WHERE user_id=? ORDER BY created_at "
                    "DESC LIMIT ? OFFSET ?", (uid, limit, offset))
    else:
        tab = "recommend"
        rows = db.q("SELECT * FROM posts ORDER BY "
                    "(like_count * 2 + comment_count * 3) DESC, "
                    "created_at DESC LIMIT ? OFFSET ?", (limit, offset))
    return {"ok": True, "tab": tab, "limit": limit, "offset": offset,
            "has_more": len(rows) == limit,
            "posts": [_post_out(p, uid) for p in rows]}


@router.get("/leaderboard")
def leaderboard(authorization: str = Header(None)):
    uid = _claims_user(authorization)["sub"]
    rows = db.q(
        "SELECT u.id, u.nickname, u.avatar, u.city,"
        " (SELECT COUNT(*) FROM posts p WHERE p.user_id=u.id) AS posts,"
        " (SELECT COALESCE(SUM(p.like_count),0) FROM posts p WHERE p.user_id=u.id) AS likes,"
        " (SELECT COUNT(*) FROM user_follows f WHERE f.followee_id=u.id) AS followers"
        " FROM users u"
        " ORDER BY likes DESC, posts DESC, followers DESC LIMIT 20")
    out = []
    for i, r in enumerate(rows):
        out.append({"rank": i + 1, "id": r["id"],
                    "nickname": r["nickname"] or "植友",
                    "avatar": r["avatar"] or "🌿",
                    "city": r["city"] or "",
                    "posts": r["posts"] or 0,
                    "likes": r["likes"] or 0,
                    "followers": r["followers"] or 0,
                    "is_me": r["id"] == uid})
    return {"ok": True, "leaderboard": out}


@router.post("/posts")
def create_post(body: PostIn, request: Request,
                authorization: str = Header(None)):
    claims = _claims_user(authorization)
    uid = claims["sub"]
    # 一人一小时最多发 10 条，防刷屏
    ratelimit.hit(request, "community_post", 10, 3600)
    text = _check_text(body.text)
    cat = body.category if body.category in CATS else "share"
    ids = _check_images(uid, body.media_ids)
    if body.plant_id and not db.one("SELECT id FROM plants WHERE id=? AND user_id=?",
                                    (body.plant_id, uid)):
        raise HTTPException(403, "只能挂自己的植物")
    pid = utils.new_id("po")
    now = utils.bj_now()[0]
    db.exe("INSERT INTO posts(id,user_id,plant_id,category,text,media_ids,topic,"
           " created_at) VALUES(?,?,?,?,?,?,?,?)",
           (pid, uid, body.plant_id, cat, text, json.dumps(ids),
            (body.topic or "")[:40], now))
    return {"ok": True, "post": _post_out(db.one("SELECT * FROM posts WHERE id=?",
                                                 (pid,)), uid)}


@router.get("/posts/{post_id}")
def post_detail(post_id: str, authorization: str = Header(None)):
    uid = _claims_user(authorization)["sub"]
    p = db.one("SELECT * FROM posts WHERE id=?", (post_id,))
    if not p:
        raise HTTPException(404, "帖子不存在或已删除")
    out = _post_out(p, uid)
    cm = db.q("SELECT * FROM post_comments WHERE post_id=? ORDER BY created_at",
              (post_id,))
    out["comments"] = [{"id": c["id"], "text": c["text"],
                        "ago": _ago(c["created_at"]),
                        "is_mine": c["user_id"] == uid,
                        "author": _author(c["user_id"])} for c in cm]
    return {"ok": True, "post": out}


@router.post("/posts/{post_id}/like")
def like_post(post_id: str, authorization: str = Header(None)):
    uid = _claims_user(authorization)["sub"]
    p = db.one("SELECT * FROM posts WHERE id=?", (post_id,))
    if not p:
        raise HTTPException(404, "帖子不存在或已删除")
    hit = db.one("SELECT 1 FROM post_likes WHERE post_id=? AND user_id=?",
                 (post_id, uid))
    if hit:
        db.exe("DELETE FROM post_likes WHERE post_id=? AND user_id=?",
               (post_id, uid))
        db.exe("UPDATE posts SET like_count=MAX(0, like_count-1) WHERE id=?",
               (post_id,))
        liked = False
    else:
        db.exe("INSERT INTO post_likes(post_id,user_id,created_at) VALUES(?,?,?)",
               (post_id, uid, utils.bj_now()[0]))
        db.exe("UPDATE posts SET like_count=like_count+1 WHERE id=?", (post_id,))
        liked = True
    n = db.one("SELECT like_count FROM posts WHERE id=?", (post_id,))
    return {"ok": True, "liked": liked, "like_count": (n or {}).get("like_count") or 0}


@router.post("/posts/{post_id}/fav")
def fav_post(post_id: str, authorization: str = Header(None)):
    uid = _claims_user(authorization)["sub"]
    if not db.one("SELECT id FROM posts WHERE id=?", (post_id,)):
        raise HTTPException(404, "帖子不存在或已删除")
    hit = db.one("SELECT 1 FROM post_favs WHERE post_id=? AND user_id=?",
                 (post_id, uid))
    if hit:
        db.exe("DELETE FROM post_favs WHERE post_id=? AND user_id=?",
               (post_id, uid))
        return {"ok": True, "faved": False}
    db.exe("INSERT INTO post_favs(post_id,user_id,created_at) VALUES(?,?,?)",
           (post_id, uid, utils.bj_now()[0]))
    return {"ok": True, "faved": True}


@router.post("/posts/{post_id}/comments")
def add_comment(post_id: str, body: CommentIn,
                authorization: str = Header(None)):
    uid = _claims_user(authorization)["sub"]
    if not db.one("SELECT id FROM posts WHERE id=?", (post_id,)):
        raise HTTPException(404, "帖子不存在或已删除")
    text = _check_text(body.text)
    if len(text) > 200:
        raise HTTPException(400, "评论太长了（最多 200 字）")
    cid = utils.new_id("cm")
    db.exe("INSERT INTO post_comments(id,post_id,user_id,text,created_at)"
           " VALUES(?,?,?,?,?)", (cid, post_id, uid, text, utils.bj_now()[0]))
    db.exe("UPDATE posts SET comment_count=comment_count+1 WHERE id=?",
           (post_id,))
    return {"ok": True, "comment": {"id": cid, "text": text, "ago": "刚刚",
                                    "is_mine": True, "author": _author(uid)}}


@router.post("/users/{user_id}/follow")
def follow_user(user_id: str, authorization: str = Header(None)):
    uid = _claims_user(authorization)["sub"]
    if user_id == uid:
        raise HTTPException(400, "不能关注自己")
    if not db.one("SELECT id FROM users WHERE id=?", (user_id,)):
        raise HTTPException(404, "用户不存在")
    hit = db.one("SELECT 1 FROM user_follows WHERE follower_id=? AND "
                 "followee_id=?", (uid, user_id))
    if hit:
        db.exe("DELETE FROM user_follows WHERE follower_id=? AND followee_id=?",
               (uid, user_id))
        return {"ok": True, "followed": False}
    db.exe("INSERT INTO user_follows(follower_id,followee_id,created_at)"
           " VALUES(?,?,?)", (uid, user_id, utils.bj_now()[0]))
    return {"ok": True, "followed": True}


@router.get("/users/{user_id}")
def user_home(user_id: str, authorization: str = Header(None)):
    uid = _claims_user(authorization)["sub"]
    if not db.one("SELECT id FROM users WHERE id=?", (user_id,)):
        raise HTTPException(404, "用户不存在")
    posts = db.q("SELECT * FROM posts WHERE user_id=? ORDER BY created_at DESC"
                 " LIMIT 30", (user_id,))
    followers = db.one("SELECT COUNT(*) n FROM user_follows WHERE followee_id=?",
                       (user_id,))["n"]
    following = db.one("SELECT COUNT(*) n FROM user_follows WHERE follower_id=?",
                       (user_id,))["n"]
    followed = db.one("SELECT 1 FROM user_follows WHERE follower_id=? AND "
                      "followee_id=?", (uid, user_id))
    return {"ok": True,
            "user": _author(user_id),
            "is_me": user_id == uid,
            "followed": bool(followed),
            "followers": followers, "following": following,
            "plants": db.one("SELECT COUNT(*) n FROM plants WHERE user_id=?",
                             (user_id,))["n"],
            "posts": [_post_out(p, uid) for p in posts]}