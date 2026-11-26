#import "nm_stream.h"
#import "narnia_ios.h"
#import "nm_audio_ios.h"
#import "nm_stream_presenter.h"
#import <CoreMedia/CoreMedia.h>
#import <VideoToolbox/VideoToolbox.h>
#include <stdatomic.h>
#include <sys/time.h>

static const uint8_t kHidToEvdev[256] = {
     0,   0,   0,   0,  30,  48,  46,  32,  18,  33,  34,  35,  23,  36,  37,  38,
    50,  49,  24,  25,  16,  19,  31,  20,  22,  47,  17,  45,  21,  44,   2,   3,
     4,   5,   6,   7,   8,   9,  10,  11,  28,   1,  14,  15,  57,  12,  13,  26,
    27,  43,  43,  39,  40,  41,  51,  52,  53,  58,  59,  60,  61,  62,  63,  64,
    65,  66,  67,  68,  87,  88,  99,  70, 119, 110, 102, 104, 111, 107, 109, 106,
   105, 108, 103,  69,  98,  55,  74,  78,  96,  79,  80,  81,  75,  76,  77,  71,
    72,  73,  82,  83,  86, 127, 116, 117, 183, 184, 185, 186, 187, 188, 189, 190,
   191, 192, 193, 194, 134, 138, 130, 132, 128, 129, 131, 137, 133, 135, 136, 113,
   115, 114,   0,   0,   0, 121,   0,  89,  93, 124,  92,  94,  95,   0,   0,   0,
   122, 123,  90,  91,  85,   0,   0,   0,   0,   0,   0,   0, 111,   0,   0,   0,
     0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
     0,   0,   0,   0,   0,   0, 179, 180,   0,   0,   0,   0,   0,   0,   0,   0,
     0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
     0,   0,   0,   0,   0,   0,   0,   0, 111,   0,   0,   0,   0,   0,   0,   0,
    29,  42,  56, 125,  97,  54, 100, 126, 164, 166, 165, 163, 161, 115, 114, 113,
   150, 158, 159, 128, 136, 177, 178, 176, 142, 152, 173, 140,   0,   0,   0,   0,
};

enum { NAL_VPS = 32, NAL_SPS = 33, NAL_PPS = 34, NAL_AUD = 35 };

typedef struct {
   const uint8_t *data;
   size_t len;
} NalUnit;

static NSArray<NSData *> *split_annexb(const uint8_t *p, size_t n) {
   NSMutableArray<NSData *> *units = [NSMutableArray array];
   size_t i = 0, start = SIZE_MAX;
   while (i + 3 <= n) {
      const BOOL three = p[i] == 0 && p[i + 1] == 0 && p[i + 2] == 1;
      if (three) {
         if (start != SIZE_MAX) {
            size_t end = i;
            while (end > start && p[end - 1] == 0)
               end--;
            [units addObject:[NSData dataWithBytes:p + start length:end - start]];
         }
         i += 3;
         start = i;
      } else {
         i++;
      }
   }
   if (start != SIZE_MAX && start < n)
      [units addObject:[NSData dataWithBytes:p + start length:n - start]];
   return units;
}

static int nal_type(NSData *unit) {
   return unit.length ? (((const uint8_t *)unit.bytes)[0] >> 1) & 0x3f : -1;
}

static uint64_t now_us(void) {
   struct timeval tv;
   gettimeofday(&tv, NULL);
   return (uint64_t)tv.tv_sec * 1000000u + (uint64_t)tv.tv_usec;
}

@implementation NMStream {
   NarniaSession *_session;
   NMStreamPresenter *_presenter;
   atomic_bool _stop;
   CMVideoFormatDescriptionRef _format;
   VTDecompressionSessionRef _decoder;
   NSArray<NSData *> *_parameterSets;
   id _aneProcessor;
   id _aneConfig;
   CVPixelBufferPoolRef _anePool;
   uint32_t _aneWidth;
   uint32_t _aneHeight;
   BOOL _aneUnavailable;
   dispatch_group_t _threads;
   void *_audioSink;
   uint64_t _statsAt;
   uint64_t _shownAt;
   uint64_t _decoded;
   uint64_t _decodeErrors;
   double _decodeMsSum;
   double _decodeMsMax;
   double _gapMsMax;
   uint64_t _lastFrameAt;
   uint64_t _inputs;
   uint32_t _lastPadButtons;
   int16_t _lastPadAxes[6];
   BOOL _havePad;
}

+ (instancetype)connectToURL:(NSString *)url
                    certHash:(NSString *)hash
                       layer:(CAMetalLayer *)layer
                       error:(NSString **)error {
   char message[512] = {0};
   NarniaSession *session =
      narnia_connect(url.UTF8String, hash.UTF8String, 8000, message, sizeof(message));
   if (!session) {
      if (error)
         *error = [NSString stringWithUTF8String:message] ?: @"connect failed";
      return nil;
   }
   NMStreamPresenter *presenter = [[NMStreamPresenter alloc] initWithLayer:layer];
   if (!presenter) {
      narnia_close(session);
      if (error)
         *error = @"Metal presenter could not start";
      return nil;
   }
   return [[NMStream alloc] initWithSession:session presenter:presenter];
}

- (instancetype)initWithSession:(NarniaSession *)session presenter:(NMStreamPresenter *)presenter {
   self = [super init];
   if (!self)
      return nil;
   _session = session;
   _presenter = presenter;
   NarniaStreamInfo info = narnia_stream_info(session);
   _streamWidth = info.width;
   _streamHeight = info.height;
   _threads = dispatch_group_create();
   return self;
}

- (void)dealloc {
   [self teardownDecoder];
   [self teardownANE];
   if (_session)
      narnia_close(_session);
}

- (uint64_t)framesShown {
   return _presenter.framesShown;
}

- (void)start {
   dispatch_group_enter(_threads);
   [NSThread detachNewThreadWithBlock:^{
      [self videoLoop];
      dispatch_group_leave(self->_threads);
   }];
   dispatch_group_enter(_threads);
   [NSThread detachNewThreadWithBlock:^{
      [self audioLoop];
      dispatch_group_leave(self->_threads);
   }];
}

- (void)stop {
   atomic_store(&_stop, true);
   dispatch_group_wait(_threads, dispatch_time(DISPATCH_TIME_NOW, 2 * NSEC_PER_SEC));
}

- (void)endWith:(NSString *)reason {
   if (atomic_exchange(&_stop, true))
      return;
   void (^ended)(NSString *) = self.onEnded;
   if (ended)
      dispatch_async(dispatch_get_main_queue(), ^{
         ended(reason);
      });
}

- (void)videoLoop {
   NarniaVideoFrame frame;
   const uint64_t stall_us = 3000000;
   uint64_t lastProgress = now_us();
   while (!atomic_load(&_stop)) {
      const int32_t got = narnia_next_video(_session, 100, &frame);
      if (got < 0) {
         [self endWith:@"stream closed"];
         return;
      }
      if (got == 1)
         lastProgress = now_us();
      else if (now_us() - lastProgress > stall_us) {
         fprintf(stderr, "stream: no video for 3 s, reconnecting\n");
         [self endWith:@"stream stalled, reconnecting"];
         return;
      }
      if (got == 1) {
         const uint64_t arrived = now_us();
         @synchronized(self) {
            if (_lastFrameAt) {
               const double gap = (double)(arrived - _lastFrameAt) / 1000.0;
               if (gap > _gapMsMax)
                  _gapMsMax = gap;
            }
            _lastFrameAt = arrived;
         }
         @autoreleasepool {
            [self decode:&frame];
         }
         const double took = (double)(now_us() - arrived) / 1000.0;
         @synchronized(self) {
            _decodeMsSum += took;
            if (took > _decodeMsMax)
               _decodeMsMax = took;
         }
      }
   }
}

- (void)audioLoop {
   const struct nm_audio_format format = {.sample_rate = 48000, .channels = 2, .frame_samples = 960};
   void *sink = nm_audio_sink_ios.start(NULL, &format);
   @synchronized(self) {
      _audioSink = sink;
   }
   const uint64_t frame_us = 20000;
   const uint64_t resync_after_us = 150000;
   uint32_t seq = 0;
   uint64_t sender_clock = 0, last_arrival = 0;
   NarniaAudioPacket packet;
   while (!atomic_load(&_stop)) {
      const int32_t got = narnia_next_audio(_session, 100, &packet);
      if (got < 0)
         break;
      if (got != 1 || !sink)
         continue;
      const uint64_t arrival = now_us();
      sender_clock = !last_arrival || arrival - last_arrival > resync_after_us ? arrival
                                                                                : sender_clock + frame_us;
      last_arrival = arrival;
      nm_audio_sink_ios.packet(sink, seq++, sender_clock, packet.data, packet.len);
   }
   @synchronized(self) {
      _audioSink = NULL;
   }
   if (sink)
      nm_audio_sink_ios.stop(sink);
}

- (void)teardownDecoder {
   if (_decoder) {
      VTDecompressionSessionInvalidate(_decoder);
      CFRelease(_decoder);
      _decoder = NULL;
   }
   if (_format) {
      CFRelease(_format);
      _format = NULL;
   }
}

- (BOOL)configureWithParameterSets:(NSArray<NSData *> *)sets {
   if (_decoder && [sets isEqualToArray:_parameterSets])
      return YES;
   [self teardownDecoder];
   const uint8_t *pointers[3];
   size_t sizes[3];
   for (NSUInteger i = 0; i < 3; i++) {
      pointers[i] = sets[i].bytes;
      sizes[i] = sets[i].length;
   }
   OSStatus status = CMVideoFormatDescriptionCreateFromHEVCParameterSets(
      NULL, 3, pointers, sizes, 4, NULL, &_format);
   if (status != noErr) {
      fprintf(stderr, "stream: HEVC format description failed (%d)\n", (int)status);
      return NO;
   }
   NSDictionary *attributes = @{
      (id)kCVPixelBufferPixelFormatTypeKey : @(kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange),
      (id)kCVPixelBufferIOSurfacePropertiesKey : @{},
      (id)kCVPixelBufferMetalCompatibilityKey : @YES,
   };
   status = VTDecompressionSessionCreate(NULL, _format, NULL,
                                         (__bridge CFDictionaryRef)attributes, NULL, &_decoder);
   if (status != noErr) {
      fprintf(stderr, "stream: HEVC decoder failed (%d)\n", (int)status);
      [self teardownDecoder];
      return NO;
   }
   VTSessionSetProperty(_decoder, kVTDecompressionPropertyKey_RealTime, kCFBooleanTrue);
   _parameterSets = sets;
   CMVideoDimensions dims = CMVideoFormatDescriptionGetDimensions(_format);
   fprintf(stderr, "stream: decoding %dx%d\n", dims.width, dims.height);
   return YES;
}

- (void)decode:(const NarniaVideoFrame *)frame {
   NSArray<NSData *> *units = split_annexb(frame->data, frame->len);
   NSData *vps = nil, *sps = nil, *pps = nil;
   NSMutableData *sample = [NSMutableData dataWithCapacity:frame->len + 4 * units.count];
   for (NSData *unit in units) {
      const int type = nal_type(unit);
      if (type == NAL_VPS)
         vps = unit;
      else if (type == NAL_SPS)
         sps = unit;
      else if (type == NAL_PPS)
         pps = unit;
      else if (type != NAL_AUD) {
         const uint32_t be = CFSwapInt32HostToBig((uint32_t)unit.length);
         [sample appendBytes:&be length:4];
         [sample appendData:unit];
      }
   }
   if (vps && sps && pps && ![self configureWithParameterSets:@[ vps, sps, pps ]])
      return;
   if (!_decoder || !sample.length)
      return;

   CMBlockBufferRef block = NULL;
   if (CMBlockBufferCreateWithMemoryBlock(NULL, NULL, sample.length, NULL, NULL, 0, sample.length,
                                          0, &block) != noErr)
      return;
   CMBlockBufferReplaceDataBytes(sample.bytes, block, 0, sample.length);
   CMSampleBufferRef buffer = NULL;
   const size_t size = sample.length;
   const CMSampleTimingInfo timing = {kCMTimeInvalid, CMTimeMake((int64_t)frame->capture_us, 1000000),
                                      kCMTimeInvalid};
   const OSStatus made =
      CMSampleBufferCreateReady(NULL, block, _format, 1, 1, &timing, 1, &size, &buffer);
   CFRelease(block);
   if (made != noErr)
      return;
   VTDecodeInfoFlags info = 0;
   const OSStatus decoded = VTDecompressionSessionDecodeFrameWithOutputHandler(
      _decoder, buffer, 0, &info,
      ^(OSStatus status, VTDecodeInfoFlags flags, CVImageBufferRef image, CMTime pts,
        CMTime duration) {
         @synchronized(self) {
            if (status == noErr && image)
               self->_decoded++;
            else
               self->_decodeErrors++;
         }
         if (status == noErr && image)
            [self show:image];
      });
   CFRelease(buffer);
   if (decoded == kVTInvalidSessionErr) {
      [self teardownDecoder];
      _parameterSets = nil;
   }
}

- (void)show:(CVPixelBufferRef)image {
   const NMUpscale mode = self.upscale;
   CVPixelBufferRef shown = image;
   CVPixelBufferRef upscaled = NULL;
   if (mode == NMUpscaleANE) {
      upscaled = [self upscaleOnANE:image];
      if (upscaled)
         shown = upscaled;
   }
   [_presenter present:shown metalFX:mode == NMUpscaleMetalFX];
   if (upscaled)
      CVPixelBufferRelease(upscaled);
}

- (void)teardownANE {
   if (@available(iOS 26.0, *)) {
      if (_aneProcessor)
         [(VTFrameProcessor *)_aneProcessor endSession];
   }
   _aneProcessor = nil;
   _aneConfig = nil;
   if (_anePool) {
      CVPixelBufferPoolRelease(_anePool);
      _anePool = NULL;
   }
}

- (CVPixelBufferRef)upscaleOnANE:(CVPixelBufferRef)image {
   if (@available(iOS 26.0, *)) {
      const uint32_t w = (uint32_t)CVPixelBufferGetWidth(image);
      const uint32_t h = (uint32_t)CVPixelBufferGetHeight(image);
      if (w != _aneWidth || h != _aneHeight) {
         [self teardownANE];
         _aneWidth = w;
         _aneHeight = h;
         _aneUnavailable = ![self startANEForWidth:w height:h];
      }
      if (_aneUnavailable)
         return NULL;
      CVPixelBufferRef out = NULL;
      if (CVPixelBufferPoolCreatePixelBuffer(NULL, _anePool, &out) != kCVReturnSuccess)
         return NULL;
      VTFrameProcessorFrame *source =
         [[VTFrameProcessorFrame alloc] initWithBuffer:image presentationTimeStamp:kCMTimeZero];
      VTFrameProcessorFrame *destination =
         [[VTFrameProcessorFrame alloc] initWithBuffer:out presentationTimeStamp:kCMTimeZero];
      VTLowLatencySuperResolutionScalerParameters *parameters =
         [[VTLowLatencySuperResolutionScalerParameters alloc] initWithSourceFrame:source
                                                                destinationFrame:destination];
      if (!parameters) {
         CVPixelBufferRelease(out);
         return NULL;
      }
      dispatch_semaphore_t done = dispatch_semaphore_create(0);
      __block BOOL ok = NO;
      [(VTFrameProcessor *)_aneProcessor processWithParameters:parameters
                                              completionHandler:^(id<VTFrameProcessorParameters> p,
                                                                  NSError *e) {
                                                 ok = e == nil;
                                                 dispatch_semaphore_signal(done);
                                              }];
      dispatch_semaphore_wait(done, DISPATCH_TIME_FOREVER);
      if (!ok) {
         CVPixelBufferRelease(out);
         return NULL;
      }
      return out;
   }
   return NULL;
}

- (BOOL)startANEForWidth:(uint32_t)w height:(uint32_t)h API_AVAILABLE(ios(26.0)) {
   if (!VTLowLatencySuperResolutionScalerConfiguration.isSupported) {
      fprintf(stderr, "stream: ANE super resolution not supported here\n");
      return NO;
   }
   NSArray<NSNumber *> *factors =
      [VTLowLatencySuperResolutionScalerConfiguration supportedScaleFactorsForFrameWidth:w
                                                                             frameHeight:h];
   if (![factors containsObject:@2.0f]) {
      fprintf(stderr, "stream: ANE has no 2x for %ux%u, showing it as is\n", w, h);
      return NO;
   }
   VTLowLatencySuperResolutionScalerConfiguration *config =
      [[VTLowLatencySuperResolutionScalerConfiguration alloc] initWithFrameWidth:w
                                                                     frameHeight:h
                                                                     scaleFactor:2.0f];
   VTFrameProcessor *processor = [[VTFrameProcessor alloc] init];
   NSError *error = nil;
   if (!config || ![processor startSessionWithConfiguration:config error:&error]) {
      fprintf(stderr, "stream: ANE session failed: %s\n", error.localizedDescription.UTF8String ?: "?");
      return NO;
   }
   NSMutableDictionary *attributes = [config.destinationPixelBufferAttributes mutableCopy]
                                        ?: [NSMutableDictionary dictionary];
   attributes[(id)kCVPixelBufferWidthKey] = @(2 * w);
   attributes[(id)kCVPixelBufferHeightKey] = @(2 * h);
   attributes[(id)kCVPixelBufferMetalCompatibilityKey] = @YES;
   if (!attributes[(id)kCVPixelBufferIOSurfacePropertiesKey])
      attributes[(id)kCVPixelBufferIOSurfacePropertiesKey] = @{};
   if (CVPixelBufferPoolCreate(NULL, NULL, (__bridge CFDictionaryRef)attributes, &_anePool) !=
       kCVReturnSuccess) {
      [processor endSession];
      return NO;
   }
   _aneConfig = config;
   _aneProcessor = processor;
   fprintf(stderr, "stream: ANE upscaling %ux%u -> %ux%u\n", w, h, 2 * w, 2 * h);
   return YES;
}

- (NSString *)takeStatsLine {
   const uint64_t now = now_us();
   const uint64_t shown = _presenter.framesShown;
   struct nm_audio_stats audio = {0};
   BOOL haveAudio = NO;
   uint64_t decoded, errors, inputs;
   double decodeSum, decodeMax, gapMax, seconds;
   @synchronized(self) {
      if (_audioSink && nm_audio_sink_ios.stats)
         haveAudio = nm_audio_sink_ios.stats(_audioSink, &audio);
      seconds = _statsAt ? (double)(now - _statsAt) / 1e6 : 0;
      decoded = _decoded;
      errors = _decodeErrors;
      decodeSum = _decodeMsSum;
      decodeMax = _decodeMsMax;
      gapMax = _gapMsMax;
      inputs = _inputs;
      _inputs = 0;
      _decoded = 0;
      _decodeErrors = 0;
      _decodeMsSum = 0;
      _decodeMsMax = 0;
      _gapMsMax = 0;
      _statsAt = now;
   }
   const double fps = seconds > 0 ? (double)(shown - _shownAt) / seconds : 0;
   _shownAt = shown;
   static const char *const upscaleNames[] = {"off", "metalfx", "ane"};
   const NMUpscale mode = self.upscale;
   NSMutableString *line = [NSMutableString
      stringWithFormat:@"stats size=%ux%u upscale=%s fps=%.1f decoded=%llu decode_err=%llu "
                        @"decode_ms_avg=%.1f decode_ms_max=%.1f frame_gap_ms_max=%.0f inputs=%llu",
                       _streamWidth, _streamHeight, upscaleNames[mode >= 0 && mode <= 2 ? mode : 0],
                       fps, (unsigned long long)decoded, (unsigned long long)errors,
                       decoded ? decodeSum / (double)decoded : 0.0, decodeMax, gapMax,
                       (unsigned long long)inputs];
   if (haveAudio)
      [line appendFormat:@" aud_depth_ms=%u aud_target_ms=%u aud_jitter_ms=%u aud_under=%llu "
                         @"aud_lost=%llu aud_drop=%llu aud_pkts=%llu",
                         audio.depth_ms, audio.target_ms, audio.jitter_ms,
                         (unsigned long long)audio.underruns, (unsigned long long)audio.lost,
                         (unsigned long long)audio.dropped, (unsigned long long)audio.packets];
   return line;
}

- (void)countInput {
   @synchronized(self) {
      _inputs++;
   }
}

- (void)keyHID:(uint32_t)usage pressed:(BOOL)pressed {
   const uint8_t code = usage < 256 ? kHidToEvdev[usage] : 0;
   if (!code)
      return;
   narnia_send_key(_session, code, pressed, false);
   [self countInput];
}

- (void)mouseDX:(double)dx dy:(double)dy {
   narnia_send_pointer_relative(_session, dx, dy);
   [self countInput];
}

- (void)mouseButton:(int)number pressed:(BOOL)pressed {
   static const uint16_t buttons[] = {0, 0x110, 0x112, 0x111};
   if (number < 1 || number > 3)
      return;
   narnia_send_mouse_button(_session, buttons[number], pressed);
   [self countInput];
}

- (void)wheel:(int)direction {
   narnia_send_scroll(_session, 0, direction);
   [self countInput];
}

- (void)padButtons:(uint32_t)buttons axes:(const int16_t *)axes {
   narnia_send_gamepad(_session, buttons, axes);
   BOOL changed;
   @synchronized(self) {
      changed = !_havePad || buttons != _lastPadButtons ||
                memcmp(axes, _lastPadAxes, sizeof(_lastPadAxes)) != 0;
      _havePad = YES;
      _lastPadButtons = buttons;
      memcpy(_lastPadAxes, axes, sizeof(_lastPadAxes));
   }
   if (changed)
      [self countInput];
}

- (void)pointerAtX:(double)x y:(double)y {
   narnia_send_pointer_absolute(_session, x * _streamWidth, y * _streamHeight, _streamWidth,
                                _streamHeight);
   [self countInput];
}

@end
