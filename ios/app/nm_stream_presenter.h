#import <CoreVideo/CoreVideo.h>
#import <Foundation/Foundation.h>
#import <QuartzCore/CAMetalLayer.h>

@interface NMStreamPresenter : NSObject
@property(nonatomic, readonly) uint64_t framesShown;
- (nullable instancetype)initWithLayer:(nonnull CAMetalLayer *)layer;
- (void)present:(nonnull CVPixelBufferRef)frame metalFX:(BOOL)metalFX;
@end
