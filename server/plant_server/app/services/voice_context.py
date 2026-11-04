"""Short-lived device dialogue context scoped to owner AND plant.

Only completed speech/vision answers are remembered; hardware actions are
never replayed from history. No disk storage or additional model call.
"""
from collections import OrderedDict
from threading import Lock
import time

_entries = OrderedDict()
_lock = Lock()
TTL = 120
MAX_KEYS = 256
MAX_TURNS = 3

def _key(owner, plant):
    return (owner, plant) if isinstance(owner,str) and owner.startswith('device:') and plant else None

def recent(owner, plant):
    key=_key(owner,plant)
    if key is None:return []
    now=time.monotonic()
    with _lock:
        row=_entries.get(key)
        if row is None:return []
        if now-row[0]>=TTL:
            _entries.pop(key,None);return []
        return [dict(m) for pair in row[1] for m in pair]

def remember(owner, plant, said, answer):
    key=_key(owner,plant)
    if key is None or not said or not answer:return
    now=time.monotonic()
    pair=({'role':'user','text':str(said)[:160]},
          {'role':'assistant','text':str(answer)[:240]})
    with _lock:
        old=_entries.pop(key,None)
        turns=list(old[1]) if old and now-old[0]<TTL else []
        turns.append(pair)
        for k in list(_entries):
            if now-_entries[k][0]>=TTL:_entries.pop(k,None)
        while len(_entries)>=MAX_KEYS:_entries.popitem(last=False)
        _entries[key]=(now,turns[-MAX_TURNS:])
