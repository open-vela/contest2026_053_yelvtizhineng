# -*- coding: utf-8 -*-
"""AI 编排：模型网关（provider 抽象）。默认小米 MiMo，无密钥/演示模式走 mock。

对外只暴露高层方法，任何端代码不接触密钥与模型细节。
"""
import base64
import io
import json
import re
import time

from .. import config

IMG_PROMPT = (
    "你是植小伴的植物医生。请观察图片，输出**严格 JSON**（不要 markdown、"
    "不要多余文字），字段如下："
    '{"name":"植物俗名","latin":"拉丁名或空","match":88,'
    '"health_score":0-100整数,"problems":[]或[{"title":"问题名",'
    '"level":"轻微|中度|严重","desc":"一句话解释"}],'
    '"suggestions":[{"action":"water|light|fertilize|move|prune|other",'
    '"text":"儿童友好的一句话建议"}],"summary":"儿童友好的一句话总结"}'
    "若看不出具体问题 problems 给空数组。分数>=80写'状态良好'，"
    "60-79写'需要留意'，<60写'需要帮助'放入 summary 开头。")

# 语音链路的人设：2026-09-12 起语音改成"ASR 转写 + CHAT_SYSTEM 作答"，
# 不再单独维护一份 AUDIO_PROMPT —— App 打字和对着板子说话就是同一套人设。
# （原 STT_PROMPT 一并删了：专用 ASR 模型自己注入提示词，带上 text part 会 400。）

# 2026-09-12 用户要求：反正接的是大模型，不必只聊植物 —— 别的话题（作业、
# 动物、宇宙、天气、笑话、故事）也要能聊，趣味性优先，同时保留养护专长。
# 三条底线：简短亲切（儿童友好）、不编造（不确定就说不确定）、有安全边界。
CHAT_SYSTEM = (
    "你是植小伴，小名叫小绿绿，是陪小朋友聊天、也帮着养植物的 AI 小伙伴。"
    "植物养护是你的专长，但你不只聊植物：讲笑话、讲故事、猜谜语、聊动物和宇宙、"
    "说说各地的风土人情、答作业里的问题、随便唠嗑，都要开开心心地陪他聊，"
    "不要因为跟植物无关就拒答，也别把每个话题都硬拐回养花。"
    "说话方式：中文，简短（一般 2-4 句），亲切、拟人化，多用 emoji，"
    "尽量有趣、有画面感，可以打比方、举例子，结尾偶尔反问一句把话头接下去。"
    "问作业题时讲思路、带他一步步想，不要直接甩答案。"
    "不许编造：不知道、不确定，或者需要实时信息（今天的天气、新闻、比赛结果、"
    "某个城市的具体情况）时，就老实说不知道，再告诉他去哪儿查、怎么验证。"
    "安全边界：遇到危险的事（爬高、玩火、乱动电器、乱吃药）、身体不舒服，"
    "或者不适合小朋友的话题，先温柔提醒去找爸爸妈妈或老师，再给一个安全的小建议。"
    "如果对方说的是自己养的那盆植物，就结合养护常识认真回答。")

# 2026-09-16 时延修复：语音这一轮的 system 尾巴。
# 音频字节数几乎正比于字数（约 9.5KB/字 @24kHz），所以"念多长"直接等于
# "板卡下载多久"。让模型把答案压短，是端到端最快的一刀。
VOICE_APPENDIX = (
    "\n\n【这一条是语音播报】你下面这段话会被直接念出来给他听："
    "请压到 2-3 句、大约 60 字以内，开口就给结论和最要紧的一句建议，"
    "不要分点、不要 markdown、不要复述他刚说的话。")

# TTS 不该念 emoji / 颜文字（会念成乱码或拉长音频，纯浪费流量）
_EMOJI_RE = re.compile(
    "[\U0001F000-\U0001FAFF\U00002600-\U000027BF\U0001F1E6-\U0001F1FF"
    "\U00002B00-\U00002BFF\U0000FE0F\U0000200D\U00002190-\U000021FF"
    "\U00002122\U00002139\U00002194-\U00002199\U000021A0-\U000021FF]+")


# 实测 2026-09-16：把"思考"关掉之后，模型偶尔会把「（思考中...）」当回答
# 直接吐出来（云端日志顺序：[AI] 返回空内容 → 降级关思考 → content 就是它）。
# 这种占位符念出来很难听，所以统一识别出来当"没答"处理。
_PLACEHOLDER_JUNK = re.compile(r"[\s（）()【】\[\].。…·\-_]")
_PLACEHOLDER_CORE = ("思考中", "正在思考", "thinking", "...", "……")


def looks_like_placeholder(text):
    """回答是不是空 / 只是「（思考中...）」这种占位符。

    做法：把括号、省略号、空白之类的"壳"全去掉再看剩下什么 ——
    比写一个要照顾全角半角各种组合的正则稳。"""
    t = (text or "").strip()
    if not t:
        return True
    return _PLACEHOLDER_JUNK.sub("", t).lower() in _PLACEHOLDER_CORE


def voice_tts_text(text, max_chars=None):
    """把"要念的文本"收拾干净：去 emoji、压空白、限长（默认 120 字）。

    2026-09-16：TTS 字节数 ≈ 9.5KB/字，795KB 一条音频在板卡 SD 上要 33s。
    念得少一点、短一点，是整条语音链路最省时间也最不伤体验的一环。
    限长时尽量断在句末，避免念到一半被硬切。
    """
    t = _EMOJI_RE.sub("", text or "")
    t = re.sub(r"[ \t\u3000]+", " ", t)
    t = re.sub(r"\n{2,}", "\n", t).strip()
    n = config.TTS_MAX_CHARS if max_chars is None else max_chars
    if n and len(t) > n:
        cut = t[:n]
        for sep in ("。", "！", "？", "；", "\n", ".", "!", "?"):
            i = cut.rfind(sep)
            if i >= n // 2:
                cut = cut[:i + 1]
                break
        t = cut.strip()
    return t


MOCK_SUGGEST = [
    {"action": "water", "text": "摸摸土壤，干的话就给小绿浇水 200ml 哦💧"},
    {"action": "light", "text": "把花盆挪到有散射光的窗边晒晒太阳☀️"},
    {"action": "care", "text": "每天和它说说话、拍张照，它会长得更开心🌱"},
]

# 模型【其实没认出来】时的自我表述（实测样本：无法识别 / 未识别植物 /
# 未知 / 未知植物；prompt 里也教过它"看不出植物就说没有植物"）。
# 命中这些词的应答只算"模型说话了"，不算"识别成功" —— 否则服务器会给
# 植物写一个凭空的健康分、ai_jobs 也会记成 ok，调试时真假难分。
UNCLEAR_WORDS = ("无法识别", "未识别", "未知", "没有植物", "看不出", "看不清")


def name_is_unclear(name):
    """植物名空了，或者模型自述"认不出" → 不算一次成功识别。"""
    n = (name or "").strip()
    if not n:
        return True
    return any(w in n for w in UNCLEAR_WORDS)


def _clean_stt(text):
    """把转写结果收拾成"一句人话"：去首尾空白与引号，多行只取第一行。"""
    t = (text or "").strip()
    for a, b in (("\u201c", "\u201d"), ("\"", "\""), ("\u300c", "\u300d")):
        if t.startswith(a) and t.endswith(b) and len(t) > len(a) + len(b):
            t = t[len(a):-len(b)].strip()
    lines = [x.strip() for x in t.splitlines() if x.strip()]
    return (lines[0] if lines else "")[:200]


class UpstreamError(RuntimeError):
    """大模型服务端故障（5xx / 连接被中断）。

    2026-09-12 实测：多模态主模型 mimo-v2.5 一度整体 500（连"你好"都失败），
    而 mimo-v2.5-pro 正常、mimo-v2.5-asr 正常。把这类错误单独标出来，
    才能跟"我们自己代码写错了"区分，也才能决定要不要换备用模型重试。
    """


def _has_media(payload):
    """请求里是否带图片/音频（带了就只能用多模态模型，没法降级到纯文本模型）。"""
    for m in payload.get("messages") or []:
        c = m.get("content")
        if isinstance(c, list):
            for part in c:
                if isinstance(part, dict) and part.get("type") in ("image_url",
                                                                   "input_audio"):
                    return True
    return False


class AiGateway:
    # 单次对话的 completion 额度。MiMo v2.5 是【推理模型】，思考过程同样占用
    # completion 额度：2026-09-10 实测额度 1024 时，部分图片的思考会把 1024
    # 全部烧完 → finish_reason=length、content 是空串 → 服务器只能回"未能
    # 识别"（最近 3 次里 2 次如此，表现成"拍啥都说识别失败"）。提到 4096 后
    # 稳定出结果。
    #
    # ⚠️ 2026-09-17 复测：`reasoning_effort="low"` 在当前 MiMo 版本上已经**压不住
    # 思考**了 —— 同一张图跑流式，现状参数下视觉请求要吐 170 KB 才收尾（纯文本
    # 对话只有 20 KB），首字节 1.9s、总耗时 58.9s，也就是时间全花在"想"上。
    # 换成 `thinking={"type":"disabled"}` 后同样一张图只吐 33 KB、总耗时 12.8s。
    # 因此图像诊断改为**默认关思考**（见 diagnose_image），失败再退回开思考。
    # 四张真实图实测：现状 22.5 / 59.8 / 85.0 / 20.0s → 关思考 13.0 / 13.1 / 9.8 / 9.1s。
    MAX_TOKENS = 4096
    EFFORT = "low"
    MAX_TOKENS = 4096
    EFFORT = "low"

    def __init__(self):
        key = config.MIMO_API_KEY
        mode = config.AI_MODE
        if mode == "mimo" and not key:
            raise RuntimeError("PLANT_AI_MODE=mimo 但未配置 PLANT_MIMO_KEY")
        if mode == "mock":
            self.mode = "mock"
        elif mode == "auto" and not key:
            self.mode = "mock"
        else:
            self.mode = "mimo"
        self.provider = "mimo" if self.mode == "mimo" else "mock"
        self.model = config.MIMO_MODEL
        if self.mode == "mock":
            print("[AI] 演示模式（mock）：未配置 PLANT_MIMO_KEY 或 PLANT_AI_MODE=mock")

    # ── MiMo 基础调用 ──
    def _mimo_post(self, payload, timeout=45):
        """打一次 MiMo。5xx / 连接被掐断 → 抛 UpstreamError（供上层决定是否降级）。"""
        import requests
        headers = {"Content-Type": "application/json",
                   "api-key": config.MIMO_API_KEY,
                   "Authorization": "Bearer " + config.MIMO_API_KEY}
        try:
            r = requests.post(config.MIMO_BASE_URL, headers=headers,
                              json=payload, timeout=timeout)
        except requests.RequestException as e:
            raise UpstreamError("连接失败 %s: %s" % (type(e).__name__, str(e)[:120]))
        if r.status_code >= 500:
            raise UpstreamError("HTTP %s %s" % (r.status_code, r.text[:120]))
        r.raise_for_status()
        j = r.json()
        msg = (j.get("choices") or [{}])[0].get("message") or {}
        return msg.get("content") or ""

    def _mimo_chat(self, payload, timeout=45):
        return self._mimo_chat_ex(payload, timeout)[0]

    def _mimo_chat_ex(self, payload, timeout=45):
        """同上，但额外告诉你"最终用的是哪个模型"（自检要如实显示降级结果）。"""
        """带降级的调用：纯文本请求在主模型 5xx 时自动换备用模型再试一次。

        图片 / 音频请求不降级 —— 备用模型（mimo-v2.5-pro）不支持多模态输入，
        换了也是白换，不如把上游故障如实抛出去。
        """
        models = [payload.get("model")]
        if not _has_media(payload):
            fb = config.MIMO_TEXT_MODEL
            if fb and fb not in models:
                models.append(fb)
        # 候选序列：主模型 → 备用模型 →（最后一招）关掉思考再来一次。
        # 关思考这招是实测总结的：模型偶尔把 completion 额度全花在思考上，
        # 接口照样 200 但 content 是空串 —— 不重试的话用户看到的就是空气泡。
        tries = [(m, {}) for m in models]
        # 调用方自己已经关思考了（图像诊断就走这条路），就别再排一遍同样的
        # 请求 —— 否则"返回空内容"时会白跑一次同样的调用。
        if (payload.get("thinking") or {}).get("type") != "disabled":
            tries.append((models[-1], {"thinking": {"type": "disabled"}}))
        err = None
        for m, extra in tries:
            p = dict(payload)
            p["model"] = m
            p.update(extra)
            try:
                out = self._mimo_post(p, timeout)
            except UpstreamError as e:
                err = e
                print("[AI] %s 调用失败（%s），试下一个" % (m, str(e)[:80]))
                continue
            if (out or "").strip() and not looks_like_placeholder(out):
                if m != payload.get("model") or extra:
                    print("[AI] 已降级：本次由 %s%s 作答"
                          % (m, "（关思考）" if extra else ""))
                return out, m
            err = UpstreamError("模型返回空内容")
            print("[AI] %s 返回空内容，试下一个" % m)
        raise err

    # ── 对外：图像诊断 ──
    def _diagnose_usable(self, raw):
        """关思考那次的结果能不能用：能解析出 JSON、且给出了植物名与分数。"""
        raw = (raw or "").strip()
        if not raw:
            return False
        try:
            m = re.search(r"\{.*\}", raw, re.S)
            obj = json.loads(m.group(0)) if m else {}
        except Exception:
            return False
        return bool(obj.get("name")) and obj.get("health_score") is not None

    def diagnose_image(self, jpeg_bytes):
        if self.mode == "mock":
            return self._mock_diagnose()
        b64 = base64.b64encode(jpeg_bytes).decode("ascii")
        content = [{"type": "text", "text": IMG_PROMPT},
                   {"type": "image_url",
                    "image_url": {"url": "data:image/jpeg;base64," + b64}}]
        base = {
            "model": config.MIMO_MODEL,
            "messages": [{"role": "user", "content": content}],
            "max_completion_tokens": self.MAX_TOKENS,
        }

        # ① 先关思考要一次：同一张图实测 58.9s → 12.8s（4.6×）。
        #    图像诊断是"看一眼给结论"的结构化抽取，不需要长思考。
        raw = self._mimo_chat(dict(base, thinking={"type": "disabled"}))

        # ② 关思考没给出可用结果（空串 / 解析不出 JSON / 没有植物名）→ 开思考
        #    再要一次：宁可慢，也不能给用户一个空洞的结论。
        if not self._diagnose_usable(raw):
            print("[AI] 关思考未给出可用诊断，开思考重试一次（这次会明显变慢）")
            raw = self._mimo_chat(dict(base, reasoning_effort=self.EFFORT))

        return self._parse_diagnose(raw)

    def _parse_diagnose(self, raw):
        raw = (raw or "").strip()
        try:
            m = re.search(r"\{.*\}", raw, re.S)
            obj = json.loads(m.group(0)) if m else {}
        except Exception:
            obj = {}
        if not raw or not obj:
            # 模型没有返回可解析内容：不能伪装成一次成功诊断。
            return {"result": {
                "name": "", "latin": "", "match": 0,
                "health_score": -1, "problems": [], "suggestions": [],
                "summary": "AI 这次没有识别出结果，请对准植物再拍一张清晰的照片试试",
                "raw_model_text": raw}, "recognized": False}
        problems = obj.get("problems") or []
        sugg = obj.get("suggestions") or []
        try:
            score = int(obj.get("health_score", 75))
            score = max(0, min(99, score))
        except (TypeError, ValueError):
            score = -1
        try:
            match = int(obj.get("match", 0))
        except (TypeError, ValueError):
            match = 0
        name = obj.get("name") or ""
        return {
            "recognized": not name_is_unclear(name),
            "result": {
                "name": name,
                "latin": obj.get("latin") or "",
                "match": match,
                "health_score": score,
                "problems": problems,
                "suggestions": sugg,
                "summary": obj.get("summary") or raw[:200],
                "raw_model_text": raw}}

    def _mock_diagnose(self):
        score = 78
        return {"result": {
            "name": "绿萝", "latin": "Epipremnum aureum", "match": 88,
            "health_score": score,
            "problems": [{"title": "叶片边缘略干", "level": "轻微",
                          "desc": "可能最近空气有点干，喷喷水雾就好啦"}],
            "suggestions": MOCK_SUGGEST,
            "summary": "整体状态不错😊 记得按时浇水、多晒太阳哦。",
            "raw_model_text": "[mock] 离线演示回复"}}

    # ── 对外：语音理解（板卡/语音对话这条线） ──
    def understand_voice(self, wav_bytes):
        """ASR 转写 + 文本模型作答，拆成 (转写, 回答) 两段返回。

        2026-09-12：以前是一把梭给多模态模型（mimo-v2.5）同时做转写和回答，
        上游一挂整条语音链路就全断（板卡那边看着像"语音没上传成功"，其实是
        上传成功、理解阶段返回 502）。拆成两步之后：转写只依赖 ASR 模型
        （0.6s），回答走带降级的文本模型。

        2026-09-16 时延修复：再把两段分开往外给 —— TTS 只念"回答"。
        以前把"转写：XXX 回答：YYY"整段喂 TTS，等于让机器把用户自己的话
        又念一遍，音频白白翻倍（实测一条 795KB 里有近一半是转写）。
        """
        if self.mode == "mock":
            return ("它今天状态怎么样",
                    "我在离线演示模式，连上网络后我就能真正听懂并回答你啦")
        said = (self.transcribe(wav_bytes) or "").strip()
        if not said or "没听清" in said:
            return "", "没听清，请再说一遍"
        try:
            answer = self.chat_text(said, voice=True)
            if looks_like_placeholder(answer):
                # 关思考偶尔只吐个「（思考中...）」→ 放开思考重来一次。
                # 宁可多等 3~4 秒，也不能让喇叭念一句"思考中"出来。
                print("[AI] 语音回答疑似占位符(%s)，放开思考重试"
                      % (answer or "")[:20])
                answer = self.chat_text(said)
                if looks_like_placeholder(answer):
                    answer = "我刚刚没想清楚，你再说一遍好吗？"
        except Exception:
            answer = "我这边想事情卡住了，等会儿再问我一次好吗？"
        return said, answer

    def audio_understand(self, wav_bytes):
        """兼容壳：仍返回"转写：… 回答：…"一整段（自检等旧调用方在用）。"""
        said, answer = self.understand_voice(wav_bytes)
        return ("转写：%s\n\n回答：%s" % (said, answer)) if said else answer

    # ── 对外：纯语音转写（只出文字，不做回答） ──
    def transcribe(self, wav_bytes):
        """用专用 ASR 模型转写（只发音频，不能带 text part —— 实测带上就 400
        "ASR request must not include text parts"）。它比多模态模型快得多：
        板卡 5 秒录音实测 0.6s 出结果。"""
        if self.mode == "mock":
            return "它今天状态怎么样"
        b64 = base64.b64encode(wav_bytes).decode("ascii")
        payload = {
            "model": config.MIMO_ASR_MODEL,
            "messages": [{"role": "user", "content": [
                {"type": "input_audio",
                 "input_audio": {"data": "data:audio/wav;base64," + b64}}]}],
            "max_completion_tokens": self.MAX_TOKENS,
        }
        return _clean_stt(self._mimo_chat(payload, timeout=60))

    # ── 对外：文本问答 ──
    def chat_text(self, text, history=None, context=None, voice=False):
        """context：可选的背景补充（比如"用户养的那盆植物 + 最近一次读数"），
        会拼进 system 里。没有它也能聊 —— 没绑植物的新用户照样能随便问。

        voice=True（2026-09-16）：这一条回答是要念出来的。多加一段
        "说短一点"的要求、单独限长，并默认关掉思考（省 ~4s）。
        只在语音链路上生效，App 打字问答的行为一字不变。
        """
        if self.mode == "mock":
            return self._mock_chat(text)
        system = CHAT_SYSTEM + ("\n\n" + context if context else "")
        if voice:
            system += VOICE_APPENDIX
        messages = [{"role": "system", "content": system}]
        for h in (history or [])[-10:]:
            messages.append({"role": h.get("role"), "content": h.get("text")})
        messages.append({"role": "user", "content": text})
        payload = {"model": config.MIMO_MODEL, "messages": messages,
                   "max_completion_tokens": (config.VOICE_ANSWER_TOKENS
                                             if voice else self.MAX_TOKENS),
                   "reasoning_effort": self.EFFORT}
        if voice and config.VOICE_THINKING:
            payload["thinking"] = {"type": config.VOICE_THINKING}
        return self._mimo_chat(payload)

    # 没配密钥时的兜底（演示模式）：也给几类非植物话题的样例回复，
    # 免得离线演示时一问"讲个笑话"就回一句植物话术，看着像坏了。
    def _mock_chat(self, text):
        # ① 植物（专长）
        if any(k in text for k in ("浇", "水", "渴")):
            return "它有点渴啦💧 手指插进土里 2 厘米，干的话就浇 200ml 吧！"
        if any(k in text for k in ("晒", "光", "暗", "阳")):
            return "它喜欢明亮的散射光☀️ 放到窗边，别让正午太阳直晒就好～"
        if any(k in text for k in ("黄", "病", "虫", "叶")):
            return "别担心🥺 先把发黄的叶子轻轻剪掉，再按诊断建议调整光照和浇水，很快会恢复的。"
        if any(k in text for k in ("怎么养", "怎么照顾", "怎么种")):
            return "养好它很简单🌱：见干浇水、多晒太阳、每周拍张照，我会一直提醒你！"
        # ② 其他话题（放开后新增）
        if any(k in text for k in ("笑话", "搞笑", "逗我")):
            return "来一个🤣 小番茄为什么不去上学？因为它已经“熟”啦！再要一个就说“再来一个”。"
        if any(k in text for k in ("故事", "讲讲")):
            return "从前有颗小种子，它躲在土里数星星✨ 直到一场春雨把它叫醒……（想听后面的就说“继续”）"
        if any(k in text for k in ("谜语", "猜谜")):
            return "猜一个🔍：白天看不见，晚上满天挂，一闪一闪的，是什么呀？（提示：天上～）"
        if any(k in text for k in ("为什么", "怎么会", "是什么")):
            return "这是个好问题🤔 我现在是离线演示模式，答不了太深。连上网后我就能陪你一条条查明白啦～"
        if any(k in text for k in ("天气", "下雨", "气温")):
            return "我离线的时候看不到实时天气☁️ 你抬头看看窗外，或者问问爸爸妈妈今天几度，我再帮你出主意！"
        # ③ 养花的日常问候（"它今天状态怎么样"是 App 里第一颗快捷问句，
        #    没有读数可查，就给一句通用的养护体感）
        if any(k in text for k in ("它", "小绿", "植物", "状态", "健康", "精神")):
            return "它今天挺精神的🌱 手指插进土里 2 厘米，干了就浇透；" \
                   "放在有散光的窗边，土壤水分保持在 40~70% 最舒服～"
        return "我在听你说～植物怎么养我会，别的问题也想陪你聊：讲笑话、讲故事、猜谜语都行！"

    # ── 对外：TTS ──
    def tts(self, text):
        """合成语音。text 先用 voice_tts_text 收拾（去 emoji / 压空白 / 限长）。

        2026-09-16 时延修复：音频字节数几乎正比于字数，而板卡下载 TTS 音频
        原来是整条链路最慢的一步（795KB @23.5KB/s = 34s）。所以这里必须
        限长；调用方另外只传"回答"，不再把转写也念一遍。
        """
        if self.mode == "mock":
            return None
        spoken = voice_tts_text(text)
        if not spoken:
            return None
        audio = {"voice": config.TTS_VOICE}
        if config.TTS_SAMPLE_RATE:
            # MiMo TTS 不一定认这个字段；认就白赚 1/3 字节，不认也无害。
            audio["sample_rate"] = config.TTS_SAMPLE_RATE
        if config.TTS_SPEED:
            audio["speed"] = config.TTS_SPEED
        payload = {
            "model": config.TTS_MODEL,
            "messages": [
                # 聊天放开后念的不只是植物内容，语气改成中性（陪小朋友的感觉）
            {"role": "user", "content": "请用温柔亲切、像陪小朋友聊天的语气朗读"},
                {"role": "assistant", "content": spoken}],
            "audio": audio,
            "max_completion_tokens": self.MAX_TOKENS,
        }
        try:
            import requests
            headers = {"Content-Type": "application/json",
                       "api-key": config.MIMO_API_KEY,
                       "Authorization": "Bearer " + config.MIMO_API_KEY}
            j = requests.post(config.MIMO_BASE_URL, headers=headers,
                              json=payload, timeout=45).json()
            audio_b64 = ((j.get("choices") or [{}])[0]
                         .get("message") or {}).get("audio", {}).get("data", "")
            if not audio_b64:
                return None
            wav = base64.b64decode(audio_b64)
            if config.TTS_SAMPLE_RATE:
                # MiMo TTS 只出 24kHz（实测 sample_rate/speed 都被忽略），
                # 所以降采样得我们自己来。失败就当没降，别把音频弄丢。
                try:
                    from .audio_util import wav_to_rate
                    wav = wav_to_rate(wav, config.TTS_SAMPLE_RATE)
                except Exception as e:
                    print("[AI] TTS 降采样失败（按原样下发）: %s" % str(e)[:120])
            return wav
        except Exception:
            return None


    # ── 自检：一键回答"大模型现在到底哪一项坏了" ──
    def _probe(self, name, payload, timeout=60):
        t0 = time.time()
        try:
            out, used = self._mimo_chat_ex(payload, timeout=timeout)
            out = (out or "").strip()
            return {"name": name, "model": used, "ok": True,
                    "ms": int((time.time() - t0) * 1000),
                    "note": out.replace("\n", " ")[:80] or "返回为空"}
        except Exception as e:
            return {"name": name, "model": payload.get("model"), "ok": False,
                    "ms": int((time.time() - t0) * 1000),
                    "note": "%s: %s" % (type(e).__name__, str(e)[:160])}

    def _sample_jpeg(self):
        """自检用的小样图（64x64 纯色），不依赖库里的数据。"""
        from PIL import Image
        buf = io.BytesIO()
        Image.new("RGB", (64, 64), (76, 175, 80)).save(buf, "JPEG", quality=80)
        return buf.getvalue()

    def _sample_wav(self):
        """自检用的 1 秒静音 WAV（16k 单声道 PCM16），只验证链路通不通。"""
        import struct
        rate = config.VOICE_RATE
        n = rate * 2
        hdr = b"RIFF" + struct.pack("<I", 36 + n) + b"WAVE"
        hdr += b"fmt " + struct.pack("<IHHIIHH", 16, 1, 1, rate, rate * 2, 2, 16)
        hdr += b"data" + struct.pack("<I", n)
        return hdr + b"\x00" * n

    def probe(self):
        """逐项探一遍：文本 / 看图 / 听音 / 语音合成。

        每题都用最小样本真打一次大模型（合计约 4 次调用，十几秒），
        按需调即可 —— 管理台"服务器自检"和排查"是不是大模型挂了"用它。
        """
        if self.mode == "mock":
            return {"mode": "mock", "checks": [
                {"name": "全部", "model": "-", "ok": True, "ms": 0,
                 "note": "演示模式：不调大模型"}]}
        img = self._sample_jpeg()
        checks = [
            self._probe("文本问答", {"model": config.MIMO_MODEL, "messages": [
                {"role": "user", "content": "一句话回答：在吗"}],
                "max_completion_tokens": 256, "reasoning_effort": self.EFFORT}),
            self._probe("看图诊断", {"model": config.MIMO_MODEL, "messages": [
                {"role": "user", "content": [
                    {"type": "text", "text": "这是什么颜色？两个字回答"},
                    {"type": "image_url", "image_url": {
                        "url": "data:image/jpeg;base64," +
                               base64.b64encode(img).decode("ascii")}}]}],
                "max_completion_tokens": 256, "reasoning_effort": self.EFFORT}),
            self._probe("语音转写", {"model": config.MIMO_ASR_MODEL, "messages": [
                {"role": "user", "content": [
                    {"type": "input_audio", "input_audio": {
                        "data": "data:audio/wav;base64," +
                                base64.b64encode(self._sample_wav()).decode("ascii")}}]}],
                "max_completion_tokens": 256}),
        ]
        t0 = time.time()
        try:
            wav = self.tts("你好呀，我是小绿绿")
            checks.append({"name": "语音合成", "model": config.TTS_MODEL,
                           "ok": bool(wav), "ms": int((time.time() - t0) * 1000),
                           "note": ("返回 %d 字节音频" % len(wav)) if wav
                                   else "没有返回音频"})
        except Exception as e:
            checks.append({"name": "语音合成", "model": config.TTS_MODEL,
                           "ok": False, "ms": int((time.time() - t0) * 1000),
                           "note": str(e)[:160]})
        return {"mode": self.mode, "checks": checks}


gateway = AiGateway()
