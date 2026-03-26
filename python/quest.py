#!/usr/bin/env python3

import argparse
import json
import sys
import threading
import paho.mqtt.client as mqtt

MQTT_HOST = "datacenter.itc-embedded.de"
MQTT_PORT = 1883

# -----------------------------
# Argumente
# -----------------------------
parser = argparse.ArgumentParser()
parser.add_argument("name", help="player name")
parser.add_argument("--xp", type=str, help="absolute value or delta like +5 / -3")
parser.add_argument("--hp", type=str, help="absolute value or delta like +5 / -10")
parser.add_argument("--role", type=str, help="resident / guest")
args = parser.parse_args()

player_name = args.name.strip()
player_key = player_name.lower()
topic = f"house/quest/players/{player_key}/state"

# -----------------------------
# Hilfsfunktionen
# -----------------------------
def apply_change(current, change, default):
    if current is None:
        current = default

    if change is None:
        return current

    change = str(change).strip()

    if change.startswith("+") or change.startswith("-"):
        return current + int(change)

    return int(change)

# -----------------------------
# Retained State lesen
# -----------------------------
state = {}
message_event = threading.Event()

def on_connect(client, userdata, flags, reason_code, properties=None):
    client.subscribe(topic)

def on_message(client, userdata, msg):
    global state
    payload = msg.payload.decode("utf-8").strip()

    if not payload:
        state = {}
    else:
        try:
            state = json.loads(payload)
        except json.JSONDecodeError:
            print("Fehler: retained payload ist kein gültiges JSON", file=sys.stderr)
            state = {}

    message_event.set()

client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2)
client.on_connect = on_connect
client.on_message = on_message

client.connect(MQTT_HOST, MQTT_PORT, 60)
client.loop_start()

# Warten auf retained message oder timeout
message_event.wait(timeout=1.5)

client.loop_stop()

# -----------------------------
# Aktuellen Zustand lesen
# -----------------------------
current_xp = state.get("xp", 0)
current_hp = state.get("hp", 100)
current_role = state.get("role", "resident")

# Falls irgendwo Strings gespeichert wurden:
try:
    current_xp = int(current_xp)
except Exception:
    current_xp = 0

try:
    current_hp = int(current_hp)
except Exception:
    current_hp = 100

current_role = str(current_role)

# -----------------------------
# Neue Werte berechnen
# -----------------------------
new_xp = apply_change(current_xp, args.xp, 0)
new_hp = apply_change(current_hp, args.hp, 100)
new_role = args.role if args.role is not None else current_role

new_state = {
    "name": player_name.capitalize(),
    "xp": new_xp,
    "hp": new_hp,
    "role": new_role,
}

# -----------------------------
# Neuen Zustand publishen
# -----------------------------
payload = json.dumps(new_state)
result = client.publish(topic, payload, retain=True)
result.wait_for_publish()

client.disconnect()

print(f"{player_name}: XP {current_xp} -> {new_xp}, HP {current_hp} -> {new_hp}, ROLE {new_role}")