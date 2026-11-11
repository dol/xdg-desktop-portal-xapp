# ScreenCast acceptance on a real Xfce (X11) session

Gate for the Debian/Ubuntu port. Nothing here needs `sudo`: the build runs from
its build directory and replaces the packaged portal for as long as it runs.

## 1. Build

```sh
# If Linuxbrew's pkg-config comes first on PATH, point it at the system .pc files:
export PKG_CONFIG_PATH=/usr/lib/x86_64-linux-gnu/pkgconfig:/usr/share/pkgconfig
meson setup build -Dscreencast=enabled -Dsystemduserunitdir=$HOME/.local/share/systemd/user
ninja -C build
meson test -C build            # capture, stream, dialog

# The portal and smoke tests skip the consent dialog through a test hook that is
# only compiled in on request. Use a separate build directory, never ship it:
meson setup build-tests -Dscreencast=enabled -Dscreencast_test_hooks=true \
    -Dsystemduserunitdir=$HOME/.local/share/systemd/user
meson test -C build-tests      # capture, stream, dialog, portal, smoke
```

## 2. Make the frontend see the backend (user level, reversible)

```sh
mkdir -p ~/.local/share/xdg-desktop-portal/portals ~/.config/xdg-desktop-portal
cp data/xapp-screencast.portal ~/.local/share/xdg-desktop-portal/portals/

# The system xfce-portals.conf prefers "wlr;gtk" for ScreenCast; prefer ours.
# (A user file replaces the system one entirely, so copy it and change one line.)
cp /usr/share/xdg-desktop-portal/xfce-portals.conf ~/.config/xdg-desktop-portal/
sed -i 's/^org.freedesktop.impl.portal.ScreenCast=.*/org.freedesktop.impl.portal.ScreenCast=xapp-screencast;/' \
    ~/.config/xdg-desktop-portal/xfce-portals.conf
```

## 3. Run the built portal instead of the packaged one

```sh
./build/src/xdg-desktop-portal-xapp -r -v     # leave running in a terminal
systemctl --user restart xdg-desktop-portal   # in another terminal, so it re-reads the files
gdbus call --session --dest org.freedesktop.portal.Desktop \
    --object-path /org/freedesktop/portal/desktop \
    --method org.freedesktop.DBus.Properties.GetAll org.freedesktop.portal.ScreenCast
# expect: AvailableSourceTypes 1 (or 3 with compositing), AvailableCursorModes 3
```

## 4. See the picker without a browser

```sh
tests/screencast/try-share.py --types 3       # picker, then plays the stream
```

## 5. Checklist (Firefox + meet.google.com)

Firefox on X11 may use its own X11 capture and never call the portal. If no
picker from this portal appears, set `media.webrtc.capture.allow-pipewire` to
`true` in `about:config` (not verified on this setup), or use OBS
("Screen Capture (PipeWire)") or Chromium with `--enable-features=WebRTCPipeWireCapturer`.

- [ ] Present, Entire screen: picker lists the monitor; Meet shows the desktop
- [ ] Cursor is visible in the received video
- [ ] Second monitor listed and shareable (if you have one)
- [ ] Present, A window (compositing on): window listed, shows its contents even when partly covered
- [ ] Compositing off (Settings, Window Manager Tweaks): picker offers only monitors
- [ ] Stop sharing from Meet: backend log prints `Closing screencast session`, no leftover process
- [ ] Stop sharing from Firefox's sharing indicator: same
- [ ] Close the shared window mid-share: the share ends cleanly, the portal keeps running
- [ ] Cancel in the picker: Meet reports a denied/cancelled share, nothing hangs
- [ ] Change resolution mid-share (`xrandr`): no crash

## 6. Roll back

Ctrl-C the portal, then:

```sh
rm ~/.local/share/xdg-desktop-portal/portals/xapp-screencast.portal \
   ~/.config/xdg-desktop-portal/xfce-portals.conf
systemctl --user restart xdg-desktop-portal
```

The packaged portal starts again on the next request through D-Bus activation.
