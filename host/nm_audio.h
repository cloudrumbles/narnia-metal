/*
 * narnia-metal host audio. Game audio arrives on its own stream from the
 * box audio daemon (box/nm-audio.c): Opus packets, no retransmission. The
 * core (nm_audio.c) parses the stream and hands packets to a sink module,
 * the same way nm_present.h hands frames to a presenter:
 *   - nm_audio_sink_wav.c: Linux/macOS CLI host, plays against a simulated
 *     device clock and writes a WAV (tests)
 *   - ios/app/nm_audio_ios.m: AudioUnit RemoteIO
 * Sinks that want decoding + jitter buffering use nm_audio_player.h.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef NM_AUDIO_H
#define NM_AUDIO_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "nm_host.h"

struct nm_audio_format {
   uint32_t sample_rate;
   uint32_t channels;
   uint32_t frame_samples; /* per channel per packet */
};

struct nm_audio_stats {
   uint32_t depth_ms;    /* decoded audio waiting to be played */
   uint32_t target_ms;   /* what the jitter buffer aims for */
   uint64_t packets;     /* received */
   uint64_t lost;        /* sequence gaps (concealed or skipped) */
   uint64_t underruns;   /* device ran dry */
   uint64_t dropped;     /* times audio was skipped to catch up */
   uint64_t late;        /* duplicates / out of order, ignored */
   uint32_t jitter_ms;   /* recent peak arrival jitter */
};

/* All callbacks for one stream run on one thread at a time, except that
 * stats may be called from any thread.
 */
struct nm_audio_sink_ops {
   /* a stream starts; returns the sink's per-stream state (NULL = refuse) */
   void *(*start)(void *user, const struct nm_audio_format *fmt);
   /* one packet, straight off the network. ts_us is the daemon's capture
    * time on its own clock; seq is consecutive unless packets were lost.
    */
   void (*packet)(void *sink, uint32_t seq, uint64_t ts_us,
                  const uint8_t *data, size_t len);
   void (*stop)(void *sink);
   /* optional */
   bool (*stats)(void *sink, struct nm_audio_stats *out);
};

/* Install the sink before any connection. With none installed the audio
 * daemon is told "not accepted" and keeps quiet.
 */
void
nm_host_set_audio_sink(const struct nm_audio_sink_ops *ops, void *user);

/* Stats of the running stream; false when there is none. */
bool
nm_host_audio_stats(struct nm_audio_stats *out);

/* Serve an audio stream whose NM_AUDIO_HELLO (len bytes) was just read by
 * nm_host_serve(). Returns when the stream ends.
 */
void
nm_audio_serve(const struct nm_transport *transport,
               const uint8_t *hello,
               size_t len);

#endif /* NM_AUDIO_H */
