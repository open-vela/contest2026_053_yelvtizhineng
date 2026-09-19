# -*- coding: utf-8 -*-
"""社区演示内容：几个"邻居"账号和他们的帖子/评论/关注。

为什么要有：社区做实之后如果空着，第一次点进去只能看到"还没有内容"，
演示效果很差。这里的点赞/评论/关注都写真实的行，计数就是真数出来的，
不写死数字 —— 免得以后算出来对不上。
幂等：库里已经有帖子就直接返回。
"""
import json

from .. import db, utils

# (手机号尾号, 昵称, 头像, 城市, 简介)
USERS = [
    ("13900000002", "花花妈妈", "🌻", "上海", "龟背竹重度爱好者"),
    ("13900000003", "绿手指老王", "🧑‍🌾", "北京", "养了 20 年花，有问题问我"),
    ("13900000004", "小满", "🌸", "杭州", "阳台党 · 多肉和香草"),
    ("13900000005", "多肉小王子", "🌵", "成都", "只养活过 3 年的多肉"),
    ("13900000006", "晨晨和豆豆", "🐱", "广州", "和娃一起养的第一盆植物"),
]

# (作者下标, 分类, 话题, 正文, [(评论者下标, 评论), ...])
POSTS = [
    (0, "share", "#最美绿植大赛#",
     "我家龟背竹开花了！！养了整整 8 个月，每天看传感器数据调光照，终于等到这一天 🥹 大家有遇到过吗？",
     [(1, "恭喜！龟背竹开花很少见，说明环境真的稳👍"),
      (2, "羡慕，我家那盆还在装绿植🌿")]),
    (1, "help", "#救救我的植物#",
     "【新手必看】多肉浇水口诀：干透浇透，不干不浇。用植小伴测土壤水分，低于 20% 再浇就对了 👇",
     [(3, "学到了，我之前一周浇一次，难怪烂根")]),
    (2, "share", "",
     "土壤湿度掉到 20% 以下就要浇水啦，我家这盆昨天刚浇过，今天回弹到 45%，传感器真准。",
     []),
    (3, "help", "",
     "求助：多肉叶片发软、底部化水，是不是水浇多了？已经停水一周了，下一步该怎么办😭",
     [(1, "先脱盆晾根两天，换颗粒土，别再浇水了"),
      (0, "我上次也是这样，救回来了，别慌～")]),
    (4, "daily", "",
     "和娃一起给植物换盆，他负责扶苗我负责填土，弄完两个人都一身泥😂 记录一下第 30 天。",
     [(2, "太有爱了！")]),
    (0, "daily", "",
     "21 天养植挑战打卡第 12 天，今天叶子又冒出一片新的嫩芽🌱 坚持真的有用。",
     []),
    (1, "share", "",
     "阳台改造完成！把喜光的都挪到了南边，耐阴的放北边，一周下来状态肉眼可见地好起来。",
     [(3, "求个阳台照片，我也想改")]),
]

COMMENTS_ON_DEMO = [
    (1, "第一次见有人给绿萝建档打卡，专业👍"),
    (4, "一起加油，我的也刚到家！"),
]


def ensure_community(verbose=False):
    db.init_db()
    if db.one("SELECT COUNT(*) n FROM posts")["n"] > 0:
        return {"skipped": True}
    now = utils.bj_now()
    ids = []
    for i, (phone, nick, avatar, city, bio) in enumerate(USERS):
        u = db.one("SELECT * FROM users WHERE phone=?", (phone,))
        if not u:
            uid = utils.new_id("u")
            db.exe("INSERT INTO users(id,phone,nickname,role,demo,avatar,city,"
                   " bio,created_at) VALUES(?,?,?,?,?,?,?,?,?)",
                   (uid, phone, nick, "parent", 1, avatar, city, bio, now[0]))
            u = db.one("SELECT * FROM users WHERE id=?", (uid,))
        ids.append(u["id"])

    post_ids = []
    for (ai, cat, topic, text, comments) in POSTS:
        pid = utils.new_id("po")
        ts = "%s %s" % (utils.date_add(now[1], -(ai + 1)),
                        ["09:12:00", "14:30:00", "20:05:00", "11:20:00",
                         "17:48:00", "08:30:00", "21:15:00"][ai])
        db.exe("INSERT INTO posts(id,user_id,plant_id,category,text,media_ids,"
               " topic,like_count,comment_count,created_at)"
               " VALUES(?,?,?,?,?,?,?,?,?,?)",
               (pid, ids[ai], None, cat, text, "[]", topic, 0, 0, ts))
        post_ids.append(pid)
        # 点赞：除作者外每个邻居都点一次（数字是真数出来的）
        for j, uid in enumerate(ids):
            if j == ai:
                continue
            db.exe("INSERT OR IGNORE INTO post_likes(post_id,user_id,created_at)"
                   " VALUES(?,?,?)", (pid, uid, ts))
            db.exe("UPDATE posts SET like_count=like_count+1 WHERE id=?", (pid,))
        for (ci, ctext) in comments:
            if ci == ai:
                continue
            db.exe("INSERT INTO post_comments(id,post_id,user_id,text,"
                   " created_at) VALUES(?,?,?,?,?)",
                   (utils.new_id("cm"), pid, ids[ci], ctext, ts))
            db.exe("UPDATE posts SET comment_count=comment_count+1 WHERE id=?",
                   (pid,))

    # 演示账号（小明）自己发一条，让"我的帖子"里有东西，并带上真实照片
    demo = db.one("SELECT * FROM users WHERE phone=?", ("13800000001",))
    if demo:
        plant = db.one("SELECT * FROM plants WHERE user_id=? LIMIT 1",
                       (demo["id"],))
        media = (db.q("SELECT id FROM media WHERE plant_id=? AND kind='image'"
                      " ORDER BY created_at DESC LIMIT 2", (plant["id"],))
                 if plant else [])
        pid = utils.new_id("po")
        ts = "%s 10:00:00" % utils.date_add(now[1], -1)
        db.exe("INSERT INTO posts(id,user_id,plant_id,category,text,media_ids,"
               " topic,like_count,comment_count,created_at)"
               " VALUES(?,?,?,?,?,?,?,?,?,?)",
               (pid, demo["id"], plant["id"] if plant else None, "share",
                "小绿绿到家第 28 天啦！今天土壤水分 15.7%，浇了 200ml，瞬间精神了🌱",
                json.dumps([m["id"] for m in media]), "", 0, 0, ts))
        post_ids.append(pid)
        for j, uid in enumerate(ids):
            db.exe("INSERT OR IGNORE INTO post_likes(post_id,user_id,created_at)"
                   " VALUES(?,?,?)", (pid, uid, ts))
            db.exe("UPDATE posts SET like_count=like_count+1 WHERE id=?", (pid,))
        for (ci, ctext) in COMMENTS_ON_DEMO:
            db.exe("INSERT INTO post_comments(id,post_id,user_id,text,"
                   " created_at) VALUES(?,?,?,?,?)",
                   (utils.new_id("cm"), pid, ids[ci], ctext, ts))
            db.exe("UPDATE posts SET comment_count=comment_count+1 WHERE id=?",
                   (pid,))

    # 关注关系：邻居们互相关注 + 都关注演示账号
    for i, a in enumerate(ids):
        for j, b in enumerate(ids):
            if i != j and (i + j) % 2 == 0:
                db.exe("INSERT OR IGNORE INTO user_follows(follower_id,"
                       " followee_id,created_at) VALUES(?,?,?)", (a, b, now[0]))
    if demo:
        for uid in ids:
            db.exe("INSERT OR IGNORE INTO user_follows(follower_id,followee_id,"
                   " created_at) VALUES(?,?,?)", (uid, demo["id"], now[0]))
            db.exe("INSERT OR IGNORE INTO user_follows(follower_id,followee_id,"
                   " created_at) VALUES(?,?,?)", (demo["id"], uid, now[0]))

    if verbose:
        print("[community] seeded users=%d posts=%d" % (len(ids), len(post_ids)))
    return {"users": len(ids), "posts": len(post_ids)}