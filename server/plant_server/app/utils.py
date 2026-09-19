# -*- coding: utf-8 -*-
"""时间(北京时间)与 id 工具。"""
import time
import uuid


def bj_now():
    """返回北京时间组件: (完整串, 日期, 时分, unix秒)。"""
    now = time.time() + 8 * 3600
    t = time.gmtime(now)
    full = time.strftime("%Y-%m-%d %H:%M:%S", t)
    date = time.strftime("%Y-%m-%d", t)
    hms = time.strftime("%H:%M", t)
    return full, date, hms, int(now)


def iso_ts(full_dt):
    """'YYYY-MM-DD HH:MM:SS' -> 'YYYY-MM-DDTHH:MM:SS+08:00'"""
    return full_dt.replace(" ", "T") + "+08:00"


def new_id(prefix):
    return "%s_%s" % (prefix, uuid.uuid4().hex[:12])


def date_add(date_str, days):
    t = time.strptime(date_str + " 12:00:00", "%Y-%m-%d %H:%M:%S")
    ts = time.mktime(t) + days * 86400
    return time.strftime("%Y-%m-%d", time.gmtime(ts))


def mood_of(score):
    if score >= 80:
        return "😊", "良好"
    if score >= 60:
        return "🥺", "注意"
    return "😷", "危险"