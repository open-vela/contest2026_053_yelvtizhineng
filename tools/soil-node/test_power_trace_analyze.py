from __future__ import annotations

import tempfile
import unittest
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import power_trace_analyze as analyzer


class PowerTraceAnalyzeTests(unittest.TestCase):
    def make_args(self, trace: Path, **overrides):
        values = {
            "trace": trace,
            "device_log": None,
            "start_s": None,
            "end_s": None,
            "cycles": 1,
            "cycle_seconds": None,
            "battery_mah": 2000.0,
            "usable_fraction": 0.8,
            "output": None,
        }
        values.update(overrides)
        return type("Args", (), values)()

    def test_duty_cycle_integration(self):
        with tempfile.TemporaryDirectory() as temp:
            trace = Path(temp) / "trace.csv"
            trace.write_text(
                "Time (s),Current (mA),Voltage (V)\n"
                "0,20,3.3\n"
                "10,20,3.3\n"
                "10.1,0.01,3.3\n"
                "60,0.01,3.3\n",
                encoding="utf-8",
            )
            result = analyzer.analyze(self.make_args(trace))

        self.assertAlmostEqual(result["duration_s"], 60.0)
        self.assertAlmostEqual(result["charge_per_cycle_mas"], 201.4995, places=4)
        self.assertAlmostEqual(result["mean_current_ma"], 3.358325, places=6)
        self.assertGreater(result["projected_runtime_days"], 0)
        self.assertIn("energy_per_cycle_j", result)

    def test_clip_and_cycle_split(self):
        with tempfile.TemporaryDirectory() as temp:
            trace = Path(temp) / "trace.csv"
            trace.write_text(
                "time_s,current_a\n"
                "0,1\n"
                "10,1\n"
                "20,1\n"
                "30,1\n",
                encoding="utf-8",
            )
            result = analyzer.analyze(
                self.make_args(trace, start_s=10, end_s=30, cycles=2)
            )

        self.assertEqual(result["duration_s"], 20.0)
        self.assertAlmostEqual(result["charge_per_cycle_mas"], 10000.0)
        self.assertAlmostEqual(result["mean_current_ma"], 1000.0)

    def test_device_log_summary(self):
        with tempfile.TemporaryDirectory() as temp:
            log = Path(temp) / "device.log"
            log.write_text(
                "*** Booting NuttX ***\n"
                "CYCLE_ACTIVE_MS=9000\n"
                "DEEP_SLEEP_US=51000000\n"
                "TLS_READY cert=verified\n"
                "MQTT_PUBLISH=OK\n"
                "SOIL_OK\n",
                encoding="utf-8",
            )
            summary = analyzer.parse_device_log(log)

        self.assertEqual(summary["boots"], 1)
        self.assertEqual(summary["successful_publishes"], 1)
        self.assertEqual(summary["median_active_ms"], 9000)
        self.assertEqual(summary["median_sleep_s"], 51.0)

    def test_time_and_current_units(self):
        with tempfile.TemporaryDirectory() as temp:
            trace = Path(temp) / "trace.csv"
            trace.write_text(
                "elapsed_ms,current_ua\n"
                "0,20000\n"
                "1000,20000\n",
                encoding="utf-8",
            )
            result = analyzer.analyze(self.make_args(trace))

        self.assertAlmostEqual(result["duration_s"], 1.0)
        self.assertAlmostEqual(result["charge_per_cycle_mas"], 20.0)
        self.assertAlmostEqual(result["mean_current_ma"], 20.0)


if __name__ == "__main__":
    unittest.main()
