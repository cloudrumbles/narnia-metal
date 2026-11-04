/*
 * Opus decoder + adaptive jitter buffer for a sink. Portable C (libopus only).
 *
 * The network thread calls push() for each packet; the device's real-time
 * callback calls pull() for exactly as many frames as it needs. pull() always
 * fills the output: with silence while the buffer fills (start, after an
 * underrun) and otherwise with audio, resampled by at most +-0.5% to slowly
 * steer the buffer depth to its target, which absorbs the clock difference
 * between the box and the device.
 *
 * Target depth follows the arrival jitter (packet timestamps against arrival
 * times): 25 ms + the recent peak, starting at ~50 ms, bumped on
 * underruns, at most 250 ms. If the depth ever exceeds the target by more than
 * max(target, 80 ms) (a TCP stall that arrives as a burst), the oldest audio
 * is dropped, so latency never builds up.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef NM_AUDIO_PLAYER_H
#define NM_AUDIO_PLAYER_H

#include "nm_audio.h"

struct nm_aplayer;

struct nm_aplayer *
nm_aplayer_create(const struct nm_audio_format *fmt);

void
nm_aplayer_destroy(struct nm_aplayer *p);

/* now_us: local monotonic clock, any epoch */
void
nm_aplayer_push(struct nm_aplayer *p,
                uint32_t seq,
                uint64_t ts_us,
                const uint8_t *data,
                size_t len,
                uint64_t now_us);

/* Fill out with `frames` interleaved int16 frames. Returns how many of them
 * are real audio (the rest is silence). *capture_ts_us (optional) gets the
 * capture timestamp of the audio being played, 0 if unknown.
 */
uint32_t
nm_aplayer_pull(struct nm_aplayer *p,
                int16_t *out,
                uint32_t frames,
                uint64_t *capture_ts_us);

void
nm_aplayer_stats(struct nm_aplayer *p, struct nm_audio_stats *out);

uint64_t
nm_audio_now_us(void);

#endif /* NM_AUDIO_PLAYER_H */
