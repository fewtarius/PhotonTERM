/* photon_sdl.h - PhotonTERM SDL2 display / keyboard layer
 *
 * Copyright (C) 2026 fewtarius and PhotonTERM contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Provides:
 *   - SDL2 window creation and management
 *   - TTF font rendering of VTE cells (photon_vte.h)
 *   - Keyboard and mouse event translation to photon_key_t
 *   - 16-colour ANSI palette
 *   - Cursor blink
 */

#pragma once

#include "photon_vte.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── Error codes ──────────────────────────────────────────────────────── */

typedef enum {
    PHOTON_SDL_OK           =  0,
    PHOTON_SDL_ERR_SDL      = -1,   /* SDL_Init or window creation failed */
    PHOTON_SDL_ERR_TTF      = -2,   /* SDL_ttf / font load failed */
    PHOTON_SDL_ERR_OOM      = -3,   /* malloc failed */
    PHOTON_SDL_ERR_BADARG   = -4,   /* bad parameter */
} photon_sdl_err_t;

/* ── Key event ────────────────────────────────────────────────────────── */

/* Modifier flags */
#define PHOTON_MOD_SHIFT    0x01
#define PHOTON_MOD_CTRL     0x02
#define PHOTON_MOD_ALT      0x04
#define PHOTON_MOD_META     0x08

/* Special key codes (> 127 to avoid ASCII conflict) */
#define PHOTON_KEY_NONE       0
#define PHOTON_KEY_UP       256
#define PHOTON_KEY_DOWN     257
#define PHOTON_KEY_LEFT     258
#define PHOTON_KEY_RIGHT    259
#define PHOTON_KEY_HOME     260
#define PHOTON_KEY_END      261
#define PHOTON_KEY_PGUP     262
#define PHOTON_KEY_PGDN     263
#define PHOTON_KEY_INS      264
#define PHOTON_KEY_DEL      265
#define PHOTON_KEY_F1       266
#define PHOTON_KEY_F2       267
#define PHOTON_KEY_F3       268
#define PHOTON_KEY_F4       269
#define PHOTON_KEY_F5       270
#define PHOTON_KEY_F6       271
#define PHOTON_KEY_F7       272
#define PHOTON_KEY_F8       273
#define PHOTON_KEY_F9       274
#define PHOTON_KEY_F10      275
#define PHOTON_KEY_F11      276
#define PHOTON_KEY_F12      277
#define PHOTON_KEY_QUIT     278     /* window close button */
#define PHOTON_KEY_COPY_SEL 279     /* mouse selection complete - copy to clipboard */
#define PHOTON_KEY_PASTE    280     /* middle/right click - paste from clipboard */

typedef struct {
    int     code;       /* ASCII 1-127, or PHOTON_KEY_*, or 0 for no event */
    int     mod;        /* PHOTON_MOD_* flags */
    char    text[8];    /* UTF-8 text for printable chars (SDL_TEXTINPUT) */
} photon_key_t;

/* ── Context ──────────────────────────────────────────────────────────── */

typedef struct photon_sdl photon_sdl_t;

/* ── Init / teardown ──────────────────────────────────────────────────── */

/* Create a window and renderer.
 *   title       - Window title string
 *   cols/rows   - Terminal grid size (e.g. 80, 24)
 *   font_path   - Path to .ttf file (NULL -> search bundle/3rdp)
 *   font_pt     - Point size (0 -> auto: 16pt)
 * Returns NULL on failure (check photon_sdl_last_error()). */
photon_sdl_t *photon_sdl_create(const char *title, int cols, int rows,
                                const char *font_path, int font_pt);

/* Destroy a context (closes window, frees resources). */
void photon_sdl_free(photon_sdl_t *ctx);

/* Last error string (static storage - not thread safe). */
const char *photon_sdl_last_error(void);

/* ── Rendering ────────────────────────────────────────────────────────── */

/* Draw a single cell at (col, row) 1-based.
 * Call photon_sdl_present() to flush to screen. */
void photon_sdl_draw_cell(photon_sdl_t *ctx, int col, int row,
                          const vte_cell_t *cell);

/* Read a cell from the shadow buffer (what's currently displayed).
 * Returns true if col/row are valid and shadow is populated. */
bool photon_sdl_get_cell(const photon_sdl_t *ctx, int col, int row,
                         vte_cell_t *cell);

/* Draw the cursor at (col, row) 1-based, with the underlying cell.
 * Pass col=0 to hide cursor. */
void photon_sdl_draw_cursor(photon_sdl_t *ctx, int col, int row,
                            const vte_cell_t *cell);

/* Fill a rectangular region with blank cells using current background attr.
 * col1/row1 inclusive, col2/row2 inclusive, 1-based. */
void photon_sdl_clear_rect(photon_sdl_t *ctx,
                           int col1, int row1, int col2, int row2,
                           uint8_t fg, uint8_t bg);

/* Flush pending draw calls to the screen. */
void photon_sdl_present(photon_sdl_t *ctx);

/* Force a full repaint from the VTE's cell grid. */
void photon_sdl_repaint(photon_sdl_t *ctx, vte_t *vte);

/* Show a "Connecting to <name> ..." splash on a blank screen. */
void photon_sdl_show_connecting(photon_sdl_t *ctx, const char *bbs_name);

/* Repaint from the shadow cell buffer (used for expose events). */
void photon_sdl_repaint_shadow(photon_sdl_t *ctx);

/* Check and clear the expose_pending flag.
   Returns true if the window needs a repaint (e.g. was uncovered). */
bool photon_sdl_take_expose(photon_sdl_t *ctx);

/* ── Input ────────────────────────────────────────────────────────────── */

/* Poll for the next key event.  Non-blocking; returns false if no event.
 * Also drives window events (resize, quit). */
bool photon_sdl_poll_key(photon_sdl_t *ctx, photon_key_t *key);
bool photon_sdl_peek_key(photon_sdl_t *ctx, photon_key_t *key);

/* Global SDL handle - set by photonterm.c after window creation */
extern photon_sdl_t *photon_sdl_global;

/* Wait up to timeout_ms milliseconds for a key event.
 * Returns false on timeout. */
bool photon_sdl_wait_key(photon_sdl_t *ctx, photon_key_t *key,
                           int timeout_ms);

/* Discard all queued key events (SDL queue + internal key ring).
 * Call this before opening a modal dialog to prevent key-press bleed. */
void photon_sdl_flush_keys(photon_sdl_t *ctx);

/* Returns true if the user has requested the window to close. */
bool photon_sdl_quit_requested(const photon_sdl_t *ctx);

/* ── Grid info ────────────────────────────────────────────────────────── */

/* Current grid dimensions (may change after a resize event). */
int photon_sdl_cols(const photon_sdl_t *ctx);
int photon_sdl_rows(const photon_sdl_t *ctx);

/* Physical pixel size of one cell. */
int photon_sdl_cell_width(const photon_sdl_t *ctx);
int photon_sdl_cell_height(const photon_sdl_t *ctx);

/* Check for a pending window-driven resize (user dragged the window).
 * Returns true and stores new grid dimensions in nc and nr if changed.
 * Updates ctx->cols/rows and reallocates the shadow buffer. */
bool photon_sdl_check_resize(photon_sdl_t *ctx, int *nc, int *nr);

/* Notify ctx that the VTE has been resized to new_cols x new_rows.
 * Resizes the SDL window to match and repaints. */
void photon_sdl_notify_resize(photon_sdl_t *ctx, vte_t *vte,
                              int new_cols, int new_rows);

/* ── Palette ──────────────────────────────────────────────────────────── */

/* Set one of the 256 palette entries (index 0-255).
 * Default: indices 0-15 = ANSI SGR palette; 16-255 = zero (black).
 * r/g/b in 0-255. */
void photon_sdl_set_palette(photon_sdl_t *ctx, int index,
                            uint8_t r, uint8_t g, uint8_t b);

/* Load the standard xterm-256 palette (0-15 xterm system, 16-231 colour cube,
 * 232-255 greyscale).  Use for shell/modern terminal sessions. */
void photon_sdl_load_xterm_palette(photon_sdl_t *ctx);

/* Load the ANSI SGR 16-colour palette (indices 0-15; 16-255 unchanged).
 * This is the correct VT100/xterm default for BBS and terminal sessions. */
void photon_sdl_load_ansi_palette(photon_sdl_t *ctx);

/* Deprecated alias for photon_sdl_load_ansi_palette(). */
void photon_sdl_load_cga_palette(photon_sdl_t *ctx);

/* Set Unicode/TTF rendering mode.  When enabled, the CP437 bitmap atlas is
 * skipped and all glyphs are rendered via SDL2_ttf (full Unicode coverage).
 * When disabled (default), the CP437 atlas is used for ASCII/CP437 chars. */
void photon_sdl_set_ttf_mode(photon_sdl_t *ctx, bool enable);
bool photon_sdl_get_ttf_mode(const photon_sdl_t *ctx);

/* Save/restore the full 256-entry palette.  buf must be 768 bytes. */
void photon_sdl_save_palette(const photon_sdl_t *ctx, uint8_t buf[768]);
void photon_sdl_restore_palette(photon_sdl_t *ctx, const uint8_t buf[768]);

/* Mouse text selection.  The SDL event handler populates the selection state.
 * photon_sdl_get_selection() returns normalized coords (start always <= end).
 * Returns false if no selection is active. */
bool photon_sdl_get_selection(const photon_sdl_t *ctx,
                               int *c0, int *r0, int *c1, int *r1);
void photon_sdl_clear_selection(photon_sdl_t *ctx);
bool photon_sdl_sel_active(const photon_sdl_t *ctx);  /* true while mouse dragging */

/* ── VTE callback adapter ─────────────────────────────────────────────── */

/* Returns a vte_callbacks_t populated to render via this ctx.
 * The ctx pointer is stored in callbacks->user.
 * After calling vte_create(... &cbs ...), calls to vte_input() will
 * automatically render to the SDL window. */
vte_callbacks_t photon_sdl_make_vte_callbacks(photon_sdl_t *ctx);

/* Set the SDL window title (from OSC title sequences or user code). */
void photon_sdl_set_title(photon_sdl_t *ctx, const char *title);

/* Visual bell: briefly flash the window (white overlay, ~100ms).
 * Safe to call from any thread. */
void photon_sdl_bell_flash(photon_sdl_t *ctx);

#ifdef __cplusplus
}
#endif
