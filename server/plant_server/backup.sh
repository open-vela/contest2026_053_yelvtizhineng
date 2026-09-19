#!/bin/bash
# 植小伴 · 云端备份（2026-09-11 v2）
#
# v1 的问题：① 注释写着"媒体目录打包"但实际只备了数据库
#            ② 没有安排定时任务（脚本在、crontab 不在）→ 已补
#            ③ 备份全在同一台机器上，机器挂了备份一起没（无异地）
#
# 本版：数据库每日快照 + 媒体每周日打包 + 保留策略 + 可选 OSS 异地
# 用法：backup.sh            常规（数据库每日；周日顺带媒体）
#       backup.sh --all      立刻连媒体一起打（首次/手动）
set -u

D=/opt/backups
SRC=/opt/plant_server/data
PY=/opt/plant_server/.venv/bin/python3
TS=$(date +%Y%m%d)
KEEP_DB_DAYS=14
KEEP_MEDIA_DAYS=28

mkdir -p "$D"
log() { echo "$(date '+%F %T') $*"; }

# ① 数据库一致性快照（SQLite 在线 backup API，不是直接拷文件）
"$PY" - <<'PY'
import sqlite3
src = sqlite3.connect("file:/opt/plant_server/data/plant.db?mode=ro", uri=True)
out = sqlite3.connect("/tmp/plant_backup.db")
with out:
    src.backup(out)
out.close(); src.close()
PY
if [ ! -f /tmp/plant_backup.db ]; then
  log "ERROR 数据库快照失败"
  exit 1
fi
mv -f /tmp/plant_backup.db "$D/plant.db"
tar czf "$D/plant.db.$TS.tgz" -C "$D" plant.db && rm -f "$D/plant.db"
log "db ok    plant.db.$TS.tgz $(stat -c%s "$D/plant.db.$TS.tgz") B"

# ② 媒体目录（图片/语音）：每周日全量打包；--all 可强制
if [ "$(date +%u)" = "7" ] || [ "${1:-}" = "--all" ]; then
  if tar czf "$D/media.$TS.tgz" -C "$SRC" media 2>/dev/null; then
    log "media ok media.$TS.tgz $(stat -c%s "$D/media.$TS.tgz") B"
  else
    log "WARN 媒体打包失败（目录可能为空）"
  fi
fi

# ③ 保留策略
find "$D" -name 'plant.db.*.tgz' -mtime +$KEEP_DB_DAYS -delete
find "$D" -name 'media.*.tgz'   -mtime +$KEEP_MEDIA_DAYS -delete

# ④ 可选：上传到阿里云 OSS（装了 ossutil 且配了 PLANT_OSS_BUCKET 才做）
if [ -n "${PLANT_OSS_BUCKET:-}" ] && command -v ossutil >/dev/null 2>&1; then
  for f in "$D/plant.db.$TS.tgz" "$D/media.$TS.tgz"; do
    [ -f "$f" ] || continue
    if ossutil cp -f "$f" "oss://$PLANT_OSS_BUCKET/backups/" >/dev/null 2>&1; then
      log "oss ok   $(basename "$f")"
    else
      log "WARN OSS 上传失败 $(basename "$f")"
    fi
  done
fi

log "done  当前备份：$(ls -1 "$D" | tr '\n' ' ')"