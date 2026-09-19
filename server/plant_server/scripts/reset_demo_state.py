# -*- coding: utf-8 -*-
"""演示前一键复位：把演示账号恢复到"随时能演"的状态。

做三件事（都可重复执行）：
  1. 清掉演示账号的理赔单 —— 不然「本期已赔付」会强制转人工，演不出秒过；
  2. 把没回执的执行指令作废、执行器收回待命 —— 不然页面上会一直挂着"等板卡执行"；
  3. 会员恢复成 PRO（12 个月）+ 押金已交 —— 演示自动执行时不会被套餐门禁拦住。

用法（服务器上）：
    python3 scripts/reset_demo_state.py                # 演示账号
    python3 scripts/reset_demo_state.py --phone 13800000001
"""
import argparse
import datetime
import os
import sqlite3
import sys

DEFAULT_DB = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
                          "data", "plant.db")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--db", default=DEFAULT_DB)
    ap.add_argument("--phone", action="append", default=[])
    args = ap.parse_args()

    if not os.path.exists(args.db):
        print("找不到数据库：%s" % args.db)
        return 1

    con = sqlite3.connect(args.db)
    con.row_factory = sqlite3.Row

    if args.phone:
        users = []
        for p in args.phone:
            row = con.execute("SELECT id, nickname FROM users WHERE phone=?",
                              (p,)).fetchone()
            if row:
                users.append(row)
            else:
                print("跳过：没有这个手机号 %s" % p)
    else:
        users = list(con.execute("SELECT id, nickname FROM users WHERE demo=1"))
    if not users:
        print("没有找到要处理的账号")
        return 0

    uid_list = [u["id"] for u in users]
    q = ",".join("?" * len(uid_list))
    today = datetime.date.today()
    exp = (today + datetime.timedelta(days=365)).isoformat()
    now = datetime.datetime.now().strftime("%Y-%m-%d %H:%M:%S")

    for u in users:
        n_claim = con.execute("DELETE FROM claims WHERE user_id=?",
                              (u["id"],)).rowcount
        # 没回执的执行指令 → 作废
        n_job = con.execute(
            "UPDATE actuator_jobs SET status='expired' WHERE status IN"
            " ('pending','sent') AND plant_id IN"
            " (SELECT id FROM plants WHERE user_id=?)", (u["id"],)).rowcount
        con.execute(
            "UPDATE actuators SET state='idle' WHERE state='running' AND plant_id IN"
            " (SELECT id FROM plants WHERE user_id=?)", (u["id"],))
        # 会员恢复 PRO + 押金已交
        s = con.execute("SELECT id FROM subscriptions WHERE user_id=?",
                        (u["id"],)).fetchone()
        if s:
            con.execute("UPDATE subscriptions SET plan='pro', status='active',"
                        " expires_at=?, auto_renew=1, deposit_cents=9900,"
                        " deposit_status='held', updated_at=? WHERE user_id=?",
                        (exp, now, u["id"]))
        else:
            con.execute("INSERT INTO subscriptions(id,user_id,plan,status,"
                        "started_at,expires_at,auto_renew,deposit_cents,"
                        "deposit_status,created_at,updated_at)"
                        " VALUES(?,?,?,?,?,?,?,?,?,?,?)",
                        ("sub_" + u["id"][-8:], u["id"], "pro", "active", now,
                         exp, 1, 9900, "held", now, now))
        print("  %s（%s）：清理赔 %d 张、作废指令 %d 条、会员恢复到 %s"
              % (u["nickname"] or u["id"], u["id"][-6:], n_claim, n_job, exp))
    con.commit()

    left = con.execute("SELECT COUNT(*) FROM claims").fetchone()[0]
    print("库里的理赔单还剩 %d 张。演示账号可以直接开演了。" % left)
    print("提示：板卡已实现执行器协议（每 15 秒取一次指令）。继电器还没接线，"
          "所以点「立即浇水」图标后约 15 秒内 App 上就会显示「完成」"
          "（板卡取到指令当场干跑完成），日记里写「…完成 · dry-run: no actuator wired」（见 README）。")
    return 0


if __name__ == "__main__":
    sys.exit(main())
