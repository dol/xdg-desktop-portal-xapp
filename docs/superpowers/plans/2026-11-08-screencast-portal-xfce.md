# ScreenCast Portal for XFCE (X11) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Implement `org.freedesktop.impl.portal.ScreenCast` for XFCE on X11 so Firefox in Google Meet can share a monitor or window; then carry it to the Ubuntu `resolute` package.

**Architecture:** The portal owns its PipeWire video stream (`pw_stream`, source role) and fills SHM buffers from X11 (XShm/XGetImage for monitors, XComposite pixmaps for windows, XDamage to pace frames, XFixes to composite the cursor). Capture is behind a small backend interface (`capture.h`) so a Cinnamon/Muffin or Wayland backend can be added later. Everything lives in `src/screencast/`, is compiled only when PipeWire and the X11 libs are found, and is registered only in `XFCE_MODE`.

**Tech Stack:** C, meson, GLib/GIO/GTK3, libpipewire-0.3, Xlib (X11, Xext, Xcomposite, Xdamage, Xfixes, Xrandr).

**Spec:** Decisions from the grill-me session of 2026-11-08 (below). Reference for the portal side only: closed Cinnamon PR linuxmint/xdg-desktop-portal-xapp#31 (picker dialog and request/session plumbing may be adapted; its Muffin backend is not reusable).

## Global Constraints

- X11 sessions only. No Wayland backend, no Muffin backend in this plan.
- Source types: MONITOR always; WINDOW only while a compositing manager owns `_NET_WM_CM_S<screen>` (evaluated each time `AvailableSourceTypes` is read and again in `Start`).
- Cursor modes: HIDDEN (1) and EMBEDDED (2) only; default EMBEDDED. No METADATA.
- No persistence: ignore `persist_mode` and `restore_token`; always show the picker; interface `version` property = 4.
- Buffers: SHM (memfd) only, no DMA-BUF. Frame rate capped at 30 fps; frames are produced on XDamage, with a 1 fps keepalive so consumers never stall.
- Build: new meson feature option `screencast` (default `auto`); the existing build must be unchanged when PipeWire is absent.
- Registered via a separate `data/xapp-screencast.portal` with `UseIn=XFCE`; `xapp.portal` is not edited (Cinnamon/MATE behaviour unchanged).
- Code style: match `src/*.c` (2-space GLib style in session/request, `g_autoptr`, `G_LOG_DOMAIN` from config.h). Add new translatable strings to `po/POTFILES.in`.
- Out of scope: RemoteDesktop, restore tokens, METADATA cursor, DMA-BUF, Wayland.

## Review Focus

- Client disconnects (Firefox killed) mid-share: session must close, stream be destroyed, X resources freed, no leaked memfds.
- User cancels the picker: `Start` responds with response 1, no stream is left behind.
- Monitor layout changes or hot-plug during a share (xrandr): stream resizes or the session ends cleanly, never crashes or sends a stale-sized buffer.
- Target window closes or is minimised while shared: the stream emits the last frame then the session closes; no BadWindow X error kills the portal.
- Compositing turned off after the picker lists windows: `Start` refuses a WINDOW selection with a clear error instead of streaming blank frames.
- Two concurrent sessions (two tabs/apps): independent streams, independent teardown.
- HiDPI / scaled monitors: reported `size` equals the captured pixel size.

## File Structure

| File | Responsibility |
|---|---|
| `src/screencast/capture.h` | Backend interface: `ScreenCastCapture` vtable (list sources, start, grab frame, stop) |
| `src/screencast/capture-x11.c` | X11 implementation: XRandR monitors, window list, XShm/XComposite grab, XFixes cursor, XDamage |
| `src/screencast/compositing.c/.h` | `gboolean screencast_compositing_active (void)` |
| `src/screencast/stream.c/.h` | `ScreenCastStream`: pw_stream on a private `pw_thread_loop`, SHM buffers, pacing |
| `src/screencast/screencast.c/.h` | Portal D-Bus impl: `CreateSession`, `SelectSources`, `Start`, properties, `ScreenCastSession` |
| `src/screencast/screencastdialog.c/.h/.ui` | GTK picker (monitor/window list, multi-select) |
| `data/xapp-screencast.portal` | Portal registration |
| `tests/screencast/smoke.sh`, `tests/screencast/client.py` | Xvfb + PipeWire smoke test |
| `debian/patches/*` or `debian/control` | Packaging (Task 9) |

---

### Task 1: Build wiring and interface XML

**Files:**
- Modify: `meson_options.txt`, `src/meson.build`, `data/meson.build`, `po/POTFILES.in`
- Create: `data/xapp-screencast.portal`

**Interfaces:**
- Produces: config macro `HAVE_SCREENCAST`; generated `XdpImplScreenCast` skeleton from `org.freedesktop.impl.portal.ScreenCast.xml` (from `desktop_portal_interfaces_dir`); `data/xapp-screencast.portal` with `DBusName=org.freedesktop.impl.portal.desktop.xapp`, `Interfaces=org.freedesktop.impl.portal.ScreenCast;`, `UseIn=XFCE`.

- [ ] **Step 1:** Add `option('screencast', type: 'feature', value: 'auto')`.
- [ ] **Step 2:** In `src/meson.build` look up `libpipewire-0.3 (>= 0.3.40)`, `x11`, `xext`, `xcomposite`, `xdamage`, `xfixes`, `xrandr` with `required: get_option('screencast')`; enable only if all found; set `HAVE_SCREENCAST`; add ScreenCast XML to gdbus codegen and `src/screencast/*.c` to sources only then.
- [ ] **Step 3:** Install `xapp-screencast.portal` only when enabled.
- [ ] **Step 4: Verify both configurations.**
  Run: `meson setup build -Dscreencast=disabled --wipe && ninja -C build` → builds, binary has no pipewire symbols (`nm -D` has none).
  Run: `meson setup build -Dscreencast=enabled --wipe` → fails with a clear missing-dependency message if dev packages are absent (install `libpipewire-0.3-dev libx11-dev libxext-dev libxcomposite-dev libxdamage-dev libxfixes-dev libxrandr-dev`), then succeeds.
- [ ] **Step 5: Commit** `build: add optional screencast feature`

### Task 2: Capture backend interface and X11 monitors

**Files:**
- Create: `src/screencast/capture.h`, `src/screencast/capture-x11.c`, `tests/screencast/test-capture.c`
- Modify: `src/meson.build` (test executable run under `xvfb-run`)

**Interfaces:**
- Produces:
  - `typedef struct { guint32 id; ScreenCastSourceType type; char *name; gint x, y, width, height; } ScreenCastSource;`
  - `GPtrArray *screencast_capture_list_sources (ScreenCastCapture *c, ScreenCastSourceType types)`
  - `gboolean screencast_capture_grab (ScreenCastCapture *c, const ScreenCastSource *s, gboolean embed_cursor, guint8 *dst, gsize stride, GError **error)` — writes BGRx pixels
  - `ScreenCastCapture *screencast_capture_x11_new (GError **error)`
- `ScreenCastSource.id` is the XRandR CRTC id for monitors and the XID for windows.

- [ ] **Step 1: Write failing test** `test_monitor_listed_and_grab_matches_root_colour`: under Xvfb `:99` with a 800x600 screen, `list_sources(MONITOR)` returns exactly one source of 800x600; after painting the root window `0x336699`, `grab` yields pixel `(0x33,0x66,0x99)` at (10,10).
- [ ] **Step 2:** Run `xvfb-run -s "-screen 0 800x600x24" meson test -C build capture` → FAIL (not defined).
- [ ] **Step 3: Implement** monitors via `XRRGetScreenResourcesCurrent`, grab via `XShmGetImage` with `XGetImage` fallback when SHM is unavailable.
- [ ] **Step 4:** Re-run → PASS.
- [ ] **Step 5: Commit** `screencast: X11 monitor capture`

### Task 3: Windows, compositing detection, cursor

**Files:**
- Create: `src/screencast/compositing.c/.h`; Modify: `capture-x11.c`, `tests/screencast/test-capture.c`

**Interfaces:**
- Produces: `gboolean screencast_compositing_active (void)`; `list_sources(WINDOW)` returns mapped, normal-type top-level windows from `_NET_CLIENT_LIST`, titled by `_NET_WM_NAME`; `grab()` for a window uses `XCompositeNameWindowPixmap`.

- [ ] **Step 1: Write failing tests:** `test_compositing_false_without_manager` (Xvfb alone → FALSE); `test_compositing_true_when_selection_owned` (test claims `_NET_WM_CM_S0` → TRUE); `test_window_grab_of_obscured_window` (two overlapping windows, red below, blue above, compositing selection faked; grab of red returns red at an obscured point — skipped if no real compositor, else run in the manual checklist); `test_cursor_embedded_changes_pixels` (XTest-warp pointer to (20,20); grab with `embed_cursor=TRUE` differs from `FALSE` at that point, and `FALSE` equals the background).
- [ ] **Step 2:** Run → FAIL.
- [ ] **Step 3: Implement** the four functions; select `XSetErrorHandler` that records `BadWindow`/`BadMatch` into the `GError` instead of aborting (Review Focus: window closes mid-share).
- [ ] **Step 4:** Run → PASS.
- [ ] **Step 5: Commit** `screencast: window capture, compositing detection, embedded cursor`

### Task 4: PipeWire stream

**Files:**
- Create: `src/screencast/stream.c/.h`, `tests/screencast/test-stream.c`

**Interfaces:**
- Consumes: Task 2/3 capture API.
- Produces:
  - `ScreenCastStream *screencast_stream_new (ScreenCastCapture *c, const ScreenCastSource *s, gboolean embed_cursor, GError **error)`
  - `guint32 screencast_stream_get_node_id (ScreenCastStream *)` — valid once the stream reached PAUSED
  - `void screencast_stream_set_closed_callback (ScreenCastStream *, void (*cb)(ScreenCastStream *, gpointer), gpointer)` — called when the source disappears or its size changes irrecoverably
  - `void screencast_stream_destroy (ScreenCastStream *)`
- Format offered: `SPA_VIDEO_FORMAT_BGRx`, `memfd` data type, size = source size, max framerate 30/1.

- [ ] **Step 1: Write failing test** `test_stream_delivers_frames`: start a private `pipewire` + `wireplumber` (or `pipewire` with the test session manager), create a stream on the Xvfb root, then run `gst-launch-1.0 pipewiresrc path=<node> num-buffers=5 ! video/x-raw,width=800,height=600 ! fakesink` and assert exit 0 within 10 s.
- [ ] **Step 2:** Run → FAIL.
- [ ] **Step 3: Implement** on a `pw_thread_loop`; on `process` dequeue a buffer, call `grab` into the mapped memfd, set chunk size/stride; XDamage events (via a GSource on the X connection fd on the main loop) mark the frame dirty; a 33 ms timer limits rate; a 1 s timer re-sends the last frame.
- [ ] **Step 4:** Run → PASS. Add `test_stream_destroy_releases_fds`: count `/proc/self/fd` before create and after destroy → equal.
- [ ] **Step 5: Commit** `screencast: PipeWire stream fed from X11`

### Task 5: Portal interface and session

**Files:**
- Create: `src/screencast/screencast.c/.h`
- Modify: `src/xdg-desktop-portal-xapp.c` (init under `#ifdef HAVE_SCREENCAST` and `XFCE_MODE`), `src/meson.build`

**Interfaces:**
- Consumes: Task 4 stream API; `Session`/`Request` from `src/session.h`, `src/request.h`; picker from Task 6 (`screencast_dialog_run_async`, defined there; stub returns "all monitors" until Task 6).
- Produces: `gboolean screencast_init (GDBusConnection *, GError **)`; `ScreenCastSession` (subclass of `Session`) with fields `selected_types`, `cursor_mode`, `multiple`, `GList *streams`; its `close` vfunc destroys every stream.
- Properties: `AvailableSourceTypes` (computed per read: `1` or `1|2`), `AvailableCursorModes = 3`, `version = 4`.
- `Start` reply: `streams` as `a(ua{sv})` with `id` (string), `source_type` (u), `position` (ii), `size` (ii); `SelectSources` rejects `cursor_mode` outside {1,2} with an error response.

- [ ] **Step 1: Write failing test** in `tests/screencast/client.py` (GDBus via `python3-gi`): `test_properties` expects `AvailableCursorModes == 3`, `version == 4`, `AvailableSourceTypes == 1` under Xvfb (no compositor); `test_select_sources_rejects_metadata_cursor` (cursor_mode 4 → response code 2).
- [ ] **Step 2:** Run via `tests/screencast/smoke.sh` (Task 8 harness) → FAIL.
- [ ] **Step 3: Implement** the handlers following the `settings.c`/`inhibit.c` pattern; `Start` re-checks `screencast_compositing_active ()` and fails with response 2 if a WINDOW is selected without it; register a name-vanished watch on the caller so its sessions close.
- [ ] **Step 4:** Re-run → PASS. Add `test_client_vanishes_closes_session`: kill the client process, assert the session object path disappears and `ls /proc/<portal>/fd | wc -l` returns to baseline.
- [ ] **Step 5: Commit** `screencast: portal implementation`

### Task 6: Picker dialog

**Files:**
- Create: `src/screencast/screencastdialog.c/.h/.ui`; Modify: `xdg-desktop-portal-xapp.gresource.xml`, `po/POTFILES.in`

**Interfaces:**
- Produces: `void screencast_dialog_run_async (GtkWindow *parent, GPtrArray *sources, gboolean multiple, GAsyncReadyCallback cb, gpointer data)`; `GArray *screencast_dialog_run_finish (GAsyncResult *, GError **)` returning chosen `ScreenCastSource.id`s; cancelling returns `G_IO_ERROR_CANCELLED`. Parent is resolved with `ExternalWindow` as in `appchooser.c`.

- [ ] **Step 1: Write failing test** `test_dialog_cancel_gives_cancelled_error` and `test_dialog_single_select_returns_one_id` (programmatic: emit the activate/response signals under Xvfb).
- [ ] **Step 2:** Run → FAIL. **Step 3:** Implement with a `GtkListBox`, a "Share" button disabled until a row is selected, a monitor/window grouping header, and `multiple` → `GTK_SELECTION_MULTIPLE`. **Step 4:** Run → PASS.
- [ ] **Step 5: Commit** `screencast: source picker dialog`

### Task 7: Portal file and desktop registration

**Files:**
- Modify: `data/meson.build` (installed in Task 1; verify), `debian/xdg-desktop-portal-xapp.install`

- [ ] **Step 1:** Install into a staging prefix: `DESTDIR=/tmp/stage ninja -C build install`; confirm `usr/share/xdg-desktop-portal/portals/xapp-screencast.portal` exists with `UseIn=XFCE`.
- [ ] **Step 2:** Document that XFCE users may need `~/.config/xdg-desktop-portal/xfce-portals.conf` with `org.freedesktop.impl.portal.ScreenCast=xapp` if `xdg-desktop-portal-gtk` is also installed (it does not implement ScreenCast, so normally unnecessary); verify with `busctl --user introspect org.freedesktop.portal.Desktop /org/freedesktop/portal/desktop | grep ScreenCast`.
- [ ] **Step 3: Commit** `screencast: install portal file`

### Task 8: Smoke-test harness

**Files:**
- Create: `tests/screencast/smoke.sh`

- [ ] **Step 1:** Script starts `Xvfb :99`, `dbus-run-session`, `pipewire` + `wireplumber`, `xdg-desktop-portal`, then the portal with `-d xfce -v`; runs `client.py` (CreateSession → SelectSources MONITOR → Start with the picker auto-accepted via `XAPP_SCREENCAST_TEST_AUTOPICK=1`, a test-only env hook honoured only when set) → reads the `node_id` → runs `gst-launch-1.0 pipewiresrc ... num-buffers=5`.
- [ ] **Step 2:** Run `tests/screencast/smoke.sh` → exit 0 and prints `streams: 1, size: (800, 600)`.
- [ ] **Step 3:** Run it three times in a row (leak/teardown check) → all exit 0.
- [ ] **Step 4: Commit** `screencast: add smoke test`

### Task 9: Manual acceptance on real XFCE (gate before Debian)

**Files:** Create `docs/screencast-acceptance.md`

- [ ] **Step 1:** On a real XFCE X11 session with the built portal running (`-r -v`), tick each line in Firefox + `meet.google.com`:
  - Present → Entire screen (monitor 1), second monitor, A window (compositing on)
  - Cursor visible in the received video
  - Compositing off → window option absent
  - Stop sharing from Meet's button and from Firefox's indicator → portal logs session closed, no stray processes/fds
  - Close the shared window mid-share → share ends cleanly
  - Cancel the picker → Meet shows "permission denied", not a hang
  - `xrandr --output ... --mode` change mid-share → no crash
- [ ] **Step 2:** Sign-off by the user. Do not start Task 10 before this.

### Task 10: Port to the Ubuntu `resolute` package

**Files:** Modify (in a checkout of the Ubuntu source) `debian/control`, `debian/rules`, `debian/xdg-desktop-portal-xapp.install`; Create `debian/patches/0001-screencast-xfce.patch`, `debian/patches/series`; update `debian/changelog`.

- [ ] **Step 1:** `pull-lp-source xdg-desktop-portal-xapp resolute`; record the packaged version and whether it is ≥1.2.0 (our patch is made against it, rebasing if not).
- [ ] **Step 2:** Produce the patch: `git diff <packaged-base>..HEAD -- src/screencast data meson.build meson_options.txt src/meson.build src/xdg-desktop-portal-xapp.c po/POTFILES.in` into `debian/patches/0001-screencast-xfce.patch` (DEP-3 header: `Forwarded: <URL of the linuxmint PR>`), listed in `series`.
- [ ] **Step 3:** Add Build-Depends `libpipewire-0.3-dev, libx11-dev, libxext-dev, libxcomposite-dev, libxdamage-dev, libxfixes-dev, libxrandr-dev`; add `-Dscreencast=enabled` in `debian/rules` so a missing dependency fails the build instead of silently shipping without the feature; add `Depends: pipewire`, `Recommends: wireplumber`.
- [ ] **Step 4:** Build in a clean resolute chroot: `sbuild -d resolute` (or `pbuilder`); verify `dpkg -c` lists `xapp-screencast.portal` and `lintian` has no new errors.
- [ ] **Step 5:** Install the built `.deb` on the XFCE test machine and re-run Task 9's Meet checklist.
- [ ] **Step 6:** Open the upstream PR to `linuxmint/xdg-desktop-portal-xapp` (user submits) and note its URL in the patch's `Forwarded:` field.

## Self-Review

- Coverage: all seven grill-me decisions map to Global Constraints and Tasks 1–6; testing (smoke + manual) to Tasks 8–9; Debian port to Task 10; Review Focus items each have a test (Tasks 4, 5, 3, 9).
- Open assumption to confirm on first build: `xdg-desktop-portal` on the target XFCE session routes ScreenCast to `xapp` via the new `.portal` file (checked in Task 7 Step 2).
- Risk: window-capture correctness depends on xfwm4 offering a usable composite pixmap; Task 3 only fakes the compositing check, so real behaviour is verified in Task 9.
