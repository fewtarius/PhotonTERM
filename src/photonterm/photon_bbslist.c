/* photon_bbslist.c - PhotonTERM dialing directory
 *
 * Copyright (C) 2026 fewtarius and PhotonTERM contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Reproduces the original screen.c / bbslist.c UI:
 *   - Splash screen with centred version block and key hints
 *   - Near-full-screen directory box (CP437 double-line border with
 *     "PhotonTERM Directory" title embedded in the top edge)
 *   - 3-column layout: Name / Type / Address (same as original)
 *   - Scrollbar when list exceeds visible rows
 *   - Settings overlay (50-col centred, Tab key)
 *   - Theme-aware colours throughout
 *
 * All drawing uses photon_sdl_draw_cell() / photon_sdl_present().
 */

#include "photon_compat.h"
#include "photon_ui.h"
#include "photon_menu.h"

/* exported from photon_main.c - updated when settings change */
extern bool g_bell_enabled;
#include "photon_bbslist.h"
#include "photon_bbs.h"
#include "photon_store.h"
#include "photon_settings.h"
#include "photon_sdl.h"
#include "photon_vte.h"

#define PHOTON_DEBUG_BUILD
#include "photon_debug.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdbool.h>
#include <ctype.h>
#include <time.h>

/* ── External version string ────────────────────────────────────────── */
extern const char *photonterm_version;

/* ── CP437 box-drawing codepoints ───────────────────────────────────── */
#define BOX_TL  0x2554u   /* ╔ double-line */
#define BOX_TR  0x2557u   /* ╗ */
#define BOX_BL  0x255Au   /* ╚ */
#define BOX_BR  0x255Du   /* ╝ */
#define BOX_H   0x2550u   /* ═ */
#define BOX_V   0x2551u   /* ║ */
#define BOX_ML  0x2560u   /* ╠ */
#define BOX_MR  0x2563u   /* ╣ */

/* Block chars for scrollbar */
#define BLK_FULL  0x2588u /* █ */
#define BLK_LIGHT 0x2591u /* ░ */

/* Cursor indicator */
#define ARROW_RIGHT 0x25BAu /* ► */

/* ── Theme colour helpers ────────────────────────────────────────────── */
/*
 * Colours are derived from the currently active theme (photon_active_theme).
 * We follow the same uifc field semantics as the original:
 *   t->hclr   = border/frame fg
 *   t->lclr   = normal text fg
 *   t->bclr   = window bg
 *   t->cclr   = inactive / column-header bg
 *   t->lbclr  = selection bar fg
 *   t->lbbclr = selection bar bg
 */
static const photon_theme_t *active_theme(void)
{
    int idx = photon_active_theme;
    if (idx < 0) idx = 0;
    /* bounds check */
    int n = 0; while (photon_themes[n].name) n++;
    if (idx >= n) idx = 0;
    return &photon_themes[idx];
}

/* Compose fg+bg into an attribute byte (high nibble = bg, low = fg) */
#define ATTR(fg, bg) ((uint8_t)(((bg) << 4) | ((fg) & 0x0f)))

/* Frequently used attribute combos */
#define A_BOX(t)    ATTR((t)->hclr,    (t)->bclr)      /* border             */
#define A_TITLE(t)  ATTR(CGA_BLACK,    (t)->hclr)      /* title in border    */
#define A_NORM(t)   ATTR((t)->lclr,    (t)->bclr)      /* normal text        */
#define A_HDR(t)    ATTR((t)->lclr,    (t)->cclr)      /* column header row  */
#define A_DIM(t)    ATTR((t)->cclr,    (t)->bclr)      /* dim/type text      */
#define A_SEL(t)    ATTR((t)->lbclr,   (t)->lbbclr)    /* selection bar      */
#define A_SEL_HI(t) ATTR((t)->hclr,    (t)->lbbclr)    /* bright on sel bar  */
#define A_HINT(t)   ATTR((t)->lclr,    (t)->bclr)      /* hint row           */
#define A_HINT_K(t) ATTR((t)->hclr,    (t)->bclr)      /* key name in hints  */
#define A_ACCENT(t) ATTR(CGA_BLACK,    (t)->hclr)      /* splash accent bars */
#define A_STATUS(t) ATTR((t)->lclr,    (t)->bclr)      /* status bar         */

/* ── Low-level cell drawing ─────────────────────────────────────────── */

static void put_cell(photon_ui_t *ui, int col, int row,
                     uint32_t cp, uint8_t attr)
{
    vte_cell_t cell = {
        .codepoint = cp,
        .fg = attr & 0x0f,
        .bg = (attr >> 4) & 0x0f,
        .attr = 0
    };
    photon_sdl_draw_cell(photon_ui_sdl(ui), col, row, &cell);
}

/* Write a NUL-terminated UTF-8 string starting at (col, row) */
static int put_str(photon_ui_t *ui, int col, int row,
                   const char *s, uint8_t attr)
{
    int c = col;
    const unsigned char *p = (const unsigned char *)s;
    while (*p) {
        uint32_t cp;
        if      (*p < 0x80) { cp = *p++; }
        else if ((*p & 0xe0) == 0xc0) {
            cp = (uint32_t)(*p++ & 0x1f) << 6;
            if (*p) cp |= (*p++ & 0x3f);
        } else if ((*p & 0xf0) == 0xe0) {
            cp = (uint32_t)(*p++ & 0x0f) << 12;
            if (*p) cp |= (uint32_t)(*p++ & 0x3f) << 6;
            if (*p) cp |= (*p++ & 0x3f);
        } else { cp = '?'; p++; }
        put_cell(ui, c++, row, cp, attr);
    }
    return c - col;  /* chars written */
}

/* Fill a rectangle with spaces */
static void fill_rect(photon_ui_t *ui,
                      int c1, int r1, int c2, int r2, uint8_t attr)
{
    for (int r = r1; r <= r2; r++)
        for (int c = c1; c <= c2; c++)
            put_cell(ui, c, r, ' ', attr);
}

/* Write a string padded/clipped to exactly `width` columns */
/* ── Connection type short label ─────────────────────────────────────── */

static const char *conn_type_label(photon_conn_type_t t)
{
    switch (t) {
        case PHOTON_CONN_SSH:    return "SSH";
        case PHOTON_CONN_TELNET: return "TEL";
        case PHOTON_CONN_SHELL:  return "PTY";
        default:                 return "   ";
    }
}

/* ── Column widths (same semantics as original DIR_COL_*) ───────────── */
/* ► + space = 2, type = 3, type_sep = 1 */
#define DIR_COL_CUR   2   /* ► + space */
#define DIR_COL_TYPE  3   /* "SSH" */
#define DIR_COL_SEP   1   /* space after name */

/* ── Terminal size helpers ──────────────────────────────────────────── */
static int tcols(photon_ui_t *ui) { return vte_cols(photon_ui_vte(ui)); }
static int trows(photon_ui_t *ui) { return vte_rows(photon_ui_vte(ui)); }

/* ── Status bar ─────────────────────────────────────────────────────── */
static void draw_statusbar(photon_ui_t *ui, bool connected, const char *bbs_name)
{
    const photon_theme_t *t = active_theme();
    int W = tcols(ui);
    int H = trows(ui);
    uint8_t a = A_STATUS(t);

    /* Clear row */
    fill_rect(ui, 1, H, W, H, a);

    if (connected && bbs_name && bbs_name[0]) {
        char rbuf[64];
        snprintf(rbuf, sizeof(rbuf), " Connected: %.40s ", bbs_name);
        int rlen = (int)strlen(rbuf);
        int col  = W - rlen + 1;
        if (col < 1) col = 1;
        put_str(ui, col, H, rbuf, ATTR((t)->hclr, (t)->bclr));
    }
}

/* Show BBS entry info in the status bar (last connected, call count, comment) */
static void draw_entry_statusbar(photon_ui_t *ui, const photon_bbs_t *bbs)
{
    const photon_theme_t *t = active_theme();
    int W = tcols(ui);
    int H = trows(ui);
    uint8_t a  = A_STATUS(t);
    uint8_t ah = ATTR((t)->hclr, (t)->bclr);

    fill_rect(ui, 1, H, W, H, a);

    /* Left: comment (if any) */
    if (bbs->comment[0]) {
        char lbuf[128];
        snprintf(lbuf, sizeof(lbuf), " %s", bbs->comment);
        put_str(ui, 1, H, lbuf, a);
    }

    /* Right: calls + last connected */
    char rbuf[64];
    if (bbs->last_connected) {
        struct tm *tm = localtime(&bbs->last_connected);
        char tstr[24];
        strftime(tstr, sizeof(tstr), "%Y-%m-%d", tm);
        snprintf(rbuf, sizeof(rbuf), " Calls: %u  Last: %s ", bbs->calls, tstr);
    } else {
        snprintf(rbuf, sizeof(rbuf), " Never connected ");
    }
    int rlen = (int)strlen(rbuf);
    int col  = W - rlen + 1;
    if (col < 1) col = 1;
    put_str(ui, col, H, rbuf, ah);
}

/* ── Splash screen ──────────────────────────────────────────────────── */
static void draw_splash(photon_ui_t *ui)
{
    const photon_theme_t *t = active_theme();
    int W = tcols(ui);
    int H = trows(ui);
    int mid = W / 2;

    /* Clean version (strip debug suffix) */
    char verbuf[64];
    const char *p = photonterm_version;
    const char *cut = strstr(p, " (");
    if (!cut) cut = strstr(p, " Debug");
    if (cut) {
        size_t n = (size_t)(cut - p);
        if (n >= sizeof(verbuf)) n = sizeof(verbuf) - 1;
        memcpy(verbuf, p, n); verbuf[n] = '\0';
    } else {
        strlcpy(verbuf, p, sizeof(verbuf));
    }
    int vlen = (int)strlen(verbuf);

    static const char *sub  = "Retro BBS Terminal";
    int slen = (int)strlen(sub);

    uint8_t bg_attr  = A_NORM(t);
    uint8_t acc_attr = A_ACCENT(t);

    /* Clear usable area (all except last row = status bar) */
    fill_rect(ui, 1, 1, W, H - 1, bg_attr);

    /* Vertically centre the logo block (4 rows tall) */
    int logo_row = (H - 1 - 4) / 2;
    if (logo_row < 2) logo_row = 2;

    /* Accent bars */
    fill_rect(ui, 1, logo_row,     W, logo_row,     acc_attr);
    fill_rect(ui, 1, logo_row + 3, W, logo_row + 3, acc_attr);

    /* Version line */
    fill_rect(ui, 1, logo_row + 1, W, logo_row + 1, bg_attr);
    put_str(ui, mid - vlen / 2, logo_row + 1, verbuf, ATTR((t)->hclr, (t)->bclr));

    /* Subtitle */
    fill_rect(ui, 1, logo_row + 2, W, logo_row + 2, bg_attr);
    put_str(ui, mid - slen / 2, logo_row + 2, sub, A_DIM(t));

    /* Key hints */
    int hint_row = H - 3;
    if (hint_row < logo_row + 5) hint_row = logo_row + 5;
    if (hint_row >= H)           hint_row = H - 1;

    static const char *k1 = "D", *d1 = " Directory  ";
    static const char *k2 = "Tab", *d2 = " Settings  ";
    static const char *k3 = "Q", *d3 = " Quit";
    int hint_len = (int)(strlen(k1)+strlen(d1)+strlen(k2)+strlen(d2)+strlen(k3)+strlen(d3));
    int col = mid - hint_len / 2;
    if (col < 1) col = 1;

    fill_rect(ui, 1, hint_row, W, hint_row, bg_attr);
    col += put_str(ui, col, hint_row, k1, A_HINT_K(t));
    col += put_str(ui, col, hint_row, d1, A_HINT(t));
    col += put_str(ui, col, hint_row, k2, A_HINT_K(t));
    col += put_str(ui, col, hint_row, d2, A_HINT(t));
    col += put_str(ui, col, hint_row, k3, A_HINT_K(t));
    col += put_str(ui, col, hint_row, d3, A_HINT(t));
    (void)col;

    draw_statusbar(ui, false, NULL);
}

/* ── Directory browser drawing ──────────────────────────────────────── */

/* ── Directory sort comparators ─────────────────────────────────────── */

static int cmp_bbs_name(const void *a, const void *b)
{
    return strcasecmp(((const photon_bbs_t *)a)->name,
                      ((const photon_bbs_t *)b)->name);
}

static int cmp_bbs_last(const void *a, const void *b)
{
    time_t ta = ((const photon_bbs_t *)a)->last_connected;
    time_t tb = ((const photon_bbs_t *)b)->last_connected;
    return (ta < tb) - (ta > tb);  /* descending: most-recent first */
}

static int cmp_bbs_calls(const void *a, const void *b)
{
    unsigned ca = ((const photon_bbs_t *)a)->calls;
    unsigned cb = ((const photon_bbs_t *)b)->calls;
    return (ca < cb) - (ca > cb);  /* descending: most-called first */
}

static void apply_sort(photon_bbs_t *list, int count, int mode)
{
    switch (mode) {
        case 1: qsort(list, (size_t)count, sizeof(*list), cmp_bbs_name);  break;
        case 2: qsort(list, (size_t)count, sizeof(*list), cmp_bbs_last);  break;
        case 3: qsort(list, (size_t)count, sizeof(*list), cmp_bbs_calls); break;
        default: break;  /* manual order - no sort */
    }
}

static const char *sort_mode_name(int mode)
{
    switch (mode) {
        case 1: return " [Sort: Name]";
        case 2: return " [Sort: Recent]";
        case 3: return " [Sort: Calls]";
        default: return "";
    }
}

/* ── Directory list item renderer (for photon_menu_list_t) ──────────── */

static void dir_draw_item(photon_ui_t *ui,
                          void *items, int index,
                          int row, int col_start, int col_end,
                          bool selected,
                          const photon_theme_t *t)
{
    const photon_bbs_t *list = (const photon_bbs_t *)items;
    const photon_bbs_t *bbs = &list[index];

    int eff_w = col_end - col_start + 1;
    int addr_w = 28;
    int name_w = eff_w - DIR_COL_CUR - DIR_COL_SEP - DIR_COL_TYPE - 1 - addr_w;
    if (name_w < 4) name_w = 4;

    uint8_t a_norm, a_hi, a_dim;
    if (selected) {
        a_norm = PHOTON_MENU_A_SEL(t);
        a_hi   = PHOTON_MENU_A_SEL_HI(t);
        a_dim  = PHOTON_MENU_A_SEL(t);
    } else {
        a_norm = PHOTON_MENU_A_NORM(t);
        a_hi   = PHOTON_MENU_ATTR(t->hclr, t->bclr);
        a_dim  = PHOTON_MENU_A_DIM(t);
    }

    int xi = col_start;

    /* Clear row */
    photon_menu_fill_rect(ui, xi, row, xi + eff_w - 1, row, a_norm);

    /* Cursor indicator */
    photon_menu_put_cell(ui, xi,     row, selected ? ARROW_RIGHT : ' ', a_hi);
    photon_menu_put_cell(ui, xi + 1, row, ' ', a_norm);

    /* Name */
    photon_menu_put_padded(ui, xi + DIR_COL_CUR, row, bbs->name, name_w, a_norm);

    /* Space + Type */
    photon_menu_put_cell(ui, xi + DIR_COL_CUR + name_w, row, ' ', a_dim);
    photon_menu_put_padded(ui, xi + DIR_COL_CUR + name_w + DIR_COL_SEP, row,
                           conn_type_label(bbs->conn_type), DIR_COL_TYPE, a_dim);

    /* Space + Address */
    photon_menu_put_cell(ui, xi + DIR_COL_CUR + name_w + DIR_COL_SEP + DIR_COL_TYPE, row, ' ', a_dim);
    const char *addr_display = bbs->addr[0] ? bbs->addr
                               : (bbs->conn_type == PHOTON_CONN_SHELL ? "$SHELL" : "");
    photon_menu_put_padded(ui, xi + DIR_COL_CUR + name_w + DIR_COL_SEP + DIR_COL_TYPE + 1, row,
                           addr_display, addr_w, a_dim);
}

/* Directory column headers */
static void dir_draw_header(photon_ui_t *ui, int row, int width,
                            const photon_theme_t *t)
{
    uint8_t hdr_a = PHOTON_MENU_A_HDR(t);
    photon_menu_fill_rect(ui, 1, row, width, row, hdr_a);

    int addr_w = 28;
    int name_w = width - DIR_COL_CUR - DIR_COL_SEP - DIR_COL_TYPE - 1 - addr_w;
    if (name_w < 4) name_w = 4;

    photon_menu_put_padded(ui, 1 + DIR_COL_CUR,                      row, "Name",    name_w,      hdr_a);
    photon_menu_put_cell  (ui, 1 + DIR_COL_CUR + name_w,             row, ' ',       hdr_a);
    photon_menu_put_padded(ui, 1 + DIR_COL_CUR + name_w + DIR_COL_SEP, row, "Type", DIR_COL_TYPE, hdr_a);
    photon_menu_put_cell  (ui, 1 + DIR_COL_CUR + name_w + DIR_COL_SEP + DIR_COL_TYPE, row, ' ', hdr_a);
    photon_menu_put_padded(ui, 1 + DIR_COL_CUR + name_w + DIR_COL_SEP + DIR_COL_TYPE + 1, row, "Address", addr_w, hdr_a);
}

/* ── Settings menus (using photon_ui_list for flicker-free overlay) ──── */

/* Run the theme picker */
static void run_theme_picker(photon_ui_t *ui, photon_settings_t *s)
{
    int n = 0;
    while (photon_themes[n].name) n++;
    const char **opts = malloc((size_t)(n + 1) * sizeof(char *));
    if (!opts) return;
    for (int i = 0; i < n; i++) opts[i] = photon_themes[i].name;
    opts[n] = NULL;

    int cur = photon_active_theme;
    int sel = photon_ui_list(ui, "Theme", opts, n, &cur);
    free(opts);
    if (sel >= 0 && sel < n) {
        photon_theme_apply(sel, photon_ui_sdl(ui), s);
        photon_settings_save(s);
    }
}

/* Run a font-size picker: returns new pt size or 0 if cancelled */
static void run_font_size(photon_ui_t *ui, photon_settings_t *s)
{
    const char *opts[] = {
        "12pt  (small)", "14pt", "16pt  (default)", "18pt",
        "20pt", "24pt  (large)", "28pt", "32pt  (xlarge)", NULL
    };
    static const int pts[] = { 12, 14, 16, 18, 20, 24, 28, 32 };
    int n = 8;
    int cur = 2; /* 16pt default */
    for (int i = 0; i < n; i++) {
        if (pts[i] == s->ttf_size_pt) { cur = i; break; }
    }
    int sel = photon_ui_list(ui, "Font Size", opts, n, &cur);
    if (sel >= 0 && sel < n) {
        s->ttf_size_pt = pts[sel];
        photon_settings_save(s);
        photon_ui_msg(ui, "Font size change will apply on next launch.");
    }
}

/* Run the terminal mode picker (CP437 / UTF-8) */
static void run_terminal_mode(photon_ui_t *ui, photon_settings_t *s)
{
    const char *opts[] = {
        "CP437  (BBS ANSI art - classic mode)",
        "UTF-8  (Unicode, modern mode)",
        NULL
    };
    int cur = (s->font_mode == PHOTON_FONT_TTF) ? 1 : 0;
    int sel = photon_ui_list(ui, "Terminal Mode", opts, 2, &cur);
    if (sel == 0) {
        s->font_mode = PHOTON_FONT_BITMAP;
        photon_settings_save(s);
    } else if (sel == 1) {
        s->font_mode = PHOTON_FONT_TTF;
        photon_settings_save(s);
    }
}

/* Run the terminal size picker */
static void run_terminal_size(photon_ui_t *ui, photon_settings_t *s)
{
    const char *opts[] = {
        "80 x 24  (classic BBS)",
        "80 x 25  (standard)",
        "80 x 40  (tall)",
        "100 x 35",
        "120 x 35",
        "132 x 37  (wide)",
        NULL
    };
    static const int col_tab[] = { 80,  80,  80, 100, 120, 132 };
    static const int row_tab[] = { 24,  25,  40,  35,  35,  37 };
    int n = 6;
    int cur = 1; /* 80x25 default */
    for (int i = 0; i < n; i++) {
        if (col_tab[i] == s->cols && row_tab[i] == s->rows) { cur = i; break; }
    }
    int sel = photon_ui_list(ui, "Terminal Size", opts, n, &cur);
    if (sel >= 0 && sel < n) {
        s->cols = col_tab[sel];
        s->rows = row_tab[sel];
        photon_settings_save(s);
        photon_ui_msg(ui, "Terminal size change applies on next connection.");
    }
}

/*
 * Run the full settings menu using photon_menu_form_t.
 * Full-screen form with ACTION fields for sub-pickers.
 */
static void run_settings(photon_ui_t *ui, photon_settings_t *s)
{
    static const photon_hint_t hints[] = {
        { "Return", " Change  " },
        { "Space",  " Toggle  " },
        { "Esc",    " Back" },
    };

    /* Current value labels (refreshed each loop) */
    char theme_val[64];
    char mode_val[64];
    char font_val[64];
    char term_val[64];

    photon_menu_form_state_t state = { .field = 0 };

    for (;;) {
        /* Refresh display values */
        strlcpy(theme_val, photon_themes[photon_active_theme].name, sizeof(theme_val));

        if (s->font_mode == PHOTON_FONT_BITMAP)
            strlcpy(mode_val, "CP437 (BBS art)", sizeof(mode_val));
        else
            strlcpy(mode_val, "UTF-8 (Unicode)", sizeof(mode_val));

        snprintf(font_val, sizeof(font_val), "%dpt", s->ttf_size_pt);
        snprintf(term_val, sizeof(term_val), "%d x %d", s->cols, s->rows);

        photon_menu_field_t fields[] = {
            { "Theme",         PHOTON_MENU_FIELD_ACTION, theme_val, sizeof(theme_val), NULL, NULL, NULL },
            { "Terminal Mode", PHOTON_MENU_FIELD_ACTION, mode_val,  sizeof(mode_val),  NULL, NULL, NULL },
            { "Font Size",     PHOTON_MENU_FIELD_ACTION, font_val,  sizeof(font_val),  NULL, NULL, NULL },
            { "Terminal Size", PHOTON_MENU_FIELD_ACTION, term_val,  sizeof(term_val),  NULL, NULL, NULL },
            { "Visual Bell",   PHOTON_MENU_FIELD_TOGGLE, NULL, 0, &s->bell_enabled, NULL, NULL },
            { NULL,            PHOTON_MENU_FIELD_SEPARATOR, NULL, 0, NULL, NULL, NULL },
            { NULL,            PHOTON_MENU_FIELD_BUTTON, NULL, 0, NULL, "[ About PhotonTERM ]", NULL },
        };
        int nfields = sizeof(fields) / sizeof(fields[0]);

        photon_menu_form_t form = {
            .title   = "Settings",
            .fields  = fields,
            .nfields = nfields,
            .hints   = hints,
            .nhints  = sizeof(hints) / sizeof(hints[0]),
        };

        photon_menu_draw_form(ui, &form, &state);
        photon_sdl_present(photon_ui_sdl(ui));

        photon_key_t key = {0};
        if (!photon_sdl_wait_key(photon_ui_sdl(ui), &key, 100)) continue;
        if (key.code == 0) continue;

        /* Escape -> back */
        if (key.code == '\x1b' || key.code == PHOTON_KEY_QUIT) break;

        /* Form navigation (arrows, tab, space for toggle) */
        if (photon_menu_form_handle_nav(&key, &state, &form)) {
            /* Toggle: also persist bell change immediately */
            g_bell_enabled = s->bell_enabled;
            photon_settings_save(s);
            continue;
        }

        /* Enter on a field -> action */
        if (key.code == '\r') {
            int fi = photon_menu_form_focus_to_field(&form, state.field);
            if (fi < 0) continue;
            switch (fi) {
            case 0: run_theme_picker(ui, s);    break;
            case 1: run_terminal_mode(ui, s);   break;
            case 2: run_font_size(ui, s);       break;
            case 3: run_terminal_size(ui, s);   break;
            case 4: /* Toggle on Enter too */
                s->bell_enabled = !s->bell_enabled;
                g_bell_enabled  = s->bell_enabled;
                photon_settings_save(s);
                break;
            case 6: { /* About */
                char about[512];
                snprintf(about, sizeof(about),
                         "%s\n\n"
                         "Copyright (C) 2026 fewtarius and contributors\n"
                         "Licensed under the GNU GPL v3 or later\n\n"
                         "A modern BBS terminal client built on SDL2.\n"
                         "Supports ANSI/VT100, Telnet, SSH, and CP437\n"
                         "art with optional Unicode/UTF-8 mode.",
                         photonterm_version);
                photon_ui_showbuf(ui, "About PhotonTERM", about, 60, 12);
                break;
            }
            default: break;
            }
        }
    }
}

/* ── Edit dialog ─────────────────────────────────────────────────────── */

#define EDIT_FIELD_NAME    0
#define EDIT_FIELD_ADDR    1
#define EDIT_FIELD_TYPE    2
#define EDIT_FIELD_PORT    3
#define EDIT_FIELD_USER    4
#define EDIT_FIELD_PASS    5
#define EDIT_FIELD_TMODE   6
#define EDIT_FIELD_COMMENT 7
#define EDIT_FIELD_SAVE    8
#define EDIT_FIELD_CANCEL  9
#define EDIT_FIELD_COUNT   10
#define EDIT_FIELD_PMODE   10
#undef  EDIT_FIELD_COMMENT
#define EDIT_FIELD_COMMENT 11
#undef  EDIT_FIELD_SAVE
#define EDIT_FIELD_SAVE    12
#undef  EDIT_FIELD_CANCEL
#define EDIT_FIELD_CANCEL  13
#undef  EDIT_FIELD_COUNT
#define EDIT_FIELD_COUNT   14

/* Build a packed items array for the edit dialog, skipping fields that
 * don't apply to the current connection type.  field_map[i] gives the
 * EDIT_FIELD_* id for packed slot i.  Returns the number of visible items. */
static int build_edit_menu(const photon_bbs_t *b, char rows[][72],
                            const char **items, int *field_map)
{
    static const char *conn_names[]   = { "Telnet", "SSH", "Shell" };
    static const char *tmode_names[]  = { "Auto", "CP437 (BBS art)", "UTF-8 (Unicode)" };
    static const char *pmode_names[]  = { "Auto", "CGA 16-color", "xterm 256-color" };
    int ct = (int)b->conn_type;
    if (ct < 0 || ct > 2) ct = 0;
    int tm = (int)b->term_mode;
    if (tm < 0 || tm > 2) tm = 0;
    int pm = (int)b->palette_mode;
    if (pm < 0 || pm > 2) pm = 0;

    bool is_shell = (b->conn_type == PHOTON_CONN_SHELL);
    bool is_ssh   = (b->conn_type == PHOTON_CONN_SSH);

    snprintf(rows[EDIT_FIELD_NAME],    72, "Name         : %s", b->name);
    if (is_shell)
        snprintf(rows[EDIT_FIELD_ADDR], 72, "Command      : %s",
                 b->addr[0] ? b->addr : "(default shell)");
    else
        snprintf(rows[EDIT_FIELD_ADDR], 72, "Address      : %s", b->addr);
    snprintf(rows[EDIT_FIELD_TYPE],    72, "Type         : %s", conn_names[ct]);
    snprintf(rows[EDIT_FIELD_PORT],    72, "Port         : %u", (unsigned)b->port);
    snprintf(rows[EDIT_FIELD_USER],    72, "Username     : %s", b->user);
    snprintf(rows[EDIT_FIELD_PASS],    72, "Password     : %s", b->pass[0] ? "********" : "");
    snprintf(rows[EDIT_FIELD_TMODE],   72, "Terminal Mode: %s", tmode_names[tm]);
    snprintf(rows[EDIT_FIELD_PMODE],   72, "Palette      : %s", pmode_names[pm]);
    snprintf(rows[EDIT_FIELD_COMMENT], 72, "Comment      : %s", b->comment);
    snprintf(rows[EDIT_FIELD_SAVE],    72, "[ Save ]");
    snprintf(rows[EDIT_FIELD_CANCEL],  72, "[ Cancel ]");

    int n = 0;
    field_map[n] = EDIT_FIELD_NAME;    items[n++] = rows[EDIT_FIELD_NAME];
    field_map[n] = EDIT_FIELD_ADDR;    items[n++] = rows[EDIT_FIELD_ADDR];
    field_map[n] = EDIT_FIELD_TYPE;    items[n++] = rows[EDIT_FIELD_TYPE];
    if (!is_shell) {
        field_map[n] = EDIT_FIELD_PORT; items[n++] = rows[EDIT_FIELD_PORT];
    }
    if (is_ssh) {
        field_map[n] = EDIT_FIELD_USER; items[n++] = rows[EDIT_FIELD_USER];
        field_map[n] = EDIT_FIELD_PASS; items[n++] = rows[EDIT_FIELD_PASS];
    }
    field_map[n] = EDIT_FIELD_TMODE;   items[n++] = rows[EDIT_FIELD_TMODE];
    field_map[n] = EDIT_FIELD_PMODE;   items[n++] = rows[EDIT_FIELD_PMODE];
    field_map[n] = EDIT_FIELD_COMMENT; items[n++] = rows[EDIT_FIELD_COMMENT];
    field_map[n] = EDIT_FIELD_SAVE;    items[n++] = rows[EDIT_FIELD_SAVE];
    field_map[n] = EDIT_FIELD_CANCEL;  items[n++] = rows[EDIT_FIELD_CANCEL];
    items[n] = NULL;
    return n;
}

static bool photon_bbs_edit(photon_ui_t *ui, photon_bbs_t *bbs, bool is_new)
{
    static const char *conn_names[] = { "Telnet", "SSH", "Shell" };
    const int n_types = 3;

    photon_bbs_t work = *bbs;

    if (is_new && work.port == 0)
        work.port = photon_bbs_default_port(work.conn_type);

    const char *title = is_new ? "New Entry" : "Edit Entry";
    char rows[EDIT_FIELD_COUNT][72];
    const char *items[EDIT_FIELD_COUNT + 1];
    int field_map[EDIT_FIELD_COUNT];
    int sel = 0;

    for (;;) {
        int nvis = build_edit_menu(&work, rows, items, field_map);
        int choice = photon_ui_list(ui, title, items, nvis, &sel);

        if (choice < 0) return false;
        int fid = (choice < nvis) ? field_map[choice] : -1;
        if (fid == EDIT_FIELD_CANCEL) return false;

        switch (fid) {
        case EDIT_FIELD_NAME: {
            char tmp[PHOTON_BBS_NAME_MAX + 1];
            SAFECOPY(tmp, work.name);
            int r = photon_ui_input(ui, "Name", tmp, sizeof(tmp),
                                    PHOTON_INPUT_EDIT | PHOTON_INPUT_TRIM);
            if (r >= 0) SAFECOPY(work.name, tmp);
            break;
        }
        case EDIT_FIELD_ADDR: {
            char tmp[PHOTON_BBS_ADDR_MAX + 1];
            SAFECOPY(tmp, work.addr);
            const char *label = (work.conn_type == PHOTON_CONN_SHELL)
                                 ? "Command (empty = $SHELL)" : "Address";
            int r = photon_ui_input(ui, label, tmp, sizeof(tmp),
                                    PHOTON_INPUT_EDIT | PHOTON_INPUT_TRIM);
            if (r >= 0) SAFECOPY(work.addr, tmp);
            break;
        }
        case EDIT_FIELD_TYPE: {
            int tp = (int)work.conn_type;
            if (tp < 0 || tp >= n_types) tp = 0;
            int prev = tp;
            int r = photon_ui_list(ui, "Connection Type", conn_names, n_types, &tp);
            if (r >= 0) {
                work.conn_type = (photon_conn_type_t)r;
                uint16_t old_def = photon_bbs_default_port((photon_conn_type_t)prev);
                if (work.port == old_def || work.port == 0)
                    work.port = photon_bbs_default_port(work.conn_type);
            }
            break;
        }
        case EDIT_FIELD_PORT: {
            char tmp[8];
            snprintf(tmp, sizeof(tmp), "%u", (unsigned)work.port);
            int r = photon_ui_input(ui, "TCP Port", tmp, sizeof(tmp),
                                    PHOTON_INPUT_EDIT | PHOTON_INPUT_NUMBER);
            if (r >= 0) {
                int pp = atoi(tmp);
                if (pp < 1 || pp > 65535)
                    pp = (int)photon_bbs_default_port(work.conn_type);
                work.port = (uint16_t)pp;
            }
            break;
        }
        case EDIT_FIELD_USER: {
            char tmp[PHOTON_BBS_USER_MAX + 1];
            SAFECOPY(tmp, work.user);
            int r = photon_ui_input(ui, "Username", tmp, sizeof(tmp),
                                    PHOTON_INPUT_EDIT | PHOTON_INPUT_TRIM);
            if (r >= 0) SAFECOPY(work.user, tmp);
            break;
        }
        case EDIT_FIELD_PASS: {
            char tmp[PHOTON_BBS_PASS_MAX + 1];
            SAFECOPY(tmp, work.pass);
            int r = photon_ui_input(ui, "Password", tmp, sizeof(tmp),
                                    PHOTON_INPUT_EDIT | PHOTON_INPUT_PASSWORD);
            if (r >= 0) SAFECOPY(work.pass, tmp);
            break;
        }
        case EDIT_FIELD_TMODE: {
            static const char *tmode_names[] = { "Auto (use global setting)",
                                                  "CP437 (BBS art / bitmap)",
                                                  "UTF-8 (Unicode / TTF)", NULL };
            int tm = (int)work.term_mode;
            if (tm < 0 || tm > 2) tm = 0;
            int r = photon_ui_list(ui, "Terminal Mode", tmode_names, 3, &tm);
            if (r >= 0) work.term_mode = (photon_term_mode_t)r;
            break;
        }
        case EDIT_FIELD_PMODE: {
            static const char *pmode_names[] = { "Auto (CGA for BBS, xterm for shell)",
                                                  "CGA 16-color (classic BBS art)",
                                                  "xterm 256-color (modern shell/apps)", NULL };
            int pm = (int)work.palette_mode;
            if (pm < 0 || pm > 2) pm = 0;
            int r = photon_ui_list(ui, "Palette Mode", pmode_names, 3, &pm);
            if (r >= 0) work.palette_mode = (photon_palette_mode_t)r;
            break;
        }
        case EDIT_FIELD_COMMENT: {
            char tmp[PHOTON_BBS_COMMENT_MAX + 1];
            SAFECOPY(tmp, work.comment);
            int r = photon_ui_input(ui, "Comment", tmp, sizeof(tmp),
                                    PHOTON_INPUT_EDIT | PHOTON_INPUT_TRIM);
            if (r >= 0) SAFECOPY(work.comment, tmp);
            break;
        }
        case EDIT_FIELD_SAVE:
            if (!work.name[0]) {
                photon_ui_msg(ui, "Name cannot be empty.");
                break;
            }
            if (!work.addr[0] && work.conn_type != PHOTON_CONN_SHELL) {
                photon_ui_msg(ui, "Address cannot be empty.");
                break;
            }
            *bbs = work;
            return true;

        default:
            break;
        }
    }
}

/* ── Directory browser (main loop) ──────────────────────────────────── */
static photon_bbs_t *run_directory(photon_ui_t *ui, photon_settings_t *s,
                                    const photon_bbs_t *reselect, bool *quit_out)
{
    photon_bbs_t *list = calloc(MAX_BBS_ENTRIES, sizeof(*list));
    if (!list) return NULL;

    int count = photon_store_load(list, MAX_BBS_ENTRIES, NULL, 0);
    if (count < 0) count = 0;

    int sort_mode = 0;
    photon_menu_list_state_t state = { .cursor = 0, .scroll = 0 };
    photon_bbs_t *result = NULL;
    bool done = false;

    /* Pre-select the entry matching reselect (e.g. after disconnect) */
    if (reselect && reselect->name[0]) {
        PHOTON_DBG("reselect: looking for name='%s' addr='%s' port=%d type=%d",
                   reselect->name, reselect->addr, reselect->port, reselect->conn_type);
        for (int i = 0; i < count; i++) {
            if (strcmp(list[i].name, reselect->name) == 0
                && strcmp(list[i].addr, reselect->addr) == 0
                && list[i].port == reselect->port
                && list[i].conn_type == reselect->conn_type) {
                state.cursor = i;
                PHOTON_DBG("reselect: matched entry %d '%s'", i, list[i].name);
                break;
            }
        }
    }

    static const photon_hint_t hints[] = {
        { "Return", " Connect  " },
        { "E",      " Edit  " },
        { "+/N",    " New  " },
        { "-/D",    " Delete  " },
        { "S",      " Sort  " },
        { "Tab",    " Settings  " },
        { "Esc",    " Back" },
    };

    while (!done) {
        char title[80];
        snprintf(title, sizeof(title), "Directory%s", sort_mode_name(sort_mode));

        photon_menu_list_t menu = {
            .title       = title,
            .items       = list,
            .count       = count,
            .empty_msg   = "No entries - press + or N to add one",
            .draw_item   = dir_draw_item,
            .hints       = hints,
            .nhints      = sizeof(hints) / sizeof(hints[0]),
            .header_rows = 1,
            .draw_header = dir_draw_header,
        };

        int visible = photon_menu_draw_list(ui, &menu, &state);

        /* Status bar: show entry info for selected item */
        if (count > 0 && state.cursor >= 0 && state.cursor < count)
            draw_entry_statusbar(ui, &list[state.cursor]);

        photon_sdl_present(photon_ui_sdl(ui));

        photon_key_t key = {0};
        if (!photon_sdl_wait_key(photon_ui_sdl(ui), &key, 100)) continue;
        if (key.code == 0) continue;

        /* Standard list navigation (arrows, pgup/dn, home/end) */
        if (photon_menu_list_handle_nav(&key, &state, count, visible))
            continue;

        switch (key.code) {
        case PHOTON_KEY_QUIT:
            if (quit_out) *quit_out = true;
            done = true;
            break;

        case '\r': case ' ':
            if (count > 0) {
                result = malloc(sizeof(*result));
                if (result) {
                    *result = list[state.cursor];
                    result->last_connected = time(NULL);
                    result->calls++;
                    list[state.cursor] = *result;
                    photon_store_save(list, count);
                }
                done = true;
            }
            break;

        case 'N': case 'n': case '+':
            if (count < MAX_BBS_ENTRIES) {
                photon_bbs_t nb;
                memset(&nb, 0, sizeof(nb));
                nb.conn_type = PHOTON_CONN_TELNET;
                nb.port      = 23;
                nb.added     = time(NULL);
                nb.id        = count;
                photon_sdl_flush_keys(photon_ui_sdl(ui));
                if (photon_bbs_edit(ui, &nb, true)) {
                    list[count++] = nb;
                    state.cursor = count - 1;
                    photon_store_save(list, count);
                }
            }
            break;

        case 'E': case 'e':
            if (count > 0) {
                photon_sdl_flush_keys(photon_ui_sdl(ui));
                if (photon_bbs_edit(ui, &list[state.cursor], false))
                    photon_store_save(list, count);
            }
            break;

        case 'D': case 'd': case '-':
        case PHOTON_KEY_DEL:
            if (count > 0) {
                photon_sdl_flush_keys(photon_ui_sdl(ui));
                if (photon_ui_confirm(ui, "Delete this entry?")) {
                    for (int i = state.cursor; i < count - 1; i++)
                        list[i] = list[i+1];
                    count--;
                    if (state.cursor >= count && state.cursor > 0)
                        state.cursor = count - 1;
                    for (int i = 0; i < count; i++) list[i].id = i;
                    photon_store_save(list, count);
                }
            }
            break;

        case '\t':
            photon_sdl_flush_keys(photon_ui_sdl(ui));
            run_settings(ui, s);
            break;

        case 'S': case 's':
            sort_mode = (sort_mode + 1) % 4;
            apply_sort(list, count, sort_mode);
            state.cursor = 0;
            state.scroll = 0;
            break;

        case '\x1b':
            done = true;
            break;

        case 'Q': case 'q':
            if (photon_ui_confirm(ui, "Quit PhotonTERM?")) {
                if (quit_out) *quit_out = true;
                done = true;
            }
            break;

        default:
            /* Typeahead: jump to next entry whose name starts with key */
            if (key.code >= ' ' && key.code <= '~' && count > 0) {
                char ch = (char)key.code;
                for (int i = 1; i <= count; i++) {
                    int next = (state.cursor + i) % count;
                    if (tolower((unsigned char)list[next].name[0])
                        == tolower((unsigned char)ch)) {
                        state.cursor = next;
                        break;
                    }
                }
            }
            break;
        }
    }

    free(list);
    return result;
}

/* ── Public entry point ──────────────────────────────────────────────── */

photon_bbs_t *photon_bbslist_run(photon_ui_t *ui, bool start_in_directory,
                                  const photon_bbs_t *reselect)
{
    /* Load settings and apply active theme */
    photon_settings_t s;
    photon_settings_load(&s);
    int theme_idx = photon_theme_find(s.theme_name);
    photon_theme_apply(theme_idx, photon_ui_sdl(ui), &s);

    photon_bbs_t *result = NULL;
    bool done = false;
    bool quit = false;

    /* Splash loop */
    while (!done) {
        if (start_in_directory) {
            /* Go straight to directory; if user ESCs, fall back to splash */
            result = run_directory(ui, &s, reselect, &quit);
            reselect = NULL;  /* only applies to first directory open */
            if (result) {
                done = true;
            } else if (quit) {
                done = true;
            } else {
                /* User cancelled the directory - show splash from here on */
                start_in_directory = false;
            }
        } else {
            draw_splash(ui);
            photon_sdl_present(photon_ui_sdl(ui));

            photon_key_t key = {0};
            if (!photon_sdl_wait_key(photon_ui_sdl(ui), &key, 200)) continue;
            if (key.code == 0) continue;

            switch (key.code) {
            case 'Q': case 'q': case '\x1b':
                /* Confirm quit */
                if (photon_ui_confirm(ui, "Quit PhotonTERM?"))
                    done = true;
                break;

            case PHOTON_KEY_QUIT:
                done = true;
                break;

            case '\t': /* Tab -> Settings from splash */
                {
                    run_settings(ui, &s);
                }
                break;

            default:
                /* Any other key - open directory */
                result = run_directory(ui, &s, NULL, &quit);
                if (result || quit)
                    done = true;
                /* else: user ESC'd from directory - loop back to splash */
                break;
            }
        }
    }

    return result;
}

void photon_bbslist_free(photon_bbs_t *bbs)
{
    free(bbs);
}

void photon_bbslist_run_settings(photon_ui_t *ui, photon_settings_t *s)
{
    run_settings(ui, s);
}
