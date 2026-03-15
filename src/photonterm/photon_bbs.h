/* photon_bbs.h - PhotonTERM BBS entry (dialing directory record)
 *
 * Copyright (C) 2026 fewtarius and PhotonTERM contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <time.h>

#define PHOTON_BBS_NAME_MAX    128
#define PHOTON_BBS_ADDR_MAX    512
#define PHOTON_BBS_USER_MAX    64
#define PHOTON_BBS_PASS_MAX    256
#define MAX_BBS_ENTRIES        256
#define PHOTON_BBS_COMMENT_MAX 512

typedef enum {
    PHOTON_CONN_TELNET = 0,
    PHOTON_CONN_SSH    = 1,
    PHOTON_CONN_SHELL  = 2,
} photon_conn_type_t;

/* Per-BBS colour palette mode */
typedef enum {
    PHOTON_PALETTE_AUTO  = 0, /* ANSI-16 for telnet/SSH BBS, xterm-256 for shell */
    PHOTON_PALETTE_ANSI  = 1, /* ANSI SGR 16-color (correct VT100/xterm default) */
    PHOTON_PALETTE_XTERM = 2, /* xterm 256-color (full palette for modern shells) */
    /* Alias so stored settings (value=1) still work */
    PHOTON_PALETTE_CGA   = PHOTON_PALETTE_ANSI,
} photon_palette_mode_t;

/* Per-BBS terminal mode override */
typedef enum {
    PHOTON_TERM_MODE_AUTO  = 0,  /* use global setting (default) */
    PHOTON_TERM_MODE_CP437 = 1,  /* force CP437 bitmap (classic BBS art) */
    PHOTON_TERM_MODE_UTF8  = 2,  /* force TTF Unicode */
} photon_term_mode_t;

#define PHOTON_FINGERPRINT_LEN 20

typedef struct {
    char     name[PHOTON_BBS_NAME_MAX + 1];
    char     addr[PHOTON_BBS_ADDR_MAX + 1];
    uint16_t port;
    char     user[PHOTON_BBS_USER_MAX + 1];
    char     pass[PHOTON_BBS_PASS_MAX + 1];
    char     comment[PHOTON_BBS_COMMENT_MAX + 1];

    photon_conn_type_t conn_type;
    photon_term_mode_t term_mode;  /* rendering mode override (auto/cp437/utf8) */
    photon_palette_mode_t palette_mode; /* colour palette override */

    time_t   added;
    time_t   last_connected;
    unsigned calls;

    bool     has_fingerprint;
    uint8_t  ssh_fingerprint[PHOTON_FINGERPRINT_LEN];

    int      id;
} photon_bbs_t;

static inline uint16_t photon_bbs_default_port(photon_conn_type_t t)
{
    switch (t) {
        case PHOTON_CONN_TELNET: return 23;
        case PHOTON_CONN_SSH:    return 22;
        default:                 return 0;
    }
}

static inline uint16_t photon_bbs_port(const photon_bbs_t *b)
{
    return (b->port != 0) ? b->port : photon_bbs_default_port(b->conn_type);
}
