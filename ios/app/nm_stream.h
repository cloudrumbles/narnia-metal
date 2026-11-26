#import <Foundation/Foundation.h>
#import <QuartzCore/CAMetalLayer.h>

typedef NS_ENUM(NSInteger, NMUpscale) {
   NMUpscaleOff = 0,
   NMUpscaleMetalFX = 1,
   NMUpscaleANE = 2,
};

@interface NMStream : NSObject
@property(atomic) NMUpscale upscale;
@property(nonatomic, readonly) uint32_t streamWidth;
@property(nonatomic, readonly) uint32_t streamHeight;
@property(nonatomic, readonly) uint64_t framesShown;
@property(nonatomic, readonly) uint64_t bytesReceived;
@property(nonatomic, readonly) uint32_t audioDepthMs;
@property(atomic, copy, nullable) void (^onEnded)(NSString *_Nonnull reason);
+ (nullable instancetype)connectToURL:(nonnull NSString *)url
                             certHash:(nonnull NSString *)hash
                                layer:(nonnull CAMetalLayer *)layer
                                error:(NSString *_Nullable *_Nullable)error;
- (void)start;
- (void)stop;
- (void)keyHID:(uint32_t)usage pressed:(BOOL)pressed;
- (void)mouseDX:(double)dx dy:(double)dy;
- (void)mouseButton:(int)number pressed:(BOOL)pressed;
- (void)wheel:(int)direction;
- (void)padButtons:(uint32_t)buttons axes:(const int16_t *_Nonnull)axes;
- (void)pointerAtX:(double)x y:(double)y;
- (nonnull NSString *)takeStatsLine;
@end
