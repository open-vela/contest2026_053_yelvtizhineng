#!/usr/bin/env python3
"""
测试 MiMo 是否接受 raw RGB565 base64 图像输入。

用法：python3 test_mimo_rgb.py

原理：
  1. PC 端生成 16×16 RGB565 四象限测试图（红/绿/蓝/白各 8×8 像素）
  2. base64 编码 → data:image/rgb;base64,... 格式
  3. POST 到 MiMo chat/completions（OpenAI 兼容格式，带 image_url content）
  4. 打印 MiMo 返回的文本 → 若有有效回复 = MiMo 接受 RGB 格式；若报错 = 不接受

需要：pip install requests（无其他依赖）
"""

import base64
import json
import struct
import sys

# ── MiMo API 配置（来自 defconfig）──
API_KEY  = "<在此填入你的 MiMo API Key>"
API_URL    = "https://token-plan-cn.xiaomimimo.com/v1/chat/completions"
MODEL    = "mimo-v2.5"

# ── 1. 生成 16×16 RGB565 测试图 ──
WIDTH, HEIGHT = 16, 16
pixels = bytearray(WIDTH * HEIGHT * 2)  # RGB565 = 2 bytes/pixel

for y in range(HEIGHT):
    for x in range(WIDTH):
        # 四象限：左上红(0xF800)、右上绿(0x07E0)、左下蓝(0x001F)、右下白(0xFFFF)
        if x < 8 and y < 8:
            rgb565 = 0xF800  # 红
        elif x >= 8 and y < 8:
            rgb565 = 0x07E0  # 绿
        elif x < 8:
            rgb565 = 0x001F  # 蓝
        else:
            rgb565 = 0xFFFF  # 白
        # RGB565 小端（低字节先）与 OV3660 大端 + 交换后一致
        offset = (y * WIDTH + x) * 2
        pixels[offset]     = rgb565 & 0xFF
        pixels[offset + 1] = rgb565 >> 8

b64_data = base64.b64encode(pixels).decode("ascii")
print(f"[INFO] 测试图：{WIDTH}×{HEIGHT} RGB565 ({len(pixels)}B → base64 {len(b64_data)}B)")

# ── 2. 构造 OpenAI 兼容 multimodal 请求 ──
payload = {
    "model": MODEL,
    "messages": [
        {
            "role": "user",
            "content": [
                {
                    "type": "text",
                    "text": "这是一张16x16像素的测试图（红/绿/蓝/白四象限）。请描述你看到的颜色分布。"
                },
                {
                    "type": "image_url",
                    "image_url": {
                        "url": f"data:image/rgb;base64,{b64_data}"
                    }
                }
            ]
        }
    ],
    "max_tokens": 200
}

headers = {
    "Content-Type": "application/json",
    "Authorization": f"Bearer {API_KEY}"
}

# ── 3. 发送请求 ──
try:
    import requests
    use_requests = True
except ImportError:
    import urllib.request
    use_requests = False

print(f"[INFO] POST {API_URL}")
print(f"[INFO] Payload size: {len(json.dumps(payload))} bytes")

if use_requests:
    resp = requests.post(
        API_URL,
        headers=headers,
        json=payload,
        timeout=30
    )
    status = resp.status_code
    body = resp.text
else:
    req = urllib.request.Request(
        API_URL,
        data=json.dumps(payload).encode("utf-8"),
        headers=headers,
        method="POST"
    )
    with urllib.request.urlopen(req, timeout=30) as resp:
        status = resp.status
        body = resp.read().decode("utf-8")

print(f"[INFO] HTTP {status}")
print(f"[RESPONSE]")
print(body[:2000])  # 打印前 2000 字符

# ── 4. 判读 ──
try:
    result = json.loads(body)
    if "choices" in result and result["choices"]:
        text = result["choices"][0].get("message", {}).get("content", "")
        print(f"\n[RESULT] ✅ MiMo 返回文本（{len(text)} 字）→ RGB 格式可用！")
        print(f"[RESULT] MiMo 说：{text[:500]}")
    elif "error" in result:
        print(f"\n[RESULT] ❌ MiMo 报错 → RGB 格式不可用，需 JPEG")
        print(f"[RESULT] 错误信息：{result['error'].get('message', '未知')}")
    else:
        print(f"\n[RESULT] ⚠️ 未知响应格式")
except Exception as e:
    print(f"\n[RESULT] ⚠️ 响应解析失败：{e}")
