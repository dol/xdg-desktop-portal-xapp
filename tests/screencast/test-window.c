/* Test helper: shows one window that looks like a client window to the
 * portal (listed in _NET_CLIENT_LIST, compositing manager selection owned),
 * destroys it when told to. Prints "ready" and "closed" on stdout; reads a
 * line from stdin to close the window. */
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <X11/Xatom.h>
#include <X11/Xlib.h>

int
main (void)
{
  Display *dpy = XOpenDisplay (NULL);
  Window root, win, owner;
  const char *title = "closeme";
  char line[64];

  if (!dpy)
    return 1;

  root = DefaultRootWindow (dpy);
  win = XCreateSimpleWindow (dpy, root, 20, 20, 300, 200, 0, 0, 0x00ff00);
  owner = XCreateSimpleWindow (dpy, root, 0, 0, 1, 1, 0, 0, 0);

  XChangeProperty (dpy, win, XInternAtom (dpy, "_NET_WM_NAME", False),
                   XInternAtom (dpy, "UTF8_STRING", False), 8, PropModeReplace,
                   (const unsigned char *) title, strlen (title));
  XMapWindow (dpy, win);
  XSetSelectionOwner (dpy, XInternAtom (dpy, "_NET_WM_CM_S0", False), owner, CurrentTime);
  XChangeProperty (dpy, root, XInternAtom (dpy, "_NET_CLIENT_LIST", False), XA_WINDOW, 32,
                   PropModeReplace, (const unsigned char *) &win, 1);
  XSync (dpy, False);

  printf ("ready\n");
  fflush (stdout);

  if (fgets (line, sizeof (line), stdin))
    {
      XDestroyWindow (dpy, win);
      XDeleteProperty (dpy, root, XInternAtom (dpy, "_NET_CLIENT_LIST", False));
      XSync (dpy, False);
      printf ("closed\n");
      fflush (stdout);
    }

  /* Keep the compositing selection until the test is over */
  fgets (line, sizeof (line), stdin);
  return 0;
}
