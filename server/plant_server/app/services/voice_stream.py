"""WAV streaming with cache commit only after successful synthesis."""
import hashlib,os,struct,tempfile,threading,time
import requests
_slots=threading.BoundedSemaphore(3)
MAX_PCM=4*1024*1024-44
def header(size,rate=24000):
    return struct.pack('<4sI4s4sIHHIIHH4sI',b'RIFF',size+36,b'WAVE',b'fmt ',16,1,1,rate,rate*2,2,16,b'data',size)
def cached_path(directory,text):return os.path.join(directory,hashlib.md5(text.encode('utf-8')).hexdigest()+'.wav')

def commit_pcm(text,chunks,directory):
    total=sum(len(p) for p in chunks)
    if not total or total>MAX_PCM or any(not p or len(p)%2 for p in chunks):raise ValueError('Invalid complete PCM')
    os.makedirs(directory,exist_ok=True)
    fd,tmp=tempfile.mkstemp(prefix='.stream-',dir=directory)
    try:
        with os.fdopen(fd,'wb') as f:f.write(header(total));f.writelines(chunks)
        os.replace(tmp,cached_path(directory,text))
    finally:
        if os.path.exists(tmp):os.unlink(tmp)
def stream_wav(text,gateway,directory):
    if not _slots.acquire(blocking=False):raise RuntimeError('Voice stream capacity reached')
    chunks=[];total=0;started=False;t0=time.monotonic();source=None
    try:
        # Ask for the first real PCM before yielding a header or committing to
        # streaming, so unsupported providers can fall back without replay.
        source=iter(gateway.tts_pcm_stream(text))
        try:first=next(source)
        except (requests.Timeout,TimeoutError):
            # The same timed-out provider must not start another45s request.
            raise
        except Exception:
            fallback=gateway.tts(text)
            if not fallback:raise RuntimeError('Voice synthesis unavailable')
            yield fallback;return
        if not first or len(first)%2 or len(first)>MAX_PCM:raise ValueError('Invalid first PCM block')
        yield header(MAX_PCM)
        print('[VoiceStream] first_pcm_ms=%d'%int((time.monotonic()-t0)*1000),flush=True)
        for pcm in _with_first(first,source):
            total+=len(pcm)
            if len(pcm)%2 or total>MAX_PCM:raise ValueError('PCM stream limit')
            chunks.append(pcm);yield pcm
        # Atomic complete WAV cache. A cancelled/failed stream never publishes
        # a partially synthesized file as a successful future cache hit.
        os.makedirs(directory,exist_ok=True)
        fd,tmp=tempfile.mkstemp(prefix='.stream-',dir=directory)
        try:
            with os.fdopen(fd,'wb') as f:f.write(header(total));f.writelines(chunks)
            os.replace(tmp,cached_path(directory,text))
        finally:
            if os.path.exists(tmp):os.unlink(tmp)
    finally:
        if source is not None and hasattr(source,'close'):source.close()
        _slots.release()
def _with_first(first,rest):
    yield first
    yield from rest
