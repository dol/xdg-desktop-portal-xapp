#!/usr/bin/python3
"""End to end: public ScreenCast API of xdg-desktop-portal -> our backend ->
PipeWire node -> a real GStreamer consumer, checking the pixels it receives."""
import os
import subprocess
import sys

import gi
gi.require_version("Gio", "2.0")
from gi.repository import Gio, GLib

DEST = "org.freedesktop.portal.Desktop"
PATH = "/org/freedesktop/portal/desktop"
IFACE = "org.freedesktop.portal.ScreenCast"
EXPECTED_BGR = (0x99, 0x66, 0x33)  # root painted #336699, grabbed as BGRx

bus = Gio.bus_get_sync(Gio.BusType.SESSION, None)
loop = GLib.MainLoop()
sender = bus.get_unique_name().lstrip(":").replace(".", "_")
counter = 0


def token():
    global counter
    counter += 1
    return f"smoke{os.getpid()}_{counter}"


def request(method, args_before, options, timeout=30):
    """Calls a Request-style method and returns (response, results)."""
    tok = token()
    options = dict(options, handle_token=GLib.Variant("s", tok))
    path = f"/org/freedesktop/portal/desktop/request/{sender}/{tok}"
    result = {}

    def on_response(conn, sender_name, obj, iface, signal, params, data):
        result["value"] = params.unpack()
        loop.quit()

    sub = bus.signal_subscribe(DEST, "org.freedesktop.portal.Request", "Response",
                               path, None, Gio.DBusSignalFlags.NONE, on_response, None)
    GLib.timeout_add_seconds(timeout, loop.quit)
    types = "".join(a[0] for a in args_before)
    bus.call_sync(DEST, PATH, IFACE, method,
                  GLib.Variant(f"({types}a{{sv}})", tuple(a[1] for a in args_before) + (options,)),
                  None, Gio.DBusCallFlags.NONE, 30000, None)
    if "value" not in result:
        loop.run()
    bus.signal_unsubscribe(sub)
    assert "value" in result, f"no Response for {method}"
    return result["value"]


def main():
    response, results = request("CreateSession", [],
                                {"session_handle_token": GLib.Variant("s", token())})
    assert response == 0, f"CreateSession: {response}"
    session = results["session_handle"]

    response, _ = request("SelectSources", [("o", session)],
                          {"types": GLib.Variant("u", 1),
                           "cursor_mode": GLib.Variant("u", 2),
                           "multiple": GLib.Variant("b", False)})
    assert response == 0, f"SelectSources: {response}"

    response, results = request("Start", [("o", session), ("s", "")], {})
    assert response == 0, f"Start: {response}"
    node, props = results["streams"][0]
    print(f"streams: {len(results['streams'])}, size: {tuple(props['size'])}")
    assert tuple(props["size"]) == (800, 600)

    frame = os.path.join(os.environ["XDG_RUNTIME_DIR"], "frame.raw")
    gst = subprocess.run(
        ["gst-launch-1.0", "-q", "pipewiresrc", f"path={node}",
         "stream-properties=props,media.type=Video,media.category=Capture,media.role=Screen",
         "num-buffers=1", "!", "videoconvert", "!",
         "video/x-raw,format=BGRx,width=800,height=600", "!", "filesink", f"location={frame}"],
        capture_output=True, text=True, timeout=30)
    assert gst.returncode == 0, gst.stderr

    data = open(frame, "rb").read()
    assert len(data) == 800 * 600 * 4, len(data)
    off = (10 * 800 + 10) * 4
    assert tuple(data[off:off + 3]) == EXPECTED_BGR, tuple(data[off:off + 3])
    print("frame ok")

    bus.call_sync(DEST, session, "org.freedesktop.portal.Session", "Close",
                  None, None, Gio.DBusCallFlags.NONE, 5000, None)


if __name__ == "__main__":
    try:
        main()
    except Exception as e:
        print("smoke test failed:", repr(e))
        sys.exit(1)
