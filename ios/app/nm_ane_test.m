#import "nm_ane_test.h"
#import <CoreMedia/CoreMedia.h>
#import <CoreVideo/CoreVideo.h>
#import <Metal/Metal.h>
#import <MetalFX/MetalFX.h>
#import <UIKit/UIKit.h>
#import <VideoToolbox/VideoToolbox.h>
#include <mach/mach_time.h>
#include <sys/sysctl.h>

static double now_ms(void) {
   static mach_timebase_info_data_t tb;
   if (!tb.denom)
      mach_timebase_info(&tb);
   return (double)mach_absolute_time() * tb.numer / tb.denom / 1e6;
}

static NSString *machine(void) {
   size_t n = 0;
   sysctlbyname("hw.machine", NULL, &n, NULL, 0);
   char *b = malloc(n);
   sysctlbyname("hw.machine", b, &n, NULL, 0);
   NSString *s = [NSString stringWithUTF8String:b];
   free(b);
   return s;
}

static NSString *fourcc(OSType t) {
   char c[5] = {(char)(t >> 24), (char)(t >> 16), (char)(t >> 8), (char)t, 0};
   return [NSString stringWithUTF8String:c];
}

static NSString *stats(NSMutableArray<NSNumber *> *a) {
   if (!a.count)
      return @"no samples";
   [a sortUsingSelector:@selector(compare:)];
   double med = a[(a.count - 1) / 2].doubleValue;
   double p95 = a[(NSUInteger)((a.count - 1) * 0.95)].doubleValue;
   return [NSString stringWithFormat:@"median %.2f ms, p95 %.2f ms, min %.2f (n=%lu)", med, p95,
                                     a[0].doubleValue, (unsigned long)a.count];
}

static CVPixelBufferRef make_buffer(NSDictionary *attrs, int w, int h, NSMutableString *err) {
   NSMutableDictionary *d = attrs ? [attrs mutableCopy] : [NSMutableDictionary dictionary];
   d[(id)kCVPixelBufferWidthKey] = @(w);
   d[(id)kCVPixelBufferHeightKey] = @(h);
   if (!d[(id)kCVPixelBufferPixelFormatTypeKey])
      d[(id)kCVPixelBufferPixelFormatTypeKey] = @(kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange);
   if (!d[(id)kCVPixelBufferIOSurfacePropertiesKey])
      d[(id)kCVPixelBufferIOSurfacePropertiesKey] = @{};
   CVPixelBufferPoolRef pool = NULL;
   CVReturn r = CVPixelBufferPoolCreate(NULL, NULL, (__bridge CFDictionaryRef)d, &pool);
   if (r != kCVReturnSuccess) {
      [err appendFormat:@"pool create %d ", r];
      return NULL;
   }
   CVPixelBufferRef pb = NULL;
   r = CVPixelBufferPoolCreatePixelBuffer(NULL, pool, &pb);
   CVPixelBufferPoolRelease(pool);
   if (r != kCVReturnSuccess) {
      [err appendFormat:@"buffer create %d ", r];
      return NULL;
   }
   return pb;
}

static void fill(CVPixelBufferRef pb) {
   CVPixelBufferLockBaseAddress(pb, 0);
   size_t planes = CVPixelBufferGetPlaneCount(pb);
   size_t count = planes ? planes : 1;
   for (size_t i = 0; i < count; i++) {
      uint8_t *p = planes ? CVPixelBufferGetBaseAddressOfPlane(pb, i) : CVPixelBufferGetBaseAddress(pb);
      size_t bpr = planes ? CVPixelBufferGetBytesPerRowOfPlane(pb, i) : CVPixelBufferGetBytesPerRow(pb);
      size_t h = planes ? CVPixelBufferGetHeightOfPlane(pb, i) : CVPixelBufferGetHeight(pb);
      for (size_t y = 0; y < h; y++)
         for (size_t x = 0; x < bpr; x++)
            p[y * bpr + x] = i == 0 ? (uint8_t)(((x / 7 + y / 5) * 9) ^ ((x * y) >> 6))
                                    : (uint8_t)(112 + ((x + y) & 31));
   }
   CVPixelBufferUnlockBaseAddress(pb, 0);
}

static double plane0_mean(CVPixelBufferRef pb) {
   CVPixelBufferLockBaseAddress(pb, kCVPixelBufferLock_ReadOnly);
   size_t planes = CVPixelBufferGetPlaneCount(pb);
   uint8_t *p = planes ? CVPixelBufferGetBaseAddressOfPlane(pb, 0) : CVPixelBufferGetBaseAddress(pb);
   size_t bpr = planes ? CVPixelBufferGetBytesPerRowOfPlane(pb, 0) : CVPixelBufferGetBytesPerRow(pb);
   size_t w = planes ? CVPixelBufferGetWidthOfPlane(pb, 0) : CVPixelBufferGetWidth(pb);
   size_t h = planes ? CVPixelBufferGetHeightOfPlane(pb, 0) : CVPixelBufferGetHeight(pb);
   double sum = 0;
   for (size_t y = 0; y < h; y += 4)
      for (size_t x = 0; x < w; x += 4)
         sum += p[y * bpr + x];
   CVPixelBufferUnlockBaseAddress(pb, kCVPixelBufferLock_ReadOnly);
   return sum / ((double)((h + 3) / 4) * ((w + 3) / 4));
}

API_AVAILABLE(ios(26.0))
static void sr_time(int w, int h, float scale, void (^L)(NSString *)) {
   int ow = (int)lroundf(w * scale), oh = (int)lroundf(h * scale);
   L([NSString stringWithFormat:@"-- ANE scaler %dx%d -> %dx%d", w, h, ow, oh]);
   VTLowLatencySuperResolutionScalerConfiguration *cfg =
      [[VTLowLatencySuperResolutionScalerConfiguration alloc] initWithFrameWidth:w
                                                                     frameHeight:h
                                                                     scaleFactor:scale];
   if (!cfg) {
      L(@"   config: nil");
      return;
   }
   NSMutableArray *fmts = [NSMutableArray array];
   for (NSNumber *f in cfg.frameSupportedPixelFormats)
      [fmts addObject:fourcc(f.unsignedIntValue)];
   L([NSString stringWithFormat:@"   pixel formats: %@", [fmts componentsJoinedByString:@","]]);
   VTFrameProcessor *proc = [[VTFrameProcessor alloc] init];
   NSError *e = nil;
   double t0 = now_ms();
   if (![proc startSessionWithConfiguration:cfg error:&e]) {
      L([NSString stringWithFormat:@"   startSession failed: %@", e]);
      return;
   }
   L([NSString stringWithFormat:@"   session start %.0f ms", now_ms() - t0]);
   NSMutableString *err = [NSMutableString string];
   CVPixelBufferRef src = make_buffer(cfg.sourcePixelBufferAttributes, w, h, err);
   CVPixelBufferRef dst = make_buffer(cfg.destinationPixelBufferAttributes, ow, oh, err);
   if (!src || !dst) {
      L([NSString stringWithFormat:@"   buffers: %@", err]);
      if (src)
         CVPixelBufferRelease(src);
      if (dst)
         CVPixelBufferRelease(dst);
      [proc endSession];
      return;
   }
   L([NSString stringWithFormat:@"   src %@ dst %@", fourcc(CVPixelBufferGetPixelFormatType(src)),
                                fourcc(CVPixelBufferGetPixelFormatType(dst))]);
   fill(src);
   NSMutableArray<NSNumber *> *times = [NSMutableArray array];
   __block NSError *perr = nil;
   for (int i = 0; i < 90; i++) {
      VTFrameProcessorFrame *sf = [[VTFrameProcessorFrame alloc] initWithBuffer:src
                                                          presentationTimeStamp:CMTimeMake(i, 30)];
      VTFrameProcessorFrame *df = [[VTFrameProcessorFrame alloc] initWithBuffer:dst
                                                          presentationTimeStamp:CMTimeMake(i, 30)];
      VTLowLatencySuperResolutionScalerParameters *p =
         [[VTLowLatencySuperResolutionScalerParameters alloc] initWithSourceFrame:sf destinationFrame:df];
      if (!sf || !df || !p) {
         L(@"   frame/parameters: nil");
         break;
      }
      dispatch_semaphore_t sem = dispatch_semaphore_create(0);
      double a = now_ms();
      [proc processWithParameters:p
                completionHandler:^(id<VTFrameProcessorParameters> done, NSError *pe) {
                   if (pe)
                      perr = pe;
                   dispatch_semaphore_signal(sem);
                }];
      dispatch_semaphore_wait(sem, DISPATCH_TIME_FOREVER);
      double b = now_ms();
      if (perr) {
         L([NSString stringWithFormat:@"   process failed at frame %d: %@", i, perr]);
         break;
      }
      if (i >= 10)
         [times addObject:@(b - a)];
   }
   if (!perr && times.count) {
      L([NSString stringWithFormat:@"   per frame: %@", stats(times)]);
      L([NSString stringWithFormat:@"   luma mean src %.1f dst %.1f", plane0_mean(src), plane0_mean(dst)]);
   }
   [proc endSession];
   CVPixelBufferRelease(src);
   CVPixelBufferRelease(dst);
}

API_AVAILABLE(ios(26.0))
static void fi_time(int w, int h, int scale, void (^L)(NSString *)) {
   int ow = w * scale, oh = h * scale;
   L([NSString stringWithFormat:@"-- ANE frame interpolation %dx%d -> %dx%d, one frame at phase 0.5", w, h, ow, oh]);
   VTLowLatencyFrameInterpolationConfiguration *cfg =
      scale > 1 ? [[VTLowLatencyFrameInterpolationConfiguration alloc] initWithFrameWidth:w
                                                                             frameHeight:h
                                                                      spatialScaleFactor:scale]
                : [[VTLowLatencyFrameInterpolationConfiguration alloc] initWithFrameWidth:w
                                                                             frameHeight:h
                                                              numberOfInterpolatedFrames:1];
   if (!cfg) {
      L(@"   config: nil");
      return;
   }
   VTFrameProcessor *proc = [[VTFrameProcessor alloc] init];
   NSError *e = nil;
   double t0 = now_ms();
   if (![proc startSessionWithConfiguration:cfg error:&e]) {
      L([NSString stringWithFormat:@"   startSession failed: %@", e]);
      return;
   }
   L([NSString stringWithFormat:@"   session start %.0f ms", now_ms() - t0]);
   NSMutableString *err = [NSMutableString string];
   CVPixelBufferRef prev = make_buffer(cfg.sourcePixelBufferAttributes, w, h, err);
   CVPixelBufferRef src = make_buffer(cfg.sourcePixelBufferAttributes, w, h, err);
   CVPixelBufferRef dst = make_buffer(cfg.destinationPixelBufferAttributes, ow, oh, err);
   if (!prev || !src || !dst) {
      L([NSString stringWithFormat:@"   buffers: %@", err]);
      if (prev)
         CVPixelBufferRelease(prev);
      if (src)
         CVPixelBufferRelease(src);
      if (dst)
         CVPixelBufferRelease(dst);
      [proc endSession];
      return;
   }
   fill(prev);
   fill(src);
   NSMutableArray<NSNumber *> *times = [NSMutableArray array];
   __block NSError *perr = nil;
   for (int i = 0; i < 80; i++) {
      VTFrameProcessorFrame *pf = [[VTFrameProcessorFrame alloc] initWithBuffer:prev
                                                          presentationTimeStamp:CMTimeMake(2 * i, 60)];
      VTFrameProcessorFrame *sf = [[VTFrameProcessorFrame alloc] initWithBuffer:src
                                                          presentationTimeStamp:CMTimeMake(2 * i + 2, 60)];
      VTFrameProcessorFrame *df = [[VTFrameProcessorFrame alloc] initWithBuffer:dst
                                                          presentationTimeStamp:CMTimeMake(2 * i + 1, 60)];
      VTLowLatencyFrameInterpolationParameters *p =
         [[VTLowLatencyFrameInterpolationParameters alloc] initWithSourceFrame:sf
                                                                 previousFrame:pf
                                                            interpolationPhase:@[ @0.5f ]
                                                             destinationFrames:@[ df ]];
      if (!pf || !sf || !df || !p) {
         L(@"   frame/parameters: nil");
         break;
      }
      dispatch_semaphore_t sem = dispatch_semaphore_create(0);
      double a = now_ms();
      [proc processWithParameters:p
                completionHandler:^(id<VTFrameProcessorParameters> done, NSError *pe) {
                   if (pe)
                      perr = pe;
                   dispatch_semaphore_signal(sem);
                }];
      dispatch_semaphore_wait(sem, DISPATCH_TIME_FOREVER);
      double b = now_ms();
      if (perr) {
         L([NSString stringWithFormat:@"   process failed at frame %d: %@", i, perr]);
         break;
      }
      if (i >= 10)
         [times addObject:@(b - a)];
   }
   if (!perr && times.count)
      L([NSString stringWithFormat:@"   per frame: %@", stats(times)]);
   [proc endSession];
   CVPixelBufferRelease(prev);
   CVPixelBufferRelease(src);
   CVPixelBufferRelease(dst);
}

static void metalfx_time(int w, int h, int ow, int oh, void (^L)(NSString *)) {
   L([NSString stringWithFormat:@"-- MetalFX spatial (GPU) %dx%d -> %dx%d", w, h, ow, oh]);
   id<MTLDevice> dev = MTLCreateSystemDefaultDevice();
   if (![MTLFXSpatialScalerDescriptor supportsDevice:dev]) {
      L(@"   not supported on this device");
      return;
   }
   MTLFXSpatialScalerDescriptor *d = [MTLFXSpatialScalerDescriptor new];
   d.inputWidth = w;
   d.inputHeight = h;
   d.outputWidth = ow;
   d.outputHeight = oh;
   d.colorTextureFormat = MTLPixelFormatBGRA8Unorm;
   d.outputTextureFormat = MTLPixelFormatBGRA8Unorm;
   d.colorProcessingMode = MTLFXSpatialScalerColorProcessingModePerceptual;
   id<MTLFXSpatialScaler> s = [d newSpatialScalerWithDevice:dev];
   if (!s) {
      L(@"   scaler: nil");
      return;
   }
   MTLTextureDescriptor *ti = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                                                  width:w
                                                                                 height:h
                                                                              mipmapped:NO];
   ti.usage = s.colorTextureUsage;
   ti.storageMode = MTLStorageModePrivate;
   MTLTextureDescriptor *to = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                                                  width:ow
                                                                                 height:oh
                                                                              mipmapped:NO];
   to.usage = s.outputTextureUsage;
   to.storageMode = MTLStorageModePrivate;
   s.colorTexture = [dev newTextureWithDescriptor:ti];
   s.outputTexture = [dev newTextureWithDescriptor:to];
   id<MTLCommandQueue> q = [dev newCommandQueue];
   NSMutableArray<NSNumber *> *gpu = [NSMutableArray array];
   for (int i = 0; i < 90; i++) {
      id<MTLCommandBuffer> cb = [q commandBuffer];
      [s encodeToCommandBuffer:cb];
      [cb commit];
      [cb waitUntilCompleted];
      if (i >= 10)
         [gpu addObject:@((cb.GPUEndTime - cb.GPUStartTime) * 1000.0)];
   }
   L([NSString stringWithFormat:@"   gpu time: %@", stats(gpu)]);
}

void nm_ane_test_run(void (^emit)(NSString *line), void (^done)(NSString *all)) {
   dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
      NSMutableString *all = [NSMutableString string];
      void (^L)(NSString *) = ^(NSString *s) {
         [all appendFormat:@"%@\n", s];
         dispatch_async(dispatch_get_main_queue(), ^{
            emit(s);
         });
      };
      L([NSString stringWithFormat:@"ANE test on %@, iOS %@", machine(), UIDevice.currentDevice.systemVersion]);
      if (@available(iOS 26.0, *)) {
         L([NSString stringWithFormat:@"VTLowLatencySuperResolutionScaler supported: %d",
                                      VTLowLatencySuperResolutionScalerConfiguration.isSupported]);
         int sizes[][2] = {{640, 360}, {960, 540}, {960, 544}, {960, 600}, {960, 608},
                           {1024, 768}, {1280, 720}, {1280, 800}, {1920, 1080}};
         NSMutableArray *timed = [NSMutableArray array];
         for (size_t i = 0; i < sizeof sizes / sizeof sizes[0]; i++) {
            NSArray<NSNumber *> *f = [VTLowLatencySuperResolutionScalerConfiguration
               supportedScaleFactorsForFrameWidth:sizes[i][0]
                                      frameHeight:sizes[i][1]];
            L([NSString stringWithFormat:@"  %dx%d scale factors: %@", sizes[i][0], sizes[i][1],
                                         f.count ? [f componentsJoinedByString:@","] : @"none"]);
            if ([f containsObject:@2.0f])
               [timed addObject:@[ @(sizes[i][0]), @(sizes[i][1]) ]];
         }
         if (VTLowLatencySuperResolutionScalerConfiguration.isSupported) {
            NSUInteger n = 0;
            for (NSArray *wh in timed) {
               int w = [wh[0] intValue], h = [wh[1] intValue];
               if (w < 900 || n >= 4)
                  continue;
               sr_time(w, h, 2.0f, L);
               n++;
            }
         }
         BOOL fiOk = VTLowLatencyFrameInterpolationConfiguration.isSupported;
         L([NSString stringWithFormat:@"VTLowLatencyFrameInterpolation supported: %d", fiOk]);
         if (fiOk) {
            fi_time(960, 544, 1, L);
            fi_time(960, 544, 2, L);
            fi_time(1920, 1080, 1, L);
         }
      } else {
         L(@"iOS < 26: no VideoToolbox frame processors");
      }
      metalfx_time(960, 544, 1920, 1088, L);
      metalfx_time(1366, 1024, 2732, 2048, L);
      dispatch_async(dispatch_get_main_queue(), ^{
         done(all);
      });
   });
}
