#!/bin/sh
# Starts the portal (built binary in $1) on the private bus created by
# with-pipewire.sh and runs the D-Bus client against it. Only the portal
# started here is killed.
set -u

PORTAL="$1"
shift

XAPP_SCREENCAST_TEST_AUTOPICK=1 "$PORTAL" -d xfce -v >"$XDG_RUNTIME_DIR/portal.log" 2>&1 &
XAPP_PORTAL_PID=$!
export XAPP_PORTAL_PID

i=0
while [ $i -lt 50 ] && ! gdbus call --session --dest org.freedesktop.DBus --object-path /org/freedesktop/DBus \
    --method org.freedesktop.DBus.NameHasOwner org.freedesktop.impl.portal.desktop.xapp 2>/dev/null | grep -q true; do
  sleep 0.2
  i=$((i + 1))
done

/usr/bin/python3 "$@"
RET=$?

kill "$XAPP_PORTAL_PID" 2>/dev/null
wait "$XAPP_PORTAL_PID" 2>/dev/null
[ $RET -ne 0 ] && tail -30 "$XDG_RUNTIME_DIR/portal.log" >&2
exit $RET
