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
#include <mach/mach.h>
#include <pthread.h>
#include <stdatomic.h>
#include <sys/utsname.h>
#include <unistd.h>

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
@end

/* ---- logs: everything written to stderr (ours, vkr's, MoltenVK's) goes
 * to the real stderr, the on-screen log and the box (telemetry) ---- */

static int g_real_stderr = -1;

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
      if (s) {
         dispatch_async(dispatch_get_main_queue(), ^{
            [g_vc appendLog:s];
         });
      }
   }
   return NULL;
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
send_pad(GCController *c)
{
   GCExtendedGamepad *g = c.extendedGamepad;
   if (!g)
      return;
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
         if ([g_vc streamHasInput])
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
            if ([g_vc streamHasInput])
               send_input([NSString stringWithFormat:@"MBTN %d %d", number,
                                                     pressed]);
         };
   }
   in.scroll.yAxis.valueChangedHandler = ^(GCControllerAxisInput *a, float v) {
      if ([g_vc streamHasInput] && v != 0)
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
   nm_presenter_metal_frame_size(&w, &h);
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
   send_input([NSString stringWithFormat:@"MABS %.4f %.4f", x, y]);
   send_input(@"MBTN 1 1");
   send_input(@"MBTN 1 0");
}

- (void)touchPan:(UIPanGestureRecognizer *)g {
   float x, y;
   if (![self streamHasInput])
      return;
   const BOOL inside = [self normalizedPoint:[g locationInView:self.view] x:&x y:&y];
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

   self.panel = [[UIView alloc] initWithFrame:CGRectMake(0, 0, 400, 420)];
   self.panel.backgroundColor = [UIColor colorWithWhite:0.12 alpha:0.92];
   self.panel.layer.cornerRadius = 16;
   [self.view addSubview:self.panel];

   UILabel *title = [[UILabel alloc] initWithFrame:CGRectMake(20, 14, 360, 30)];
   title.text = @"Narnia Metal";
   title.textColor = UIColor.whiteColor;
   title.font = [UIFont boldSystemFontOfSize:22];
   [self.panel addSubview:title];

   self.hostField = [self fieldWithPlaceholder:@"relay host (box IP)" key:@"host" y:56];
   self.portField = [self fieldWithPlaceholder:@"port (7398)" key:@"port" y:104];
   self.portField.keyboardType = UIKeyboardTypeNumberPad;
   self.tokenField = [self fieldWithPlaceholder:@"token" key:@"token" y:152];

   self.connectButton = [UIButton buttonWithType:UIButtonTypeSystem];
   self.connectButton.frame = CGRectMake(20, 200, 360, 44);
   [self.connectButton setTitle:@"Connect" forState:UIControlStateNormal];
   self.connectButton.titleLabel.font = [UIFont boldSystemFontOfSize:18];
   [self.connectButton addTarget:self
                          action:@selector(toggleConnect)
                forControlEvents:UIControlEventTouchUpInside];
   [self.panel addSubview:self.connectButton];

   self.statusLabel = [[UILabel alloc] initWithFrame:CGRectMake(20, 248, 360, 24)];
   self.statusLabel.textColor = [UIColor colorWithWhite:0.8 alpha:1];
   self.statusLabel.font = [UIFont systemFontOfSize:14];
   self.statusLabel.text = @"not connected";
   [self.panel addSubview:self.statusLabel];

   self.logView = [[UITextView alloc] initWithFrame:CGRectMake(20, 276, 360, 130)];
   self.logView.editable = NO;
   self.logView.backgroundColor = UIColor.clearColor;
   self.logView.textColor = [UIColor colorWithWhite:0.6 alpha:1];
   self.logView.font = [UIFont monospacedSystemFontOfSize:10
                                                   weight:UIFontWeightRegular];
   [self.panel addSubview:self.logView];

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
}

- (void)viewDidLayoutSubviews {
   [super viewDidLayoutSubviews];
   self.panel.center = CGPointMake(CGRectGetMidX(self.view.bounds),
                                   CGRectGetMidY(self.view.bounds));
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
   const uint64_t frames = nm_presenter_metal_frames();
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
   const uint32_t streams = nm_relay_client_active_streams();

   self.statsLabel.hidden = !streams;
   self.statsLabel.text = [NSString
      stringWithFormat:@" %.0f fps · %.1f Mbit/s · %u stream%s · %.0f MB ",
                       fps, mbps, streams, streams == 1 ? "" : "s", mem_mb];

   if (!self.running)
      return;
   char kv[256];
   snprintf(kv, sizeof(kv),
            "fps=%.1f rx_mbit=%.2f streams=%u frames=%llu mem_mb=%.0f thermal=%s",
            fps, mbps, streams, (unsigned long long)frames, mem_mb,
            thermal_names[thermal < 4 ? thermal : 0]);
   nm_relay_client_stat(kv);
}

- (BOOL)textFieldShouldReturn:(UITextField *)textField {
   [textField resignFirstResponder];
   return YES;
}

- (void)togglePanel {
   self.panel.hidden = !self.panel.hidden;
   [self updateInputState];
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
@end

@implementation NMAppDelegate

- (BOOL)application:(UIApplication *)application
   didFinishLaunchingWithOptions:(NSDictionary *)launchOptions {
   capture_stderr();
   struct utsname u;
   uname(&u);
   NSString *ver = [NSBundle.mainBundle objectForInfoDictionaryKey:@"CFBundleShortVersionString"];
   fprintf(stderr, "Narnia Metal %s on %s, iOS %s\n", ver.UTF8String, u.machine,
           UIDevice.currentDevice.systemVersion.UTF8String);
   nm_host_set_extra_extensions(nm_presenter_metal_instance_exts, 2,
                                nm_presenter_metal_device_exts, 1);
   nm_host_set_presenter(&nm_presenter_metal,
                         nm_presenter_metal_user(&layer_ops));

   self.window = [[UIWindow alloc] initWithFrame:UIScreen.mainScreen.bounds];
   g_vc = [[NMViewController alloc] init];
   self.window.rootViewController = g_vc;
   [self.window makeKeyAndVisible];

   if (!nm_host_init(nm_driver_get_instance_proc_addr(), on_log))
      [g_vc setStatus:@"vkr failed to start"];
   else
      [g_vc appendLog:[NSString stringWithFormat:@"driver: %s",
                                                 nm_driver_name()]];
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
