/*
 * LHSR Daemon - Prometheus metrics HTTP endpoint
 *
 * Serves /var/lib/lhsrd/metrics.prom on TCP port 9101 for
 * Prometheus scraping.
 *
 * Copyright (C) 2026 LHSR Team
 * License: GPLv3
 */
#ifndef LHSR_HTTP_H
#define LHSR_HTTP_H

/*
 * Start the HTTP metrics server thread.
 * Returns 0 on success, -1 on error (not fatal).
 */
int lhsr_http_start(struct daemon_state *st);

/*
 * Stop the HTTP server thread.
 */
void lhsr_http_stop(void);

#endif /* LHSR_HTTP_H */
