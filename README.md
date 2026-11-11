# xdg-desktop-portal-xapp

A backend implementation for [xdg-desktop-portal](http://github.com/flatpak/xdg-desktop-portal)
that is using GTK and various pieces of Cinnamon/MATE/Xfce4 infrastructure.

## Screen sharing (Xfce, X11)

On Xfce the backend implements `org.freedesktop.portal.ScreenCast`, so browsers
(Firefox, Chromium) can share a monitor or a window, for example in Google Meet.
It captures the X11 screen itself and publishes it as a PipeWire stream.

Requirements:

* an X11 session (Wayland is not supported)
* PipeWire and a session manager such as WirePlumber
* for sharing a single window, a running compositor (xfwm4 compositing enabled);
  without one only whole monitors are offered

The feature is built when PipeWire and the X11 extension libraries
(`xcomposite`, `xdamage`, `xfixes`, `xrandr`) are found. Use
`-Dscreencast=enabled` to make a missing dependency a build error, or
`-Dscreencast=disabled` to leave it out.

Cursor modes `hidden` and `embedded` are supported. Choices are not remembered
(no `restore_token`), so the picker is shown every time.
