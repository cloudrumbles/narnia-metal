#import "nm_coreml_sweep.h"
#import <CoreML/CoreML.h>
#import <UIKit/UIKit.h>
#include <mach/mach_time.h>
#include <math.h>
#include <sys/sysctl.h>

static double ms_now(void) {
   static mach_timebase_info_data_t tb;
   if (!tb.denom)
      mach_timebase_info(&tb);
   return (double)mach_absolute_time() * tb.numer / tb.denom / 1e6;
}

static NSString *hw_machine(void) {
   size_t n = 0;
   sysctlbyname("hw.machine", NULL, &n, NULL, 0);
   char *b = malloc(n);
   sysctlbyname("hw.machine", b, &n, NULL, 0);
   NSString *s = [NSString stringWithUTF8String:b];
   free(b);
   return s;
}

static double pct(NSArray<NSNumber *> *sorted, double q) {
   return sorted[(NSUInteger)((sorted.count - 1) * q)].doubleValue;
}

static NSString *summary(NSArray<NSNumber *> *a) {
   if (!a.count)
      return @"no samples";
   NSArray<NSNumber *> *s = [a sortedArrayUsingSelector:@selector(compare:)];
   return [NSString stringWithFormat:@"median %.2f p95 %.2f min %.2f ms (n=%lu)", pct(s, 0.5), pct(s, 0.95),
                                     s[0].doubleValue, (unsigned long)s.count];
}

static NSString *thermal(void) {
   switch (NSProcessInfo.processInfo.thermalState) {
   case NSProcessInfoThermalStateNominal:
      return @"nominal";
   case NSProcessInfoThermalStateFair:
      return @"fair";
   case NSProcessInfoThermalStateSerious:
      return @"serious";
   case NSProcessInfoThermalStateCritical:
      return @"critical";
   }
   return @"?";
}

static NSData *fetch(NSURL *url, NSString *method, NSData *body, NSError **err) {
   NSMutableURLRequest *r = [NSMutableURLRequest requestWithURL:url
                                                    cachePolicy:NSURLRequestReloadIgnoringLocalCacheData
                                                timeoutInterval:60];
   r.HTTPMethod = method;
   r.HTTPBody = body;
   if (body)
      [r setValue:@"text/plain; charset=utf-8" forHTTPHeaderField:@"Content-Type"];
   __block NSData *out = nil;
   __block NSError *e = nil;
   dispatch_semaphore_t sem = dispatch_semaphore_create(0);
   [[NSURLSession.sharedSession dataTaskWithRequest:r
                                  completionHandler:^(NSData *d, NSURLResponse *resp, NSError *re) {
                                     NSInteger code = [(NSHTTPURLResponse *)resp statusCode];
                                     if (re)
                                        e = re;
                                     else if (code < 200 || code >= 300)
                                        e = [NSError errorWithDomain:@"http"
                                                                code:code
                                                            userInfo:@{NSLocalizedDescriptionKey : url.absoluteString}];
                                     else
                                        out = d;
                                     dispatch_semaphore_signal(sem);
                                  }] resume];
   dispatch_semaphore_wait(sem, DISPATCH_TIME_FOREVER);
   if (err)
      *err = e;
   return out;
}

static NSURL *download(NSURL *base, NSDictionary *m, NSURL *dir, NSError **err) {
   NSFileManager *fm = NSFileManager.defaultManager;
   for (NSString *f in m[@"files"]) {
      NSData *d = fetch([NSURL URLWithString:f relativeToURL:base], @"GET", nil, err);
      if (!d)
         return nil;
      NSURL *dst = [dir URLByAppendingPathComponent:f];
      [fm createDirectoryAtURL:dst.URLByDeletingLastPathComponent withIntermediateDirectories:YES attributes:nil error:nil];
      if (![d writeToURL:dst options:NSDataWritingAtomic error:err])
         return nil;
   }
   return [dir URLByAppendingPathComponent:[m[@"name"] stringByAppendingString:@".mlpackage"]];
}

static MLMultiArray *half_array(NSArray<NSNumber *> *shape, BOOL fill, NSError **err) {
   MLMultiArray *a = [[MLMultiArray alloc] initWithShape:shape dataType:MLMultiArrayDataTypeFloat16 error:err];
   if (a && fill)
      [a getMutableBytesWithHandler:^(void *bytes, NSInteger size, NSArray<NSNumber *> *strides) {
         _Float16 *p = bytes;
         NSInteger n = size / (NSInteger)sizeof(_Float16);
         for (NSInteger i = 0; i < n; i++)
            p[i] = (_Float16)(0.5f + 0.4f * sinf((float)(i % 977) * 0.37f + (float)(i / 977) * 0.11f));
      }];
   return a;
}

API_AVAILABLE(ios(17.4))
static void count_ops(MLComputePlan *plan, MLModelStructureProgramBlock *blk, NSMutableDictionary<NSString *, NSNumber *> *n,
                      NSMutableSet<NSString *> *off) {
   for (MLModelStructureProgramOperation *op in blk.operations) {
      for (MLModelStructureProgramBlock *b in op.blocks)
         count_ops(plan, b, n, off);
      if ([op.operatorName isEqualToString:@"const"])
         continue;
      id<MLComputeDeviceProtocol> d = [plan computeDeviceUsageForMLProgramOperation:op].preferredComputeDevice;
      NSString *k = [(id)d isKindOfClass:MLNeuralEngineComputeDevice.class] ? @"ane"
                    : [(id)d isKindOfClass:MLGPUComputeDevice.class]       ? @"gpu"
                    : d                                                     ? @"cpu"
                                                                            : @"none";
      n[k] = @(n[k].integerValue + 1);
      if (![k isEqualToString:@"ane"])
         [off addObject:[NSString stringWithFormat:@"%@:%@", op.operatorName, k]];
   }
}

static NSString *placement(NSURL *compiled, MLModelConfiguration *cfg) {
   if (@available(iOS 17.4, *)) {
      __block NSString *out = @"placement: unavailable";
      dispatch_semaphore_t sem = dispatch_semaphore_create(0);
      [MLComputePlan loadContentsOfURL:compiled
                         configuration:cfg
                     completionHandler:^(MLComputePlan *plan, NSError *e) {
                        if (plan) {
                           NSMutableDictionary<NSString *, NSNumber *> *n = [NSMutableDictionary dictionary];
                           NSMutableSet<NSString *> *off = [NSMutableSet set];
                           for (MLModelStructureProgramFunction *fn in plan.modelStructure.program.functions.allValues)
                              count_ops(plan, fn.block, n, off);
                           out = [NSString stringWithFormat:@"placement: ane %ld gpu %ld cpu %ld%@%@", n[@"ane"].longValue,
                                                            n[@"gpu"].longValue, n[@"cpu"].longValue,
                                                            off.count ? @" off-ane " : @"",
                                                            [off.allObjects componentsJoinedByString:@","]];
                        } else {
                           out = [NSString stringWithFormat:@"placement: %@", e.localizedDescription];
                        }
                        dispatch_semaphore_signal(sem);
                     }];
      dispatch_semaphore_wait(sem, DISPATCH_TIME_FOREVER);
      return out;
   }
   return @"placement: needs iOS 17.4";
}

static void run_model(NSURL *base, NSDictionary *m, NSURL *dir, void (^L)(NSString *)) {
   NSString *name = m[@"name"];
   NSError *e = nil;
   double t0 = ms_now();
   NSURL *pkg = download(base, m, dir, &e);
   if (!pkg) {
      L([NSString stringWithFormat:@"%@: download failed: %@", name, e.localizedDescription]);
      return;
   }
   double t1 = ms_now();
   NSURL *compiled = [MLModel compileModelAtURL:pkg error:&e];
   if (!compiled) {
      L([NSString stringWithFormat:@"%@: compile failed: %@", name, e.localizedDescription]);
      return;
   }
   double t2 = ms_now();
   MLModelConfiguration *cfg = [MLModelConfiguration new];
   cfg.computeUnits = MLComputeUnitsCPUAndNeuralEngine;
   MLModel *model = [MLModel modelWithContentsOfURL:compiled configuration:cfg error:&e];
   if (!model) {
      L([NSString stringWithFormat:@"%@: load failed: %@", name, e.localizedDescription]);
      return;
   }
   double t3 = ms_now();
   L([NSString stringWithFormat:@"%@: download %.0f compile %.0f load %.0f ms; %@", name, t1 - t0, t2 - t1, t3 - t2,
                                placement(compiled, cfg)]);
   MLMultiArray *in = half_array(m[@"input_shape"], YES, &e);
   MLMultiArray *outArr = half_array(m[@"output_shape"], NO, &e);
   if (!in || !outArr) {
      L([NSString stringWithFormat:@"   arrays: %@", e.localizedDescription]);
      return;
   }
   MLDictionaryFeatureProvider *fp = [[MLDictionaryFeatureProvider alloc] initWithDictionary:@{m[@"input"] : in}
                                                                                       error:&e];
   MLPredictionOptions *opts = [MLPredictionOptions new];
   opts.outputBackings = @{m[@"output"] : outArr};
   NSMutableArray<NSNumber *> *times = [NSMutableArray array];
   for (int i = 0; i < 70; i++) {
      @autoreleasepool {
         double a = ms_now();
         id<MLFeatureProvider> r = [model predictionFromFeatures:fp options:opts error:&e];
         double b = ms_now();
         if (!r) {
            L([NSString stringWithFormat:@"   predict failed at %d: %@", i, e.localizedDescription]);
            return;
         }
         if (i >= 10)
            [times addObject:@(b - a)];
      }
   }
   NSArray<NSNumber *> *sorted = [times sortedArrayUsingSelector:@selector(compare:)];
   L([NSString stringWithFormat:@"   predict %@, thermal %@", summary(times), thermal()]);
   if (pct(sorted, 0.5) > 30)
      return;
   NSMutableArray<NSNumber *> *first = [NSMutableArray array], *last = [NSMutableArray array];
   double start = ms_now();
   while (ms_now() - start < 15000) {
      @autoreleasepool {
         double a = ms_now();
         if (![model predictionFromFeatures:fp options:opts error:&e])
            break;
         double b = ms_now();
         if (b - start < 5000)
            [first addObject:@(b - a)];
         else if (b - start > 10000)
            [last addObject:@(b - a)];
      }
   }
   L([NSString stringWithFormat:@"   sustained 15 s: first 5 s %@; last 5 s %@; thermal %@", summary(first), summary(last),
                                thermal()]);
}

void nm_coreml_sweep_run(NSString *baseString, void (^emit)(NSString *line), void (^done)(NSString *all)) {
   dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
      NSMutableString *all = [NSMutableString string];
      __block NSURL *results = nil;
      void (^L)(NSString *) = ^(NSString *s) {
         [all appendFormat:@"%@\n", s];
         if (results)
            fetch(results, @"POST", [[s stringByAppendingString:@"\n"] dataUsingEncoding:NSUTF8StringEncoding], nil);
         dispatch_async(dispatch_get_main_queue(), ^{
            emit(s);
         });
      };
      NSString *b = [baseString hasSuffix:@"/"] ? baseString : [baseString stringByAppendingString:@"/"];
      NSURL *base = [NSURL URLWithString:b];
      NSError *e = nil;
      NSData *md = base ? fetch([NSURL URLWithString:@"manifest.json" relativeToURL:base], @"GET", nil, &e) : nil;
      NSDictionary *man = md ? [NSJSONSerialization JSONObjectWithData:md options:0 error:&e] : nil;
      if (!man) {
         L([NSString stringWithFormat:@"sweep: no manifest at %@: %@", b, e.localizedDescription]);
         dispatch_async(dispatch_get_main_queue(), ^{
            done(all);
         });
         return;
      }
      if ([man[@"results_url"] isKindOfClass:NSString.class])
         results = [NSURL URLWithString:man[@"results_url"]];
      NSURL *dir = [NSFileManager.defaultManager.temporaryDirectory URLByAppendingPathComponent:@"ane-sweep"];
      [NSFileManager.defaultManager removeItemAtURL:dir error:nil];
      L([NSString stringWithFormat:@"model sweep on %@, iOS %@, %lu models, cpu+ane", hw_machine(),
                                   UIDevice.currentDevice.systemVersion, (unsigned long)[man[@"models"] count]]);
      for (NSDictionary *m in man[@"models"]) {
         @autoreleasepool {
            run_model(base, m, dir, L);
         }
      }
      L(@"sweep done");
      [NSFileManager.defaultManager removeItemAtURL:dir error:nil];
      dispatch_async(dispatch_get_main_queue(), ^{
         done(all);
      });
   });
}
