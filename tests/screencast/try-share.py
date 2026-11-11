#!/usr/bin/python3
"""Manual helper: asks the running portal for a screen share (showing the real
picker), then plays the stream in a window so you can see what a browser gets.

  tests/screencast/try-share.py [--types 1|2|3] [--cursor 1|2] [--seconds N]
"""
import argparse
import subprocess
import sys

from smoke_client import GLib, request, token  # same public-API helpers

parser = argparse.ArgumentParser()
parser.add_argument("--types", type=int, default=3, help="1 monitors, 2 windows, 3 both")
parser.add_argument("--cursor", type=int, default=2, help="1 hidden, 2 embedded")
parser.add_argument("--seconds", type=int, default=20)
args = parser.parse_args()

response, results = request("CreateSession", [], {"session_handle_token": GLib.Variant("s", token())})
if response != 0:
    sys.exit(f"CreateSession failed: {response}")
session = results["session_handle"]

response, _ = request("SelectSources", [("o", session)],
                      {"types": GLib.Variant("u", args.types),
                       "cursor_mode": GLib.Variant("u", args.cursor),
                       "multiple": GLib.Variant("b", False)})
if response != 0:
    sys.exit(f"SelectSources failed: {response}")

print("Pick something in the dialog...")
response, results = request("Start", [("o", session), ("s", "")], {})
if response == 1:
    sys.exit("Cancelled in the picker (response 1)")
if response != 0:
    sys.exit(f"Start failed: {response}")

for node, props in results["streams"]:
    print(f"node {node}: source_type={props['source_type']} size={tuple(props['size'])}")

node = results["streams"][0][0]
print(f"Playing for {args.seconds}s (close the window to stop)...")
try:
    subprocess.run(["timeout", str(args.seconds), "gst-launch-1.0", "pipewiresrc", f"path={node}",
                    "stream-properties=props,media.type=Video,media.category=Capture,media.role=Screen",
                    "!", "videoconvert", "!", "autovideosink"])
finally:
    import smoke_client
    smoke_client.bus.call_sync(smoke_client.DEST, session, "org.freedesktop.portal.Session", "Close",
                               None, None, smoke_client.Gio.DBusCallFlags.NONE, 5000, None)
