"""Bounded ASR lookahead. A partial hypothesis NEVER becomes a result.

One optional cloud request overlaps the existing capture-end silence. Reuse
requires exact agreement with the independently decoded, hash-sealed FULL PCM.
No reply, command, persistence, or context work runs speculatively.
"""
import threading,time,unicodedata,re,struct
from collections import deque
from concurrent.futures import ThreadPoolExecutor

def canonical(text):
    # Keep words, negation, numbers, decimal points, signs and word boundaries.
    value=unicodedata.normalize('NFKC',str(text)).casefold().strip()
    value=value.translate(str.maketrans('','','，。！？、；?!;'))
    # ASR inserts punctuation; the full local recognizer does not. Preserve
    # punctuation *inside numbers* (0.5, 1,000, 1:30), and all lexical content.
    value=re.sub(r'(?<![0-9])[.,:]|[.,:](?![0-9])','',value)
    return ' '.join(value.split())

class Pool:
    def __init__(self, transcribe, wav, workers=2, per_minute=12, clock=time.monotonic, eligible=None):
        self.transcribe=transcribe;self.wav=wav;self.clock=clock
        self.executor=ThreadPoolExecutor(max_workers=workers,thread_name_prefix='asr_lookahead')
        self.slots=threading.BoundedSemaphore(workers)
        self.lock=threading.Lock();self.admitted=[];self.per_minute=per_minute;self.eligible=eligible

    def submit(self, pcm):
        if not self.slots.acquire(blocking=False):return None
        with self.lock:
            now=self.clock();self.admitted=[t for t in self.admitted if now-t<60]
            if len(self.admitted)>=self.per_minute:
                self.slots.release();return None
            self.admitted.append(now)
        def run():
            return (self.transcribe(self.wav(pcm)) or '').strip()
        try:
            future=self.executor.submit(run)
            # Includes cancellation before worker entry; no leaked admission.
            future.add_done_callback(lambda _:self.slots.release())
            return future
        except RuntimeError:
            self.slots.release();return None

class Turn:
    def __init__(self, pool):
        self.pool=pool;self.hint='';self.stable_at=pool.clock()
        self.future=None;self.closed=False;self.attempted=False;self.bytes=0
        self.scan_bytes=0;self.peak_msq=0;self.tail_msq=deque(maxlen=20)
        self.stats=dict(started=0,reused=0,seal_wait_ms=0,reason='not-started')

    def observe(self, pcm, shadow):
        if self.closed or self.attempted or shadow is None or shadow.invalid:return
        if not 32000<=len(pcm)<=384000:return
        # Scheduling only: 20x20ms quiet relative to this capture's peak lets
        # ASR overlap the unchanged 1.5s recorder tail. This does not declare
        # an endpoint, cut audio, or weaken full sealed-consensus admission.
        while self.scan_bytes+640<=len(pcm):
            block=struct.unpack('<320h',pcm[self.scan_bytes:self.scan_bytes+640])
            msq=sum(v*v for v in block)//320
            self.peak_msq=max(self.peak_msq,msq);self.tail_msq.append(msq);self.scan_bytes+=640
        quiet=(len(self.tail_msq)==20 and self.peak_msq>=90000 and
               max(self.tail_msq)*8<=self.peak_msq)
        hint=canonical(shadow.peek_hint());now=self.pool.clock()
        if hint!=self.hint:
            self.hint=hint;self.stable_at=now
        eligible=getattr(self.pool,'eligible',None)
        if eligible is not None and not eligible(hint):
            self.stats['reason']='not-exact-faq';return
        # A stable hypothesis is only a scheduling hint, never evidence that
        # speech ended. Full-capture consensus below is mandatory, including
        # when a speaker pauses then continues with another clause.
        if len(hint)<4 or (not quiet and now-self.stable_at<.6):return
        self.attempted=True
        self.future=self.pool.submit(bytes(pcm));self.bytes=len(pcm)
        self.stats.update(started=int(self.future is not None),reason='pending' if self.future else 'capacity',
                          trigger='quiet-tail' if quiet else 'stable-hint')

    def select(self, shadow, full_pcm):
        self.closed=True
        future=self.future
        if future is None or not future.done():
            self.stats['reason']='not-ready' if future else self.stats['reason'];return None
        try:text=future.result()
        except Exception:
            self.stats['reason']='preview-error';return None
        if not text or shadow is None or shadow.invalid:
            self.stats['reason']='invalid';return None
        # Full SDK sealing measured 66-124ms on the deployed decoder. Bound
        # this optional handoff to 150ms; a 25ms budget could never use a
        # completed preview. Never await cloud work or
        # postpone the normal full-ASR fallback on a slow/failed preview.
        started=self.pool.clock();complete=shadow.seal(full_pcm,timeout=.15)
        self.stats['seal_wait_ms']=round((self.pool.clock()-started)*1000)
        if not complete:
            self.stats['reason']='full-decoder-not-ready';return None
        if not canonical(text) or canonical(text)!=canonical(complete['text']):
            self.stats['reason']='full-text-mismatch';return None
        self.stats.update(reused=1,reason='full-consensus')
        return text

    def cancel(self):
        self.closed=True
        if self.future:self.future.cancel()
