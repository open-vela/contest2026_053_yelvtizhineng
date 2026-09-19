# -*- coding: utf-8 -*-
"""演示前把理赔记录清空，让「一键理赔」重新回到秒级自动通过的状态。

为什么需要它：
    同一盆植物在一个保障期内只要赔付过一次，再报案就会被强制转人工（防刷）。
    连着演示几遍之后，演示账号就再也演不出「证据齐全、免人工直接通过」的效果了。
    演示前跑一下这个脚本即可恢复。

用法（在服务器上）：
    python3 /opt/plant_server/scripts/reset_demo_claims.py            # 只清演示账号
    python3 /opt/plant_server/scripts/reset_demo_claims.py --all      # 清全部账号（慎用）
    python3 /opt/plant_server/scripts/reset_demo_claims.py --phone 13800000001
"""
import argparse
import os
import sqlite3
import sys

DEFAULT_DB = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
                          "data", "plant.db")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--db", default=DEFAULT_DB)
    ap.add_argument("--all", action="store_true", help="清掉所有账号的理赔单")
    ap.add_argument("--phone", action="append", default=[],
                    help="指定手机号，可重复；不填则只处理 demo=1 的演示账号")
    args = ap.parse_args()

    if not os.path.exists(args.db):
        print("找不到数据库：%s" % args.db)
        return 1

    con = sqlite3.connect(args.db)
    con.row_factory = sqlite3.Row

    if args.all:
        users = [r["id"] for r in con.execute("SELECT id FROM users")]
        label = "全部账号"
    elif args.phone:
        users = []
        for p in args.phone:
            row = con.execute("SELECT id FROM users WHERE phone=?", (p,)).fetchone()
            if row:
                users.append(row["id"])
            else:
                print("跳过：没有这个手机号 %s" % p)
        label = "指定手机号 " + ",".join(args.phone)
    else:
        users = [r["id"] for r in con.execute("SELECT id FROM users WHERE demo=1")]
        label = "演示账号"

    if not users:
        print("没有找到要处理的账号")
        return 0

    total = 0
    for uid in users:
        rows = con.execute("SELECT COUNT(*) n FROM claims WHERE user_id=?", (uid,)).fetchone()
        cur = con.execute("DELETE FROM claims WHERE user_id=?", (uid,))
        # 保单上的「本期已理赔」是直接从 claims 表算出来的，删完就自动复位
        total += cur.rowcount
        print("  %s：清掉 %s 张理赔单" % (uid, rows["n"]))
    con.commit()

    left = con.execute("SELECT COUNT(*) n FROM claims").fetchone()["n"]
    print("%s 共清理 %d 张；数据库里还剩 %d 张。" % (label, total, left))
    print("现在再点「一键理赔」，证据齐全的植物会重新走秒级自动通过。")
    return 0


if __name__ == "__main__":
    sys.exit(main())