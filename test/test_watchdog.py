import importlib.util
import tempfile
import threading
import time
import unittest
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path
from unittest.mock import patch


SERVER_PATH = Path(__file__).resolve().parents[1] / "src" / "server.py"
SPEC = importlib.util.spec_from_file_location("cason_server", SERVER_PATH)
server = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(server)


class FakeClock:
    def __init__(self, start):
        self.current = float(start)

    def time(self):
        return self.current

    def advance(self, seconds):
        self.current += seconds


class WatchdogTests(unittest.TestCase):
    def setUp(self):
        self.tmpdir = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmpdir.cleanup)

        self.clock = FakeClock(1401)
        self.messages = []
        self.push_lock = threading.Lock()

        self.patchers = [
            patch.object(
                server,
                "HEARTBEAT_FILE",
                Path(self.tmpdir.name) / "heartbeat_status.json",
            ),
            patch.object(server, "SERVER_START_TIME", 1),
            patch.object(server, "SERVER_START_TIME_TEXT", "1970-01-01 00:00:01"),
            patch.object(server, "HEARTBEAT_TIMEOUT_SECONDS", 300),
            patch.object(server, "WATCHDOG_ALERT_COOLDOWN_SECONDS", 1800),
            patch.object(server, "WATCHDOG_RETRY_BASE_SECONDS", 60),
            patch.object(server, "WATCHDOG_MAX_RETRIES", 3),
            patch.object(server, "now_seconds", self.clock.time),
            patch.dict(server.os.environ, {"CASON_MAINTENANCE_UNTIL": ""}, clear=False),
        ]

        for patcher in self.patchers:
            patcher.start()
            self.addCleanup(patcher.stop)

    def write_online_status(self):
        server.save_heartbeat_status({
            "devices": {
                "CASON-0001": {
                    "device": "CASON-0001",
                    "state": "ONLINE",
                    "last_seen": 1000,
                    "last_seen_text": "1970-01-01 00:16:40",
                    "last_source": "heartbeat",
                }
            }
        })

    def fake_push_ok(self, message):
        with self.push_lock:
            self.messages.append(message)
        return True, {"sent": 1, "failed": 0, "details": []}

    def fake_push_fail(self, message):
        with self.push_lock:
            self.messages.append(message)
        return False, {"sent": 0, "failed": 1, "details": []}

    def test_offline_alert_is_sent_once_for_one_offline_episode(self):
        self.write_online_status()

        with patch.object(server, "push_to_all_users", self.fake_push_ok):
            server.check_heartbeat_watchdog()
            server.check_heartbeat_watchdog()

        self.assertEqual(len(self.messages), 1)
        status = server.public_device_status()
        self.assertEqual(status["state"], "OFFLINE")
        self.assertTrue(status["last_offline_line_sent"])

    def test_parallel_watchdog_checks_do_not_send_duplicate_offline_alerts(self):
        self.write_online_status()

        def slow_push(message):
            time.sleep(0.05)
            return self.fake_push_ok(message)

        with patch.object(server, "push_to_all_users", slow_push):
            with ThreadPoolExecutor(max_workers=8) as executor:
                futures = [
                    executor.submit(server.check_heartbeat_watchdog)
                    for _ in range(8)
                ]
                for future in futures:
                    future.result()

        self.assertEqual(len(self.messages), 1)

    def test_recovery_alert_is_sent_once_when_device_returns_online(self):
        self.write_online_status()

        with patch.object(server, "push_to_all_users", self.fake_push_ok):
            server.check_heartbeat_watchdog()
            self.clock.advance(10)
            server.mark_device_seen("CASON-0001", "heartbeat", {})
            server.mark_device_seen("CASON-0001", "heartbeat", {})

        self.assertEqual(len(self.messages), 2)
        self.assertIn("ไม่ตอบสนอง", self.messages[0])
        self.assertIn("กลับมาออนไลน์", self.messages[1])

    def test_failed_offline_alert_retries_are_limited(self):
        self.write_online_status()

        with patch.object(server, "push_to_all_users", self.fake_push_fail):
            server.check_heartbeat_watchdog()
            for _ in range(10):
                self.clock.advance(3600)
                server.check_heartbeat_watchdog()

        self.assertEqual(len(self.messages), 1 + server.WATCHDOG_MAX_RETRIES)
        status = server.public_device_status()
        self.assertEqual(
            status["offline_retry_count"],
            server.WATCHDOG_MAX_RETRIES,
        )

    def test_maintenance_mode_suppresses_offline_alert(self):
        self.write_online_status()
        server.set_maintenance_mode("CASON-0001", 3, "test")

        with patch.object(server, "push_to_all_users", self.fake_push_ok):
            server.check_heartbeat_watchdog()

        self.assertEqual(self.messages, [])
        self.assertEqual(server.public_device_status()["state"], "MAINTENANCE")


if __name__ == "__main__":
    unittest.main()
