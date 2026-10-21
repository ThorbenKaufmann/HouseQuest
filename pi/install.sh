#!/bin/sh
# Runs on the Pi as root (via deploy.sh), from the unpacked deploy directory.
set -e
cd "$(dirname "$0")"
KIOSK_USER=${SUDO_USER:-tkaufmann}
KIOSK_HOME=$(getent passwd "$KIOSK_USER" | cut -d: -f6)

# Game service needs paho (Debian package, same version as python/requirements.txt)
dpkg -s python3-paho-mqtt >/dev/null 2>&1 || apt-get install -y -q python3-paho-mqtt

# App
install -d /opt/housequest
rsync -a --delete www/ /opt/housequest/www/
install -m 644 server.py game.py rules.json /opt/housequest/
install -m 755 kiosk.sh /opt/housequest/

# Kiosk login — created once from the example, never overwritten
install -d -m 700 /etc/housequest
if [ ! -e /etc/housequest/kiosk.json ]; then
  install -m 600 kiosk.json.example /etc/housequest/kiosk.json
  echo "NOTE: /etc/housequest/kiosk.json created — set MQTT password and PIN there"
fi

# Broker config (password file is managed with mosquitto_passwd, not here)
if ! cmp -s mosquitto/housequest.conf /etc/mosquitto/conf.d/housequest.conf ||
   ! cmp -s mosquitto/acl /etc/mosquitto/acl; then
  install -m 644 mosquitto/housequest.conf /etc/mosquitto/conf.d/housequest.conf
  install -m 640 -o mosquitto -g mosquitto mosquitto/acl /etc/mosquitto/acl
  systemctl restart mosquitto
fi

# Services
install -m 644 housequest-web.service housequest-game.service /etc/systemd/system/
systemctl daemon-reload
for s in housequest-web housequest-game; do
  systemctl enable $s >/dev/null 2>&1
  systemctl restart $s
done

# Kiosk autostart for the desktop user (previous file kept as .orig)
AUTOSTART="$KIOSK_HOME/.config/labwc/autostart"
if ! cmp -s labwc-autostart "$AUTOSTART"; then
  [ -e "$AUTOSTART" ] && [ ! -e "$AUTOSTART.orig" ] && cp -p "$AUTOSTART" "$AUTOSTART.orig"
  install -D -m 644 -o "$KIOSK_USER" -g "$KIOSK_USER" labwc-autostart "$AUTOSTART"
  echo "NOTE: kiosk autostart installed — takes effect at next login/reboot"
fi

systemctl is-active mosquitto housequest-web housequest-game
