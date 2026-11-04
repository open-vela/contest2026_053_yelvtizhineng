"""Transient routing after a real pre-content upstream failure. No user memory."""
import threading,time
class FastFallbackRoute:
    def __init__(self, seconds=60, clock=time.monotonic):
        self.seconds=seconds;self.clock=clock;self.lock=threading.Lock()
        self.primary=None;self.until=0.
    def order(self, models):
        models=list(models)
        with self.lock:
            if len(models)==2 and models[0]==self.primary and self.clock()<self.until:
                return [models[1],models[0]]
        return models
    def failed(self, primary):
        with self.lock:self.primary=primary;self.until=self.clock()+self.seconds
    def succeeded(self, model):
        with self.lock:
            if model==self.primary:self.until=0.
    def remaining_ms(self):
        with self.lock:return max(0,round((self.until-self.clock())*1000))
