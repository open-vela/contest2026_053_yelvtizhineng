"""Optional cached notice, paced so a ready answer can take priority.

Never synthesizes, consumes main PCM, cancels the main reply, or changes cache.
The existing MCU WAV player accepts unknown-length WAV with finite HTTP EOF.
"""
import array,io,json,os,sys,threading,time,wave
from .voice_stream import header,MAX_PCM

_slots=threading.BoundedSemaphore(3)
_LIMIT=256000

def claim_session(sessions,lock,owner,now=None):
    now=time.monotonic() if now is None else now
    with lock:
        pending=[s for s in tuple(sessions.values()) if s.get('owner')==owner
                 and s.get('thinking_announced') and not s.get('hint_claimed')
                 and 0<=now-s.get('hint_pending_at',float('-inf'))<=15]
        # Ambiguous simultaneous turns must not attach to the wrong reply.
        if len(pending)!=1:return None
        pending[0]['hint_claimed']=True
        return pending[0]

def answer_ready(session):
    if session.get('prepare_failed'):return True
    pipeline=session.get('reply_pipeline')
    if pipeline is not None:return pipeline.wait_audio_ready(0)
    future=session.get('future')
    return bool('result' in session or (future is not None and future.done()))

def _cached_pcm(path):
    with open(path,'rb') as source:data=source.read(_LIMIT+1)
    if len(data)>_LIMIT:raise ValueError('notice limit')
    with wave.open(io.BytesIO(data),'rb') as wav:
        rate=wav.getframerate();frames=wav.getnframes()
        if (wav.getnchannels()!=1 or wav.getsampwidth()!=2 or
            wav.getcomptype()!='NONE' or rate not in (16000,24000) or frames>rate*4):
            raise ValueError('notice format')
        pcm=wav.readframes(frames)
        if len(pcm)!=frames*2 or not pcm:raise ValueError('notice length')
        return rate,pcm

def _fade(pcm):
    samples=array.array('h');samples.frombytes(pcm)
    if sys.byteorder!='little':samples.byteswap()
    last=len(samples)-1
    for i in range(len(samples)):samples[i]=round(samples[i]*(last-i)/max(1,last))
    if sys.byteorder!='little':samples.byteswap()
    return samples.tobytes()

def stream(path,session,*,clock=time.monotonic,sleep=time.sleep):
    # Acquire during iteration: an unstarted/discarded response owns no slot.
    started=clock();owned=False;sent=0;checks=0;outcome='client_closed'
    try:
        def ready():
            nonlocal checks
            checks+=1
            return answer_ready(session)
        if ready():
            outcome='skipped_ready';yield header(0);return
        if not _slots.acquire(blocking=False):
            outcome='skipped_capacity';yield header(0);return
        owned=True
        try:rate,pcm=_cached_pcm(path)
        except (OSError,ValueError,EOFError,wave.Error):
            outcome='skipped_cache';yield header(0);return
        if ready():
            outcome='skipped_ready';yield header(0);return
        yield header(MAX_PCM,rate)
        # MCU stages6400 PCM bytes before feeding I2S. Keep one stage plus40ms
        # queued to avoid making pacing itself starve playback.
        chunk=max(2,int(rate*.02)*2);lead=6400/(rate*2)+.04;started=clock()
        while sent<len(pcm):
            deadline=started+max(0,sent/(rate*2)-lead)
            while clock()<deadline and not ready():sleep(min(.02,deadline-clock()))
            if ready():
                if sent:
                    fade=_fade(pcm[sent:sent+int(rate*.04)*2])
                    sent+=len(fade)
                    if fade:yield fade
                outcome='interrupted';return
            block=pcm[sent:sent+chunk];sent+=len(block);yield block
        outcome='played_full'
    except GeneratorExit:raise
    except Exception:
        outcome='error'
        raise
    finally:
        if owned:_slots.release()
        # Fixed fields only: no session IDs, paths, text, audio, or exceptions.
        print('[VoiceHintTiming] '+json.dumps({'trace':session.get('prepare_trace',0),
              'pid':os.getpid(),'outcome':outcome,'pcm_bytes':sent,
              'elapsed_ms':round((clock()-started)*1000),'readiness_checks':checks},sort_keys=True),flush=True)
