import sqlite3
con = sqlite3.connect(r"E:\zhixiaoban_prj\plant_server\data\plant.db")
con.row_factory = sqlite3.Row
cur = con.cursor()
cols = [r[1] for r in cur.execute("PRAGMA table_info(telemetry)")]
print("columns:", cols)
print()
rows = cur.execute("SELECT * FROM telemetry ORDER BY id DESC LIMIT 6").fetchall()
for r in rows:
    d = dict(r)
    print({k: d[k] for k in d})
con.close()
