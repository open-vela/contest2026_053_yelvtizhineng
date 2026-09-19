# -*- coding: utf-8 -*-
"""通用被动监听: psniff.py <port> <秒> <out>"""
import sys, time, threading
try:
    sys.stdout.reconfigure(encoding='utf-8', errors='replace')
except Exception:
    pass
import serial
port = sys.argv[1]; dur = float(sys.argv[2]); outp = sys.argv[3]
s = serial.Serial(); s.port=port; s.baudrate=115200; s.timeout=0.05
s._dtr_state=False; s._rts_state=False; s.open()
buf = bytearray(); stop = threading.Event(); t0 = time.time()
def reader():
    while not stop.is_set():
        try: d = s.read(8192)
        except Exception: break
        if d: buf.extend(d)
threading.Thread(target=reader, daemon=True).start()
seen = 0
while time.time() - t0 < dur:
    time.sleep(0.2)
    raw = bytes(buf); txt = raw[seen:].decode('utf-8','replace'); seen = len(raw)
    for line in txt.splitlines():
        if line.strip(): print('[%6.2fs] %s' % (time.time()-t0, line.strip()))
stop.set(); time.sleep(0.2)
try: s.close()
except Exception: pass
open(outp,'wb').write(bytes(buf))
print('[%6.2fs] === end, %d bytes ===' % (time.time()-t0, len(buf)))
