/* photon_term.h - PhotonTERM terminal session loop public API
 *
 * Copyright (C) 2026 fewtarius and PhotonTERM contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include "photon_vte.h"
#include "photon_sdl.h"
#include "photon_bbs.h"
#include "photon_ui.h"
#include "photon_settings.h"

#include <stdbool.h>

/* ── Tab bar overlay (shown while Alt is held) ──────────────────────── */

#define PHOTON_TAB_BAR_MAX 9

typedef struct {
    int   ntabs;
    int   active;       /* 0-based index of active tab */
    char  names[PHOTON_TAB_BAR_MAX][64];
    bool  activity[PHOTON_TAB_BAR_MAX];  /* unseen activity flag */
} photon_tab_bar_t;


/* Return codes from photon_doterm() */
typedef enum {
    PHOTON_TERM_DISCONNECT = 0,  /* remote disconnected - return to BBS list */
    PHOTON_TERM_QUIT,            /* user closed window / requested app exit  */
    PHOTON_TERM_NEWTAB,          /* user requested new tab (Alt-W)           */
    PHOTON_TERM_SWITCH_TAB,      /* user switched tab; check photon_switch_tab_target */
} photon_term_result_t;

/* Run one terminal session.
 *
 * vte  - pre-initialised VTE emulator (will receive remote data)
 * sdl  - SDL context (renders VTE output, delivers key events)
 * bbs  - BBS entry being connected (used for logging / protocol hints)
 *
 * The connection must already be open before calling this function.
 * Returns when the session ends (disconnect, quit, or new-tab request).
 */
photon_term_result_t photon_doterm(vte_t *vte, photon_sdl_t *sdl,
                                   photon_ui_t *ui, const photon_bbs_t *bbs,
                                   photon_settings_t *settings,
                                   const photon_tab_bar_t *tabbar);

/* Tab switch target (0-based index) when photon_doterm returns PHOTON_TERM_SWITCH_TAB */
extern int photon_switch_tab_target;
