/*
 * CLI test audio sink. See nm_audio_sink_wav.h.
 *
 * SPDX-License-Identifier: MIT
 */
#include "nm_audio_sink_wav.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "nm_audio_player.h"

#define TICK_MS 10

struct wav_sink {
   struct nm_audio_format fmt;
   struct nm_aplayer *player;
   FILE *wav;
   uint64_t wav_frames;
   pthread_t thread;
   atomic_bool stop;
   /* latency samples (ms * 10) */
   uint32_t *lat;
   size_t lat_n, lat_cap;
};

static void
wav_header(FILE *f, uint32_t rate, uint32_t ch, uint64_t frames)
{
   const uint32_t data = (uint32_t)(frames * ch * 2);
   uint8_t h[44];
   memcpy(h, "RIFF", 4);
   const uint32_t riff = 36 + data, brate = rate * ch * 2;
   memcpy(h + 4, &riff, 4);
   memcpy(h + 8, "WAVEfmt ", 8);
   const uint32_t fmtlen = 16;
   memcpy(h + 16, &fmtlen, 4);
   const uint16_t pcm = 1, chans = ch, align = ch * 2, bits = 16;
   memcpy(h + 20, &pcm, 2);
   memcpy(h + 22, &chans, 2);
   memcpy(h + 24, &rate, 4);
   memcpy(h + 28, &brate, 4);
   memcpy(h + 32, &align, 2);
   memcpy(h + 34, &bits, 2);
   memcpy(h + 36, "data", 4);
   memcpy(h + 40, &data, 4);
   fseek(f, 0, SEEK_SET);
   fwrite(h, 1, sizeof(h), f);
}

static int
cmp_u32(const void *a, const void *b)
{
   const uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;
   return x < y ? -1 : x > y;
}

static void
latency_summary(struct wav_sink *s)
{
   if (!s->lat_n) {
      fprintf(stderr, "nm-audio: no audio was played\n");
      return;
   }
   qsort(s->lat, s->lat_n, sizeof(uint32_t), cmp_u32);
   double sum = 0;
   for (size_t i = 0; i < s->lat_n; i++)
      sum += s->lat[i];
   fprintf(stderr,
           "nm-audio: capture->playout latency over %zu ticks: min %.1f "
           "p50 %.1f p95 %.1f p99 %.1f max %.1f mean %.1f ms\n",
           s->lat_n, s->lat[0] / 10.0, s->lat[s->lat_n / 2] / 10.0,
           s->lat[s->lat_n * 95 / 100] / 10.0, s->lat[s->lat_n * 99 / 100] / 10.0,
           s->lat[s->lat_n - 1] / 10.0, sum / s->lat_n / 10.0);
}

static void *
play_thread(void *arg)
{
   struct wav_sink *s = arg;
   const uint32_t n = s->fmt.sample_rate * TICK_MS / 1000;
   int16_t *buf = malloc((size_t)n * s->fmt.channels * 2);
   struct timespec next;
   clock_gettime(CLOCK_MONOTONIC, &next);
   uint64_t tick = 0, last_under = 0, last_drop = 0;
   uint64_t window_lat_sum = 0, window_lat_n = 0;

   while (!atomic_load(&s->stop)) {
      next.tv_nsec += TICK_MS * 1000000L;
      if (next.tv_nsec >= 1000000000L) {
         next.tv_nsec -= 1000000000L;
         next.tv_sec++;
      }
      clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next, NULL);

      uint64_t cap_ts = 0;
      const uint32_t real = nm_aplayer_pull(s->player, buf, n, &cap_ts);
      if (s->wav) {
         fwrite(buf, s->fmt.channels * 2, n, s->wav);
         s->wav_frames += n;
      }
      if (real == n && cap_ts) {
         /* the device would play the end of this buffer; count the start */
         const uint64_t lat = nm_audio_now_us() - cap_ts;
         if (s->lat_n == s->lat_cap) {
            s->lat_cap = s->lat_cap ? s->lat_cap * 2 : 4096;
            s->lat = realloc(s->lat, s->lat_cap * sizeof(uint32_t));
         }
         s->lat[s->lat_n++] = (uint32_t)(lat / 100);
         window_lat_sum += lat;
         window_lat_n++;
      }

      if (++tick % (1000 / TICK_MS) == 0) {
         struct nm_audio_stats st;
         nm_aplayer_stats(s->player, &st);
         fprintf(stderr,
                 "nm-audio: depth %u ms (target %u) lat %.1f ms jitter %u ms "
                 "pkts %llu lost %llu underruns %llu (+%llu) dropped %llu (+%llu)\n",
                 st.depth_ms, st.target_ms,
                 window_lat_n ? window_lat_sum / 1000.0 / window_lat_n : 0.0,
                 st.jitter_ms, (unsigned long long)st.packets,
                 (unsigned long long)st.lost, (unsigned long long)st.underruns,
                 (unsigned long long)(st.underruns - last_under),
                 (unsigned long long)st.dropped,
                 (unsigned long long)(st.dropped - last_drop));
         last_under = st.underruns;
         last_drop = st.dropped;
         window_lat_sum = window_lat_n = 0;
      }
   }
   free(buf);
   return NULL;
}

static void *
wav_start(void *user, const struct nm_audio_format *fmt)
{
   struct wav_sink *s = calloc(1, sizeof(*s));
   if (!s)
      return NULL;
   s->fmt = *fmt;
   s->player = nm_aplayer_create(fmt);
   if (!s->player) {
      free(s);
      return NULL;
   }
   const char *path = getenv("NM_AUDIO_WAV");
   if (path && *path) {
      s->wav = fopen(path, "wb");
      if (s->wav)
         wav_header(s->wav, fmt->sample_rate, fmt->channels, 0);
   }
   fprintf(stderr, "nm-audio: stream started, %u Hz, %u ch, %u-sample packets%s%s\n",
           fmt->sample_rate, fmt->channels, fmt->frame_samples,
           s->wav ? ", writing " : "", s->wav ? path : "");
   pthread_create(&s->thread, NULL, play_thread, s);
   return s;
}

static void
wav_packet(void *sink, uint32_t seq, uint64_t ts_us, const uint8_t *data,
           size_t len)
{
   struct wav_sink *s = sink;
   nm_aplayer_push(s->player, seq, ts_us, data, len, nm_audio_now_us());
}

static void
wav_stop(void *sink)
{
   struct wav_sink *s = sink;
   atomic_store(&s->stop, true);
   pthread_join(s->thread, NULL);
   latency_summary(s);
   if (s->wav) {
      wav_header(s->wav, s->fmt.sample_rate, s->fmt.channels, s->wav_frames);
      fclose(s->wav);
   }
   nm_aplayer_destroy(s->player);
   free(s->lat);
   free(s);
   fprintf(stderr, "nm-audio: stream ended\n");
}

static bool
wav_stats(void *sink, struct nm_audio_stats *out)
{
   nm_aplayer_stats(((struct wav_sink *)sink)->player, out);
   return true;
}

const struct nm_audio_sink_ops nm_audio_sink_wav = {
   .start = wav_start,
   .packet = wav_packet,
   .stop = wav_stop,
   .stats = wav_stats,
};
