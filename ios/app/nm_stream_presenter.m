#import "nm_stream_presenter.h"
#import <Metal/Metal.h>
#import <MetalFX/MetalFX.h>
#include <stdatomic.h>

static NSString *const kShaderSource =
   @"#include <metal_stdlib>\n"
    "using namespace metal;\n"
    "struct VOut { float4 pos [[position]]; float2 uv; };\n"
    "vertex VOut fullscreen(uint vid [[vertex_id]]) {\n"
    "  float2 p = float2((vid << 1) & 2, vid & 2);\n"
    "  VOut o;\n"
    "  o.pos = float4(p * float2(2, -2) + float2(-1, 1), 0, 1);\n"
    "  o.uv = p;\n"
    "  return o;\n"
    "}\n"
    "fragment float4 yuv_to_rgb(VOut in [[stage_in]], texture2d<float> luma [[texture(0)]],\n"
    "                           texture2d<float> chroma [[texture(1)]]) {\n"
    "  constexpr sampler s(filter::linear, address::clamp_to_edge);\n"
    "  float y = (luma.sample(s, in.uv).r - 16.0 / 255.0) * (255.0 / 219.0);\n"
    "  float2 c = (chroma.sample(s, in.uv).rg - 128.0 / 255.0) * (255.0 / 224.0);\n"
    "  float3 rgb = float3(y + 1.5748 * c.y, y - 0.1873 * c.x - 0.4681 * c.y, y + 1.8556 * c.x);\n"
    "  return float4(saturate(rgb), 1.0);\n"
    "}\n"
    "fragment float4 copy_rgb(VOut in [[stage_in]], texture2d<float> src [[texture(0)]]) {\n"
    "  constexpr sampler s(filter::linear, address::clamp_to_edge);\n"
    "  return float4(src.sample(s, in.uv).rgb, 1.0);\n"
    "}\n";

@implementation NMStreamPresenter {
   CAMetalLayer *_layer;
   id<MTLDevice> _device;
   id<MTLCommandQueue> _queue;
   id<MTLRenderPipelineState> _yuvPipeline;
   id<MTLRenderPipelineState> _copyPipeline;
   CVMetalTextureCacheRef _textureCache;
   id<MTLTexture> _rgbTexture;
   id<MTLTexture> _upscaledTexture;
   id<MTLFXSpatialScaler> _scaler;
   atomic_uint_fast64_t _frames;
}

- (instancetype)initWithLayer:(CAMetalLayer *)layer {
   self = [super init];
   if (!self)
      return nil;
   _layer = layer;
   _device = MTLCreateSystemDefaultDevice();
   if (!_device)
      return nil;
   _queue = [_device newCommandQueue];
   layer.device = _device;
   layer.pixelFormat = MTLPixelFormatBGRA8Unorm;
   layer.framebufferOnly = YES;
   NSError *error = nil;
   id<MTLLibrary> library = [_device newLibraryWithSource:kShaderSource options:nil error:&error];
   if (!library) {
      fprintf(stderr, "stream presenter: shaders failed: %s\n",
              error.localizedDescription.UTF8String);
      return nil;
   }
   _yuvPipeline = [self pipelineWithLibrary:library fragment:@"yuv_to_rgb"];
   _copyPipeline = [self pipelineWithLibrary:library fragment:@"copy_rgb"];
   if (!_yuvPipeline || !_copyPipeline)
      return nil;
   if (CVMetalTextureCacheCreate(NULL, NULL, _device, NULL, &_textureCache) != kCVReturnSuccess)
      return nil;
   return self;
}

- (void)dealloc {
   if (_textureCache)
      CFRelease(_textureCache);
}

- (id<MTLRenderPipelineState>)pipelineWithLibrary:(id<MTLLibrary>)library
                                         fragment:(NSString *)fragment {
   MTLRenderPipelineDescriptor *d = [MTLRenderPipelineDescriptor new];
   d.vertexFunction = [library newFunctionWithName:@"fullscreen"];
   d.fragmentFunction = [library newFunctionWithName:fragment];
   d.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
   NSError *error = nil;
   id<MTLRenderPipelineState> state = [_device newRenderPipelineStateWithDescriptor:d error:&error];
   if (!state)
      fprintf(stderr, "stream presenter: pipeline %s failed: %s\n", fragment.UTF8String,
              error.localizedDescription.UTF8String);
   return state;
}

- (uint64_t)framesShown {
   return atomic_load(&_frames);
}

- (id<MTLTexture>)planeTexture:(CVPixelBufferRef)frame
                         plane:(size_t)plane
                        format:(MTLPixelFormat)format
                        holder:(NSMutableArray *)holder {
   CVMetalTextureRef ref = NULL;
   const size_t w = CVPixelBufferGetWidthOfPlane(frame, plane);
   const size_t h = CVPixelBufferGetHeightOfPlane(frame, plane);
   if (CVMetalTextureCacheCreateTextureFromImage(NULL, _textureCache, frame, NULL, format, w, h,
                                                 plane, &ref) != kCVReturnSuccess)
      return nil;
   [holder addObject:(__bridge_transfer id)ref];
   return CVMetalTextureGetTexture(ref);
}

- (BOOL)prepareMetalFXForWidth:(NSUInteger)w height:(NSUInteger)h {
   if (_scaler && _rgbTexture.width == w && _rgbTexture.height == h)
      return YES;
   if (![MTLFXSpatialScalerDescriptor supportsDevice:_device])
      return NO;
   MTLFXSpatialScalerDescriptor *d = [MTLFXSpatialScalerDescriptor new];
   d.inputWidth = w;
   d.inputHeight = h;
   d.outputWidth = 2 * w;
   d.outputHeight = 2 * h;
   d.colorTextureFormat = MTLPixelFormatBGRA8Unorm;
   d.outputTextureFormat = MTLPixelFormatBGRA8Unorm;
   d.colorProcessingMode = MTLFXSpatialScalerColorProcessingModePerceptual;
   id<MTLFXSpatialScaler> scaler = [d newSpatialScalerWithDevice:_device];
   if (!scaler)
      return NO;
   MTLTextureDescriptor *in = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                                                  width:w
                                                                                 height:h
                                                                              mipmapped:NO];
   in.usage = scaler.colorTextureUsage | MTLTextureUsageRenderTarget;
   in.storageMode = MTLStorageModePrivate;
   MTLTextureDescriptor *out = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                                                   width:2 * w
                                                                                  height:2 * h
                                                                               mipmapped:NO];
   out.usage = scaler.outputTextureUsage | MTLTextureUsageShaderRead;
   out.storageMode = MTLStorageModePrivate;
   _rgbTexture = [_device newTextureWithDescriptor:in];
   _upscaledTexture = [_device newTextureWithDescriptor:out];
   scaler.colorTexture = _rgbTexture;
   scaler.outputTexture = _upscaledTexture;
   _scaler = scaler;
   return _rgbTexture && _upscaledTexture;
}

- (MTLViewport)fitWidth:(double)w height:(double)h into:(id<MTLTexture>)target {
   const double tw = target.width, th = target.height;
   const double scale = MIN(tw / w, th / h);
   const double vw = w * scale, vh = h * scale;
   return (MTLViewport){(tw - vw) / 2, (th - vh) / 2, vw, vh, 0, 1};
}

- (void)drawWith:(id<MTLRenderPipelineState>)pipeline
        textures:(NSArray<id<MTLTexture>> *)textures
          target:(id<MTLTexture>)target
        viewport:(MTLViewport)viewport
          buffer:(id<MTLCommandBuffer>)buffer {
   MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor renderPassDescriptor];
   pass.colorAttachments[0].texture = target;
   pass.colorAttachments[0].loadAction = MTLLoadActionClear;
   pass.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 1);
   pass.colorAttachments[0].storeAction = MTLStoreActionStore;
   id<MTLRenderCommandEncoder> enc = [buffer renderCommandEncoderWithDescriptor:pass];
   [enc setRenderPipelineState:pipeline];
   [enc setViewport:viewport];
   for (NSUInteger i = 0; i < textures.count; i++)
      [enc setFragmentTexture:textures[i] atIndex:i];
   [enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
   [enc endEncoding];
}

- (void)present:(CVPixelBufferRef)frame metalFX:(BOOL)metalFX {
   NSMutableArray *holder = [NSMutableArray arrayWithCapacity:2];
   id<MTLTexture> luma = [self planeTexture:frame plane:0 format:MTLPixelFormatR8Unorm holder:holder];
   id<MTLTexture> chroma = [self planeTexture:frame plane:1 format:MTLPixelFormatRG8Unorm holder:holder];
   if (!luma || !chroma)
      return;
   id<CAMetalDrawable> drawable = [_layer nextDrawable];
   if (!drawable)
      return;
   id<MTLCommandBuffer> buffer = [_queue commandBuffer];
   const double w = luma.width, h = luma.height;
   if (metalFX && [self prepareMetalFXForWidth:luma.width height:luma.height]) {
      [self drawWith:_yuvPipeline
            textures:@[ luma, chroma ]
              target:_rgbTexture
            viewport:(MTLViewport){0, 0, w, h, 0, 1}
              buffer:buffer];
      [_scaler encodeToCommandBuffer:buffer];
      [self drawWith:_copyPipeline
            textures:@[ _upscaledTexture ]
              target:drawable.texture
            viewport:[self fitWidth:2 * w height:2 * h into:drawable.texture]
              buffer:buffer];
   } else {
      [self drawWith:_yuvPipeline
            textures:@[ luma, chroma ]
              target:drawable.texture
            viewport:[self fitWidth:w height:h into:drawable.texture]
              buffer:buffer];
   }
   [buffer presentDrawable:drawable];
   [buffer addCompletedHandler:^(id<MTLCommandBuffer> done) {
      [holder removeAllObjects];
   }];
   [buffer commit];
   atomic_fetch_add(&_frames, 1);
}

@end
