"""Standard TTS with early PCM delivery; tone shaping belongs to the BOX."""
import hashlib
import io
import json
import struct
import threading
import time
import uuid
import wave
from pathlib import Path
from fastapi import Depends, HTTPException
from fastapi.responses import Response, StreamingResponse
from pydantic import BaseModel, Field
from starlette.background import BackgroundTask
from app import config
from app.services import voice_profile

PROFILE = 'standard-stream-local-soft-v1'


def wav_header(length=0xffffffff):
    return struct.pack('<4sI4s4sIHHIIHH4sI', b'RIFF', length + 36 if length < 0xffffffdb else 0xffffffff,
                       b'WAVE', b'fmt ', 16, 1, 1, 24000, 48000, 2, 16, b'data', length)


class VoiceRequest(BaseModel):
    text: str = Field(..., min_length=1, max_length=400)
    stream_audio: bool = False


class SpeechSource:
    def __init__(self, generator, slots):
        self.generator = generator
        self.slots = slots
        self.lock = threading.Lock()
        self.closed = False

    def close(self):
        with self.lock:
            if not self.closed:
                self.closed = True
                try:
                    self.generator.close()
                finally:
                    self.slots.release()


class StandardVoice:
    def __init__(self, gateway, text_filter):
        self.gateway = gateway
        self.text_filter = text_filter
        self.lock = threading.Lock()
        self.slots = threading.BoundedSemaphore(2)
        self.cache = {}
        self.cache_bytes = 0
        self.sessions = {}
        self.max_audio = 4 * 1024 * 1024

    def key(self, text):
        profile = [PROFILE, config.TTS_MODEL, voice_profile.VOICE, voice_profile.STYLE, text]
        return hashlib.sha256(json.dumps(profile, ensure_ascii=False).encode()).hexdigest()

    def remember(self, key, pcm):
        audio = wav_header(len(pcm)) + bytes(pcm)
        with self.lock:
            if key in self.cache:
                return
            while self.cache and self.cache_bytes + len(audio) > 16_000_000:
                old = next(iter(self.cache))
                self.cache_bytes -= len(self.cache.pop(old))
            self.cache[key] = audio
            self.cache_bytes += len(audio)

    def prepare(self, text):
        if not self.slots.acquire(blocking=False):
            raise HTTPException(503, '语音正在准备，请稍后再试')
        source = None
        try:
            source = SpeechSource(self.gateway.tts_pcm_stream(text), self.slots)
            first = next(source.generator)
            if not first or len(first) % 2:
                raise ValueError('Invalid first PCM')
            return source, first
        except Exception as error:
            if source is not None:
                source.close()
            else:
                self.slots.release()
            print('[BOX Standard Voice] first audio failed type=' + type(error).__name__, flush=True)
            raise HTTPException(502, '语音暂时没有准备好，请再试一次')

    def body(self, source, first, key):
        pcm = bytearray()
        completed = False
        try:
            yield wav_header()
            for part in self.parts(source, first):
                if len(part) % 2 or len(pcm) + len(part) > self.max_audio:
                    raise ValueError('Invalid PCM length')
                pcm.extend(part)
                yield part
            completed = True
            self.remember(key, pcm)
        finally:
            source.close()
            print('[BOX Standard Voice] stream_complete=' + str(int(completed)) + ' pcm_bytes=' + str(len(pcm)), flush=True)

    @staticmethod
    def parts(source, first):
        yield first
        yield from source.generator

    def notice(self, body, subject):
        text = ' '.join(self.text_filter(body.text, max_chars=120).split())
        if not text:
            raise HTTPException(400, '播报文字不能为空')
        key = self.key(text)
        with self.lock:
            cached = self.cache.get(key)
        if cached is None and not body.stream_audio:
            # Compatibility for the preceding firmware: still standard TTS,
            # never voice cloning. New firmware requests streaming sessions.
            source, first = self.prepare(text)
            pcm = bytearray()
            try:
                for part in self.parts(source, first):
                    if len(part) % 2 or len(pcm) + len(part) > self.max_audio:
                        raise ValueError('Invalid PCM length')
                    pcm.extend(part)
                self.remember(key, pcm)
            finally:
                source.close()
        sid = 'nv_' + uuid.uuid4().hex
        with self.lock:
            now = time.monotonic()
            for old in list(self.sessions):
                if now - self.sessions[old][0] > 180:
                    del self.sessions[old]
            while len(self.sessions) >= 32:
                del self.sessions[next(iter(self.sessions))]
            self.sessions[sid] = (now, subject, key, text, False)
        return {'ok': True, 'session_id': sid, 'has_audio': True, 'profile': PROFILE,
                'stream_audio': body.stream_audio}

    def audio(self, sid, subject):
        with self.lock:
            saved = self.sessions.get(sid)
            if not saved or saved[1] != subject or time.monotonic() - saved[0] > 180:
                raise HTTPException(404, '语音会话已过期')
            created, owner, key, text, used = saved
            cached = self.cache.get(key)
            if cached is None and used:
                raise HTTPException(409, '这次语音已经在播放')
            if cached is None:
                self.sessions[sid] = (created, owner, key, text, True)
        headers = {'Cache-Control': 'no-store', 'X-BOX-Voice': PROFILE, 'X-Accel-Buffering': 'no'}
        if cached is not None:
            return Response(cached, media_type='audio/wav', headers=headers)
        started = time.monotonic()
        source, first = self.prepare(text)
        print('[BOX Standard Voice] first_pcm_ms=' + str(round((time.monotonic() - started) * 1000)), flush=True)
        return StreamingResponse(self.body(source, first, key), media_type='audio/wav', headers=headers,
                                 background=BackgroundTask(source.close))


def install_standard_voice(app, authorized, gateway, text_filter):
    service = StandardVoice(gateway, text_filter)
    # Normal-stock reference prompts are prepared once, not cloned per request.
    for name, text in [('wake', '我在呢，你想做什么？')]:
        path = Path(__file__).with_name('standard_tts_probe_20261103') / (name + '.wav')
        if path.exists():
            with wave.open(str(path), 'rb') as wav:
                if (wav.getnchannels(), wav.getsampwidth(), wav.getframerate()) != (1, 2, 24000):
                    raise ValueError('Unexpected fixed prompt format')
                pcm = wav.readframes(wav.getnframes())
                if len(pcm) != wav.getnframes() * 2:
                    raise ValueError('Incomplete fixed prompt')
            service.remember(service.key(text), pcm)

    @app.get('/api/v1/box/voice/profile')
    def standard_profile(auth=Depends(authorized)):
        return {'ok': True, 'profile': PROFILE, 'model': config.TTS_MODEL,
                'voice': voice_profile.VOICE, 'processing': 'BOX local soft EQ', 'voice_clone': False}

    @app.post('/api/v1/box/voice/notice')
    def standard_notice(body: VoiceRequest, auth=Depends(authorized)):
        return service.notice(body, auth[0])

    @app.get('/api/v1/box/voice/audio')
    def standard_audio(session_id: str, auth=Depends(authorized)):
        return service.audio(session_id, auth[0])

    return service
