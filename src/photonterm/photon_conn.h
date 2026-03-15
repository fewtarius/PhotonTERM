/* photon_conn.h - PhotonTERM connection API
 *
 * Copyright (C) 2026 fewtarius and PhotonTERM contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Network connection layer supporting:
 *   PHOTON_CONN_TELNET  - standard telnet (RFC 854)
 *   PHOTON_CONN_SSH     - SSH via libssh2
 *   PHOTON_CONN_SHELL   - local PTY / shell (POSIX only)
 *
 * Data flow:
 *   Outbound: photon_conn_send() -> output thread -> socket
 *   Inbound:  socket -> input thread -> ring buffer -> photon_conn_recv()
 *
 * All functions are thread-safe.  photon_conn_connect() spawns I/O threads.
 * photon_conn_close() signals them to stop and joins before returning.
 *
 * Multi-tab support:
 *   Each tab has its own photon_conn_t handle (allocated via photon_conn_new()).
 *   photon_conn_set_active() switches which handle send/recv/etc. operate on.
 *   Background connections stay live (their I/O threads keep running);
 *   photon_conn_data_waiting() on the active handle drives the UI.
 */

#pragma once

#include "photon_compat.h"
#include "photon_bbs.h"

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

/* Opaque connection handle */
typedef struct photon_conn photon_conn_t;

/* ── Connection result type ─────────────────────────────────────────── */

typedef enum {
    PHOTON_CONN_OK        = 0,
    PHOTON_CONN_ERR_DNS   = 1,
    PHOTON_CONN_ERR_TCP   = 2,
    PHOTON_CONN_ERR_AUTH  = 3,
    PHOTON_CONN_ERR_PROTO = 4,
    PHOTON_CONN_ERR_OTHER = 5,
} photon_conn_err_t;

/* ── Public API ─────────────────────────────────────────────────────── */

/* Connect to a BBS entry.  Spawns I/O threads.  Returns true on success. */
bool   photon_conn_connect(const photon_bbs_t *bbs);

/* Disconnect and join I/O threads. */
void   photon_conn_close(void);

/* Returns true if a connection is currently live. */
bool   photon_conn_connected(void);

/* Returns the number of bytes available to read right now (non-blocking). */
size_t photon_conn_data_waiting(void);

/* Read up to buflen bytes.  timeout_ms==0 -> non-blocking.
 * Returns number of bytes read, 0 on timeout / no data, -1 on error. */
int    photon_conn_recv(void *buf, size_t buflen, unsigned int timeout_ms);

/* Send exactly buflen bytes.  timeout_ms==0 -> non-blocking.
 * Returns bytes sent, or -1 on error. */
int    photon_conn_send(const void *buf, size_t buflen, unsigned int timeout_ms);

/* Notify the remote that the terminal dimensions have changed.
 * SSH: PTY resize request; Telnet: NAWS SB; PTY: TIOCSWINSZ. */
void photon_conn_resize(int cols, int rows);

/* Last human-readable error string (static storage, valid until next call). */
const char *photon_conn_last_error(void);

/* ── Multi-connection handle API ─────────────────────────────────────── */

/* Allocate a new unconnected handle (for tabs). */
photon_conn_t *photon_conn_new(void);

/* Free a closed connection handle. */
void photon_conn_free(photon_conn_t *C);

/* Switch which handle send/recv/etc. operate on. */
void photon_conn_set_active(photon_conn_t *C);

/* Return the currently active handle. */
photon_conn_t *photon_conn_get_active(void);

/* Set a callback that is invoked to prompt the user for an SSH password.
 * The callback should return true if the user entered something, false to cancel.
 * Call with NULL,NULL to clear. */
typedef bool (*photon_conn_prompt_fn)(const char *prompt, char *out,
                                      size_t outsz, void *userdata);
void photon_conn_set_ssh_prompt(photon_conn_prompt_fn fn, void *userdata);

/*
 * photon_conn_raw_fd - return the raw file descriptor for direct I/O.
 *
 * For PHOTON_CONN_PTY: returns the PTY master fd.
 * For PHOTON_CONN_TELNET: returns the TCP socket fd.
 * For PHOTON_CONN_SSH: returns -1 (no raw fd; use photon_conn_send/recv).
 *
 * Returns -1 if not connected.
 *
 * NOTE: while the raw fd is in use the caller is responsible for NOT
 * calling photon_conn_send/recv concurrently (the I/O threads must be
 * paused or the caller must hold appropriate locks).
 */
int photon_conn_raw_fd(void);
