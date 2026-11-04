/*
 * Client side of the box relay (box/nm-relay.c), for hosts that sit behind
 * NAT (the iPad). The host connects out to the relay:
 *
 *   control connection:  "NMCTL1 <token>\n"  <- "OK\n", then "OPEN <id>\n" /
 *                        "PING\n" lines from the relay
 *   per guest:           "NMDATA1 <token> <id>\n", then the nm byte stream,
 *                        served by nm_host_serve()
 *
 * Telemetry goes back up the control connection as "LOG <text>" and
 * "STAT <key=value ...>" lines; the relay writes them to a file on the box.
 *
 * Plain POSIX sockets, so the same code runs on Linux and iOS.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef NM_RELAY_CLIENT_H
#define NM_RELAY_CLIENT_H

#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>

#define NM_RELAY_DEFAULT_PORT 7398

struct nm_relay_client_config {
   const char *host;
   int port;
   const char *token;
   /* optional status updates ("connected", "relay closed", ...) */
   void (*status)(void *user, const char *msg);
   void *user;
};

enum nm_relay_result {
   NM_RELAY_FAILED,  /* couldn't connect or the token was refused */
   NM_RELAY_DROPPED, /* was connected, then lost the relay (or *stop) */
   NM_RELAY_KICKED,  /* another host took over; don't reconnect */
};

/* Connects and serves guests until *stop becomes true, the control
 * connection drops, or another host takes over.
 */
enum nm_relay_result
nm_relay_client_run(const struct nm_relay_client_config *cfg,
                    atomic_bool *stop);

/* Telemetry, callable from any thread. Logs from before the control
 * connection is up are kept (last 200 lines) and sent once it is.
 */
void
nm_relay_client_log(const char *line);

void
nm_relay_client_stat(const char *key_values);

/* Input for the box (see box/nm-input.c for the message formats), sent as
 * "IN <msg>". Dropped while not connected.
 */
void
nm_relay_client_input(const char *msg);

/* bytes received on all guest streams so far */
uint64_t
nm_relay_client_rx_bytes(void);

/* guest streams being served right now */
uint32_t
nm_relay_client_active_streams(void);

#endif /* NM_RELAY_CLIENT_H */
