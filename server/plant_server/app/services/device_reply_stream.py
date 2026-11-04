"""Bounded text -> sentence -> PCM pipeline for device CHAT replies only.

Commands/FAQ selections are returned only after complete JSON validation.
An early chat prefix can never turn into a command later. Incomplete model
or audio streams fail without publishing a complete result/cache. The caller
owns authentication, persistence and WAV HTTP framing.
"""
import json,re,threading,time,queue,os
from collections import deque

class _Workers:
    """At most three running jobs and three pending handoffs, never unbounded.

    Reply slots still govern admission until both upstream workers exit.
    Idle workers retain only their thread-local HTTP session, never a Reply.
    Daemon shutdown matches the previous per-question daemon threads.
    """
    def __init__(self, name):
        self._queue=queue.Queue(maxsize=3)
        self._lock=threading.Lock()
        self._threads=[]
        self._outstanding=0
        self._name=name

    def submit(self, job):
        with self._lock:
            self._outstanding+=1
            try:
                # A normal serial conversation needs one worker and keeps its
                # private HTTPS session warm. Concurrent jobs still grow to3.
                while len(self._threads)<min(3,self._outstanding):
                    worker=threading.Thread(target=self._run,name=self._name,daemon=True)
                    worker.start();self._threads.append(worker)
                # Commit only after a worker exists; failed startup must not
                # strand an enqueued Reply or its admission slot.
                self._queue.put_nowait(job)
            except BaseException:
                self._outstanding-=1
                raise

    def _run(self):
        while True:
            job=self._queue.get()
            try:job()
            finally:
                # Do not retain the previous bound method while waiting for work.
                del job
                with self._lock:self._outstanding-=1
                self._queue.task_done()

_text_workers=_Workers('m5_reply_text')
_audio_workers=_Workers('m5_reply_pcm')

_slots=threading.BoundedSemaphore(3)
_HEADER=re.compile(r'^\s*\{\s*"kind"\s*:\s*"chat"\s*,\s*"action"\s*:\s*""\s*,\s*"arg"\s*:\s*""\s*,\s*"answer"\s*:\s*(")')
_END=re.compile('[。！？!?]')
_FIRST_CLAUSE=re.compile('[，,；;]')
ORDER_PROMPT=('\n流式输出时严格按 kind,action,arg,answer 的顺序写字段，faq_id 如需则放最后。'
              '聊天开口就回答要点，不先复述问题，不说“好问题”或“让我想想”等占位话。'
              '仅在kind=chat时：先用约10到20个汉字说出一个简短完整的要点，以句号结束，'
              '然后再用一两句补充解释。第一句要直接回应孩子的问题，不用条件铺垫或泛泛承诺。')

def _unique(pairs):
    out={}
    for k,v in pairs:
        if k in out:raise ValueError('Duplicate intent field')
        out[k]=v
    return out

def chat_prefix(raw):
    """Only the ordered, empty-action CHAT header permits speculative speech.

    Decode using JSON rules, never regex-unescape partial unicode/escapes.
    A split escape simply waits for another provider chunk.
    """
    m=_HEADER.match(raw)
    if not m:return None
    fragment=raw[m.start(1):]
    try:return json.JSONDecoder().raw_decode(fragment)[0]
    except ValueError:
        try:return json.loads(fragment+'"')
        except ValueError:return None

class Reply:
    MAX_RAW=65536
    MAX_PCM=4*1024*1024-44
    def __init__(self, complete_stream, synthesize, system, message, validate,
                 normalize=lambda x:x.strip(), speech_limit=120, deadline=12,
                 claim_timeout=30, audio_deadline=45, finish_text=None, metrics_trace=0, metrics_origin=None):
        if not _slots.acquire(blocking=False):raise RuntimeError('Reply pipeline capacity reached')
        self._complete=complete_stream;self._synthesize=synthesize
        self._system=system+ORDER_PROMPT;self._message=message;self._validate=validate
        self._normalize=normalize;self._limit=speech_limit or 320
        self._finish_text=finish_text
        self._metrics_trace=metrics_trace if type(metrics_trace) is int and metrics_trace>=0 else 0
        self._timing_start=time.monotonic();self._timing_marks={}
        if type(metrics_origin) in (int,float) and 0<=metrics_origin<=self._timing_start:
            self._timing_marks['prepare_to_reply_ms']=round((self._timing_start-metrics_origin)*1000)
        self._cv=threading.Condition();self._ready=threading.Event()
        self._sentences=deque();self._pcm=deque();self._speech='';self._total_pcm=0
        self._decision=None;self._initial=None;self._error=None
        self._cancelled=False;self._claimed=False;self._release_requested=False;self._released=False
        self._text_done=False;self._audio_done=True
        self._expires=time.monotonic()+deadline
        self._audio_expires=time.monotonic()+audio_deadline
        self._claim_timer=threading.Timer(claim_timeout,self._expire_unclaimed)
        self._claim_timer.daemon=True;self._claim_timer.start()
        try:_text_workers.submit(self._text_worker)
        except Exception:
            with self._cv:
                self._text_done=True;self._cancel_locked()
            raise

    def _mark(self, key):
        # Called under _cv; fixed internal keys and numerical durations only.
        self._timing_marks.setdefault(key,round((time.monotonic()-self._timing_start)*1000))

    def timings(self):
        with self._cv:return dict(self._timing_marks)

    def _log_timing(self):
        kind=(self._initial or {}).get('kind','unknown')
        if kind not in {'chat','command','clarify','faq'}:kind='unknown'
        status='failed' if self._error else ('generated' if self._decision else 'cancelled')
        row=dict(self._timing_marks,trace=self._metrics_trace,pid=os.getpid(),kind=kind,status=status,
                 total_ms=round((time.monotonic()-self._timing_start)*1000),
                 pcm_bytes=self._total_pcm,audio_claimed=self._claimed)
        # Metrics must never change slot ownership or response/error behavior.
        try:print('[ReplyTiming] '+json.dumps(row,sort_keys=True),flush=True)
        except Exception:pass

    def _log_failure(self, stage, error):
        # Fixed categories and durations only: never log exception messages,
        # prompts, answer fragments, provider bodies or credentials.
        try:
            allowed={'TimeoutError','ValueError','JSONDecodeError','UpstreamError',
                     'ConnectionError','ReadTimeout','ConnectTimeout','RuntimeError'}
            category=type(error).__name__
            if category not in allowed:category='Other'
            row={'stage':stage,'category':category,'trace':self._metrics_trace,'pid':os.getpid(),
                 'elapsed_ms':round((time.monotonic()-self._timing_start)*1000),
                 'first_delta_seen':'first_delta_ms' in self._timing_marks,
                 'first_pcm_seen':'first_pcm_ms' in self._timing_marks}
            cause=error
            status=0
            for _ in range(4):
                if cause is None:break
                match=re.fullmatch(r'Intent stream HTTP ([1-5][0-9]{2})',str(cause))
                if match:status=int(match[1]);break
                cause=cause.__cause__
            row['http_status']=status
            print('[ReplyFailure] '+json.dumps(row,sort_keys=True),flush=True)
        except Exception:pass

    def _maybe_release(self):
        # Never release capacity while a timed-out upstream worker is alive.
        if self._release_requested and self._text_done and self._audio_done and not self._released:
            self._released=True;self._claim_timer.cancel();_slots.release();self._log_timing()

    def _expire_unclaimed(self):
        with self._cv:
            if not self._claimed:self._cancel_locked()

    def _cancel_locked(self):
        self._cancelled=True;self._pcm.clear();self._sentences.clear()
        self._release_requested=True;self._ready.set();self._cv.notify_all();self._maybe_release()

    def cancel(self):
        with self._cv:self._cancel_locked()

    def _check(self):
        if self._cancelled:raise RuntimeError('Reply cancelled')
        if time.monotonic()>=self._expires:raise TimeoutError('Intent stream deadline')

    def _emit(self, speech):
        # Called by the text worker with the condition lock held.
        if not speech.startswith(self._speech):raise ValueError('Chat prefix changed')
        rest=speech[len(self._speech):]
        if not rest:return
        self._mark('first_sentence_ms')
        self._speech=speech;self._sentences.append(rest)
        if self._initial is None:
            self._initial={'kind':'chat','answer':speech}
            self._audio_done=False
            self._mark('audio_submit_ms')
            try:_audio_workers.submit(self._audio_worker)
            except Exception:
                self._audio_done=True
                raise
            self._ready.set()
        self._cv.notify_all()

    def _partial(self, raw):
        prefix=chat_prefix(raw)
        if prefix is None:return
        self._mark('chat_prefix_ms')
        speech=self._normalize(prefix)
        ends=[m.end() for m in _END.finditer(speech) if 8<=m.end()<=self._limit]
        if ends:
            self._mark('first_full_sentence_ms')
            self._emit(speech[:ends[-1]])
        elif not self._speech:
            # Start real answer audio at a natural first-clause boundary.
            # Split only once before a full sentence: do not turn every comma
            # into another synthesis request or change final spoken content.
            clauses=[m.end() for m in _FIRST_CLAUSE.finditer(speech)
                     if 12<=m.end()<=min(32,self._limit)]
            if clauses:
                self._mark('first_clause_ms')
                self._emit(speech[:clauses[0]])

    def _text_worker(self):
        with self._cv:self._mark('text_worker_start_ms')
        source=None
        try:
            source=iter(self._complete(self._system,self._message));raw=''
            for part in source:
                if not isinstance(part,str):raise ValueError('Invalid text delta')
                with self._cv:
                    if part:self._mark('first_delta_ms')
                    self._check();raw+=part
                    if len(raw)>self.MAX_RAW:raise ValueError('Intent stream limit')
                    self._partial(raw)
            with self._cv:
                self._check()
                parsed=json.loads(raw,object_pairs_hook=_unique)
                decision=self._validate(parsed)
                if decision is None:raise ValueError('Invalid streamed intent')
                if self._initial and decision['kind']!='chat':raise ValueError('Early chat changed kind')
                if decision['kind']=='chat':
                    full=self._normalize(decision['answer'])
                    if self._finish_text is not None:
                        full=self._finish_text(decision['answer'])
                        if len(full)>self._limit:raise ValueError('Final speech limit')
                    elif len(full)>self._limit:
                        cut=full[:self._limit]
                        ends=[m.end() for m in _END.finditer(cut)]
                        full=cut[:ends[-1]] if ends else cut
                    self._emit(full)
                self._decision=decision
                if self._initial is None:
                    self._initial=decision;self._release_requested=True
                self._ready.set()
        except Exception as error:
            self._log_failure('text',error)
            with self._cv:
                self._error=error;self._pcm.clear();self._sentences.clear()
                self._release_requested=True;self._ready.set()
        finally:
            if source is not None and hasattr(source,'close'):
                try:source.close()
                except Exception:pass
            with self._cv:self._mark('text_done_ms');self._text_done=True;self._cv.notify_all();self._maybe_release()

    def _audio_worker(self):
        with self._cv:self._mark('audio_worker_start_ms')
        source=None
        try:
            while True:
                with self._cv:
                    while not self._sentences and not self._text_done and not self._cancelled and not self._error:
                        remaining=self._expires-time.monotonic()
                        if remaining<=0:raise TimeoutError('Sentence stream deadline')
                        self._cv.wait(min(.5,remaining))
                    if self._cancelled:return
                    if self._error:raise self._error
                    if not self._sentences:break
                    text=self._sentences.popleft()
                    self._mark('tts_start_ms')
                source=iter(self._synthesize(text))
                count=0
                for pcm in source:
                    with self._cv:
                        if self._cancelled:return
                        if time.monotonic()>=self._audio_expires:raise TimeoutError('Audio generation deadline')
                        if not isinstance(pcm,bytes) or not pcm or len(pcm)%2:raise ValueError('Invalid PCM block')
                        self._total_pcm+=len(pcm);count+=len(pcm)
                        if self._total_pcm>self.MAX_PCM:raise ValueError('Reply PCM limit')
                        self._mark('first_pcm_ms')
                        self._pcm.append(pcm);self._cv.notify_all()
                if not count:raise ValueError('Empty sentence audio')
                source=None
        except Exception as error:
            self._log_failure('audio',error)
            with self._cv:
                self._error=error;self._pcm.clear();self._sentences.clear();self._release_requested=True
        finally:
            if source is not None and hasattr(source,'close'):
                try:source.close()
                except Exception:pass
            with self._cv:self._mark('audio_done_ms');self._audio_done=True;self._cv.notify_all();self._maybe_release()

    def wait_audio_ready(self, timeout):
        """Wait without claiming or consuming the first PCM block."""
        until=time.monotonic()+max(0,timeout)
        with self._cv:
            while not self._pcm and not self._error and not self._cancelled and not self._audio_done:
                remaining=until-time.monotonic()
                if remaining<=0:return False
                self._cv.wait(remaining)
            if self._error:raise self._error
            if self._cancelled:raise RuntimeError('Reply cancelled')
            return bool(self._pcm)

    def wait_ready(self):
        """Return first actual chat sentence or a fully validated other kind."""
        if not self._ready.wait(max(0,self._expires-time.monotonic())):
            self.cancel();raise TimeoutError('Reply readiness deadline')
        with self._cv:
            if self._cancelled:raise RuntimeError('Reply cancelled')
            if self._error:raise self._error
            return dict(self._initial)

    def pcm(self):
        """Single consumer; failures/cancellation cannot replay heard prefixes."""
        with self._cv:
            if self._claimed:raise RuntimeError('Reply audio already claimed')
            if self._cancelled:raise RuntimeError('Reply cancelled')
            self._claimed=True;self._claim_timer.cancel()
        try:
            while True:
                with self._cv:
                    while not self._pcm and not self._audio_done and not self._cancelled:
                        self._cv.wait(.5)
                    if self._cancelled:raise RuntimeError('Reply cancelled')
                    if self._pcm:block=self._pcm.popleft()
                    elif self._error:raise self._error
                    elif self._text_done and self._audio_done:break
                    else:continue
                yield block
            with self._cv:
                if not self._decision or self._decision['kind']!='chat':raise ValueError('Incomplete chat')
        finally:self.cancel()

    def completed(self):
        with self._cv:
            if self._error or not self._decision:return None
            return dict(self._decision)

    @property
    def spoken(self):
        with self._cv:return self._speech
