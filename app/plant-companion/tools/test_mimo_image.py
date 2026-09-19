#!/usr/bin/env python3
"""
测试 MiMo 图像输入：模型名 × MIME 类型组合遍历。

用法：python3 test_mimo_image.py
需要：pip install requests （PIL/Pillow 可选，有则同时测真实 JPEG）
"""

import base64
import json
import sys

API_KEY  = "<在此填入你的 MiMo API Key>"
API_URL  = "https://token-plan-cn.xiaomimimo.com/v1/chat/completions"

# ── 组合遍历 ──
MODELS = ["mimo-v2.5", "mimo-v2.5-pro"]
MIMES  = ["image/rgb", "image/jpeg", "image/png"]

WIDTH, HEIGHT = 16, 16
PROMPT = "这是一张16x16像素测试图（红/绿/蓝/白四象限）。请描述你看到的颜色分布。"

def make_rgb565():
    """生成 16×16 RGB565 四象限测试图（红/绿/蓝/白）"""
    px = bytearray(WIDTH * HEIGHT * 2)
    for y in range(HEIGHT):
        for x in range(WIDTH):
            if x < 8 and y < 8:
                c = 0xF800
            elif x >= 8 and y < 8:
                c = 0x07E0
            elif x < 8:
                c = 0x001F
            else:
                c = 0xFFFF
            off = (y * WIDTH + x) * 2
            px[off] = c & 0xFF
            px[off + 1] = c >> 8
    return bytes(px)

def make_real_jpeg():
    """用 PIL 生成一张真实 JPEG 测试图（可选）"""
    try:
        from PIL import Image, ImageDraw
    except ImportError:
        return None
    img = Image.new("RGB", (64, 64), "white")
    d = ImageDraw.Draw(img)
    d.rectangle([0, 0, 31, 31], fill="red")
    d.rectangle([32, 0, 63, 31], fill="green")
    d.rectangle([0, 32, 31, 63], fill="blue")
    import io
    buf = io.BytesIO()
    img.save(buf, format="JPEG")
    return buf.getvalue()

def send(model, mime, data_b64):
    payload = {
        "model": model,
        "messages": [{
            "role": "user",
            "content": [
                {"type": "text", "text": PROMPT},
                {"type": "image_url",
                 "image_url": {"url": f"data:{mime};base64,{data_b64}"}}
            ]
        }],
        "max_tokens": 200
    }
    headers = {"Content-Type": "application/json",
               "Authorization": f"Bearer {API_KEY}"}
    import requests
    try:
        resp = requests.post(API_URL, headers=headers, json=payload, timeout=30)
        return resp.status_code, resp.text
    except Exception as e:
        return 0, f"网络异常: {e}"

def main():
    rgb565 = make_rgb565()
    rgb_b64 = base64.b64encode(rgb565).decode()
    jpeg = make_real_jpeg()

    print("=" * 60)
    print("MiMo 图像输入能力测试（模型 × MIME）")
    print("=" * 60)

    combos = []
    for m in MODELS:
        for mime in MIMES:
            combos.append((m, mime, rgb_b64))
    if jpeg:
        print("[INFO] 检测到 PIL → 增加真实 JPEG 测试")
        jpeg_b64 = base64.b64encode(jpeg).decode()
        for m in MODELS:
            combos.append((m, "image/jpeg", jpeg_b64))
    else:
        print("[INFO] 无 PIL → 跳过真实 JPEG 测试（RGB565 伪装为各 MIME）")

    any_ok = False
    for model, mime, b64 in combos:
        print(f"\n--- 模型={model}  MIME={mime}  (base64 {len(b64)}B) ---")
        status, body = send(model, mime, b64)
        print(f"HTTP {status}")
        # 截断长响应
        print(body[:400])
        try:
            j = json.loads(body)
            if "choices" in j and j["choices"]:
                text = j["choices"][0].get("message", {}).get("content", "")
                print(f"✅ 成功！模型={model} MIME={mime} → {text[:200]}")
                any_ok = True
            elif "error" in j:
                print(f"❌ 错误：{j['error'].get('message','')[:150]}")
        except Exception:
            pass

    print("\n" + "=" * 60)
    if any_ok:
        print("结论：存在可用的 模型×MIME 组合 ✅")
    else:
        print("结论：全部组合失败 ❌ → 该 endpoint 可能不支持图像输入，需查 MiMo 文档/换端点")
    print("=" * 60)

if __name__ == "__main__":
    main()
