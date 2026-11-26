/*
 * Narnia Metal: the iPad host. Connects out to nm-relay on a box, runs the
 * box's Vulkan streams on vkr + MoltenVK, and shows presented frames full
 * screen.
 *
 * UI: a small connect panel (relay host, port, token). Tap with three
 * fingers to bring it back while streaming.
 *
 * SPDX-License-Identifier: MIT
 */
#import <GameController/GameController.h>
#import <QuartzCore/CAMetalLayer.h>
#import <UIKit/UIKit.h>
#include <execinfo.h>
#include <fcntl.h>
#include <mach/mach.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <sys/utsname.h>
#include <unistd.h>

#include "nm_audio.h"
#include "nm_audio_ios.h"
#import "nm_ane_test.h"
#import "nm_stream.h"
#import "nm_coreml_sweep.h"
#include "nm_driver.h"
#include "nm_host.h"
#include "nm_presenter_metal.h"
#include "nm_relay_client.h"

@class NMViewController;
static NMViewController *g_vc;
struct session;

/* ------------------------------------------------------------------ */

@interface NMViewController
   : UIViewController <UITextFieldDelegate, UIGestureRecognizerDelegate>
@property(nonatomic, strong) UIView *panel;
@property(nonatomic, strong) UITextField *hostField;
@property(nonatomic, strong) UITextField *portField;
@property(nonatomic, strong) UITextField *tokenField;
@property(nonatomic, strong) UIButton *connectButton;
@property(nonatomic, strong) UILabel *statusLabel;
@property(nonatomic, strong) UITextView *logView;
@property(nonatomic, strong) UILabel *statsLabel;
@property(nonatomic, strong) UIButton *aneButton;
@property(nonatomic, strong) UISegmentedControl *modeControl;
@property(nonatomic, strong) UISegmentedControl *upscaleControl;
@property(nonatomic, strong) NMStream *stream;
@property(nonatomic, strong) CAMetalLayer *streamLayer;
@property(nonatomic, copy) NSString *declinedBox;
@property(nonatomic, copy) NSString *lastAutoBox;
@property(nonatomic, assign) CFTimeInterval lastAutoAt;
@property(nonatomic, assign) BOOL autoChecking;
@property(nonatomic, assign) NSUInteger statsTick;
@property(nonatomic, strong) UITextField *sweepField;
@property(nonatomic, strong) UIButton *sweepButton;
@property(nonatomic, strong) NSTimer *statsTimer;
@property(nonatomic, assign) uint64_t lastFrames;
@property(nonatomic, assign) uint64_t lastRx;
@property(nonatomic, assign) CFTimeInterval lastStatsTime;
@property(nonatomic, assign) float mouseRemX;
@property(nonatomic, assign) float mouseRemY;
@property(nonatomic, assign) BOOL running;
- (void)sessionEnded:(struct session *)s;
- (void)updateInputState;
- (BOOL)streamHasInput;
- (void)setStatus:(NSString *)text;
- (void)appendLog:(NSString *)text;
- (void)openStreamURL:(NSString *)url hash:(NSString *)hash;
@end

/* ---- logs: everything written to stderr (ours, vkr's, MoltenVK's) goes
 * to the real stderr, the on-screen log and the box (telemetry) ---- */

static int g_real_stderr = -1;
static NSString *const kTelemetryBase = @"http://100.80.160.41:7696";
static NSMutableArray<NSString *> *g_stream_log;

static void
keep_stream_log(NSString *line)
{
   if (![line hasPrefix:@"stream"])
      return;
   @synchronized(g_stream_log) {
      if (g_stream_log.count < 200)
         [g_stream_log addObject:line];
   }
}

static NSArray<NSString *> *
take_stream_log(void)
{
   @synchronized(g_stream_log) {
      NSArray<NSString *> *lines = [g_stream_log copy];
      [g_stream_log removeAllObjects];
      return lines;
   }
}

static void *
stderr_pump(void *arg)
{
   int fd = (int)(intptr_t)arg;
   FILE *in = fdopen(fd, "r");
   char line[2048];
   while (fgets(line, sizeof(line), in)) {
      if (g_real_stderr >= 0)
         write(g_real_stderr, line, strlen(line));
      size_t n = strlen(line);
      while (n && (line[n - 1] == '\n' || line[n - 1] == '\r'))
         line[--n] = '\0';
      if (!n)
         continue;
      nm_relay_client_log(line);
      NSString *s = [NSString stringWithUTF8String:line];
      if (s)
         keep_stream_log(s);
      if (s) {
         dispatch_async(dispatch_get_main_queue(), ^{
            [g_vc appendLog:s];
         });
      }
   }
   return NULL;
}

/* ---- crash reports: a fatal signal writes a backtrace to a file, which
 * goes to the box as telemetry on the next launch ---- */

static char g_crash_path[1024];

static void
crash_handler(int sig)
{
   int fd = open(g_crash_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
   if (fd >= 0) {
      char head[64];
      int n = snprintf(head, sizeof(head), "crash: signal %d\n", sig);
      write(fd, head, n);
      void *frames[64];
      int count = backtrace(frames, 64);
      backtrace_symbols_fd(frames, count, fd);
      close(fd);
   }
   /* also into the live log, in case the connection is still up */
   void *frames[64];
   int count = backtrace(frames, 64);
   backtrace_symbols_fd(frames, count, STDERR_FILENO);
   signal(sig, SIG_DFL);
   raise(sig);
}

static void
crash_exception(NSException *e)
{
   NSString *s = [NSString stringWithFormat:@"crash: exception %@: %@\n%@\n", e.name,
                                            e.reason, e.callStackSymbols];
   [s writeToFile:@(g_crash_path) atomically:NO encoding:NSUTF8StringEncoding error:nil];
}

static void
install_crash_reporting(void)
{
   NSString *docs =
      NSSearchPathForDirectoriesInDomains(NSDocumentDirectory, NSUserDomainMask, YES)
         .firstObject;
   snprintf(g_crash_path, sizeof(g_crash_path), "%s/last-crash.txt", docs.UTF8String);

   /* report the previous run's crash (logged before connecting: it goes out
    * with the backlog as soon as the relay connection is up) */
   NSString *last = [NSString stringWithContentsOfFile:@(g_crash_path)
                                              encoding:NSUTF8StringEncoding
                                                 error:nil];
   if (last.length) {
      fprintf(stderr, "previous run crashed:\n%s\n", last.UTF8String);
      [NSFileManager.defaultManager removeItemAtPath:@(g_crash_path) error:nil];
   }

   const int sigs[] = { SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT, SIGTRAP };
   for (unsigned i = 0; i < sizeof(sigs) / sizeof(sigs[0]); i++)
      signal(sigs[i], crash_handler);
   NSSetUncaughtExceptionHandler(crash_exception);
}

static void
capture_stderr(void)
{
   int fds[2];
   if (pipe(fds))
      return;
   g_real_stderr = dup(STDERR_FILENO);
   dup2(fds[1], STDERR_FILENO);
   close(fds[1]);
   setvbuf(stderr, NULL, _IOLBF, 0);
   pthread_t t;
   pthread_create(&t, NULL, stderr_pump, (void *)(intptr_t)fds[0]);
   pthread_detach(t);
}

/* ---- C callbacks from the host core (any thread) ---- */

static void
on_log(const char *msg)
{
   fprintf(stderr, "%s\n", msg);
}

static void
on_status(void *user, const char *msg)
{
   fprintf(stderr, "relay: %s\n", msg);
   NSString *s = [NSString stringWithUTF8String:msg];
   dispatch_async(dispatch_get_main_queue(), ^{
      [g_vc setStatus:s];
   });
}

static void *
create_layer(void *user, uint32_t width, uint32_t height)
{
   __block void *result = NULL;
   dispatch_sync(dispatch_get_main_queue(), ^{
      CAMetalLayer *layer = [CAMetalLayer layer];
      layer.frame = g_vc.view.layer.bounds;
      layer.contentsGravity = kCAGravityResizeAspect;
      layer.backgroundColor = UIColor.blackColor.CGColor;
      layer.opaque = YES;
      layer.drawableSize = CGSizeMake(width, height);
      /* below the panel */
      [g_vc.view.layer insertSublayer:layer below:g_vc.panel.layer];
      result = (__bridge_retained void *)layer;
   });
   return result;
}

static void
destroy_layer(void *user, void *layer_ptr)
{
   CAMetalLayer *layer = (__bridge_transfer CAMetalLayer *)layer_ptr;
   dispatch_async(dispatch_get_main_queue(), ^{
      [layer removeFromSuperlayer];
   });
}

static void
first_frame(void *user, uint32_t width, uint32_t height)
{
   dispatch_async(dispatch_get_main_queue(), ^{
      [g_vc setStatus:[NSString stringWithFormat:@"streaming %ux%u", width,
                                                 height]];
      g_vc.panel.hidden = YES;
      [g_vc updateInputState];
   });
}

static const struct nm_metal_layer_ops layer_ops = {
   .create_layer = create_layer,
   .destroy_layer = destroy_layer,
   .first_frame = first_frame,
};

/* ---- input: controllers, keyboard, mouse, touch -> box (nm-input) ---- */

/* PAD button bits, see box/nm-input.c */
enum {
   PB_A = 1 << 0, PB_B = 1 << 1, PB_X = 1 << 2, PB_Y = 1 << 3,
   PB_LB = 1 << 4, PB_RB = 1 << 5, PB_BACK = 1 << 6, PB_START = 1 << 7,
   PB_GUIDE = 1 << 8, PB_LS = 1 << 9, PB_RS = 1 << 10, PB_UP = 1 << 11,
   PB_DOWN = 1 << 12, PB_LEFT = 1 << 13, PB_RIGHT = 1 << 14,
};

static void
send_input(NSString *msg)
{
   nm_relay_client_input(msg.UTF8String);
}

static int
axis16(float v)
{
   if (v > 1)
      v = 1;
   if (v < -1)
      v = -1;
   return (int)(v * 32767.0f);
}

static void
send_stream_pad(GCExtendedGamepad *g)
{
   uint32_t b = 0;
   const struct {
      GCControllerButtonInput *button;
      uint32_t bit;
   } map[] = {
      {g.buttonA, 1u << 0},          {g.buttonB, 1u << 1},
      {g.buttonX, 1u << 2},          {g.buttonY, 1u << 3},
      {g.buttonOptions, 1u << 4},    {g.buttonHome, 1u << 5},
      {g.buttonMenu, 1u << 6},       {g.leftThumbstickButton, 1u << 7},
      {g.rightThumbstickButton, 1u << 8}, {g.leftShoulder, 1u << 9},
      {g.rightShoulder, 1u << 10},   {g.dpad.up, 1u << 11},
      {g.dpad.down, 1u << 12},       {g.dpad.left, 1u << 13},
      {g.dpad.right, 1u << 14},
   };
   for (size_t i = 0; i < sizeof(map) / sizeof(map[0]); i++)
      if (map[i].button.isPressed)
         b |= map[i].bit;
   const int16_t axes[6] = {
      (int16_t)axis16(g.leftThumbstick.xAxis.value),
      (int16_t)axis16(-g.leftThumbstick.yAxis.value),
      (int16_t)axis16(g.rightThumbstick.xAxis.value),
      (int16_t)axis16(-g.rightThumbstick.yAxis.value),
      (int16_t)(g.leftTrigger.value * 65535.0f - 32768.0f),
      (int16_t)(g.rightTrigger.value * 65535.0f - 32768.0f),
   };
   [g_vc.stream padButtons:b axes:axes];
}

static void
send_pad(GCController *c)
{
   GCExtendedGamepad *g = c.extendedGamepad;
   if (!g)
      return;
   if (g_vc.stream) {
      send_stream_pad(g);
      return;
   }
   unsigned b = 0;
#define BIT(elem, bit)        \
   if ((elem).isPressed)      \
      b |= (bit)
   BIT(g.buttonA, PB_A);
   BIT(g.buttonB, PB_B);
   BIT(g.buttonX, PB_X);
   BIT(g.buttonY, PB_Y);
   BIT(g.leftShoulder, PB_LB);
   BIT(g.rightShoulder, PB_RB);
   if (g.buttonOptions)
      BIT(g.buttonOptions, PB_BACK);
   BIT(g.buttonMenu, PB_START);
   if (g.buttonHome)
      BIT(g.buttonHome, PB_GUIDE);
   if (g.leftThumbstickButton)
      BIT(g.leftThumbstickButton, PB_LS);
   if (g.rightThumbstickButton)
      BIT(g.rightThumbstickButton, PB_RS);
   BIT(g.dpad.up, PB_UP);
   BIT(g.dpad.down, PB_DOWN);
   BIT(g.dpad.left, PB_LEFT);
   BIT(g.dpad.right, PB_RIGHT);
#undef BIT
   const int index = c.playerIndex >= 0 ? (int)c.playerIndex : 0;
   /* GameController's y is up; evdev pads report up as negative */
   send_input([NSString
      stringWithFormat:@"PAD %d %x %d %d %d %d %d %d", index, b,
                       axis16(g.leftThumbstick.xAxis.value),
                       axis16(-g.leftThumbstick.yAxis.value),
                       axis16(g.rightThumbstick.xAxis.value),
                       axis16(-g.rightThumbstick.yAxis.value),
                       (int)(g.leftTrigger.value * 255),
                       (int)(g.rightTrigger.value * 255)]);
}

/* ---- relay thread ---- */

/* One per Connect tap. The relay thread owns it; Disconnect only sets
 * stop, so an old thread can never pick up a newer session's settings.
 */
struct session {
   atomic_bool stop;
   char host[256];
   char token[128];
   struct nm_relay_client_config cfg;
};
static struct session *g_session;

@implementation NMViewController

- (BOOL)prefersStatusBarHidden {
   return YES;
}

- (BOOL)prefersHomeIndicatorAutoHidden {
   return YES;
}

/* capture the mouse while the stream is up and the panel is away */
- (BOOL)prefersPointerLocked {
   return self.running && self.panel.hidden;
}

- (BOOL)streamHasInput {
   return self.running && self.panel.hidden;
}

- (void)updateInputState {
   [self setNeedsUpdateOfPrefersPointerLocked];
}

- (void)setupController:(GCController *)c {
   if (!c.extendedGamepad)
      return;
   if (c.playerIndex == GCControllerPlayerIndexUnset) {
      NSInteger used = 0;
      for (GCController *o in GCController.controllers) {
         if (o != c && o.playerIndex >= 0)
            used |= 1 << o.playerIndex;
      }
      for (NSInteger i = 0; i < 4; i++) {
         if (!(used & (1 << i))) {
            c.playerIndex = (GCControllerPlayerIndex)i;
            break;
         }
      }
   }
   fprintf(stderr, "input: controller %ld: %s\n", (long)c.playerIndex,
           c.vendorName.UTF8String ?: "?");
   __weak GCController *weak = c;
   c.extendedGamepad.valueChangedHandler =
      ^(GCExtendedGamepad *g, GCControllerElement *e) {
         GCController *strong = weak;
         if (strong && [g_vc streamHasInput])
            send_pad(strong);
      };
}

- (void)setupKeyboard:(GCKeyboard *)kb {
   fprintf(stderr, "input: keyboard connected\n");
   kb.keyboardInput.keyChangedHandler =
      ^(GCKeyboardInput *k, GCControllerButtonInput *key, GCKeyCode code,
        BOOL pressed) {
         /* key codes are USB HID usages, which nm-input expects */
         if (![g_vc streamHasInput])
            return;
         if (g_vc.stream)
            [g_vc.stream keyHID:(uint32_t)code pressed:pressed];
         else
            send_input([NSString stringWithFormat:@"KEY %ld %d", (long)code,
                                                  pressed]);
      };
}

- (void)setupMouse:(GCMouse *)m {
   fprintf(stderr, "input: mouse connected\n");
   GCMouseInput *in = m.mouseInput;
   in.mouseMovedHandler = ^(GCMouseInput *mi, float dx, float dy) {
      if (![g_vc streamHasInput])
         return;
      if (g_vc.stream) {
         [g_vc.stream mouseDX:dx dy:-dy];
         return;
      }
      /* keep the fractions so slow movement isn't lost */
      g_vc.mouseRemX += dx;
      g_vc.mouseRemY -= dy; /* up is positive here, down is positive in X */
      const int ix = (int)g_vc.mouseRemX, iy = (int)g_vc.mouseRemY;
      if (!ix && !iy)
         return;
      g_vc.mouseRemX -= ix;
      g_vc.mouseRemY -= iy;
      send_input([NSString stringWithFormat:@"MREL %d %d", ix, iy]);
   };
   GCControllerButtonInput *buttons[3] = { in.leftButton, in.middleButton,
                                           in.rightButton };
   for (int i = 0; i < 3; i++) {
      const int number = i + 1;
      buttons[i].pressedChangedHandler =
         ^(GCControllerButtonInput *b, float v, BOOL pressed) {
            if (![g_vc streamHasInput])
               return;
            if (g_vc.stream)
               [g_vc.stream mouseButton:number pressed:pressed];
            else
               send_input([NSString stringWithFormat:@"MBTN %d %d", number,
                                                     pressed]);
         };
   }
   in.scroll.yAxis.valueChangedHandler = ^(GCControllerAxisInput *a, float v) {
      if (![g_vc streamHasInput] || v == 0)
         return;
      if (g_vc.stream)
         [g_vc.stream wheel:v > 0 ? 1 : -1];
      else
         send_input([NSString stringWithFormat:@"WHEEL %d", v > 0 ? 1 : -1]);
   };
}

- (void)setupInput {
   NSNotificationCenter *nc = NSNotificationCenter.defaultCenter;
   [nc addObserverForName:GCControllerDidConnectNotification
                   object:nil
                    queue:NSOperationQueue.mainQueue
               usingBlock:^(NSNotification *n) {
                  [self setupController:n.object];
               }];
   [nc addObserverForName:GCControllerDidDisconnectNotification
                   object:nil
                    queue:NSOperationQueue.mainQueue
               usingBlock:^(NSNotification *n) {
                  GCController *c = n.object;
                  send_input([NSString stringWithFormat:@"PADOFF %ld",
                                                        (long)MAX(c.playerIndex, 0)]);
               }];
   [nc addObserverForName:GCKeyboardDidConnectNotification
                   object:nil
                    queue:NSOperationQueue.mainQueue
               usingBlock:^(NSNotification *n) {
                  [self setupKeyboard:n.object];
               }];
   [nc addObserverForName:GCMouseDidConnectNotification
                   object:nil
                    queue:NSOperationQueue.mainQueue
               usingBlock:^(NSNotification *n) {
                  [self setupMouse:n.object];
               }];
   for (GCController *c in GCController.controllers)
      [self setupController:c];
   if (GCKeyboard.coalescedKeyboard)
      [self setupKeyboard:GCKeyboard.coalescedKeyboard];
   for (GCMouse *m in GCMouse.mice)
      [self setupMouse:m];

   /* touch: tap = click, one-finger drag = drag with the left button */
   UITapGestureRecognizer *tap =
      [[UITapGestureRecognizer alloc] initWithTarget:self
                                              action:@selector(touchTap:)];
   tap.numberOfTouchesRequired = 1;
   tap.delegate = self;
   [self.view addGestureRecognizer:tap];
   UIPanGestureRecognizer *pan =
      [[UIPanGestureRecognizer alloc] initWithTarget:self
                                              action:@selector(touchPan:)];
   pan.maximumNumberOfTouches = 1;
   pan.allowedTouchTypes = @[ @(UITouchTypeDirect) ];
   tap.allowedTouchTypes = @[ @(UITouchTypeDirect) ];
   pan.delegate = self;
   [self.view addGestureRecognizer:pan];
}

/* touches on the connect panel are for the panel, not the game */
- (BOOL)gestureRecognizer:(UIGestureRecognizer *)g shouldReceiveTouch:(UITouch *)t {
   return !(self.panel && !self.panel.hidden && [t.view isDescendantOfView:self.panel]);
}

/* touch point -> 0..1 over the shown frame (aspect fit), or NO if outside */
- (BOOL)normalizedPoint:(CGPoint)p x:(float *)x y:(float *)y {
   uint32_t w, h;
   if (self.stream) {
      w = self.stream.streamWidth;
      h = self.stream.streamHeight;
   } else {
      nm_presenter_metal_frame_size(&w, &h);
   }
   if (!w || !h)
      return NO;
   const CGRect b = self.view.bounds;
   const CGFloat scale = MIN(b.size.width / w, b.size.height / h);
   const CGFloat cw = w * scale, ch = h * scale;
   const CGFloat ox = (b.size.width - cw) / 2, oy = (b.size.height - ch) / 2;
   *x = (p.x - ox) / cw;
   *y = (p.y - oy) / ch;
   return *x >= 0 && *x <= 1 && *y >= 0 && *y <= 1;
}

- (void)touchTap:(UITapGestureRecognizer *)g {
   float x, y;
   if (![self streamHasInput] ||
       ![self normalizedPoint:[g locationInView:self.view] x:&x y:&y])
      return;
   if (self.stream) {
      [self.stream pointerAtX:x y:y];
      [self.stream mouseButton:1 pressed:YES];
      [self.stream mouseButton:1 pressed:NO];
      return;
   }
   send_input([NSString stringWithFormat:@"MABS %.4f %.4f", x, y]);
   send_input(@"MBTN 1 1");
   send_input(@"MBTN 1 0");
}

- (void)touchPan:(UIPanGestureRecognizer *)g {
   float x, y;
   if (![self streamHasInput])
      return;
   const BOOL inside = [self normalizedPoint:[g locationInView:self.view] x:&x y:&y];
   if (self.stream) {
      if (inside)
         [self.stream pointerAtX:x y:y];
      if (g.state == UIGestureRecognizerStateBegan)
         [self.stream mouseButton:1 pressed:YES];
      else if (g.state == UIGestureRecognizerStateEnded ||
               g.state == UIGestureRecognizerStateCancelled)
         [self.stream mouseButton:1 pressed:NO];
      return;
   }
   if (inside)
      send_input([NSString stringWithFormat:@"MABS %.4f %.4f", x, y]);
   if (g.state == UIGestureRecognizerStateBegan)
      send_input(@"MBTN 1 1");
   else if (g.state == UIGestureRecognizerStateEnded ||
            g.state == UIGestureRecognizerStateCancelled)
      send_input(@"MBTN 1 0");
}

- (UITextField *)fieldWithPlaceholder:(NSString *)placeholder
                                  key:(NSString *)key
                                    y:(CGFloat)y {
   UITextField *f = [[UITextField alloc] initWithFrame:CGRectMake(20, y, 360, 40)];
   f.placeholder = placeholder;
   f.borderStyle = UITextBorderStyleRoundedRect;
   f.autocapitalizationType = UITextAutocapitalizationTypeNone;
   f.autocorrectionType = UITextAutocorrectionTypeNo;
   f.spellCheckingType = UITextSpellCheckingTypeNo;
   f.delegate = self;
   f.text = [NSUserDefaults.standardUserDefaults stringForKey:key];
   [self.panel addSubview:f];
   return f;
}

- (void)viewDidLoad {
   [super viewDidLoad];
   self.view.backgroundColor = UIColor.blackColor;

   self.panel = [[UIView alloc] initWithFrame:CGRectMake(0, 0, 400, 552)];
   self.panel.backgroundColor = [UIColor colorWithWhite:0.12 alpha:0.92];
   self.panel.layer.cornerRadius = 16;
   [self.view addSubview:self.panel];

   /* frame rate cap: fewer frames = less memory traffic over the network */
   UISegmentedControl *fps =
      [[UISegmentedControl alloc] initWithItems:@[ @"30", @"60", @"120", @"max" ]];
   fps.frame = CGRectMake(200, 16, 180, 28);
   NSInteger cap = [NSUserDefaults.standardUserDefaults integerForKey:@"maxFps"];
   fps.selectedSegmentIndex = cap == 30 ? 0 : cap == 120 ? 2 : cap < 0 ? 3 : 1;
   [fps addTarget:self action:@selector(fpsChanged:) forControlEvents:UIControlEventValueChanged];
   [self.panel addSubview:fps];

   UILabel *title = [[UILabel alloc] initWithFrame:CGRectMake(20, 14, 180, 30)];
   title.text = @"Narnia Metal";
   title.textColor = UIColor.whiteColor;
   title.font = [UIFont boldSystemFontOfSize:22];
   [self.panel addSubview:title];

   self.hostField = [self fieldWithPlaceholder:@"relay host (box IP)" key:@"host" y:56];
   self.portField = [self fieldWithPlaceholder:@"port (7398)" key:@"port" y:104];
   self.portField.keyboardType = UIKeyboardTypeNumberPad;
   self.tokenField = [self fieldWithPlaceholder:@"token" key:@"token" y:152];

   self.connectButton = [UIButton buttonWithType:UIButtonTypeSystem];
   self.modeControl = [[UISegmentedControl alloc] initWithItems:@[ @"Metal", @"Stream" ]];
   self.modeControl.frame = CGRectMake(20, 200, 170, 30);
   self.modeControl.selectedSegmentIndex =
      [NSUserDefaults.standardUserDefaults integerForKey:@"mode"] == 1 ? 1 : 0;
   [self.modeControl addTarget:self action:@selector(modeChanged) forControlEvents:UIControlEventValueChanged];
   [self.panel addSubview:self.modeControl];

   self.upscaleControl = [[UISegmentedControl alloc] initWithItems:@[ @"Off", @"MetalFX", @"ANE" ]];
   self.upscaleControl.frame = CGRectMake(200, 200, 180, 30);
   self.upscaleControl.selectedSegmentIndex =
      MIN(MAX([NSUserDefaults.standardUserDefaults integerForKey:@"upscale"], 0), 2);
   [self.upscaleControl addTarget:self action:@selector(upscaleChanged) forControlEvents:UIControlEventValueChanged];
   [self.panel addSubview:self.upscaleControl];

   self.connectButton.frame = CGRectMake(20, 240, 360, 44);
   [self.connectButton setTitle:@"Connect" forState:UIControlStateNormal];
   self.connectButton.titleLabel.font = [UIFont boldSystemFontOfSize:18];
   [self.connectButton addTarget:self
                          action:@selector(toggleConnect)
                forControlEvents:UIControlEventTouchUpInside];
   [self.panel addSubview:self.connectButton];

   self.statusLabel = [[UILabel alloc] initWithFrame:CGRectMake(20, 288, 250, 24)];
   self.statusLabel.textColor = [UIColor colorWithWhite:0.8 alpha:1];
   self.statusLabel.font = [UIFont systemFontOfSize:14];
   self.statusLabel.text = @"not connected";
   [self.panel addSubview:self.statusLabel];

   self.aneButton = [UIButton buttonWithType:UIButtonTypeSystem];
   self.aneButton.frame = CGRectMake(280, 286, 100, 28);
   [self.aneButton setTitle:@"ANE test" forState:UIControlStateNormal];
   [self.aneButton addTarget:self action:@selector(runAneTest) forControlEvents:UIControlEventTouchUpInside];
   [self.panel addSubview:self.aneButton];

   self.logView = [[UITextView alloc] initWithFrame:CGRectMake(20, 316, 360, 140)];
   self.logView.editable = NO;
   self.logView.backgroundColor = UIColor.clearColor;
   self.logView.textColor = [UIColor colorWithWhite:0.6 alpha:1];
   self.logView.font = [UIFont monospacedSystemFontOfSize:10
                                                   weight:UIFontWeightRegular];
   [self.panel addSubview:self.logView];

   self.sweepField = [self fieldWithPlaceholder:@"model sweep URL" key:@"sweepURL" y:452];
   self.sweepField.keyboardType = UIKeyboardTypeURL;
   self.sweepButton = [UIButton buttonWithType:UIButtonTypeSystem];
   self.sweepButton.frame = CGRectMake(20, 498, 360, 40);
   [self.sweepButton setTitle:@"Model sweep" forState:UIControlStateNormal];
   [self.sweepButton addTarget:self action:@selector(runSweep) forControlEvents:UIControlEventTouchUpInside];
   [self.panel addSubview:self.sweepButton];

   self.statsLabel = [[UILabel alloc] initWithFrame:CGRectMake(12, 12, 420, 22)];
   self.statsLabel.textColor = [UIColor colorWithWhite:1 alpha:0.75];
   self.statsLabel.backgroundColor = [UIColor colorWithWhite:0 alpha:0.35];
   self.statsLabel.font = [UIFont monospacedDigitSystemFontOfSize:12
                                                          weight:UIFontWeightMedium];
   self.statsLabel.layer.cornerRadius = 6;
   self.statsLabel.layer.masksToBounds = YES;
   self.statsLabel.hidden = YES;
   [self.view addSubview:self.statsLabel];

   self.lastStatsTime = CACurrentMediaTime();
   self.statsTimer = [NSTimer scheduledTimerWithTimeInterval:1.0
                                                      target:self
                                                    selector:@selector(reportStats)
                                                    userInfo:nil
                                                     repeats:YES];
   /* repeat pad state so the box can tell a quiet pad from a lost one */
   [NSTimer scheduledTimerWithTimeInterval:0.25
                                   repeats:YES
                                     block:^(NSTimer *t) {
                                        if (![g_vc streamHasInput])
                                           return;
                                        for (GCController *c in GCController.controllers)
                                           send_pad(c);
                                     }];

   UITapGestureRecognizer *tap =
      [[UITapGestureRecognizer alloc] initWithTarget:self
                                              action:@selector(togglePanel)];
   tap.numberOfTouchesRequired = 3;
   [self.view addGestureRecognizer:tap];

   [self setupInput];
   [self modeChanged];
   [NSTimer scheduledTimerWithTimeInterval:3.0
                                    target:self
                                  selector:@selector(checkForBox)
                                  userInfo:nil
                                   repeats:YES];
   [self checkForBox];
}

- (void)sendTelemetry:(NSArray<NSString *> *)lines {
   if (!lines.count)
      return;
   NSMutableURLRequest *request = [NSMutableURLRequest
      requestWithURL:[NSURL URLWithString:[kTelemetryBase stringByAppendingString:@"/ipad/telemetry"]]];
   request.HTTPMethod = @"POST";
   request.timeoutInterval = 5;
   [request setValue:@"application/json" forHTTPHeaderField:@"Content-Type"];
   request.HTTPBody = [NSJSONSerialization dataWithJSONObject:@{@"lines" : lines} options:0 error:nil];
   [[NSURLSession.sharedSession dataTaskWithRequest:request] resume];
}

- (void)checkForBox {
   if (self.running || self.autoChecking)
      return;
   self.autoChecking = YES;
   NSURL *url = [NSURL URLWithString:[kTelemetryBase stringByAppendingString:@"/ipad/box"]];
   NSURLRequest *request = [NSURLRequest requestWithURL:url
                                            cachePolicy:NSURLRequestReloadIgnoringLocalCacheData
                                        timeoutInterval:10];
   [[NSURLSession.sharedSession
      dataTaskWithRequest:request
        completionHandler:^(NSData *data, NSURLResponse *response, NSError *error) {
           NSDictionary *box = nil;
           if (!error && [(NSHTTPURLResponse *)response statusCode] == 200 && data) {
              id parsed = [NSJSONSerialization JSONObjectWithData:data options:0 error:nil];
              if ([parsed isKindOfClass:NSDictionary.class])
                 box = parsed;
           }
           dispatch_async(dispatch_get_main_queue(), ^{
              self.autoChecking = NO;
              [self autoConnectTo:box];
           });
        }] resume];
}

- (void)autoConnectTo:(NSDictionary *)box {
   NSString *server = [box[@"server"] isKindOfClass:NSString.class] ? box[@"server"] : nil;
   NSString *hash = [box[@"hash"] isKindOfClass:NSString.class] ? box[@"hash"] : nil;
   if (self.running || !server.length || !hash.length)
      return;
   NSString *key = [NSString stringWithFormat:@"%@ %@", server, hash];
   if ([key isEqualToString:self.declinedBox])
      return;
   const CFTimeInterval now = CACurrentMediaTime();
   if ([key isEqualToString:self.lastAutoBox] && now - self.lastAutoAt < 10)
      return;
   self.lastAutoBox = key;
   self.lastAutoAt = now;
   fprintf(stderr, "stream: auto-connecting to %s\n", server.UTF8String);
   [self openStreamURL:server hash:hash];
}

- (BOOL)streamMode {
   return self.modeControl.selectedSegmentIndex == 1;
}

- (void)modeChanged {
   const BOOL stream = [self streamMode];
   NSUserDefaults *d = NSUserDefaults.standardUserDefaults;
   [d setInteger:stream ? 1 : 0 forKey:@"mode"];
   self.portField.hidden = stream;
   self.upscaleControl.hidden = !stream;
   self.hostField.placeholder = stream ? @"box URL (https://ip:port)" : @"relay host (box IP)";
   self.tokenField.placeholder = stream ? @"certificate sha-256" : @"token";
   self.hostField.text = [d stringForKey:stream ? @"streamUrl" : @"host"];
   self.tokenField.text = [d stringForKey:stream ? @"streamHash" : @"token"];
}

- (void)upscaleChanged {
   const NSInteger index = self.upscaleControl.selectedSegmentIndex;
   [NSUserDefaults.standardUserDefaults setInteger:index forKey:@"upscale"];
   self.stream.upscale = (NMUpscale)index;
}

- (void)sizeStreamLayer {
   CAMetalLayer *layer = self.streamLayer;
   if (!layer)
      return;
   const CGFloat scale = self.view.window.screen.scale ?: UIScreen.mainScreen.scale;
   layer.frame = self.view.layer.bounds;
   layer.contentsScale = scale;
   layer.drawableSize = CGSizeMake(layer.bounds.size.width * scale, layer.bounds.size.height * scale);
}

- (void)openStreamURL:(NSString *)url hash:(NSString *)hash {
   if (self.running)
      [self toggleConnect];
   self.modeControl.selectedSegmentIndex = 1;
   [self modeChanged];
   self.hostField.text = url;
   self.tokenField.text = hash;
   [self toggleConnect];
}

- (void)toggleStream {
   if (self.running) {
      self.declinedBox = [NSString stringWithFormat:@"%@ %@", self.hostField.text, self.tokenField.text];
      [self endStream:@"disconnected"];
      return;
   }
   NSString *url = [self.hostField.text
      stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceCharacterSet];
   NSString *hash = [self.tokenField.text
      stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceCharacterSet];
   if (!url.length || !hash.length) {
      [self setStatus:@"need the box URL and its certificate hash"];
      return;
   }
   NSUserDefaults *d = NSUserDefaults.standardUserDefaults;
   [d setObject:url forKey:@"streamUrl"];
   [d setObject:hash forKey:@"streamHash"];

   CAMetalLayer *layer = [CAMetalLayer layer];
   layer.opaque = YES;
   layer.backgroundColor = UIColor.blackColor.CGColor;
   [self.view.layer insertSublayer:layer below:self.panel.layer];
   self.streamLayer = layer;
   [self sizeStreamLayer];

   self.running = YES;
   self.modeControl.enabled = NO;
   [self.connectButton setTitle:@"Disconnect" forState:UIControlStateNormal];
   [self setStatus:@"connecting"];
   UIApplication.sharedApplication.idleTimerDisabled = YES;
   const NMUpscale upscale = (NMUpscale)self.upscaleControl.selectedSegmentIndex;
   dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
      NSString *error = nil;
      NMStream *stream = [NMStream connectToURL:url certHash:hash layer:layer error:&error];
      dispatch_async(dispatch_get_main_queue(), ^{
         if (self.streamLayer != layer) {
            dispatch_async(dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{
               [stream stop];
            });
            return;
         }
         if (!stream) {
            [self endStream:error ?: @"connect failed"];
            return;
         }
         stream.upscale = upscale;
         __weak NMViewController *weakSelf = self;
         stream.onEnded = ^(NSString *reason) {
            NMViewController *strongSelf = weakSelf;
            if (strongSelf.stream == stream)
               [strongSelf endStream:reason];
         };
         self.stream = stream;
         [stream start];
         [self setStatus:[NSString stringWithFormat:@"streaming %ux%u", stream.streamWidth,
                                                    stream.streamHeight]];
         self.panel.hidden = YES;
         [self updateInputState];
      });
   });
}

- (void)endStream:(NSString *)reason {
   NMStream *stream = self.stream;
   CAMetalLayer *layer = self.streamLayer;
   self.stream = nil;
   self.streamLayer = nil;
   self.running = NO;
   self.modeControl.enabled = YES;
   [self.connectButton setTitle:@"Connect" forState:UIControlStateNormal];
   UIApplication.sharedApplication.idleTimerDisabled = NO;
   self.panel.hidden = NO;
   [self updateInputState];
   [self setStatus:reason];
   dispatch_async(dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{
      [stream stop];
      dispatch_async(dispatch_get_main_queue(), ^{
         [layer removeFromSuperlayer];
      });
   });
}

- (void)viewDidLayoutSubviews {
   [super viewDidLayoutSubviews];
   self.panel.center = CGPointMake(CGRectGetMidX(self.view.bounds),
                                   CGRectGetMidY(self.view.bounds));
   [self sizeStreamLayer];
   for (CALayer *layer in self.view.layer.sublayers) {
      if ([layer isKindOfClass:CAMetalLayer.class])
         layer.frame = self.view.layer.bounds;
   }
}

- (void)reportStats {
   const CFTimeInterval now = CACurrentMediaTime();
   const double dt = now - self.lastStatsTime;
   if (dt <= 0)
      return;
   const uint64_t frames = self.stream ? self.stream.framesShown : nm_presenter_metal_frames();
   const uint64_t rx = nm_relay_client_rx_bytes();
   const double fps = (frames - self.lastFrames) / dt;
   const double mbps = (rx - self.lastRx) * 8.0 / dt / 1e6;
   self.lastFrames = frames;
   self.lastRx = rx;
   self.lastStatsTime = now;

   task_vm_info_data_t vm;
   mach_msg_type_number_t count = TASK_VM_INFO_COUNT;
   double mem_mb = 0;
   if (task_info(mach_task_self(), TASK_VM_INFO, (task_info_t)&vm, &count) ==
       KERN_SUCCESS)
      mem_mb = vm.phys_footprint / 1048576.0;

   static const char *thermal_names[] = { "nominal", "fair", "serious",
                                          "critical" };
   const NSProcessInfoThermalState thermal = NSProcessInfo.processInfo.thermalState;
   struct nm_audio_stats au;
   const BOOL hasAudio = nm_host_audio_stats(&au);
   /* the audio daemon's stream is one of the relay's streams */
   const uint32_t all_streams = nm_relay_client_active_streams();
   const uint32_t streams = all_streams > (hasAudio ? 1 : 0)
                               ? all_streams - (hasAudio ? 1 : 0) : 0;

   self.statsLabel.hidden = !all_streams && !self.stream;
   if (self.stream && (++self.statsTick % 2) == 0) {
      NSMutableArray<NSString *> *lines = [NSMutableArray arrayWithObject:[self.stream takeStatsLine]];
      [lines addObjectsFromArray:take_stream_log()];
      [self sendTelemetry:lines];
   }
   NSString *audioText = hasAudio
      ? [NSString stringWithFormat:@" · audio %u ms%s", au.depth_ms,
                                   au.underruns ? "!" : ""]
      : @"";
   self.statsLabel.text = [NSString
      stringWithFormat:@" %.0f fps · %.1f Mbit/s · %u stream%s%@ · %.0f MB ",
                       fps, mbps, streams, streams == 1 ? "" : "s", audioText,
                       mem_mb];

   if (!self.running)
      return;
   char kv[448];
   int len = snprintf(kv, sizeof(kv),
            "fps=%.1f rx_mbit=%.2f streams=%u frames=%llu mem_mb=%.0f thermal=%s",
            fps, mbps, streams, (unsigned long long)frames, mem_mb,
            thermal_names[thermal < 4 ? thermal : 0]);
   if (hasAudio && len > 0 && len < (int)sizeof(kv))
      snprintf(kv + len, sizeof(kv) - len,
               " aud_buf_ms=%u aud_target_ms=%u aud_jitter_ms=%u aud_pkts=%llu"
               " aud_lost=%llu aud_under=%llu aud_drop=%llu",
               au.depth_ms, au.target_ms, au.jitter_ms,
               (unsigned long long)au.packets, (unsigned long long)au.lost,
               (unsigned long long)au.underruns, (unsigned long long)au.dropped);
   nm_relay_client_stat(kv);
}

- (BOOL)textFieldShouldReturn:(UITextField *)textField {
   [textField resignFirstResponder];
   return YES;
}

- (void)fpsChanged:(UISegmentedControl *)c {
   static const NSInteger caps[] = { 30, 60, 120, -1 };
   const NSInteger cap = caps[c.selectedSegmentIndex];
   [NSUserDefaults.standardUserDefaults setInteger:cap forKey:@"maxFps"];
   nm_host_set_max_fps(cap > 0 ? (unsigned)cap : 0);
   fprintf(stderr, "frame rate cap: %s\n", cap > 0 ? [@(cap) stringValue].UTF8String : "none");
}

- (void)togglePanel {
   self.panel.hidden = !self.panel.hidden;
   [self updateInputState];
}

- (void)runAneTest {
   self.aneButton.enabled = NO;
   [self appendLog:@"ANE test running..."];
   nm_ane_test_run(
      ^(NSString *line) {
         [self appendLog:line];
      },
      ^(NSString *all) {
         UIPasteboard.generalPasteboard.string = all;
         [self appendLog:@"results copied to clipboard"];
         self.aneButton.enabled = YES;
      });
}

- (void)runSweep {
   [self.view endEditing:YES];
   NSString *url = [self.sweepField.text stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceCharacterSet];
   if (!url.length) {
      [self appendLog:@"model sweep: enter the sweep URL first"];
      return;
   }
   [NSUserDefaults.standardUserDefaults setObject:url forKey:@"sweepURL"];
   self.sweepButton.enabled = NO;
   self.aneButton.enabled = NO;
   UIApplication.sharedApplication.idleTimerDisabled = YES;
   [self appendLog:@"model sweep running, keep the app open..."];
   nm_coreml_sweep_run(
      url,
      ^(NSString *line) {
         [self appendLog:line];
      },
      ^(NSString *all) {
         UIPasteboard.generalPasteboard.string = all;
         [self appendLog:@"results copied to clipboard"];
         UIApplication.sharedApplication.idleTimerDisabled = NO;
         self.sweepButton.enabled = YES;
         self.aneButton.enabled = YES;
      });
}

- (void)setStatus:(NSString *)text {
   self.statusLabel.text = text;
}

- (void)appendLog:(NSString *)text {
   NSString *all = [self.logView.text stringByAppendingFormat:@"%@\n", text];
   if (all.length > 8000)
      all = [all substringFromIndex:all.length - 8000];
   self.logView.text = all;
   [self.logView scrollRangeToVisible:NSMakeRange(all.length, 0)];
}

- (void)toggleConnect {
   [self.view endEditing:YES];
   if (self.stream || self.streamLayer || (!self.running && [self streamMode])) {
      [self toggleStream];
      return;
   }
   if (self.running) {
      if (g_session)
         atomic_store(&g_session->stop, true);
      g_session = NULL;
      self.running = NO;
      [self.connectButton setTitle:@"Connect" forState:UIControlStateNormal];
      [self setStatus:@"disconnecting"];
      UIApplication.sharedApplication.idleTimerDisabled = NO;
      [self updateInputState];
      return;
   }

   NSString *host = [self.hostField.text
      stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceCharacterSet];
   NSString *token = [self.tokenField.text
      stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceCharacterSet];
   int port = self.portField.text.length ? self.portField.text.intValue
                                         : NM_RELAY_DEFAULT_PORT;
   if (!host.length || !token.length) {
      [self setStatus:@"need a host and a token"];
      return;
   }
   NSUserDefaults *d = NSUserDefaults.standardUserDefaults;
   [d setObject:host forKey:@"host"];
   [d setObject:self.portField.text ?: @"" forKey:@"port"];
   [d setObject:token forKey:@"token"];

   /* sessions are never freed: data threads may still read cfg */
   struct session *s = calloc(1, sizeof(*s));
   snprintf(s->host, sizeof(s->host), "%s", host.UTF8String);
   snprintf(s->token, sizeof(s->token), "%s", token.UTF8String);
   s->cfg = (struct nm_relay_client_config){
      .host = s->host,
      .port = port,
      .token = s->token,
      .status = on_status,
   };
   g_session = s;

   self.running = YES;
   [self.connectButton setTitle:@"Disconnect" forState:UIControlStateNormal];
   UIApplication.sharedApplication.idleTimerDisabled = YES;

   [NSThread detachNewThreadWithBlock:^{
      while (!atomic_load(&s->stop)) {
         if (nm_relay_client_run(&s->cfg, &s->stop) == NM_RELAY_KICKED)
            break;
         for (int i = 0; i < 20 && !atomic_load(&s->stop); i++)
            usleep(100 * 1000);
      }
      on_status(NULL, "disconnected");
      dispatch_async(dispatch_get_main_queue(), ^{
         [g_vc sessionEnded:s];
      });
   }];
}

- (void)sessionEnded:(struct session *)s {
   /* a kicked session ends by itself; reset the button if it's current */
   if (g_session != s)
      return;
   g_session = NULL;
   self.running = NO;
   [self.connectButton setTitle:@"Connect" forState:UIControlStateNormal];
   UIApplication.sharedApplication.idleTimerDisabled = NO;
   [self updateInputState];
}

@end

/* ------------------------------------------------------------------ */

@interface NMAppDelegate : UIResponder <UIApplicationDelegate>
@property(nonatomic, strong) UIWindow *window;
- (BOOL)openNarniaURL:(NSURL *)url;
@end

@implementation NMAppDelegate

- (BOOL)application:(UIApplication *)application
   didFinishLaunchingWithOptions:(NSDictionary *)launchOptions {
   g_stream_log = [NSMutableArray array];
   capture_stderr();
   install_crash_reporting();
   struct utsname u;
   uname(&u);
   NSString *ver = [NSBundle.mainBundle objectForInfoDictionaryKey:@"CFBundleShortVersionString"];
   fprintf(stderr, "Narnia Metal %s on %s, iOS %s\n", ver.UTF8String, u.machine,
           UIDevice.currentDevice.systemVersion.UTF8String);
   nm_host_set_extra_extensions(nm_presenter_metal_instance_exts, 2,
                                nm_presenter_metal_device_exts, 1);
   nm_host_set_presenter(&nm_presenter_metal,
                         nm_presenter_metal_user(&layer_ops));
   /* 60 fps unless changed: a 120 Hz screen would otherwise double the
    * game's frame rate and the memory traffic that comes with it */
   NSInteger cap = [NSUserDefaults.standardUserDefaults integerForKey:@"maxFps"];
   nm_host_set_max_fps(cap > 0 ? (unsigned)cap : cap < 0 ? 0 : 60);
   nm_host_set_audio_sink(&nm_audio_sink_ios, NULL);

   self.window = [[UIWindow alloc] initWithFrame:UIScreen.mainScreen.bounds];
   g_vc = [[NMViewController alloc] init];
   self.window.rootViewController = g_vc;
   [self.window makeKeyAndVisible];
   NSURL *launchURL = launchOptions[UIApplicationLaunchOptionsURLKey];
   if (launchURL)
      dispatch_async(dispatch_get_main_queue(), ^{
         [self openNarniaURL:launchURL];
      });

   nm_vk_proc gipa = nm_driver_get_instance_proc_addr();
   fprintf(stderr, "driver: %s (%s)\n", nm_driver_name(),
           gipa ? "loaded" : "FAILED to load");
   if (!nm_host_init(gipa, on_log))
      [g_vc setStatus:@"vkr failed to start"];
   return YES;
}

@end

@implementation NMAppDelegate (Links)

- (BOOL)application:(UIApplication *)app
            openURL:(NSURL *)url
            options:(NSDictionary<UIApplicationOpenURLOptionsKey, id> *)options {
   return [self openNarniaURL:url];
}

- (BOOL)openNarniaURL:(NSURL *)url {
   if (![url.scheme isEqualToString:@"narniametal"])
      return NO;
   NSURLComponents *parts = [NSURLComponents componentsWithURL:url resolvingAgainstBaseURL:NO];
   NSString *server = nil, *hash = nil;
   for (NSURLQueryItem *item in parts.queryItems) {
      if ([item.name isEqualToString:@"server"])
         server = item.value;
      else if ([item.name isEqualToString:@"hash"])
         hash = item.value;
   }
   if (!server.length || !hash.length)
      return NO;
   [g_vc openStreamURL:server hash:hash];
   return YES;
}

@end

int
main(int argc, char *argv[])
{
   @autoreleasepool {
      return UIApplicationMain(argc, argv, nil,
                               NSStringFromClass(NMAppDelegate.class));
   }
}
