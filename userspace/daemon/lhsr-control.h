/*
 * LHSR Daemon - Control socket (Unix domain socket)
 *
 * Listens on /run/lhsrd.sock for JSON commands from lhsrctl.
 *
 * Copyright (C) 2026 LHSR Team
 * License: GPLv3
 */
#ifndef LHSR_CONTROL_H
#define LHSR_CONTROL_H

#include "lhsrd.h"

#define LHSRD_SOCKET_PATH "/run/lhsrd.sock"

/*
 * Start the control socket listener thread.
 * The thread runs until st->running is 0.
 * Returns 0 on success, -1 on error.
 */
int lhsr_control_start(struct daemon_state *st);

/*
 * Signal the control socket thread to stop.
 * Closes the listener socket, causing accept() to return.
 */
void lhsr_control_stop(struct daemon_state *st);

/*
 * Build a JSON status string from the current daemon state.
 * Returns malloc'd string (caller must free), or NULL on error.
 */
char *lhsr_control_build_status(struct daemon_state *st);

/*
 * Build a JSON trends string from the current daemon state.
 * Returns malloc'd string (caller must free), or NULL on error.
 */
char *lhsr_control_build_trends(struct daemon_state *st);

#endif /* LHSR_CONTROL_H */
