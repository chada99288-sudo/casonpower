#!/usr/bin/env python3
# -*- coding: utf-8 -*-

import base64
import hashlib
import hmac
import json
import os
import random
import threading
import time
from urllib.parse import parse_qs, urlparse
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

import requests
from dotenv import load_dotenv

load_dotenv()

HOST = os.getenv("CASON_SERVER_HOST", "0.0.0.0")
PORT = int(os.getenv("PORT") or os.getenv("CASON_SERVER_PORT", "8080"))

LINE_TOKEN = os.getenv("LINE_CHANNEL_ACCESS_TOKEN", "").strip()
LINE_CHANNEL_SECRET = os.getenv("LINE_CHANNEL_SECRET", "").strip()

LINE_PUSH_URL = "https://api.line.me/v2/bot/message/push"

CASON_CHAOS_MODE = os.getenv("CASON_CHAOS_MODE", "0") == "1"
CASON_CHAOS_DELAY_SECONDS = float(
    os.getenv("CASON_CHAOS_DELAY_SECONDS", "0") or "0"
)
CASON_CHAOS_HTTP_500_RATE = float(
    os.getenv("CASON_CHAOS_HTTP_500_RATE", "0") or "0"
)
CASON_CHAOS_BAD_JSON_RATE = float(
    os.getenv("CASON_CHAOS_BAD_JSON_RATE", "0") or "0"
)
CASON_CHAOS_DROP_LINE_RATE = float(
    os.getenv("CASON_CHAOS_DROP_LINE_RATE", "0") or "0"
)

HEARTBEAT_TIMEOUT_SECONDS = int(
    os.getenv("CASON_HEARTBEAT_TIMEOUT_SECONDS", "180")
)
HEARTBEAT_REPEAT_ALERT_SECONDS = int(
    os.getenv("CASON_HEARTBEAT_REPEAT_ALERT_SECONDS", "1800")
)
HEARTBEAT_CHECK_INTERVAL_SECONDS = int(
    os.getenv("CASON_HEARTBEAT_CHECK_INTERVAL_SECONDS", "30")
)

DUPLICATE_BLOCK_SECONDS = int(
    os.getenv("CASON_DUPLICATE_BLOCK_SECONDS", "300")
)

ALWAYS_SEND_EVENTS = {
    "BOOT",
    "FAULT",
    "ALARM",
    "RECOVERY",
    "RESET",
    "TEST",
}

ALLOWED_ESP32_PATHS = {
    "/",
    "/alert",
    "/event",
    "/api/alert",
    "/api/event",
}

PROJECT_ROOT = Path(__file__).resolve().parent.parent
USER_FILE = PROJECT_ROOT / "line_users.json"
COMMAND_FILE = PROJECT_ROOT / "command_queue.json"
COMMAND_RESULT_FILE = PROJECT_ROOT / "command_results.json"
COMMAND_INFLIGHT_FILE = PROJECT_ROOT / "command_inflight.json"
HEARTBEAT_FILE = PROJECT_ROOT / "heartbeat_status.json"
ALLOWED_LINE_COMMANDS = {
    "STATUS",
    "TEST",
    "ON",
    "OFF",
    "RESET",
    "CHECK",
    "WIFI_RESET",
}
_last_sent = {}


def clamp_rate(value):
    return max(0.0, min(1.0, float(value)))


def chaos_hit(rate):
    return CASON_CHAOS_MODE and random.random() < clamp_rate(rate)


def maybe_apply_chaos(handler, path):
    if not CASON_CHAOS_MODE:
        return False

    if CASON_CHAOS_DELAY_SECONDS > 0:
        print(
            f"[CHAOS] delay {CASON_CHAOS_DELAY_SECONDS}s path={path}"
        )
        time.sleep(CASON_CHAOS_DELAY_SECONDS)

    if chaos_hit(CASON_CHAOS_HTTP_500_RATE):
        print(f"[CHAOS] forced HTTP 503 path={path}")
        send_json(
            handler,
            503,
            {
                "ok": False,
                "error": "chaos_forced_http_503",
            }
        )
        return True

    if chaos_hit(CASON_CHAOS_BAD_JSON_RATE):
        print(f"[CHAOS] forced bad JSON path={path}")
        body = b"{bad-json"
        handler.send_response(200)
        handler.send_header(
            "Content-Type",
            "application/json; charset=utf-8"
        )
        handler.send_header("Content-Length", str(len(body)))
        handler.send_header("Connection", "close")
        handler.end_headers()
        handler.wfile.write(body)
        return True

    return False


def send_json(handler, code, payload):
    body = json.dumps(
        payload,
        ensure_ascii=False
    ).encode("utf-8")

    handler.send_response(code)
    handler.send_header(
        "Content-Type",
        "application/json; charset=utf-8"
    )
    handler.send_header(
        "Content-Length",
        str(len(body))
    )
    handler.send_header("Connection", "close")
    handler.end_headers()
    handler.wfile.write(body)


def load_users():
    if not USER_FILE.exists():
        return []

    try:
        data = json.loads(
            USER_FILE.read_text(encoding="utf-8")
        )

        if isinstance(data, list):
            return [
                item for item in data
                if isinstance(item, str) and item.startswith("U")
            ]

    except Exception as exc:
        print(f"[USER] อ่านไฟล์ไม่สำเร็จ: {exc}")

    return []


def save_user(user_id):
    if not user_id or not user_id.startswith("U"):
        return False

    users = load_users()

    if user_id in users:
        return False

    users.append(user_id)

    USER_FILE.write_text(
        json.dumps(
            users,
            ensure_ascii=False,
            indent=2
        ),
        encoding="utf-8"
    )

    print(f"[USER] บันทึก LINE userId แล้ว: {user_id}")
    return True



def load_command_queue():
    if not COMMAND_FILE.exists():
        return []

    try:
        data = json.loads(
            COMMAND_FILE.read_text(encoding="utf-8")
        )
        if isinstance(data, list):
            return [item for item in data if isinstance(item, dict)]
    except Exception as exc:
        print(f"[COMMAND] อ่านคิวไม่สำเร็จ: {exc}")

    return []


def save_command_queue(queue):
    COMMAND_FILE.write_text(
        json.dumps(
            queue,
            ensure_ascii=False,
            indent=2
        ),
        encoding="utf-8"
    )


def load_command_results():
    if not COMMAND_RESULT_FILE.exists():
        return []

    try:
        data = json.loads(
            COMMAND_RESULT_FILE.read_text(encoding="utf-8")
        )
        if isinstance(data, list):
            return [item for item in data if isinstance(item, dict)]
    except Exception as exc:
        print(f"[COMMAND] อ่านผลคำสั่งไม่สำเร็จ: {exc}")

    return []


def save_command_result(result):
    results = load_command_results()
    results.append(result)
    results = results[-100:]
    COMMAND_RESULT_FILE.write_text(
        json.dumps(
            results,
            ensure_ascii=False,
            indent=2
        ),
        encoding="utf-8"
    )


def load_inflight_commands():
    if not COMMAND_INFLIGHT_FILE.exists():
        return {}

    try:
        data = json.loads(
            COMMAND_INFLIGHT_FILE.read_text(encoding="utf-8")
        )
        if isinstance(data, dict):
            return {
                str(key): value for key, value in data.items()
                if isinstance(value, dict)
            }
    except Exception as exc:
        print(f"[COMMAND] อ่านคำสั่งที่กำลังทำงานไม่สำเร็จ: {exc}")

    return {}


def save_inflight_commands(commands):
    COMMAND_INFLIGHT_FILE.write_text(
        json.dumps(
            commands,
            ensure_ascii=False,
            indent=2
        ),
        encoding="utf-8"
    )


def remember_inflight_command(command):
    command_id = str(command.get("id", ""))

    if not command_id:
        return

    commands = load_inflight_commands()
    commands[command_id] = command

    # เก็บเฉพาะคำสั่งล่าสุด ป้องกันไฟล์โตถ้า ESP32 ไม่ส่งผลกลับ
    items = sorted(
        commands.items(),
        key=lambda item: int(item[1].get("created_at", 0))
    )[-50:]
    save_inflight_commands(dict(items))


def pop_inflight_command(command_id):
    command_id = str(command_id or "")

    if not command_id:
        return {}

    commands = load_inflight_commands()
    command = commands.pop(command_id, {})
    save_inflight_commands(commands)
    return command


def build_command_result_message(command, ok, detail):
    command = str(command or "").upper()
    detail = str(detail or "")

    status_text = "สำเร็จ" if ok else "ไม่สำเร็จ"
    action_text = {
        "STATUS": "ดูสถานะระบบ",
        "TEST": "ทดสอบระบบ",
        "ON": "เปิด Relay CH1",
        "OFF": "ปิด Relay CH1",
        "RESET": "รีเซ็ต Alarm",
        "CHECK": "ตรวจระบบทั้งหมด",
        "WIFI_RESET": "ล้างค่า Wi-Fi",
    }.get(command, command or "ไม่ทราบคำสั่ง")

    lines = [
        "ผลการทำงานจาก ESP32",
        f"คำสั่ง: {action_text}",
        f"สถานะ: {status_text}",
    ]

    if detail and detail not in {"executed", "refused_or_failed"}:
        lines.append(f"รายละเอียด: {detail}")

    return "\n".join(lines)


def is_authorized_command_user(user_id):
    return bool(user_id) and user_id in set(load_users())


def normalize_line_command(text):
    command = str(text or "").strip().upper()

    aliases = {
        "เปิด": "ON",
        "เปิดระบบ": "ON",
        "ปิด": "OFF",
        "ปิดระบบ": "OFF",
        "สถานะ": "STATUS",
        "ดูสถานะ": "STATUS",
        "รีเซ็ต": "RESET",
        "ทดสอบ": "TEST",
        "เช็ค": "TEST",
        "ตรวจระบบ": "CHECK",
        "ตรวจสอบระบบ": "CHECK",
        "เช็คระบบ": "CHECK",
        "ล้างไวไฟ": "WIFI_RESET",
        "ล้าง WiFi": "WIFI_RESET",
        "ล้าง WIFI": "WIFI_RESET",
        "RESET_WIFI": "WIFI_RESET",
        "WIFI_RESET": "WIFI_RESET",
    }

    command = aliases.get(command, command)

    if command in ALLOWED_LINE_COMMANDS:
        return command

    return ""


def enqueue_line_command(user_id, command):
    queue = load_command_queue()
    command_id = str(int(time.time() * 1000))

    queue.append({
        "id": command_id,
        "command": command,
        "user_id": user_id,
        "created_at": int(time.time()),
    })

    queue = queue[-20:]
    save_command_queue(queue)

    print(
        f"[COMMAND] queued id={command_id} "
        f"command={command} user={user_id}"
    )
    return command_id


def pop_next_command():
    queue = load_command_queue()

    if not queue:
        return None

    command = queue.pop(0)
    save_command_queue(queue)
    remember_inflight_command(command)
    return command


def verify_line_signature(raw_body, received_signature):
    if not LINE_CHANNEL_SECRET:
        print("[WEBHOOK] ไม่พบ LINE_CHANNEL_SECRET")
        return False

    if not received_signature:
        print("[WEBHOOK] ไม่พบ X-Line-Signature")
        return False

    digest = hmac.new(
        LINE_CHANNEL_SECRET.encode("utf-8"),
        raw_body,
        hashlib.sha256
    ).digest()

    expected_signature = base64.b64encode(
        digest
    ).decode("utf-8")

    return hmac.compare_digest(
        expected_signature,
        received_signature
    )


def signature(data):
    fields = {
        "device": data.get("device"),
        "event": data.get("event") or data.get("type"),
        "status": data.get("status"),
        "source": data.get("source"),
        "message": data.get("message"),
        "relay1": data.get("relay1"),
        "alarm_active": data.get("alarm_active"),
    }

    return json.dumps(
        fields,
        ensure_ascii=False,
        sort_keys=True
    )


def build_line_message(data):
    event = str(
        data.get("event")
        or data.get("type")
        or "UNKNOWN"
    ).upper()

    status = str(
        data.get("status")
        or "UNKNOWN"
    ).upper()

    device = str(
        data.get("device")
        or "CASON-ESP32"
    )

    controller = str(
        data.get("controller")
        or "Cason Solar Safety Controller"
    )

    source = str(
        data.get("source")
        or "-"
    )

    message = str(
        data.get("message")
        or "ไม่มีรายละเอียด"
    )

    relay1 = str(
        data.get("relay1")
        or "-"
    )

    di1_raw = data.get("di1_raw", "-")

    alarm = (
        "YES"
        if bool(data.get("alarm_active"))
        else "NO"
    )

    if event in {"FAULT", "ALARM", "TRIP"}:
        title = "🚨 CASON SAFETY ALERT"

    elif event in {"RECOVERY", "RESET", "NORMAL"}:
        title = "✅ CASON SYSTEM RECOVERY"

    elif event == "TEST":
        title = "🧪 CASON CONNECTION TEST"

    else:
        title = "ℹ️ CASON SYSTEM EVENT"

    text = "\n".join([
        title,
        f"ระบบ: {controller}",
        f"อุปกรณ์: {device}",
        f"เหตุการณ์: {event}",
        f"สถานะ: {status}",
        f"แหล่งสัญญาณ: {source}",
        f"DI1 RAW: {di1_raw}",
        f"Relay CH1: {relay1}",
        f"Alarm Lock: {alarm}",
        "",
        message,
    ])

    return text[:5000]


def push_line(user_id, text):
    if chaos_hit(CASON_CHAOS_DROP_LINE_RATE):
        print(f"[CHAOS] simulated LINE drop user={user_id}")
        return False, 0, "chaos_simulated_line_drop"

    if not LINE_TOKEN:
        return False, 0, (
            "ไม่พบ LINE_CHANNEL_ACCESS_TOKEN ในไฟล์ .env"
        )

    headers = {
        "Authorization": f"Bearer {LINE_TOKEN}",
        "Content-Type": "application/json",
    }

    payload = {
        "to": user_id,
        "messages": [
            {
                "type": "text",
                "text": text
            }
        ]
    }

    try:
        response = requests.post(
            LINE_PUSH_URL,
            headers=headers,
            json=payload,
            timeout=15
        )

    except requests.RequestException as exc:
        return False, 0, str(exc)

    return (
        response.status_code == 200,
        response.status_code,
        response.text
    )


def push_to_all_users(text):
    users = load_users()

    if not users:
        return False, {
            "sent": 0,
            "failed": 0,
            "error": (
                "ยังไม่มี LINE userId "
                "กรุณาเพิ่มเพื่อนแล้วส่งข้อความหา Bot ก่อน"
            )
        }

    sent = 0
    failed = 0
    details = []

    for user_id in users:
        ok, code, response_text = push_line(
            user_id,
            text
        )

        if ok:
            sent += 1
            print(
                f"[LINE] Push สำเร็จ user={user_id}"
            )

        else:
            failed += 1
            print(
                f"[LINE] Push ไม่สำเร็จ "
                f"user={user_id} HTTP={code}"
            )
            print(
                f"[LINE] Response={response_text}"
            )

        details.append({
            "user_id": user_id,
            "ok": ok,
            "http_code": code,
        })

    return sent > 0, {
        "sent": sent,
        "failed": failed,
        "details": details,
    }


def load_heartbeat_status():
    if not HEARTBEAT_FILE.exists():
        return {}

    try:
        data = json.loads(
            HEARTBEAT_FILE.read_text(encoding="utf-8")
        )
        if isinstance(data, dict):
            return data
    except Exception as exc:
        print(f"[WATCHDOG] อ่านสถานะ heartbeat ไม่สำเร็จ: {exc}")

    return {}


def save_heartbeat_status(status):
    HEARTBEAT_FILE.write_text(
        json.dumps(
            status,
            ensure_ascii=False,
            indent=2
        ),
        encoding="utf-8"
    )


def heartbeat_age_seconds(status=None):
    status = status or load_heartbeat_status()
    last_seen = float(status.get("last_seen", 0) or 0)
    if last_seen <= 0:
        return None
    return max(0, int(time.time() - last_seen))


def mark_device_seen(device="CASON-ESP32-01", source="unknown", data=None):
    now = time.time()
    status = load_heartbeat_status()
    was_offline = bool(status.get("offline_notified"))

    status.update({
        "device": device or "CASON-ESP32-01",
        "last_seen": now,
        "last_seen_text": time.strftime(
            "%Y-%m-%d %H:%M:%S",
            time.localtime(now)
        ),
        "last_source": source,
        "offline_notified": False,
    })

    if isinstance(data, dict):
        status["last_payload"] = {
            key: data.get(key)
            for key in (
                "di1_raw",
                "di1_active",
                "relay1",
                "relay1_on",
                "alarm_active",
                "uptime_seconds",
                "free_heap",
                "wifi",
                "ip",
                "esp32_ip",
            )
            if key in data
        }

    if was_offline:
        print("[WATCHDOG] ESP32 กลับมาออนไลน์แล้ว")
        ok, result = push_to_all_users(
            "CASON Watchdog\n"
            "ESP32 กลับมาออนไลน์แล้ว\n"
            f"Device: {status['device']}\n"
            f"Source: {source}"
        )
        status.update({
            "last_online_alert": now,
            "last_online_alert_text": time.strftime(
                "%Y-%m-%d %H:%M:%S",
                time.localtime(now)
            ),
            "last_online_line_sent": bool(ok),
            "last_online_line_result": result,
        })

    save_heartbeat_status(status)
    return status


def check_heartbeat_watchdog():
    status = load_heartbeat_status()
    last_seen = float(status.get("last_seen", 0) or 0)

    if last_seen <= 0:
        return

    now = time.time()
    age = int(now - last_seen)

    if age < HEARTBEAT_TIMEOUT_SECONDS:
        return

    last_alert = float(status.get("last_offline_alert", 0) or 0)
    already_notified = bool(status.get("offline_notified"))

    if already_notified and now - last_alert < HEARTBEAT_REPEAT_ALERT_SECONDS:
        return

    status["offline_notified"] = True
    status["last_offline_alert"] = now
    status["last_offline_alert_text"] = time.strftime(
        "%Y-%m-%d %H:%M:%S",
        time.localtime(now)
    )
    status["last_offline_age_seconds"] = age

    print(f"[WATCHDOG] ESP32 ไม่ตอบสนอง age={age}s")
    ok, result = push_to_all_users(
        "CASON Watchdog\n"
        "ESP32 ไม่ตอบสนอง\n"
        f"ไม่ได้รับ heartbeat {age} วินาที\n"
        "อาจเกิดไฟดับ, เครื่องค้าง, Wi-Fi หลุด หรืออินเทอร์เน็ตมีปัญหา"
    )
    status["last_offline_line_sent"] = bool(ok)
    status["last_offline_line_result"] = result
    save_heartbeat_status(status)


def watchdog_loop():
    while True:
        try:
            check_heartbeat_watchdog()
        except Exception as exc:
            print(f"[WATCHDOG] ตรวจสอบผิดพลาด: {exc}")
        time.sleep(max(5, HEARTBEAT_CHECK_INTERVAL_SECONDS))


class Handler(BaseHTTPRequestHandler):
    server_version = "CasonLineServer/3.0"

    def log_message(self, fmt, *args):
        print("HTTP: " + (fmt % args))

    def do_GET(self):
        parsed = urlparse(self.path)
        path = parsed.path

        if maybe_apply_chaos(self, path):
            return

        check_heartbeat_watchdog()

        if path == "/api/command":
            self.handle_command_poll()
            return

        if path not in {"/", "/health"}:
            send_json(
                self,
                404,
                {
                    "ok": False,
                    "error": "not_found"
                }
            )
            return

        send_json(
            self,
            200,
            {
                "ok": True,
                "service": (
                    "CASON ESP32 LINE COMMAND SERVER V4"
                ),
                "port": PORT,
                "line_token_configured": bool(
                    LINE_TOKEN
                ),
                "line_secret_configured": bool(
                    LINE_CHANNEL_SECRET
                ),
                "registered_users": len(
                    load_users()
                ),
                "queued_commands": len(
                    load_command_queue()
                ),
                "chaos_mode": CASON_CHAOS_MODE,
                "heartbeat_timeout_seconds": HEARTBEAT_TIMEOUT_SECONDS,
                "esp32_last_seen_age_seconds": heartbeat_age_seconds(),
                "esp32_offline_notified": bool(
                    load_heartbeat_status().get("offline_notified")
                ),
                "watchdog": {
                    key: load_heartbeat_status().get(key)
                    for key in (
                        "device",
                        "last_seen_text",
                        "last_source",
                        "last_offline_alert_text",
                        "last_offline_age_seconds",
                        "last_offline_line_sent",
                        "last_offline_line_result",
                        "last_online_alert_text",
                        "last_online_line_sent",
                        "last_online_line_result",
                    )
                    if key in load_heartbeat_status()
                },
            }
        )

    def do_POST(self):
        path = self.path.split("?", 1)[0]

        if maybe_apply_chaos(self, path):
            return

        check_heartbeat_watchdog()

        if path == "/webhook":
            self.handle_line_webhook()
            return

        if path == "/api/command/result":
            self.handle_command_result()
            return

        if path == "/api/heartbeat":
            self.handle_esp32_heartbeat()
            return

        if path in ALLOWED_ESP32_PATHS:
            self.handle_esp32_event()
            return

        send_json(
            self,
            404,
            {
                "ok": False,
                "error": "unsupported_path",
                "allowed_paths": sorted(
                    ALLOWED_ESP32_PATHS | {"/webhook", "/api/command", "/api/command/result", "/api/heartbeat"}
                ),
            }
        )

    def read_raw_body(self):
        try:
            length = int(
                self.headers.get(
                    "Content-Length",
                    "0"
                )
            )
        except ValueError:
            return None, "invalid_content_length"

        if length < 0 or length > 64000:
            return None, "invalid_content_length"

        return self.rfile.read(length), None

    def handle_line_webhook(self):
        raw_body, error = self.read_raw_body()

        if error:
            send_json(
                self,
                400,
                {
                    "ok": False,
                    "error": error
                }
            )
            return

        received_signature = self.headers.get(
            "X-Line-Signature",
            ""
        )

        if not verify_line_signature(
            raw_body,
            received_signature
        ):
            send_json(
                self,
                401,
                {
                    "ok": False,
                    "error": "invalid_line_signature"
                }
            )
            return

        try:
            payload = json.loads(
                raw_body.decode("utf-8")
            )
        except Exception as exc:
            send_json(
                self,
                400,
                {
                    "ok": False,
                    "error": "invalid_json",
                    "detail": str(exc)
                }
            )
            return

        events = payload.get("events", [])

        print("=" * 60)
        print("[WEBHOOK] รับข้อมูลจาก LINE")
        print(
            json.dumps(
                payload,
                ensure_ascii=False,
                indent=2
            )
        )

        saved_count = 0

        for event in events:
            source = event.get("source", {})
            user_id = source.get("userId")

            if user_id and save_user(user_id):
                saved_count += 1

            event_type = event.get("type")

            if event_type == "follow" and user_id:
                push_line(
                    user_id,
                    (
                        "เชื่อมต่อกับ "
                        "CASON Solar Safety Controller "
                        "เรียบร้อยแล้ว ✅"
                    )
                )

            elif event_type == "message" and user_id:
                message = event.get(
                    "message",
                    {}
                )

                if message.get("type") == "text":
                    raw_text = message.get("text", "")
                    command = normalize_line_command(raw_text)

                    if not is_authorized_command_user(user_id):
                        push_line(
                            user_id,
                            "ไม่ได้รับอนุญาตให้สั่งงานระบบนี้"
                        )

                    elif command:
                        command_id = enqueue_line_command(
                            user_id,
                            command
                        )
                        push_line(
                            user_id,
                            (
                                f"รับคำสั่ง {command} แล้ว\n"
                                f"เลขคำสั่ง: {command_id}\n"
                                "กำลังรอ ESP32 ทำงานและส่งผลกลับ"
                            )
                        )

                    else:
                        push_line(
                            user_id,
                            (
                                "คำสั่งที่ใช้ได้:\n"
                                "STATUS - ดูสถานะ\n"
                                "TEST - ตรวจสอบ LINE\n"
                                "ON - เปิด Relay CH1\n"
                                "OFF - ปิด Relay CH1\n"
                                "RESET - รีเซ็ต alarm\n"
                                "CHECK - ตรวจระบบทั้งหมด\n"
                                "WIFI_RESET - ล้างค่า Wi-Fi"
                            )
                        )

        send_json(
            self,
            200,
            {
                "ok": True,
                "events_received": len(events),
                "users_saved": saved_count,
            }
        )

    def handle_command_poll(self):
        parsed = urlparse(self.path)
        query = parse_qs(parsed.query)
        device = (query.get("device") or ["CASON-ESP32-01"])[0]
        mark_device_seen(device, "command_poll")

        command = pop_next_command()

        if not command:
            send_json(
                self,
                200,
                {
                    "ok": True,
                    "command": "",
                }
            )
            return

        print(
            f"[COMMAND] ESP32 picked "
            f"id={command.get('id')} "
            f"command={command.get('command')}"
        )

        send_json(
            self,
            200,
            {
                "ok": True,
                "id": command.get("id", ""),
                "command": command.get("command", ""),
                "created_at": command.get("created_at", 0),
            }
        )

    def handle_command_result(self):
        raw_body, error = self.read_raw_body()

        if error or not raw_body:
            send_json(
                self,
                400,
                {
                    "ok": False,
                    "error": error or "empty_body"
                }
            )
            return

        try:
            data = json.loads(raw_body.decode("utf-8"))
        except Exception as exc:
            send_json(
                self,
                400,
                {
                    "ok": False,
                    "error": "invalid_json",
                    "detail": str(exc)
                }
            )
            return

        data["received_at"] = int(time.time())
        save_command_result(data)

        command_id = str(data.get("id", ""))
        inflight_command = pop_inflight_command(command_id)
        user_id = inflight_command.get("user_id", "")

        print(
            f"[COMMAND] result id={data.get('id')} "
            f"command={data.get('command')} "
            f"ok={data.get('ok')}"
        )

        # ESP32 ส่งข้อความผลลัพธ์จริงเข้า LINE เองผ่าน LINE Direct
        # จึงไม่ push ผลซ้ำจาก server ตรงนี้ เพื่อไม่ให้ LINE แสดงข้อความซ้ำกัน
        if user_id:
            print(
                f"[COMMAND] result notification suppressed "
                f"to avoid duplicate LINE message user={user_id}"
            )

        send_json(
            self,
            200,
            {
                "ok": True,
                "accepted": True,
            }
        )

    def handle_esp32_heartbeat(self):
        raw_body, error = self.read_raw_body()

        if error or not raw_body:
            send_json(
                self,
                400,
                {
                    "ok": False,
                    "error": error or "empty_body"
                }
            )
            return

        try:
            data = json.loads(raw_body.decode("utf-8"))
        except Exception as exc:
            send_json(
                self,
                400,
                {
                    "ok": False,
                    "error": "invalid_json",
                    "detail": str(exc)
                }
            )
            return

        if not isinstance(data, dict):
            send_json(
                self,
                400,
                {
                    "ok": False,
                    "error": "json_must_be_object"
                }
            )
            return

        device = str(data.get("device") or "CASON-ESP32-01")
        status = mark_device_seen(device, "heartbeat", data)

        send_json(
            self,
            200,
            {
                "ok": True,
                "accepted": True,
                "device": status.get("device"),
                "timeout_seconds": HEARTBEAT_TIMEOUT_SECONDS,
            }
        )

    def handle_esp32_event(self):
        raw_body, error = self.read_raw_body()

        if error or not raw_body:
            send_json(
                self,
                400,
                {
                    "ok": False,
                    "error": (
                        error
                        or "empty_body"
                    )
                }
            )
            return

        try:
            data = json.loads(
                raw_body.decode("utf-8")
            )
        except Exception as exc:
            send_json(
                self,
                400,
                {
                    "ok": False,
                    "error": "invalid_json",
                    "detail": str(exc)
                }
            )
            return

        if not isinstance(data, dict):
            send_json(
                self,
                400,
                {
                    "ok": False,
                    "error": "json_must_be_object"
                }
            )
            return

        mark_device_seen(
            str(data.get("device") or "CASON-ESP32-01"),
            "esp32_event",
            data
        )

        print("=" * 60)
        print(
            f"[ESP32] Path={self.path}"
        )
        print(
            json.dumps(
                data,
                ensure_ascii=False,
                indent=2
            )
        )

        event_name = str(
            data.get("event")
            or data.get("type")
            or "UNKNOWN"
        ).upper()

        sig = signature(data)
        now = time.time()
        previous = _last_sent.get(sig)

        if (
            event_name not in ALWAYS_SEND_EVENTS
            and previous is not None
            and now - previous
            < DUPLICATE_BLOCK_SECONDS
        ):
            remaining = int(
                DUPLICATE_BLOCK_SECONDS
                - (now - previous)
            )

            print(
                f"[SKIP] Duplicate event; "
                f"{remaining}s remaining"
            )

            send_json(
                self,
                200,
                {
                    "ok": True,
                    "accepted": True,
                    "line_sent": False,
                    "reason": (
                        "duplicate_suppressed"
                    ),
                    "retry_after_seconds": max(
                        1,
                        remaining
                    ),
                }
            )
            return

        ok, result = push_to_all_users(
            build_line_message(data)
        )

        if ok:
            _last_sent[sig] = now

            print(
                f"[LINE] ส่งสำเร็จ "
                f"{result['sent']} ผู้ใช้"
            )

            send_json(
                self,
                200,
                {
                    "ok": True,
                    "accepted": True,
                    "line_sent": True,
                    "result": result,
                }
            )

        else:
            print(
                f"[LINE] ส่งไม่สำเร็จ: {result}"
            )

            send_json(
                self,
                502,
                {
                    "ok": False,
                    "accepted": True,
                    "line_sent": False,
                    "result": result,
                }
            )


def main():
    print("=" * 55)
    print("CASON LINE COMMAND SERVER V4")
    print(f"กำลังรอข้อมูลที่ Port {PORT}")
    print(
        "รองรับ ESP32 POST "
        "/ /alert /event /api/alert /api/event"
    )
    print("รองรับ LINE POST /webhook")
    print("รองรับ ESP32 GET /api/command")
    print("รองรับ ESP32 POST /api/command/result")
    print("รองรับ ESP32 POST /api/heartbeat")
    print(
        f"ป้องกันข้อความเดิมส่งซ้ำ "
        f"{DUPLICATE_BLOCK_SECONDS} วินาที"
    )
    print(
        f"Watchdog timeout: "
        f"{HEARTBEAT_TIMEOUT_SECONDS} วินาที"
    )
    print(
        "LINE Token: "
        + (
            "พร้อมใช้งาน"
            if LINE_TOKEN
            else "ยังไม่ได้ตั้งค่า"
        )
    )
    print(
        "LINE Channel Secret: "
        + (
            "พร้อมใช้งาน"
            if LINE_CHANNEL_SECRET
            else "ยังไม่ได้ตั้งค่า"
        )
    )
    print(
        f"LINE Users ที่บันทึกไว้: "
        f"{len(load_users())}"
    )
    if CASON_CHAOS_MODE:
        print("CHAOS MODE: เปิดใช้งาน")
        print(f"- delay_seconds={CASON_CHAOS_DELAY_SECONDS}")
        print(f"- http_500_rate={CASON_CHAOS_HTTP_500_RATE}")
        print(f"- bad_json_rate={CASON_CHAOS_BAD_JSON_RATE}")
        print(f"- drop_line_rate={CASON_CHAOS_DROP_LINE_RATE}")
    else:
        print("CHAOS MODE: ปิดอยู่")
    print("หยุดโปรแกรมด้วย Control + C")
    print("=" * 55)

    watchdog_thread = threading.Thread(
        target=watchdog_loop,
        daemon=True
    )
    watchdog_thread.start()

    server = ThreadingHTTPServer(
        (HOST, PORT),
        Handler
    )

    try:
        server.serve_forever()

    except KeyboardInterrupt:
        print("\nกำลังหยุด Server...")

    finally:
        server.server_close()
        print("Server หยุดแล้ว")


if __name__ == "__main__":
    main()

