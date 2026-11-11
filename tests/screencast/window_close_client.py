#!/usr/bin/python3
"""Sharing a window and then closing it must end the session: the application
has to get the Session.Closed signal, otherwise it keeps showing the last frame."""
import os
import subprocess
import sys

import smoke_client as sc
from smoke_client import GLib, Gio


def main():
    helper = subprocess.Popen([os.environ["XAPP_TEST_WINDOW_HELPER"]], stdin=subprocess.PIPE,
                              stdout=subprocess.PIPE, text=True)
    assert helper.stdout.readline().strip() == "ready", "helper did not start"

    response, results = sc.request("CreateSession", [],
                                   {"session_handle_token": GLib.Variant("s", sc.token())})
    assert response == 0, f"CreateSession: {response}"
    session = results["session_handle"]

    response, _ = sc.request("SelectSources", [("o", session)],
                             {"types": GLib.Variant("u", 2),
                              "cursor_mode": GLib.Variant("u", 2)})
    assert response == 0, f"SelectSources: {response}"

    response, results = sc.request("Start", [("o", session), ("s", "")], {})
    assert response == 0, f"Start: {response}"
    node, props = results["streams"][0]
    assert props["source_type"] == 2, props
    print(f"sharing window, node {node}, size {tuple(props['size'])}")

    closed = {}

    def on_closed(*args):
        closed["yes"] = True
        sc.loop.quit()

    sub = sc.bus.signal_subscribe(sc.DEST, "org.freedesktop.portal.Session", "Closed", session,
                                  None, Gio.DBusSignalFlags.NONE, on_closed)

    helper.stdin.write("close\n")
    helper.stdin.flush()
    assert helper.stdout.readline().strip() == "closed"

    GLib.timeout_add_seconds(10, sc.loop.quit)
    sc.loop.run()
    sc.bus.signal_unsubscribe(sub)
    helper.stdin.write("done\n")
    helper.stdin.flush()

    assert closed.get("yes"), "no Session.Closed after the shared window was destroyed"
    print("session closed ok")


if __name__ == "__main__":
    try:
        main()
    except Exception as e:
        print("window close test failed:", repr(e))
        sys.exit(1)
