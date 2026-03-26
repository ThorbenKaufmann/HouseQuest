## Medienkonsum als negatives Handicap einbauen mit Unify:

Python fragt regelmäßig den UniFi-Controller nach Traffic pro Client.

MAC-Adresse wird einem Spieler zugeordnet.

Datenmenge wird berechnet (GB).

Script sendet MQTT-Update an dein Panel.

```
pip install unifi-client
pip install paho-mqtt
```


```python
from unifi_client import UnifiClient

HOST = "192.168.10.2"
USERNAME = "apiuser"
PASSWORD = "password"

client = UnifiClient(HOST, USERNAME, PASSWORD)

clients = client.list_clients()

for c in clients:
    name = c.get("name", "unknown")
    mac = c.get("mac")
    rx = c.get("rx_bytes", 0)
    tx = c.get("tx_bytes", 0)

    gb = (rx + tx) / (1024**3)

    print(name, mac, round(gb,2), "GB")
```

### MAC → Spieler Mapping

MAC_TO_PLAYER = {
    "aa:bb:cc:dd:ee:ff": "Paul",
    "11:22:33:44:55:66": "Levin",
    "aa:bb:11:22:33:44": "Juna",
}

### MQTT an dein Panel senden

```python
import json
import paho.mqtt.client as mqtt

MQTT_HOST = "192.168.10.5"

client_mqtt = mqtt.Client()
client_mqtt.connect(MQTT_HOST)

for c in clients:

    mac = c["mac"]

    if mac not in MAC_TO_PLAYER:
        continue

    player = MAC_TO_PLAYER[mac]

    rx = c.get("rx_bytes",0)
    tx = c.get("tx_bytes",0)

    gb = (rx + tx)/(1024**3)

    payload = {
        "name": player,
        "dataGB": round(gb,2)
    }

    topic = f"house/quest/players/{player.lower()}/state"

    client_mqtt.publish(topic, json.dumps(payload))
```

### Optional: Daten-Handicap berechnen

def data_penalty(gb):

    if gb > 15:
        return -15
    if gb > 10:
        return -10
    if gb > 5:
        return -5
    if gb > 2:
        return -2

    return 0


### Noch lustiger: Daten-Titel

| Titel              | Bedingung |
| ------------------ | --------- |
| Bandwidth Goblin   | >10 GB    |
| Streaming Overlord | >20 GB    |
| Silent Ninja       | <0.5 GB   |
