/*
 * Client side of the box relay (box/nm-relay.c), for hosts that sit behind
 * NAT (the iPad). The host connects out to the relay:
 *
 *   control connection:  "NMCTL1 <token>\n"  <- "OK\n", then "OPEN <id>\n" /
 *                        "PING\n" lines from the relay
 *   per guest:           "NMDATA1 <token> <id>\n", then the nm byte stream,
 *                        served by nm_host_serve()
 *
 * Plain POSIX sockets, so the same code runs on Linux and iOS.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef NM_RELAY_CLIENT_H
#define NM_RELAY_CLIENT_H

#include <stdatomic.h>
#include <stdbool.h>

#define NM_RELAY_DEFAULT_PORT 7398

struct nm_relay_client_config {
   const char *host;
   int port;
   const char *token;
   /* optional status updates ("connected", "relay closed", ...) */
   void (*status)(void *user, const char *msg);
   void *user;
};

/* Connects and serves guests until *stop becomes true or the control
 * connection drops. Returns true if it got as far as being connected.
 */
bool
nm_relay_client_run(const struct nm_relay_client_config *cfg,
                    atomic_bool *stop);

#endif /* NM_RELAY_CLIENT_H */
