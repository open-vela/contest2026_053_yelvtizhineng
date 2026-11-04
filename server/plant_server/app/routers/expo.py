# -*- coding: utf-8 -*-
"""展会扫码页反馈接口：单独写 JSONL，不触碰现有业务表。"""

import json
import os
import time
import uuid

from fastapi import APIRouter, HTTPException
from pydantic import BaseModel

from .. import config


router = APIRouter(prefix="/expo", tags=["展会"])


class ExpoFeedback(BaseModel):
    name: str = ""
    company: str = ""
    contact: str = ""
    role: str = ""
    interest: str = ""
    message: str = ""
    source: str = ""
    consent: bool = False
    website: str = ""


def _cut(value, limit):
    value = (value or "").strip()
    return value[:limit]


@router.post("/feedback")
def submit_feedback(item: ExpoFeedback):
    if item.website:
        return {"ok": True, "id": "ignored"}

    contact = _cut(item.contact, 120)
    message = _cut(item.message, 2000)
    if not contact and not message:
        raise HTTPException(400, "请留下联系方式或填写反馈内容")

    row = {
        "id": "expo_" + uuid.uuid4().hex[:12],
        "created_at": time.strftime("%Y-%m-%d %H:%M:%S", time.localtime()),
        "name": _cut(item.name, 80),
        "company": _cut(item.company, 160),
        "contact": contact,
        "role": _cut(item.role, 80),
        "interest": _cut(item.interest, 120),
        "message": message,
        "source": _cut(item.source, 80),
        "consent": bool(item.consent),
    }

    directory = os.path.join(config.BASE_DIR, "data")
    os.makedirs(directory, exist_ok=True)
    path = os.path.join(directory, "expo_feedback.jsonl")
    with open(path, "a", encoding="utf-8") as fp:
        fp.write(json.dumps(row, ensure_ascii=False) + "\n")

    return {"ok": True, "id": row["id"]}
