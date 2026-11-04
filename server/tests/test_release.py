"""Offline integration checks for the packaged source, with isolated state."""
from pathlib import Path
import ast
import json
import os
import shutil
import sqlite3
import sys
import tempfile
import unittest
from unittest.mock import patch

SERVER = Path(__file__).resolve().parents[1]
TEMP = tempfile.TemporaryDirectory(prefix='plant_server_release_')
DATA = Path(TEMP.name) / 'data'
DATA.mkdir()
shutil.copy2(SERVER / 'plant_server/resources/qa_bank.json', DATA / 'qa_bank.json')
for name in list(os.environ):
    if name.startswith(('PLANT_', 'BOX_', 'SOIL_')):
        del os.environ[name]
os.environ.update(PLANT_DATA_DIR=str(DATA), PLANT_DB=str(DATA / 'test.sqlite'),
                  PLANT_AI_MODE='mock', PLANT_MIMO_KEY='offline-test-placeholder',
                  PLANT_TOKEN_SECRET='offline-test-only-not-a-production-secret',
                  PLANT_RATE_LIMIT='0', PLANT_DEMO_OPEN='1',
                  PLANT_DEMO_SMS_ECHO='1', PLANT_DEMO_CODE='123456',
                  BOX_ALLOWED_DEVICE_IDS='d_000000000001',
                  BOX_SOIL_BINDINGS='{}')
sys.path[:0] = [str(SERVER / 'plant_server'), str(SERVER / 'box_ai'), str(SERVER / 'soil_gateway')]
import requests
from fastapi.testclient import TestClient
from app import config, security
from app.main import app
from box_candidate_bank_v1 import CandidateBank


class ReleaseTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.connections = []
        original_connect = sqlite3.connect
        def connect(*args, **kwargs):
            connection = original_connect(*args, **kwargs)
            cls.connections.append(connection)
            return connection
        cls.sqlite_guard = patch.object(sqlite3, 'connect', side_effect=connect)
        cls.sqlite_guard.start()
        cls.network_guard = patch.object(requests.sessions.Session, 'request', side_effect=AssertionError('External HTTP is disabled during release tests'))
        cls.network_guard.start()
        cls.client = TestClient(app)
        cls.client.__enter__()

    @classmethod
    def tearDownClass(cls):
        cls.client.__exit__(None, None, None)
        cls.network_guard.stop()
        for connection in cls.connections:
            try:
                connection.close()
            except sqlite3.ProgrammingError:
                pass  # A short-lived worker may already have closed its connection.
        cls.sqlite_guard.stop()

    def login(self, phone):
        response = self.client.post('/api/v1/auth/sms', json={'phone': phone})
        self.assertEqual(response.status_code, 200)
        response = self.client.post('/api/v1/auth/login', json={'phone': phone, 'code': '123456'})
        self.assertEqual(response.status_code, 200)
        return {'Authorization': 'Bearer ' + response.json()['token']}

    def test_python_source_parses(self):
        files = [p for p in SERVER.rglob('*.py') if not any(part in ('.venv', 'venv', '__pycache__') for part in p.parts)]
        for path in files:
            ast.parse(path.read_text(encoding='utf-8'), filename=str(path))
        self.assertGreater(len(files), 60)

    def test_main_health_and_isolated_database(self):
        self.assertEqual(self.client.get('/api/v1/health').status_code, 200)
        self.assertEqual(Path(config.DB_PATH).parent, DATA)
        self.assertTrue(Path(config.DB_PATH).is_file())

    def test_static_pages_and_learning_resources(self):
        for url in ['/mobile/', '/admin/', '/expo/', '/mobile/app-growth-20261104.js', '/mobile/catalog.js', '/mobile/growth.css']:
            self.assertEqual(self.client.get(url).status_code, 200, url)
        response = self.client.get('/mobile/')
        self.assertIn('app-growth-20261104.js', response.text)
        self.assertEqual(self.client.get('/api/v1/learning/overview').status_code, 401)
        headers = self.login('13800000011')
        response = self.client.get('/api/v1/learning/overview', headers=headers)
        self.assertEqual(response.status_code, 200, response.text)
        self.assertGreater(len(response.json()['entries']), 0)

    def test_main_tokens_enforce_role(self):
        headers = self.login('13800000012')
        claims = security.verify_token(headers['Authorization'].split()[1])
        self.assertEqual(claims['role'], 'user')
        self.assertIsNone(security.verify_token('not-a-valid-token'))

    def test_faq_reconstruction_and_matching(self):
        bank = CandidateBank(SERVER / 'box_ai/qa_bank_v490_20261103.sqlite3')
        self.assertEqual(bank.count, 43406)
        historical = json.loads((SERVER / 'box_ai/qa_bank_v490_20261103.manifest.json').read_text(encoding='utf-8'))
        for sample in historical['samples']:
            entry = bank.match(sample['question'])
            self.assertIsNotNone(entry)
            self.assertEqual(entry['id'], sample['expected_id'])
            self.assertEqual(entry['license'], 'CC-BY-SA-4.0')
            self.assertTrue(entry['references'])
        for sample in historical['negative_tests']:
            self.assertIsNone(bank.match(sample['question']))
        with bank.connect() as connection:
            self.assertEqual(connection.execute('PRAGMA query_only').fetchone()[0], 1)
            with self.assertRaises(sqlite3.OperationalError):
                connection.execute("DELETE FROM entries")

    def test_box_import_auth_and_faq_without_model_calls(self):
        from app.services.ai_gateway import gateway
        with patch.object(gateway, 'mode', 'mimo'):
            import box_ai_service as box
        with TestClient(box.app) as client:
            self.assertEqual(client.get('/api/v1/box/health').json()['qa_candidate_entries'], 43406)
            self.assertEqual(client.post('/api/v1/box/answer', json={'text': '太阳系是怎么形成的？'}).status_code, 401)
            token = security.create_token('d_000000000001', 'device')
            with patch.object(gateway, 'chat_text', side_effect=AssertionError('FAQ must not invoke a model')):
                response = client.post('/api/v1/box/answer', headers={'X-Device-Token': token}, json={'text': '太阳系是怎么形成的？'})
            self.assertEqual(response.status_code, 200, response.text)
            self.assertEqual(response.json()['answer_source'], 'qa_bank')
            self.assertEqual(client.get('/api/v1/box/soil', headers={'X-Device-Token': token}).status_code, 404)
            outsider = security.create_token('d_000000000099', 'device')
            self.assertEqual(client.post('/api/v1/box/answer', headers={'X-Device-Token': outsider}, json={'text': '你好'}).status_code, 403)

    def test_admin_password_must_be_configured(self):
        from app import admin
        with patch.object(admin, 'ADMIN_PASSWORD', ''):
            response = self.client.post('/api/v1/admin/login', json={'password': 'offline-test-only'})
            self.assertEqual(response.status_code, 503)
        with patch.object(admin, 'ADMIN_PASSWORD', 'offline-test-only'):
            response = self.client.post('/api/v1/admin/login', json={'password': 'offline-test-only'})
            self.assertEqual(response.status_code, 200)
        self.client.cookies.clear()

    def test_json_gateway_management_keeps_credentials_private(self):
        from app.routers import mqtt_admin
        headers = {'Authorization': 'Bearer ' + security.create_token('admin', 'admin')}
        private = {'SOIL_MQTT_TOPIC': 'plant/+/sensor', 'SOIL_MQTT_PASS': 'offline-secret-value',
                   'SOIL_DEVICE_TOKENS': '{"test":"offline-device-token"}'}
        with patch.object(mqtt_admin, '_read_env', return_value=private), \
             patch.object(mqtt_admin, '_unit_active', return_value=True), \
             patch.object(mqtt_admin, '_run', return_value=(0, '')) as run:
            self.assertEqual(self.client.get('/api/v1/admin/mqtt/status').status_code, 401)
            response = self.client.get('/api/v1/admin/mqtt/status', headers=headers)
            self.assertEqual(response.status_code, 200, response.text)
            self.assertEqual(response.json()['gateway']['service'], 'plant-soil-json-gw.service')
            self.assertNotIn('offline-secret-value', response.text)
            self.assertNotIn('offline-device-token', response.text)
            self.assertEqual(set(response.json()['gateway']['env']), set(mqtt_admin.SAFE_GW_KEYS))
            response = self.client.post('/api/v1/admin/mqtt/gateway', headers=headers, json={'action': 'save'})
            self.assertEqual(response.status_code, 422)
            response = self.client.post('/api/v1/admin/mqtt/gateway', headers=headers, json={'action': 'restart'})
            self.assertEqual(response.status_code, 200)
            run.assert_called_with(['systemctl', 'restart', 'plant-soil-json-gw.service'])
        with patch.object(mqtt_admin, '_run', return_value=(1, 'offline-secret-value')):
            response = self.client.post('/api/v1/admin/mqtt/gateway', headers=headers, json={'action': 'restart'})
            self.assertEqual(response.status_code, 503)
            self.assertNotIn('offline-secret-value', response.text)

    def test_esp32_soil_device_registers_before_binding(self):
        headers = self.login('13800000013')
        response = self.client.post('/api/v1/devices/register',
                                    json={'sn': 'SOIL-RELEASE-TEST-01', 'model': 'soil-wifi', 'fw_version': 'soil-node'})
        self.assertEqual(response.status_code, 200)
        token = response.json()['device_token']
        device_id = response.json()['device']['id']
        self.assertEqual(security.verify_token(token)['role'], 'device')
        response = self.client.post('/api/v1/devices/bind', headers=headers,
                                    json={'sn': 'SOIL-RELEASE-TEST-01', 'model': 'soil-wifi'})
        self.assertEqual(response.status_code, 200, response.text)
        self.assertEqual(response.json()['kind'], 'soil-wifi')
        other = self.client.post('/api/v1/devices/register',
                                  json={'sn': 'SOIL-OTHER-TEST-01', 'model': 'soil-wifi'})
        self.assertEqual(other.status_code, 200)
        other_id = other.json()['device']['id']
        response = self.client.post('/api/v1/telemetry/put', headers={'X-Device-Token': token},
                                    json={'moisture': 35, 'temp': 23, 'ph': 6.8})
        self.assertEqual(response.status_code, 200, response.text)
        self.assertEqual(response.json()['saved'], 1)
        from app import db
        self.assertIsNotNone(db.one('SELECT last_seen_at FROM devices WHERE id=?', (device_id,))['last_seen_at'])
        self.assertIsNone(db.one('SELECT last_seen_at FROM devices WHERE id=?', (other_id,))['last_seen_at'])
        self.assertEqual(self.client.get('/api/v1/telemetry/soil/now').status_code, 404)
        response = self.client.post('/api/v1/devices/bind', headers=headers,
                                    json={'sn': 'SOIL-UNREGISTERED-01', 'model': 'soil-wifi'})
        self.assertEqual(response.status_code, 404)


if __name__ == '__main__':
    outcome = unittest.main(verbosity=2, exit=False)
    TEMP.cleanup()
    sys.exit(0 if outcome.result.wasSuccessful() else 1)
