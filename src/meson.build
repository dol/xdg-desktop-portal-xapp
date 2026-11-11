gnome = import('gnome')
pkg = import('pkgconfig')

top_srcdir = meson.project_source_root()

# Desktop Portal D-Bus interfaces

xdg_desktop_portal_dep = dependency('xdg-desktop-portal', version: '>= 1.5')
desktop_portal_interfaces_dir = xdg_desktop_portal_dep.get_variable(
  pkgconfig: 'interfaces_dir'
)

desktop_portal_dbus_interfaces = [
  desktop_portal_interfaces_dir / 'org.freedesktop.impl.portal.AppChooser.xml',
  desktop_portal_interfaces_dir / 'org.freedesktop.impl.portal.Background.xml',
  desktop_portal_interfaces_dir / 'org.freedesktop.impl.portal.Inhibit.xml',
  desktop_portal_interfaces_dir / 'org.freedesktop.impl.portal.Lockdown.xml',
  desktop_portal_interfaces_dir / 'org.freedesktop.impl.portal.Request.xml',
  desktop_portal_interfaces_dir / 'org.freedesktop.impl.portal.Session.xml',
  desktop_portal_interfaces_dir / 'org.freedesktop.impl.portal.Settings.xml',
  desktop_portal_interfaces_dir / 'org.freedesktop.impl.portal.Wallpaper.xml',
]
# This is a newer screenshot interface that has a version property
# ref https://github.com/flatpak/xdg-desktop-portal-gtk/commit/30ef5f8ed500c2a7f850ed2164d7ee201860a700
desktop_portal_dbus_interfaces += [
  top_srcdir / 'data' / 'org.freedesktop.impl.portal.Screenshot.xml',
]

# ScreenCast needs PipeWire and the X11 extension libraries
screencast_opt = get_option('screencast')
screencast_deps = [
  dependency('libpipewire-0.3', version: '>= 0.3.40', required: screencast_opt),
  dependency('x11', required: screencast_opt),
  dependency('xext', required: screencast_opt),
  dependency('xcomposite', required: screencast_opt),
  dependency('xdamage', required: screencast_opt),
  dependency('xfixes', required: screencast_opt),
  dependency('xrandr', required: screencast_opt),
]
have_screencast = true
foreach d : screencast_deps
  have_screencast = have_screencast and d.found()
endforeach

if have_screencast
  desktop_portal_dbus_interfaces += [
    desktop_portal_interfaces_dir / 'org.freedesktop.impl.portal.ScreenCast.xml',
  ]
endif

built_sources = gnome.gdbus_codegen(
  'xdg-desktop-portal-dbus',
  sources: desktop_portal_dbus_interfaces,
  interface_prefix: 'org.freedesktop.impl.portal.',
  namespace: 'XdpImpl',
)

# GResources
built_sources += gnome.compile_resources(
  'xdg-desktop-portal-xapp-resources',
  'xdg-desktop-portal-xapp.gresource.xml',
  c_name: '_xdg_desktop',
)

desktop_dbus_interfaces = files(
  top_srcdir / 'data' / 'org.freedesktop.ScreenSaver.xml',
  top_srcdir / 'data' / 'org.gnome.SessionManager.xml',
  top_srcdir / 'data' / 'org.cinnamon.Screenshot.xml',
  top_srcdir / 'data' / 'org.cinnamon.PortalHandlers.xml',
  top_srcdir / 'data' / 'org.cinnamon.ScreenSaver.xml',
  top_srcdir / 'data' / 'org.gnome.ScreenSaver.xml',
  top_srcdir / 'data' / 'org.mate.ScreenSaver.xml'
)

built_sources += gnome.gdbus_codegen(
  'common-dbus',
  sources: desktop_dbus_interfaces
)

config_entries = {
    'GETTEXT_PACKAGE' : '"@0@"'.format(meson.project_name()),
    'LOCALEDIR': '"@0@"'.format(prefix / get_option('localedir')),
    'PACKAGE_STRING': '"xdg-desktop-portal-xapp @0@"'.format(meson.project_version()),
    'G_LOG_DOMAIN': '"xdg-desktop-portal-xapp"'

}
config = configuration_data(config_entries)

gtk_x11_dep = dependency('gtk+-x11-3.0', required: false)
config.set('HAVE_GTK_X11', gtk_x11_dep.found())

gtk_wayland_dep = dependency('gtk+-wayland-3.0', required: false)
config.set('HAVE_GTK_WAYLAND', gtk_wayland_dep.found())

config.set('HAVE_SCREENCAST', have_screencast)

built_sources += configure_file(output: 'config.h', configuration: config)

deps = [
  meson.get_compiler('c').find_library('m'),
  dependency('glib-2.0', version: '>= 2.44'),
  dependency('gio-unix-2.0'),
  dependency('gtk+-3.0'),
  xdg_desktop_portal_dep,
]

sources = built_sources + files(
  'appchooser.c',
  'appchooserdialog.c',
  'appchooserrow.c',
  'background.c',
  'externalwindow.c',
  'inhibit.c',
  'lockdown.c',
  'request.c',
  'screenshot.c',
  'session.c',
  'settings.c',
  'utils.c',
  'wallpaper.c',
  'wallpaperdialog.c',
  'xdg-desktop-portal-xapp.c',
)

# Filled in as the screencast sources are added
screencast_sources = files(
  'screencast/capture-x11.c',
  'screencast/compositing.c',
  'screencast/stream.c',
  'screencast/screencast.c',
  'screencast/screencastdialog.c',
)

if have_screencast
  deps += screencast_deps
  sources += screencast_sources
endif

if gtk_x11_dep.found()
  deps += gtk_x11_dep
  sources += files('externalwindow-x11.c')
endif

if gtk_wayland_dep.found()
  deps += gtk_wayland_dep
  sources += files('externalwindow-wayland.c')
endif

executable(
  'xdg-desktop-portal-xapp',
  sources,
  dependencies: deps,
  install: true,
  install_dir: libexecdir,
)

if have_screencast
  xvfb_run = find_program('xvfb-run', required: false)
  if xvfb_run.found()
    test_capture = executable(
      'test-capture',
      files(top_srcdir / 'tests' / 'screencast' / 'test-capture.c')
        + files('screencast/capture-x11.c', 'screencast/compositing.c'),
      include_directories: include_directories('screencast', '.'),
      dependencies: [dependency('glib-2.0')] + screencast_deps,
    )
    test('capture', xvfb_run,
      args: ['-a', '-s', '-screen 0 800x600x24', test_capture])

    with_pipewire = files(top_srcdir / 'tests' / 'screencast' / 'with-pipewire.sh')
    test_stream = executable(
      'test-stream',
      files(top_srcdir / 'tests' / 'screencast' / 'test-stream.c')
        + files('screencast/capture-x11.c', 'screencast/stream.c'),
      include_directories: include_directories('screencast', '.'),
      dependencies: [dependency('glib-2.0'), dependency('gio-2.0')] + screencast_deps,
    )
    test('stream', xvfb_run,
      args: ['-a', '-s', '-screen 0 800x600x24', with_pipewire, test_stream],
      timeout: 120)

    test_dialog = executable(
      'test-dialog',
      files(top_srcdir / 'tests' / 'screencast' / 'test-dialog.c')
        + files('screencast/screencastdialog.c', 'screencast/capture-x11.c', 'externalwindow.c')
        + (gtk_x11_dep.found() ? files('externalwindow-x11.c') : [])
        + (gtk_wayland_dep.found() ? files('externalwindow-wayland.c') : []),
      include_directories: include_directories('screencast', '.'),
      dependencies: [dependency('gtk+-3.0'), dependency('gio-2.0')] + screencast_deps
        + (gtk_x11_dep.found() ? [gtk_x11_dep] : [])
        + (gtk_wayland_dep.found() ? [gtk_wayland_dep] : []),
    )
    test('dialog', xvfb_run,
      args: ['-a', '-s', '-screen 0 800x600x24', test_dialog])

    python = find_program('/usr/bin/python3', required: false)
    gdbus = find_program('gdbus', required: false)
    if python.found() and gdbus.found()
      test('portal', xvfb_run,
        args: ['-a', '-s', '-screen 0 800x600x24', with_pipewire,
               files(top_srcdir / 'tests' / 'screencast' / 'portal-test.sh'),
               meson.current_build_dir() / 'xdg-desktop-portal-xapp',
               files(top_srcdir / 'tests' / 'screencast' / 'client.py')],
        timeout: 180)
    endif
  endif
endif
