"""A single short-lived bridge from completed visual text to the same notice.

Shares the existing one-job prefetch admission, worker, framing and cleanup.
It never publishes unfinished captions and does not claim an audio stream.
"""
import functools
import os
import threading
import time
from . import notice_prefetch, qa_bank, voice_stream
from .ai_gateway import gateway, voice_tts_text

_lock = threading.Lock()
_pending = None
_LIFETIME = 5


def _expire_locked(now):
    global _pending
    if _pending is not None and now - _pending[3] >= _LIFETIME:
        old = _pending
        _pending = None
        old[2].cancel()


def offer(owner, text):
    """Best effort only: caller already validated owner/frame/full caption."""
    global _pending
    if not isinstance(owner, str) or not owner.startswith('device:'):
        return False
    if not isinstance(text, str) or not text.strip() or len(text) > 240:
        return False
    try:
        spoken = voice_tts_text(text.strip())
        with _lock:
            _expire_locked(time.monotonic())
            if _pending is not None or os.path.exists(voice_stream.cached_path(qa_bank.TTS_CACHE_DIR, spoken)):
                return False
            job = notice_prefetch.start(functools.partial(
                voice_stream.stream_wav, spoken, gateway, qa_bank.TTS_CACHE_DIR))
            if job is None:
                return False
            _pending = (owner, spoken, job, time.monotonic())
        print('[VisionNotice] offered=1', flush=True)
        return True
    except Exception:
        # A speculative optimization must not fail a valid visual response.
        return False


def take(owner, spoken):
    """Transfer the job only to the exact authenticated owner and spoken text."""
    global _pending
    with _lock:
        now = time.monotonic()
        _expire_locked(now)
        if _pending is None or _pending[0] != owner or _pending[1] != spoken:
            return None
        _, _, job, started = _pending
        _pending = None
        if job.stats().get('cancelled'):
            return None
    try:
        print('[VisionNotice] matched=1 lead_ms=%u' % round((now - started) * 1000), flush=True)
    except OSError:
        pass
    return job

class _Prefix:
    """Speculative audio remains private until the full frame result commits."""
    def __init__(self, owner):
        self._owner = owner
        self._cv = threading.Condition()
        self._prefix = self._spoken = self._tail = ''
        self._job = None
        self._started = 0
        self._attempted = self._committed = self._published = False
        self._cancelled = self._failed = False

    def update(self, text):
        global _pending
        if self._attempted or self._cancelled:
            return
        text = voice_tts_text(text, max_chars=0)
        # A short complete clause, never a word-count cut mid phrase. An
        # incompatible/unsplittable final answer retains the normal path.
        end = next((i + 1 for i, c in enumerate(text)
                    if c in '，,。！？!?；;' and 8 <= i + 1 <= 32), None)
        if end is None:
            return
        self._attempted = True
        prefix = text[:end]
        if voice_tts_text(prefix) != prefix:
            return
        with _lock:
            _expire_locked(time.monotonic())
            with self._cv:
                if _pending is not None or self._cancelled:
                    return
                self._prefix = prefix
                self._started = time.monotonic()
                self._job = notice_prefetch.start(self._stream)

    def _segment(self, text):
        # Existing canonical 24k PCM caches are reusable. Other WAV formats
        # keep using the validated streaming provider contract.
        path = voice_stream.cached_path(qa_bank.TTS_CACHE_DIR, text)
        try:
            with open(path, 'rb') as f:data = f.read(voice_stream.MAX_PCM + 45)
            if (44 < len(data) <= voice_stream.MAX_PCM + 44 and
                    len(data) % 2 == 0 and data[:44] == voice_stream.header(len(data) - 44)):
                for offset in range(44, len(data), 4096):yield data[offset:offset + 4096]
                return
        except OSError:
            pass
        source = iter(gateway.tts_pcm_stream(text))
        try:yield from source
        finally:
            if hasattr(source, 'close'):source.close()

    def _await_commit(self):
        with self._cv:
            while not self._committed and not self._cancelled:
                remaining = self._started + _LIFETIME - time.monotonic()
                if remaining <= 0:
                    raise TimeoutError('Uncommitted visual prefix')
                self._cv.wait(min(.05, remaining))
            if self._cancelled:
                raise RuntimeError('Visual prefix cancelled')
            return self._spoken, self._tail

    def _stream(self):
        chunks = []
        total = 0
        header_sent = False
        source = None
        slot = False
        try:
            # Share the existing three-stream audio budget as well as the
            # one speculative-job budget; do not add a fourth TTS producer.
            slot = voice_stream._slots.acquire(blocking=False)
            if not slot:
                raise RuntimeError('Voice stream capacity reached')
            for segment in range(2):
                if segment == 0:
                    text = self._prefix
                else:
                    spoken, text = self._await_commit()
                    if not text:
                        break
                segment_bytes = 0
                source = iter(self._segment(text))
                for pcm in source:
                    with self._cv:
                        if self._cancelled:
                            raise RuntimeError('Visual prefix cancelled')
                    if not isinstance(pcm, bytes) or not pcm or len(pcm) % 2:
                        raise ValueError('Invalid visual segment PCM')
                    total += len(pcm)
                    segment_bytes += len(pcm)
                    if total > voice_stream.MAX_PCM:
                        raise ValueError('Visual speech PCM limit')
                    if not header_sent:
                        yield voice_stream.header(voice_stream.MAX_PCM)
                        header_sent = True
                    chunks.append(pcm)
                    yield pcm
                if hasattr(source, 'close'):
                    source.close()
                source = None
                # A successful empty tail/prefix is not a complete utterance.
                if not segment_bytes:
                    raise ValueError('Empty visual segment PCM')
                if segment == 0:
                    _, tail = self._await_commit()
                    # Only a successfully validated full caption permits a
                    # reusable first-clause cache; never cache failed SSE.
                    if tail:voice_stream.commit_pcm(self._prefix, chunks, qa_bank.TTS_CACHE_DIR)
            spoken, _ = self._await_commit()
            voice_stream.commit_pcm(spoken, chunks, qa_bank.TTS_CACHE_DIR)
        except BaseException as error:
            # Before any PCM/header was queued, a failed speculative request
            # can still retain the ordinary whole-notice fallback, including
            # its existing non-streaming provider/WAV behavior. Never replay
            # a prefix after audio has begun.
            if isinstance(error, Exception) and not header_sent:
                try:
                    if source is not None and hasattr(source, 'close'):source.close()
                    source = None
                    if slot:
                        voice_stream._slots.release()
                        slot = False
                    spoken, _ = self._await_commit()
                    source = iter(voice_stream.stream_wav(spoken, gateway, qa_bank.TTS_CACHE_DIR))
                    yield from source
                except BaseException:
                    with self._cv:self._failed = True
                    raise
            else:
                with self._cv:self._failed = True
                raise
        finally:
            try:
                if source is not None and hasattr(source, 'close'):
                    source.close()
            finally:
                if slot:voice_stream._slots.release()

    def publish(self, text):
        global _pending
        if self._job is None:
            return False
        spoken = voice_tts_text((text or '').strip())
        if os.path.exists(voice_stream.cached_path(qa_bank.TTS_CACHE_DIR, spoken)):
            self.close_unpublished()
            return False
        with self._cv:
            tail = spoken[len(self._prefix):]
            valid = (not self._failed and not self._cancelled and
                     spoken.startswith(self._prefix) and
                     voice_tts_text(tail) == tail and
                     time.monotonic() - self._started < _LIFETIME)
            if valid:
                self._spoken, self._tail = spoken, tail
                self._committed = True
                self._cv.notify_all()
        if not valid or not self._job.renew_claim(_LIFETIME):
            self.close_unpublished()
            return False
        with _lock:
            _expire_locked(time.monotonic())
            if _pending is not None:
                self.close_unpublished()
                return False
            _pending = (self._owner, spoken, self._job, time.monotonic())
            self._published = True
        stats = self._job.stats()
        try:
            print('[VisionPrefix] published=1 prefix_chars=%u full_chars=%u prefix_to_publish_ms=%u ready_before_publish=%u' %
                  (len(self._prefix), len(spoken), round((time.monotonic() - self._started) * 1000),
                   int('first_ready_ms' in stats)), flush=True)
        except OSError:
            pass
        return True

    def close_unpublished(self):
        if self._published:
            return
        with self._cv:
            self._cancelled = True
            self._cv.notify_all()
        if self._job is not None:
            self._job.cancel()


def begin(owner):
    if not isinstance(owner, str) or not owner.startswith('device:'):
        return None
    try:return _Prefix(owner)
    except Exception:return None
