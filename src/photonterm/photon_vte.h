/* PhotonTERM VTE - Clean-room ANSI/VT terminal emulator
 *
 * Copyright (C) 2026 fewtarius and PhotonTERM contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Architecture follows the DEC STD-070 state machine described at:
 *   https://vt100.net/emu/dec_ansi_parser   (public domain reference)
 *
 * Designed to replace cterm.c for BBS use.  Supports:
 *   - CP437 -> Unicode mapping
 *   - VT100 / ANSI X3.64 cursor movement and SGR colour
 *   - 80x24 (or any configured size) cell grid
 *   - 16-colour ANSI palette + bold/blink/reverse attributes
 *   - Scrollback buffer
 *   - Autowrap, scroll regions (top/bottom margins)
 *   - Renders via callbacks (draw_cell / set_cursor / scroll)
 *
 * Intentionally omits: PETSCII, Avatar, RIPscrip, Prestel,
 *   sixel, ANSI music - none used on modern BBS systems.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── Cell ──────────────────────────────────────────────────────────────── */

/* Attribute flags packed into vte_cell.attr */
#define VTE_ATTR_BOLD       0x01
#define VTE_ATTR_BLINK      0x02
#define VTE_ATTR_REVERSE    0x04
#define VTE_ATTR_UNDERLINE  0x08
#define VTE_ATTR_CONCEALED  0x10
#define VTE_ATTR_FG_RGB     0x20  /* fg_rgb is valid (24-bit truecolor fg) */
#define VTE_ATTR_BG_RGB     0x40  /* bg_rgb is valid (24-bit truecolor bg) */

/* ANSI/BBS colour indices 0-7; high-intensity via BOLD or index 8-15 */
#define VTE_COLOR_DEFAULT_FG 7
#define VTE_COLOR_DEFAULT_BG 0

typedef struct {
    uint32_t codepoint; /* Unicode codepoint (from CP437 or direct UTF-8) */
    uint8_t  fg;        /* foreground colour index 0-255 (xterm-256) */
    uint8_t  bg;        /* background colour index 0-255 (xterm-256) */
    uint8_t  attr;      /* VTE_ATTR_* flags */
    uint8_t  _pad;      /* padding for alignment */
    uint32_t fg_rgb;    /* truecolor fg as 0x00RRGGBB (valid if VTE_ATTR_FG_RGB) */
    uint32_t bg_rgb;    /* truecolor bg as 0x00RRGGBB (valid if VTE_ATTR_BG_RGB) */
} vte_cell_t;

/* ── VTE instance ──────────────────────────────────────────────────────── */

typedef struct vte vte_t;

/* Callback invoked by vte_input() when a cell changes.
 * col/row are 1-based.  Implementor calls gotoxy()+putch() or equivalent. */
typedef void (*vte_draw_cb)(vte_t *vte, int col, int row, const vte_cell_t *cell, void *user);

/* Callback invoked when cursor position changes (1-based col/row). */
typedef void (*vte_cursor_cb)(vte_t *vte, int col, int row, void *user);

/* Callback invoked when a full or partial scroll-region clear is needed. */
typedef void (*vte_clear_cb)(vte_t *vte, int col1, int row1, int col2, int row2, void *user);

/* Callback invoked when the terminal requests a device attributes response.
 * Implementation should write the response string to the output connection. */
typedef void (*vte_response_cb)(vte_t *vte, const char *str, size_t len, void *user);

/* Callback invoked when OSC 0 or OSC 2 sets the window title.
 * title is a NUL-terminated UTF-8 string owned by the VTE (valid only
 * during this call - copy if you need to keep it). */
typedef void (*vte_title_cb)(vte_t *vte, const char *title, void *user);

/* Callback invoked on BEL (0x07).  Implementation may flash the screen or
 * play a sound.  May be NULL to suppress the bell entirely. */
typedef void (*vte_bell_cb)(vte_t *vte, void *user);

typedef struct {
    vte_draw_cb     draw;
    vte_cursor_cb   cursor;
    vte_clear_cb    clear;
    vte_response_cb response;
    vte_title_cb    title;
    vte_bell_cb     bell;
    void           *user;
} vte_callbacks_t;

/* ── Public API ────────────────────────────────────────────────────────── */

/* Create a VTE instance.
 *   cols/rows     - terminal grid size (e.g. 80, 24)
 *   backlines     - number of scrollback lines (0 = no scrollback)
 *   cb            - rendering callbacks (may be NULL for headless)
 *   cp437         - if true, bytes 0x00-0xFF are CP437; else UTF-8
 * Returns NULL on OOM. */
vte_t *vte_create(int cols, int rows, int backlines, const vte_callbacks_t *cb, bool cp437);

/* Free a VTE instance. */
void   vte_free(vte_t *vte);

/* Feed raw bytes from the remote (BBS output) into the emulator.
 * Returns number of bytes consumed (always == len unless internal error). */
size_t vte_input(vte_t *vte, const uint8_t *data, size_t len);

/* Reset to power-on state (clear screen, home cursor, default attributes). */
void   vte_reset(vte_t *vte, bool hard);

/* Resize the terminal grid.  Existing content is preserved where possible.
 * Triggers a full redraw via callbacks. */
void   vte_resize(vte_t *vte, int cols, int rows);

/* Read current cursor position (1-based). */
void   vte_cursor_pos(const vte_t *vte, int *col, int *row);

/* Read a cell from the current screen (1-based col/row).
 * Returns false if out of range. */
bool   vte_get_cell(const vte_t *vte, int col, int row, vte_cell_t *out);

/* Grid dimensions. */
int    vte_cols(const vte_t *vte);
int    vte_rows(const vte_t *vte);

/* Scrollback access (line 0 = oldest).  Returns false if no scrollback. */
bool   vte_scrollback_get(const vte_t *vte, int line, vte_cell_t *row_out, int *ncols);
int    vte_scrollback_lines(const vte_t *vte);

/* Force a full repaint via callbacks. */
void   vte_repaint(vte_t *vte);

/* Enable/disable CP437 byte mapping (can be changed live). */
void   vte_set_cp437(vte_t *vte, bool cp437);

/* Update the user data pointer passed to all callbacks (e.g. after tab
 * creation when the connection handle is not yet known). */
void   vte_set_user(vte_t *vte, void *user);

#ifdef __cplusplus
}
#endif
