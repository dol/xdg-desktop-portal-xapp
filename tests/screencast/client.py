#!/usr/bin/python3
"""Drives org.freedesktop.impl.portal.ScreenCast of a running
xdg-desktop-portal-xapp on the current session bus (no frontend needed).

Environment: XAPP_PORTAL_PID is the portal's pid (for fd accounting).
"""
import os
import subprocess
import sys
import time
import traceback

import gi
gi.require_version("Gio", "2.0")
from gi.repository import Gio, GLib

NAME = "org.freedesktop.impl.portal.desktop.xapp"
PATH = "/org/freedesktop/portal/desktop"
IFACE = "org.freedesktop.impl.portal.ScreenCast"

bus = Gio.bus_get_sync(Gio.BusType.SESSION, None)
_counter = 0


def new_token():
    global _counter
    _counter += 1
    return f"t{os.getpid()}_{_counter}"


def call(method, params, flags=Gio.DBusCallFlags.NONE, timeout=60000):
    return bus.call_sync(NAME, PATH, IFACE, method, params, None, flags, timeout, None)


def prop(name):
    reply = bus.call_sync(NAME, PATH, "org.freedesktop.DBus.Properties", "Get",
                          GLib.Variant("(ss)", (IFACE, name)), None,
                          Gio.DBusCallFlags.NONE, 5000, None)
    return reply.unpack()[0]


def handles():
    token = new_token()
    return (f"/org/freedesktop/portal/desktop/request/xapp_test/{token}",
            f"/org/freedesktop/portal/desktop/session/xapp_test/{token}")


def create_session():
    request, session = handles()
    response, _ = call("CreateSession",
                       GLib.Variant("(oosa{sv})", (request, session, "org.example.Test", {}))
                       ).unpack()
    assert response == 0, f"CreateSession response {response}"
    return session


def select_sources(session, **options):
    request, _ = handles()
    opts = {k: GLib.Variant(sig, v) for k, (sig, v) in options.items()}
    response, _ = call("SelectSources",
                       GLib.Variant("(oosa{sv})", (request, session, "org.example.Test", opts))
                       ).unpack()
    return response


def start(session):
    request, _ = handles()
    response, results = call("Start",
                             GLib.Variant("(oossa{sv})", (request, session, "org.example.Test", "", {}))
                             ).unpack()
    return response, results


def fd_count():
    return len(os.listdir(f"/proc/{os.environ['XAPP_PORTAL_PID']}/fd"))


def session_exists(session):
    try:
        bus.call_sync(NAME, session, "org.freedesktop.impl.portal.Session", "Close",
                      None, None, Gio.DBusCallFlags.NONE, 5000, None)
        return True
    except GLib.Error as e:
        assert "UnknownMethod" in e.message or "UnknownObject" in e.message \
            or "No such" in e.message or "unknown" in e.message.lower(), e.message
        return False


# --- tests -----------------------------------------------------------------

def test_properties():
    assert prop("AvailableCursorModes") == 3
    assert prop("version") == 4
    assert prop("AvailableSourceTypes") == 1  # Xvfb has no compositing manager


def test_select_sources_rejects_metadata_cursor():
    session = create_session()
    assert select_sources(session, cursor_mode=("u", 4)) == 2


def test_full_flow_returns_one_monitor_stream():
    session = create_session()
    assert select_sources(session, types=("u", 1), multiple=("b", False), cursor_mode=("u", 2)) == 0
    response, results = start(session)
    assert response == 0, f"Start response {response}"
    streams = results["streams"]
    assert len(streams) == 1, streams
    node_id, props = streams[0]
    assert node_id > 0
    assert props["source_type"] == 1
    assert tuple(props["size"]) == (800, 600), props
    assert tuple(props["position"]) == (0, 0), props


def test_window_selection_refused_without_compositing():
    session = create_session()
    assert select_sources(session, types=("u", 2)) == 0
    response, _ = start(session)
    assert response == 2, f"expected 2, got {response}"


def test_cancelled_picker_gives_response_1():
    session = create_session()
    assert select_sources(session, types=("u", 1)) == 0
    # Test-only switch honoured by the portal while XAPP_SCREENCAST_TEST_AUTOPICK is set
    flag = os.path.join(os.environ["XDG_RUNTIME_DIR"], "xapp-autopick-cancel")
    open(flag, "w").close()
    try:
        response, results = start(session)
    finally:
        os.unlink(flag)
    assert response == 1, f"expected 1, got {response}"
    assert "streams" not in results


def test_client_vanishes_closes_session():
    before = fd_count()
    code = (
        "import sys; sys.path.insert(0, %r)\n"
        "import client\n"
        "s = client.create_session()\n"
        "assert client.select_sources(s, types=('u', 1)) == 0\n"
        "r, res = client.start(s)\n"
        "assert r == 0\n"
        "print(s, flush=True)\n"
    ) % os.path.dirname(os.path.abspath(__file__))
    out = subprocess.run(["/usr/bin/python3", "-c", code], capture_output=True, text=True,
                         env=dict(os.environ), timeout=60)
    assert out.returncode == 0, out.stderr
    session = out.stdout.strip().splitlines()[-1]

    deadline = time.time() + 5
    while time.time() < deadline and session_exists(session):
        time.sleep(0.2)
    assert not session_exists(session), "session survived its client"
    assert fd_count() <= before, f"fd leak: {before} -> {fd_count()}"


TESTS = [v for k, v in sorted(globals().items()) if k.startswith("test_")]

if __name__ == "__main__":
    wanted = sys.argv[1:]
    failed = 0
    for t in TESTS:
        if wanted and t.__name__ not in wanted:
            continue
        try:
            t()
            print(f"ok {t.__name__}")
        except Exception:
            failed += 1
            print(f"not ok {t.__name__}")
            traceback.print_exc()
    sys.exit(1 if failed else 0)
