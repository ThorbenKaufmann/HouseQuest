#!/usr/bin/env python3

import argparse
import json
import paho.mqtt.client as mqtt

MQTT_HOST = "datacenter.itc-embedded.de"

parser = argparse.ArgumentParser()
parser.add_argument("name")
parser.add_argument("--xp", type=int, default=0)
parser.add_argument("--hp", type=int, default=100)
parser.add_argument("--role", default="resident")

args = parser.parse_args()

data = {
    "name": args.name,
    "xp": args.xp,
    "hp": args.hp,
    "role": args.role
}

topic = f"house/quest/players/{args.name.lower()}/state"

client = mqtt.Client()
client.connect(MQTT_HOST,1883)
client.publish(topic,json.dumps(data))
client.disconnect()
