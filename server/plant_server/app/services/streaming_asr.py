"""Bounded capture-time decoder. A result exists only after full PCM is sealed.

Default off. The shadow mode never replaces the existing ASR or emits commands.
One worker owns the recognizer; HTTP uploads only enqueue bounded PCM blocks.
"""
import hashlib
import queue
import threading
import time
import os

_engine_lock=threading.Lock()
_engine=None

def get_shadow_engine():
    """Explicit opt-in; importing the module alone never loads a model."""
    global _engine
    if os.environ.get('PLANT_STREAM_ASR_MODE','off') not in {'shadow','fast'}:return None
    model=os.environ.get('PLANT_STREAM_ASR_MODEL','')
    if not model:return None
    with _engine_lock:
        if _engine is None:
            def factory():
                import sys
                vendor=os.environ.get('PLANT_STREAM_ASR_VENDOR','')
                if vendor and vendor not in sys.path:sys.path.insert(0,vendor)
                return SherpaRecognizer(model)
            _engine=Engine(factory)
        return _engine


class Turn:
    def __init__(self, engine):
        self.engine=engine;self.created=time.monotonic()
        self.lock=threading.Lock();self.done=threading.Event()
        self.seq=0;self.bytes=0;self.digest=hashlib.sha256()
        self.sealed=False;self.invalid=False;self.reason='';self.result=None
        self.stream=None;self.carry=b'';self.decode_ms=0;self.hint=''

    def cancel(self, reason='cancelled'):
        with self.lock:
            self.invalid=True;self.result=None;self.reason=reason;self.done.set()
        self.engine.retire(self)

    def feed(self, seq, pcm):
        pcm=bytes(pcm);reason=None
        with self.lock:
            if self.invalid or self.sealed:return False
            if seq!=self.seq:reason='sequence'
            elif not pcm or len(pcm)>self.engine.max_chunk:reason='chunk'
            elif self.bytes+len(pcm)>self.engine.max_bytes:reason='size'
            else:
                try:self.engine.tasks.put_nowait(('data',self,pcm))
                except queue.Full:reason='backpressure'
                else:
                    self.digest.update(pcm);self.bytes+=len(pcm);self.seq+=1
                    return True
        self.cancel(reason);return False

    def seal(self, full_pcm, timeout=0):
        """No partial transcript is returned, including after queue/sequence loss."""
        data=bytes(full_pcm);digest=hashlib.sha256(data).digest();reason=None
        with self.lock:
            if self.invalid:return None
            if len(data)!=self.bytes or digest!=self.digest.digest():reason='capture-mismatch'
            elif len(data)<1600 or len(data)%2:reason='pcm-format'
            elif not self.sealed:
                try:self.engine.tasks.put_nowait(('seal',self,b''))
                except queue.Full:reason='backpressure'
                else:self.sealed=True
        if reason:self.cancel(reason);return None
        self.done.wait(max(0,min(timeout,.25)))
        return self.peek()

    def peek_hint(self):
        with self.lock:return self.hint if not self.invalid else ""

    def peek(self):
        with self.lock:return dict(self.result) if self.done.is_set() and not self.invalid and self.result else None


class Engine:
    def __init__(self, factory, max_turns=2, queue_blocks=32, max_chunk=32768,
                 max_bytes=4*1024*1024, ttl=45):
        self.factory=factory;self.max_turns=max_turns;self.max_chunk=max_chunk
        self.max_bytes=max_bytes;self.ttl=ttl;self.tasks=queue.Queue(queue_blocks)
        self.lock=threading.Lock();self.turns=set();self.ready=threading.Event()
        self.stopping=threading.Event();self.failed=False
        self.worker=threading.Thread(target=self._run,name='m5_stream_asr',daemon=True)
        self.worker.start()

    def start(self):
        with self.lock:
            if not self.ready.is_set() or self.failed or self.stopping.is_set() or len(self.turns)>=self.max_turns:return None
            turn=Turn(self);self.turns.add(turn);return turn

    def retire(self, turn):
        with self.lock:self.turns.discard(turn)

    def _expire(self):
        with self.lock:turns=list(self.turns)
        for turn in turns:
            if time.monotonic()-turn.created>self.ttl:turn.cancel('expired');turn.stream=None

    def close(self, timeout=3):
        self.stopping.set()
        with self.lock:turns=list(self.turns)
        for turn in turns:turn.cancel('shutdown')
        self.worker.join(timeout)

    def _run(self):
        try:
            rec=self.factory()
        except Exception:
            self.failed=True;self.ready.set();return
        self.ready.set()
        print("[StreamASR] ready=1 pid=%d" % os.getpid(),flush=True)
        while not self.stopping.is_set():
            self._expire()
            try:kind,turn,data=self.tasks.get(timeout=.1)
            except queue.Empty:continue
            try:
                if turn.invalid:turn.stream=None;continue
                if turn.stream is None:turn.stream=rec.create_stream()
                started=time.monotonic()
                if kind=='data':
                    block=turn.carry+data;n=len(block)//2*2;turn.carry=block[n:]
                    if n:rec.accept(turn.stream,block[:n])
                    if hasattr(rec,'partial'):
                        hint=rec.partial(turn.stream)
                        with turn.lock:
                            if not turn.invalid:turn.hint=hint
                else:
                    if turn.carry:raise ValueError('odd PCM')
                    text,confidence=rec.finish(turn.stream)
                    row={'text':text,'bytes':turn.bytes,'decode_ms':round(turn.decode_ms),'completed_at':time.monotonic(),
                         'seal_ms':round((time.monotonic()-started)*1000),'confidence':confidence}
                    with turn.lock:
                        if not turn.invalid:turn.result=row;turn.done.set()
                    turn.stream=None;self.retire(turn)
                turn.decode_ms+=(time.monotonic()-started)*1000
            except Exception:
                turn.cancel('decoder-error');turn.stream=None
            finally:self.tasks.task_done()
        # Release queued private PCM references on shutdown.
        while True:
            try:self.tasks.get_nowait();self.tasks.task_done()
            except queue.Empty:break


class SherpaRecognizer:
    def __init__(self, model):
        import numpy as np
        import sherpa_onnx
        from pathlib import Path
        self.np=np;model=Path(model)
        self.rec=sherpa_onnx.OnlineRecognizer.from_transducer(
            tokens=str(model/'tokens.txt'),encoder=str(model/'encoder.int8.onnx'),
            decoder=str(model/'decoder.onnx'),joiner=str(model/'joiner.int8.onnx'),
            model_type='zipformer2',num_threads=1,enable_endpoint_detection=False)

    def create_stream(self):return self.rec.create_stream()

    def accept(self, stream, pcm):
        values=self.np.frombuffer(pcm,dtype='<i2').astype(self.np.float32)/32768
        stream.accept_waveform(16000,values)
        while self.rec.is_ready(stream):self.rec.decode_stream(stream)

    def partial(self, stream):return self.rec.get_result(stream)

    def finish(self, stream):
        stream.accept_waveform(16000,self.np.zeros(6400,dtype=self.np.float32));stream.input_finished()
        while self.rec.is_ready(stream):self.rec.decode_stream(stream)
        probs=self.rec.ys_probs(stream)
        confidence={'min_logprob':min(probs) if probs else None,'tokens':len(probs)}
        return self.rec.get_result(stream),confidence
