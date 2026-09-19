# -*- coding: utf-8 -*-
"""演示用：给演示账号补一盆「证据不够」的第二盆植物。

为什么要有它
    一键理赔有几种结果，演示时最好都能演出来：
      · 小绿绿（绿萝）：证据齐全 → 免人工直接通过、秒级赔付
      · 小肉肉（多肉）：证据一般 → 转人工复核
    现场手动「添加植物」加出来的是「一张照片都没有」的极端情况（材料不足），
    演不出「资料基本齐了、需要人工看一眼」这个中间态。所以这里预置一盆。

    这盆的证据是刻意配的，分数刚好落在「转人工」区间（50~74）：
        基础 40
        + 12  有照片（2 张，但都在 11 天前 → 拿不到「最近 7 天拍过」的 +8）
        +  8  有 AI 体检（健康分 70，不高不低 → 不加不减）
        +  8  有传感器数据（水分 33~44%，没有长期缺水 → 拿不到 +8）
        +  0  打卡率 33%（0.2~0.5 之间 → 不加不减）
        = 68 分 → manual（转人工复核）

    另外这盆**故意不绑设备**，留着给「绑定设备」演示用（理赔结果里会显示「设备：未绑定」）。

幂等：同一账号下同名植物已存在就跳过；服务每次启动都会调用。
"""
import json
import os
import shutil

from .. import config, db, utils

DEMO_PHONE = "13800000001"
SECOND_NAME = "小肉肉"

PHOTO_DAYS_AGO = 11          # >7 天，避开「最近拍过照」加分
TELEMETRY_DAYS = 10
TELEMETRY_PER_DAY = 6
DIAGNOSE_SCORE = 70          # 60~79：不加不减
TASK_ROWS = [("搬到阳台晒晒太阳", "done"),
             ("浇一次水（干透浇透）", "open"),
             ("拍一张成长照", "open")]


def _photo(plant_id, uid, src, days_ago, hhmm):
    """借一张已有的真实照片复制一份，省得凭空造图。"""
    day = utils.date_add(utils.bj_now()[1], -days_ago)
    out_dir = os.path.join(config.MEDIA_DIR, "image", day.replace("-", ""))
    os.makedirs(out_dir, exist_ok=True)
    mid = utils.new_id("md")
    ext = os.path.splitext(src["path"])[1] or ".jpg"
    dst = os.path.join(out_dir, mid + ext)
    shutil.copyfile(src["path"], dst)
    thumb = ""
    tp = src.get("thumb_path")
    if tp and os.path.exists(tp):
        thumb = os.path.join(out_dir, mid + "_thumb" + ext)
        shutil.copyfile(tp, thumb)
    created = "%s %s:00" % (day, hhmm)
    db.exe("INSERT INTO media(id,plant_id,owner_user_id,kind,fmt,path,"
           "thumb_path,meta,created_at) VALUES(?,?,?,?,?,?,?,?,?)",
           (mid, plant_id, uid, "image", src.get("fmt") or "jpeg", dst,
            thumb, "seed", created))
    return mid, created


def _diagnose(media_id, created_at):
    structured = {
        "name": "多肉", "latin": "Succulent", "match": 78,
        "health_score": DIAGNOSE_SCORE,
        "problems": [{"title": "叶色偏淡", "level": "轻微",
                      "desc": "叶片颜色比刚买回来时淡了些，可能是光照不足。"}],
        "suggestions": [{"action": "light", "text": "多晒晒早晚的太阳，中午避开直射。"}],
        "summary": "总体还行，叶色有点发淡，多晒晒太阳会好一些。",
    }
    db.exe("INSERT INTO ai_jobs(id,job_type,status,provider,model,"
           "request_source,input_ref,structured_json,raw_model_text,error,"
           "created_at) VALUES(?,?,?,?,?,?,?,?,?,?,?)",
           (utils.new_id("aj"), "diagnose", "ok", "seed", "seed", "seed",
            media_id, json.dumps(structured, ensure_ascii=False), "", None,
            created_at))


def _telemetry(plant_id):
    """水分 33~44%：不算缺水，拿不到「长期缺水」的加分。"""
    now = utils.bj_now()
    n = 0
    for d in range(TELEMETRY_DAYS, 0, -1):
        day = utils.date_add(now[1], -d)
        for i in range(TELEMETRY_PER_DAY):
            ts = "%s %02d:15:00" % (day, 8 + i * 2)
            db.exe("INSERT OR IGNORE INTO telemetry(id,device_id,plant_id,ts,"
                   "moisture,temp,light,ec,source) VALUES(?,?,?,?,?,?,?,?,?)",
                   (utils.new_id("tl"), "seed-second-plant", plant_id, ts,
                    33 + (d * 7 + i * 5) % 12, 21 + (d * 3 + i) % 6,
                    3800 + i * 150, 240 + i * 5, "device"))
            n += 1
    return n


def _tasks(plant_id, uid):
    """打卡率 1/3 ≈ 33%：落在 0.2~0.5 之间，预审不加不减。"""
    now = utils.bj_now()
    for i, (content, status) in enumerate(TASK_ROWS):
        day = utils.date_add(now[1], -3 + i)
        db.exe("INSERT OR IGNORE INTO tasks(id,plant_id,date,template_id,"
               "content,source,status,completed_by,completed_at,growth_delta)"
               " VALUES(?,?,?,?,?,?,?,?,?,?)",
               (utils.new_id("tk"), plant_id, day, None, content, "template",
                status, uid if status == "done" else None,
                ("%s 09:00:00" % day) if status == "done" else None, 5))


def ensure_demo_plants(verbose=False):
    """幂等：给演示账号补一盆「转人工」档的第二盆植物。"""
    db.init_db()
    user = db.one("SELECT * FROM users WHERE phone=?", (DEMO_PHONE,))
    if not user:
        if verbose:
            print("[demo-plants] 没有演示账号，跳过")
        return {"ok": False, "reason": "no-demo-user"}

    old = db.one("SELECT * FROM plants WHERE user_id=? AND name=?",
                 (user["id"], SECOND_NAME))
    if old:
        return {"ok": True, "skipped": True, "plant_id": old["id"]}

    now = utils.bj_now()
    pid = utils.new_id("pl")
    mood, level = utils.mood_of(72)
    db.exe("INSERT INTO plants(id,user_id,device_id,species,name,started_at,"
           "location,health_score,mood,health_level,updated_at,created_at)"
           " VALUES(?,?,?,?,?,?,?,?,?,?,?,?)",
           (pid, user["id"], None, "多肉", SECOND_NAME,
            utils.date_add(now[1], -45), "阳台", 72, mood, level,
            now[0], now[0]))

    src = db.one("SELECT * FROM media WHERE kind='image' AND path IS NOT NULL"
                 " ORDER BY created_at DESC LIMIT 1")
    if src and os.path.exists(src["path"]):
        _photo(pid, user["id"], src, PHOTO_DAYS_AGO, "09:30")
        m2, c2 = _photo(pid, user["id"], src, PHOTO_DAYS_AGO, "17:40")
        _diagnose(m2, c2)
    else:
        if verbose:
            print("[demo-plants] 库里还没有照片，第二盆只能先给传感器数据")

    tel = _telemetry(pid)
    _tasks(pid, user["id"])

    if verbose:
        print("[demo-plants] 已补第二盆 %s（%s）传感器 %d 条"
              % (SECOND_NAME, pid, tel))
    return {"ok": True, "plant_id": pid, "telemetry": tel}


if __name__ == "__main__":
    print(ensure_demo_plants(verbose=True))
