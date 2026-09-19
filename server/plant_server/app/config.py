# -*- coding: utf-8 -*-
"""服务器配置：全部可用环境变量覆盖，演示期默认本地运行。"""
import os
import sys

BASE_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DATA_DIR = os.environ.get("PLANT_DATA_DIR", os.path.join(BASE_DIR, "data"))
DB_PATH = os.environ.get("PLANT_DB", os.path.join(DATA_DIR, "plant.db"))
MEDIA_DIR = os.path.join(DATA_DIR, "media")


def _registry_env(name):
    """Windows 兜底：手工重启时进程可能没继承环境变量，
    此时回读用户级注册表，避免静默掉回演示模式。"""
    if not sys.platform.startswith("win"):
        return ""
    try:
        import winreg
        with winreg.OpenKey(winreg.HKEY_CURRENT_USER, "Environment") as k:
            val, _ = winreg.QueryValueEx(k, name)
            return str(val or "").strip()
    except Exception:
        return ""


# 小米 MiMo（OpenAI 兼容 chat/completions）。密钥只允许来自环境变量。
MIMO_BASE_URL = os.environ.get(
    "PLANT_MIMO_URL", "https://token-plan-cn.xiaomimimo.com/v1/chat/completions")
MIMO_API_KEY = (os.environ.get("PLANT_MIMO_KEY", "").strip()
                or _registry_env("PLANT_MIMO_KEY"))
MIMO_MODEL = os.environ.get("PLANT_MIMO_MODEL", "mimo-v2.5")
# 2026-09-12：主模型（多模态，能看图/听音）之外再配两个专用模型。
# 实测那天主模型 mimo-v2.5 整体 500，而 mimo-v2.5-pro（纯文本）和
# mimo-v2.5-asr（转写）都正常 —— 所以文本问答和语音转写各有退路。
MIMO_TEXT_MODEL = os.environ.get("PLANT_MIMO_TEXT_MODEL", "mimo-v2.5-pro")
MIMO_ASR_MODEL = os.environ.get("PLANT_MIMO_ASR_MODEL", "mimo-v2.5-asr")
TTS_MODEL = os.environ.get("PLANT_TTS_MODEL", "mimo-v2.5-tts")
TTS_VOICE = os.environ.get("PLANT_TTS_VOICE", "mimo_default")
# 2026-09-16 语音时延修复：TTS 音频原来 795KB~1.5MB（24kHz/16bit 单声道 =
# 48KB/s），板卡写 SD 时下载速率被压到 23.5KB/s → 听一句回复要等 34s。
# 三件事：① 只念"回答"不念转写；② 限制念多少字；③ 能降到 16kHz 就降
# （字节数直接砍 1/3）。三个都能用环境变量回退，出问题不用改代码。
TTS_MAX_CHARS = int(os.environ.get("PLANT_TTS_MAX_CHARS", "120"))
TTS_SAMPLE_RATE = int(os.environ.get("PLANT_TTS_SAMPLE_RATE", "16000"))
TTS_SPEED = os.environ.get("PLANT_TTS_SPEED", "").strip()
# 语音这一轮单独限长 + 关思考：板卡屏幕放得下、念得快；
# 关思考实测省 ~4s（文本问答不受影响，仍走 MAX_TOKENS/EFFORT）。
VOICE_ANSWER_TOKENS = int(os.environ.get("PLANT_VOICE_ANSWER_TOKENS", "512"))
VOICE_THINKING = os.environ.get("PLANT_VOICE_THINKING", "disabled").strip().lower()

# AI 模式: auto=有密钥走 MiMo 否则 mock | mimo=强制 MiMo | mock=强制演示回复
AI_MODE = os.environ.get("PLANT_AI_MODE", "auto").strip().lower()

# 鉴权
TOKEN_SECRET = os.environ.get("PLANT_TOKEN_SECRET", "plant-demo-secret-change-me")
TOKEN_SECRET_IS_DEFAULT = (TOKEN_SECRET == "plant-demo-secret-change-me")
TOKEN_TTL_DAYS = int(os.environ.get("PLANT_TOKEN_TTL_DAYS", "7"))
# 设备凭证单独一套：板卡只在自己开机/换服务器时才注册一次，
# 跟着用户 token 一起 7 天过期会导致「板卡到点静默掉线」（2026-09-11 修正）。
DEVICE_TOKEN_TTL_DAYS = int(os.environ.get("PLANT_DEVICE_TOKEN_TTL_DAYS", "3650"))
# 设备重新注册时，剩余有效期不足这么多就顺手补发新凭证
DEVICE_TOKEN_REFRESH_S = int(os.environ.get("PLANT_DEVICE_TOKEN_REFRESH_DAYS", "30")) * 86400
DEMO_SMS_CODE = os.environ.get("PLANT_DEMO_CODE", "123456")  # 演示固定验证码

# 公网（阿里云）安全开关
DEMO_OPEN = os.environ.get("PLANT_DEMO_OPEN", "1").strip() != "0"       # 一键演示登录
DEMO_SMS_ECHO = os.environ.get("PLANT_DEMO_SMS_ECHO", "1").strip() != "0"  # 验证码接口回显
LEGACY_BRIDGE = os.environ.get("PLANT_LEGACY_BRIDGE", "0").strip() == "1"  # 旧协议兼容层（无鉴权）
RATE_LIMIT = os.environ.get("PLANT_RATE_LIMIT", "1").strip() != "0"    # 接口限流

# 设备/传感器默认
UPLOAD_INTERVAL_S = int(os.environ.get("PLANT_UPLOAD_INTERVAL_S", "60"))
# 上报周期（秒）：服务器通过 /devices/heartbeat 下发给板卡，板卡按此执行。
# 2026-09-11：原来写 600（10min）但固件根本不看这个字段、自己写死 30s 上报，
# 两边长期不一致；现在固件改成听服务器的，默认取 60s（与心跳同频，演示够实时）。
IMAGE_W = 160
IMAGE_H = 120
# 设备端 raw565 允许的尺寸（按 payload 长度自动识别，也支持 w/h 显式指定）：
#   160x120 = 预览帧（旧固件/兜底）
#   320x240 = 拍照上传帧（2026-09-10：2×2 binning，亮部不过曝；160x120 的 4 倍像素）
#   640x480 = 旧高清拍照帧（2026-09-10 之前；逐像素输出会出粉块，保留兼容即可）
RAW565_DIMS = [(640, 480), (320, 240), (160, 120)]
RAW565_MAX_BYTES = max(w * h * 2 for (w, h) in RAW565_DIMS)
DEVICE_HEARTBEAT_S = 60  # 板卡心跳周期（固件 ui_app.c UI_HB_INTERVAL_SEC 默认 60）
OFFLINE_AFTER_S = 150  # 超过 2.5 个心跳周期无任何请求判定离线（容忍一次丢包）
CACHE_EVENTS_MAX = 50  # 设备断网补传上限（规格 DR-11）
VOICE_RATE = 16000
AI_DAILY_QUOTA = int(os.environ.get("PLANT_AI_DAILY_QUOTA", "100"))

# 会员门禁：打开后，自动执行等高级功能要 PRO 及以上套餐。
# 演示期默认关（不然演示账号没开会员就点不动自动浇水）。
BILLING_ENFORCE = os.environ.get("PLANT_BILLING_ENFORCE", "0").strip() == "1"
# 自动执行的默认规则（板卡没单独配时用这套）
AUTO_WATER_MOISTURE_BELOW = int(os.environ.get("PLANT_AUTO_WATER_BELOW", "25"))
AUTO_WATER_COOLDOWN_MIN = int(os.environ.get("PLANT_AUTO_WATER_COOLDOWN_MIN", "30"))

for _d in (DATA_DIR, MEDIA_DIR, os.path.join(MEDIA_DIR, "raw"),
           os.path.join(MEDIA_DIR, "image"), os.path.join(MEDIA_DIR, "audio")):
    os.makedirs(_d, exist_ok=True)
