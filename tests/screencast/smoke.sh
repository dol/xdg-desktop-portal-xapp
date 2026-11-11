#!/bin/sh
# Full-stack smoke test: Xvfb (from xvfb-run) + PipeWire + the xdg-desktop-portal
# frontend + our backend, driven through the public ScreenCast API.
# Usage (inside with-pipewire.sh): smoke.sh BACKEND_BINARY PORTALS_DIR
set -u

BACKEND="$1"
PORTALS_DIR="$2"

export XDG_CURRENT_DESKTOP=XFCE
export XDG_DESKTOP_PORTAL_DIR="$PORTALS_DIR"
export XAPP_SCREENCAST_TEST_AUTOPICK=1

# Xvfb resets the screen when its last client disconnects, so the colour is
# painted only once the backend (an X client through GTK) is up and stays up.
"$BACKEND" -d xfce -v >"$XDG_RUNTIME_DIR/backend.log" 2>&1 &
BACKEND_PID=$!
i=0
while [ $i -lt 50 ] && ! gdbus call --session --dest org.freedesktop.DBus --object-path /org/freedesktop/DBus \
    --method org.freedesktop.DBus.NameHasOwner org.freedesktop.impl.portal.desktop.xapp 2>/dev/null | grep -q true; do
  sleep 0.2
  i=$((i + 1))
done
xsetroot -solid '#336699'
/usr/libexec/xdg-desktop-portal -r >"$XDG_RUNTIME_DIR/frontend.log" 2>&1 &
FRONTEND_PID=$!

i=0
while [ $i -lt 50 ] && ! gdbus call --session --dest org.freedesktop.DBus --object-path /org/freedesktop/DBus \
    --method org.freedesktop.DBus.NameHasOwner org.freedesktop.portal.Desktop 2>/dev/null | grep -q true; do
  sleep 0.2
  i=$((i + 1))
done

/usr/bin/python3 "$(dirname "$0")/smoke_client.py"
RET=$?

kill "$FRONTEND_PID" "$BACKEND_PID" 2>/dev/null
wait "$FRONTEND_PID" "$BACKEND_PID" 2>/dev/null
if [ $RET -ne 0 ]; then
  echo "--- backend"; tail -20 "$XDG_RUNTIME_DIR/backend.log"
  echo "--- frontend"; tail -20 "$XDG_RUNTIME_DIR/frontend.log"
fi
exit $RET
