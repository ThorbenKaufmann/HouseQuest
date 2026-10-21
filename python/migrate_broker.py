#!/usr/bin/env python3
"""Move the retained HouseQuest state (house/quest/#) from one broker to another.

Brokers from environment, e.g.:
  export HOUSEQUEST_SRC_MQTT_HOST=old.broker HOUSEQUEST_SRC_MQTT_USER=... HOUSEQUEST_SRC_MQTT_PASSWORD=...
  export HOUSEQUEST_DST_MQTT_HOST=new.broker HOUSEQUEST_DST_MQTT_USER=... HOUSEQUEST_DST_MQTT_PASSWORD=...
  (optional: HOUSEQUEST_SRC_MQTT_PORT / HOUSEQUEST_DST_MQTT_PORT, default 1883)

Steps:
  python migrate_broker.py copy           # source → destination, then verify
  (point the panels to the new broker)
  python migrate_broker.py clear-source   # verify again, then delete retained on source
  python migrate_broker.py clear-source --force   # delete on source without comparing (no DST needed)

Every run writes a backup of the source state to migrate_backup_<timestamp>.json.
"""

import argparse
import base64
import json
import os
import sys
import time
import paho.mqtt.client as mqtt

TOPIC_FILTER = "house/quest/#"
QUIET_S      = 1.5    # retained burst is over after this long without a message
TIMEOUT_S    = 10.0

# -----------------------------
# Broker access
# -----------------------------
def broker(prefix):
    host = os.environ.get(f"HOUSEQUEST_{prefix}_MQTT_HOST")
    if not host:
        sys.exit(f"Fehler: HOUSEQUEST_{prefix}_MQTT_HOST nicht gesetzt")
    return {
        "name": prefix.lower(),
        "host": host,
        "port": int(os.environ.get(f"HOUSEQUEST_{prefix}_MQTT_PORT", "1883")),
        "user": os.environ.get(f"HOUSEQUEST_{prefix}_MQTT_USER"),
        "password": os.environ.get(f"HOUSEQUEST_{prefix}_MQTT_PASSWORD"),
    }

def connect(cfg, on_message=None):
    connected = {"rc": None}

    def on_connect(client, userdata, flags, reason_code, properties=None):
        connected["rc"] = reason_code

    client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2,
                         client_id=f"housequest-migrate-{cfg['name']}-{os.getpid()}")
    client.on_connect = on_connect
    if on_message:
        client.on_message = on_message
    if cfg["user"]:
        client.username_pw_set(cfg["user"], cfg["password"])

    client.connect(cfg["host"], cfg["port"], 60)
    client.loop_start()

    deadline = time.time() + TIMEOUT_S
    while connected["rc"] is None and time.time() < deadline:
        time.sleep(0.05)
    if connected["rc"] is None or connected["rc"].is_failure:
        client.loop_stop()
        sys.exit(f"Fehler: Verbindung zu {cfg['host']}:{cfg['port']} fehlgeschlagen ({connected['rc']})")
    return client

# -----------------------------
# Retained state lesen
# -----------------------------
def read_retained(cfg):
    """Returns {topic: payload_bytes} of all retained messages below TOPIC_FILTER."""
    state = {}
    last = {"t": time.time()}

    def on_message(client, userdata, msg):
        # Live traffic is not part of the stored state
        if msg.retain and msg.payload:
            state[msg.topic] = msg.payload
        last["t"] = time.time()

    client = connect(cfg, on_message)
    client.subscribe(TOPIC_FILTER, qos=1)

    start = time.time()
    while time.time() - last["t"] < QUIET_S and time.time() - start < TIMEOUT_S:
        time.sleep(0.05)

    client.loop_stop()
    client.disconnect()
    return state

def publish_retained(cfg, messages):
    """messages: {topic: payload_bytes}; empty payload deletes the retained message."""
    client = connect(cfg)
    for topic, payload in messages.items():
        info = client.publish(topic, payload, qos=1, retain=True)
        info.wait_for_publish(timeout=TIMEOUT_S)
        if not info.is_published():
            sys.exit(f"Fehler: publish auf {cfg['host']} fehlgeschlagen: {topic}")
    client.loop_stop()
    client.disconnect()

# -----------------------------
# Hilfsfunktionen
# -----------------------------
def write_backup(cfg, state):
    path = time.strftime("migrate_backup_%Y%m%d_%H%M%S.json")
    data = {
        "broker": f"{cfg['host']}:{cfg['port']}",
        "messages": [{"topic": t, "payload_b64": base64.b64encode(p).decode("ascii")}
                     for t, p in sorted(state.items())],
    }
    with open(path, "w", encoding="utf-8") as f:
        json.dump(data, f, indent=2)
    print(f"Backup: {path} ({len(state)} Topics)")

def missing_or_different(src_state, dst_state):
    return [t for t, p in sorted(src_state.items()) if dst_state.get(t) != p]

def show(state):
    for topic, payload in sorted(state.items()):
        print(f"  {topic}  {payload.decode('utf-8', errors='replace')}")

# -----------------------------
# Kommandos
# -----------------------------
def cmd_copy(src, dst, force):
    src_state = read_retained(src)
    if not src_state:
        sys.exit(f"Fehler: keine retained Messages unter {TOPIC_FILTER} auf {src['host']}")
    print(f"Quelle {src['host']}: {len(src_state)} Topics")
    show(src_state)
    write_backup(src, src_state)

    # Don't silently overwrite state that already lives on the destination
    dst_state = read_retained(dst)
    conflicts = [t for t in dst_state if t in src_state and dst_state[t] != src_state[t]]
    extra = [t for t in dst_state if t not in src_state]
    if (conflicts or extra) and not force:
        print(f"Ziel {dst['host']} hat bereits abweichende Daten:", file=sys.stderr)
        for t in conflicts + extra:
            print(f"  {t}", file=sys.stderr)
        sys.exit("Abbruch (mit --force trotzdem überschreiben)")

    publish_retained(dst, src_state)

    bad = missing_or_different(src_state, read_retained(dst))
    if bad:
        print("Fehler: auf dem Ziel fehlen / weichen ab:", file=sys.stderr)
        for t in bad:
            print(f"  {t}", file=sys.stderr)
        sys.exit(1)
    print(f"OK: {len(src_state)} Topics nach {dst['host']} kopiert und geprüft")

def cmd_clear_source(src, dst):
    src_state = read_retained(src)
    if not src_state:
        print(f"Quelle {src['host']}: nichts mehr unter {TOPIC_FILTER}")
        return
    write_backup(src, src_state)

    # Only delete what the destination demonstrably holds (dst=None: --force)
    bad = missing_or_different(src_state, read_retained(dst)) if dst else []
    if bad:
        print("Abbruch: Ziel hat diese Topics nicht (oder anders) — erst 'copy' ausführen:", file=sys.stderr)
        for t in bad:
            print(f"  {t}", file=sys.stderr)
        sys.exit(1)

    publish_retained(src, {t: b"" for t in src_state})

    left = read_retained(src)
    if left:
        print("Fehler: auf der Quelle noch vorhanden:", file=sys.stderr)
        show(left)
        sys.exit(1)
    print(f"OK: {len(src_state)} Topics auf {src['host']} gelöscht")

# -----------------------------
# Argumente
# -----------------------------
parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
parser.add_argument("command", choices=["copy", "clear-source"])
parser.add_argument("--force", action="store_true",
                    help="copy: overwrite differing data on destination; "
                         "clear-source: delete without comparing to destination")
args = parser.parse_args()

src = broker("SRC")
if args.command == "clear-source" and args.force:
    cmd_clear_source(src, None)
    sys.exit(0)

dst = broker("DST")
if (src["host"], src["port"]) == (dst["host"], dst["port"]):
    sys.exit("Fehler: Quelle und Ziel sind derselbe Broker")

if args.command == "copy":
    cmd_copy(src, dst, args.force)
else:
    cmd_clear_source(src, dst)
