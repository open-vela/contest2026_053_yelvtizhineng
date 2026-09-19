# -*- coding: utf-8 -*-
"""AI 编排路由：诊断、文字问答、语音会话（session 内存态，V1 够用）。"""
import os
import struct
import threading
import time

from fastapi import APIRouter, Header, HTTPException, Query, Request, Response
from pydantic import BaseModel

from .. import config, db, ratelimit, utils
from ..services import media_store, plant_ops
from ..services.ai_gateway import UpstreamError, gateway, voice_tts_text
from . import either, resolve_plant

router = APIRouter(prefix="/ai", tags=["AI"])
voice_router = APIRouter(prefix="/voice", tags=["AI语音"])

_S = threading.local()
_SESSIONS = {}
_SESS_LOCK = threading.Lock()


class DiagIn(BaseModel):
    media_id: str
    plant_id: str = None


class ChatIn(BaseModel):
    plant_id: str = None
    text: str
    conversation_id: str = None


class VoiceSessionIn(BaseModel):
    plant_id: str = None


class SpeakIn(BaseModel):
    """手机端「把回复读出来」：任意文字 → 语音。"""
    text: str
    plant_id: str = None


# 手机端语音转写限流与体量上限：60 秒 16k 单声道 PCM16 = 1.92 MB。
STT_LIMIT = (20, 60)      # 每 IP 每分钟最多 20 次（一次调用就是一次大模型费用）
STT_MAX_PCM = 4 * 1024 * 1024

# 手机端「把回复念出来」也是一次大模型调用，同样限流（正常使用远够）
SPEAK_LIMIT = (30, 60)


def _pcm_to_wav(pcm):
    n = len(pcm)
    rate = config.VOICE_RATE
    hdr = b"RIFF" + struct.pack("<I", 36 + n) + b"WAVE"
    hdr += b"fmt " + struct.pack("<IHHIIHH", 16, 1, 1, rate, rate * 2, 2, 16)
    hdr += b"data" + struct.pack("<I", n)
    return hdr + bytes(pcm)


def _cleanup_sessions():
    now = time.time()
    dead = [k for k, v in _SESSIONS.items() if now - v["created"] > 900]
    for k in dead:
        _SESSIONS.pop(k, None)


def run_diagnose(media, plant, kind):
    """共享图像诊断流水线：/ai/diagnose 与设备兼容层 /image/analyze 复用。
    media: media 表记录；plant: 绑定植物；kind: "user"|"device"。"""
    path = media_store.resolve_path(media["path"])
    if not path or not os.path.exists(path):
        raise HTTPException(404, "图片文件缺失")
    with open(path, "rb") as f:
        jpeg = f.read()

    job_id = utils.new_id("aj")
    db.exe("INSERT INTO ai_jobs(id,job_type,status,provider,model,"
           " request_source,input_ref,created_at) VALUES(?,?,?,?,?,?,?,?)",
           (job_id, "diagnose", "running", gateway.provider, gateway.model,
            kind, media["id"], utils.bj_now()[0]))
    try:
        res = gateway.diagnose_image(jpeg)
    except Exception as e:
        db.exe("UPDATE ai_jobs SET status='error', error=? WHERE id=?",
               (str(e)[:500], job_id))
        # 上游 5xx 和"我们自己出错"分开报：前者用户等一等就好，后者要我们改代码
        if isinstance(e, UpstreamError):
            print("[AI] 诊断失败：上游模型故障 %s" % str(e)[:160])
            raise HTTPException(502, "AI 服务暂时不可用（上游模型故障），请稍后再试")
        raise HTTPException(502, "AI 诊断暂时不可用，请稍后再试")
    result = res.get("result") or {}
    import json
    recognized = bool(res.get("recognized", True))
    raw_text = (result.get("raw_model_text") or "").strip()

    # 状态如实记录（2026-09-10 用户要求：调试阶段要看真实结果）：
    #   模型返回了可解析内容     -> ok
    #   模型什么都没返回/解析不出 -> empty
    # 此前无论识别成败一律写 ok，"真识别成功"和"模型答了个空"在库里长得
    # 一模一样（那批"未识别植物/无法识别"全是 ok），排查时被带偏过。
    status = "ok" if (recognized and raw_text) else "empty"
    db.exe("UPDATE ai_jobs SET status=?, structured_json=?, "
           " raw_model_text=? WHERE id=?",
           (status, json.dumps(result, ensure_ascii=False),
            result.get("raw_model_text", "")[:2000], job_id))
    if status != "ok":
        print("[AI] 诊断未识别 job=%s status=%s reply=%.200s"
              % (job_id, status, raw_text or "<空>"))

    if recognized:
        try:
            score = int(result.get("health_score", 75))
        except (TypeError, ValueError):
            score = 75
        plant_ops.update_plant_health(plant["id"], score, "diagnose")
        plant_ops.add_event(
            plant["id"], "diagnose",
            title="AI 体检：" + (result.get("name") or plant["name"]),
            summary=result.get("summary", "体检完成"),
            media_id=media["id"], source="app" if kind == "user" else "device",
            delta=0, user_id=db.one("SELECT user_id FROM plants WHERE id=?",
                                    (plant["id"],))["user_id"])
    return {"ok": True, "diagnose_id": job_id, "recognized": recognized,
            "result": result}


@router.get("/health")
def ai_health():
    """大模型自检（免登录）：四项能力各打一次最小请求，哪一项坏了、坏在哪一步一目了然。

    2026-09-12 加的：那天板卡说"语音上传不成功"、手机说"图像识别不了"，
    查了半天才发现是上游多模态模型整体 500，服务器和密钥都是好的。
    有了这个入口，下次十秒钟就能定位。
    """
    return {"ok": True, "checks": gateway.probe()["checks"]}


@router.post("/diagnose")
def diagnose(body: DiagIn, authorization: str = Header(None),
             x_device_token: str = Header(None)):
    claims, kind = either(authorization, x_device_token)
    plant = resolve_plant(claims, kind, body.plant_id)
    media = db.one("SELECT * FROM media WHERE id=?", (body.media_id,))
    if not media or media["kind"] != "image":
        raise HTTPException(404, "图片不存在或类型错误")
    return run_diagnose(media, plant, kind)


# 传感器读数的展示口径跟手机端 App 一致（micrometer 的 EC 是 µS/cm）
_CTX_METRICS = (
    ("moisture", "土壤水分", "%", 0), ("temp", "温度", "℃", 1),
    ("ec", "EC 电导率", "µS/cm", 0), ("ph", "pH", "", 1),
    ("salt", "盐分", "mg/kg", 0), ("nitrogen", "氮 N", "mg/kg", 0),
    ("phosphorus", "磷 P", "mg/kg", 0), ("potassium", "钾 K", "mg/kg", 0))


def plant_context(plant):
    """聊天用的背景补充：这盆植物叫什么 + 最近一次真实读数。

    以前 /ai/chat 完全不带植物信息，问"它今天状态怎么样"模型只能瞎猜。
    没有植物（新用户还没添加）就返回空串，照样走通用问答。
    """
    if not plant:
        return ""
    bits = ["用户正在照顾的植物：" + (plant.get("name") or "未命名植物")]
    if plant.get("species"):
        bits.append("品种：" + str(plant["species"]))
    t = db.one("SELECT moisture,temp,ec,ph,salt,nitrogen,phosphorus,potassium,ts"
               " FROM telemetry WHERE plant_id=? ORDER BY ts DESC LIMIT 1",
               (plant["id"],))
    if t:
        vals = []
        for key, label, unit, nd in _CTX_METRICS:
            v = t.get(key)
            if v is None:
                continue
            try:
                vals.append(("%s %." + str(nd) + "f%s") % (label, float(v), unit))
            except (TypeError, ValueError):
                continue
        if vals:
            bits.append("最近一次传感器读数（%s，可能滞后）：%s"
                        % (t.get("ts") or "时间未知", "、".join(vals)))
    bits.append("聊到植物时可以结合这些真实读数；没提到的项目别编造。")
    return "【当前植物】" + " ".join(bits)


@router.post("/chat")
def chat(body: ChatIn, authorization: str = Header(None),
         x_device_token: str = Header(None)):
    claims, kind = either(authorization, x_device_token)
    # 2026-09-12：聊天放开成"什么都能聊"，没绑植物也该能问（optional=True）。
    # 只有显式传了 plant_id 才做归属校验 —— 传了错的照样报错，不静默降级。
    plant = resolve_plant(claims, kind, body.plant_id) if body.plant_id \
        else resolve_plant(claims, kind, None, optional=True)
    text = (body.text or "").strip()
    if not text:
        raise HTTPException(400, "请输入内容")
    now = utils.bj_now()
    conv_id = body.conversation_id
    if conv_id:
        conv = db.one("SELECT * FROM conversations WHERE id=?", (conv_id,))
        if not conv:
            raise HTTPException(404, "会话不存在")
        # 归属校验：以前不查，拿到别人的 conversation_id 就能把那 10 条
        # 历史消息塞进模型上下文（等于读到别人的聊天记录）。在这里补上。
        if kind == "user" and conv["user_id"] != claims["sub"]:
            raise HTTPException(403, "无权访问该会话")
    else:
        conv_id = utils.new_id("cv")
        db.exe("INSERT INTO conversations(id,plant_id,user_id,title,"
               " created_at,updated_at) VALUES(?,?,?,?,?,?)",
               (conv_id, plant["id"] if plant else None, claims["sub"],
                text[:20], now[0], now[0]))
    # ⚠️ 必须带 rowid 兜底排序：一轮的 user / assistant 用的是同一个 ts（秒级），
    # 只按 ts DESC 排时 SQLite 会把 assistant 排在 user 前面，反转后模型收到的是
    # "assistant回答 → user提问"这种倒过来的历史 —— 表现成"模型总在回答上一个问题"。
    # rowid 是插入顺序（user 先于 assistant），ts 相同时用它还原真实先后。
    history = [dict(m) for m in db.q(
        "SELECT role,text FROM messages WHERE conversation_id=? "
        "ORDER BY ts DESC, rowid DESC LIMIT 10", (conv_id,))][::-1]
    try:
        reply = gateway.chat_text(text, history, plant_context(plant))
        # 兜底：模型偶尔返回空串（思考吃满额度那类），别让用户看到空气泡
        reply = (reply or "").strip() or "我刚才走神了，再问我一次好吗？🌱"
    except Exception as e:
        db.exe("UPDATE conversations SET updated_at=? WHERE id=?",
               (now[0], conv_id))
        raise HTTPException(502, "AI 暂时开小差，请稍后再试")
    db.exe("UPDATE conversations SET updated_at=? WHERE id=?",
           (now[0], conv_id))
    uid_user = claims["sub"] if kind == "user" else None
    db.exe("INSERT INTO messages(id,conversation_id,role,text,ts) "
           "VALUES(?,?,?,?,?)",
           (utils.new_id("m"), conv_id, "user", text, now[0]))
    mid = utils.new_id("m")
    db.exe("INSERT INTO messages(id,conversation_id,role,text,ts) "
           "VALUES(?,?,?,?,?)", (mid, conv_id, "assistant", reply, now[0]))
    return {"ok": True, "conversation_id": conv_id,
            "message": db.one("SELECT * FROM messages WHERE id=?", (mid,))}


# ───────── 语音：流式上传 → 服务器理解+TTS ─────────
@voice_router.post("/session")
def voice_session(body: VoiceSessionIn, authorization: str = Header(None),
                  x_device_token: str = Header(None)):
    claims, kind = either(authorization, x_device_token)
    plant = resolve_plant(claims, kind, body.plant_id)
    _cleanup_sessions()
    sid = utils.new_id("vs")
    _SESSIONS[sid] = {"pcm": bytearray(), "plant_id": plant["id"],
                      "kind": kind, "created": time.time()}
    return {"ok": True, "session_id": sid}


@voice_router.post("/upload")
async def voice_upload(request: Request, session_id: str = Query(...),
                       seq: int = Query(0),
                       authorization: str = Header(None),
                       x_device_token: str = Header(None)):
    either(authorization, x_device_token)
    s = _SESSIONS.get(session_id)
    if not s:
        raise HTTPException(404, "会话不存在或已过期")
    chunk = await request.body()
    if chunk:
        s["pcm"].extend(chunk)
    return {"ok": True, "seq": seq, "received": len(chunk)}


@voice_router.post("/transcribe")
async def voice_transcribe(request: Request, plant_id: str = Query(None),
                           authorization: str = Header(None),
                           x_device_token: str = Header(None)):
    """手机端「语音输入」：整段 PCM16@16k 单声道直接 POST 上来，只转写、不回答。

    和设备那条 /voice/session->upload->finalize 分开：那条会顺带合成 TTS
    语音回复（慢、且会再写一条对话记录），手机端只是想把说的话变成文字，
    填进输入框后走 /ai/chat 正常问答（能带上下文）。
    """
    claims, kind = either(authorization, x_device_token)
    plant = resolve_plant(claims, kind, plant_id)
    ratelimit.hit(request, "voice_stt", *STT_LIMIT)
    pcm = await request.body()
    if len(pcm) > STT_MAX_PCM:
        raise HTTPException(413, "录音太长了，请分几次说")
    if len(pcm) < 1600:
        raise HTTPException(400, "录音太短，请再说一次")
    wav = _pcm_to_wav(pcm)
    try:
        text = (gateway.transcribe(wav) or "").strip()
    except Exception as e:
        raise HTTPException(502, "语音识别失败：%s" % str(e)[:200])
    if not text:
        raise HTTPException(400, "没听清，请再说一次")
    # 原始录音留档：识别不准时能回听（调试期很有用）
    media_store.save_audio(None, plant["id"], wav, meta="stt")
    return {"ok": True, "text": text}


def finalize_voice_session(s):
    """共享语音收尾流水线：/voice/finalize 与设备兼容层复用。
    s: 会话 dict（{pcm, plant_id, kind}），返回最终 payload dict。"""
    pcm = bytes(s["pcm"])
    if len(pcm) < 1600:
        raise HTTPException(400, "录音太短，请再说一次")
    wav = _pcm_to_wav(pcm)
    # 2026-09-16 时延修复：转写和回答分开拿。
    #   上屏/存档  = "转写：… 回答：…"（板卡屏幕和 App 历史都还要看转写）
    #   交给 TTS 念的 = 只有"回答"（以前连转写一起念，音频白翻一倍）
    try:
        said, answer = gateway.understand_voice(wav)
    except UpstreamError as e:
        print("[AI] 语音理解失败：上游模型故障 %s" % str(e)[:160])
        raise HTTPException(502, "AI 服务暂时不可用（上游模型故障），请稍后再说一次")
    except Exception as e:
        raise HTTPException(502, "语音理解失败: %s" % str(e)[:200])
    said = (said or "").strip()
    answer = (answer or "").strip() or "我没听清，再说一次好吗？"
    text = ("转写：%s\n\n回答：%s" % (said, answer)) if said else answer
    tts = None
    try:
        tts = gateway.tts(answer)
    except Exception:
        tts = None
    plant_id = s["plant_id"]
    audio_media = None
    if tts:
        audio_media = media_store.save_audio(None, plant_id, tts,
                                             meta="tts")
    # 存档 wav（调试/回听）
    media_store.save_audio(None, plant_id, wav, meta="recording")
    # 写入对话
    now = utils.bj_now()
    conv_id = db.one("SELECT id FROM conversations WHERE plant_id=? "
                     "ORDER BY updated_at DESC LIMIT 1", (plant_id,))
    if not conv_id:
        conv_id = utils.new_id("cv")
        db.exe("INSERT INTO conversations(id,plant_id,user_id,title,"
               " created_at,updated_at) VALUES(?,?,?,?,?,?)",
               (conv_id, plant_id, None, "语音对话", now[0], now[0]))
    else:
        conv_id = conv_id["id"]
    db.exe("UPDATE conversations SET updated_at=? WHERE id=?",
           (now[0], conv_id))
    db.exe("INSERT INTO messages(id,conversation_id,role,text,ts) "
           "VALUES(?,?,?,?,?)",
           (utils.new_id("m"), conv_id, "user",
            said or "（语音，没听清）", now[0]))
    mid = utils.new_id("m")
    db.exe("INSERT INTO messages(id,conversation_id,role,text,"
           " audio_media_id,ts) VALUES(?,?,?,?,?,?)",
           (mid, conv_id, "assistant", text,
            audio_media["id"] if audio_media else None, now[0]))
    return {"ok": True, "text": text, "said": said, "answer": answer,
            "has_audio": audio_media is not None,
            "audio_media_id": audio_media["id"] if audio_media else None,
            # 音频直链（/media/{id}/file 免鉴权）：板卡边下边播、App 直接播
            "audio_url": ("/api/v1/media/%s/file" % audio_media["id"])
                         if audio_media else None,
            "message": db.one("SELECT * FROM messages WHERE id=?", (mid,))}


@voice_router.post("/finalize")
def voice_finalize(session_id: str = Query(...),
                   authorization: str = Header(None),
                   x_device_token: str = Header(None)):
    either(authorization, x_device_token)
    s = _SESSIONS.get(session_id)
    if not s:
        raise HTTPException(404, "会话不存在或已过期")
    payload = finalize_voice_session(s)
    _SESSIONS[session_id]["reply_text"] = payload["text"]
    _SESSIONS[session_id]["audio_media_id"] = payload["audio_media_id"]
    return payload


@voice_router.get("/audio")
def voice_audio(session_id: str = Query(...)):
    s = _SESSIONS.get(session_id)
    mid = (s or {}).get("audio_media_id")
    if not mid:
        raise HTTPException(404, "无 TTS 音频（当前可能为演示模式）")
    p = media_store.media_path(mid)
    if not p or not os.path.exists(p):
        raise HTTPException(404, "音频文件缺失")
    with open(p, "rb") as f:
        return Response(f.read(), media_type="audio/wav")


@voice_router.post("/speak")
def voice_speak(body: SpeakIn, request: Request,
                authorization: str = Header(None)):
    """把一段文字念出来（手机端「自动朗读」用）。

    手机端拍照诊断 / 问 AI 拿到回复后调这里，服务器合成后回**免鉴权直链**，
    前端 `new Audio(url).play()` 即可 —— 小朋友不用看字也能听。

    和板卡那条 /voice/session→finalize 分开：那条是"录音进来、问答回去"，
    这条只是"给字，回声音"，不需要会话，一次请求结束。
    """
    claims, _kind = either(authorization, None)
    ratelimit.hit(request, "voice_speak", *SPEAK_LIMIT)
    text = (body.text or "").strip()
    if not text:
        raise HTTPException(400, "text 不能为空")

    try:
        wav = gateway.tts(text)
    except UpstreamError as e:
        raise HTTPException(502, "语音合成失败（上游模型）：%s" % str(e)[:120])
    except Exception as e:
        raise HTTPException(502, "语音合成失败: %s" % str(e)[:160])

    if not wav:
        # mock 模式或上游没给音频：如实告诉前端"这次念不出来"，
        # 前端静默跳过即可 —— 文字照样在屏上，不影响使用。
        raise HTTPException(503, "语音合成不可用（演示模式或上游无音频）")

    media = media_store.save_audio(claims["sub"], body.plant_id, wav,
                                   meta="tts")
    return {"ok": True,
            "audio_url": "/api/v1/media/%s/file" % media["id"],
            "chars": len(voice_tts_text(text)),
            "bytes": len(wav)}
