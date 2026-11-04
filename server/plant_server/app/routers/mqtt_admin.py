# -*- coding: utf-8 -*-
"""植小伴 · MQTT 接入控制与管理（管理台专用）。

覆盖：
- mosquitto 服务状态 / 账号（只读用户名 + 新增/禁用）
- JSON 土壤 MQTT 网关：状态、只读配置和服务控制
- ESP32 无线土壤设备列表与绑定关系

原则：只调本机 systemctl / 文件，不改 plant_server 业务表结构以外的数据。
"""
from __future__ import annotations

import os
import re
import subprocess
import time

from fastapi import APIRouter, Body, Depends, HTTPException
from pydantic import BaseModel
from typing import Literal

from .. import db, utils
from ..admin import require_admin  # 复用管理台登录

router = APIRouter(prefix="/admin/mqtt", tags=["MQTT 管理"])

MOSQ_PASSWD = "/etc/mosquitto/passwd"
GW_ENV = "/opt/plant_soil/json_bridge.env"
GW_UNIT = "plant-soil-json-gw.service"


def _run(cmd, timeout=8):
    try:
        p = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout)
        return p.returncode, (p.stdout or "") + (p.stderr or "")
    except Exception as e:
        return -1, str(e)


def _unit_active(name):
    rc, out = _run(["systemctl", "is-active", name])
    return out.strip() == "active"


def _read_env():
    cfg = {}
    if not os.path.isfile(GW_ENV):
        return cfg
    for line in open(GW_ENV, encoding="utf-8"):
        line = line.strip()
        if not line or line.startswith("#") or "=" not in line:
            continue
        k, v = line.split("=", 1)
        cfg[k.strip()] = v.strip()
    return cfg


class MqttUserIn(BaseModel):
    username: str
    password: str
    action: str = "add"  # add | del


class GwConfigIn(BaseModel):
    action: Literal["restart", "stop", "start"]


SAFE_GW_KEYS = ("SOIL_MQTT_HOST", "SOIL_MQTT_PORT", "SOIL_MQTT_TOPIC",
                "SOIL_MQTT_QOS", "SOIL_MQTT_TLS", "SOIL_RETRY_SEC")


@router.get("/status")
def mqtt_status(_admin=Depends(require_admin)):
    mosq = _unit_active("mosquitto")
    gw = _unit_active(GW_UNIT)
    cfg = _read_env()
    rc, ss = _run(["ss", "-tnp"])
    clients = []
    for line in ss.splitlines():
        if any(":" + str(port) in line for port in (1883, 8883)) and "ESTAB" in line:
            clients.append(line.strip())
    # 最近土壤遥测
    last = db.one(
        "SELECT ts,moisture,temp,ec,ph,salt,nitrogen,phosphorus,potassium,"
        "source FROM telemetry ORDER BY ts DESC LIMIT 1")
    # 无线土壤设备
    soil_devs = db.q(
        "SELECT id,sn,model,status,last_seen_at FROM devices "
        "WHERE model LIKE '%soil%' "
        "ORDER BY last_seen_at DESC LIMIT 20")
    return {
        "ok": True,
        "mosquitto": {"active": mosq, "port": 1883},
        "gateway": {
            "active": gw,
            "service": GW_UNIT,
            "configuration_is_read_only": True,
            "env": {key: cfg.get(key) for key in SAFE_GW_KEYS},
        },
        "mqtt_clients_estab": clients,
        "last_soil": last,
        "soil_devices": soil_devs or [],
        "server_time": utils.bj_now()[0],
    }


@router.get("/users")
def mqtt_users(_admin=Depends(require_admin)):
    names = []
    if os.path.isfile(MOSQ_PASSWD):
        for line in open(MOSQ_PASSWD, encoding="utf-8"):
            line = line.strip()
            if not line or line.startswith("#") or ":" not in line:
                continue
            names.append(line.split(":", 1)[0])
    return {"ok": True, "users": names}


@router.post("/users")
def mqtt_user_op(body: MqttUserIn, _admin=Depends(require_admin)):
    un = re.sub(r"[^A-Za-z0-9_.-]", "", body.username or "")
    if not un:
        raise HTTPException(400, "用户名非法")
    if body.action == "add":
        if not body.password or len(body.password) < 4:
            raise HTTPException(400, "密码至少 4 位")
        rc, out = _run(["mosquitto_passwd", "-b", MOSQ_PASSWD, un, body.password])
        if rc != 0:
            raise HTTPException(500, f"mosquitto_passwd 失败: {out[:200]}")
        _run(["systemctl", "reload", "mosquitto"])
        return {"ok": True, "user": un, "action": "add"}
    if body.action == "del":
        rc, out = _run(["mosquitto_passwd", "-D", MOSQ_PASSWD, un])
        if rc != 0:
            raise HTTPException(500, f"删除失败: {out[:200]}")
        _run(["systemctl", "reload", "mosquitto"])
        return {"ok": True, "user": un, "action": "del"}
    raise HTTPException(400, "action 须为 add/del")


@router.post("/gateway")
def gateway_op(body: GwConfigIn, _admin=Depends(require_admin)):
    # Configuration and credentials stay in the private environment file.
    rc, _output = _run(["systemctl", body.action, GW_UNIT])
    if rc != 0:
        raise HTTPException(503, "网关服务操作失败，请检查服务权限")
    return {"ok": True, "action": body.action, "active": _unit_active(GW_UNIT)}


@router.get("/gateway/log")
def gateway_log(lines: int = 40, _admin=Depends(require_admin)):
    n = max(5, min(200, int(lines)))
    rc, out = _run(["journalctl", "-u", GW_UNIT, "-n", str(n), "--no-pager"])
    return {"ok": True, "log": out, "rc": rc}
