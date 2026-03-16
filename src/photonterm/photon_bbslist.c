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
static void put_padded(photon_ui_t *ui, int col, int row,
                       const char *s, int width, uint8_t attr)
{
    char tmp[256];
    snprintf(tmp, sizeof(tmp), "%-*.*s", width, width, s ? s : "");
    put_str(ui, col, row, tmp, attr);
}

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

/* Draw top edge of box with centred title: ╔════ Title ════╗ */
static void draw_box_top_titled(photon_ui_t *ui,
                                int x1, int x2, int y,
                                uint8_t box_a, uint8_t title_a,
                                const char *title)
{
    int inner_w = x2 - x1 - 1;
    int tlen    = (int)strlen(title);
    int pad     = (inner_w - tlen - 2) / 2;
    int rem     = inner_w - tlen - 2 - 2 * pad;

    put_cell(ui, x1, y, BOX_TL, box_a);
    for (int i = 0; i < pad; i++) put_cell(ui, x1 + 1 + i, y, BOX_H, box_a);
    int cx = x1 + 1 + pad;
    put_cell(ui, cx++, y, ' ', title_a);
    cx += put_str(ui, cx, y, title, title_a);
    put_cell(ui, cx++, y, ' ', title_a);
    for (int i = 0; i < pad + rem; i++) put_cell(ui, cx + i, y, BOX_H, box_a);
    put_cell(ui, x2, y, BOX_TR, box_a);
}

/* Draw a horizontal separator ╠═══╣ */
static void draw_box_sep(photon_ui_t *ui, int x1, int x2, int y, uint8_t box_a)
{
    int inner_w = x2 - x1 - 1;
    put_cell(ui, x1, y, BOX_ML, box_a);
    for (int i = 0; i < inner_w; i++) put_cell(ui, x1 + 1 + i, y, BOX_H, box_a);
    put_cell(ui, x2, y, BOX_MR, box_a);
}

/* Draw a single directory entry row */
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

static void draw_dir_entry(photon_ui_t *ui,
                           int xi, int y, int eff_w,
                           const photon_bbs_t *bbs, bool selected,
                           bool has_scroll)
{
    const photon_theme_t *t = active_theme();
    (void)has_scroll;

    /* Calculate column widths */
    int addr_w = 28;
    int name_w = eff_w - DIR_COL_CUR - DIR_COL_SEP - DIR_COL_TYPE - 1 - addr_w;
    if (name_w < 4) name_w = 4;

    uint8_t a_norm, a_hi, a_dim;
    if (selected) {
        a_norm = A_SEL(t);
        a_hi   = A_SEL_HI(t);
        a_dim  = A_SEL(t);
    } else {
        a_norm = A_NORM(t);
        a_hi   = ATTR((t)->hclr, (t)->bclr);
        a_dim  = A_DIM(t);
    }

    /* Clear row */
    fill_rect(ui, xi, y, xi + eff_w - 1, y, a_norm);

    /* Cursor indicator */
    put_cell(ui, xi,     y, selected ? ARROW_RIGHT : ' ', a_hi);
    put_cell(ui, xi + 1, y, ' ', a_norm);

    /* Name */
    put_padded(ui, xi + DIR_COL_CUR, y, bbs->name, name_w, a_norm);

    /* Space + Type */
    put_cell(ui, xi + DIR_COL_CUR + name_w, y, ' ', a_dim);
    put_padded(ui, xi + DIR_COL_CUR + name_w + DIR_COL_SEP, y,
               conn_type_label(bbs->conn_type), DIR_COL_TYPE, a_dim);

    /* Space + Address / Command */
    put_cell(ui, xi + DIR_COL_CUR + name_w + DIR_COL_SEP + DIR_COL_TYPE, y, ' ', a_dim);
    const char *addr_display = bbs->addr[0] ? bbs->addr
                               : (bbs->conn_type == PHOTON_CONN_SHELL ? "$SHELL" : "");
    put_padded(ui, xi + DIR_COL_CUR + name_w + DIR_COL_SEP + DIR_COL_TYPE + 1, y,
               addr_display, addr_w, a_dim);
}

/* Full directory redraw */
static void redraw_dir(photon_ui_t *ui,
                       int x1, int y1, int x2, int y2,
                       const photon_bbs_t *list, int count,
                       int cur, int top, bool connected, int sort_mode)
{
    const photon_theme_t *t = active_theme();

    int ix1      = x1 + 1;
    int ix2      = x2 - 1;
    int inner_w  = ix2 - ix1 + 1;
    int hdr_row  = y1 + 1;
    int sep_row  = y2 - 2;
    int hint_row = y2 - 1;
    int list_y1  = hdr_row + 1;
    int list_y2  = sep_row - 1;
    int visible  = list_y2 - list_y1 + 1;
    if (visible < 1) visible = 1;

    bool has_scroll = count > visible;
    int  eff_w      = has_scroll ? inner_w - 1 : inner_w;

    uint8_t box_a   = A_BOX(t);
    uint8_t title_a = A_TITLE(t);
    uint8_t hdr_a   = A_HDR(t);
    uint8_t norm_a  = A_NORM(t);
    uint8_t hint_a  = A_HINT(t);
    uint8_t hintk_a = A_HINT_K(t);

    /* Top edge with title */
    char dir_title[64];
    snprintf(dir_title, sizeof(dir_title), "PhotonTERM Directory%s", sort_mode_name(sort_mode));
    draw_box_top_titled(ui, x1, x2, y1, box_a, title_a, dir_title);

    /* Side edges */
    for (int r = y1 + 1; r < y2; r++) {
        put_cell(ui, x1, r, BOX_V, box_a);
        put_cell(ui, x2, r, BOX_V, box_a);
    }

    /* Bottom edge */
    put_cell(ui, x1, y2, BOX_BL, box_a);
    for (int i = 0; i < inner_w; i++) put_cell(ui, x1 + 1 + i, y2, BOX_H, box_a);
    put_cell(ui, x2, y2, BOX_BR, box_a);

    /* Column header row */
    {
        int addr_w = 28;
        int name_w = eff_w - DIR_COL_CUR - DIR_COL_SEP - DIR_COL_TYPE - 1 - addr_w;
        if (name_w < 4) name_w = 4;
        fill_rect(ui, ix1, hdr_row, ix2, hdr_row, hdr_a);
        put_padded(ui, ix1 + DIR_COL_CUR,                      hdr_row, "Name",    name_w,      hdr_a);
        put_cell  (ui, ix1 + DIR_COL_CUR + name_w,             hdr_row, ' ',       hdr_a);
        put_padded(ui, ix1 + DIR_COL_CUR + name_w + DIR_COL_SEP, hdr_row, "Type", DIR_COL_TYPE, hdr_a);
        put_cell  (ui, ix1 + DIR_COL_CUR + name_w + DIR_COL_SEP + DIR_COL_TYPE, hdr_row, ' ', hdr_a);
        put_padded(ui, ix1 + DIR_COL_CUR + name_w + DIR_COL_SEP + DIR_COL_TYPE + 1, hdr_row, "Address", addr_w, hdr_a);
    }

    /* Separator after header */
    draw_box_sep(ui, x1, x2, sep_row, box_a);

    /* Hint bar */
    {
        const char *hints = connected
            ? "ENTER Edit  E Edit  +/N New  -/D Delete  S Sort  Tab Settings  ESC Close"
            : "ENTER Connect  E Edit  +/N New  -/D Delete  S Sort  Tab Settings  ESC Back";
        int hlen = (int)strlen(hints);
        int hx   = ix1 + (inner_w - hlen) / 2;
        if (hx < ix1) hx = ix1;
        fill_rect(ui, ix1, hint_row, ix2, hint_row, hint_a);
        put_str(ui, hx, hint_row, hints, hintk_a);
    }

    /* List area */
    fill_rect(ui, ix1, list_y1, ix2, list_y2, norm_a);

    if (count == 0) {
        static const char *msg = "No entries - press + or N to add one";
        int mx = ix1 + (inner_w - (int)strlen(msg)) / 2;
        int my = list_y1 + (list_y2 - list_y1) / 2;
        if (mx < ix1) mx = ix1;
        put_str(ui, mx, my, msg, A_DIM(t));
        return;
    }

    /* Scrollbar */
    if (has_scroll) {
        int scol  = ix2;
        int sh    = list_y2 - list_y1 + 1;
        int thumb = sh > 0 ? (int)((long)cur * sh / count) : 0;
        for (int i = 0; i < sh; i++) {
            put_cell(ui, scol, list_y1 + i,
                     i == thumb ? BLK_FULL : BLK_LIGHT, norm_a);
        }
    }

    /* Entry rows */
    for (int i = 0; i < visible; i++) {
        int idx = top + i;
        if (idx < count && list[idx].name[0])
            draw_dir_entry(ui, ix1, list_y1 + i, eff_w,
                           &list[idx], idx == cur, has_scroll);
        else
            fill_rect(ui, ix1, list_y1 + i, ix1 + eff_w - 1, list_y1 + i, norm_a);
    }
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

/* Enum for settings menu items */
typedef enum {
    SOPT_THEME   = 0,
    SOPT_MODE    = 1,
    SOPT_FONT_SZ = 2,
    SOPT_TERM_SZ = 3,
    SOPT_BELL    = 4,
    SOPT_ABOUT   = 5,
    SOPT_COUNT   = 6,
} settings_opt_t;

/*
 * Run the full settings menu. Uses photon_ui_list for clean save/restore.
 * Pass sdl for in-session use (allows checking resize after return).
 */
static void run_settings(photon_ui_t *ui, photon_settings_t *s)
{
    const char *items[SOPT_COUNT + 1];
    items[SOPT_THEME]   = "Theme";
    items[SOPT_MODE]    = "Terminal Mode  (CP437 / UTF-8)";
    items[SOPT_FONT_SZ] = "Font Size";
    items[SOPT_TERM_SZ] = "Terminal Size";
    items[SOPT_ABOUT]   = "About PhotonTERM";
    items[SOPT_COUNT]   = NULL;
    char bell_label[64];
    static int cur = 0;

    for (;;) {
        snprintf(bell_label, sizeof(bell_label),
                 "Visual Bell: %s", s->bell_enabled ? "On " : "Off");
        items[SOPT_BELL] = bell_label;
        int sel = photon_ui_list(ui, "Settings", items, SOPT_COUNT, &cur);
        if (sel < 0) break;
        cur = sel;
        switch ((settings_opt_t)sel) {
            case SOPT_THEME:   run_theme_picker(ui, s);    break;
            case SOPT_MODE:    run_terminal_mode(ui, s);   break;
            case SOPT_FONT_SZ: run_font_size(ui, s);       break;
            case SOPT_TERM_SZ: run_terminal_size(ui, s);   break;
            case SOPT_BELL:
                s->bell_enabled = !s->bell_enabled;
                g_bell_enabled  = s->bell_enabled;
                photon_settings_save(s);
                break;
            case SOPT_ABOUT: {
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
static photon_bbs_t *run_directory(photon_ui_t *ui, photon_settings_t *s)
{
    photon_bbs_t *list = calloc(MAX_BBS_ENTRIES, sizeof(*list));
    if (!list) return NULL;

    int count = photon_store_load(list, MAX_BBS_ENTRIES, NULL, 0);
    if (count < 0) count = 0;

    int W = tcols(ui);
    int H = trows(ui);

    /* Box coordinates: nearly full screen (x=2..W-1, y=2..H-2) */
    int x1 = 2, y1 = 2;
    int x2 = W - 1, y2 = H - 2;

    int cur    = 0;
    int top    = 0;
    int sort_mode = 0;  /* 0=manual order, 1=name, 2=last-connected, 3=calls */
    bool redraw = true;
    photon_bbs_t *result = NULL;
    bool done  = false;

    /* Settings */
    /* (settings state managed by run_settings()) */

    while (!done) {
        int visible = (y2 - 2) - (y1 + 1);   /* list_y2 - list_y1 + 1 */
        if (visible < 1) visible = 1;

        /* Clamp cursor */
        if (count > 0) {
            if (cur < 0)      cur = 0;
            if (cur >= count) cur = count - 1;
        } else {
            cur = 0;
        }
        if (top > cur)           top = cur;
        if (top + visible <= cur) top = cur - visible + 1;
        if (top < 0)             top = 0;

        if (redraw) {
            /* Clear full screen background first */
            const photon_theme_t *t = active_theme();
            fill_rect(ui, 1, 1, W, H - 1, A_NORM(t));
            if (count > 0)
                draw_entry_statusbar(ui, &list[cur]);
            else
                draw_statusbar(ui, false, NULL);
            redraw_dir(ui, x1, y1, x2, y2, list, count, cur, top, false, sort_mode);
            photon_sdl_present(photon_ui_sdl(ui));
            redraw = false;
        }

        photon_key_t key = {0};
        if (!photon_sdl_wait_key(photon_ui_sdl(ui), &key, 100)) continue;
        if (key.code == 0) continue;

        switch (key.code) {
        case PHOTON_KEY_QUIT:
            done = true;
            break;

        case PHOTON_KEY_UP:
            if (count > 0 && cur > 0) { cur--; redraw = true; }
            break;

        case PHOTON_KEY_DOWN:
            if (count > 0 && cur < count - 1) { cur++; redraw = true; }
            break;

        case PHOTON_KEY_PGUP:
            if (count > 0) { cur -= visible - 1; if (cur < 0) cur = 0; redraw = true; }
            break;

        case PHOTON_KEY_PGDN:
            if (count > 0) {
                cur += visible - 1;
                if (cur >= count) cur = count - 1;
                redraw = true;
            }
            break;

        case PHOTON_KEY_HOME:
            if (count > 0) { cur = 0; redraw = true; }
            break;

        case PHOTON_KEY_END:
            if (count > 0) { cur = count - 1; redraw = true; }
            break;

        case '\r': case ' ':
            /* Connect */
            if (count > 0) {
                result = malloc(sizeof(*result));
                if (result) {
                    *result = list[cur];
                    result->last_connected = time(NULL);
                    result->calls++;
                    list[cur] = *result;
                    photon_store_save(list, count);
                }
                done = true;
            }
            break;

        case 'N': case 'n': case '+':
            /* New entry */
            if (count < MAX_BBS_ENTRIES) {
                photon_bbs_t nb;
                memset(&nb, 0, sizeof(nb));
                nb.conn_type = PHOTON_CONN_TELNET;
                nb.port      = 23;
                nb.added     = time(NULL);
                nb.id        = count;
                PHOTON_DBG("N: flushing keys before edit");
                photon_sdl_flush_keys(photon_ui_sdl(ui));
                PHOTON_DBG("N: calling photon_bbs_edit");
                if (photon_bbs_edit(ui, &nb, true)) {
                    list[count++] = nb;
                    cur = count - 1;
                    photon_store_save(list, count);
                }
            }
            redraw = true;
            break;

        case 'E': case 'e':
            /* Edit */
            if (count > 0) {
                photon_sdl_flush_keys(photon_ui_sdl(ui));
                if (photon_bbs_edit(ui, &list[cur], false))
                    photon_store_save(list, count);
            }
            redraw = true;
            break;

        case 'D': case 'd': case '-':
        case PHOTON_KEY_DEL:
            /* Delete */
            if (count > 0) {
                photon_sdl_flush_keys(photon_ui_sdl(ui));
                if (photon_ui_confirm(ui, "Delete this entry?")) {
                    for (int i = cur; i < count - 1; i++)
                        list[i] = list[i+1];
                    count--;
                    if (cur >= count && cur > 0) cur = count - 1;
                    for (int i = 0; i < count; i++) list[i].id = i;
                    photon_store_save(list, count);
                }
            }
            redraw = true;
            break;

        case '\t': /* Tab -> Settings */
        {
            photon_sdl_flush_keys(photon_ui_sdl(ui));
            run_settings(ui, s);
            redraw = true;
        }
        break;

        case 'S': case 's':
            /* Cycle sort modes: manual -> name -> recent -> calls -> manual */
            sort_mode = (sort_mode + 1) % 4;
            apply_sort(list, count, sort_mode);
            cur = 0; top = 0;
            redraw = true;
            break;

        case '\x1b': /* ESC - back to splash */
            done = true;
            break;

        case 'Q': case 'q':
            done = true;
            break;

        default:
            /* Typeahead: jump to next entry whose name starts with key */
            if (key.code >= ' ' && key.code <= '~' && count > 0) {
                char ch = (char)key.code;
                for (int i = 1; i <= count; i++) {
                    int next = (cur + i) % count;
                    if (tolower((unsigned char)list[next].name[0])
                        == tolower((unsigned char)ch)) {
                        cur = next;
                        redraw = true;
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

photon_bbs_t *photon_bbslist_run(photon_ui_t *ui, bool start_in_directory)
{
    /* Load settings and apply active theme */
    photon_settings_t s;
    photon_settings_load(&s);
    int theme_idx = photon_theme_find(s.theme_name);
    photon_theme_apply(theme_idx, photon_ui_sdl(ui), &s);

    int W = tcols(ui);
    int H = trows(ui);

    photon_bbs_t *result = NULL;
    bool done = false;

    /* Splash loop */
    while (!done) {
        if (start_in_directory) {
            /* Go straight to directory; if user ESCs, fall back to splash */
            result = run_directory(ui, &s);
            if (result) {
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
                    /* Draw directory background first so settings overlay is readable */
                    const photon_theme_t *t = active_theme();
                    fill_rect(ui, 1, 1, W, H - 1, A_NORM(t));
                    draw_statusbar(ui, false, NULL);
                    photon_sdl_present(photon_ui_sdl(ui));

                    run_settings(ui, &s);
                }
                break;

            default:
                /* Any other key - open directory */
                result = run_directory(ui, &s);
                if (result)
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
