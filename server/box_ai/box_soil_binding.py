"""Per-BOX read-only binding to an explicitly shared cloud soil source."""
import json
import os
import re
import threading
from pathlib import Path

import sqlite3
from contextlib import closing
from app import config
from fastapi import Depends, HTTPException


def install_soil_routes(app, authorized, primary):
    binding_file = os.environ.get('BOX_SOIL_BINDINGS_FILE')
    bindings = json.loads(Path(binding_file).read_text(encoding='utf-8')
                          if binding_file else os.environ.get('BOX_SOIL_BINDINGS', '{}'))
    if not isinstance(bindings, dict):
        raise RuntimeError('BOX soil bindings must be a mapping')
    for subject, source in bindings.items():
        if not re.fullmatch(r'd_[0-9a-f]{12}', subject) or not re.fullmatch(r'd_[0-9a-f]{12}', source):
            raise RuntimeError('Invalid BOX soil binding identity')
    cache = {}
    lock = threading.Lock()

    def bound_soil(subject, token, refresh=False):
        source = bindings.get(subject)
        if not source:
            return None
        with lock:
            saved = cache.get(subject)
        if saved is not None and not refresh:
            return saved
        # The shared legacy endpoint returns the newest sample globally.
        # This BOX mapping explicitly shares one source only. Read that
        # source from this seesoil server, without changing ownership or DB.
        try:
            uri = Path(config.DB_PATH).resolve().as_uri() + '?mode=ro'
            with closing(sqlite3.connect(uri, uri=True, timeout=3)) as db:
                db.row_factory = sqlite3.Row
                db.execute('PRAGMA query_only=ON')
                sensor_row = db.execute(
                    'SELECT sn,model FROM devices WHERE id=?', (source,)).fetchone()
                latest_row = db.execute(
                    'SELECT ts,moisture,temp,ec,ph,salt,nitrogen,phosphorus,'
                    'potassium,source,device_id,plant_id FROM telemetry '
                    "WHERE device_id=? AND source='device' ORDER BY ts DESC LIMIT 1",
                    (source,)).fetchone()
        except (sqlite3.Error, OSError):
            raise HTTPException(502, '暂时无法读取已绑定的云端探头')
        if sensor_row is None or latest_row is None:
            raise HTTPException(503, '已绑定的云端探头暂无读数')
        latest = dict(latest_row)
        sensor = dict(sensor_row)
        sensor['name'] = '已绑定土壤传感器'
        result = {'ok': True, 'bound': True, 'source_device_id': source,
                  'poll_interval_s': 300, 'sensor': sensor,
                  'latest_telemetry': latest}
        with lock:
            cache[subject] = result
        return result

    @app.get('/api/v1/box/soil')
    def soil(auth=Depends(authorized)):
        subject, token = auth
        result = bound_soil(subject, token, refresh=True)
        if result is None:
            raise HTTPException(404, '这台BOX尚未绑定云端探头')
        return result

    return bound_soil
