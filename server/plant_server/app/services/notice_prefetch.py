"""One bounded, cancellable WAV prefetch for an already decided notice.

The producer owns its provider iterator/thread throughout. Bytes retain their
original framing and order; the existing WAV producer owns validation/cache.
Other notices fall back to the existing lazy stream when this slot is occupied.
"""
from collections import deque
from concurrent.futures import ThreadPoolExecutor
import threading
import time

_slots = threading.BoundedSemaphore(1)
_workers = ThreadPoolExecutor(max_workers=1, thread_name_prefix='m5_notice_pcm')


class Prefetch:
    MAX_BYTES = 4 * 1024 * 1024

    def __init__(self, factory, ahead_bytes=65536, claim_seconds=5):
        self._cv = threading.Condition()
        self._queue = deque()
        self._queued = self._total = 0
        self._limit = ahead_bytes
        self._done = self._cancelled = self._claimed = self._released = False
        self._error = None
        self._origin = time.monotonic()
        self._marks = {}
        self._timer_epoch = 0
        self._timer = threading.Timer(claim_seconds, self._expire, args=(self._timer_epoch,))
        self._timer.daemon = True
        self._timer.start()
        try:
            self._future = _workers.submit(self._run, factory)
        except BaseException:
            self._timer.cancel()
            raise

    def _mark(self, name):
        self._marks.setdefault(name, round((time.monotonic() - self._origin) * 1000))

    def _release(self):
        # Hold admission until BOTH producer exit and consumer cancellation/
        # completion, bounding completed-but-unclaimed buffers and timers too.
        if self._done and self._cancelled and not self._released:
            self._released = True
            _slots.release()

    def _expire(self, epoch):
        with self._cv:
            if epoch == self._timer_epoch and not self._claimed:
                self._cancel_locked()

    def _cancel_locked(self):
        self._cancelled = True
        self._queue.clear()
        self._queued = 0
        self._cv.notify_all()
        self._release()

    def cancel(self):
        with self._cv:
            self._timer.cancel()
            self._cancel_locked()

    def renew_claim(self, seconds):
        """Give a newly committed speculative job a full consumer window."""
        if seconds <= 0:
            raise ValueError('Invalid claim window')
        with self._cv:
            if self._claimed or self._cancelled or self._error is not None:
                return False
            self._timer_epoch += 1
            self._timer.cancel()
            self._timer = threading.Timer(seconds, self._expire, args=(self._timer_epoch,))
            self._timer.daemon = True
            try:self._timer.start()
            except Exception:
                self._cancel_locked()
                return False
            return True

    def _run(self, factory):
        source = None
        try:
            with self._cv:
                if self._cancelled:
                    return
                self._mark('worker_start_ms')
            source = iter(factory())
            for block in source:
                if not isinstance(block, bytes) or not block:
                    raise ValueError('Invalid WAV stream block')
                with self._cv:
                    self._total += len(block)
                    if self._total > self.MAX_BYTES:
                        raise ValueError('WAV prefetch limit')
                    # One valid fallback WAV may exceed the ahead window; an
                    # empty queue accepts it, still within the total cap.
                    while self._queue and self._queued + len(block) > self._limit and not self._cancelled:
                        self._cv.wait(.1)
                    if self._cancelled:
                        return
                    self._mark('first_ready_ms')
                    self._queue.append(block)
                    self._queued += len(block)
                    self._cv.notify_all()
            if not self._total:
                raise ValueError('Empty WAV stream')
        except BaseException as error:
            with self._cv:
                # Do not retain provider exception text/tracebacks or keys.
                self._error = type(error).__name__
                self._queue.clear()
                self._queued = 0
        finally:
            if source is not None and hasattr(source, 'close'):
                try:
                    source.close()
                except Exception:
                    pass
            with self._cv:
                self._mark('producer_done_ms')
                self._done = True
                self._cv.notify_all()
                self._release()

    def claim(self):
        with self._cv:
            if self._cancelled or self._claimed:
                return False
            self._claimed = True
            self._mark('claim_ms')
            self._timer.cancel()
            return True

    def blocks(self):
        try:
            while True:
                with self._cv:
                    if not self._claimed:
                        raise RuntimeError('Unclaimed notice stream')
                    while not self._queue and not self._done and not self._cancelled:
                        self._cv.wait(.1)
                    if self._cancelled:
                        raise RuntimeError('Notice stream cancelled')
                    if self._error:
                        raise RuntimeError('Notice producer failed: ' + self._error)
                    if not self._queue:
                        break
                    block = self._queue.popleft()
                    self._queued -= len(block)
                    self._cv.notify_all()
                yield block
        finally:
            self.cancel()

    def stats(self):
        with self._cv:
            return dict(self._marks, total_bytes=self._total, queued_bytes=self._queued,
                        producer_done=self._done, cancelled=self._cancelled)


def start(factory, *, ahead_bytes=65536, claim_seconds=5):
    if ahead_bytes < 1 or claim_seconds <= 0:
        raise ValueError('Invalid notice prefetch bounds')
    if not _slots.acquire(blocking=False):
        return None
    try:
        return Prefetch(factory, ahead_bytes, claim_seconds)
    except BaseException:
        _slots.release()
        return None
