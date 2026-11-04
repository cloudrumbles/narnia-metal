/*
 * iPad audio sink. See nm_audio_ios.h.
 *
 * The RemoteIO render callback runs on the system's audio thread and pulls
 * straight from the jitter buffer; packets are pushed from the network
 * thread that serves the audio stream.
 *
 * SPDX-License-Identifier: MIT
 */
#import <AVFoundation/AVFoundation.h>
#import <AudioToolbox/AudioToolbox.h>

#include <stdio.h>
#include <stdlib.h>

#include "nm_audio_ios.h"
#include "nm_audio_player.h"

struct ios_sink {
   struct nm_audio_format fmt;
   struct nm_aplayer *player;
   AudioUnit unit;
   BOOL started;
};

/* the running sink, for interruption handling. Guarded by g_lock; never
 * taken together with anything that waits for the main queue. */
static struct ios_sink *g_sink;
static NSObject *g_lock;

static OSStatus
render(void *ref,
       AudioUnitRenderActionFlags *flags,
       const AudioTimeStamp *ts,
       UInt32 bus,
       UInt32 frames,
       AudioBufferList *io)
{
   struct ios_sink *s = ref;
   for (UInt32 i = 0; i < io->mNumberBuffers; i++) {
      AudioBuffer *b = &io->mBuffers[i];
      const UInt32 ch = b->mNumberChannels ? b->mNumberChannels : 1;
      const UInt32 n = b->mDataByteSize / (ch * sizeof(int16_t));
      nm_aplayer_pull(s->player, b->mData, n, NULL);
   }
   return noErr;
}

static BOOL
unit_start(struct ios_sink *s)
{
   NSError *err = nil;
   AVAudioSession *as = AVAudioSession.sharedInstance;
   /* playback: sounds with the silent switch on, and doesn't need the mic */
   [as setCategory:AVAudioSessionCategoryPlayback
              mode:AVAudioSessionModeDefault
           options:0
             error:&err];
   [as setPreferredSampleRate:s->fmt.sample_rate error:nil];
   [as setPreferredIOBufferDuration:0.005 error:nil];
   if (![as setActive:YES error:&err]) {
      fprintf(stderr, "audio: session activate failed: %s\n",
              err.localizedDescription.UTF8String);
      return NO;
   }

   if (!s->unit) {
      const AudioComponentDescription desc = {
         .componentType = kAudioUnitType_Output,
         .componentSubType = kAudioUnitSubType_RemoteIO,
         .componentManufacturer = kAudioUnitManufacturer_Apple,
      };
      AudioComponent comp = AudioComponentFindNextComponent(NULL, &desc);
      if (!comp || AudioComponentInstanceNew(comp, &s->unit) != noErr) {
         fprintf(stderr, "audio: no RemoteIO unit\n");
         return NO;
      }
      const AudioStreamBasicDescription asbd = {
         .mSampleRate = s->fmt.sample_rate,
         .mFormatID = kAudioFormatLinearPCM,
         .mFormatFlags = kAudioFormatFlagIsSignedInteger | kAudioFormatFlagIsPacked,
         .mBytesPerPacket = s->fmt.channels * 2,
         .mFramesPerPacket = 1,
         .mBytesPerFrame = s->fmt.channels * 2,
         .mChannelsPerFrame = s->fmt.channels,
         .mBitsPerChannel = 16,
      };
      const AURenderCallbackStruct cb = { .inputProc = render, .inputProcRefCon = s };
      OSStatus st = AudioUnitSetProperty(s->unit, kAudioUnitProperty_StreamFormat,
                                         kAudioUnitScope_Input, 0, &asbd, sizeof(asbd));
      if (st == noErr)
         st = AudioUnitSetProperty(s->unit, kAudioUnitProperty_SetRenderCallback,
                                   kAudioUnitScope_Input, 0, &cb, sizeof(cb));
      if (st == noErr)
         st = AudioUnitInitialize(s->unit);
      if (st != noErr) {
         fprintf(stderr, "audio: RemoteIO setup failed (%d)\n", (int)st);
         AudioComponentInstanceDispose(s->unit);
         s->unit = NULL;
         return NO;
      }
   }
   const OSStatus st = AudioOutputUnitStart(s->unit);
   if (st != noErr) {
      fprintf(stderr, "audio: couldn't start the output (%d)\n", (int)st);
      return NO;
   }
   s->started = YES;
   return YES;
}

static void
install_observers(void)
{
   static dispatch_once_t once;
   dispatch_once(&once, ^{
      /* a call or Siri stops the unit; start it again when that ends */
      [NSNotificationCenter.defaultCenter
         addObserverForName:AVAudioSessionInterruptionNotification
                     object:nil
                      queue:NSOperationQueue.mainQueue
                 usingBlock:^(NSNotification *n) {
                    const AVAudioSessionInterruptionType type =
                       [n.userInfo[AVAudioSessionInterruptionTypeKey] unsignedIntegerValue];
                    if (type != AVAudioSessionInterruptionTypeEnded)
                       return;
                    @synchronized(g_lock) {
                       if (g_sink) {
                          fprintf(stderr, "audio: interruption ended, restarting\n");
                          unit_start(g_sink);
                       }
                    }
                 }];
      [NSNotificationCenter.defaultCenter
         addObserverForName:AVAudioSessionMediaServicesWereResetNotification
                     object:nil
                      queue:NSOperationQueue.mainQueue
                 usingBlock:^(NSNotification *n) {
                    @synchronized(g_lock) {
                       if (g_sink) {
                          fprintf(stderr, "audio: media services reset, restarting\n");
                          if (g_sink->unit) {
                             AudioComponentInstanceDispose(g_sink->unit);
                             g_sink->unit = NULL;
                          }
                          unit_start(g_sink);
                       }
                    }
                 }];
   });
}

static void *
ios_start(void *user, const struct nm_audio_format *fmt)
{
   struct ios_sink *s = calloc(1, sizeof(*s));
   if (!s)
      return NULL;
   s->fmt = *fmt;
   s->player = nm_aplayer_create(fmt);
   if (!s->player) {
      free(s);
      return NULL;
   }
   /* the unit is started once the jitter buffer has something: the render
    * callback outputs silence until then anyway */
   if (!unit_start(s)) {
      nm_aplayer_destroy(s->player);
      free(s);
      return NULL;
   }
   static dispatch_once_t lock_once;
   dispatch_once(&lock_once, ^{ g_lock = [[NSObject alloc] init]; });
   @synchronized(g_lock) { g_sink = s; }
   dispatch_async(dispatch_get_main_queue(), ^{ install_observers(); });
   fprintf(stderr, "audio: playing %u Hz, %u ch, %u-sample packets\n",
           fmt->sample_rate, fmt->channels, fmt->frame_samples);
   return s;
}

static void
ios_packet(void *sink, uint32_t seq, uint64_t ts_us, const uint8_t *data,
           size_t len)
{
   struct ios_sink *s = sink;
   nm_aplayer_push(s->player, seq, ts_us, data, len, nm_audio_now_us());
}

static void
ios_stop(void *sink)
{
   struct ios_sink *s = sink;
   /* once the unit is gone nothing touches the player or s any more */
   @synchronized(g_lock) {
      if (g_sink == s)
         g_sink = NULL;
      if (s->unit) {
         AudioOutputUnitStop(s->unit);
         AudioUnitUninitialize(s->unit);
         AudioComponentInstanceDispose(s->unit);
         s->unit = NULL;
      }
   }
   [AVAudioSession.sharedInstance setActive:NO
                                withOptions:AVAudioSessionSetActiveOptionNotifyOthersOnDeactivation
                                      error:nil];
   nm_aplayer_destroy(s->player);
   free(s);
   fprintf(stderr, "audio: stopped\n");
}

static bool
ios_stats(void *sink, struct nm_audio_stats *out)
{
   nm_aplayer_stats(((struct ios_sink *)sink)->player, out);
   return true;
}

const struct nm_audio_sink_ops nm_audio_sink_ios = {
   .start = ios_start,
   .packet = ios_packet,
   .stop = ios_stop,
   .stats = ios_stats,
};
