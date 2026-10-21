#!/bin/sh
# Deploy board app, web service, broker config and kiosk to the Pi.
#   pi/deploy.sh [ssh-host]      (default: housequest)
set -e
HOST=${1:-housequest}
cd "$(dirname "$0")"
DIR=/tmp/housequest-deploy

rsync -a --delete www server.py kiosk.sh kiosk.json.example install.sh labwc-autostart \
  housequest-web.service mosquitto ../HouseQuest/data/rules.json "$HOST:$DIR/"
ssh -t "$HOST" "sudo sh $DIR/install.sh && rm -rf $DIR"   # -t: sudo may ask for the password
