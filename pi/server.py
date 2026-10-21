#!/usr/bin/env python3
"""HouseQuest web server for the Pi (stdlib only).

Serves the board app (www/) and the ruleset to everyone on the LAN.
/config.json (MQTT login + admin PIN for the kiosk) goes to loopback clients
only — the kiosk browser on the Pi itself. Other devices log in on the page.

Environment:
  HOUSEQUEST_WWW     app directory          (default: www/ next to this file)
  HOUSEQUEST_RULES   rules.json             (default: rules.json next to this file)
  HOUSEQUEST_PORT    listen port            (default: 8080)
  CREDENTIALS_DIRECTORY/kiosk.json          (systemd LoadCredential)
"""

import ipaddress
import os
from functools import partial
from http.server import SimpleHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

HERE       = Path(__file__).resolve().parent
WWW_DIR    = Path(os.environ.get("HOUSEQUEST_WWW", HERE / "www"))
RULES_FILE = Path(os.environ.get("HOUSEQUEST_RULES", HERE / "rules.json"))
PORT       = int(os.environ.get("HOUSEQUEST_PORT", "8080"))
CREDS_DIR  = os.environ.get("CREDENTIALS_DIRECTORY")
KIOSK_FILE = Path(CREDS_DIR) / "kiosk.json" if CREDS_DIR else None


class Handler(SimpleHTTPRequestHandler):
    def end_headers(self):
        self.send_header("Cache-Control", "no-cache")
        super().end_headers()

    def send_file(self, path, ctype):
        try:
            data = path.read_bytes()
        except OSError:
            self.send_error(404)
            return
        self.send_response(200)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def do_GET(self):
        path = self.path.split("?", 1)[0]
        if path == "/rules.json":
            self.send_file(RULES_FILE, "application/json")
        elif path == "/config.json":
            if KIOSK_FILE and ipaddress.ip_address(self.client_address[0]).is_loopback:
                self.send_file(KIOSK_FILE, "application/json")
            else:
                self.send_error(404)
        else:
            super().do_GET()

    def log_message(self, fmt, *args):
        pass   # journal stays quiet; errors still go through log_error


if __name__ == "__main__":
    server = ThreadingHTTPServer(("", PORT), partial(Handler, directory=str(WWW_DIR)))
    print(f"HouseQuest web on :{PORT}, www={WWW_DIR}, rules={RULES_FILE}, "
          f"kiosk config={'yes' if KIOSK_FILE and KIOSK_FILE.exists() else 'no'}", flush=True)
    server.serve_forever()
