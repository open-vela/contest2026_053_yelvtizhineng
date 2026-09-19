# -*- coding: utf-8 -*-
"""无状态 token：HMAC-SHA256 签名 (sub, role, exp)。演示期够用，正式化换 JWT/刷新。"""
import base64
import hashlib
import hmac
import json
import time

from . import config


def _b64e(raw: bytes) -> str:
    return base64.urlsafe_b64encode(raw).decode("ascii").rstrip("=")


def _b64d(s: str) -> bytes:
    s += "=" * (-len(s) % 4)
    return base64.urlsafe_b64decode(s.encode("ascii"))


def _sign(payload_b64: str) -> str:
    return _b64e(hmac.new(config.TOKEN_SECRET.encode(), payload_b64.encode(),
                          hashlib.sha256).digest())


def create_token(sub: str, role: str) -> str:
    # 设备凭证用长有效期：板卡不会自己处理"凭证过期"，只会在开机时注册一次
    ttl_days = (config.DEVICE_TOKEN_TTL_DAYS if role == "device"
                else config.TOKEN_TTL_DAYS)
    body = {"sub": sub, "role": role,
            "exp": int(time.time()) + ttl_days * 86400}
    payload = _b64e(json.dumps(body, separators=(",", ":")).encode())
    return payload + "." + _sign(payload)


def verify_token(token: str):
    """返回 claims dict；无效返回 None。"""
    try:
        payload, sig = token.split(".")
        expect = _sign(payload)
        if not hmac.compare_digest(expect, sig):
            return None
        claims = json.loads(_b64d(payload))
        if claims.get("exp", 0) < time.time():
            return None
        return claims
    except Exception:
        return None