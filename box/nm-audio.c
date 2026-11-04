/*
 * nm-audio: runs on the box next to the games. Captures what they play and
 * sends it to the iPad as Opus.
 *
 * Games under Proton (and FMOD especially) want a running pulse server with a
 * sink, and the box has no sound card, so this makes sure there is one: a
 * null sink called "streamsink", set as the default. Its monitor is what we
 * capture (libpulse-simple, 48 kHz stereo) and encode with libopus
 * (restricted low delay, 10 ms packets, 128 kbit/s by default).
 *
 * Transport: the daemon connects to nm-relay's unix socket as if it were a
 * guest, so the relay asks the iPad for a data connection and splices it.
 * The first message is NM_AUDIO_HELLO instead of NM_HELLO, which makes the
 * host serve the stream as audio. Nothing is captured until the host has
 * answered, so there is no stale backlog when the iPad connects late. If the
 * relay or the iPad goes away the daemon reconnects every 2 s.
 *
 * Latency control on this side: a short send queue (default 8 packets). If
 * the relay doesn't keep up, the oldest packets are dropped (the host
 * conceals the gap), so audio never lags behind by more than the queue.
 *
 *   nm-audio [-s relay_socket] [-f frame_ms] [-b bitrate] [-d pulse_source]
 *            [-q queue_packets] [-n] [-v]
 *     -n   don't start pulseaudio / create the sink (it's already there)
 *     -v   log stats every 5 s
 *     -t   send a 440 Hz test tone instead of capturing (no pulse needed)
 *
 * SPDX-License-Identifier: MIT
 */
#define _GNU_SOURCE
#include <errno.h>
#include <math.h>
#include <opus/opus.h>
#include <poll.h>
#include <pthread.h>
#include <pulse/error.h>
#include <pulse/simple.h>
#include <signal.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#include "../common/nm_proto.h"

#define RATE 48000
#define CHANNELS 2
#define SINK_NAME "streamsink"

static const char *relay_path = "/tmp/narnia-metal.sock";
static const char *source_name = SINK_NAME ".monitor";
static unsigned frame_ms = 10;
static unsigned bitrate = 128000;
static unsigned queue_max = 8;
static bool setup_pulse = true;
static bool verbose;
static bool tone;

static void
log_(const char *fmt, ...)
{
   char buf[512];
   va_list ap;
   va_start(ap, fmt);
   vsnprintf(buf, sizeof(buf), fmt, ap);
   va_end(ap);
   time_t t = time(NULL);
   char ts[32];
   strftime(ts, sizeof(ts), "%H:%M:%S", localtime(&t));
   fprintf(stderr, "%s nm-audio: %s\n", ts, buf);
}

static uint64_t
now_us(void)
{
   struct timespec ts;
   clock_gettime(CLOCK_MONOTONIC, &ts);
   return (uint64_t)ts.tv_sec * 1000000u + ts.tv_nsec / 1000u;
}

/* ---- pulse server with a null sink ---- */

static void
ensure_pulse(void)
{
   if (!setup_pulse)
      return;
   if (system("pactl info >/dev/null 2>&1")) {
      log_("starting pulseaudio");
      if (system("pulseaudio --start --exit-idle-time=-1 --log-target=syslog"
                 " >/dev/null 2>&1"))
         log_("pulseaudio --start failed");
      for (int i = 0; i < 50 && system("pactl info >/dev/null 2>&1"); i++)
         usleep(100000);
   }
   if (system("pactl list short sinks | cut -f2 | grep -qx " SINK_NAME)) {
      log_("creating null sink " SINK_NAME);
      if (system("pactl load-module module-null-sink sink_name=" SINK_NAME
                 " sink_properties=device.description=NarniaStream"
                 " >/dev/null"))
         log_("couldn't create the null sink");
   }
   /* games should find it without being told */
   if (system("pactl set-default-sink " SINK_NAME " >/dev/null 2>&1"))
      log_("couldn't make " SINK_NAME " the default sink");
}

/* ---- the send queue ---- */

struct packet {
   size_t len;
   uint8_t data[sizeof(struct nm_hdr) + sizeof(struct nm_audio_packet) +
                NM_AUDIO_MAX_PACKET];
};

static pthread_mutex_t qlock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t qcond = PTHREAD_COND_INITIALIZER;
static struct packet *queue[64];
static unsigned q_head, q_count;
static atomic_bool conn_alive;
static atomic_uint_fast64_t sent_packets, sent_bytes, queue_drops;

static void
enqueue(struct packet *p)
{
   pthread_mutex_lock(&qlock);
   if (q_count >= queue_max) {
      free(queue[q_head]);
      q_head = (q_head + 1) % 64;
      q_count--;
      atomic_fetch_add(&queue_drops, 1);
   }
   queue[(q_head + q_count++) % 64] = p;
   pthread_cond_signal(&qcond);
   pthread_mutex_unlock(&qlock);
}

static void
queue_clear(void)
{
   pthread_mutex_lock(&qlock);
   while (q_count) {
      free(queue[q_head]);
      q_head = (q_head + 1) % 64;
      q_count--;
   }
   pthread_mutex_unlock(&qlock);
}

static bool
send_all(int fd, const void *data, size_t size)
{
   const char *p = data;
   while (size) {
      ssize_t n = send(fd, p, size, MSG_NOSIGNAL);
      if (n < 0 && errno == EINTR)
         continue;
      if (n <= 0)
         return false;
      p += n;
      size -= n;
   }
   return true;
}

static bool
recv_all(int fd, void *data, size_t size)
{
   char *p = data;
   while (size) {
      ssize_t n = recv(fd, p, size, 0);
      if (n < 0 && errno == EINTR)
         continue;
      if (n <= 0)
         return false;
      p += n;
      size -= n;
   }
   return true;
}

/* sends queued packets; also notices when the host hangs up */
static void *
sender_thread(void *arg)
{
   const int fd = *(int *)arg;
   while (atomic_load(&conn_alive)) {
      struct packet *p = NULL;
      pthread_mutex_lock(&qlock);
      if (!q_count) {
         struct timespec to;
         clock_gettime(CLOCK_REALTIME, &to);
         to.tv_nsec += 100 * 1000000L;
         if (to.tv_nsec >= 1000000000L) {
            to.tv_nsec -= 1000000000L;
            to.tv_sec++;
         }
         pthread_cond_timedwait(&qcond, &qlock, &to);
      }
      if (q_count) {
         p = queue[q_head];
         q_head = (q_head + 1) % 64;
         q_count--;
      }
      pthread_mutex_unlock(&qlock);

      /* the host never sends anything after its reply; EOF means gone */
      struct pollfd pfd = { .fd = fd, .events = POLLIN };
      if (poll(&pfd, 1, 0) > 0 && (pfd.revents & (POLLIN | POLLHUP | POLLERR))) {
         char c;
         if (recv(fd, &c, 1, MSG_DONTWAIT) <= 0 && errno != EAGAIN) {
            free(p);
            break;
         }
      }
      if (!p)
         continue;
      if (!send_all(fd, p->data, p->len)) {
         free(p);
         break;
      }
      atomic_fetch_add(&sent_packets, 1);
      atomic_fetch_add(&sent_bytes, p->len);
      free(p);
   }
   atomic_store(&conn_alive, false);
   return NULL;
}

/* ---- connection to the relay ---- */

/* connect and complete the hello; returns the fd or -1 */
static int
connect_relay(void)
{
   int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
   struct sockaddr_un un = { .sun_family = AF_UNIX };
   snprintf(un.sun_path, sizeof(un.sun_path), "%s", relay_path);
   if (connect(fd, (struct sockaddr *)&un, sizeof(un))) {
      close(fd);
      return -1;
   }
   /* keep the local backlog short: it is stale audio waiting to be played */
   const int sndbuf = 32 * 1024;
   setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf));

   struct {
      struct nm_hdr hdr;
      struct nm_audio_hello h;
   } hello = {
      .hdr = { NM_AUDIO_HELLO, sizeof(struct nm_audio_hello) },
      .h = {
         .magic = NM_MAGIC,
         .version = NM_VERSION,
         .codec = NM_AUDIO_CODEC_OPUS,
         .sample_rate = RATE,
         .channels = CHANNELS,
         .frame_samples = RATE * frame_ms / 1000,
         .bitrate = bitrate,
      },
   };
   if (!send_all(fd, &hello, sizeof(hello))) {
      close(fd);
      return -1;
   }
   log_("connected to the relay, waiting for the iPad");

   /* the relay holds us until an iPad is there (up to its -w seconds) */
   struct pollfd pfd = { .fd = fd, .events = POLLIN };
   if (poll(&pfd, 1, 90000) <= 0) {
      close(fd);
      return -1;
   }
   struct {
      struct nm_hdr hdr;
      struct nm_audio_hello_reply r;
   } reply;
   if (!recv_all(fd, &reply, sizeof(reply)) ||
       reply.hdr.type != NM_AUDIO_HELLO_REPLY || reply.r.magic != NM_MAGIC ||
       reply.r.version != NM_VERSION) {
      log_("bad reply from the host (version mismatch?)");
      close(fd);
      return -1;
   }
   if (!reply.r.accepted) {
      log_("the host has no audio sink");
      close(fd);
      return -1;
   }
   return fd;
}

/* ---- capture + encode ---- */

static const char *
opus_name(int err)
{
   return opus_strerror(err);
}

/* returns when the connection or the capture fails */
static void
stream(int fd)
{
   const unsigned frame_samples = RATE * frame_ms / 1000;
   const size_t frame_bytes = frame_samples * CHANNELS * sizeof(int16_t);
   int err;

   OpusEncoder *enc = opus_encoder_create(RATE, CHANNELS,
                                          OPUS_APPLICATION_RESTRICTED_LOWDELAY, &err);
   if (!enc) {
      log_("opus_encoder_create: %s", opus_name(err));
      return;
   }
   opus_encoder_ctl(enc, OPUS_SET_BITRATE(bitrate));
   opus_encoder_ctl(enc, OPUS_SET_VBR(1));
   opus_encoder_ctl(enc, OPUS_SET_VBR_CONSTRAINT(1));
   opus_encoder_ctl(enc, OPUS_SET_COMPLEXITY(8));
   opus_encoder_ctl(enc, OPUS_SET_SIGNAL(OPUS_SIGNAL_MUSIC));
   opus_encoder_ctl(enc, OPUS_SET_PACKET_LOSS_PERC(1));

   pa_simple *pa = NULL;
   if (!tone) {
      const pa_sample_spec ss = {
         .format = PA_SAMPLE_S16LE, .rate = RATE, .channels = CHANNELS,
      };
      /* ask for one packet per fragment: that sets the capture latency */
      const pa_buffer_attr attr = {
         .maxlength = (uint32_t)-1,
         .tlength = (uint32_t)-1,
         .prebuf = (uint32_t)-1,
         .minreq = (uint32_t)-1,
         .fragsize = frame_bytes,
      };
      pa = pa_simple_new(NULL, "nm-audio", PA_STREAM_RECORD, source_name,
                         "game audio", &ss, NULL, &attr, &err);
      if (!pa) {
         log_("pulse capture from %s failed: %s", source_name, pa_strerror(err));
         opus_encoder_destroy(enc);
         sleep(1);
         return;
      }
   }
   log_("streaming: %u ms Opus packets at %u kbit/s from %s", frame_ms,
        bitrate / 1000, tone ? "a test tone" : source_name);

   pthread_t sender;
   atomic_store(&conn_alive, true);
   queue_clear();
   pthread_create(&sender, NULL, sender_thread, &fd);

   int16_t *pcm = malloc(frame_bytes);
   uint32_t seq = 0;
   uint64_t tone_n = 0;
   uint64_t next_tone = now_us();
   uint64_t last_stat = now_us(), enc_max_us = 0;
   uint64_t lat_sum = 0;

   while (atomic_load(&conn_alive)) {
      uint64_t lat = 0;
      if (tone) {
         for (unsigned i = 0; i < frame_samples; i++, tone_n++) {
            const int16_t v = (int16_t)(8000 * sin(2 * M_PI * 440.0 * tone_n / RATE));
            pcm[i * 2] = v;
            pcm[i * 2 + 1] = v;
         }
         next_tone += frame_ms * 1000;
         const uint64_t t = now_us();
         if (next_tone > t)
            usleep(next_tone - t);
      } else {
         if (pa_simple_read(pa, pcm, frame_bytes, &err) < 0) {
            log_("pulse read failed: %s", pa_strerror(err));
            break;
         }
         lat = pa_simple_get_latency(pa, &err);
         /* pa gives the latency of the data just read, as a buffer's worth */
      }
      /* capture time of the first sample in this packet */
      const uint64_t ts = now_us() - lat - frame_ms * 1000;
      lat_sum += lat;

      struct packet *p = malloc(sizeof(*p));
      uint8_t *payload = p->data + sizeof(struct nm_hdr) + sizeof(struct nm_audio_packet);
      const uint64_t e0 = now_us();
      const int n = opus_encode(enc, pcm, frame_samples, payload, NM_AUDIO_MAX_PACKET);
      const uint64_t e1 = now_us();
      if (e1 - e0 > enc_max_us)
         enc_max_us = e1 - e0;
      if (n < 0) {
         log_("opus_encode: %s", opus_name(n));
         free(p);
         break;
      }
      const struct nm_hdr hdr = { NM_AUDIO_PACKET, sizeof(struct nm_audio_packet) + n };
      const struct nm_audio_packet ap = { .seq = seq++, .ts_us = ts };
      memcpy(p->data, &hdr, sizeof(hdr));
      memcpy(p->data + sizeof(hdr), &ap, sizeof(ap));
      p->len = sizeof(hdr) + hdr.len;
      enqueue(p);

      if (verbose && e1 - last_stat > 5000000) {
         log_("sent %llu pkts, %llu kB, queue drops %llu, encode max %llu us, "
              "pulse latency %llu us",
              (unsigned long long)atomic_load(&sent_packets),
              (unsigned long long)atomic_load(&sent_bytes) / 1024,
              (unsigned long long)atomic_load(&queue_drops),
              (unsigned long long)enc_max_us,
              (unsigned long long)(lat_sum / (seq ? seq : 1)));
         last_stat = e1;
         enc_max_us = 0;
      }
   }

   atomic_store(&conn_alive, false);
   pthread_cond_signal(&qcond);
   pthread_join(sender, NULL);
   free(pcm);
   if (pa)
      pa_simple_free(pa);
   opus_encoder_destroy(enc);
}

int
main(int argc, char **argv)
{
   int opt;
   while ((opt = getopt(argc, argv, "s:f:b:d:q:nvt")) != -1) {
      switch (opt) {
      case 's': relay_path = optarg; break;
      case 'f': frame_ms = atoi(optarg); break;
      case 'b': bitrate = atoi(optarg); break;
      case 'd': source_name = optarg; break;
      case 'q': queue_max = atoi(optarg); break;
      case 'n': setup_pulse = false; break;
      case 'v': verbose = true; break;
      case 't': tone = true; setup_pulse = false; break;
      default:
         fprintf(stderr,
                 "usage: %s [-s relay_socket] [-f 5|10|20] [-b bitrate] "
                 "[-d pulse_source] [-q queue_packets] [-n] [-v] [-t]\n",
                 argv[0]);
         return 1;
      }
   }
   if (frame_ms != 5 && frame_ms != 10 && frame_ms != 20 && frame_ms != 40) {
      fprintf(stderr, "nm-audio: -f must be 5, 10, 20 or 40\n");
      return 1;
   }
   if (queue_max < 1 || queue_max > 60)
      queue_max = 8;
   signal(SIGPIPE, SIG_IGN);
   setvbuf(stderr, NULL, _IOLBF, 0);

   ensure_pulse();

   bool warned = false;
   while (true) {
      const int fd = connect_relay();
      if (fd < 0) {
         if (!warned)
            log_("no relay at %s yet, retrying", relay_path);
         warned = true;
         sleep(2);
         continue;
      }
      warned = false;
      stream(fd);
      close(fd);
      log_("stream ended, reconnecting");
      sleep(1);
   }
}
