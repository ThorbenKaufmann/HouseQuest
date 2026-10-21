#!/bin/sh
# Board in Chromium kiosk mode, once the local web server answers.
URL=http://127.0.0.1:8080/
for i in $(seq 30); do
  curl -fs -o /dev/null "$URL" && break
  sleep 1
done
exec chromium --kiosk --noerrdialogs --disable-infobars --no-first-run \
  --hide-crash-restore-bubble --password-store=basic --blink-settings=preferredColorScheme=0 \
  --disable-pinch --overscroll-history-navigation=0 --disable-features=Translate \
  "$URL"
