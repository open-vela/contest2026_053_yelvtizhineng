# -*- coding: utf-8 -*-
import serial, time, sys
sys.stdout.reconfigure(encoding='utf-8', errors='replace')
port='COM23'; outp=sys.argv[1]
s = serial.Serial(); s.port=port; s.baudrate=115200; s.timeout=0.1
s._dtr_state=False; s._rts_state=False
s.open()
def cap(sec):
    t=time.time(); out=bytearray()
    while time.time()-t<sec:
        d=s.read(8192)
        if d: out.extend(d)
    return out
# classic reset: EN low via RTS, IO0 high via DTR
s.setDTR(False); s.setRTS(True); time.sleep(0.2)
s.setRTS(False); time.sleep(0.2)
b1 = cap(12)
txt = b1.decode('utf-8','replace')
print('--- after classic reset: %d bytes ---' % len(b1))
print(txt[:1500])
if b'Firmware V' not in b1 and b'rst:' not in b1:
    # try inverted polarity
    s.setDTR(True); s.setRTS(False); time.sleep(0.2)
    s.setRTS(True); time.sleep(0.2); s.setRTS(False)
    b2 = cap(12)
    print('--- after inverted toggle: %d bytes ---' % len(b2))
    print(b2.decode('utf-8','replace')[:1500])
    b1 += b2
open(outp,'wb').write(bytes(b1))
try: s.close()
except Exception: pass
print('=== total %d ===' % len(b1))
