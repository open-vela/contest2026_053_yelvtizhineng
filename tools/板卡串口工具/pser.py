# -*- coding: utf-8 -*-
"""通用串口命令会话: pser.py <port> <out> [wait] cmd1 cmd2 ..."""
import sys, time, threading
try:
    sys.stdout.reconfigure(encoding='utf-8', errors='replace')
except Exception:
    pass
import serial
port = sys.argv[1]; outp = sys.argv[2]
args = sys.argv[3:]
wait = 3.0
if args and args[0].startswith('+'):
    wait = float(args[0][1:]); args = args[1:]
cmds = args
s = serial.Serial(); s.port=port; s.baudrate=115200; s.timeout=0.05; s.write_timeout=3
s._dtr_state=False; s._rts_state=False
s.open()
buf = bytearray(); stop = threading.Event(); t0 = time.time()
def reader():
    while not stop.is_set():
        try: d = s.read(8192)
        except Exception: break
        if d: buf.extend(d)
threading.Thread(target=reader, daemon=True).start()
seen = 0
def drain(secs):
    global seen
    t = time.time()
    while time.time() - t < secs:
        time.sleep(0.15)
    raw = bytes(buf); txt = raw[seen:].decode('utf-8','replace'); seen = len(raw)
    for line in txt.splitlines():
        if line.strip(): print('[%6.2fs] %s' % (time.time()-t0, line.strip()))
drain(1.0)
for c in cmds:
    print('>>> %s' % c)
    s.write((c + '\r\n').encode()); s.flush()
    drain(wait)
stop.set(); time.sleep(0.2)
try: s.close()
except Exception: pass
open(outp,'wb').write(bytes(buf))
print('=== total bytes %d ===' % len(buf))
