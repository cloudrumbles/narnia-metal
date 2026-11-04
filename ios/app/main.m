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
#import <QuartzCore/CAMetalLayer.h>
#import <UIKit/UIKit.h>
#include <stdatomic.h>

#include "nm_driver.h"
#include "nm_host.h"
#include "nm_presenter_metal.h"
#include "nm_relay_client.h"

@class NMViewController;
static NMViewController *g_vc;

/* ------------------------------------------------------------------ */

@interface NMViewController : UIViewController <UITextFieldDelegate>
@property(nonatomic, strong) UIView *panel;
@property(nonatomic, strong) UITextField *hostField;
@property(nonatomic, strong) UITextField *portField;
@property(nonatomic, strong) UITextField *tokenField;
@property(nonatomic, strong) UIButton *connectButton;
@property(nonatomic, strong) UILabel *statusLabel;
@property(nonatomic, strong) UITextView *logView;
@property(nonatomic, assign) BOOL running;
- (void)setStatus:(NSString *)text;
- (void)appendLog:(NSString *)text;
@end

/* ---- C callbacks from the host core (any thread) ---- */

static void
on_log(const char *msg)
{
   NSString *s = [NSString stringWithUTF8String:msg];
   NSLog(@"%@", s);
   dispatch_async(dispatch_get_main_queue(), ^{
      [g_vc appendLog:s];
   });
}

static void
on_status(void *user, const char *msg)
{
   NSString *s = [NSString stringWithUTF8String:msg];
   NSLog(@"relay: %@", s);
   dispatch_async(dispatch_get_main_queue(), ^{
      [g_vc setStatus:s];
      [g_vc appendLog:[@"relay: " stringByAppendingString:s]];
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
   });
}

static const struct nm_metal_layer_ops layer_ops = {
   .create_layer = create_layer,
   .destroy_layer = destroy_layer,
   .first_frame = first_frame,
};

/* ---- relay thread ---- */

static atomic_bool g_stop;
static char g_host[256], g_token[128];
static struct nm_relay_client_config g_cfg;

@implementation NMViewController

- (BOOL)prefersStatusBarHidden {
   return YES;
}

- (BOOL)prefersHomeIndicatorAutoHidden {
   return YES;
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

   UITapGestureRecognizer *tap =
      [[UITapGestureRecognizer alloc] initWithTarget:self
                                              action:@selector(togglePanel)];
   tap.numberOfTouchesRequired = 3;
   [self.view addGestureRecognizer:tap];
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

- (BOOL)textFieldShouldReturn:(UITextField *)textField {
   [textField resignFirstResponder];
   return YES;
}

- (void)togglePanel {
   self.panel.hidden = !self.panel.hidden;
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
      atomic_store(&g_stop, true);
      self.running = NO;
      [self.connectButton setTitle:@"Connect" forState:UIControlStateNormal];
      [self setStatus:@"disconnecting"];
      UIApplication.sharedApplication.idleTimerDisabled = NO;
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

   snprintf(g_host, sizeof(g_host), "%s", host.UTF8String);
   snprintf(g_token, sizeof(g_token), "%s", token.UTF8String);
   g_cfg = (struct nm_relay_client_config){
      .host = g_host,
      .port = port,
      .token = g_token,
      .status = on_status,
   };

   self.running = YES;
   atomic_store(&g_stop, false);
   [self.connectButton setTitle:@"Disconnect" forState:UIControlStateNormal];
   UIApplication.sharedApplication.idleTimerDisabled = YES;

   [NSThread detachNewThreadWithBlock:^{
      while (!atomic_load(&g_stop)) {
         nm_relay_client_run(&g_cfg, &g_stop);
         for (int i = 0; i < 20 && !atomic_load(&g_stop); i++)
            usleep(100 * 1000);
      }
      on_status(NULL, "disconnected");
   }];
}

@end

/* ------------------------------------------------------------------ */

@interface NMAppDelegate : UIResponder <UIApplicationDelegate>
@property(nonatomic, strong) UIWindow *window;
@end

@implementation NMAppDelegate

- (BOOL)application:(UIApplication *)application
   didFinishLaunchingWithOptions:(NSDictionary *)launchOptions {
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
