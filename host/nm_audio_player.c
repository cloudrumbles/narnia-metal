/*
 * See nm_audio_player.h.
 *
 * SPDX-License-Identifier: MIT
 */
#include "nm_audio_player.h"

#include <opus.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define RING_FRAMES 32768 /* power of two; ~680 ms at 48 kHz */
#define TS_RING 256
#define MIN_TARGET_US 25000
#define MAX_TARGET_US 250000
#define START_PEAK_US 25000
#define PEAK_TAU_US 4000000.0
#define MAX_CONCEAL 3
#define MAX_RATIO_ADJ 0.005
#define CONVERGE_SECS 1.5

struct nm_aplayer {
   struct nm_audio_format fmt;
   OpusDecoder *dec;

   /* ring and reader state, all under the lock. The critical sections are
    * a few microseconds (one frame memcpy / one device buffer).
    */
   pthread_mutex_t lock;
   int16_t *ring;
   uint64_t wr, rd; /* frames; wr is always a multiple of frame_samples */
   double frac;     /* reader position between rd and rd + 1 */
   double ratio;
   bool primed;
   uint64_t ts_ring[TS_RING]; /* capture ts per pushed frame */
   uint32_t target_frames;

   /* writer state (network thread only) */
   int16_t *pcm;
   bool have_seq;
   uint32_t next_seq;
   uint64_t last_arrival;
   bool have_win;
   int64_t win_start, cur_min, prev_min[3];
   double peak_us;
   uint64_t seen_underruns;

   /* stats, under the lock */
   uint64_t packets, lost, underruns, dropped, late, overflows;
   uint32_t jitter_us;
};

uint64_t
nm_audio_now_us(void)
{
   struct timespec ts;
   clock_gettime(CLOCK_MONOTONIC, &ts);
   return (uint64_t)ts.tv_sec * 1000000u + ts.tv_nsec / 1000u;
}

struct nm_aplayer *
nm_aplayer_create(const struct nm_audio_format *fmt)
{
   int err;
   struct nm_aplayer *p = calloc(1, sizeof(*p));
   if (!p)
      return NULL;
   pthread_mutex_init(&p->lock, NULL);
   p->fmt = *fmt;
   p->dec = opus_decoder_create(fmt->sample_rate, fmt->channels, &err);
   p->ring = calloc((size_t)RING_FRAMES * fmt->channels, sizeof(int16_t));
   p->pcm = calloc((size_t)fmt->frame_samples * fmt->channels, sizeof(int16_t));
   if (!p->dec || !p->ring || !p->pcm) {
      nm_aplayer_destroy(p);
      return NULL;
   }
   p->ratio = 1.0;
   p->peak_us = START_PEAK_US;
   p->target_frames = (uint64_t)fmt->sample_rate * 50 / 1000;
   return p;
}

void
nm_aplayer_destroy(struct nm_aplayer *p)
{
   if (!p)
      return;
   if (p->dec)
      opus_decoder_destroy(p->dec);
   free(p->ring);
   free(p->pcm);
   pthread_mutex_destroy(&p->lock);
   free(p);
}

static uint64_t
depth_frames(const struct nm_aplayer *p)
{
   return p->wr - p->rd;
}

/* lock held */
static void
drop_to_target(struct nm_aplayer *p)
{
   p->rd = p->wr - p->target_frames;
   p->frac = 0;
   p->dropped++;
}

/* lock held; the frame carries p->fmt.frame_samples samples */
static void
push_frame(struct nm_aplayer *p, const int16_t *pcm, uint64_t ts_us)
{
   const uint32_t n = p->fmt.frame_samples, ch = p->fmt.channels;
   if (depth_frames(p) + n > RING_FRAMES) {
      p->overflows++;
      drop_to_target(p);
   }
   const uint32_t pos = p->wr & (RING_FRAMES - 1);
   const uint32_t first = pos + n <= RING_FRAMES ? n : RING_FRAMES - pos;
   memcpy(p->ring + (size_t)pos * ch, pcm, (size_t)first * ch * 2);
   if (first < n)
      memcpy(p->ring, pcm + (size_t)first * ch, (size_t)(n - first) * ch * 2);
   p->ts_ring[(p->wr / n) % TS_RING] = ts_us;
   p->wr += n;

   /* a burst after a stall: skip the stale audio */
   const uint64_t slack = p->target_frames > p->fmt.sample_rate * 80 / 1000
                             ? p->target_frames
                             : p->fmt.sample_rate * 80 / 1000;
   if (depth_frames(p) > p->target_frames + slack)
      drop_to_target(p);
}

static void
update_target(struct nm_aplayer *p, uint64_t ts_us, uint64_t now_us)
{
   const int64_t transit = (int64_t)now_us - (int64_t)ts_us;
   if (!p->have_win) {
      p->have_win = true;
      p->win_start = now_us;
      p->cur_min = p->prev_min[0] = p->prev_min[1] = p->prev_min[2] = transit;
   }
   if ((int64_t)now_us - p->win_start > 2000000) {
      p->prev_min[2] = p->prev_min[1];
      p->prev_min[1] = p->prev_min[0];
      p->prev_min[0] = p->cur_min;
      p->cur_min = transit;
      p->win_start = now_us;
   }
   if (transit < p->cur_min)
      p->cur_min = transit;
   int64_t base = p->cur_min;
   for (int i = 0; i < 3; i++) {
      if (p->prev_min[i] < base)
         base = p->prev_min[i];
   }

   double delay = (double)(transit - base);
   if (delay > 100000)
      delay = 100000;
   if (p->last_arrival) {
      const double dt = (double)(now_us - p->last_arrival);
      p->peak_us -= p->peak_us * dt / PEAK_TAU_US;
   }
   p->last_arrival = now_us;
   if (delay > p->peak_us)
      p->peak_us = delay;

   pthread_mutex_lock(&p->lock);
   if (p->underruns != p->seen_underruns) {
      p->seen_underruns = p->underruns;
      p->peak_us += 10000;
   }
   double t = MIN_TARGET_US + p->peak_us;
   if (t > MAX_TARGET_US)
      t = MAX_TARGET_US;
   p->target_frames = (uint32_t)(t * p->fmt.sample_rate / 1e6);
   p->jitter_us = (uint32_t)p->peak_us;
   pthread_mutex_unlock(&p->lock);
}

void
nm_aplayer_push(struct nm_aplayer *p,
                uint32_t seq,
                uint64_t ts_us,
                const uint8_t *data,
                size_t len,
                uint64_t now_us)
{
   const uint32_t n = p->fmt.frame_samples;

   if (p->have_seq) {
      const int32_t gap = (int32_t)(seq - p->next_seq);
      if (gap < 0) {
         pthread_mutex_lock(&p->lock);
         p->late++;
         pthread_mutex_unlock(&p->lock);
         return;
      }
      if (gap > 0) {
         pthread_mutex_lock(&p->lock);
         p->lost += gap;
         pthread_mutex_unlock(&p->lock);
         /* conceal a few; beyond that the timeline just skips ahead */
         for (int32_t i = 0; i < gap && i < MAX_CONCEAL; i++) {
            if (opus_decode(p->dec, NULL, 0, p->pcm, n, 0) != (int)n)
               break;
            pthread_mutex_lock(&p->lock);
            push_frame(p, p->pcm, ts_us);
            pthread_mutex_unlock(&p->lock);
         }
      }
   }
   p->have_seq = true;
   p->next_seq = seq + 1;

   update_target(p, ts_us, now_us);

   const int got = opus_decode(p->dec, data, (opus_int32)len, p->pcm, n, 0);
   pthread_mutex_lock(&p->lock);
   p->packets++;
   if (got == (int)n)
      push_frame(p, p->pcm, ts_us);
   pthread_mutex_unlock(&p->lock);
}

uint32_t
nm_aplayer_pull(struct nm_aplayer *p,
                int16_t *out,
                uint32_t frames,
                uint64_t *capture_ts_us)
{
   const uint32_t ch = p->fmt.channels;
   uint32_t done = 0;

   pthread_mutex_lock(&p->lock);

   if (!p->primed && depth_frames(p) >= p->target_frames) {
      p->primed = true;
      p->ratio = 1.0;
   }

   if (p->primed) {
      /* steer the depth to the target: resample a little, with a dead zone */
      const double depth = (double)depth_frames(p) - p->frac;
      const double err = depth - p->target_frames;
      const double dead = p->fmt.sample_rate * 0.002;
      double want = 1.0;
      if (err > dead || err < -dead) {
         want = 1.0 + err / (p->fmt.sample_rate * CONVERGE_SECS / (err > 0 ? 4 : 1));
         /* well above the target (after a stall): catch up faster */
         const double up = err > p->fmt.sample_rate * 0.04 ? 4 * MAX_RATIO_ADJ
                                                           : MAX_RATIO_ADJ;
         if (want > 1.0 + up)
            want = 1.0 + up;
         if (want < 1.0 - MAX_RATIO_ADJ)
            want = 1.0 - MAX_RATIO_ADJ;
      }
      /* slew, so the pitch doesn't jump */
      p->ratio += (want - p->ratio) * 0.02;
      if (p->ratio > 0.9999 && p->ratio < 1.0001 && want == 1.0)
         p->ratio = 1.0;

      const uint32_t mask = RING_FRAMES - 1;
      uint64_t rd = p->rd;
      double frac = p->frac;
      for (; done < frames; done++) {
         const bool exact = frac == 0.0;
         if (exact ? rd >= p->wr : rd + 1 >= p->wr) {
            p->underruns++;
            p->primed = false;
            break;
         }
         const int16_t *a = p->ring + (size_t)(rd & mask) * ch;
         if (exact) {
            for (uint32_t c = 0; c < ch; c++)
               out[done * ch + c] = a[c];
         } else {
            const int16_t *b = p->ring + (size_t)((rd + 1) & mask) * ch;
            for (uint32_t c = 0; c < ch; c++)
               out[done * ch + c] = (int16_t)(a[c] + (b[c] - a[c]) * frac);
         }
         frac += p->ratio;
         while (frac >= 1.0) {
            frac -= 1.0;
            rd++;
         }
         /* keep exact reads exact when the ratio is 1 */
         if (p->ratio == 1.0)
            frac = 0.0;
      }
      p->rd = rd;
      p->frac = frac;
      if (capture_ts_us)
         *capture_ts_us = done ? p->ts_ring[(rd / p->fmt.frame_samples) % TS_RING] : 0;
   } else if (capture_ts_us) {
      *capture_ts_us = 0;
   }

   pthread_mutex_unlock(&p->lock);

   if (done < frames)
      memset(out + (size_t)done * ch, 0, (size_t)(frames - done) * ch * 2);
   return done;
}

void
nm_aplayer_stats(struct nm_aplayer *p, struct nm_audio_stats *out)
{
   pthread_mutex_lock(&p->lock);
   const double rate = p->fmt.sample_rate;
   out->depth_ms = (uint32_t)(depth_frames(p) * 1000.0 / rate);
   out->target_ms = (uint32_t)(p->target_frames * 1000.0 / rate);
   out->packets = p->packets;
   out->lost = p->lost;
   out->underruns = p->underruns;
   out->dropped = p->dropped;
   out->late = p->late;
   out->jitter_ms = p->jitter_us / 1000;
   pthread_mutex_unlock(&p->lock);
}
