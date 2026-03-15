/* photon_settings.h - Application settings and themes
 *
 * Copyright (C) 2026 fewtarius and PhotonTERM contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "photon_bbs.h"

/* ── Font selection ─────────────────────────────────────────────────── */
typedef enum {
    PHOTON_FONT_BITMAP = 0,   /* Built-in bitmap font */
    PHOTON_FONT_TTF    = 1,   /* Terminus TTF */
} photon_font_mode_t;

/* ── Theme ──────────────────────────────────────────────────────────── */
/*
 * A photon_theme_t defines the UI colour scheme using CGA 16-colour indices
 * plus an optional per-slot RGB palette override.
 *
 * Field semantics (same as original bbslist.c uifc fields):
 *   hclr   - border/frame foreground   (CGA 0-15)
 *   lclr   - normal list text fg       (CGA 0-15)
 *   bclr   - window background         (CGA bg 0-7)
 *   cclr   - inactive/title background (CGA bg 0-7; != bclr)
 *   lbclr  - selection bar fg          (CGA 0-15)
 *   lbbclr - selection bar bg          (CGA bg 0-7; != bclr)
 *
 * rgb[16] - custom 0x00RRGGBB for each CGA slot 0-15; 0 = use default VGA value.
 */
typedef struct {
    const char *name;
    uint8_t hclr;
    uint8_t lclr;
    uint8_t bclr;
    uint8_t cclr;
    uint8_t lbclr;
    uint8_t lbbclr;
    uint32_t rgb[16];   /* 0x00RRGGBB per CGA slot; 0 = standard value */
} photon_theme_t;

/* CGA colour index names (for convenience) */
#define CGA_BLACK         0
#define CGA_BLUE          1
#define CGA_GREEN         2
#define CGA_CYAN          3
#define CGA_RED           4
#define CGA_MAGENTA       5
#define CGA_BROWN         6
#define CGA_LIGHTGRAY     7
#define CGA_DARKGRAY      8
#define CGA_LIGHTBLUE     9
#define CGA_LIGHTGREEN   10
#define CGA_LIGHTCYAN    11
#define CGA_LIGHTRED     12
#define CGA_LIGHTMAGENTA 13
#define CGA_YELLOW       14
#define CGA_WHITE        15

/* Built-in theme table (NULL-name terminated) */
extern const photon_theme_t photon_themes[];

/* Return the index of the currently active theme (or 0 if not found) */
int photon_theme_find(const char *name);

/* ── Settings ───────────────────────────────────────────────────────── */
typedef struct photon_settings {
    /* Display */
    int   cols;           /* Terminal columns (default 80) */
    int   rows;           /* Terminal rows (default 25) */
    int   font_height;    /* Bitmap font height in pixels (0=auto) */

    /* Font */
    photon_font_mode_t font_mode;
    char  ttf_path[512]; /* Path to TTF file (empty = bundled Terminus) */
    int   ttf_size_pt;   /* TTF point size (default 12) */

    /* Theme */
    char  theme_name[64];/* Active theme name */

    /* Misc */
    bool  invert_scroll;  /* Invert mouse scroll direction */
    bool  debug;          /* Enable debug log */
    bool  bell_enabled;   /* Visual bell on BEL (ESC-G / 0x07) */

    /* Palette */
    photon_palette_mode_t default_palette_mode;  /* PHOTON_PALETTE_AUTO/ANSI/XTERM */
} photon_settings_t;

/* Currently active theme index (index into photon_themes[]) */
extern int photon_active_theme;

/* Apply a theme: update active_theme and set the SDL palette entries.
 * sdl may be NULL (palette not updated).
 * Saves theme name to *s (caller responsible for photon_settings_save). */
struct photon_sdl;   /* forward decl (full type in photon_sdl.h) */
void photon_theme_apply(int idx, struct photon_sdl *sdl,
                        photon_settings_t *s);

/* Reset SDL palette to the ANSI SGR 16-colour defaults (no theme overrides).
 * Call before entering a terminal session so SGR colours are accurate
 * regardless of the current UI theme. */
void photon_sdl_reset_to_ansi_palette(struct photon_sdl *sdl);

/* Deprecated alias for photon_sdl_reset_to_ansi_palette(). */
void photon_sdl_reset_to_cga_palette(struct photon_sdl *sdl);

/* Apply palette to sdl based on mode + conn_type (for AUTO resolution).
 * Equivalent to calling photon_sdl_load_ansi_palette or photon_sdl_load_xterm_palette. */
void photon_sdl_apply_palette_mode(struct photon_sdl *sdl,
                                   photon_palette_mode_t mode,
                                   photon_conn_type_t    conn_type);

/* Load settings from disk (photon_store_config_dir()/settings.ini).
 * Returns true if a file was found; false if defaults used. */
bool photon_settings_load(photon_settings_t *s);

/* Save settings to disk.  Returns true on success. */
bool photon_settings_save(const photon_settings_t *s);

/* Fill *s with compiled-in defaults. */
void photon_settings_defaults(photon_settings_t *s);
