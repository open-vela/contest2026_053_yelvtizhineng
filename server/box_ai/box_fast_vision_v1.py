"""BOX-only complete image diagnosis, using the existing bounded SSE transport."""
import base64
import io
from PIL import Image
import json
import os
import re
import time

VISION_SYSTEM = (
    '你是植小伴的植物观察助手，用儿童能理解的中文。只判断当前照片，'
    '尚未确认照片与绑定传感器属于同一盆植物，因此不要套用土壤读数。'
    '只写可见现象，不凭照片断言缺水、缺肥、病原或立即浇水施肥。'
    '剪刀、肥料、药剂和电器应请大人帮助，不要品尝未知植物。'
)
VISION_PROMPT = (
    '观察主要植物，只输出完整JSON：'
    '{"name":"植物俗名或空串","match":0,"health_score":null,"summary":"一句话"}。'
    '看得出植物时填俗名，两个分数填0到100整数；品种不确定可写常见大类并在总结说明。'
    '没拍到植物或实在认不清时name为空、match为0、health_score为null。'
    'summary在25个汉字以内，含最明显的可见现象；分数>=80以状态良好、60到79以需要留意、低于60以需要帮助开头。'
    '不编造精确品种，不凭照片断言缺水缺肥。总JSON不超过140个字符。'
)



def prepare_image(jpeg):
    # Keep the full original in the media archive. The provider only needs
    # this proportional copy for a quick whole-plant observation.
    with Image.open(io.BytesIO(jpeg)) as image:
        if max(image.size) <= 640:
            return jpeg
        image.thumbnail((640, 640), Image.Resampling.LANCZOS)
        if image.mode != 'RGB':
            image = image.convert('RGB')
        output = io.BytesIO()
        image.save(output, format='JPEG', quality=90)
        return output.getvalue()


def complete_vision(gateway, payload):
    """Allow a separate standard API credential for BOX vision only."""
    url = os.environ.get('BOX_VISION_URL', '').strip()
    key = os.environ.get('BOX_VISION_KEY', '').strip()
    if not url and not key:
        return gateway.vision_inspect_complete(payload)
    if url != 'https://api.xiaomimimo.com/v1/chat/completions' or not key:
        raise ValueError('BOX vision requires its standard API URL and credential together')
    from app.services.ai_gateway import _mimo_http
    from app.services.vision_reply_stream import complete_caption
    raw, metrics = complete_caption(_mimo_http().post, url,
        {'api-key': key, 'Authorization': 'Bearer ' + key}, payload,
        total_seconds=12)
    print('[BOX Vision] dedicated_api complete_ms=%d' % metrics['complete_ms'], flush=True)
    return raw


def diagnose_fast(jpeg, gateway, validate_result):
    """Publish only a full, valid result. Never retry with reasoning enabled.

    vision_inspect_complete uses the shared 12s body watchdog, validates stop
    and DONE, caps bytes/text, and retains connection ownership through cleanup.
    No sensor HTTP fetch or multi-call fallback sits ahead of the image request.
    """
    started = time.monotonic()
    jpeg = prepare_image(jpeg)
    payload = {
        'model': gateway.model,
        'messages': [
            {'role': 'system', 'content': VISION_SYSTEM},
            {'role': 'user', 'content': [
                {'type': 'text', 'text': VISION_PROMPT},
                {'type': 'image_url', 'image_url': {
                    'url': 'data:image/jpeg;base64,' + base64.b64encode(jpeg).decode('ascii')}},
            ]},
        ],
        'max_completion_tokens': 768,
        'thinking': {'type': 'disabled'},
    }
    # No prefix callback: partially generated JSON must never become a diagnosis.
    raw = complete_vision(gateway, payload)
    match = re.fullmatch(r'\s*(\{.*\})\s*', raw or '', re.S)
    obj = json.loads(match.group(1)) if match else None
    if not isinstance(obj, dict) or not isinstance(obj.get('name'), str):
        raise ValueError('Incomplete diagnosis JSON')
    if not isinstance(obj.get('summary'), str) or not obj['summary'].strip():
        raise ValueError('Missing diagnosis summary')
    obj.setdefault('latin', '')
    obj.setdefault('problems', [])
    obj.setdefault('suggestions', [])
    if 'health_score' not in obj or not isinstance(obj.get('problems'), list) or not isinstance(obj.get('suggestions'), list):
        raise ValueError('Incomplete diagnosis fields')
    parsed = validate_result(obj, gateway._parse_diagnose(json.dumps(obj, ensure_ascii=False)))
    if parsed.get('recognized'):
        for key in ('health_score', 'match'):
            value = obj.get(key)
            if isinstance(value, bool) or not isinstance(value, int) or not 0 <= value <= 100:
                raise ValueError('Invalid diagnosis score')
        for item in obj['problems']:
            if not isinstance(item, dict) or not all(isinstance(item.get(k), str) for k in ('title', 'level', 'desc')):
                raise ValueError('Invalid diagnosis problem')
        for item in obj['suggestions']:
            if not isinstance(item, dict) or not all(isinstance(item.get(k), str) for k in ('action', 'text')):
                raise ValueError('Invalid diagnosis suggestion')
    print('[BOX Vision] complete_ms=%d recognized=%d calls=1' % (
        round((time.monotonic()-started)*1000), bool(parsed.get('recognized'))), flush=True)
    return parsed
