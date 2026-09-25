#!/usr/bin/env python3
"""Zero-dependency local server for iterating on the watchdog dashboard."""

from __future__ import annotations

import json
import time
from http.server import SimpleHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import urlparse


WEB_DIR = Path(__file__).resolve().parent
MODE = "normal"

CONFIG = {
    "wifi_ssid": "NossaGalera",
    "wifi_password_configured": True,
    "wifi_pending": False,
    "device_name": "esp-vo",
    "auth_key_configured": True,
    "ctrl_host": "headscale.novoagatto.com",
    "ctrl_tls": True,
    "tls_supported": True,
    "noise_pubkey": "",
    "subnet_enabled": True,
    "subnet_supported": True,
    "subnet_route": "192.168.50.0/24",
    "priority_peer_ip": "",
    "check_interval_s": 60,
    "failures_before_reboot": 5,
    "modem_off_s": 20,
    "modem_boot_s": 300,
    "max_auto_reboots": 3,
    "reboot_window_s": 7200,
}

HEALTH_BASE = {
    "state": "monitoring",
    "state_label": "monitorando",
    "state_remaining_s": 0,
    "firmware_version": "dev-local",
    "ota_slot": "ota_1",
    "ota_pending_verify": False,
    "ota_in_progress": False,
    "wifi_provisioned": True,
    "rescue_ap_active": False,
    "rescue_ap_ssid": "",
    "rescue_ap_ip": "",
    "wifi": True,
    "tailscale_connected": True,
    "local_ip": "192.168.50.3",
    "tailscale_ip": "100.64.0.5",
    "failures": 0,
    "failures_limit": 5,
    "auto_reboots": 0,
    "auto_reboots_limit": 3,
    "modem_off_s": 20,
    "modem_boot_s": 300,
    "check_interval_s": 60,
    "wifi_pending": False,
    "heap_free": 60564,
    "heap_largest": 45056,
}


def health_for_mode() -> dict:
    h = dict(HEALTH_BASE)
    if MODE == "rescue":
        h.update(
            wifi_provisioned=False,
            rescue_ap_active=True,
            rescue_ap_ssid="ModemWatchdog-A0B8",
            rescue_ap_ip="192.168.4.1",
            wifi=False,
            tailscale_connected=False,
            local_ip="",
            tailscale_ip="",
        )
    elif MODE == "rebooting":
        h.update(
            state="waiting-for-modem",
            state_label="modem inicializando",
            state_remaining_s=187,
            wifi=False,
            tailscale_connected=False,
            failures=5,
        )
    elif MODE == "pending":
        h.update(wifi_pending=True)
    elif MODE == "offline":
        h.update(wifi=False, tailscale_connected=False, failures=3)
    return h


class Handler(SimpleHTTPRequestHandler):
    def __init__(self, *args, **kwargs):
        super().__init__(*args, directory=str(WEB_DIR), **kwargs)

    def _json(self, payload: dict, status: int = 200) -> None:
        body = json.dumps(payload).encode()
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Cache-Control", "no-store")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self) -> None:
        global MODE
        parsed = urlparse(self.path)

        if parsed.path == "/health":
            self._json(health_for_mode())
            return
        if parsed.path in ("/api/config", "/api/settings"):
            payload = dict(CONFIG)
            payload["wifi_pending"] = health_for_mode()["wifi_pending"]
            self._json(payload)
            return

        if parsed.path == "/":
            query = parsed.query
            if query.startswith("state="):
                candidate = query.split("=", 1)[1].split("&", 1)[0]
                if candidate in {"normal", "rescue", "rebooting", "pending", "offline"}:
                    MODE = candidate
            self.path = "/index.html"
        super().do_GET()

    def do_POST(self) -> None:
        global CONFIG
        parsed = urlparse(self.path)
        length = int(self.headers.get("Content-Length", "0"))

        if parsed.path in ("/api/config", "/api/settings"):
            raw = self.rfile.read(length)
            try:
                payload = json.loads(raw or b"{}")
            except json.JSONDecodeError:
                self._json({"error": "invalid JSON"}, 400)
                return

            for key in CONFIG:
                if key in payload and key not in {
                    "wifi_password_configured",
                    "auth_key_configured",
                    "tls_supported",
                    "subnet_supported",
                }:
                    CONFIG[key] = payload[key]

            if payload.get("wifi_password"):
                CONFIG["wifi_password_configured"] = True
            if payload.get("auth_key"):
                CONFIG["auth_key_configured"] = True

            self._json({
                "ok": True,
                "restart": True,
                "wifi_changed": "wifi_ssid" in payload or bool(payload.get("wifi_password")),
            })
            return

        if parsed.path in ("/api/reboot", "/reboot"):
            self.rfile.read(length)
            self._json({"ok": True})
            return

        if parsed.path == "/api/ota":
            remaining = length
            while remaining > 0:
                chunk = self.rfile.read(min(65536, remaining))
                if not chunk:
                    break
                remaining -= len(chunk)
                time.sleep(0.005)
            self._json({"ok": True, "restarting": True})
            return

        self.rfile.read(length)
        self._json({"error": "not found"}, 404)

    def log_message(self, fmt: str, *args) -> None:
        print(f"[dev] {self.address_string()} - {fmt % args}")


if __name__ == "__main__":
    print("Modem Watchdog UI dev server")
    print("  http://localhost:8080/")
    print("  http://localhost:8080/?state=rescue")
    print("  http://localhost:8080/?state=rebooting")
    print("  http://localhost:8080/?state=pending")
    print("  http://localhost:8080/?state=offline")
    ThreadingHTTPServer(("127.0.0.1", 8080), Handler).serve_forever()
