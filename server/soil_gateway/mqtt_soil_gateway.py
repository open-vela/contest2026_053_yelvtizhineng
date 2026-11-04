#!/usr/bin/env python3
"""Bridge plant soil telemetry from MQTT to plant_server's HTTP API.

The cyStudio device publishes one JSON object to ``plant/<device_id>/sensor``.
This process validates that object, resolves the device token, and posts the
eight telemetry fields to ``/api/v1/telemetry/put``. Failed posts are kept in a
bounded JSONL queue and retried in the background.

Only the MQTT transport needs an external package:

    python -m pip install paho-mqtt

All other functionality uses the Python standard library.
"""

from __future__ import annotations

import json
import logging
import math
import os
import ssl
import tempfile
import threading
import urllib.error
import urllib.request
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Callable, Mapping

LOG = logging.getLogger("soil-mqtt")

CANONICAL_FIELDS = (
    "moisture",
    "temp",
    "ec",
    "ph",
    "salt",
    "nitrogen",
    "phosphorus",
    "potassium",
)

FIELD_ALIASES = {
    "moisture": ("moisture", "soil_moisture", "humidity"),
    "temp": ("temp", "temperature", "soil_temp", "soil_temperature"),
    "ec": ("ec", "conductivity", "soil_ec"),
    "ph": ("ph", "soil_ph"),
    "salt": ("salt", "salinity"),
    "nitrogen": ("nitrogen", "n"),
    "phosphorus": ("phosphorus", "p"),
    "potassium": ("potassium", "k"),
}


class TelemetryError(ValueError):
    """The MQTT payload does not match the telemetry contract."""


class TokenError(RuntimeError):
    """No API token is available for the topic/device."""


def _number(value: Any, field: str) -> float:
    if isinstance(value, bool) or value is None:
        raise TelemetryError(f"{field} must be a finite number")
    try:
        number = float(value)
    except (TypeError, ValueError) as exc:
        raise TelemetryError(f"{field} must be a finite number") from exc
    if not math.isfinite(number):
        raise TelemetryError(f"{field} must be a finite number")
    return number


def normalize_telemetry(payload: bytes | str | Mapping[str, Any]) -> dict[str, float]:
    """Return the eight canonical fields, accepting common field aliases."""
    if isinstance(payload, bytes):
        try:
            payload = payload.decode("utf-8")
        except UnicodeDecodeError as exc:
            raise TelemetryError("payload is not UTF-8") from exc

    if isinstance(payload, str):
        try:
            data = json.loads(payload)
        except json.JSONDecodeError as exc:
            raise TelemetryError("payload is not valid JSON") from exc
    elif isinstance(payload, Mapping):
        data = dict(payload)
    else:
        raise TelemetryError("payload must be a JSON object")

    if not isinstance(data, dict):
        raise TelemetryError("payload must be a JSON object")

    # Accept a small amount of wrapper variation from gateways/devices.
    for wrapper in ("data", "telemetry", "payload"):
        nested = data.get(wrapper)
        if isinstance(nested, dict):
            data = nested
            break

    result: dict[str, float] = {}
    for field in CANONICAL_FIELDS:
        raw = next((data[key] for key in FIELD_ALIASES[field] if key in data), None)
        if raw is None:
            raise TelemetryError(f"missing required field: {field}")
        result[field] = _number(raw, field)
    return result


def device_id_from_topic(topic: str, fallback: str = "") -> str:
    """Extract the device id from plant/<device_id>/sensor-like topics."""
    parts = [part for part in topic.strip("/").split("/") if part]
    if len(parts) >= 3 and parts[0] == "plant":
        return "/".join(parts[1:-1])
    if len(parts) >= 2:
        return parts[-2]
    return fallback


@dataclass(frozen=True)
class TokenResolver:
    default: str = ""
    by_device: Mapping[str, str] | None = None

    def get(self, device_id: str) -> str:
        if self.by_device:
            token = self.by_device.get(device_id) or self.by_device.get("*")
            if token:
                return token
        if self.default:
            return self.default
        raise TokenError(f"no API token configured for device {device_id!r}")


@dataclass(frozen=True)
class QueueItem:
    token: str
    payload: Mapping[str, float]


class JsonlQueue:
    """A small, ordered, bounded retry queue persisted as one JSON per line."""

    def __init__(self, path: str | Path, max_items: int = 5000) -> None:
        self.path = Path(path)
        self.max_items = max(1, max_items)
        self._lock = threading.Lock()
        self._items: list[QueueItem] = []
        self._load()

    def _load(self) -> None:
        if not self.path.exists():
            return
        try:
            with self.path.open("r", encoding="utf-8") as handle:
                for line in handle:
                    line = line.strip()
                    if not line:
                        continue
                    item = json.loads(line)
                    payload = item.get("payload")
                    token = item.get("token")
                    if isinstance(token, str) and isinstance(payload, dict):
                        self._items.append(QueueItem(token, payload))
        except (OSError, json.JSONDecodeError) as exc:
            LOG.warning("cannot load retry queue %s: %s", self.path, exc)
            return
        self._items = self._items[-self.max_items :]

    def _persist_locked(self) -> None:
        self.path.parent.mkdir(parents=True, exist_ok=True)
        fd, temp_name = tempfile.mkstemp(
            prefix=self.path.name + ".", suffix=".tmp", dir=str(self.path.parent)
        )
        try:
            with os.fdopen(fd, "w", encoding="utf-8") as handle:
                for item in self._items:
                    handle.write(
                        json.dumps(
                            {"token": item.token, "payload": item.payload},
                            ensure_ascii=False,
                            separators=(",", ":"),
                        )
                        + "\n"
                    )
            os.replace(temp_name, self.path)
        finally:
            try:
                os.unlink(temp_name)
            except FileNotFoundError:
                pass

    def __len__(self) -> int:
        with self._lock:
            return len(self._items)

    def append(self, item: QueueItem) -> None:
        with self._lock:
            self._items.append(item)
            if len(self._items) > self.max_items:
                del self._items[: len(self._items) - self.max_items]
            self._persist_locked()

    def flush(self, poster: Callable[[str, Mapping[str, float]], None]) -> int:
        """Post queued items in order until one fails. Returns delivered count."""
        delivered = 0
        while True:
            with self._lock:
                if not self._items:
                    return delivered
                item = self._items[0]
            try:
                poster(item.token, item.payload)
            except Exception as exc:  # retry later; keep the item at the head
                LOG.warning("queued telemetry retry failed: %s", exc)
                return delivered
            with self._lock:
                if self._items and self._items[0] == item:
                    self._items.pop(0)
                    self._persist_locked()
            delivered += 1


def post_telemetry(
    url: str,
    token: str,
    payload: Mapping[str, float],
    timeout_s: float = 10.0,
) -> None:
    body = json.dumps(payload, separators=(",", ":"), ensure_ascii=False).encode("utf-8")
    request = urllib.request.Request(
        url,
        data=body,
        method="POST",
        headers={
            "Content-Type": "application/json",
            "X-Device-Token": token,
            "User-Agent": "plant-soil-mqtt-bridge/1.0",
        },
    )
    try:
        with urllib.request.urlopen(request, timeout=timeout_s) as response:
            if not 200 <= response.status < 300:
                raise RuntimeError(f"telemetry API returned HTTP {response.status}")
            response.read(1024)
    except urllib.error.HTTPError as exc:
        detail = exc.read(512).decode("utf-8", "replace")
        raise RuntimeError(f"telemetry API HTTP {exc.code}: {detail}") from exc
    except urllib.error.URLError as exc:
        raise RuntimeError(f"telemetry API unreachable: {exc.reason}") from exc


def heartbeat_url(telemetry_url: str) -> str:
    marker = "/api/v1/"
    index = telemetry_url.find(marker)
    if index < 0:
        raise ValueError("telemetry URL does not contain /api/v1/")
    return telemetry_url[:index] + "/api/v1/devices/heartbeat"


def post_heartbeat(url: str, token: str, timeout_s: float = 10.0) -> None:
    request = urllib.request.Request(
        url,
        data=b"",
        method="POST",
        headers={
            "X-Device-Token": token,
            "User-Agent": "plant-soil-mqtt-bridge/1.0",
        },
    )
    try:
        with urllib.request.urlopen(request, timeout=timeout_s) as response:
            if not 200 <= response.status < 300:
                raise RuntimeError(f"heartbeat API returned HTTP {response.status}")
            response.read(1024)
    except urllib.error.HTTPError as exc:
        detail = exc.read(512).decode("utf-8", "replace")
        raise RuntimeError(f"heartbeat API HTTP {exc.code}: {detail}") from exc
    except urllib.error.URLError as exc:
        raise RuntimeError(f"heartbeat API unreachable: {exc.reason}") from exc


@dataclass(frozen=True)
class GatewayConfig:
    mqtt_host: str = "127.0.0.1"
    mqtt_port: int = 1883
    mqtt_user: str = ""
    mqtt_password: str = ""
    mqtt_topic: str = "plant/+/sensor"
    mqtt_client_id: str = "plant-soil-mqtt-bridge"
    mqtt_qos: int = 1
    mqtt_tls: bool = False
    mqtt_ca_file: str = ""
    mqtt_tls_insecure: bool = False
    api_url: str = "http://127.0.0.1:8000/api/v1/telemetry/put"
    api_timeout_s: float = 10.0
    queue_path: str = "mqtt_soil_queue.jsonl"
    queue_max_items: int = 5000
    retry_s: float = 10.0
    fallback_device_id: str = "soil-node-1"
    default_token: str = ""
    device_tokens: Mapping[str, str] | None = None

    @classmethod
    def from_env(cls, env: Mapping[str, str] | None = None) -> "GatewayConfig":
        env = os.environ if env is None else env

        def get(name: str, default: str = "") -> str:
            return env.get(name, default)

        tokens: Mapping[str, str] | None = None
        tokens_raw = get("SOIL_DEVICE_TOKENS").strip()
        if tokens_raw:
            try:
                parsed = json.loads(tokens_raw)
            except json.JSONDecodeError as exc:
                raise ValueError("SOIL_DEVICE_TOKENS must be a JSON object") from exc
            if not isinstance(parsed, dict):
                raise ValueError("SOIL_DEVICE_TOKENS must be a JSON object")
            tokens = {str(key): str(value) for key, value in parsed.items()}

        return cls(
            mqtt_host=get("SOIL_MQTT_HOST", cls.mqtt_host),
            mqtt_port=int(get("SOIL_MQTT_PORT", str(cls.mqtt_port))),
            mqtt_user=get("SOIL_MQTT_USER"),
            mqtt_password=get("SOIL_MQTT_PASS"),
            mqtt_topic=get("SOIL_MQTT_TOPIC", cls.mqtt_topic),
            mqtt_client_id=get("SOIL_MQTT_CLIENT_ID", cls.mqtt_client_id),
            mqtt_qos=int(get("SOIL_MQTT_QOS", str(cls.mqtt_qos))),
            mqtt_tls=get("SOIL_MQTT_TLS").lower() in {"1", "true", "yes", "on"},
            mqtt_ca_file=get("SOIL_MQTT_CA_FILE"),
            mqtt_tls_insecure=get("SOIL_MQTT_TLS_INSECURE").lower()
            in {"1", "true", "yes", "on"},
            api_url=get("SOIL_API_URL", cls.api_url),
            api_timeout_s=float(get("SOIL_API_TIMEOUT", str(cls.api_timeout_s))),
            queue_path=get("SOIL_QUEUE_PATH", cls.queue_path),
            queue_max_items=int(get("SOIL_QUEUE_MAX", str(cls.queue_max_items))),
            retry_s=float(get("SOIL_RETRY_SEC", str(cls.retry_s))),
            fallback_device_id=get("SOIL_DEVICE_ID", cls.fallback_device_id),
            default_token=get("SOIL_DEVICE_TOKEN"),
            device_tokens=tokens,
        )


class SoilGateway:
    """MQTT callbacks and HTTP forwarding, usable without an active broker in tests."""

    def __init__(
        self,
        config: GatewayConfig,
        poster: Callable[[str, Mapping[str, float]], None] | None = None,
        heartbeat: Callable[[str], None] | None = None,
    ) -> None:
        self.config = config
        self.tokens = TokenResolver(config.default_token, config.device_tokens)
        self.queue = JsonlQueue(config.queue_path, config.queue_max_items)
        custom_poster = poster is not None
        self.poster = poster or (
            lambda token, payload: post_telemetry(
                config.api_url, token, payload, config.api_timeout_s
            )
        )
        self.heartbeat = heartbeat
        if not custom_poster and heartbeat is None:
            url = heartbeat_url(config.api_url)
            self.heartbeat = lambda token: post_heartbeat(
                url, token, config.api_timeout_s
            )
        self._stop = threading.Event()
        self._thread: threading.Thread | None = None

    def handle_message(self, topic: str, payload: bytes) -> None:
        device_id = device_id_from_topic(topic, self.config.fallback_device_id)
        telemetry = normalize_telemetry(payload)
        token = self.tokens.get(device_id)
        try:
            self.poster(token, telemetry)
            LOG.info("telemetry delivered device=%s", device_id)
            if self.heartbeat:
                try:
                    self.heartbeat(token)
                except Exception as exc:
                    LOG.warning("heartbeat failed device=%s reason=%s", device_id, exc)
        except Exception as exc:
            LOG.warning("telemetry queued device=%s reason=%s", device_id, exc)
            self.queue.append(QueueItem(token, telemetry))

    def start(self) -> None:
        if self._thread and self._thread.is_alive():
            return
        self._thread = threading.Thread(target=self._flush_loop, name="soil-retry", daemon=True)
        self._thread.start()

    def stop(self) -> None:
        self._stop.set()
        if self._thread:
            self._thread.join(timeout=2)

    def _flush_loop(self) -> None:
        while not self._stop.is_set():
            if len(self.queue):
                delivered = self.queue.flush(self.poster)
                if delivered:
                    LOG.info("retry queue delivered=%d remaining=%d", delivered, len(self.queue))
            self._stop.wait(self.config.retry_s)

    def run(self) -> None:
        try:
            import paho.mqtt.client as mqtt
        except ImportError as exc:
            raise RuntimeError(
                "paho-mqtt is required: python -m pip install paho-mqtt"
            ) from exc

        callback_version = getattr(mqtt, "CallbackAPIVersion", None)
        if callback_version is not None:
            client = mqtt.Client(
                callback_api_version=callback_version.VERSION2,
                client_id=self.config.mqtt_client_id,
            )
        else:  # paho-mqtt 1.x
            client = mqtt.Client(client_id=self.config.mqtt_client_id)

        if self.config.mqtt_user:
            client.username_pw_set(self.config.mqtt_user, self.config.mqtt_password)
        if self.config.mqtt_tls:
            context = ssl.create_default_context(cafile=self.config.mqtt_ca_file or None)
            if self.config.mqtt_tls_insecure:
                context.check_hostname = False
                context.verify_mode = ssl.CERT_NONE
            client.tls_set_context(context)

        def on_connect(
            client_: Any,
            userdata: Any,
            flags: Any,
            reason_code: Any,
            properties: Any = None,
        ) -> None:
            code = getattr(reason_code, "value", reason_code)
            if code != 0:
                LOG.error("MQTT connection rejected: %s", reason_code)
                return
            LOG.info("MQTT connected; subscribing %s", self.config.mqtt_topic)
            client_.subscribe(self.config.mqtt_topic, qos=self.config.mqtt_qos)
            self.queue.flush(self.poster)

        def on_message(client_: Any, userdata: Any, message: Any) -> None:
            try:
                self.handle_message(message.topic, bytes(message.payload))
            except Exception as exc:
                LOG.warning(
                    "discarding MQTT message topic=%s reason=%s", message.topic, exc
                )

        def on_disconnect(client_: Any, userdata: Any, *args: Any) -> None:
            LOG.warning("MQTT disconnected; paho will retry")

        client.on_connect = on_connect
        client.on_message = on_message
        client.on_disconnect = on_disconnect

        self.start()
        client.connect(self.config.mqtt_host, self.config.mqtt_port, keepalive=30)
        try:
            client.loop_forever(retry_first_connection=True)
        finally:
            self.stop()
            client.disconnect()


def configure_logging() -> None:
    logging.basicConfig(
        level=os.environ.get("SOIL_LOG_LEVEL", "INFO").upper(),
        format="%(asctime)s %(levelname)s %(name)s: %(message)s",
    )


def main() -> int:
    configure_logging()
    try:
        config = GatewayConfig.from_env()
        if not config.default_token and not config.device_tokens:
            raise ValueError("set SOIL_DEVICE_TOKEN or SOIL_DEVICE_TOKENS")
        LOG.info(
            "starting bridge: %s:%d %s -> %s",
            config.mqtt_host,
            config.mqtt_port,
            config.mqtt_topic,
            config.api_url,
        )
        SoilGateway(config).run()
    except KeyboardInterrupt:
        return 0
    except Exception as exc:
        LOG.error("bridge stopped: %s", exc)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
