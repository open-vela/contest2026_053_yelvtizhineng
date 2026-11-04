"""Bounded, complete-caption SSE collection. No partial caption is published."""
import json
import time


class CaptionStreamError(RuntimeError):
    pass


def complete_caption(post, url, headers, payload, total_seconds=12, on_prefix=None):
    start = time.monotonic()
    request = dict(payload)
    request['stream'] = True
    parts = []
    wire_bytes = 0
    chars = 0
    first_ms = None
    finished = False
    done = False
    drain = None
    # The body watchdog covers heartbeats and partial SSE/HTTP chunk lines.
    # HTTP headers, DNS and connection setup retain transport timeout limits.
    from .mimo_transport import ResponseDeadline
    from urllib3.util import Timeout
    with post(url, headers=headers, json=request, stream=True,
              timeout=Timeout(total=total_seconds,connect=min(4,total_seconds),read=min(6,total_seconds))) as response:
        with ResponseDeadline(response,start+total_seconds) as guard:
            if response.status_code != 200:
                raise CaptionStreamError('vision upstream HTTP %d' % response.status_code)
            headers_ms = int((time.monotonic() - start) * 1000)
            connect_ms = getattr(response, '_plant_connect_ms', -1)
            fresh = getattr(response, '_plant_fresh_connection', -1)
            body = None
            if getattr(response, 'raw', None) is not None and hasattr(response, 'iter_content'):
                from .mimo_transport import SSEBody
                body = SSEBody(response, 1)
            lines = body.lines() if body is not None else response.iter_lines(chunk_size=1)
            for line in lines:
                if time.monotonic() - start > total_seconds:
                    raise CaptionStreamError('vision stream deadline')
                wire_bytes += len(line)
                if wire_bytes > 131072:
                    raise CaptionStreamError('vision stream response limit')
                if not line.startswith(b'data:'):
                    continue
                raw = line[5:].strip()
                if raw == b'[DONE]':
                    done = True
                    break
                try:
                    obj = json.loads(raw)
                    if not isinstance(obj, dict) or obj.get('error'):
                        raise ValueError()
                    choices = obj.get('choices') or []
                    if not isinstance(choices, list) or len(choices) > 1:
                        raise ValueError()
                    if not choices:
                        continue  # Optional usage-only event.
                    choice = choices[0]
                    reason = choice.get('finish_reason')
                    if reason not in (None, 'stop'):
                        raise CaptionStreamError('vision stream incomplete completion')
                    delta = choice.get('delta') or {}
                    content = delta.get('content') or ''
                    if not isinstance(content, str):
                        raise ValueError()
                    if content:
                        if finished:
                            raise CaptionStreamError('vision text after completion')
                        chars += len(content)
                        if chars > 320:
                            raise CaptionStreamError('vision caption limit')
                        if first_ms is None:
                            first_ms = int((time.monotonic() - start) * 1000)
                        parts.append(content)
                        if on_prefix is not None:
                            try:on_prefix(''.join(parts))
                            except Exception:pass  # Best-effort, never invalidate the caption.

                    if reason == 'stop':
                        finished = True
                except CaptionStreamError:
                    raise
                except (ValueError, TypeError, AttributeError, KeyError):
                    raise CaptionStreamError('invalid vision stream event') from None
            # Only a validated complete answer may consume its HTTP trailer.
            # Retain the Requests iterator so breaking at DONE does not discard a
            # reusable connection. Existing SSEBody caps bytes and wall time.
            if done and finished and ''.join(parts).strip():
                guard.content_complete()
                if body is not None:drain = body.finish()
    text = ''.join(parts).strip()
    if not done or not finished or not text:
        raise CaptionStreamError('incomplete vision stream')
    return text, {'headers_ms': headers_ms, 'first_text_ms': first_ms,
                  'connect_ms': connect_ms, 'fresh': fresh, 'drain': drain,
                  'complete_ms': int((time.monotonic() - start) * 1000)}
