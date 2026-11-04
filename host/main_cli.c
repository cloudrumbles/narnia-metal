/*
 * nm-host: command-line host for Linux (system Vulkan loader, e.g. lavapipe)
 * and macOS (MoltenVK, used by CI). Listens for guests over TCP or a unix
 * socket, or connects out to a box relay, and runs them on vkr. Presented
 * frames go to the PNG presenter.
 *
 *   VK_DRIVER_FILES=/opt/mesa/share/vulkan/icd.d/lvp_icd.x86_64.json \
 *     nm-host [tcp:[addr:]port | unix:/path]     (default tcp:7397)
 *
 * or connect out to a box relay, like the iPad does:
 *
 *     nm-host relay:<host>:<port>:<token>
 *
 * SPDX-License-Identifier: MIT
 */
#ifdef __linux__
#define _GNU_SOURCE
#endif
#include <errno.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include "nm_driver.h"
#include "nm_host.h"
#include "nm_presenter_png.h"
#include "nm_relay_client.h"

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0 /* SIGPIPE is ignored in main() */
#endif
#ifndef SOCK_CLOEXEC
#define SOCK_CLOEXEC 0
#endif
#include "nm_proto.h"

struct fd_conn {
   int fd;
   struct nm_transport transport;
};

static bool
fd_read(void *user, void *data, size_t size)
{
   struct fd_conn *c = user;
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
fd_write(void *user, const void *data, size_t size)
{
   struct fd_conn *c = user;
   const uint8_t *p = data;
   while (size) {
      const ssize_t n = send(c->fd, p, size, MSG_NOSIGNAL);
      if (n < 0 && errno == EINTR)
         continue;
      if (n <= 0)
         return false;
      p += n;
      size -= n;
   }
   return true;
}

static void *
conn_thread(void *arg)
{
   struct fd_conn *c = arg;
   nm_host_serve(&c->transport);
   close(c->fd);
   free(c);
   return NULL;
}

static int
listen_on(const char *spec)
{
   if (!strncmp(spec, "unix:", 5)) {
      struct sockaddr_un un = { .sun_family = AF_UNIX };
      snprintf(un.sun_path, sizeof(un.sun_path), "%s", spec + 5);
      unlink(un.sun_path);
      int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
      if (fd < 0 || bind(fd, (struct sockaddr *)&un, sizeof(un)) ||
          listen(fd, 8)) {
         perror(spec);
         return -1;
      }
      return fd;
   }

   if (!strncmp(spec, "tcp:", 4))
      spec += 4;
   const char *colon = strrchr(spec, ':');
   const int port = atoi(colon ? colon + 1 : spec);

   int fd = socket(AF_INET6, SOCK_STREAM | SOCK_CLOEXEC, 0);
   const int one = 1, zero = 0;
   setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
   setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &zero, sizeof(zero));
   struct sockaddr_in6 sa = {
      .sin6_family = AF_INET6,
      .sin6_port = htons(port),
      .sin6_addr = in6addr_any,
   };
   if (fd < 0 || bind(fd, (struct sockaddr *)&sa, sizeof(sa)) ||
       listen(fd, 8)) {
      perror("listen");
      return -1;
   }
   return fd;
}

int
main(int argc, char **argv)
{
   char def[32];
   snprintf(def, sizeof(def), "tcp:%d", NM_DEFAULT_PORT);
   const char *spec = argc > 1 ? argv[1] : def;

   signal(SIGPIPE, SIG_IGN);
   setvbuf(stderr, NULL, _IOLBF, 0);

   nm_host_set_presenter(&nm_presenter_png, NULL);
   if (!nm_host_init(nm_driver_get_instance_proc_addr(), NULL))
      return 1;

   if (!strncmp(spec, "relay:", 6)) {
      /* relay:<host>:<port>:<token>, reconnecting forever */
      static char host[256], tok[128];
      static struct nm_relay_client_config cfg;
      int port = NM_RELAY_DEFAULT_PORT;
      if (sscanf(spec + 6, "%255[^:]:%d:%127s", host, &port, tok) != 3) {
         fprintf(stderr, "nm-host: expected relay:<host>:<port>:<token>\n");
         return 1;
      }
      cfg = (struct nm_relay_client_config){
         .host = host,
         .port = port,
         .token = tok,
      };
      static atomic_bool stop;
      while (nm_relay_client_run(&cfg, &stop) != NM_RELAY_KICKED)
         sleep(2);
      return 0;
   }

   int lfd = listen_on(spec);
   if (lfd < 0)
      return 1;
   fprintf(stderr, "nm-host: driver %s, listening on %s\n", nm_driver_name(),
           spec);

   while (true) {
#ifdef __linux__
      int fd = accept4(lfd, NULL, NULL, SOCK_CLOEXEC);
#else
      int fd = accept(lfd, NULL, NULL);
#endif
      if (fd < 0) {
         if (errno == EINTR)
            continue;
         perror("accept");
         return 1;
      }
      const int one = 1;
      setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

      struct fd_conn *c = calloc(1, sizeof(*c));
      c->fd = fd;
      c->transport = (struct nm_transport){
         .user = c,
         .read = fd_read,
         .write = fd_write,
      };
      pthread_t t;
      pthread_create(&t, NULL, conn_thread, c);
      pthread_detach(t);
   }
}
