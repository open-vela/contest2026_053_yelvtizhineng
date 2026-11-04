# -*- coding: utf-8 -*-
"""AI 编排路由：诊断、文字问答、语音会话（session 内存态，V1 够用）。"""
import os
import json
import itertools
import functools
import struct
import threading
import time
from concurrent.futures import ThreadPoolExecutor, TimeoutError

from fastapi import APIRouter, Header, HTTPException, Query, Request, Response
from pydantic import BaseModel
from starlette.responses import StreamingResponse

from .. import config, db, ratelimit, utils
from ..services import media_store, plant_ops, qa_bank, interaction_intents, voice_stream, voice_context, voice_hint
from ..services.ai_gateway import UpstreamError, gateway, voice_tts_text
from . import either, resolve_plant
from ..services import streaming_asr, consensus_asr

_STREAM_SHADOW = streaming_asr.get_shadow_engine()
_STREAM_MODE = os.environ.get('PLANT_STREAM_ASR_MODE','off')
_STREAM_OWNER = os.environ.get('PLANT_STREAM_ASR_OWNER','')
def _device_alias_enabled():
    return bool(_STREAM_OWNER and os.environ.get('PLANT_DEVICE_ALIAS_MODE','off')=='on')

def _resolve_session_asr(s,text,names=()):
    if s.get('power_sleep'):
        power=interaction_intents.resolve_power(text, _device_alias_enabled() and s.get('owner')==_STREAM_OWNER)
        if power:return power
    if _device_alias_enabled() and s.get('owner')==_STREAM_OWNER:
        return interaction_intents.resolve_asr_device_alias(text,names)
    return interaction_intents.resolve_asr(text,names)


def _stream_command(text):
    # This predicate is consumed only by the existing owner-restricted race.
    hit=(interaction_intents.resolve_asr_device_alias(text,[]) if _device_alias_enabled() else interaction_intents.resolve_asr(text,[]))
    if not hit:return None
    if hit['action'] in {'look','home','inspect'}:return hit
    if hit['action']=='page' and hit['arg'] in {'camera','home'}:return hit
    return None


_STREAM_CLOUD_POOL=ThreadPoolExecutor(max_workers=2,thread_name_prefix='stream_asr_fallback')
_STREAM_CLOUD_SLOTS=threading.BoundedSemaphore(2)
def _stream_select(shadow,wav):
    """Race a sealed command against the one normal ASR request.

    General questions never wait for local decoding. Losing cloud requests do
    no hardware work; at most two remain in flight, and each is made once.
    """
    def local():
        value=shadow.peek()
        return value['text'] if value and _stream_command(value['text']) else None
    text=local()
    if text:return text,True
    if shadow.invalid or not _STREAM_CLOUD_SLOTS.acquire(blocking=False):
        return (gateway.transcribe(wav) or '').strip(),False
    transcribe=gateway.transcribe
    def call():
        try:return (transcribe(wav) or '').strip()
        finally:_STREAM_CLOUD_SLOTS.release()
    try:future=_STREAM_CLOUD_POOL.submit(call)
    except RuntimeError:
        _STREAM_CLOUD_SLOTS.release()
        return (transcribe(wav) or '').strip(),False
    while not future.done():
        if shadow.done.wait(.005):
            text=local()
            if text:return text,True
            break
    return future.result(),False


def _intent_stream_for_session(s):
    kwargs={'trace':s.get('prepare_trace',0)}
    if (_STREAM_OWNER and s.get('owner')==_STREAM_OWNER and
        os.environ.get('PLANT_INTENT_FIRST_CONTENT_MODE','off')=='on'):
        kwargs['first_content_seconds']=2.0 if os.environ.get('PLANT_INTENT_FIRST_CONTENT_FAST_MODE','off')=='on' else 5.0
    return functools.partial(gateway.device_intent_stream,**kwargs)


router = APIRouter(prefix="/ai", tags=["AI"])
voice_router = APIRouter(prefix="/voice", tags=["AI语音"])

_S = threading.local()
_SESSIONS = {}
_SESS_LOCK = threading.Lock()
_VOICE_POOL = ThreadPoolExecutor(max_workers=3, thread_name_prefix="voice_reply")
_VOICE_SLOTS = threading.BoundedSemaphore(3)
_THINKING_TEXT = "让我好好想一想。"
_THINKING_DELAY_SECONDS = 5.0
_THINKING_READY_GRACE_SECONDS = .15


class DiagIn(BaseModel):
    media_id: str
    plant_id: str = None


class ChatIn(BaseModel):
    plant_id: str = None
    text: str
    conversation_id: str = None


class VoiceSessionIn(BaseModel):
    power_sleep: bool = False
    plant_id: str = None
    interaction: bool = False
    stream_audio: bool = False
    stream_reply: bool = False


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


# Admission only: exact existing FAQ aliases avoid wasted previews for open
# questions. Build outside the upload event loop; no matcher/bank mutation.
_LOOKAHEAD_FAQ_ALIASES=frozenset(consensus_asr.canonical(raw)
    for entry in qa_bank.entries()
    for raw in ([entry.get('question','')]+list(entry.get('examples') or []))
    if raw and len(consensus_asr.canonical(raw))>=4)
_CONSENSUS_POOL=consensus_asr.Pool(lambda wav:gateway.transcribe(wav),_pcm_to_wav,
    eligible=lambda hint:hint in _LOOKAHEAD_FAQ_ALIASES)


def _cleanup_sessions():
    now = time.time()
    dead = [k for k, v in _SESSIONS.items() if now - v["created"] > 900]
    for k in dead:
        old=_SESSIONS.pop(k, None)
        if old and old.get('reply_pipeline'):old['reply_pipeline'].cancel()
        if old and old.get('stream_asr_shadow'):old['stream_asr_shadow'].cancel('session-expired')
        if old and old.get('consensus_asr'):old['consensus_asr'].cancel()
        if old and old.get('notice_prefetch'):old['notice_prefetch'].cancel()


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
                      "kind": kind, "created": time.time(), "interaction": bool(body.interaction),
                      "power_sleep": bool(body.power_sleep and kind=="device"),
                      "stream_audio": bool(body.stream_audio and kind=='device'), "owner":kind+":"+str(claims["sub"])}
    _SESSIONS[sid]['stream_reply']=bool(body.stream_reply and body.stream_audio and body.interaction and kind=='device')
    if _STREAM_SHADOW and kind=='device' and body.interaction and config.VOICE_RATE==16000 and (_STREAM_MODE=='shadow' or (_STREAM_MODE=='fast' and _STREAM_OWNER==_SESSIONS[sid]['owner'])):
        _SESSIONS[sid]['stream_asr_shadow']=_STREAM_SHADOW.start()
        if (_STREAM_MODE=='fast' and _STREAM_OWNER==_SESSIONS[sid]['owner'] and
            os.environ.get('PLANT_ASR_LOOKAHEAD_MODE','off')=='on' and
            _SESSIONS[sid].get('stream_asr_shadow')):
            _SESSIONS[sid]['consensus_asr']=consensus_asr.Turn(_CONSENSUS_POOL)
    return {"ok": True, "session_id": sid}


@voice_router.post("/upload")
async def voice_upload(request: Request, session_id: str = Query(...),
                       seq: int = Query(0),
                       authorization: str = Header(None),
                       x_device_token: str = Header(None)):
    s = _owned_voice_session(session_id, authorization, x_device_token)
    chunk = await request.body()
    if chunk:
        s["pcm"].extend(chunk)
        shadow=s.get("stream_asr_shadow")
        if shadow:shadow.feed(seq,chunk)
        preview=s.get('consensus_asr')
        if preview:preview.observe(s['pcm'],shadow)
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


_ASR_FAQ_ALIAS_INDEX=None
_ASR_FAQ_ALIAS_LOCK=threading.Lock()

def _faq_alias_scope(s):
    return bool(s.get('interaction') and _STREAM_OWNER and
                s.get('owner')==_STREAM_OWNER and
                os.environ.get('PLANT_ASR_FAQ_ALIAS_MODE','off')=='on')

def _session_asr_failed(s,said):
    if not said:return True
    if "没听清" not in said:return False
    if not (s.get('interaction') and _STREAM_OWNER and s.get('owner')==_STREAM_OWNER and
            os.environ.get('PLANT_REPEAT_LAST_MODE','off')=='on'):
        return True
    from ..services import repeat_last
    return repeat_last.is_failure_marker(said)

def _repeat_session_hit(s,said,names=()):
    if not (s.get('interaction') and _STREAM_OWNER and s.get('owner')==_STREAM_OWNER and
            os.environ.get('PLANT_REPEAT_LAST_MODE','off')=='on'):
        return None
    key=(s.get('owner'),s.get('plant_id'),said)
    saved=s.get('_repeat_lookup')
    if saved is not None and saved[0]==key:return saved[1]
    try:
        from ..services import repeat_last
        if not repeat_last.is_request(said,names):return None
        answer=repeat_last.complete_answer(voice_context.recent(s.get('owner'),s.get('plant_id')))
        hit={'id':'context_repeat_last','answer':answer,'_repeat_context':True} if answer else None
        s['_repeat_lookup']=(key,hit)
        if hit:
            print('[RepeatLast] '+json.dumps({'pid':os.getpid(),'trace':s.get('prepare_trace',0),'hit':True},sort_keys=True),flush=True)
        return hit
    except Exception:
        return None

def _match_session_faq(s,said,names=()):
    hit=qa_bank.match_device(said)
    if hit is None:
        repeated=_repeat_session_hit(s,said,names)
        if repeated is not None:return repeated
    if not _faq_alias_scope(s):return hit
    try:
        from ..services import asr_faq_alias
        if hit is not None:
            return hit if not asr_faq_alias.is_meta_question(said) or asr_faq_alias.definition_entry(hit) else None
        global _ASR_FAQ_ALIAS_INDEX
        if _ASR_FAQ_ALIAS_INDEX is None:
            with _ASR_FAQ_ALIAS_LOCK:
                if _ASR_FAQ_ALIAS_INDEX is None:
                    _ASR_FAQ_ALIAS_INDEX=asr_faq_alias.make_index(qa_bank)
        hit=asr_faq_alias.select(said,qa_bank,_ASR_FAQ_ALIAS_INDEX,names)
        if hit is not None and 'faq_alias_id' not in s:
            s['faq_alias_id']=hit['id']
            try:print('[ASRFAQAlias] '+json.dumps({'pid':os.getpid(),
                'trace':s.get('prepare_trace',0),'id':hit['id']},sort_keys=True),flush=True)
            except Exception:pass
        return hit
    except Exception:
        return hit

def _meta_complete(s,said,complete):
    if not _faq_alias_scope(s):return complete
    try:
        from ..services import asr_faq_alias
        if not asr_faq_alias.is_meta_question(said):return complete
        note=asr_faq_alias.META_INSTRUCTION
    except Exception:
        return complete
    def focused(system,message):
        return complete(system+'\n'+note,message)
    return focused

def _session_faq_candidates(s,said,entries):
    if not _faq_alias_scope(s):return entries
    try:
        from ..services import asr_faq_alias
        return asr_faq_alias.definition_candidates(said,entries)
    except Exception:
        return entries


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
    # 2026-09-25 秒回层（孩子等不起）：转写(≈0.6s)之后先查题库和
    # 24h 答案缓存，命中就直接回，只有真正的新问题才调大模型。
    # TTS 音频按"要念的文本"做哈希缓存 —— 题库命中时连 TTS 模型都省掉，
    # 整条 finalize 从十几秒压到 1 秒上下。
    t0 = time.time()
    source = "ai"
    action = None
    try:
        if gateway.mode == "mock":
            said, answer = gateway.understand_voice(wav)
            source = "mock"
        else:
            said = (s["prepared_said"] if "prepared_said" in s else gateway.transcribe(wav) or "").strip()
            if _session_asr_failed(s,said):
                said, answer = "", "没听清，请再说一遍"
                source = "asr_fail"
            else:
                if s.get("interaction"):
                    from .vision import engine
                    names=engine().names(s.get("owner",""))
                    action = _resolve_session_asr(s,said,names)
                hit = _match_session_faq(s,said,names) if s.get("interaction") else qa_bank.match(said)
                if action is not None:
                    answer = action["answer"]
                    source = "interaction:" + action["action"]
                elif s.get("interaction") and hit is not None:
                    answer = hit["answer"] if hit.get("_repeat_context") else qa_bank.answer_of(hit).replace("小绿绿", "小伴")
                    source = "context:repeat" if hit.get("_repeat_context") else "bank:%s" % hit.get("id", "?")
                elif s.get("interaction"):
                    candidates = qa_bank.candidates_device(said, 10)
                    candidates = _session_faq_candidates(s,said,candidates)
                    facts = [{"id": e["id"], "question": e.get("question") or (e.get("examples") or [""])[0],
                              "answer": qa_bank.answer_of(e)[:150]} for e in candidates]
                    history=voice_context.recent(s.get('owner'),s['plant_id'])
                    if s.get('stream_reply'):
                        decision,pipeline=interaction_intents.semantic_reply(
                            said,engine().names(s.get('owner','')),_meta_complete(s,said,_intent_stream_for_session(s)),
                            gateway.tts_pcm_stream,facts,history,
                            normalize=lambda text:voice_tts_text(text,max_chars=0),
                            speech_limit=config.TTS_MAX_CHARS,finish_text=voice_tts_text,metrics_trace=s.get('prepare_trace',0),metrics_origin=s.get('prepare_started_at'))
                        if pipeline:s['reply_pipeline']=pipeline
                    else:
                        decision = interaction_intents.semantic(
                            said, engine().names(s.get("owner", "")), _meta_complete(s,said,gateway.device_intent), facts,history)
                    if decision is None:
                        answer = "我没弄清你想让我做什么，换个说法告诉我好吗？"
                        source = "interaction:unclear"
                    elif decision["kind"] == "command":
                        action = decision["command"]
                        answer = action["answer"]
                        source = "interaction:semantic:" + action["action"]
                    elif decision["kind"] == "faq":
                        selected = next(e for e in candidates if e["id"] == decision["faq_id"])
                        answer = qa_bank.answer_of(selected).replace("小绿绿", "小伴")
                        source = "bank_semantic:" + selected["id"]
                    else:
                        answer = decision["answer"]
                        source = "interaction:" + decision["kind"]
                elif hit is not None:
                    answer = qa_bank.answer_of(hit)
                    source = "bank:%s" % hit.get("id", "?")
                else:
                    cached = qa_bank.cache_get(said)
                    if cached is not None:
                        answer = cached["answer"]
                        source = "cache"
                    else:
                        answer = gateway.voice_answer(said)
                        qa_bank.cache_put(said, answer)
    except UpstreamError as e:
        print("[AI] 语音理解失败：上游模型故障 %s" % str(e)[:160])
        raise HTTPException(502, "AI 服务暂时不可用（上游模型故障），请稍后再说一次")
    except Exception as e:
        raise HTTPException(502, "语音理解失败: %s" % str(e)[:200])
    t1 = time.time()
    said = (said or "").strip()
    answer = (answer or "").strip() or "我没听清，再说一次好吗？"
    text = ("转写：%s\n\n回答：%s" % (said, answer)) if said else answer
    tts = None
    spoken = voice_tts_text(answer)
    # A hardware command must not wait for cold TTS or confirmation playback.
    # Its on-screen confirmation is returned with the validated action; the
    # device executes after this short voice turn. Job results speak later.
    streaming=bool(spoken and action is None and s.get('interaction') and s.get('stream_audio'))
    if streaming:
        s['tts_text']=spoken
        if not s.get('reply_pipeline'):
            try:
                with open(voice_stream.cached_path(qa_bank.TTS_CACHE_DIR,spoken),'rb') as f:tts=f.read()
            except FileNotFoundError:pass
    if spoken and action is None and not streaming:
        try:
            tts = qa_bank.tts_cached(spoken, lambda: gateway.tts(answer))
        except Exception:
            tts = None
    t2 = time.time()
    print("[AI][Perf] asr+answer=%dms(%s) tts=%dms said=%.24s"
          % (int((t1 - t0) * 1000), source, int((t2 - t1) * 1000), said), flush=True)
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
            "reply_stream":bool(s.get('reply_pipeline')),
            "action": action["action"] if action else "",
            "arg": action["arg"] if action else "",
            "latency": {"answer_ms": int((t1 - t0) * 1000),
                        "tts_ms": int((t2 - t1) * 1000), "source": source},
            "has_audio": streaming or audio_media is not None,
            "audio_media_id": audio_media["id"] if audio_media else None,
            # 音频直链（/media/{id}/file 免鉴权）：板卡边下边播、App 直接播
            "audio_url": ("/api/v1/media/%s/file" % audio_media["id"])
                         if audio_media else None,
            "message": db.one("SELECT * FROM messages WHERE id=?", (mid,))}


def _owned_voice_session(session_id, authorization, x_device_token):
    claims, kind = either(authorization, x_device_token)
    s = _SESSIONS.get(session_id)
    if not s or s.get("owner") != kind + ":" + str(claims["sub"]):
        raise HTTPException(404, "会话不存在或已过期")
    return s


def _voice_result(s):
    with s.setdefault("result_lock", threading.Lock()):
        if "result" not in s:
            payload = finalize_voice_session(s)
            if s.get('interaction') and not s.get('reply_pipeline') and not payload.get('action') and payload.get('said'):
                voice_context.remember(s.get('owner'),s['plant_id'],payload['said'],payload['answer'])
            s.update(result=payload, reply_text=payload["text"],
                     audio_media_id=payload["audio_media_id"])
        return s["result"]


_PREPARE_TRACE=itertools.count(1)

class _VoicePrepareMetrics:
    # Local numerical trace, never an authentication/session identifier.
    STAGES={'wav','shadow','asr','context','intent','faq','shadow_metrics','submit'}
    CATEGORIES={'TimeoutError','ValueError','TypeError','KeyError','AttributeError',
                'RuntimeError','UpstreamError','HTTPError','ConnectionError',
                'ReadTimeout','ConnectTimeout','JSONDecodeError'}
    def __init__(self, trace):
        self.trace=trace;self.started=time.monotonic();self.last=self.started
        self.current=None;self.stages={};self.finished=False
    def stage(self, name):
        now=time.monotonic()
        if self.current:self.stages[self.current]=round((now-self.last)*1000)
        self.current=name if name in self.STAGES else 'unknown';self.last=now
    def finish(self, error=None):
        if self.finished:return
        self.finished=True
        # Logging must not alter result/error/slot ownership, including when
        # an exception property or stdout itself fails. No raw error text.
        try:
            now=time.monotonic()
            if self.current:self.stages[self.current]=round((now-self.last)*1000)
            category=type(error).__name__ if error is not None else 'none'
            if category!='none' and category not in self.CATEGORIES:category='Other'
            status=0;cause=error
            for _ in range(4):
                if cause is None:break
                try:
                    value=getattr(getattr(cause,'response',None),'status_code',0) or getattr(cause,'http_status',0)
                    if type(value) is int and 400<=value<=599:status=value;break
                except Exception:pass
                cause=getattr(cause,'__cause__',None)
            row={'trace':self.trace,'pid':os.getpid(),'ok':error is None,
                 'failure_stage':self.current if error is not None else 'none',
                 'category':category,'http_status':status,
                 'elapsed_ms':round((now-self.started)*1000),'stages_ms':self.stages}
            print('[VoicePrepareMetrics] '+json.dumps(row,sort_keys=True),flush=True)
        except Exception:pass


def _voice_background(s):
    try:
        if 'prepare_trace' in s:
            try:
                print('[VoiceBackgroundStart] '+json.dumps({'trace':s['prepare_trace'],
                    'pid':os.getpid(),'queue_ms':round((time.monotonic()-s['prepare_submit_at'])*1000)},sort_keys=True),flush=True)
            except Exception:pass
        return _voice_result(s)
    finally:
        _VOICE_SLOTS.release()


@voice_router.post("/prepare")
def voice_prepare(session_id: str = Query(...), authorization: str = Header(None),
                  x_device_token: str = Header(None)):
    """Only announce a genuinely slow reply, once, without delaying fast audio."""
    s = _owned_voice_session(session_id, authorization, x_device_token)
    with s.setdefault("prepare_lock", threading.Lock()):
        if s.get("prepare_failed"):
            raise HTTPException(502, "语音服务暂时不可用，请再说一次")
        started = time.monotonic()
        if "future" not in s and "result" not in s:
            if len(s["pcm"]) < 1600:
                raise HTTPException(400, "录音太短，请再说一次")
            if not _VOICE_SLOTS.acquire(blocking=False):
                raise HTTPException(503, "语音服务正忙，请稍后再试")
            s['prepare_trace']=next(_PREPARE_TRACE)
            prepare_metrics=_VoicePrepareMetrics(s['prepare_trace'])
            s['prepare_started_at']=prepare_metrics.started
            try:
                prepare_metrics.stage('wav')
                wav = _pcm_to_wav(bytes(s["pcm"]))
                t0 = time.monotonic()
                prepare_metrics.stage('shadow')
                shadow=s.get("stream_asr_shadow")
                fast_allowed=bool(shadow and _STREAM_MODE=='fast' and _STREAM_OWNER==s['owner'])
                # Hints are diagnostics only. Only a full sealed command can win.
                likely=bool(fast_allowed and _stream_command(shadow.peek_hint()))
                if shadow:shadow.seal(bytes(s['pcm']),timeout=0)
                prepare_metrics.stage('asr')
                preview=s.get('consensus_asr') if fast_allowed else None
                said=preview.select(shadow,bytes(s['pcm'])) if preview else None
                reused=said is not None
                if reused:used=False
                elif fast_allowed:said,used=_stream_select(shadow,wav)
                else:said,used=(gateway.transcribe(wav) or '').strip(),False
                s['asr_source']='full_consensus' if reused else ('stream_command' if used else 'existing_asr')
                if preview:
                    print('[ASRLookahead] '+json.dumps(dict(preview.stats,trace=s['prepare_trace'],
                        preview_bytes=preview.bytes,full_bytes=len(s['pcm'])),sort_keys=True),flush=True)
                print('[StreamASR] used=%d hint=%d wait_ms=%d fallback=%d' %
                      (used,likely,round((time.monotonic()-t0)*1000),not used and not reused),flush=True)
                s["prepared_said"] = said
                s["prepare_ms"] = round((time.monotonic() - t0) * 1000)
                prepare_metrics.stage('context')
                action = None
                names=[]
                if s.get("interaction") and said:
                    from .vision import engine
                    names=engine().names(s["owner"])
                    prepare_metrics.stage('intent')
                    action = _resolve_session_asr(s,said,names)
                prepare_metrics.stage('faq')
                hit = _match_session_faq(s,said,names) if s.get("interaction") else qa_bank.match(said)
                prepare_metrics.stage('shadow_metrics')
                if shadow:
                    candidate=shadow.peek()
                    shadow_action=(interaction_intents.resolve_asr(candidate['text'],[]) if candidate else None)
                    official_action=interaction_intents.resolve_asr(said,[])
                    metrics={'ready':bool(candidate),'same_text':bool(candidate and candidate['text']==said),
                             'same_action':bool(not used and candidate and shadow_action==official_action),
                             'seal_ms':candidate['seal_ms'] if candidate else -1,
                             'reason':shadow.reason,'production_result_replaced':bool(used or reused)}
                    s['stream_asr_shadow_metrics']=metrics
                    print('[ASRShadow] ready=%d same_action=%d seal_ms=%d' %
                          (metrics['ready'],metrics['same_action'],metrics['seal_ms']),flush=True)
                    print('[StreamASRState] reason=%s bytes=%d' % (shadow.reason or 'ok',shadow.bytes),flush=True)
                s["thinking"] = bool(said and "没听清" not in said and not action and hit is None)
                prepare_metrics.stage('submit')
                s['prepare_submit_at']=time.monotonic()
                s["future"] = _VOICE_POOL.submit(_voice_background, s)
            except Exception as error:
                prepare_metrics.finish(error)
                # This sealed turn failed before its future could be installed.
                # Keep no exception/traceback/audio reference; a new recording
                # gets a new session and can retry normally.
                s["prepare_failed"] = True
                _VOICE_SLOTS.release()
                raise HTTPException(502, "语音服务暂时不可用，请再说一次") from None
            except BaseException as error:
                prepare_metrics.finish(error)
                _VOICE_SLOTS.release()
                raise
            else:prepare_metrics.finish()
        announce = False
        if s.get("thinking") and not s.get("thinking_announced"):
            deadline = started + _THINKING_DELAY_SECONDS
            try:
                future = s.get("future")
                if future is not None:
                    future.result(timeout=max(0, deadline-time.monotonic()))
                pipeline = s.get("reply_pipeline")
                ready = (pipeline.wait_audio_ready(max(0, deadline-time.monotonic()))
                         if pipeline else True)
                if pipeline and not ready:
                    # Text is already decided. Coalesce a just-arriving first
                    # PCM block rather than start a whole spoken wait notice.
                    # No additional wait when text/model work is still pending.
                    ready=pipeline.wait_audio_ready(_THINKING_READY_GRACE_SECONDS)
                announce = not ready
            except TimeoutError:
                # A completed worker's TimeoutError is a failure, not a wait hint.
                if future is not None and future.done():
                    raise
                announce = True
            if announce:
                s["thinking_announced"] = True
                s["hint_pending_at"] = time.monotonic()
        print("[AI][WaitHint] announce=%d wait_ms=%d" %
              (announce, round((time.monotonic()-started)*1000)), flush=True)
        return {"ok": True, "thinking": announce,
                "prepare_ms": s.get("prepare_ms", 0),
                "thinking_audio": "/api/v1/voice/thinking" if announce else ""}


@voice_router.get("/thinking")
def voice_thinking(session_id: str = Query(None), authorization: str = Header(None),
                   x_device_token: str = Header(None)):
    if session_id is not None:
        s = _owned_voice_session(session_id, authorization, x_device_token)
        path = voice_stream.cached_path(qa_bank.TTS_CACHE_DIR, _THINKING_TEXT)
        return StreamingResponse(voice_hint.stream(path, s), media_type="audio/wav")
    if authorization or x_device_token:
        claims, kind = either(authorization, x_device_token)
        if kind == 'device':
            # Existing firmware strictly accepts only the fixed thinking path.
            # Claim exactly one recent notice for this authenticated device.
            s = voice_hint.claim_session(_SESSIONS, _SESS_LOCK, kind+":"+str(claims["sub"]))
            if s is None:
                return Response(voice_stream.header(0), media_type="audio/wav")
            path = voice_stream.cached_path(qa_bank.TTS_CACHE_DIR, _THINKING_TEXT)
            return StreamingResponse(voice_hint.stream(path, s), media_type="audio/wav")
    wav = qa_bank.tts_cached(_THINKING_TEXT, lambda: gateway.tts(_THINKING_TEXT))
    if not wav:
        raise HTTPException(503, "提示语音暂不可用")
    return Response(wav, media_type="audio/wav")


_RETRY_NOTICE_TEXT = "这次没有答上来，请再说一次。"


@voice_router.get("/retry")
def voice_retry(authorization: str = Header(None), x_device_token: str = Header(None)):
    # Only a precached failure notice. Never start synthesis on a failed turn.
    claims, kind = either(authorization, x_device_token)
    resolve_plant(claims, kind)
    from fastapi.responses import FileResponse
    from pathlib import Path
    path = Path(voice_stream.cached_path(qa_bank.TTS_CACHE_DIR, voice_tts_text(_RETRY_NOTICE_TEXT)))
    if not path.is_file() or not 44 < path.stat().st_size <= 192000:
        raise HTTPException(503, "提示语音暂不可用，请看屏幕")
    return FileResponse(path, media_type="audio/wav")


@voice_router.post("/finalize")
def voice_finalize(session_id: str = Query(...),
                   authorization: str = Header(None),
                   x_device_token: str = Header(None)):
    s = _owned_voice_session(session_id, authorization, x_device_token)
    # Serialize with prepare's ASR/hand-off so concurrent finalize cannot
    # restart a failed preparation while the first request is leaving.
    with s.setdefault("prepare_lock", threading.Lock()):
        if s.get("prepare_failed"):
            raise HTTPException(502, "语音服务暂时不可用，请再说一次")
        future = s.get("future")
    if future is not None:
        try:
            return future.result(timeout=90)
        except TimeoutError:
            raise HTTPException(504, "回答等待超时，请稍后再试")
    return _voice_result(s)


@voice_router.get("/audio")
def voice_audio(session_id: str = Query(...), authorization: str = Header(None),
                x_device_token: str = Header(None)):
    s = _SESSIONS.get(session_id)
    if s and s.get('reply_pipeline'):
        s=_owned_voice_session(session_id,authorization,x_device_token)
        if s.get('reply_audio_complete'):
            path=voice_stream.cached_path(qa_bank.TTS_CACHE_DIR,s['tts_text'])
            if os.path.exists(path):
                with open(path,'rb') as f:return Response(f.read(),media_type='audio/wav')
        if s.get('reply_audio_started'):raise HTTPException(409,'本轮流式回答已播放或中断')
        lock=s.setdefault('audio_lock',threading.Lock())
        if not lock.acquire(blocking=False):raise HTTPException(409,'正在播放此音频')
        s['reply_audio_started']=True
        def pipeline_body():
            pipeline=s['reply_pipeline'];parts=[];source=pipeline.pcm()
            try:
                first=next(source)
                yield voice_stream.header(voice_stream.MAX_PCM)
                parts.append(first);yield first
                for pcm in source:parts.append(pcm);yield pcm
                complete=pipeline.completed()
                if not complete or complete['kind']!='chat':raise ValueError('回答未完成')
                answer=complete['answer'];said=s['result']['said']
                text=('转写：%s\n\n回答：%s'%(said,answer)) if said else answer
                spoken=voice_tts_text(answer)
                if spoken!=pipeline.spoken:raise ValueError('回答音频与文本不一致')
                voice_stream.commit_pcm(spoken,parts,qa_bank.TTS_CACHE_DIR)
                s['tts_text']=spoken;s['reply_text']=text
                s['result'].update(answer=answer,text=text,reply_stream=False)
                message=(s['result'].get('message') or {}).get('id')
                if message:
                    db.exe('UPDATE messages SET text=? WHERE id=?',(text,message))
                    s['result']['message']['text']=text
                voice_context.remember(s.get('owner'),s['plant_id'],said,answer)
                s['reply_audio_complete']=True
                try:
                    path=voice_stream.cached_path(qa_bank.TTS_CACHE_DIR,spoken)
                    with open(path,'rb') as f:audio=f.read()
                    media=media_store.save_audio(None,s['plant_id'],audio,meta='tts')
                    s['audio_media_id']=media['id']
                    if message:db.exe('UPDATE messages SET audio_media_id=? WHERE id=?',(media['id'],message))
                except Exception:print('[VoiceStream] sentence history archive deferred',flush=True)
            except Exception:
                if s.get('result'):
                    s['result'].update(reply_stream=False,reply_failed=True)
                raise
            finally:
                source.close();pipeline.cancel();lock.release()
        async def async_pipeline_body():
            from starlette.concurrency import run_in_threadpool
            source=pipeline_body()
            def take():
                try:return True,next(source)
                except StopIteration:return False,None
            try:
                while True:
                    available,block=await run_in_threadpool(take)
                    if not available:break
                    yield block
            finally:source.close()
        return StreamingResponse(async_pipeline_body(),media_type='audio/wav',headers={'Cache-Control':'no-store'})
    if s and s.get('tts_text'):
        s=_owned_voice_session(session_id,authorization,x_device_token)
        text=s['tts_text'];path=voice_stream.cached_path(qa_bank.TTS_CACHE_DIR,text)
        if os.path.exists(path):
            prefetched=s.pop('notice_prefetch',None)
            if prefetched is not None:prefetched.cancel()
            with open(path,'rb') as f:return Response(f.read(),media_type='audio/wav')
        lock=s.setdefault('audio_lock',threading.Lock())
        if not lock.acquire(blocking=False):raise HTTPException(409,'正在播放此音频')
        def body():
            source=None;prefetched=None
            try:
                prefetched=s.pop('notice_prefetch',None)
                if prefetched is not None and prefetched.claim():source=prefetched.blocks()
                else:
                    if prefetched is not None:prefetched.cancel()
                    source=voice_stream.stream_wav(text,gateway,qa_bank.TTS_CACHE_DIR)
                yield from source
                # Keep successful streamed replies replayable in app history.
                # Cancellation never reaches this point or publishes a cache.
                message=((s.get('result') or {}).get('message') or {}).get('id')
                if message and os.path.exists(path):
                    try:
                        with open(path,'rb') as f:audio=f.read()
                        media=media_store.save_audio(None,s['plant_id'],audio,meta='tts')
                        s['audio_media_id']=media['id']
                        db.exe('UPDATE messages SET audio_media_id=? WHERE id=?',(media['id'],message))
                    except Exception:print('[VoiceStream] history archive deferred',flush=True)
            finally:
                try:
                    if source is not None and hasattr(source,'close'):source.close()
                finally:
                    try:
                        if prefetched is not None:
                            prefetched.cancel()
                            try:print('[NoticePrefetch] '+json.dumps(prefetched.stats(),sort_keys=True),flush=True)
                            except Exception:pass
                    finally:lock.release()
        async def fixed_audio_body():
            from starlette.concurrency import run_in_threadpool
            source=body()
            def take():
                try:return True,next(source)
                except StopIteration:return False,None
            try:
                while True:
                    available,block=await run_in_threadpool(take)
                    if not available:break
                    yield block
            finally:source.close()
        return StreamingResponse(fixed_audio_body(),media_type='audio/wav',headers={'Cache-Control':'no-store'})
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


class NoticeIn(BaseModel):
    stream_audio: bool = False
    text: str


@voice_router.post("/notice")
def voice_notice(body: NoticeIn, authorization: str = Header(None),
                 x_device_token: str = Header(None)):
    claims,kind = either(authorization,x_device_token)
    plant=resolve_plant(claims,kind)
    text=(body.text or "").strip()
    if not text or len(text)>240:raise HTTPException(400,"播报文字长度不正确")
    spoken=voice_tts_text(text)
    if kind=='device' and body.stream_audio:
        sid=utils.new_id('vs');_cleanup_sessions()
        _SESSIONS[sid]={'plant_id':plant['id'],'owner':kind+':'+str(claims['sub']),
                        'created':time.time(),'tts_text':spoken,'reply_text':text}
        from ..services import vision_notice
        prefetched=vision_notice.take(kind+':'+str(claims['sub']),spoken)
        if prefetched is None and not os.path.exists(voice_stream.cached_path(qa_bank.TTS_CACHE_DIR,spoken)):
            from ..services import notice_prefetch
            prefetched=notice_prefetch.start(functools.partial(
                voice_stream.stream_wav,spoken,gateway,qa_bank.TTS_CACHE_DIR))
        if prefetched is not None:_SESSIONS[sid]['notice_prefetch']=prefetched
        return {'ok':True,'session_id':sid,'has_audio':True,'text':text}
    try:
        audio=qa_bank.tts_cached(spoken,lambda:gateway.tts(text))
    except Exception:
        raise HTTPException(502,"暂时不能播报，请看屏幕提示")
    if not audio:raise HTTPException(502,"暂时不能播报，请看屏幕提示")
    media=media_store.save_audio(None,plant["id"],audio,meta="interaction_tts")
    sid=utils.new_id("vs")
    _cleanup_sessions()
    _SESSIONS[sid]={"plant_id":plant["id"],"created":time.time(),"audio_media_id":media["id"],"reply_text":text}
    return {"ok":True,"session_id":sid,"has_audio":True,"text":text}
