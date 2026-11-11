#!/bin/sh
# Runs "$@" with a private session bus, PipeWire and WirePlumber, all inside
# a private runtime dir. Only processes started here are ever killed.
set -u

if [ -z "${XAPP_TEST_PRIVATE_BUS:-}" ]; then
  XAPP_TEST_PRIVATE_BUS=1 exec dbus-run-session -- "$0" "$@"
fi

R=$(mktemp -d)
chmod 700 "$R"
export XDG_RUNTIME_DIR="$R" PIPEWIRE_RUNTIME_DIR="$R"
export GIO_USE_VFS=local

pipewire >"$R/pipewire.log" 2>&1 &
PW=$!
sleep 1
wireplumber >"$R/wireplumber.log" 2>&1 &
WP=$!

# Wait for the session manager to be up before the test links anything
i=0
while [ $i -lt 50 ] && ! pw-cli ls Node >/dev/null 2>&1; do
  sleep 0.2
  i=$((i + 1))
done
sleep 1

"$@"
RET=$?

kill "$WP" "$PW" 2>/dev/null
wait "$WP" "$PW" 2>/dev/null
rm -rf "$R"
exit $RET
