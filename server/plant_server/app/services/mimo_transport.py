"""Connection timing only. Preserve Requests' TLS, proxy, pooling and retries."""
import time
import requests
from requests.adapters import HTTPAdapter
from urllib3.connection import HTTPSConnection
from urllib3.connectionpool import HTTPSConnectionPool


class TimedHTTPSConnection(HTTPSConnection):
    def connect(self):
        self._plant_connect_start = time.monotonic_ns()
        try:
            return super().connect()
        finally:
            self._plant_connect_end = time.monotonic_ns()


class TimedHTTPSPool(HTTPSConnectionPool):
    ConnectionCls = TimedHTTPSConnection


class TimedAdapter(HTTPAdapter):
    @staticmethod
    def _observe_manager(manager):
        # PoolManager initially points at urllib3's shared class dictionary.
        # Copy it so an unrelated Session's transport is never changed.
        manager.pool_classes_by_scheme = dict(manager.pool_classes_by_scheme)
        manager.pool_classes_by_scheme['https'] = TimedHTTPSPool

    def init_poolmanager(self, *args, **kwargs):
        super().init_poolmanager(*args, **kwargs)
        self._observe_manager(self.poolmanager)

    def proxy_manager_for(self, *args, **kwargs):
        manager = super().proxy_manager_for(*args, **kwargs)
        if manager.pool_classes_by_scheme.get('https') is not TimedHTTPSPool:
            self._observe_manager(manager)
        return manager

    def send(self, request, *args, **kwargs):
        start = time.monotonic_ns()
        response = super().send(request, *args, **kwargs)
        connection = getattr(response.raw, 'connection', None)
        connected_at = getattr(connection, '_plant_connect_start', None)
        completed_at = getattr(connection, '_plant_connect_end', None)
        response._plant_connect_ms = -1
        response._plant_fresh_connection = -1
        if connected_at is not None and completed_at is not None:
            fresh = connected_at >= start
            response._plant_fresh_connection = int(fresh)
            response._plant_connect_ms = (completed_at-connected_at)//1000000 if fresh else 0
        return response


def make_session():
    session = requests.Session()
    session.get_adapter('https://').close()  # The fresh default pool is unused.
    session.mount('https://', TimedAdapter())
    return session

class SSEBody:
    """Keep the body iterator so DONE can be followed by bounded HTTP EOF.

    Use Requests' own line splitting. Draining consumes body chunks directly,
    not potentially unbounded lines; a socket watchdog also bounds trickle peers.
    Only call finish after valid DONE, never after cancellation/partial failure.
    """
    def __init__(self,response,chunk_size):
        self.response=response
        self.chunks=response.iter_content(chunk_size=chunk_size,decode_unicode=False)

    def iter_content(self,chunk_size=None,decode_unicode=False):
        return self.chunks

    def lines(self):
        return requests.Response.iter_lines(self)

    def finish(self):
        import socket,threading
        started=time.monotonic();raw=self.response.raw
        connection=getattr(raw,'_connection',None);sock=getattr(connection,'sock',None)
        # A detached closed body cannot block on network. An unfamiliar active
        # transport is closed normally rather than drained without a watchdog.
        if sock is None and not getattr(raw,'closed',False):
            return {'outcome':'unavailable','elapsed_ms':0,'bytes':0}
        finished=threading.Event();expired=threading.Event();timer=None;timer_started=False
        def interrupt():
            if not finished.is_set():
                expired.set()
                try:sock.shutdown(socket.SHUT_RDWR)
                except OSError:pass
        outcome='limited';total=0;chunks=0
        try:
            if sock is not None:
                timer=threading.Timer(.040,interrupt);timer.daemon=True;timer.start();timer_started=True
            while time.monotonic()-started<.040 and total<=16384 and chunks<16384:
                try:block=next(self.chunks)
                except StopIteration:
                    outcome='drained';break
                total+=len(block);chunks+=1
        except Exception:
            # DONE already established business success. A stalled/malformed
            # HTTP trailer must only lose keep-alive, never replay the answer.
            outcome='timeout' if expired.is_set() else 'error'
        finally:
            finished.set()
            if timer is not None:
                timer.cancel()
                if timer_started:timer.join() # No watchdog may touch a reused socket.
        if expired.is_set() and outcome!='drained':outcome='timeout'
        return {'outcome':outcome,'elapsed_ms':round((time.monotonic()-started)*1000),'bytes':total}


class IntentStreamDeadline:
    """Absolute body deadline, including heartbeats and incomplete SSE lines.

    Hold the urllib3 connection until the watchdog has joined. Otherwise EOF
    could return a socket to the pool while its old watchdog still owns it.
    Only the device intent stream opts in; normal pooled requests are unchanged.
    """
    def __init__(self, response, deadline, first_content_deadline=None):
        import threading
        self.response = response
        self.total_deadline = deadline
        self.first_content_deadline = first_content_deadline
        self.deadline = min(deadline,first_content_deadline) if first_content_deadline is not None else deadline
        self.interrupt = None
        self.expired = threading.Event()
        self.timer = None
        self.raw = getattr(response, 'raw', None)
        self.release = None
        self.deferred = False

    def check(self):
        if self.expired.is_set() or time.monotonic() >= self.deadline:
            raise requests.Timeout('Intent stream deadline')

    def content_complete(self):
        """Valid DONE ends the content deadline; retain socket ownership.

        Bounded40ms trailer drain is transport cleanup, not another answer.
        Never let a still-running watchdog touch a returned pooled socket.
        """
        self.check()
        if self.timer is not None:
            self.timer.cancel()
            self.timer.join()
            self.timer = None
        if self.expired.is_set():
            raise requests.Timeout('Response body deadline')

    def content_started(self):
        """Only the first real text delta changes the first-content deadline.

        Cancel and join the old watchdog while retaining socket ownership;
        a timer at its boundary wins and fails closed instead of rearming.
        """
        if self.first_content_deadline is None:
            self.check()
            return
        self.content_complete()
        self.first_content_deadline = None
        self.deadline = self.total_deadline
        self.check()
        if self.raw is None:return
        import threading
        timer=threading.Timer(max(0,self.deadline-time.monotonic()),self.interrupt)
        timer.daemon=True
        self.timer=timer
        try:timer.start()
        except BaseException:
            self.timer=None
            raise

    def __enter__(self):
        import socket, threading
        self.check()
        if self.raw is None:  # Protocol doubles: checked at every yielded line.
            return self
        connection = getattr(self.raw, '_connection', None)
        sock = getattr(connection, 'sock', None)
        if sock is None:
            try:
                sock = self.raw._fp.fp.raw._sock
            except AttributeError:
                raise requests.Timeout('Intent stream has no interruptible socket')
        if self.first_content_deadline is not None:
            # The short HTTP-header read timeout must not shorten body reads.
            # Our socket watchdog still enforces the first-content deadline.
            try:sock.settimeout(max(.001,self.total_deadline-time.monotonic()))
            except OSError:pass
        self.release = self.raw.release_conn
        def defer_release():
            self.deferred = True
        self.raw.release_conn = defer_release
        def interrupt():
            self.expired.set()
            try:
                sock.shutdown(socket.SHUT_RDWR)
            except OSError:
                pass
        self.interrupt = interrupt
        try:
            self.timer = threading.Timer(max(0, self.deadline-time.monotonic()), interrupt)
            self.timer.daemon = True
            self.timer.start()
        except BaseException:
            self.raw.release_conn = self.release
            raise
        return self

    def __exit__(self, exc_type, exc, tb):
        if self.timer is not None:
            self.timer.cancel()
            self.timer.join()
        try:
            if self.expired.is_set():
                self.raw.close()  # Discard the interrupted connection.
        finally:
            if self.release is not None:
                self.raw.release_conn = self.release
                if self.deferred:
                    self.release()
        if self.expired.is_set() and (exc_type is None or issubclass(exc_type, Exception)):
            raise requests.Timeout('Response body deadline') from exc
        return False

# The same connection-owning guard is shared by intent, vision and TTS.
ResponseDeadline = IntentStreamDeadline
