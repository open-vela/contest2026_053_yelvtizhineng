import json
import tempfile
import threading
import unittest
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

from mqtt_soil_gateway import (
    GatewayConfig,
    JsonlQueue,
    QueueItem,
    SoilGateway,
    TelemetryError,
    TokenResolver,
    device_id_from_topic,
    heartbeat_url,
    normalize_telemetry,
    post_telemetry,
)


SAMPLE = {
    "moisture": 37.4,
    "temp": 24.8,
    "ec": 512,
    "ph": 6.8,
    "salt": 110,
    "nitrogen": 42,
    "phosphorus": 18,
    "potassium": 95,
}


class NormalizeTelemetryTests(unittest.TestCase):
    def test_direct_payload(self):
        self.assertEqual(normalize_telemetry(SAMPLE), SAMPLE)

    def test_aliases_and_nested_wrapper(self):
        payload = {
            "data": {
                "humidity": "37.4",
                "temperature": "24.8",
                "conductivity": 512,
                "soil_ph": 6.8,
                "salinity": 110,
                "n": 42,
                "p": 18,
                "k": 95,
            }
        }
        self.assertEqual(normalize_telemetry(json.dumps(payload)), SAMPLE)

    def test_missing_field_is_rejected(self):
        payload = dict(SAMPLE)
        del payload["ph"]
        with self.assertRaisesRegex(TelemetryError, "missing required field: ph"):
            normalize_telemetry(payload)

    def test_boolean_and_non_finite_are_rejected(self):
        payload = dict(SAMPLE, ph=True)
        with self.assertRaises(TelemetryError):
            normalize_telemetry(payload)
        payload = dict(SAMPLE, ph="NaN")
        with self.assertRaises(TelemetryError):
            normalize_telemetry(payload)


class TopicAndTokenTests(unittest.TestCase):
    def test_device_id_from_default_topic(self):
        self.assertEqual(
            device_id_from_topic("plant/ZXB-SOIL-01/sensor"), "ZXB-SOIL-01"
        )

    def test_token_specific_device_and_wildcard(self):
        tokens = TokenResolver(by_device={"node-a": "token-a", "*": "fallback"})
        self.assertEqual(tokens.get("node-a"), "token-a")
        self.assertEqual(tokens.get("node-b"), "fallback")


class QueueAndGatewayTests(unittest.TestCase):
    def test_gateway_sends_heartbeat_after_delivery(self):
        with tempfile.TemporaryDirectory() as tmp:
            delivered = []
            heartbeats = []
            config = GatewayConfig(
                queue_path=str(Path(tmp) / "queue.jsonl"), default_token="token-1"
            )
            gateway = SoilGateway(
                config,
                poster=lambda token, payload: delivered.append(token),
                heartbeat=lambda token: heartbeats.append(token),
            )
            gateway.handle_message("plant/node-a/sensor", json.dumps(SAMPLE).encode())

            self.assertEqual(delivered, ["token-1"])
            self.assertEqual(heartbeats, ["token-1"])

    def test_gateway_queues_failed_post(self):
        with tempfile.TemporaryDirectory() as tmp:
            calls = []

            def failing_poster(token, payload):
                calls.append((token, payload))
                raise ConnectionError("offline")

            config = GatewayConfig(
                queue_path=str(Path(tmp) / "queue.jsonl"),
                default_token="token-1",
            )
            gateway = SoilGateway(config, poster=failing_poster)
            gateway.handle_message("plant/node-a/sensor", json.dumps(SAMPLE).encode())

            self.assertEqual(len(calls), 1)
            self.assertEqual(len(gateway.queue), 1)
            self.assertTrue((Path(tmp) / "queue.jsonl").exists())

    def test_queue_retries_in_order(self):
        with tempfile.TemporaryDirectory() as tmp:
            queue = JsonlQueue(Path(tmp) / "queue.jsonl", max_items=10)
            queue.append(QueueItem("t1", {"moisture": 1.0}))
            queue.append(QueueItem("t2", {"moisture": 2.0}))
            delivered = []

            def poster(token, payload):
                delivered.append((token, payload["moisture"]))

            self.assertEqual(queue.flush(poster), 2)
            self.assertEqual(delivered, [("t1", 1.0), ("t2", 2.0)])
            self.assertEqual(len(queue), 0)

    def test_queue_keeps_head_when_retry_fails(self):
        with tempfile.TemporaryDirectory() as tmp:
            queue = JsonlQueue(Path(tmp) / "queue.jsonl")
            queue.append(QueueItem("t1", {"moisture": 1.0}))
            queue.append(QueueItem("t2", {"moisture": 2.0}))

            def poster(token, payload):
                raise ConnectionError("still offline")

            self.assertEqual(queue.flush(poster), 0)
            self.assertEqual(len(queue), 2)


class PostTelemetryTests(unittest.TestCase):
    def test_builds_heartbeat_url_from_telemetry_url(self):
        self.assertEqual(
            heartbeat_url("https://api.example.com/api/v1/telemetry/put"),
            "https://api.example.com/api/v1/devices/heartbeat",
        )

    def test_posts_contract_fields_and_device_token(self):
        received = {}

        class Handler(BaseHTTPRequestHandler):
            def do_POST(self):
                length = int(self.headers["Content-Length"])
                received["path"] = self.path
                received["token"] = self.headers.get("X-Device-Token")
                received["body"] = json.loads(self.rfile.read(length))
                self.send_response(200)
                self.end_headers()
                self.wfile.write(b'{"ok":true}')

            def log_message(self, *args):
                pass

        server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        try:
            port = server.server_address[1]
            post_telemetry(f"http://127.0.0.1:{port}/api/v1/telemetry/put", "token-1", SAMPLE)
        finally:
            server.shutdown()
            server.server_close()
            thread.join(timeout=2)

        self.assertEqual(received["path"], "/api/v1/telemetry/put")
        self.assertEqual(received["token"], "token-1")
        self.assertEqual(received["body"], SAMPLE)


if __name__ == "__main__":
    unittest.main()
