/*
 * nm-relay: runs on the box. Guests (Venus with VN_NET=unix:<socket>)
 * connect to a unix socket; the host (iPad) connects in over TCP, since it
 * sits behind NAT. For each guest the relay asks the host, over its control
 * connection, to open a data connection, then splices the two.
 *
 *   NM_TOKEN=secret nm-relay [-p tcp_port] [-s socket_path] [-w wait_secs]
 *
 * Without NM_TOKEN a random token is generated and printed. Protocol: see
 * host/nm_relay_client.h. The host's telemetry (LOG/STAT lines, plus the
 * ping round trip) goes to -t <file> (default /tmp/nm-ipad.log). Its input
 * ("IN ..." lines) is forwarded as datagrams to nm-input's socket
 * (-i <path>, default /tmp/narnia-input.sock).
 *
 * SPDX-License-Identifier: MIT
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#define MAX_PENDING 64

static char token[65];
static int wait_secs = 60;
static const char *telemetry_path = "/tmp/nm-ipad.log";
static FILE *telemetry;
static struct timespec ping_sent;
static const char *input_path = "/tmp/narnia-input.sock";
static int input_fd = -1;

static void
forward_input(const char *msg)
{
   if (input_fd < 0)
      input_fd = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0);
   struct sockaddr_un un = { .sun_family = AF_UNIX };
   snprintf(un.sun_path, sizeof(un.sun_path), "%s", input_path);
   /* nm-input not running: input is dropped */
   sendto(input_fd, msg, strlen(msg), MSG_DONTWAIT, (struct sockaddr *)&un,
          sizeof(un));
}

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cond = PTHREAD_COND_INITIALIZER;
static int control_fd = -1;
static unsigned next_id = 1;

/* guests waiting for their data connection */
static struct {
   unsigned id;
   int data_fd;
} pending[MAX_PENDING];

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
   fprintf(stderr, "%s nm-relay: %s\n", ts, buf);
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
read_line(int fd, char *buf, size_t size)
{
   size_t len = 0;
   while (len + 1 < size) {
      char ch;
      ssize_t n = recv(fd, &ch, 1, 0);
      if (n < 0 && errno == EINTR)
         continue;
      if (n <= 0)
         return false;
      if (ch == '\n')
         break;
      buf[len++] = ch;
   }
   buf[len] = '\0';
   return true;
}

static bool
token_ok(const char *t)
{
   const size_t n = strlen(token);
   if (strlen(t) != n)
      return false;
   unsigned diff = 0;
   for (size_t i = 0; i < n; i++)
      diff |= (unsigned char)t[i] ^ (unsigned char)token[i];
   return !diff;
}

/* copy both ways until either side closes */
static void
splice_fds(int a, int b)
{
   char buf[256 * 1024];
   struct pollfd pfd[2] = { { .fd = a, .events = POLLIN },
                            { .fd = b, .events = POLLIN } };
   while (true) {
      if (poll(pfd, 2, -1) < 0) {
         if (errno == EINTR)
            continue;
         return;
      }
      for (int i = 0; i < 2; i++) {
         if (!(pfd[i].revents & (POLLIN | POLLHUP | POLLERR)))
            continue;
         ssize_t n = recv(pfd[i].fd, buf, sizeof(buf), 0);
         if (n <= 0)
            return;
         if (!send_all(pfd[1 - i].fd, buf, n))
            return;
      }
   }
}

static void
set_nodelay(int fd)
{
   const int one = 1;
   setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
   setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &one, sizeof(one));
}

/* ---- a guest connected on the unix socket ---- */

static void *
guest_thread(void *arg)
{
   const int guest_fd = (int)(intptr_t)arg;
   struct timespec deadline;

   pthread_mutex_lock(&lock);
   const unsigned id = next_id++;
   int slot = -1;
   for (int i = 0; i < MAX_PENDING; i++) {
      if (!pending[i].id) {
         slot = i;
         pending[i].id = id;
         pending[i].data_fd = -1;
         break;
      }
   }
   if (slot < 0) {
      pthread_mutex_unlock(&lock);
      log_("guest %u: too many pending guests", id);
      close(guest_fd);
      return NULL;
   }

   /* wait for a host to be connected */
   clock_gettime(CLOCK_REALTIME, &deadline);
   deadline.tv_sec += wait_secs;
   if (control_fd < 0)
      log_("guest %u: waiting up to %d s for the iPad", id, wait_secs);
   while (control_fd < 0) {
      if (pthread_cond_timedwait(&cond, &lock, &deadline) == ETIMEDOUT)
         break;
   }
   bool asked = false;
   if (control_fd >= 0) {
      char line[64];
      snprintf(line, sizeof(line), "OPEN %u\n", id);
      asked = send_all(control_fd, line, strlen(line));
   }

   /* wait for its data connection */
   clock_gettime(CLOCK_REALTIME, &deadline);
   deadline.tv_sec += 15;
   while (asked && pending[slot].data_fd < 0) {
      if (pthread_cond_timedwait(&cond, &lock, &deadline) == ETIMEDOUT)
         break;
   }
   const int data_fd = pending[slot].data_fd;
   pending[slot].id = 0;
   pending[slot].data_fd = -1;
   pthread_mutex_unlock(&lock);

   if (data_fd < 0) {
      log_("guest %u: no iPad, closing", id);
      close(guest_fd);
      return NULL;
   }

   log_("guest %u: streaming", id);
   splice_fds(guest_fd, data_fd);
   log_("guest %u: done", id);
   close(guest_fd);
   close(data_fd);
   return NULL;
}

/* ---- the host connected over TCP ---- */

static void *
host_thread(void *arg)
{
   const int fd = (int)(intptr_t)arg;
   set_nodelay(fd);

   /* the first line says what this connection is */
   const struct timeval tv = { .tv_sec = 10 };
   setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
   char line[256], tok[128];
   unsigned id;
   if (!read_line(fd, line, sizeof(line))) {
      close(fd);
      return NULL;
   }
   const struct timeval none = { 0 };
   setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &none, sizeof(none));

   if (sscanf(line, "NMCTL1 %127s", tok) == 1) {
      if (!token_ok(tok)) {
         log_("control connection with a wrong token");
         send_all(fd, "NO\n", 3);
         close(fd);
         return NULL;
      }
      pthread_mutex_lock(&lock);
      const int old = control_fd;
      control_fd = fd;
      send_all(fd, "OK\n", 3);
      pthread_cond_broadcast(&cond);
      /* the newest host wins; tell the old one not to reconnect */
      if (old >= 0)
         send_all(old, "KICK\n", 5);
      pthread_mutex_unlock(&lock);
      if (old >= 0)
         shutdown(old, SHUT_RDWR);
      log_("iPad connected");

      /* telemetry and PONGs until it goes away */
      while (read_line(fd, line, sizeof(line))) {
         struct timespec now;
         clock_gettime(CLOCK_MONOTONIC, &now);
         char ts[32];
         time_t t = time(NULL);
         strftime(ts, sizeof(ts), "%H:%M:%S", localtime(&t));
         if (!strncmp(line, "IN ", 3)) {
            forward_input(line + 3);
            continue;
         }
         if (!strcmp(line, "PONG")) {
            pthread_mutex_lock(&lock);
            const double rtt = (now.tv_sec - ping_sent.tv_sec) * 1e3 +
                               (now.tv_nsec - ping_sent.tv_nsec) / 1e6;
            pthread_mutex_unlock(&lock);
            if (telemetry)
               fprintf(telemetry, "%s RTT %.1f ms\n", ts, rtt);
         } else if (telemetry) {
            fprintf(telemetry, "%s %s\n", ts, line);
         }
         if (telemetry)
            fflush(telemetry);
      }
      pthread_mutex_lock(&lock);
      if (control_fd == fd)
         control_fd = -1;
      pthread_mutex_unlock(&lock);
      close(fd);
      log_("iPad disconnected");
      return NULL;
   }

   if (sscanf(line, "NMDATA1 %127s %u", tok, &id) == 2 && token_ok(tok)) {
      pthread_mutex_lock(&lock);
      bool taken = false;
      for (int i = 0; i < MAX_PENDING; i++) {
         if (pending[i].id == id && pending[i].data_fd < 0) {
            pending[i].data_fd = fd;
            taken = true;
            break;
         }
      }
      pthread_cond_broadcast(&cond);
      pthread_mutex_unlock(&lock);
      if (!taken)
         close(fd);
      return NULL;
   }

   log_("unknown connection: %.40s", line);
   close(fd);
   return NULL;
}

static void *
ping_thread(void *arg)
{
   (void)arg;
   while (true) {
      sleep(5);
      pthread_mutex_lock(&lock);
      clock_gettime(CLOCK_MONOTONIC, &ping_sent);
      if (control_fd >= 0 && !send_all(control_fd, "PING\n", 5)) {
         shutdown(control_fd, SHUT_RDWR);
         control_fd = -1;
      }
      pthread_mutex_unlock(&lock);
   }
   return NULL;
}

static void
spawn(void *(*fn)(void *), int fd)
{
   pthread_t t;
   if (pthread_create(&t, NULL, fn, (void *)(intptr_t)fd)) {
      close(fd);
      return;
   }
   pthread_detach(t);
}

int
main(int argc, char **argv)
{
   int port = 7398;
   const char *sock_path = "/tmp/narnia-metal.sock";
   int opt;
   while ((opt = getopt(argc, argv, "p:s:w:t:i:")) != -1) {
      switch (opt) {
      case 'p': port = atoi(optarg); break;
      case 's': sock_path = optarg; break;
      case 'w': wait_secs = atoi(optarg); break;
      case 't': telemetry_path = optarg; break;
      case 'i': input_path = optarg; break;
      default:
         fprintf(stderr, "usage: NM_TOKEN=... %s [-p port] [-s socket] [-w wait_secs] [-t telemetry_file] [-i input_socket]\n",
                 argv[0]);
         return 1;
      }
   }
   signal(SIGPIPE, SIG_IGN);
   setvbuf(stderr, NULL, _IOLBF, 0);

   const char *env = getenv("NM_TOKEN");
   if (env && *env) {
      snprintf(token, sizeof(token), "%s", env);
   } else {
      static const char alphabet[] = "abcdefghjkmnpqrstuvwxyz23456789";
      unsigned char rnd[12];
      FILE *f = fopen("/dev/urandom", "rb");
      if (!f || fread(rnd, 1, sizeof(rnd), f) != sizeof(rnd))
         return 1;
      fclose(f);
      for (size_t i = 0; i < sizeof(rnd); i++)
         token[i] = alphabet[rnd[i] % (sizeof(alphabet) - 1)];
      token[sizeof(rnd)] = '\0';
   }

   int tcp = socket(AF_INET6, SOCK_STREAM | SOCK_CLOEXEC, 0);
   const int one = 1, zero = 0;
   setsockopt(tcp, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
   setsockopt(tcp, IPPROTO_IPV6, IPV6_V6ONLY, &zero, sizeof(zero));
   struct sockaddr_in6 sa = {
      .sin6_family = AF_INET6,
      .sin6_port = htons(port),
      .sin6_addr = in6addr_any,
   };
   if (bind(tcp, (struct sockaddr *)&sa, sizeof(sa)) || listen(tcp, 16)) {
      perror("tcp listen");
      return 1;
   }

   unlink(sock_path);
   int unx = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
   struct sockaddr_un un = { .sun_family = AF_UNIX };
   snprintf(un.sun_path, sizeof(un.sun_path), "%s", sock_path);
   if (bind(unx, (struct sockaddr *)&un, sizeof(un)) || listen(unx, 16)) {
      perror("unix listen");
      return 1;
   }

   telemetry = fopen(telemetry_path, "a");
   log_("listening: iPad on tcp %d, apps on %s", port, sock_path);
   log_("iPad telemetry: %s", telemetry_path);
   log_("token: %s", token);
   log_("apps: VN_NET=unix:%s", sock_path);

   spawn(ping_thread, -1);

   struct pollfd pfd[2] = { { .fd = tcp, .events = POLLIN },
                            { .fd = unx, .events = POLLIN } };
   while (true) {
      if (poll(pfd, 2, -1) < 0) {
         if (errno == EINTR)
            continue;
         perror("poll");
         return 1;
      }
      if (pfd[0].revents & POLLIN) {
         int fd = accept4(tcp, NULL, NULL, SOCK_CLOEXEC);
         if (fd >= 0)
            spawn(host_thread, fd);
      }
      if (pfd[1].revents & POLLIN) {
         int fd = accept4(unx, NULL, NULL, SOCK_CLOEXEC);
         if (fd >= 0)
            spawn(guest_thread, fd);
      }
   }
}
