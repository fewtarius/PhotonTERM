/* photon_ui.h - PhotonTERM text-mode UI (list picker, input, message popup)
 *
 * Copyright (C) 2026 fewtarius and PhotonTERM contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Clean text-mode UI widget library: menus, input boxes, message dialogs,
 * SDL-backed implementation that draws directly into the photon_sdl renderer.
 *
 * All widget functions are modal: they block until the user completes or
 * cancels, then return to the caller.  The underlying terminal content is
 * saved and restored around each popup.
 */

#pragma once

#include "photon_sdl.h"
#include "photon_vte.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── Context ──────────────────────────────────────────────────────────── */

typedef struct photon_ui photon_ui_t;

/* Global UI handle - set by photonterm.c after creating the ui instance.
 * NULL until photonterm.c initialises the SDL window. */
extern photon_ui_t *photon_ui_global;

/* Create a UI context bound to an SDL window and VTE.
 * The ctx shares the SDL renderer and must outlive all widget calls.
 * Returns NULL on OOM. */
photon_ui_t *photon_ui_create(photon_sdl_t *sdl, vte_t *vte);
void         photon_ui_free(photon_ui_t *ui);

/* Accessors for embedded SDL / VTE handles */
photon_sdl_t *photon_ui_sdl(photon_ui_t *ui);
vte_t        *photon_ui_vte(photon_ui_t *ui);

/* ── List picker ──────────────────────────────────────── */
/*
 *   title       - Box title (displayed in top border)
 *   items       - NULL-terminated array of C strings
 *   n_items     - number of valid entries in items[] (0 = count to NULL)
 *   cur         - [in/out] currently selected index (preserved across calls)
 *
 * Returns selected index (0-based), or -1 if cancelled (ESC).
 * Saves and restores the underlying screen. */
int photon_ui_list(photon_ui_t *ui,
                   const char *title,
                   const char **items,
                   int n_items,
                   int *cur);

/* ── Text input ───────────────────────────────────────────────────────── */

/* Input field flags (compatible with old K_* constants where useful) */
#define PHOTON_INPUT_EDIT      0x0001  /* pre-fill buf with existing value */
#define PHOTON_INPUT_PASSWORD  0x0002  /* mask input with '*' */
#define PHOTON_INPUT_NUMBER    0x0004  /* accept digits only */
#define PHOTON_INPUT_TRIM      0x0008  /* trim leading/trailing whitespace */
#define PHOTON_INPUT_NOSAVE    0x0010  /* caller manages screen save/restore */

/* Show a single-line input dialog.
 *
 *   title       - Prompt label
 *   buf         - [in/out] buffer; pre-filled if PHOTON_INPUT_EDIT
 *   buflen      - capacity of buf (including NUL)
 *   flags       - PHOTON_INPUT_* flags
 *
 * Returns length of entered string, or -1 if cancelled. */
int photon_ui_input(photon_ui_t *ui,
                    const char *title,
                    char *buf,
                    int buflen,
                    int flags);

/* ── Message popup ────────────────────────────────────────────────────── */

/* Show a one-line message; user presses any key to dismiss. */
void photon_ui_msg(photon_ui_t *ui, const char *message);

/* ── Scrollable text viewer ───────────────────────────────────────────── */

/* Show a scrollable multi-line text buffer.
 *
 *   title       - Box title
 *   text        - newline-separated text; supports \x01 ANSI colours
 *   max_cols    - maximum window width (0 = auto: 78)
 *   max_rows    - maximum window height (0 = auto: 20)
 */
void photon_ui_showbuf(photon_ui_t *ui,
                       const char *title,
                       const char *text,
                       int max_cols,
                       int max_rows);

/* ── Yes/No confirm ───────────────────────────────────────────────────── */

/* Returns true = Yes, false = No/cancel. */
bool photon_ui_confirm(photon_ui_t *ui, const char *question);

/* ── Screen save / restore ────────────────────────────────────────────── */

/* Opaque saved-screen handle */
typedef struct photon_ui_screen photon_ui_screen_t;

/* Save the current terminal grid (allocates, caller frees with _free). */
photon_ui_screen_t *photon_ui_save_screen(photon_ui_t *ui);

/* Restore a previously saved screen and repaint. */
void photon_ui_restore_screen(photon_ui_t *ui, photon_ui_screen_t *s);

/* Free a saved screen (does NOT restore). */
void photon_ui_free_screen(photon_ui_screen_t *s);

#ifdef __cplusplus
}
#endif
