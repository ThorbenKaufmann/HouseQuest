
---

# 1. Edit rules / values in Doc/ruleset.md, then:
source python/venv/bin/activate
python python/ruleset_to_yaml.py
# → Doc/ruleset.yaml, Doc/ruleset_flat.yaml, HouseQuest/data/ruleset.json

# Device setup (WiFi / MQTT / PIN) happens on the panel itself, see below.

# 2. Upload filesystem (rules only, no reflash):
pio run --target uploadfs

# 3. Upload firmware (when code changes):
pio run --target upload
Device setup (WiFi / MQTT / admin PIN):

  First boot (no WiFi saved) → panel opens the hotspot "HouseQuest-XXXXXX"
  and shows its password on the display.
    1. Connect phone/laptop to that hotspot
    2. Browser → http://192.168.4.1 (usually opens automatically)
    3. Pick WiFi, enter MQTT broker / user / password and admin PIN → Save
  Settings are stored in NVS and survive firmware uploads.
  Change later: Admin PIN → SETTINGS → [WIFI / MQTT SETUP]
  Saved WiFi unreachable at boot → portal opens for 5 min, then retries in background.
  Optional first-boot defaults for MQTT/PIN: HouseQuest/include/secrets.h
  (copy from secrets.h.example, git-ignored).

CLI (python/quest.py) reads the broker from the environment:
  export HOUSEQUEST_MQTT_HOST=mqtt.local
  export HOUSEQUEST_MQTT_USER=...  HOUSEQUEST_MQTT_PASSWORD=...   # optional

User flow on the device:


Admin PIN → Select player → Action screen
  [BY RULE]  (greyed out if no ruleset on filesystem)
    → numpad: type  2.1.3  → [OK]
    → confirm screen: "Rule 2.1.3 / Launen sind erlaubt... / HP -10 / Mia: 85 → 75 HP"
    → [APPLY]  →  publishes via MQTT, returns to action screen

Both options work — no extra installation needed.

VS Code (easiest)
In the PlatformIO sidebar (the hexagon/alien icon on the left):


PROJECT TASKS
└── az-delivery-devkit-v4
    └── Platform
        ├── Build Filesystem Image
        └── Upload Filesystem Image   ← this one
Click Upload Filesystem Image — it builds the LittleFS image from HouseQuest/data/ and flashes it to the ESP32 in one step.

Bash (your pio is already installed)
You just need to use the full path — or add it to your PATH:


# One-off:
~/.platformio/penv/bin/pio run -t uploadfs

# Or add to ~/.bashrc / ~/.zshrc so plain 'pio' works:
export PATH="$HOME/.platformio/penv/bin:$PATH"
After that, pio run -t uploadfs works directly.

Important: the filesystem upload is separate from the firmware upload. If you flash new firmware (pio run -t upload) it won't erase the LittleFS partition — but if you do a full erase or change the partition table, you'll need to re-upload the filesystem image too.

---

# View in terminal
python python/level_rules.py 1

# Save to file
python python/level_rules.py 1 > Doc/rules_level1.md
python python/level_rules.py 2 > Doc/rules_level2.md

# Custom ruleset path
python python/level_rules.py 1 path/to/other_ruleset.md