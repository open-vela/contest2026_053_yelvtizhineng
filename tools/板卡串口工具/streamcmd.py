# -*- coding: utf-8 -*-
"""streamcmd.py <port> <outfile> <timeout_s> <cmd...>  — 流式带时间戳执行串口命令"""
import serial, socket, time, sys
sys.stdout.reconfigure(encoding='utf-8', errors='replace')
port=sys.argv[1]; outp=sys.argv[2]; tout=float(sys.argv[3]); cmd=' '.join(sys.argv[4:])
s=serial.Serial(); s.port=port; s.baudrate=115200; s.timeout=0.05
s._dtr_state=False; s._rts_state=False
s.open()
buf=bytearray(); t0=time.time()
def drain():
    got=False
    while True:
        d=s.read(4096)
        if not d: break
        buf.extend(d); got=True
    return got
# wait for prompt
t=time.time()
while time.time()-t < 8:
    drain(); time.sleep(0.05)
print('>>> %s' % cmd)
s.write((cmd+'\r\n').encode()); s.flush()
last=time.time(); printed=0
while time.time()-t0 < tout:
    drain()
    time.sleep(0.05)
    if len(buf) > printed:
        txt = bytes(buf[printed:]).decode('utf-8','replace')
        printed = len(buf)
        for ln in txt.splitlines():
            if ln.strip():
                print('[%6.2fs] %s' % (time.time()-t0, ln.strip()))
    if (b'STAGED' in buf or b'Downloaded' in buf) and len(buf)==printed:
        if time.time()-last > 2: break
    if (buf.count(b'Progress:') >= 1) and (len(buf)==printed):
        pass
    last=time.time()
try: s.close()
except Exception: pass
open(outp,'wb').write(bytes(buf))
print('=== total %d bytes, elapsed %.1fs ===' % (len(buf), time.time()-t0))
