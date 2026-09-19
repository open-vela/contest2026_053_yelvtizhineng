# -*- coding: utf-8 -*-
"""启动：python run_server.py [--host 0.0.0.0] [--port 8000] [--seed]"""
import argparse


def main():
    ap = argparse.ArgumentParser(description="植小伴服务器")
    ap.add_argument("--host", default="0.0.0.0")
    ap.add_argument("--port", type=int, default=8000)
    ap.add_argument("--seed", action="store_true", help="写入演示数据")
    args = ap.parse_args()

    import uvicorn
    from app import db
    from app.services import plant_ops
    db.init_db()
    plant_ops.ensure_templates()
    if args.seed:
        from app.services.seed import ensure_demo
        r = ensure_demo(verbose=True)
        print("[seed] done:", r)
    uvicorn.run("app.main:app", host=args.host, port=args.port,
                log_level="info")


if __name__ == "__main__":
    main()