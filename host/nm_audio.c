/*
 * Audio stream handling in the host core. See nm_audio.h.
 *
 * SPDX-License-Identifier: MIT
 */
#include "nm_audio.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nm_proto.h"

static pthread_mutex_t audio_lock = PTHREAD_MUTEX_INITIALIZER;
static const struct nm_audio_sink_ops *sink_ops;
static void *sink_user;

struct audio_stream {
   void *sink;
   atomic_bool superseded;
};
static struct audio_stream *current; /* under audio_lock */

void
nm_host_set_audio_sink(const struct nm_audio_sink_ops *ops, void *user)
{
   sink_ops = ops;
   sink_user = user;
}

bool
nm_host_audio_stats(struct nm_audio_stats *out)
{
   bool ok = false;
   pthread_mutex_lock(&audio_lock);
   if (current && sink_ops && sink_ops->stats)
      ok = sink_ops->stats(current->sink, out);
   pthread_mutex_unlock(&audio_lock);
   return ok;
}

static bool
valid_rate(uint32_t r)
{
   return r == 8000 || r == 12000 || r == 16000 || r == 24000 || r == 48000;
}

static void
reply(const struct nm_transport *t, bool accepted)
{
   struct {
      struct nm_hdr hdr;
      struct nm_audio_hello_reply r;
   } msg = {
      .hdr = { NM_AUDIO_HELLO_REPLY, sizeof(struct nm_audio_hello_reply) },
      .r = { NM_MAGIC, NM_VERSION, accepted, 0 },
   };
   t->write(t->user, &msg, sizeof(msg));
}

void
nm_audio_serve(const struct nm_transport *t, const uint8_t *hello, size_t len)
{
   struct nm_audio_hello h;
   if (len < sizeof(h)) {
      return;
   }
   memcpy(&h, hello, sizeof(h));
   if (h.magic != NM_MAGIC || h.version != NM_VERSION ||
       h.codec != NM_AUDIO_CODEC_OPUS || !valid_rate(h.sample_rate) ||
       h.channels < 1 || h.channels > 2 || h.frame_samples < 1 ||
       h.frame_samples > h.sample_rate / 5) {
      reply(t, false);
      return;
   }
   if (!sink_ops) {
      reply(t, false);
      return;
   }

   struct audio_stream *me = calloc(1, sizeof(*me));
   if (!me)
      return;
   const struct nm_audio_format fmt = {
      .sample_rate = h.sample_rate,
      .channels = h.channels,
      .frame_samples = h.frame_samples,
   };

   /* a new stream replaces the old one (the daemon restarted) */
   pthread_mutex_lock(&audio_lock);
   if (current) {
      current->superseded = true;
      sink_ops->stop(current->sink);
      current = NULL;
   }
   me->sink = sink_ops->start(sink_user, &fmt);
   if (me->sink)
      current = me;
   pthread_mutex_unlock(&audio_lock);
   if (!me->sink) {
      reply(t, false);
      free(me);
      return;
   }
   reply(t, true);

   uint8_t buf[sizeof(struct nm_audio_packet) + NM_AUDIO_MAX_PACKET];
   while (true) {
      struct nm_hdr hdr;
      if (!t->read(t->user, &hdr, sizeof(hdr)))
         break;
      if (hdr.len > sizeof(buf))
         break;
      if (hdr.len && !t->read(t->user, buf, hdr.len))
         break;
      if (me->superseded)
         break;
      if (hdr.type != NM_AUDIO_PACKET || hdr.len <= sizeof(struct nm_audio_packet))
         continue;
      struct nm_audio_packet pk;
      memcpy(&pk, buf, sizeof(pk));
      /* under the lock, so a replacing stream can't stop the sink while a
       * packet is inside it */
      pthread_mutex_lock(&audio_lock);
      if (!me->superseded)
         sink_ops->packet(me->sink, pk.seq, pk.ts_us, buf + sizeof(pk),
                          hdr.len - sizeof(pk));
      pthread_mutex_unlock(&audio_lock);
   }

   pthread_mutex_lock(&audio_lock);
   if (current == me) {
      sink_ops->stop(me->sink);
      current = NULL;
   }
   pthread_mutex_unlock(&audio_lock);
   free(me);
}
