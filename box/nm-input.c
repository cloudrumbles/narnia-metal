/*
 * nm-input: turns input from the iPad into real input on the box.
 *
 * nm-relay forwards the host's "IN ..." control lines here as datagrams on
 * a unix socket. Keyboard and mouse go into the X display with XTEST;
 * gamepads become virtual Xbox 360 pads through /dev/uinput (SDL, Proton's
 * winebus and evdev all see them as ordinary controllers).
 *
 *   nm-input [-s socket] [-d display]     (default /tmp/narnia-input.sock, $DISPLAY)
 *
 * Messages (one per datagram, text):
 *   KEY <hid usage> <1|0>         keyboard key down/up (USB HID usage id)
 *   MREL <dx> <dy>                relative mouse motion, pixels
 *   MABS <x> <y>                  absolute pointer, 0..1 over the X screen
 *   MBTN <1|2|3> <1|0>            left/middle/right button
 *   WHEEL <steps>                 positive = up
 *   PAD <i> <buttons hex> <lx> <ly> <rx> <ry> <lt> <rt>
 *                                 axes -32768..32767 (y up = negative),
 *                                 triggers 0..255; button bits below
 *   PADOFF <i>                    controller i went away
 *
 * SPDX-License-Identifier: MIT
 */
#define _GNU_SOURCE
#include <X11/XKBlib.h>
#include <X11/Xlib.h>
#include <X11/extensions/XTest.h>
#include <X11/keysym.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/uinput.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#define MAX_PADS 4

/* PAD button bits, shared with the iPad app */
enum {
   PB_A = 1 << 0,
   PB_B = 1 << 1,
   PB_X = 1 << 2,
   PB_Y = 1 << 3,
   PB_LB = 1 << 4,
   PB_RB = 1 << 5,
   PB_BACK = 1 << 6,
   PB_START = 1 << 7,
   PB_GUIDE = 1 << 8,
   PB_LS = 1 << 9,
   PB_RS = 1 << 10,
   PB_UP = 1 << 11,
   PB_DOWN = 1 << 12,
   PB_LEFT = 1 << 13,
   PB_RIGHT = 1 << 14,
};

static Display *dpy;
static int pad_fd[MAX_PADS] = { -1, -1, -1, -1 };
static unsigned pad_buttons[MAX_PADS];
static bool uinput_warned;

static void
log_(const char *fmt, ...)
{
   char ts[32];
   time_t t = time(NULL);
   strftime(ts, sizeof(ts), "%H:%M:%S", localtime(&t));
   va_list ap;
   va_start(ap, fmt);
   fprintf(stderr, "%s nm-input: ", ts);
   vfprintf(stderr, fmt, ap);
   fputc('\n', stderr);
   va_end(ap);
}

/* ---- keyboard: USB HID usage -> X keysym ---- */

static KeySym
hid_to_keysym(unsigned usage)
{
   if (usage >= 0x04 && usage <= 0x1d)
      return XK_a + (usage - 0x04);
   if (usage >= 0x1e && usage <= 0x26)
      return XK_1 + (usage - 0x1e);
   if (usage >= 0x3a && usage <= 0x45)
      return XK_F1 + (usage - 0x3a);
   switch (usage) {
   case 0x27: return XK_0;
   case 0x28: return XK_Return;
   case 0x29: return XK_Escape;
   case 0x2a: return XK_BackSpace;
   case 0x2b: return XK_Tab;
   case 0x2c: return XK_space;
   case 0x2d: return XK_minus;
   case 0x2e: return XK_equal;
   case 0x2f: return XK_bracketleft;
   case 0x30: return XK_bracketright;
   case 0x31: return XK_backslash;
   case 0x33: return XK_semicolon;
   case 0x34: return XK_apostrophe;
   case 0x35: return XK_grave;
   case 0x36: return XK_comma;
   case 0x37: return XK_period;
   case 0x38: return XK_slash;
   case 0x39: return XK_Caps_Lock;
   case 0x46: return XK_Print;
   case 0x47: return XK_Scroll_Lock;
   case 0x48: return XK_Pause;
   case 0x49: return XK_Insert;
   case 0x4a: return XK_Home;
   case 0x4b: return XK_Prior;
   case 0x4c: return XK_Delete;
   case 0x4d: return XK_End;
   case 0x4e: return XK_Next;
   case 0x4f: return XK_Right;
   case 0x50: return XK_Left;
   case 0x51: return XK_Down;
   case 0x52: return XK_Up;
   case 0x53: return XK_Num_Lock;
   case 0x54: return XK_KP_Divide;
   case 0x55: return XK_KP_Multiply;
   case 0x56: return XK_KP_Subtract;
   case 0x57: return XK_KP_Add;
   case 0x58: return XK_KP_Enter;
   case 0x59: return XK_KP_1;
   case 0x5a: return XK_KP_2;
   case 0x5b: return XK_KP_3;
   case 0x5c: return XK_KP_4;
   case 0x5d: return XK_KP_5;
   case 0x5e: return XK_KP_6;
   case 0x5f: return XK_KP_7;
   case 0x60: return XK_KP_8;
   case 0x61: return XK_KP_9;
   case 0x62: return XK_KP_0;
   case 0x63: return XK_KP_Decimal;
   case 0xe0: return XK_Control_L;
   case 0xe1: return XK_Shift_L;
   case 0xe2: return XK_Alt_L;
   case 0xe3: return XK_Super_L;
   case 0xe4: return XK_Control_R;
   case 0xe5: return XK_Shift_R;
   case 0xe6: return XK_Alt_R;
   case 0xe7: return XK_Super_R;
   default: return NoSymbol;
   }
}

static void
do_key(unsigned usage, bool down)
{
   const KeySym sym = hid_to_keysym(usage);
   if (sym == NoSymbol)
      return;
   const KeyCode code = XKeysymToKeycode(dpy, sym);
   if (!code)
      return;
   XTestFakeKeyEvent(dpy, code, down, CurrentTime);
}

/* ---- gamepads: virtual Xbox 360 pads via uinput ---- */

static bool
pad_create(int i)
{
   int fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK);
   if (fd < 0) {
      if (!uinput_warned)
         log_("no /dev/uinput (%s): controllers disabled", strerror(errno));
      uinput_warned = true;
      return false;
   }

   ioctl(fd, UI_SET_EVBIT, EV_KEY);
   static const int keys[] = { BTN_A, BTN_B, BTN_X, BTN_Y, BTN_TL, BTN_TR,
                               BTN_SELECT, BTN_START, BTN_MODE, BTN_THUMBL,
                               BTN_THUMBR };
   for (unsigned k = 0; k < sizeof(keys) / sizeof(keys[0]); k++)
      ioctl(fd, UI_SET_KEYBIT, keys[k]);

   ioctl(fd, UI_SET_EVBIT, EV_ABS);
   struct uinput_abs_setup abs[] = {
      { ABS_X, { .minimum = -32768, .maximum = 32767, .flat = 128, .fuzz = 16 } },
      { ABS_Y, { .minimum = -32768, .maximum = 32767, .flat = 128, .fuzz = 16 } },
      { ABS_RX, { .minimum = -32768, .maximum = 32767, .flat = 128, .fuzz = 16 } },
      { ABS_RY, { .minimum = -32768, .maximum = 32767, .flat = 128, .fuzz = 16 } },
      { ABS_Z, { .minimum = 0, .maximum = 255 } },
      { ABS_RZ, { .minimum = 0, .maximum = 255 } },
      { ABS_HAT0X, { .minimum = -1, .maximum = 1 } },
      { ABS_HAT0Y, { .minimum = -1, .maximum = 1 } },
   };
   for (unsigned a = 0; a < sizeof(abs) / sizeof(abs[0]); a++) {
      ioctl(fd, UI_SET_ABSBIT, abs[a].code);
      ioctl(fd, UI_ABS_SETUP, &abs[a]);
   }

   struct uinput_setup setup = {
      .id = { .bustype = BUS_USB, .vendor = 0x045e, .product = 0x028e,
              .version = 0x0110 },
   };
   snprintf(setup.name, sizeof(setup.name),
            "Microsoft X-Box 360 pad (narnia %d)", i);
   if (ioctl(fd, UI_DEV_SETUP, &setup) || ioctl(fd, UI_DEV_CREATE)) {
      log_("uinput device setup failed: %s", strerror(errno));
      close(fd);
      return false;
   }
   pad_fd[i] = fd;
   pad_buttons[i] = 0;
   log_("controller %d connected (virtual Xbox 360 pad)", i);
   return true;
}

static void
emit(int fd, int type, int code, int value)
{
   struct input_event ev = { .type = type, .code = code, .value = value };
   if (write(fd, &ev, sizeof(ev)) < 0) {
      /* nothing useful to do; the next state update will retry */
   }
}

static void
do_pad(int i, unsigned buttons, int lx, int ly, int rx, int ry, int lt, int rt)
{
   if (i < 0 || i >= MAX_PADS)
      return;
   if (pad_fd[i] < 0 && !pad_create(i))
      return;
   const int fd = pad_fd[i];

   static const struct { unsigned bit; int code; } map[] = {
      { PB_A, BTN_A }, { PB_B, BTN_B }, { PB_X, BTN_X }, { PB_Y, BTN_Y },
      { PB_LB, BTN_TL }, { PB_RB, BTN_TR }, { PB_BACK, BTN_SELECT },
      { PB_START, BTN_START }, { PB_GUIDE, BTN_MODE }, { PB_LS, BTN_THUMBL },
      { PB_RS, BTN_THUMBR },
   };
   for (unsigned k = 0; k < sizeof(map) / sizeof(map[0]); k++) {
      if ((buttons ^ pad_buttons[i]) & map[k].bit)
         emit(fd, EV_KEY, map[k].code, !!(buttons & map[k].bit));
   }
   pad_buttons[i] = buttons;

   emit(fd, EV_ABS, ABS_X, lx);
   emit(fd, EV_ABS, ABS_Y, ly);
   emit(fd, EV_ABS, ABS_RX, rx);
   emit(fd, EV_ABS, ABS_RY, ry);
   emit(fd, EV_ABS, ABS_Z, lt);
   emit(fd, EV_ABS, ABS_RZ, rt);
   emit(fd, EV_ABS, ABS_HAT0X,
        (buttons & PB_RIGHT ? 1 : 0) - (buttons & PB_LEFT ? 1 : 0));
   emit(fd, EV_ABS, ABS_HAT0Y,
        (buttons & PB_DOWN ? 1 : 0) - (buttons & PB_UP ? 1 : 0));
   emit(fd, EV_SYN, SYN_REPORT, 0);
}

static void
pad_destroy(int i)
{
   if (i < 0 || i >= MAX_PADS || pad_fd[i] < 0)
      return;
   ioctl(pad_fd[i], UI_DEV_DESTROY);
   close(pad_fd[i]);
   pad_fd[i] = -1;
   log_("controller %d disconnected", i);
}

/* ---- dispatch ---- */

static void
handle(char *msg)
{
   int a, b, c, d, e, f, g;
   unsigned u;
   float x, y;

   if (sscanf(msg, "KEY %u %d", &u, &a) == 2) {
      do_key(u, a);
   } else if (sscanf(msg, "MREL %d %d", &a, &b) == 2) {
      XTestFakeRelativeMotionEvent(dpy, a, b, CurrentTime);
   } else if (sscanf(msg, "MABS %f %f", &x, &y) == 2) {
      const int scr = DefaultScreen(dpy);
      const int w = DisplayWidth(dpy, scr), h = DisplayHeight(dpy, scr);
      XTestFakeMotionEvent(dpy, scr, (int)(x * (w - 1) + 0.5f),
                           (int)(y * (h - 1) + 0.5f), CurrentTime);
   } else if (sscanf(msg, "MBTN %d %d", &a, &b) == 2) {
      if (a >= 1 && a <= 3)
         XTestFakeButtonEvent(dpy, a, b, CurrentTime);
   } else if (sscanf(msg, "WHEEL %d", &a) == 1) {
      const int button = a > 0 ? 4 : 5;
      for (int n = a > 0 ? a : -a; n > 0; n--) {
         XTestFakeButtonEvent(dpy, button, True, CurrentTime);
         XTestFakeButtonEvent(dpy, button, False, CurrentTime);
      }
   } else if (sscanf(msg, "PAD %d %x %d %d %d %d %d %d", &a, &u, &b, &c, &d,
                     &e, &f, &g) == 8) {
      do_pad(a, u, b, c, d, e, f, g);
   } else if (sscanf(msg, "PADOFF %d", &a) == 1) {
      pad_destroy(a);
   } else {
      return;
   }
   XFlush(dpy);
}

int
main(int argc, char **argv)
{
   const char *sock_path = "/tmp/narnia-input.sock";
   const char *display = NULL;
   int opt;
   while ((opt = getopt(argc, argv, "s:d:")) != -1) {
      switch (opt) {
      case 's': sock_path = optarg; break;
      case 'd': display = optarg; break;
      default:
         fprintf(stderr, "usage: %s [-s socket] [-d display]\n", argv[0]);
         return 1;
      }
   }
   setvbuf(stderr, NULL, _IOLBF, 0);

   dpy = XOpenDisplay(display);
   if (!dpy) {
      log_("can't open X display %s", display ? display : getenv("DISPLAY"));
      return 1;
   }
   int ev, err, major, minor;
   if (!XTestQueryExtension(dpy, &ev, &err, &major, &minor)) {
      log_("X server has no XTEST");
      return 1;
   }

   unlink(sock_path);
   int fd = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0);
   struct sockaddr_un un = { .sun_family = AF_UNIX };
   snprintf(un.sun_path, sizeof(un.sun_path), "%s", sock_path);
   if (fd < 0 || bind(fd, (struct sockaddr *)&un, sizeof(un))) {
      log_("bind %s: %s", sock_path, strerror(errno));
      return 1;
   }
   log_("listening on %s, display %s", sock_path, DisplayString(dpy));

   char buf[512];
   while (true) {
      ssize_t n = recv(fd, buf, sizeof(buf) - 1, 0);
      if (n < 0) {
         if (errno == EINTR)
            continue;
         log_("recv: %s", strerror(errno));
         return 1;
      }
      buf[n] = '\0';
      handle(buf);
   }
}
