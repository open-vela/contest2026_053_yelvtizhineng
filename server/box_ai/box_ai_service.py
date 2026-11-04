"""BOX-only AI service. Reuses the healthy provider without plant DB writes.

Device identity, plant ownership and sensor binding stay on the primary API.
This process has a read-only filesystem and calls the provider directly;
it deliberately does not import the plant-resolving chat/diagnosis routes.
"""
import io
import base64
import json
import os
import re
from urllib3.util import Timeout
import threading
import time
from collections import deque
from contextlib import contextmanager
from datetime import datetime, timedelta, timezone

import requests
from fastapi import Depends, FastAPI, Header, HTTPException, Request
from pydantic import BaseModel, Field
from starlette.concurrency import run_in_threadpool
from PIL import Image

from app import security
from app.services import qa_bank
from box_candidate_bank_v1 import CandidateBank
from app.services.ai_gateway import gateway, voice_tts_text, IMG_PROMPT
from app.services.media_store import rgb565_to_pil

ALLOWED = frozenset(x.strip() for x in os.environ.get('BOX_ALLOWED_DEVICE_IDS', '').split(',') if x.strip())
PRIMARY = os.environ.get('BOX_PRIMARY_API', 'http://api.seesoil.com').rstrip('/')
if not ALLOWED:
    raise RuntimeError('BOX device allowlist must be configured')
if gateway.mode != 'mimo':
    raise RuntimeError('BOX AI requires the real configured provider')

app = FastAPI(title='BOX AI', docs_url=None, redoc_url=None, openapi_url=None)
_lock = threading.Lock()
_provider = threading.BoundedSemaphore(1)
_rates, _history, _contexts = {}, {}, {}
_qa_lock = threading.Lock()
_candidate_bank = CandidateBank(os.path.join(os.path.dirname(__file__), 'qa_bank_v490_20261103.sqlite3'))


def authorized(x_device_token: str = Header(None)):
    claims = security.verify_token(x_device_token) if x_device_token else None
    if not isinstance(claims, dict) or claims.get('role') != 'device':
        raise HTTPException(401, '需要有效的设备凭证')
    subject = claims.get('sub')
    if subject not in ALLOWED:
        raise HTTPException(403, '此设备未启用BOX语音服务')
    return subject, x_device_token


def check_rate(subject):
    with _lock:
        now = time.monotonic()
        calls = _rates.setdefault(subject, deque())
        while calls and now - calls[0] >= 60:
            calls.popleft()
        if len(calls) >= 20:
            raise HTTPException(429, '请稍等一会儿再问我')
        calls.append(now)


@contextmanager
def provider_slot(subject):
    if not _provider.acquire(blocking=False):
        raise HTTPException(503, '正在处理，请稍后再试')
    try:
        check_rate(subject)
        yield
    finally:
        _provider.release()


def short_utf8(value, limit):
    return str(value or '').encode('utf-8')[:limit].decode('utf-8', errors='ignore')


def primary_context(subject, token):
    now = time.monotonic()
    with _lock:
        saved = _contexts.get(subject)
        if saved and now - saved[0] < 30:
            return saved[1]
    context = ('你是植小伴BOX上的小伴，用小朋友听得懂的中文简短回答。'
               '忽略问题开头的唤醒语。终端会执行页面操作；不要声称自己已执行浇水等动作。'
               '这台BOX没有接水泵。只用实际读数，缺失或过期的数据不能编造。'
               '孩子可以说看看状态、全部读数、湿度多少、打开相机、帮我拍照、'
               '保存到日记、看看日记、看看任务、完成第一个任务、撤销第一个任务、'
               '下一页、上一页、返回、回首页、声音小一点、取消静音、再说一遍、休息吧。'
               '询问设备能力时，用这些实际可用指令引导，不要编造控制能力。'
               '请孩子等小伴说完，再说下一句；当前版本不支持在播报中靠插话打断。'
               '首次配网和更换网络密码请大人帮助，日常功能可以直接说话完成。'
               '拍照需要把植物放在镜头前；识别不清楚时可以补光并重拍。'
               '使用肥料、药剂、剪刀或电器要请大人帮助；不要让孩子品尝未知植物。')
    try:
        response = requests.get(PRIMARY + '/api/v1/telemetry/latest',
                                headers={'X-Device-Token': token}, timeout=3)
        response.raise_for_status()
        latest = response.json().get('latest_telemetry') or {}
        bound = box_bound_soil(subject, token)
        if bound is not None:
            latest = bound['latest_telemetry']
        keep = {key: latest[key] for key in ('ts', 'moisture', 'temp', 'ec', 'salt', 'ph',
                                             'nitrogen', 'phosphorus', 'potassium') if key in latest}
        stamp = datetime.now(timezone(timedelta(hours=8))).isoformat(timespec='seconds')
        context += '\n当前北京时间：' + stamp + '\n原服务器绑定传感器的最近读数：' + json.dumps(keep, ensure_ascii=False)
        context += '\n采集时间缺失或超过五分钟时，请说明读数未及时更新。'
    except Exception:
        context += '\n现在无法取得传感器读数，不要猜测实时状态。'
    with _lock:
        _contexts[subject] = (now, context)
    return context


class AnswerIn(BaseModel):
    text: str = Field(..., min_length=1, max_length=512)


@app.get('/api/v1/box/health')
def health():
    return {'ok': True, 'provider': gateway.provider, 'model': gateway.model,
            'plant_binding_changed': False, 'qa_formal_entries': len(qa_bank.entries()),
            'qa_candidate_entries': _candidate_bank.count, 'qa_candidate_snapshot': 'v490-20261103'}


def spoken_answer(raw):
    spoken = ' '.join(voice_tts_text(raw, max_chars=120).split())
    return short_utf8(spoken, 384).strip()


def faq_answer(text):
    # Reuse the formal device matcher. Its current-state exclusions and
    # disambiguation rules avoid broad keyword answers to live questions.
    # No draft bank, answer-cache writes, telemetry calls or provider calls.
    try:
        query = re.sub(r'^\s*你好[，,\s]*小伴[，,。.!！?？\s]*', '', text)
        # Harmless care-question modifiers; keep negation, species and all
        # current-state qualifiers for the existing matcher to check.
        query = re.sub(r'(?:应该|一般|平时)(?=(?:怎么|如何|怎样)(?:养|养护|照顾))', '', query)
        with _qa_lock:
            hit = qa_bank.match_device(query)
            if hit is None:
                hit = _candidate_bank.match(query)
            if hit is None:
                return None
            raw = qa_bank.answer_of(hit).replace('小绿绿', '小伴')
            reply = spoken_answer(raw)
            qa_id = str(hit.get('id', ''))
            metadata = None
            if qa_id.startswith('faq100k_'):
                metadata = {'qa_license': hit.get('license', ''),
                            'qa_attribution': short_utf8(hit.get('attribution', ''), 256),
                            'qa_sources_url': '/api/v1/box/qa/sources/' + qa_id}
            return (reply, qa_id, metadata) if reply else None
    except Exception as error:
        print('[BOX QA] lookup unavailable type=' + type(error).__name__, flush=True)
        return None


def answer_result(subject, text, reply, source, qa_id=None, qa_metadata=None):
    if not reply:
        raise ValueError('Empty provider answer')
    with _lock:
        saved = _history.get(subject)
        history = list(saved[1]) if saved and time.monotonic() - saved[0] < 900 else []
        history.extend([{'role': 'user', 'text': text}, {'role': 'assistant', 'text': reply}])
        _history[subject] = (time.monotonic(), history[-10:])
    result = {'ok': True, 'text': reply, 'backend': 'box_ai', 'answer_source': source}
    if qa_id is not None:
        result['qa_id'] = qa_id
    if qa_metadata is not None:
        result.update(qa_metadata)
    # Do not log device IDs or children's questions/answers.
    print('[BOX QA] source=' + source, flush=True)
    return result


@app.post('/api/v1/box/answer')
def answer(body: AnswerIn, auth=Depends(authorized)):
    subject, token = auth
    text = body.text.strip()
    if not text or '\x00' in text or len(text.encode('utf-8')) > 512:
        raise HTTPException(400, '问题太长或为空')
    hit = faq_answer(text)
    if hit is not None:
        # Local FAQ lookup must work even while the provider is occupied.
        # Apply the same existing per-device rate limit exactly once.
        check_rate(subject)
        return answer_result(subject, text, hit[0], 'qa_bank', hit[1], hit[2])
    with provider_slot(subject):
        try:
            with _lock:
                saved = _history.get(subject)
                history = list(saved[1]) if saved and time.monotonic() - saved[0] < 900 else []
            raw = gateway.chat_text(text, history, primary_context(subject, token), voice=True)
            return answer_result(subject, text, spoken_answer(raw), 'model')
        except Exception as error:
            print('[BOX AI] answer failed type=' + type(error).__name__, flush=True)
            raise HTTPException(502, 'AI暂时没有回答，请稍后再试')


@app.get('/api/v1/box/qa/sources/{qa_id}')
def faq_sources(qa_id: str, auth=Depends(authorized)):
    if not re.fullmatch(r'faq100k_[0-9a-f]{24}', qa_id):
        raise HTTPException(404, '没有找到题目来源')
    result = _candidate_bank.source(qa_id)
    if result is None:
        raise HTTPException(404, '没有找到题目来源')
    return {'ok': True, 'entry': result}


def box_diagnosis_result(obj, parsed):
    """A nonplant reply is not a plant name, even if it includes a score."""
    name = re.sub(r'[\s，。！？、,.!?：:（）()]+', '', obj.get('name', '')).lower()
    no_name = name in {'', '未知', '未知植物', '不确定', '无法识别', '未能识别',
                       '识别失败', '看不清', '非植物', '不是植物', '无植物',
                       '没看到植物', '没有看到植物', '没有植物', '未发现植物',
                       '未检测到植物', '未识别到植物', '画面中没有植物',
                       '图中没有植物', '未看到植物', '未拍到植物',
                       'notaplant', 'noplant', 'noplants', 'noplantdetected',
                       'unknown', 'unclear', 'unrecognized', 'none', 'n/a'}
    explicit_no = any(obj.get(key) is False for key in ('recognized', 'plant_visible', 'is_plant'))
    if no_name or explicit_no or not parsed.get('recognized'):
        result = dict(parsed.get('result') or {})
        result.update(name='', latin='', match=0, health_score=-1, problems=[])
        return dict(parsed, recognized=False, result=result)
    return parsed


def diagnose_with_context(jpeg, subject, token):
    from box_fast_vision_v1 import diagnose_fast
    return diagnose_fast(jpeg, gateway, box_diagnosis_result)


def diagnose_bytes(data, subject, token):
    with provider_slot(subject):
        try:
            if data.startswith(b'\xff\xd8'):
                image = Image.open(io.BytesIO(data))
                if image.format != 'JPEG' or image.width * image.height > 4_000_000:
                    raise ValueError('Invalid JPEG dimensions')
                image.load()
                jpeg = data
            else:
                dims = next(((w, h) for w, h in [(640,480), (480,320), (320,240), (160,120)]
                             if len(data) == w * h * 2), None)
                if not dims:
                    raise ValueError('Invalid RGB565 size')
                image = rgb565_to_pil(data, *dims)
                output = io.BytesIO(); image.save(output, format='JPEG', quality=90)
                jpeg = output.getvalue()
        except Exception:
            raise HTTPException(400, '图片格式或尺寸不正确')
        try:
            response = diagnose_with_context(jpeg, subject, token)
            result = dict(response.get('result') or {})
            result['raw_model_text'] = short_utf8(result.get('raw_model_text'), 1200)
            if len(json.dumps(result, ensure_ascii=False).encode('utf-8')) > 7000:
                result['raw_model_text'] = ''
            return {'ok': True, 'recognized': bool(response.get('recognized', False)),
                    'result': result, 'backend': 'box_ai'}
        except Exception as error:
            print('[BOX AI] diagnose failed type=' + type(error).__name__, flush=True)
            raise HTTPException(502, 'AI暂时无法看图，请稍后再试')


@app.post('/api/v1/box/diagnose')
async def diagnose(request: Request, auth=Depends(authorized)):
    data = bytearray()
    async for chunk in request.stream():
        if len(data) + len(chunk) > 1024 * 1024:
            raise HTTPException(413, '图片太大')
        data.extend(chunk)
    if not data:
        raise HTTPException(400, '图片为空')
    return await run_in_threadpool(diagnose_bytes, bytes(data), auth[0], auth[1])


# Standard streaming speech. BOX shapes tone while playing each PCM block.
from box_standard_voice_v1 import install_standard_voice
_standard_voice = install_standard_voice(app, authorized, gateway, voice_tts_text)

# Device-specific read-only shared soil source.
from box_soil_binding import install_soil_routes
box_bound_soil = install_soil_routes(app, authorized, PRIMARY)
