# -*- coding: utf-8 -*-
"""植小伴 · 服务器大脑（FastAPI 入口）。"""
import os
from contextlib import asynccontextmanager

from fastapi import FastAPI, HTTPException
from fastapi.middleware.cors import CORSMiddleware
from fastapi.responses import FileResponse
from fastapi.staticfiles import StaticFiles

from . import admin, config, db
from .routers import (actuator, ai, auth, billing, community,
                      device_bridge, devices, events, insurance, media, plants,
                      reports, system, tasks, telemetry)
from .services import plant_ops


@asynccontextmanager
async def lifespan(app):
    db.init_db()
    if config.TOKEN_SECRET_IS_DEFAULT:
        print("[SECURITY] PLANT_TOKEN_SECRET is still the built-in default; "
              "set it before exposing this server to the internet")
    plant_ops.ensure_templates()
    # 社区做实：库是空的话补一批"邻居"和帖子，不然第一次点进去什么都没有
    try:
        from .services import community_seed
        community_seed.ensure_community()
    except Exception as e:
        print("[community] seed skipped: %s" % e)
    # 一键理赔有"秒过"和"转人工"两种结果，演示账号需要两盆不同证据的植物
    try:
        from .services import demo_plants
        demo_plants.ensure_demo_plants()
    except Exception as e:
        print("[demo-plants] seed skipped: %s" % e)
    # 自动执行层：演示账号的板卡先摆出"水泵 + 补光灯"（固件接入后会自动接管）
    try:
        from .services import actuator_ops
        actuator_ops.ensure_demo_actuators()
    except Exception as e:
        print("[actuator] seed skipped: %s" % e)
    # 商业模式骨架：演示账号默认 PRO + 押金已交，自动执行才不会演示到一半被门禁拦住
    try:
        from .services import billing_ops
        billing_ops.ensure_demo_subscription()
    except Exception as e:
        print("[billing] seed skipped: %s" % e)
    yield


app = FastAPI(
    title="植小伴 · 服务器大脑",
    description="账号/设备/数据/AI 编排/任务/日记的统一后端（参赛演示版 V1.0）",
    version="0.1.0",
    lifespan=lifespan,
)

app.add_middleware(
    CORSMiddleware,
    allow_origins=["*"],
    allow_methods=["*"],
    allow_headers=["*"],
)

# ── 前端资源不做 HTTP 缓存（2026-09-17 踩过的坑） ──
#   StaticFiles 默认只发 ETag / Last-Modified，**没有 Cache-Control**，
#   Android WebView 就按"启发式缓存"长期复用旧的 app.js —— 表现是
#   "服务器改了手机端前端，手机上完全没变化"，重启 App 也没用
#   （试过：手机只发 /api/v1/* 请求，一次都不来取 /mobile/，
#    而 Service Worker 在 http 非安全上下文下根本不注册，兜不住）。
#   这里统一加 no-store：每 次都回源校验，改完立刻生效。
#   前端文件都很小，且只有一台服务器，这点代价可以忽略。
_NO_CACHE_PATHS = ("/mobile", "/admin", "/version.json")


@app.middleware("http")
async def no_cache_frontend(request, call_next):
    resp = await call_next(request)
    path = request.url.path
    for p in _NO_CACHE_PATHS:
        if path == p or path.startswith(p + "/"):
            resp.headers["Cache-Control"] = "no-store, no-cache, must-revalidate, max-age=0"
            resp.headers["Pragma"] = "no-cache"
            resp.headers["Expires"] = "0"
            break
    return resp

API = "/api/v1"
for r in (auth.router, devices.router, devices.sync_router, plants.router, telemetry.router,
          events.router, media.router, ai.router, ai.voice_router,
          tasks.router, tasks.growth_router, reports.router, system.router,
          community.router, insurance.router,
          actuator.router, billing.router):
    app.include_router(r, prefix=API)

# ── 管理台（网页版）：/api/v1/admin/* 接口 + /admin 页面 ──
#   独立口令鉴权（见 app/admin.py），不影响设备链路与现有接口。
app.include_router(admin.router, prefix=API)

_ADMIN_STATIC = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                             "static", "admin")
if os.path.isdir(_ADMIN_STATIC):
    app.mount("/admin", StaticFiles(directory=_ADMIN_STATIC, html=True),
              name="admin")

# ── 手机端 App（网页版 / PWA）：/mobile 页面 ──
#   与上面接口同源，手机浏览器打开 http://<服务器IP>:8011/mobile/ 即可用，
#   「添加到主屏幕」后全屏运行，和原生 App 观感一致。
_MOBILE_STATIC = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                              "static", "mobile")
if os.path.isdir(_MOBILE_STATIC):
    app.mount("/mobile", StaticFiles(directory=_MOBILE_STATIC, html=True),
              name="mobile")

# 设备旧协议兼容层（根路径 /time、/voice/*、/image/analyze）：**无鉴权**。
# 现固件已全部走 /api/v1/*，公网部署默认不挂载；确需时 PLANT_LEGACY_BRIDGE=1。
if config.LEGACY_BRIDGE:
    app.include_router(device_bridge.router)

# ── 固件升级（OTA）：板卡用同一个地址（plant.cfg 里的 server_host/server_port） ──
#   来查版本并下载，所以这里和业务接口同端口发布，不再需要单独的文件服务器。
#     GET /version.json      版本信息（含大小与 SHA256）
#     GET /firmware/<file>   固件二进制
#   发布新固件：python publish_firmware.py <nuttx.bin> <版本号>
_OTA_DIR = os.path.join(config.BASE_DIR, "ota")
_OTA_FW_DIR = os.path.join(_OTA_DIR, "firmware")
os.makedirs(_OTA_FW_DIR, exist_ok=True)
app.mount("/firmware", StaticFiles(directory=_OTA_FW_DIR), name="firmware")


@app.get("/version.json")
def ota_version():
    p = os.path.join(_OTA_DIR, "version.json")
    if not os.path.exists(p):
        raise HTTPException(404, "还没有发布固件版本")
    return FileResponse(p, media_type="application/json")

for r in (system.router,):
    pass  # system 已在上面注册


@app.get("/")
def root():
    return {"service": "植小伴 · 服务器大脑", "version": "0.1.0",
            "docs": "/docs", "api": API,
            "ai_mode": _ai_mode()}


def _ai_mode():
    from .services.ai_gateway import gateway
    return gateway.mode
