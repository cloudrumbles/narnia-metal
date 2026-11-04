/*
 * Client side of the box relay. See nm_relay_client.h.
 *
 * SPDX-License-Identifier: MIT
 */
#include "nm_relay_client.h"

#include <errno.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include "nm_host.h"

/* the relay pings every 5 s */
#define NM_RELAY_DEAD_SECS 20

static void
status(const struct nm_relay_client_config *cfg, const char *fmt, ...)
{
   char buf[256];
   va_list ap;
   va_start(ap, fmt);
   vsnprintf(buf, sizeof(buf), fmt, ap);
   va_end(ap);
   if (cfg->status)
      cfg->status(cfg->user, buf);
   else
      fprintf(stderr, "relay: %s\n", buf);
}

static int
connect_to(const char *host, int port)
{
   char port_str[16];
   snprintf(port_str, sizeof(port_str), "%d", port);
   const struct addrinfo hints = {
      .ai_family = AF_UNSPEC,
      .ai_socktype = SOCK_STREAM,
   };
   struct addrinfo *res;
   if (getaddrinfo(host, port_str, &hints, &res))
      return -1;

   int fd = -1;
   for (struct addrinfo *ai = res; ai; ai = ai->ai_next) {
      fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
      if (fd < 0)
         continue;
      if (!connect(fd, ai->ai_addr, ai->ai_addrlen))
         break;
      close(fd);
      fd = -1;
   }
   freeaddrinfo(res);
   if (fd < 0)
      return -1;

   const int one = 1;
   setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
   setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &one, sizeof(one));
#ifdef SO_NOSIGPIPE
   setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#endif
   return fd;
}

static bool
send_all(int fd, const void *data, size_t size)
{
   const uint8_t *p = data;
   while (size) {
#ifdef MSG_NOSIGNAL
      const ssize_t n = send(fd, p, size, MSG_NOSIGNAL);
#else
      const ssize_t n = send(fd, p, size, 0);
#endif
      if (n < 0 && errno == EINTR)
         continue;
      if (n <= 0)
         return false;
      p += n;
      size -= n;
   }
   return true;
}

/* ---- one guest stream ---- */

struct data_conn {
   int fd;
   struct nm_transport transport;
};

static bool
data_read(void *user, void *data, size_t size)
{
   struct data_conn *c = user;
   uint8_t *p = data;
   while (size) {
      const ssize_t n = recv(c->fd, p, size, 0);
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
data_write(void *user, const void *data, size_t size)
{
   return send_all(((struct data_conn *)user)->fd, data, size);
}

struct data_job {
   const struct nm_relay_client_config *cfg;
   unsigned id;
};

static void *
data_thread(void *arg)
{
   struct data_job *job = arg;
   const struct nm_relay_client_config *cfg = job->cfg;

   struct data_conn *c = calloc(1, sizeof(*c));
   c->fd = connect_to(cfg->host, cfg->port);
   if (c->fd < 0) {
      status(cfg, "stream %u: connect failed", job->id);
      free(c);
      free(job);
      return NULL;
   }
   char line[256];
   snprintf(line, sizeof(line), "NMDATA1 %s %u\n", cfg->token, job->id);
   if (send_all(c->fd, line, strlen(line))) {
      c->transport = (struct nm_transport){
         .user = c,
         .read = data_read,
         .write = data_write,
      };
      status(cfg, "stream %u: app connected", job->id);
      nm_host_serve(&c->transport);
      status(cfg, "stream %u: app closed", job->id);
   }
   close(c->fd);
   free(c);
   free(job);
   return NULL;
}

/* ---- control connection ---- */

/* read one line; false on error/EOF, *timed_out on a receive timeout */
static bool
read_line(int fd, char *buf, size_t size, bool *timed_out)
{
   size_t len = 0;
   *timed_out = false;
   while (len + 1 < size) {
      char ch;
      const ssize_t n = recv(fd, &ch, 1, 0);
      if (n < 0 && errno == EINTR)
         continue;
      if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
         if (!len) {
            *timed_out = true;
            return false;
         }
         continue;
      }
      if (n <= 0)
         return false;
      if (ch == '\n')
         break;
      buf[len++] = ch;
   }
   buf[len] = '\0';
   return true;
}

enum nm_relay_result
nm_relay_client_run(const struct nm_relay_client_config *cfg,
                    atomic_bool *stop)
{
   status(cfg, "connecting to %s:%d", cfg->host, cfg->port);
   int fd = connect_to(cfg->host, cfg->port);
   if (fd < 0) {
      status(cfg, "can't reach %s:%d", cfg->host, cfg->port);
      return NM_RELAY_FAILED;
   }

   char line[256];
   snprintf(line, sizeof(line), "NMCTL1 %s\n", cfg->token);
   bool timed_out;
   const struct timeval tv = { .tv_sec = 1 };
   setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
   if (!send_all(fd, line, strlen(line))) {
      close(fd);
      return NM_RELAY_FAILED;
   }

   /* wait up to 10 s for the answer */
   bool ok = false;
   for (int i = 0; i < 10 && !*stop; i++) {
      if (read_line(fd, line, sizeof(line), &timed_out)) {
         ok = !strcmp(line, "OK");
         break;
      }
      if (!timed_out)
         break;
   }
   if (!ok) {
      status(cfg, "relay refused the token");
      close(fd);
      return NM_RELAY_FAILED;
   }
   status(cfg, "connected, waiting for apps");

   enum nm_relay_result result = NM_RELAY_DROPPED;
   int idle = 0;
   while (!*stop) {
      if (!read_line(fd, line, sizeof(line), &timed_out)) {
         if (timed_out && ++idle < NM_RELAY_DEAD_SECS)
            continue;
         status(cfg, timed_out ? "relay stopped answering" : "relay closed");
         break;
      }
      idle = 0;

      unsigned id;
      if (sscanf(line, "OPEN %u", &id) == 1) {
         struct data_job *job = calloc(1, sizeof(*job));
         job->cfg = cfg;
         job->id = id;
         pthread_t t;
         if (pthread_create(&t, NULL, data_thread, job))
            free(job);
         else
            pthread_detach(t);
      } else if (!strcmp(line, "PING")) {
         send_all(fd, "PONG\n", 5);
      } else if (!strcmp(line, "KICK")) {
         status(cfg, "another device took over the relay");
         result = NM_RELAY_KICKED;
         break;
      }
   }

   close(fd);
   return result;
}
