# -*- coding: utf-8 -*-
"""语音秒回层：题库 + AI 答案缓存 + TTS 音频缓存。

孩子的高频问题（打招呼/浇水/光照/笑话/故事……）不该每次都等
大模型十几秒。这里在 ASR 之后插一层：

1) data/qa_bank.json  —— 手工题库，命中即回（<50ms）；
2) data/qa_cache.json —— AI 答案 24h 缓存（重复问题第二次秒回）；
3) data/media/tts_cache/<md5>.wav —— 按"要念的文本"哈希缓存音频，
   题库和缓存命中时连 TTS 模型都不用调。

匹配对孩子容错：归一化（去标点/语气词）→ 关键词子串优先 →
与 examples 做 difflib 相似度兜底（默认 ≥0.72）。
"""
import difflib
import hashlib
import json
import os
import random
import re
import time
import math
from collections import defaultdict, Counter

from .. import config
from . import voice_profile

BANK_PATH = os.path.join(config.DATA_DIR, "qa_bank.json")
CACHE_PATH = os.path.join(config.DATA_DIR, "qa_cache.json")
TTS_CACHE_DIR = os.path.join(config.MEDIA_DIR, "tts_cache", voice_profile.cache_namespace())
CACHE_TTL_S = 24 * 3600
MATCH_THRESHOLD = 0.72

_FILLER_END = "呀啊呢吧吗哦喔哈啦嘛喽呗~！!？?。."
_PUNCT = re.compile(
    r"[\s，。！？、,.!?~…·\-—_（）()【】\[\]\"'“”‘’:：;；]+")

_bank = None
_cache = None
_device_index = None
_plant_slots = {}
_candidate_index = None
_spoken_index = None


def _norm(text):
    t = _PUNCT.sub("", text or "").lower()
    return t.strip(_FILLER_END)


def _load(path, default):
    try:
        with open(path, encoding="utf-8") as f:
            return json.load(f)
    except Exception:
        return default


def _save(path, obj):
    tmp = path + ".tmp"
    with open(tmp, "w", encoding="utf-8") as f:
        json.dump(obj, f, ensure_ascii=False, indent=1)
    os.replace(tmp, path)


def reload_bank():
    global _bank, _device_index, _candidate_index, _spoken_index
    data = _load(BANK_PATH, [])
    _bank = [e for e in data if e.get("answer") or e.get("answers")]
    _device_index = None
    _candidate_index = None
    _spoken_index = None
    return len(_bank)


def answer_of(entry):
    """取词条答案：带 answers 列表的词条（笑话/谜语/绕口令/故事池）
    随机挑一条，孩子每次问听到的都不一样。"""
    pool = entry.get("answers")
    if pool:
        return random.choice(pool)
    return entry.get("answer", "")


def entries():
    if _bank is None:
        reload_bank()
    return _bank


def match(text):
    """命中返回词条 dict（含 id/answer），未命中返回 None。"""
    t = _norm(text)
    if not t:
        return None
    best = None
    best_len = 0
    for e in entries():
        # 关键词子串：命中最长关键词的词条优先（"天气"比"怎么样"更具体，
        # 避免泛词抢走具体问题）
        for kw in e.get("keywords", []):
            k = _norm(kw)
            if k and k in t and len(k) > best_len:
                best, best_len = e, len(k)
    if best is not None:
        return best
    best_score = 0.0
    for e in entries():
        # examples 相似度兜底
        for ex in e.get("examples", []):
            if e.get("exact_only"):
                if t == _norm(ex):
                    return e
                continue
            r = difflib.SequenceMatcher(None, t, _norm(ex)).ratio()
            if r > best_score:
                best_score = r
                best = e
    if best is not None and best_score >= MATCH_THRESHOLD:
        return best
    return None


def _device_norm(text):
    t = re.sub(r"[呀啊呢吧吗哦喔啦嘛呗]+$", "", _PUNCT.sub("", text or "").lower())
    t = re.sub(r"^(?:(?:小伴|植小伴|请问|我想知道|告诉我))+", "", t)
    t = re.sub(r"为啥|为何", "为什么", t)
    t = re.sub(r"如何|怎样|怎么样", "怎么", t)
    t = t.replace("多长时间", "多久")
    for phrase in ("有什么用处", "有什么用途", "有什么用", "有何作用", "的作用是什么", "有哪些功能", "有什么功能"):
        t = t.replace(phrase, "有什么作用")
    if "有什么作用" in t:
        return "purpose:" + t.replace("有什么作用", "").replace("的", "")
    if "为什么" in t:
        return "why:" + t.replace("为什么", "").replace("的", "")
    if t.startswith("什么是"):
        return "what:" + t[3:]
    if t.endswith("是什么"):
        return "what:" + t[:-3]
    if "怎么" in t:
        return "how:" + t.replace("怎么", "")
    return t


def _plant_key(alias, entity):
    if not entity:
        return alias
    kind, core = alias.split(":", 1) if ":" in alias else ("", alias)
    core = core.replace(entity, "")
    if kind == "how":
        core = core.replace("给", "").replace("要", "").replace("照顾", "养").replace("养护", "养")
    return (kind + ":" if kind else "") + entity + "|" + core


def _plant_topic(text, entity):
    """Closed species/facet patterns, not arbitrary substring routing."""
    t = _PUNCT.sub("", text or "")
    t = re.sub(r"^(?:(?:小伴|植小伴|请问|我想知道|告诉我))+", "", t)
    t = re.sub(r"[呀啊呢吧吗哦喔啦嘛呗]+$", "", t.replace(entity, ""))
    patterns = {
        "care": r"(?:怎么|如何|怎样|要怎么)(?:养|养护|照顾)|(?:养护|养殖)(?:方法|要点)(?:是什么)?",
        "water": r"(?:要|需要|需不需要|要不要)?浇水|(?:多久|多长时间|几天|什么时候)浇(?:一次)?水|(?:怎么|如何)(?:给)?浇水",
        "light": r"(?:喜欢|需要|要不要|要)(?:阳光|晒太阳|光照)|(?:放在|放到|放)(?:哪里|哪儿|什么地方)好|(?:怎么|如何)晒太阳",
        "flower": r"(?:会|能|什么时候|多久)?开花|(?:开的)?花好看|会不会开花",
        "easy": r"好养|难养|难不难养|(?:新手|初学者)(?:能|可以|适合)养|适合新手(?:养)?",
    }
    for facet, pattern in patterns.items():
        if re.fullmatch(pattern, t):
            return facet
    return None


def _device_aliases():
    global _device_index, _plant_slots
    if _device_index is not None:
        return _device_index
    rows, exact, grams, slots = [], defaultdict(list), defaultdict(set), {}
    entities = {}
    for e in entries():
        if re.fullmatch(r"plant\d+_care", e.get("id", "")):
            q = (e.get("examples") or [""])[0]
            if q.endswith("怎么养"):
                entities[e["id"].split("_")[0]] = q[:-3]
    for e in entries():
        eid = e.get("id", "")
        # These old generic entries assume live plant state or toxicity.
        if eid in {"status", "water", "light", "howraise", "weather"} or eid.endswith("_poison"):
            continue
        if re.fullmatch(r"plant\d+_(?:care|water|light|flower|easy)", eid):
            entity = entities.get(eid.split("_")[0])
            if entity:
                slots[entity, eid.split("_")[1]] = e
        aliases = list(e.get("examples", []))
        if eid == "greet":
            aliases += ["你好", "您好", "哈喽", "嗨", "早上好", "下午好", "晚上好", "你好小伴"]
        for raw in aliases:
            alias = _device_norm(raw)
            if not alias:
                continue
            i = len(rows)
            entity = entities.get(eid.split("_")[0], "")
            if not entity:
                found = sorted({x for x in entities.values() if x in alias}, key=len, reverse=True)
                found = [x for x in found if not any(x != y and x in y for y in found)]
                if len(found) == 1:
                    entity = found[0]
            alias = _plant_key(alias, entity)
            rows.append((alias, e, entity))
            exact[alias].append(i)
            if not e.get("exact_only"):
                for gram in set(zip(alias, alias[1:])):
                    grams[gram].add(i)
    _plant_slots = slots
    _device_index = rows, exact, grams, set(entities.values())
    return _device_index


def _match_device_original(text):
    """Conservative FAQ lookup: confidence is similarity, not probability.
    Never match broad keyword substrings or fabricate current device state.
    """
    q = _device_norm(text)
    if not q or re.search(r"现在|实时|刚才|上次|之前|这盆|这棵|我的|我们|它今天|它.*(?:状态|健康)", q):
        return None
    if re.search(r"然后|并且|先.+再", q):
        return None
    rows, exact, grams, entities = _device_aliases()
    found = [x for x in entities if x in q]
    found = [x for x in found if not any(x != y and x in y for y in found)]
    if len(found) > 1:
        return None
    if found:
        facet = _plant_topic(text, found[0])
        if facet and (found[0], facet) in _plant_slots:
            return _plant_slots[found[0], facet]
    q = _plant_key(q, found[0]) if found else q
    if q in exact:
        ids = {rows[i][1]["id"] for i in exact[q]}
        return rows[exact[q][0]][1] if len(ids) == 1 else None
    if re.search(r"不|别|没", q):
        return None
    if len(q) < 6:
        return None
    counts = Counter()
    for gram in set(zip(q, q[1:])):
        counts.update(grams.get(gram, ()))
    scores = {}
    for i, _ in counts.most_common(60):
        alias, entry, entity = rows[i]
        if (q.split(":", 1)[0] if ":" in q else "") != (alias.split(":", 1)[0] if ":" in alias else ""):
            continue
        if entity and entity not in q:
            continue
        score = difflib.SequenceMatcher(None, q.split(":", 1)[-1], alias.split(":", 1)[-1]).ratio()
        old = scores.get(entry["id"])
        if old is None or score > old[0]:
            scores[entry["id"]] = score, entry
    ranked = sorted(scores.values(), key=lambda x: x[0], reverse=True)
    if ranked and ranked[0][0] >= .88 and (len(ranked) == 1 or ranked[0][0] - ranked[1][0] >= .06):
        return ranked[0][1]
    return None


def _spoken_norm(text):
    t = re.sub(r"[呀啊呢吧吗哦喔啦嘛呗]+$", "", _PUNCT.sub("", text or "").lower())
    t = re.sub(r"^(?:(?:小伴|植小伴|请问|我想知道|告诉我|能不能给我讲讲|能给我讲讲|能给我说说|能解释一下|可以解释一下|帮我解释一下|能告诉我))+", "", t)
    t = re.sub(r"为啥|为何", "为什么", t)
    t = re.sub(r"如何|怎样|怎么样", "怎么", t)
    t = t.replace("多长时间", "多久")
    # Fixed technical noun qualifiers, not arbitrary deletion of context.
    t = re.sub(r"电脑(?:里|里面)?(?:的)?(?:那个)?(操作系统|内存|硬盘|键盘|鼠标|防火墙)", r"\1", t)
    t = re.sub(r"到底(?=是(?:做|干)什么|有什么(?:用|作用|功能))", "", t)
    t = re.sub(r"是(?:做|干)什么(?:用的|用|的)$|用来(?:做|干)什么$", "有什么作用", t)
    t = re.sub(r"的(?:作用|用途|功能)$", "有什么作用", t)
    for phrase in ("有什么用处", "有什么用途", "有什么用", "有何作用", "的作用是什么", "有哪些功能", "有什么功能"):
        t = t.replace(phrase, "有什么作用")
    if "有什么作用" in t:
        return "purpose:" + t.replace("有什么作用", "").replace("的", "")
    if "为什么" in t:
        return "why:" + t.replace("为什么", "").replace("的", "")
    if t.startswith("什么是"):
        return "what:" + t[3:]
    if t.endswith("是什么"):
        return "what:" + t[:-3]
    if "怎么" in t:
        return "how:" + t.replace("怎么", "")
    return t

def _match_spoken_faq(text):
    # Additions are exact alias equivalences only.
    raw = _device_norm(text)
    if re.search(r"现在|实时|刚才|上次|之前|这盆|这棵|我的|我们|今天|区别|比较|哪个|还是|然后|并且|先.+再", raw):
        return None
    q = _spoken_norm(text)
    if not q or re.search(r"不|别|没", q):
        return None
    global _spoken_index
    if _spoken_index is None:
        table = defaultdict(dict)
        functional = {'操作系统','内存','硬盘','键盘','鼠标','浏览器','文件夹','防火墙'}
        eligible = {row[1]['id'] for row in _device_aliases()[0]}
        for entry in entries():
            if entry.get('id') not in eligible:
                continue
            aliases = list(entry.get('examples', []))
            question = entry.get('question','')
            if question:
                aliases.append(question)
            definition = re.fullmatch(r"什么是(.+)[？?]", question)
            if entry['id'].startswith('faq_') and definition and definition[1] in functional:
                aliases.append(definition[1] + '有什么作用')
            for alias in aliases:
                key = _spoken_norm(alias)
                if key:
                    table[key][entry['id']] = entry
        _spoken_index = table
    matches = _spoken_index.get(q, {})
    if len(matches) != 1:
        return None
    entry = next(iter(matches.values()))
    return entry if entry['id'].startswith('faq_') else None


def match_device(text):
    hit = _match_spoken_faq(text)
    return hit if hit is not None else _match_device_original(text)


def candidates_device(text, limit=10):
    """Recall candidates for semantic verification, never auto-answer them."""
    global _candidate_index
    rows, _, _, entities = _device_aliases()
    if _candidate_index is None:
        indexed, postings = [], defaultdict(set)
        byid = {e["id"]: (e, entity) for _, e, entity in rows}
        docs = [(alias, e, entity) for alias, e, entity in rows]
        docs += [(e.get("answer") or " ".join(e.get("answers", [])[:1]), e, entity)
                 for e, entity in byid.values()]
        fact_df = defaultdict(set)
        for raw, e, entity in docs:
            s = _PUNCT.sub("", raw.split(":", 1)[-1])
            gs = set(zip(s, s[1:]))
            i = len(indexed);indexed.append((e, entity, gs))
            for g in gs:
                postings[g].add(i);fact_df[g].add(e["id"])
        n = len(byid)
        idf = {g: 1 + math.log((n + 1) / (len(ids) + 1)) for g, ids in fact_df.items()}
        _candidate_index = indexed, postings, idf
    indexed, postings, idf = _candidate_index
    q = _device_norm(text).split(":", 1)[-1]
    gs = set(zip(q, q[1:]))
    if not gs:
        return []
    weights = Counter()
    for g in gs:
        for i in postings.get(g, ()):
            weights[i] += idf.get(g, 1) ** 2
    selected = {x for x in entities if x in q}
    scores = {}
    for i, dot in weights.most_common(160):
        e, entity, terms = indexed[i]
        if entity and entity not in selected:
            continue
        denominator = math.sqrt(sum(idf.get(g, 1) ** 2 for g in terms))
        score = dot / denominator if denominator else 0
        if e["id"] not in scores or score > scores[e["id"]][0]:
            scores[e["id"]] = score, e
    return [e for _, e in sorted(scores.values(), key=lambda x: x[0], reverse=True)[:limit]]


# ── AI 答案 24h 缓存 ──

def _cache_load():
    global _cache
    if _cache is None:
        _cache = _load(CACHE_PATH, {})
    return _cache


def cache_get(text):
    key = _norm(text)
    if not key:
        return None
    ent = _cache_load().get(key)
    if not ent:
        return None
    if time.time() - ent.get("ts", 0) > CACHE_TTL_S:
        return None
    return ent


def cache_put(text, answer):
    key = _norm(text)
    if not key or not answer:
        return
    c = _cache_load()
    c[key] = {"answer": answer, "ts": time.time()}
    # 简单封顶：超过 500 条丢最老的一半
    if len(c) > 500:
        for k in sorted(c, key=lambda k: c[k].get("ts", 0))[:250]:
            c.pop(k, None)
    try:
        _save(CACHE_PATH, c)
    except Exception:
        pass


# ── TTS 音频缓存（按要念的文本哈希） ──

def tts_cached(spoken_text, synth_fn):
    """spoken_text 已收拾过的播报文本；synth_fn() 返回 wav bytes 或 None。"""
    if not spoken_text:
        return None
    key = hashlib.md5(spoken_text.encode("utf-8")).hexdigest()
    os.makedirs(TTS_CACHE_DIR, exist_ok=True)
    path = os.path.join(TTS_CACHE_DIR, key + ".wav")
    try:
        with open(path, "rb") as f:
            return f.read()
    except Exception:
        pass
    audio = synth_fn()
    if audio:
        try:
            with open(path, "wb") as f:
                f.write(audio)
        except Exception:
            pass
    return audio


def warm_device_indexes():
    """Build existing read-only indexes before the first device question.

    No answer/ASR/TTS request, cache persistence or matching rule changes.
    The non-user sentinel only forces the existing exact-alias index build.
    Candidate retrieval builds its index before its empty-query early return.
    """
    started=time.monotonic()
    _device_aliases()
    _match_spoken_faq('startupwarmup')
    candidates_device('',limit=0)
    return {'entries':len(entries()),'aliases':len(_device_index[0]),
            'spoken_keys':len(_spoken_index or {}),
            'candidate_docs':len(_candidate_index[0]),
            'elapsed_ms':round((time.monotonic()-started)*1000)}
