"""Product speech profile selected by user audition B."""
import hashlib,json
from .. import config
VOICE = '冰糖'
STYLE = '用柔软可爱、轻快有趣的年轻女声陪小朋友聊天。音高自然适中，咬字清楚，语速适中，语气有好奇心和轻柔的笑意；不过分尖细，不喊叫，不用夸张的夹子音。'
def cache_namespace(model=None, voice=None, style=None, rate=None, speed=None):
    identity=[model if model is not None else config.TTS_MODEL,
              voice if voice is not None else VOICE,
              style if style is not None else STYLE,
              rate if rate is not None else config.TTS_SAMPLE_RATE,
              speed if speed is not None else config.TTS_SPEED]
    return 'profile-'+hashlib.sha256(json.dumps(identity,ensure_ascii=False,separators=(',',':')).encode()).hexdigest()[:24]
