# -*- coding: utf-8 -*-
"""进程内轻量限流（按 IP + 桶）：给公网暴露的演示接口防刷。

- 滑动窗口计数，内存保存；进程重启即清零（演示场景可接受）
- 反向代理场景优先取 X-Forwarded-For 的第一个地址
- 关闭限流：环境变量 PLANT_RATE_LIMIT=0
"""
import threading
import time

from fastapi import HTTPException

from . import config

_HITS = {}
_LOCK = threading.Lock()
_MAX_KEYS = 4096
_LAST_GC = [0.0]


def client_ip(request):
    xff = (request.headers.get("x-forwarded-for") or "").strip()
    if xff:
        return xff.split(",")[0].strip()
    return request.client.host if request.client else "unknown"


def hit(request, bucket, limit, window_s):
    """记一次访问；超限抛 429，返回剩余额度（未开限流返回 -1）。"""
    if not config.RATE_LIMIT:
        return -1
    ip = client_ip(request)
    now = time.time()
    key = bucket + "|" + ip
    with _LOCK:
        if now - _LAST_GC[0] > 60:
            _LAST_GC[0] = now
            for k in [k for k, v in _HITS.items() if not v or now - v[-1] > 3600]:
                _HITS.pop(k, None)
            if len(_HITS) > _MAX_KEYS:
                _HITS.clear()
        arr = [t for t in _HITS.get(key, []) if now - t < window_s]
        if len(arr) >= limit:
            _HITS[key] = arr
            raise HTTPException(429, u"操作过于频繁，请稍后再试")
        arr.append(now)
        _HITS[key] = arr
        return limit - len(arr)
