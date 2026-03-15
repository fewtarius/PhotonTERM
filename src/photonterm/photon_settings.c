/* photon_settings.c - Application settings and themes
 *
 * Copyright (C) 2026 fewtarius and PhotonTERM contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "photon_settings.h"
#include "photon_sdl.h"
#include "photon_store.h"
#include "photon_ui.h"
#include "photon_compat.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <limits.h>

/* ── Active theme state ─────────────────────────────────────────────── */
int photon_active_theme = 0;

/* ── Built-in theme table ───────────────────────────────────────────── */
/* Standard VGA RGB values indexed by CGA attribute 0-15 */
/* ANSI SGR 16-colour palette in packed RGB.  This is the norm.
 * Index order matches ECMA-48 SGR: 0=Black 1=Red 2=Green 3=Yellow
 * 4=Blue 5=Magenta 6=Cyan 7=White, then bright variants 8-15.
 * CGA hardware used a different index order; that is the deviation. */
static const uint32_t ansi_sgr_rgb[16] = {
    0x000000, 0xaa0000, 0x00aa00, 0xaa5500,  /* 0-3  Black Red Green Brown/DkYellow */
    0x0000aa, 0xaa00aa, 0x00aaaa, 0xaaaaaa,  /* 4-7  Blue Magenta Cyan LightGray   */
    0x555555, 0xff5555, 0x55ff55, 0xffff55,  /* 8-11 DkGray BrRed BrGreen BrYellow */
    0x5555ff, 0xff55ff, 0x55ffff, 0xffffff   /* 12-15 BrBlue BrMagenta BrCyan White */
};

const photon_theme_t photon_themes[] = {
    /* PhotonTERM default: dark charcoal, grey text */
    { "PhotonTERM",
      CGA_LIGHTGRAY, CGA_WHITE, CGA_BLACK, CGA_BLUE, CGA_BLACK, CGA_LIGHTGRAY,
      { 0x262626, 0x3a3a3a, 0x1c3c1c, 0x888888, 0x1a1a3a, 0x3a1a3a,
        0x1a3a3a, 0xb2b2b2, 0x585858, 0xd04040, 0x50c050, 0xd8d060,
        0x4080d0, 0xb060c0, 0x40b0c0, 0xe4e4e4 } },

    /* Amber Terminal: warm phosphor CRT */
    { "Amber Terminal",
      CGA_YELLOW, CGA_YELLOW, CGA_BLACK, CGA_BROWN, CGA_BLACK, CGA_LIGHTGRAY,
      { 0x0f0900, 0x2a1800, 0x1a1400, 0x5a3800, 0x0d0600, 0x3a2000,
        0x7a4a00, 0xd4880a, 0x3a2000, 0xd04000, 0xa06000, 0xf0c050,
        0x503800, 0xd06000, 0xc87c00, 0xffd060 } },

    /* Green Screen: classic phosphor */
    { "Green Screen",
      CGA_YELLOW, CGA_YELLOW, CGA_BLACK, CGA_GREEN, CGA_BLACK, CGA_LIGHTGRAY,
      { 0x001200, 0x001a00, 0x006600, 0x00aa00, 0x000d00, 0x003300,
        0x004400, 0x00cc00, 0x002200, 0x00ee00, 0x44dd44, 0xaaffaa,
        0x003300, 0x00bb00, 0x22ee22, 0x00ff44 } },

    /* Nord: Arctic Polar Night / Snow Storm */
    { "Nord",
      CGA_CYAN, CGA_WHITE, CGA_BLACK, CGA_BLUE, CGA_BLACK, CGA_LIGHTGRAY,
      { 0x2e3440, 0x3b4252, 0x434c5e, 0x88c0d0, 0x4c566a, 0xb48ead,
        0x5e81ac, 0x81a1c1, 0x616e88, 0xbf616a, 0xa3be8c, 0xebcb8b,
        0x5e81ac, 0xb48ead, 0x8fbcbb, 0xeceff4 } },

    /* Ocean: deep teal on dark navy */
    { "Ocean",
      CGA_CYAN, CGA_WHITE, CGA_BLACK, CGA_BLUE, CGA_BLACK, CGA_LIGHTGRAY,
      { 0x0d1b2a, 0x1b3a5c, 0x0a3347, 0x007fa8, 0x0d2840, 0x5c3060,
        0x006080, 0x20c0d8, 0x1a3a50, 0x60b0d0, 0x40e0d0, 0xa0f0e0,
        0x0050a0, 0xa050c0, 0x40c8e0, 0xe0f8ff } },

    /* Dracula: deep purple with neon accents */
    { "Dracula",
      CGA_LIGHTMAGENTA, CGA_WHITE, CGA_BLACK, CGA_BLUE, CGA_BLACK, CGA_LIGHTGRAY,
      { 0x282a36, 0x44475a, 0x1e6045, 0x6272a4, 0x383a4a, 0xbd93f9,
        0x44475a, 0x6272a4, 0x4d4f5e, 0xff5555, 0x50fa7b, 0xf1fa8c,
        0x6272a4, 0xff79c6, 0x8be9fd, 0xf8f8f2 } },

    /* Synthwave: neon 80s on deep purple */
    { "Synthwave",
      CGA_LIGHTMAGENTA, CGA_WHITE, CGA_BLACK, CGA_BLUE, CGA_BLACK, CGA_LIGHTGRAY,
      { 0x1a1033, 0x2d1b69, 0x0d3040, 0x00c8ff, 0x261546, 0xff00ff,
        0x1a2a60, 0x4060c0, 0x3d2080, 0xff4060, 0x00e080, 0xffff40,
        0x4060ff, 0xff40ff, 0x40e0ff, 0xe0d0ff } },

    /* Cyberpunk: neon green on near-black */
    { "Cyberpunk",
      CGA_LIGHTGREEN, CGA_WHITE, CGA_BLACK, CGA_MAGENTA, CGA_BLACK, CGA_LIGHTGRAY,
      { 0x0d0d0d, 0x1a001a, 0x001a00, 0x00e000, 0x000d00, 0xff00aa,
        0x001400, 0x00d000, 0x1a1a1a, 0xff2040, 0x40ff40, 0xffff00,
        0x0060ff, 0xff00ff, 0x00ff80, 0xffffff } },

    /* Slate: pure greyscale */
    { "Slate",
      CGA_WHITE, CGA_LIGHTGRAY, CGA_BLACK, CGA_BLUE, CGA_BLACK, CGA_LIGHTGRAY,
      { 0x1a1a1a, 0x2e2e2e, 0x242424, 0x707070, 0x202020, 0x585858,
        0x3c3c3c, 0xc0c0c0, 0x3a3a3a, 0xb0b0b0, 0xc8c8c8, 0xe0e0e0,
        0x505050, 0x909090, 0xd0d0d0, 0xf0f0f0 } },

    /* Classic: original CGA blue */
    { "Classic",
      CGA_YELLOW, CGA_WHITE, CGA_BLUE, CGA_CYAN, CGA_BLUE, CGA_LIGHTGRAY,
      { 0x000000, 0xaa0000, 0x00aa00, 0xaa5500, 0x0000aa, 0xaa00aa,
        0x00aaaa, 0xaaaaaa, 0x555555, 0xff5555, 0x55ff55, 0xffff55,
        0x5555ff, 0xff55ff, 0x55ffff, 0xffffff } },

    { NULL, 0, 0, 0, 0, 0, 0, {0} }
};

/* ── Theme application ──────────────────────────────────────────────── */

int photon_theme_find(const char *name)
{
    if (!name || !name[0]) return 0;
    for (int i = 0; photon_themes[i].name; i++) {
        if (strcmp(photon_themes[i].name, name) == 0)
            return i;
    }
    return 0;
}

void photon_theme_apply(int idx, struct photon_sdl *sdl,
                        photon_settings_t *s)
{
    if (idx < 0) idx = 0;
    /* Count themes */
    int n = 0;
    while (photon_themes[n].name) n++;
    if (idx >= n) idx = 0;

    photon_active_theme = idx;
    const photon_theme_t *t = &photon_themes[idx];

    /* Push palette to SDL renderer */
    if (sdl) {
        for (int i = 0; i < 16; i++) {
            uint32_t rgb = t->rgb[i] ? t->rgb[i] : ansi_sgr_rgb[i];
            photon_sdl_set_palette(sdl, i,
                                   (rgb >> 16) & 0xff,
                                   (rgb >>  8) & 0xff,
                                   (rgb      ) & 0xff);
        }
    }

    /* Save theme name to settings */
    if (s) strlcpy(s->theme_name, t->name, sizeof(s->theme_name));

    /* Sync the global UI widget colour scheme to match */
    if (photon_ui_global) {
        photon_ui_colors_t c = {
            .border_fg = t->hclr,
            .border_bg = t->bclr,
            .title_fg  = t->lclr,
            .normal_fg = t->lclr,
            .normal_bg = t->bclr,
            .hilite_fg = t->lbclr,
            .hilite_bg = t->lbbclr,
            .input_fg  = t->hclr,
            .input_bg  = t->bclr,
        };
        photon_ui_set_colors(photon_ui_global, &c);
    }
}

void photon_sdl_reset_to_ansi_palette(struct photon_sdl *sdl)
{
    if (!sdl) return;
    for (int i = 0; i < 16; i++) {
        uint32_t rgb = ansi_sgr_rgb[i];
        photon_sdl_set_palette(sdl, i,
                               (rgb >> 16) & 0xff,
                               (rgb >>  8) & 0xff,
                               (rgb      ) & 0xff);
    }
}

/* Deprecated alias. */
void photon_sdl_reset_to_cga_palette(struct photon_sdl *sdl)
{
    photon_sdl_reset_to_ansi_palette(sdl);
}

void photon_sdl_apply_palette_mode(struct photon_sdl *sdl,
                                   photon_palette_mode_t mode,
                                   photon_conn_type_t    conn_type)
{
    if (!sdl) return;
    bool use_xterm;
    if (mode == PHOTON_PALETTE_AUTO)
        use_xterm = (conn_type == PHOTON_CONN_SHELL);
    else
        use_xterm = (mode == PHOTON_PALETTE_XTERM);

    if (use_xterm)
        photon_sdl_load_xterm_palette(sdl);
    else
        photon_sdl_load_cga_palette(sdl);
}

/* ── Settings defaults ──────────────────────────────────────────────── */

void photon_settings_defaults(photon_settings_t *s)
{
    memset(s, 0, sizeof(*s));
    s->cols         = 80;
    s->rows         = 25;
    s->font_height  = 0;  /* auto */
    s->font_mode    = PHOTON_FONT_TTF;
    s->ttf_path[0]  = 0;  /* bundled */
    s->ttf_size_pt  = 12;
    s->invert_scroll= false;
    s->debug        = false;
    s->bell_enabled = true;
    s->default_palette_mode = PHOTON_PALETTE_AUTO;
    strlcpy(s->theme_name, "PhotonTERM", sizeof(s->theme_name));
}

static void strip_nl(char *p) {
    char *e = p + strlen(p);
    while (e > p && (e[-1] == '\n' || e[-1] == '\r')) *--e = 0;
}

bool photon_settings_load(photon_settings_t *s)
{
    photon_settings_defaults(s);

    char path[PATH_MAX];
    snprintf(path, sizeof(path), "%s/settings.ini", photon_store_config_dir());
    FILE *f = fopen(path, "r");
    if (!f) return false;

    char line[1024];
    while (fgets(line, sizeof(line), f)) {
        strip_nl(line);
        if (!line[0] || line[0] == ';' || line[0] == '#' || line[0] == '[') continue;
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = 0;
        const char *key = line;
        const char *val = eq + 1;

        if      (strcmp(key, "cols")         == 0) s->cols          = atoi(val);
        else if (strcmp(key, "rows")         == 0) s->rows          = atoi(val);
        else if (strcmp(key, "font_height")  == 0) s->font_height   = atoi(val);
        else if (strcmp(key, "font_mode")    == 0) s->font_mode     = (photon_font_mode_t)atoi(val);
        else if (strcmp(key, "ttf_path")     == 0) strlcpy(s->ttf_path, val, sizeof(s->ttf_path));
        else if (strcmp(key, "ttf_size_pt")  == 0) s->ttf_size_pt   = atoi(val);
        else if (strcmp(key, "default_palette_mode") == 0)
            s->default_palette_mode = (photon_palette_mode_t)atoi(val);
        else if (strcmp(key, "invert_scroll")== 0) s->invert_scroll = atoi(val) != 0;
        else if (strcmp(key, "debug")        == 0) s->debug         = atoi(val) != 0;
        else if (strcmp(key, "bell_enabled") == 0) s->bell_enabled  = atoi(val) != 0;
        else if (strcmp(key, "theme")        == 0) strlcpy(s->theme_name, val, sizeof(s->theme_name));
    }
    fclose(f);
    return true;
}

bool photon_settings_save(const photon_settings_t *s)
{
    char path[PATH_MAX];
    snprintf(path, sizeof(path), "%s/settings.ini", photon_store_config_dir());

    char tmp[PATH_MAX];
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    FILE *f = fopen(tmp, "w");
    if (!f) return false;

    fprintf(f, "; PhotonTERM settings\n\n[photonterm]\n");
    fprintf(f, "cols=%d\n",         s->cols);
    fprintf(f, "rows=%d\n",         s->rows);
    fprintf(f, "font_height=%d\n",  s->font_height);
    fprintf(f, "font_mode=%d\n",    (int)s->font_mode);
    fprintf(f, "ttf_path=%s\n",     s->ttf_path);
    fprintf(f, "ttf_size_pt=%d\n",  s->ttf_size_pt);
    fprintf(f, "default_palette_mode=%d\n", (int)s->default_palette_mode);
    fprintf(f, "invert_scroll=%d\n",(int)s->invert_scroll);
    fprintf(f, "debug=%d\n",        (int)s->debug);
    fprintf(f, "bell_enabled=%d\n", (int)s->bell_enabled);
    fprintf(f, "theme=%s\n",        s->theme_name);
    fclose(f);

    if (rename(tmp, path) != 0) { remove(tmp); return false; }
    return true;
}
