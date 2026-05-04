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

#include "photon_conn.h"

/* ── Tab bar / status bar ──────────────────────────────────────────── */

#define PHOTON_TAB_BAR_MAX 9

typedef struct {
    int   ntabs;
    int   active;       /* 0-based index of active tab */
    char  names[PHOTON_TAB_BAR_MAX][64];
    bool  activity[PHOTON_TAB_BAR_MAX];  /* unseen activity flag */
    photon_conn_type_t conn_type;        /* active tab's connection type */
} photon_tab_bar_t;


/* Return codes from photon_doterm() and key handling */
typedef enum {
    PHOTON_TERM_DISCONNECT = 0,  /* remote disconnected - return to BBS list */
    PHOTON_TERM_QUIT,            /* user closed window / requested app exit  */
    PHOTON_TERM_NEWTAB,          /* user requested new tab (Alt-W)           */
    PHOTON_TERM_SWITCH_TAB,      /* user switched tab; check photon_switch_tab_target */
    PHOTON_TERM_RESUME,          /* menu closed, resume terminal (no action) */
    PHOTON_TERM_CONTINUE,        /* no action needed, keep running */
} photon_term_result_t;

/* Session menu callback type.
 * Host apps can override the Alt-Z session menu by setting this callback.
 * Return PHOTON_TERM_RESUME to return to the terminal with no action. */
typedef photon_term_result_t (*photon_session_menu_fn)(
    photon_ui_t *ui, photon_sdl_t *sdl, vte_t *vte,
    const photon_bbs_t *bbs, photon_settings_t *settings,
    void *userdata);

/* Set a custom session menu callback (replaces the built-in Alt-Z menu).
 * Pass NULL to restore the default PhotonTERM session menu. */
void photon_term_set_session_menu(photon_session_menu_fn fn, void *userdata);

/* Render overlay callback - called after terminal repaint, before present.
 * Host apps use this to draw status indicators, spinners, etc. */
typedef void (*photon_render_overlay_fn)(photon_sdl_t *sdl, vte_t *vte,
                                         void *userdata);

/* Set a render overlay callback. Called every frame after terminal renders. */
void photon_term_set_render_overlay(photon_render_overlay_fn fn, void *userdata);

/* Key hook callback type.
 * Called for every key event before PhotonTERM processes it.
 * Return true to consume the key (PhotonTERM will not process it further).
 * Return false to let PhotonTERM handle it normally. */
typedef bool (*photon_key_hook_fn)(const photon_key_t *key,
                                   photon_sdl_t *sdl,
                                   photon_settings_t *settings,
                                   void *userdata);

/* Set a key hook callback. Pass NULL to clear. */
void photon_term_set_key_hook(photon_key_hook_fn fn, void *userdata);

/* ── Unified main loop API ──────────────────────────────────────────── */

/* Non-blocking: drain data from connection into VTE for one tab.
 * For background tabs (is_active=false), only processes data (no rendering).
 * For active tab, also handles rendering if dirty.
 * Returns true if data was processed. */
bool photon_term_pump_tab(vte_t *vte, photon_conn_t *conn,
                          photon_sdl_t *sdl, bool is_active,
                          const photon_tab_bar_t *tabbar);

/* Handle a key event for the active tab.
 * Returns an action code the main loop should act on.
 * May trigger modal dialogs (scrollback viewer, session menu, etc.)
 * which block internally but pump SDL events. */
photon_term_result_t photon_term_handle_key(const photon_key_t *k,
                                            vte_t *vte, photon_conn_t *conn,
                                            photon_sdl_t *sdl, photon_ui_t *ui,
                                            const photon_bbs_t *bbs,
                                            photon_settings_t *settings,
                                            const photon_tab_bar_t *tabbar);

/* Check if the connection for the active tab is still live.
 * Returns PHOTON_TERM_CONTINUE if OK, PHOTON_TERM_DISCONNECT if dead. */
photon_term_result_t photon_term_check_connection(photon_conn_t *conn);

/* Render the active tab's VTE to screen (call after pump + key handling).
 * Handles frame-rate limiting and overlay drawing. */
void photon_term_render(photon_sdl_t *sdl, vte_t *vte,
                        const photon_tab_bar_t *tabbar, bool dirty);

/* Force the next photon_term_render() call to render immediately,
 * bypassing the 60fps frame timer.  Call after tab switches, resizes,
 * and other UI events that need immediate visual feedback. */
void photon_term_render_force_next(void);

/* Tab switch target (0-based index) when result is PHOTON_TERM_SWITCH_TAB */
extern int photon_switch_tab_target;
