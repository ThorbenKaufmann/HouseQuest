#!/usr/bin/env python3
"""Import task templates from Doc/catalog.yaml into the game service on the Pi.

Goes through the service (MQTT requests), not the database. Templates are
matched by title: changed ones are updated, new ones added, templates that
exist only on the Pi are listed and left alone.

Broker config from environment, e.g.:
  export HOUSEQUEST_MQTT_HOST=housequest.itc-embedded.de HOUSEQUEST_MQTT_USER=housequest
  export HOUSEQUEST_MQTT_PASSWORD=...

  python catalog_import.py [catalog.yaml] [--dry-run]
"""

import argparse
import json
import os
import sys
import threading
import time
import uuid
from pathlib import Path

import paho.mqtt.client as mqtt
import yaml

MQTT_HOST     = os.environ.get("HOUSEQUEST_MQTT_HOST", "localhost")
MQTT_PORT     = int(os.environ.get("HOUSEQUEST_MQTT_PORT", "1883"))
MQTT_USER     = os.environ.get("HOUSEQUEST_MQTT_USER")
MQTT_PASSWORD = os.environ.get("HOUSEQUEST_MQTT_PASSWORD")

T_CATALOG = "house/quest/catalog"
CLIENT    = "catalog-import-" + uuid.uuid4().hex[:6]
T_CMD     = "house/quest/cmd/" + CLIENT
T_REPLY   = "house/quest/reply/" + CLIENT
TIMEOUT_S = 5.0

DEFAULT_FILE = Path(__file__).resolve().parent.parent / "Doc" / "catalog.yaml"

# -----------------------------
# Argumente
# -----------------------------
parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
parser.add_argument("file", nargs="?", default=str(DEFAULT_FILE))
parser.add_argument("--dry-run", action="store_true", help="only show what would change")
args = parser.parse_args()

with open(args.file, encoding="utf-8") as f:
    wanted = (yaml.safe_load(f) or {}).get("templates") or []

for t in wanted:
    t["title"]  = str(t["title"]).strip()
    t["xp"]     = int(t.get("xp", 0))
    t["hp"]     = int(t.get("hp", 0))
    t["active"] = bool(t.get("active", True))
    if not 1 <= len(t["title"]) <= 30 or not 0 <= t["xp"] <= 100 or not 0 <= t["hp"] <= 100:
        sys.exit(f"Fehler in {args.file}: {t} (Titel 1–30 Zeichen, XP/HP 0–100)")

# -----------------------------
# Verbindung
# -----------------------------
catalog_event = threading.Event()
reply_event   = threading.Event()
current = []
last_reply = {}

def on_connect(client, userdata, flags, reason_code, properties=None):
    client.subscribe([(T_CATALOG, 1), (T_REPLY, 1)])

def on_message(client, userdata, msg):
    global current, last_reply
    if msg.topic == T_CATALOG:
        current = json.loads(msg.payload or b"{}").get("templates", [])
        catalog_event.set()
    elif msg.topic == T_REPLY:
        last_reply = json.loads(msg.payload)
        reply_event.set()

client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2, client_id=CLIENT)
client.on_connect = on_connect
client.on_message = on_message
if MQTT_USER:
    client.username_pw_set(MQTT_USER, MQTT_PASSWORD)
client.connect(MQTT_HOST, MQTT_PORT, 60)
client.loop_start()

if not catalog_event.wait(TIMEOUT_S):
    sys.exit(f"Fehler: kein Katalog von {MQTT_HOST} – läuft der Spieldienst (housequest-game)?")

def save(template):
    reply_event.clear()
    req_id = uuid.uuid4().hex[:8]
    client.publish(T_CMD, json.dumps({"id": req_id, "op": "catalog.save", "template": template}), qos=1)
    if not reply_event.wait(TIMEOUT_S) or last_reply.get("id") != req_id:
        sys.exit("Fehler: keine Antwort vom Spieldienst")
    if not last_reply.get("ok"):
        sys.exit(f"Fehler bei „{template['title']}“: {last_reply.get('error')}")

# -----------------------------
# Abgleich
# -----------------------------
by_title = {t["title"].casefold(): t for t in current}
added = updated = same = 0
for t in wanted:
    have = by_title.pop(t["title"].casefold(), None)
    if have and (have["title"], have["xp"], have["hp"], have["active"]) == (t["title"], t["xp"], t["hp"], t["active"]):
        same += 1
        continue
    if have:
        print(f"  ändern  {t['title']}: XP {have['xp']}→{t['xp']}, HP {have['hp']}→{t['hp']}"
              + ("" if have["active"] == t["active"] else f", aktiv {have['active']}→{t['active']}"))
        updated += 1
        if not args.dry_run:
            save({**t, "id": have["id"]})
    else:
        print(f"  neu     {t['title']}: XP {t['xp']}, HP {t['hp']}")
        added += 1
        if not args.dry_run:
            save(t)

for t in by_title.values():
    print(f"  nur auf dem Pi (bleibt): {t['title']}")

client.loop_stop()
client.disconnect()
print(f"{'Probelauf: ' if args.dry_run else ''}{added} neu, {updated} geändert, {same} unverändert")
