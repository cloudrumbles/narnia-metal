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
 * The iPad repeats each pad's state 4 times a second; a pad that hears
 * nothing for a second goes back to neutral, so a dropped connection
 * can't leave a stick held (same rule as narnia's sdl-vpad).
 *
 * SPDX-License-Identifier: MIT
 */
#define _GNU_SOURCE
#include <X11/Xlib.h>
#include <X11/extensions/XTest.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/uinput.h>
#include <poll.h>
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
static struct timespec pad_last[MAX_PADS];
static bool pad_neutral[MAX_PADS];
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

/* ---- keyboard: USB HID usage -> evdev key code ----
 *
 * Physical keys, not symbols, like narnia's scancode path: the X server
 * (Xvfb/Xorg with the evdev keymap) uses keycode = evdev code + 8, so the
 * box's keyboard layout decides what a key means, exactly as with a local
 * USB keyboard.
 */
static const unsigned char hid_to_evdev[256] = {
   [0x04] = KEY_A, [0x05] = KEY_B, [0x06] = KEY_C, [0x07] = KEY_D,
   [0x08] = KEY_E, [0x09] = KEY_F, [0x0a] = KEY_G, [0x0b] = KEY_H,
   [0x0c] = KEY_I, [0x0d] = KEY_J, [0x0e] = KEY_K, [0x0f] = KEY_L,
   [0x10] = KEY_M, [0x11] = KEY_N, [0x12] = KEY_O, [0x13] = KEY_P,
   [0x14] = KEY_Q, [0x15] = KEY_R, [0x16] = KEY_S, [0x17] = KEY_T,
   [0x18] = KEY_U, [0x19] = KEY_V, [0x1a] = KEY_W, [0x1b] = KEY_X,
   [0x1c] = KEY_Y, [0x1d] = KEY_Z, [0x1e] = KEY_1, [0x1f] = KEY_2,
   [0x20] = KEY_3, [0x21] = KEY_4, [0x22] = KEY_5, [0x23] = KEY_6,
   [0x24] = KEY_7, [0x25] = KEY_8, [0x26] = KEY_9, [0x27] = KEY_0,
   [0x28] = KEY_ENTER, [0x29] = KEY_ESC, [0x2a] = KEY_BACKSPACE,
   [0x2b] = KEY_TAB, [0x2c] = KEY_SPACE, [0x2d] = KEY_MINUS,
   [0x2e] = KEY_EQUAL, [0x2f] = KEY_LEFTBRACE, [0x30] = KEY_RIGHTBRACE,
   [0x31] = KEY_BACKSLASH, [0x32] = KEY_BACKSLASH, [0x33] = KEY_SEMICOLON,
   [0x34] = KEY_APOSTROPHE, [0x35] = KEY_GRAVE, [0x36] = KEY_COMMA,
   [0x37] = KEY_DOT, [0x38] = KEY_SLASH, [0x39] = KEY_CAPSLOCK,
   [0x3a] = KEY_F1, [0x3b] = KEY_F2, [0x3c] = KEY_F3, [0x3d] = KEY_F4,
   [0x3e] = KEY_F5, [0x3f] = KEY_F6, [0x40] = KEY_F7, [0x41] = KEY_F8,
   [0x42] = KEY_F9, [0x43] = KEY_F10, [0x44] = KEY_F11, [0x45] = KEY_F12,
   [0x46] = KEY_SYSRQ, [0x47] = KEY_SCROLLLOCK, [0x48] = KEY_PAUSE,
   [0x49] = KEY_INSERT, [0x4a] = KEY_HOME, [0x4b] = KEY_PAGEUP,
   [0x4c] = KEY_DELETE, [0x4d] = KEY_END, [0x4e] = KEY_PAGEDOWN,
   [0x4f] = KEY_RIGHT, [0x50] = KEY_LEFT, [0x51] = KEY_DOWN, [0x52] = KEY_UP,
   [0x53] = KEY_NUMLOCK, [0x54] = KEY_KPSLASH, [0x55] = KEY_KPASTERISK,
   [0x56] = KEY_KPMINUS, [0x57] = KEY_KPPLUS, [0x58] = KEY_KPENTER,
   [0x59] = KEY_KP1, [0x5a] = KEY_KP2, [0x5b] = KEY_KP3, [0x5c] = KEY_KP4,
   [0x5d] = KEY_KP5, [0x5e] = KEY_KP6, [0x5f] = KEY_KP7, [0x60] = KEY_KP8,
   [0x61] = KEY_KP9, [0x62] = KEY_KP0, [0x63] = KEY_KPDOT,
   [0x64] = KEY_102ND, [0x65] = KEY_COMPOSE,
   [0xe0] = KEY_LEFTCTRL, [0xe1] = KEY_LEFTSHIFT, [0xe2] = KEY_LEFTALT,
   [0xe3] = KEY_LEFTMETA, [0xe4] = KEY_RIGHTCTRL, [0xe5] = KEY_RIGHTSHIFT,
   [0xe6] = KEY_RIGHTALT, [0xe7] = KEY_RIGHTMETA,
};

static void
do_key(unsigned usage, bool down)
{
   if (usage >= 256 || !hid_to_evdev[usage])
      return;
   XTestFakeKeyEvent(dpy, hid_to_evdev[usage] + 8, down, CurrentTime);
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
   clock_gettime(CLOCK_MONOTONIC, &pad_last[i]);
   pad_neutral[i] = !buttons && !lx && !ly && !rx && !ry && !lt && !rt;

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

/* pads that went quiet for a second return to neutral */
static void
pad_timeouts(void)
{
   struct timespec now;
   clock_gettime(CLOCK_MONOTONIC, &now);
   for (int i = 0; i < MAX_PADS; i++) {
      if (pad_fd[i] < 0 || pad_neutral[i])
         continue;
      const double idle = (now.tv_sec - pad_last[i].tv_sec) +
                          (now.tv_nsec - pad_last[i].tv_nsec) / 1e9;
      if (idle > 1.0) {
         log_("controller %d: no updates for a second, centering", i);
         do_pad(i, 0, 0, 0, 0, 0, 0, 0);
      }
   }
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
      struct pollfd pfd = { .fd = fd, .events = POLLIN };
      if (poll(&pfd, 1, 250) <= 0) {
         pad_timeouts();
         continue;
      }
      ssize_t n = recv(fd, buf, sizeof(buf) - 1, 0);
      if (n < 0) {
         if (errno == EINTR)
            continue;
         log_("recv: %s", strerror(errno));
         return 1;
      }
      buf[n] = '\0';
      handle(buf);
      pad_timeouts();
   }
}
