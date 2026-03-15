/* photon_ui.c - PhotonTERM text-mode UI widgets
 *
 * Copyright (C) 2026 fewtarius and PhotonTERM contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#define PHOTON_DEBUG_BUILD
#include "photon_debug.h"
#include "photon_ui.h"
#include "photon_sdl.h"
#include "photon_vte.h"

#include <assert.h>
#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── Internal helpers ──────────────────────────────────────────────────── */

/* Box-drawing characters (Unicode, rendered via TTF) */
#define BOX_TL   0x250C  /* ┌ */
#define BOX_TR   0x2510  /* ┐ */
#define BOX_BL   0x2514  /* └ */
#define BOX_BR   0x2518  /* ┘ */
#define BOX_H    0x2500  /* ─ */
#define BOX_V    0x2502  /* │ */
#define BOX_ML   0x251C  /* ├ */
#define BOX_MR   0x2524  /* ┤ */

/* ── Saved screen ──────────────────────────────────────────────────────── */

struct photon_ui_screen {
    int        cols, rows;
    vte_cell_t cells[];   /* flexible array: cols*rows */
};

/* ── Context ───────────────────────────────────────────────────────────── */

struct photon_ui {
    photon_sdl_t       *sdl;
    vte_t              *vte;
    photon_ui_colors_t  colors;
};

static const photon_ui_colors_t default_colors = {
    .border_fg = 14,  /* bright cyan   */
    .border_bg =  1,  /* dark blue     */
    .title_fg  = 15,  /* bright white  */
    .normal_fg = 15,  /* bright white  */
    .normal_bg =  1,  /* dark blue     */
    .hilite_fg =  0,  /* black         */
    .hilite_bg = 14,  /* bright cyan   */
    .input_fg  = 11,  /* bright yellow */
    .input_bg  =  1,  /* dark blue     */
};

/* Global handle set by photonterm.c */
photon_ui_t *photon_ui_global = NULL;

photon_ui_t *photon_ui_create(photon_sdl_t *sdl, vte_t *vte)
{
    photon_ui_t *ui = calloc(1, sizeof(*ui));
    if (!ui) return NULL;
    ui->sdl    = sdl;
    ui->vte    = vte;
    ui->colors = default_colors;
    return ui;
}

void photon_ui_free(photon_ui_t *ui)
{
    free(ui);
}

photon_sdl_t *photon_ui_sdl(photon_ui_t *ui) { return ui ? ui->sdl : NULL; }
vte_t        *photon_ui_vte(photon_ui_t *ui) { return ui ? ui->vte : NULL; }

void photon_ui_set_colors(photon_ui_t *ui, const photon_ui_colors_t *c)
{
    if (ui && c) ui->colors = *c;
}

/* ── Screen save / restore ─────────────────────────────────────────────── */

photon_ui_screen_t *photon_ui_save_screen(photon_ui_t *ui)
{
    int cols = photon_sdl_cols(ui->sdl);
    int rows = photon_sdl_rows(ui->sdl);
    size_t n = (size_t)(cols * rows);

    photon_ui_screen_t *s = malloc(sizeof(*s) + n * sizeof(vte_cell_t));
    if (!s) return NULL;
    s->cols = cols;
    s->rows = rows;

    for (int r = 1; r <= rows; r++)
        for (int c = 1; c <= cols; c++) {
            vte_cell_t cell = {0};
            photon_sdl_get_cell(ui->sdl, c, r, &cell);
            s->cells[(r - 1) * cols + (c - 1)] = cell;
        }
    return s;
}

void photon_ui_restore_screen(photon_ui_t *ui, photon_ui_screen_t *s)
{
    if (!ui || !s) return;
    int cols = s->cols < vte_cols(ui->vte) ? s->cols : vte_cols(ui->vte);
    int rows = s->rows < vte_rows(ui->vte) ? s->rows : vte_rows(ui->vte);
    for (int r = 1; r <= rows; r++) {
        for (int c = 1; c <= cols; c++) {
            const vte_cell_t *cell = &s->cells[(r-1)*s->cols + (c-1)];
            photon_sdl_draw_cell(ui->sdl, c, r, cell);
        }
    }
    photon_sdl_present(ui->sdl);
}

void photon_ui_free_screen(photon_ui_screen_t *s)
{
    free(s);
}

/* Paint a saved screen into the renderer without presenting.
   Used to restore background before drawing a dialog on top. */
static void repaint_saved(photon_ui_t *ui, const photon_ui_screen_t *s)
{
    if (!ui || !s) return;
    int cols = s->cols < photon_sdl_cols(ui->sdl)
               ? s->cols : photon_sdl_cols(ui->sdl);
    int rows = s->rows < photon_sdl_rows(ui->sdl)
               ? s->rows : photon_sdl_rows(ui->sdl);
    for (int r = 1; r <= rows; r++)
        for (int c = 1; c <= cols; c++)
            photon_sdl_draw_cell(ui->sdl, c, r,
                &s->cells[(r-1)*s->cols + (c-1)]);
}

/* ── Low-level draw helpers ────────────────────────────────────────────── */

static void ui_put_cell(photon_ui_t *ui, int col, int row,
                        uint32_t cp, uint8_t fg, uint8_t bg, uint8_t attr)
{
    vte_cell_t cell = {
        .codepoint = cp,
        .fg        = fg,
        .bg        = bg,
        .attr      = attr,
    };
    photon_sdl_draw_cell(ui->sdl, col, row, &cell);
}

static void ui_put_str(photon_ui_t *ui, int col, int row,
                       const char *s, uint8_t fg, uint8_t bg)
{
    int c = col;
    const unsigned char *p = (const unsigned char *)s;
    while (*p) {
        /* Simple UTF-8 decode */
        uint32_t cp;
        if (*p < 0x80) {
            cp = *p++;
        } else if ((*p & 0xE0) == 0xC0) {
            cp = (uint32_t)(*p++ & 0x1F) << 6;
            if (*p) cp |= *p++ & 0x3F;
        } else if ((*p & 0xF0) == 0xE0) {
            cp = (uint32_t)(*p++ & 0x0F) << 12;
            if (*p) cp |= (uint32_t)(*p++ & 0x3F) << 6;
            if (*p) cp |= *p++ & 0x3F;
        } else {
            cp = '?'; p++;
        }
        ui_put_cell(ui, c++, row, cp, fg, bg, 0);
    }
}

/* Clear a rectangular area (1-based, inclusive) */
static void ui_fill_rect(photon_ui_t *ui,
                         int col1, int row1, int col2, int row2,
                         uint8_t fg, uint8_t bg)
{
    for (int r = row1; r <= row2; r++)
        for (int c = col1; c <= col2; c++)
            ui_put_cell(ui, c, r, ' ', fg, bg, 0);
}

/* Draw a box; title may be NULL */
static void ui_draw_box(photon_ui_t *ui,
                        int col1, int row1, int col2, int row2,
                        const char *title)
{
    const photon_ui_colors_t *cl = &ui->colors;
    uint8_t bfg = cl->border_fg;
    uint8_t bbg = cl->border_bg;

    /* Fill interior */
    if (row2 > row1 + 1 && col2 > col1 + 1)
        ui_fill_rect(ui, col1+1, row1+1, col2-1, row2-1, cl->normal_fg, bbg);

    /* Corners */
    ui_put_cell(ui, col1, row1, BOX_TL, bfg, bbg, 0);
    ui_put_cell(ui, col2, row1, BOX_TR, bfg, bbg, 0);
    ui_put_cell(ui, col1, row2, BOX_BL, bfg, bbg, 0);
    ui_put_cell(ui, col2, row2, BOX_BR, bfg, bbg, 0);

    /* Top and bottom edges */
    for (int c = col1+1; c < col2; c++) {
        ui_put_cell(ui, c, row1, BOX_H, bfg, bbg, 0);
        ui_put_cell(ui, c, row2, BOX_H, bfg, bbg, 0);
    }

    /* Side edges */
    for (int r = row1+1; r < row2; r++) {
        ui_put_cell(ui, col1, r, BOX_V, bfg, bbg, 0);
        ui_put_cell(ui, col2, r, BOX_V, bfg, bbg, 0);
    }

    /* Title */
    if (title && *title) {
        int max_title = col2 - col1 - 3;
        char tbuf[128];
        snprintf(tbuf, sizeof(tbuf), " %.*s ", max_title, title);
        int tlen = (int)strlen(tbuf);
        int tstart = col1 + (col2 - col1 - tlen) / 2;
        if (tstart < col1+1) tstart = col1+1;
        ui_put_str(ui, tstart, row1, tbuf, cl->title_fg, bbg);
    }
}

/* ── Geometry helpers ───────────────────────────────────────────────────── */

/* Compute a centered box position on the terminal grid */
static void center_box(photon_ui_t *ui, int box_w, int box_h,
                       int *out_col1, int *out_row1)
{
    int tcols = vte_cols(ui->vte);
    int trows = vte_rows(ui->vte);
    *out_col1 = (tcols - box_w) / 2 + 1;
    *out_row1 = (trows - box_h) / 2 + 1;
    if (*out_col1 < 1) *out_col1 = 1;
    if (*out_row1 < 1) *out_row1 = 1;
}

/* ── List picker ────────────────────────────────────────────────────────── */

int photon_ui_list(photon_ui_t *ui,
                   const char *title,
                   const char **items,
                   int n_items,
                   int *cur)
{
    if (!ui || !items) return -1;

    /* Count items if not provided */
    if (n_items <= 0) {
        n_items = 0;
        while (items[n_items]) n_items++;
    }
    if (n_items == 0) return -1;

    /* Width: longest item + 2 padding + 2 border */
    int max_item_w = (int)strlen(title ? title : "");
    for (int i = 0; i < n_items; i++) {
        int w = (int)strlen(items[i]);
        if (w > max_item_w) max_item_w = w;
    }
    if (max_item_w < 10) max_item_w = 10;
    if (max_item_w > vte_cols(ui->vte) - 4) max_item_w = vte_cols(ui->vte) - 4;

    int box_w    = max_item_w + 4;           /* 2 border + 2 padding */
    int max_vis  = vte_rows(ui->vte) - 6;    /* leave some margin */
    if (max_vis < 3) max_vis = 3;
    int vis_rows = n_items < max_vis ? n_items : max_vis;
    int box_h    = vis_rows + 2;             /* +2 for borders */

    int col1, row1;
    center_box(ui, box_w, box_h, &col1, &row1);
    int col2 = col1 + box_w - 1;
    int row2 = row1 + box_h - 1;

    /* Save screen */
    photon_ui_screen_t *saved = photon_ui_save_screen(ui);

    /* Current selection */
    int sel    = cur ? *cur : 0;
    int scroll = 0;  /* first visible item index */

    if (sel < 0) sel = 0;
    if (sel >= n_items) sel = n_items - 1;

    const photon_ui_colors_t *cl = &ui->colors;

    for (;;) {
        /* Adjust scroll so sel is visible */
        if (sel < scroll) scroll = sel;
        if (sel >= scroll + vis_rows) scroll = sel - vis_rows + 1;

        /* Repaint background from saved screen first to avoid
         * back-buffer flicker with hardware-accelerated SDL rendering */
        if (saved) {
            repaint_saved(ui, saved);
        }

        /* Draw box */
        ui_draw_box(ui, col1, row1, col2, row2, title);

        /* Draw items */
        for (int i = 0; i < vis_rows; i++) {
            int idx  = scroll + i;
            int row  = row1 + 1 + i;
            int icol = col1 + 2;

            /* Clear item row */
            ui_fill_rect(ui, icol, row, col2-2, row,
                         cl->normal_fg, cl->normal_bg);

            if (idx >= n_items) continue;

            uint8_t fg = (idx == sel) ? cl->hilite_fg : cl->normal_fg;
            uint8_t bg = (idx == sel) ? cl->hilite_bg : cl->normal_bg;

            /* Truncate item to box width */
            char tmp[256];
            int avail = max_item_w;
            snprintf(tmp, sizeof(tmp), "%-*.*s", avail, avail, items[idx]);
            ui_put_str(ui, icol, row, tmp, fg, bg);
        }

        /* Scroll indicators */
        if (scroll > 0)
            ui_put_cell(ui, col2-1, row1+1, 0x25B2 /* ▲ */,
                        cl->border_fg, cl->border_bg, 0);
        if (scroll + vis_rows < n_items)
            ui_put_cell(ui, col2-1, row2-1, 0x25BC /* ▼ */,
                        cl->border_fg, cl->border_bg, 0);

        photon_sdl_present(ui->sdl);

        /* Wait for key */
        photon_key_t key = {0};
        if (!photon_sdl_wait_key(ui->sdl, &key, 100)) continue;
        if (key.code == 0) continue;

        switch (key.code) {
        case PHOTON_KEY_UP:
            if (sel > 0) sel--;
            break;
        case PHOTON_KEY_DOWN:
            if (sel < n_items - 1) sel++;
            break;
        case PHOTON_KEY_PGUP:
            sel -= vis_rows - 1;
            if (sel < 0) sel = 0;
            break;
        case PHOTON_KEY_PGDN:
            sel += vis_rows - 1;
            if (sel >= n_items) sel = n_items - 1;
            break;
        case PHOTON_KEY_HOME:
            sel = 0;
            break;
        case PHOTON_KEY_END:
            sel = n_items - 1;
            break;
        case '\r':
            if (cur) *cur = sel;
            photon_ui_restore_screen(ui, saved);
            photon_ui_free_screen(saved);
            return sel;
        case 27:  /* ESC */
        case PHOTON_KEY_QUIT:
            photon_ui_restore_screen(ui, saved);
            photon_ui_free_screen(saved);
            return -1;
        default:
            /* Typeahead: jump to first item starting with typed char */
            if (key.code >= ' ' && key.code <= '~') {
                char ch = (char)tolower(key.code);
                for (int i = 0; i < n_items; i++) {
                    int next = (sel + 1 + i) % n_items;
                    if (tolower((unsigned char)items[next][0]) == (unsigned char)ch) {
                        sel = next;
                        break;
                    }
                }
            }
            break;
        }
    }
}

/* ── Text input ─────────────────────────────────────────────────────────── */

int photon_ui_input(photon_ui_t *ui,
                    const char *title,
                    char *buf,
                    int buflen,
                    int flags)
{
    if (!ui || !buf || buflen <= 0) return -1;

    /* If not EDIT mode, clear buf */
    if (!(flags & PHOTON_INPUT_EDIT)) buf[0] = '\0';

    int max_field = 40;
    if (max_field > buflen - 1) max_field = buflen - 1;
    if (max_field > vte_cols(ui->vte) - 8) max_field = vte_cols(ui->vte) - 8;

    int box_w = max_field + 4;
    int box_h = 3;
    int col1, row1;
    center_box(ui, box_w, box_h, &col1, &row1);
    int col2 = col1 + box_w - 1;
    int row2 = row1 + box_h - 1;
    int field_col = col1 + 2;
    int field_row = row1 + 1;

    photon_ui_screen_t *saved = (flags & PHOTON_INPUT_NOSAVE)
                                ? NULL
                                : photon_ui_save_screen(ui);
    const photon_ui_colors_t *cl = &ui->colors;

    int pos   = (int)strlen(buf);
    int view  = 0;  /* scroll offset within buf */

    for (;;) {
        /* Keep cursor visible */
        if (pos < view) view = pos;
        if (pos > view + max_field - 1) view = pos - max_field + 1;

        /* Repaint background from saved screen before drawing dialog */
        if (saved) {
            repaint_saved(ui, saved);
        }

        ui_draw_box(ui, col1, row1, col2, row2, title);

        /* Draw field */
        ui_fill_rect(ui, field_col, field_row, field_col + max_field - 1, field_row,
                     cl->input_fg, cl->input_bg);

        /* Draw text */
        int len = (int)strlen(buf);
        for (int i = 0; i < max_field && view + i <= len; i++) {
            uint32_t cp;
            if (view + i < len) {
                cp = (flags & PHOTON_INPUT_PASSWORD)
                     ? '*'
                     : (unsigned char)buf[view + i];
            } else {
                cp = ' ';
            }
            ui_put_cell(ui, field_col + i, field_row, cp,
                        cl->input_fg, cl->input_bg, 0);
        }

        /* Cursor indicator: underline at cursor pos */
        int cursor_screen_col = field_col + (pos - view);
        uint32_t cursor_cp = (pos < len)
            ? ((flags & PHOTON_INPUT_PASSWORD) ? '*' : (unsigned char)buf[pos])
            : ' ';
        ui_put_cell(ui, cursor_screen_col, field_row, cursor_cp,
                    cl->input_bg, cl->input_fg, 0);  /* inverted */

        photon_sdl_present(ui->sdl);

        photon_key_t key = {0};
        if (!photon_sdl_wait_key(ui->sdl, &key, 100)) continue;
        PHOTON_DBG("photon_ui_input: got key code=0x%x text='%s'", key.code, key.text);
        if (key.code == 0) continue;

        int len2 = (int)strlen(buf);

        if (key.code == '\r') {
            /* Done */
            if (flags & PHOTON_INPUT_TRIM) {
                /* Trim leading */
                char *s = buf;
                while (*s == ' ') s++;
                memmove(buf, s, strlen(s) + 1);
                /* Trim trailing */
                int l = (int)strlen(buf);
                while (l > 0 && buf[l-1] == ' ') buf[--l] = '\0';
            }
            int ret = (int)strlen(buf);
            photon_ui_restore_screen(ui, saved);
            photon_ui_free_screen(saved);
            return ret;
        }

        if (key.code == 27 || key.code == PHOTON_KEY_QUIT) {
            photon_ui_restore_screen(ui, saved);
            photon_ui_free_screen(saved);
            return -1;
        }

        if (key.code == PHOTON_KEY_LEFT) {
            if (pos > 0) pos--;
            continue;
        }
        if (key.code == PHOTON_KEY_RIGHT) {
            if (pos < len2) pos++;
            continue;
        }
        if (key.code == PHOTON_KEY_HOME) { pos = 0; continue; }
        if (key.code == PHOTON_KEY_END)  { pos = len2; continue; }

        if (key.code == PHOTON_KEY_DEL) {
            /* Delete character at cursor */
            if (pos < len2)
                memmove(buf + pos, buf + pos + 1, (size_t)(len2 - pos));
            continue;
        }
        if (key.code == 8 || key.code == 0x7f) {
            /* Backspace (BS=8) or Delete key on macOS (DEL=0x7f) */
            if (pos > 0) {
                memmove(buf + pos - 1, buf + pos, (size_t)(len2 - pos + 1));
                pos--;
            }
            continue;
        }

        /* Printable character */
        if (key.text[0] >= ' ' && len2 < buflen - 1) {
            char ch = key.text[0];
            if (flags & PHOTON_INPUT_NUMBER) {
                if (!isdigit((unsigned char)ch) && ch != '-') continue;
            }
            memmove(buf + pos + 1, buf + pos, (size_t)(len2 - pos + 1));
            buf[pos] = ch;
            pos++;
        }
    }
}

/* ── Multi-field form ─────────────────────────────────────────────────────── */

bool photon_ui_form(photon_ui_t *ui,
                    const char *title,
                    photon_form_field_t *fields,
                    int n_fields)
{
    if (!ui || !fields || n_fields <= 0) return false;
    PHOTON_DBG("photon_ui_form: ENTER title='%s' n_fields=%d", title ? title : "(null)", n_fields);

    const photon_ui_colors_t *cl = &ui->colors;
    int vte_cols_n = vte_cols(ui->vte);
    int vte_rows_n = vte_rows(ui->vte);

    /* Layout */
    int label_w = 0;
    for (int i = 0; i < n_fields; i++) {
        int lw = fields[i].label ? (int)strlen(fields[i].label) : 0;
        if (lw > label_w) label_w = lw;
    }
    if (label_w < 8) label_w = 8;

    /* Box: label_w + ": " + value_area + borders */
    int value_w = 28;                           /* value display width */
    int box_w   = 2 + label_w + 2 + value_w + 2;  /* borders + padding */
    if (box_w > vte_cols_n - 2) {
        value_w -= (box_w - (vte_cols_n - 2));
        if (value_w < 8) value_w = 8;
        box_w = 2 + label_w + 2 + value_w + 2;
    }

    /* Height: title + blank + n_fields + blank + [Enter/Esc hint] + border */
    int box_h = 2 + n_fields + 2;  /* top border, n rows, hint, bottom border */
    if (box_h > vte_rows_n - 2) box_h = vte_rows_n - 2;

    int col1, row1;
    center_box(ui, box_w, box_h, &col1, &row1);
    int col2 = col1 + box_w - 1;
    int row2 = row1 + box_h - 1;

    /* Field rows start at row1+1, label at col1+2, value at col1+2+label_w+2 */
    int val_col  = col1 + 2 + label_w + 2;
    int field_r1 = row1 + 1;
    int hint_row = row2 - 1;

    /* Per-field edit state: cursor position and view offset */
    int *pos  = calloc((size_t)n_fields, sizeof(int));
    int *view = calloc((size_t)n_fields, sizeof(int));
    if (!pos || !view) { free(pos); free(view); return false; }

    /* Initialise cursors at end of pre-filled text */
    for (int i = 0; i < n_fields; i++)
        pos[i] = fields[i].buf ? (int)strlen(fields[i].buf) : 0;

    photon_ui_screen_t *saved = photon_ui_save_screen(ui);

    int active = 0;
    bool result = false;

    for (;;) {
        repaint_saved(ui, saved);
        /* Draw box chrome */
        ui_draw_box(ui, col1, row1, col2, row2, title);

        /* Draw hint */
        const char *hint = "Tab/Enter:\xe2\x86\x93next  Enter on last:save  Esc:cancel";
        ui_put_str(ui, col1 + 1, hint_row, hint, cl->normal_fg, cl->normal_bg);

        /* Draw all fields */
        for (int i = 0; i < n_fields; i++) {
            bool act = (i == active);
            int  fg  = act ? cl->hilite_fg : cl->normal_fg;
            int  bg  = act ? cl->hilite_bg : cl->normal_bg;
            int  row = field_r1 + i;

            /* Label */
            char lbuf[32];
            snprintf(lbuf, sizeof(lbuf), "%-*s:", label_w, fields[i].label ? fields[i].label : "");
            ui_put_str(ui, col1 + 2, row, lbuf, cl->normal_fg, cl->normal_bg);

            /* Value area background */
            char blanks[64];
            memset(blanks, ' ', (size_t)value_w);
            blanks[value_w] = '\0';
            ui_put_str(ui, val_col, row, blanks, fg, bg);

            /* Value text */
            const char *buf = fields[i].buf ? fields[i].buf : "";
            int vlen = (int)strlen(buf);
            bool is_pw = (fields[i].type == PHOTON_FIELD_PASSWORD);

            if (fields[i].type == PHOTON_FIELD_CHOICE) {
                /* Show current choice value */
                int idx = 0;
                if (fields[i].n_choices > 0) {
                    for (int c = 0; c < fields[i].n_choices; c++) {
                        if (strcmp(buf, fields[i].choices[c]) == 0) {
                            idx = c; break;
                        }
                    }
                }
                const char *cv = (fields[i].n_choices > 0) ? fields[i].choices[idx] : buf;
                char row_str[64];
                snprintf(row_str, sizeof(row_str), "%-*s", value_w, cv);
                ui_put_str(ui, val_col, row, row_str, fg, bg);
            } else {
                /* Text/number/password: scrolled view */
                int v = act ? view[i] : 0;
                /* For inactive fields use simple left-aligned view */
                if (!act && vlen > value_w) v = 0;
                for (int c = 0; c < value_w; c++) {
                    int ci = v + c;
                    char ch = (ci < vlen) ? buf[ci] : ' ';
                    if (is_pw && ci < vlen) ch = '*';
                    ui_put_cell(ui, val_col + c, row, (unsigned char)ch, fg, bg, 0);
                }
            }
        }

        /* Draw cursor in active text field */
        {
            int i   = active;
            int row = field_r1 + i;
            if (fields[i].type != PHOTON_FIELD_CHOICE) {
                int v    = view[i];
                int p    = pos[i];
                int cx   = val_col + (p - v);
                if (cx >= val_col && cx < val_col + value_w) {
                    const char *buf = fields[i].buf ? fields[i].buf : "";
                    int vlen = (int)strlen(buf);
                    char ch = (p < vlen) ? buf[p] : ' ';
                    if (fields[i].type == PHOTON_FIELD_PASSWORD && p < vlen) ch = '*';
                    ui_put_cell(ui, cx, row, (unsigned char)ch,
                                cl->input_bg, cl->input_fg, 0);
                }
            }
        }

        photon_sdl_present(ui->sdl);

        photon_key_t key = {0};
        if (!photon_sdl_wait_key(ui->sdl, &key, 100)) continue;
        if (key.code == 0) continue;
        PHOTON_DBG("photon_ui_form: key code=0x%x mod=0x%x text='%s' active=%d", key.code, key.mod, key.text, active);

        /* ESC / Quit */
        if (key.code == 27 || key.code == PHOTON_KEY_QUIT) {
            result = false;
            break;
        }

        /* Shift-Tab or Up - previous field (check before plain Tab) */
        if (key.code == PHOTON_KEY_UP ||
            (key.code == '\t' && (key.mod & PHOTON_MOD_SHIFT))) {
            active = (active - 1 + n_fields) % n_fields;
            continue;
        }

        /* Tab or Down - next field */
        if (key.code == '\t' || key.code == PHOTON_KEY_DOWN) {
            active = (active + 1) % n_fields;
            continue;
        }

        /* Enter: advance to next field, or submit from last field */
        if (key.code == '\r') {
            if (active < n_fields - 1) {
                active++;
                continue;
            }
            result = true;
            break;
        }

        /* Active field editing */
        photon_form_field_t *f = &fields[active];

        if (f->type == PHOTON_FIELD_CHOICE) {
            /* Space/Enter/Left/Right cycle choices */
            if (key.code == ' ' || key.code == PHOTON_KEY_RIGHT) {
                if (f->n_choices > 0) {
                    int idx = 0;
                    for (int c = 0; c < f->n_choices; c++) {
                        if (strcmp(f->buf, f->choices[c]) == 0) { idx = c; break; }
                    }
                    idx = (idx + 1) % f->n_choices;
                    strncpy(f->buf, f->choices[idx], (size_t)(f->buflen - 1));
                    f->buf[f->buflen - 1] = '\0';
                }
            } else if (key.code == PHOTON_KEY_LEFT) {
                if (f->n_choices > 0) {
                    int idx = 0;
                    for (int c = 0; c < f->n_choices; c++) {
                        if (strcmp(f->buf, f->choices[c]) == 0) { idx = c; break; }
                    }
                    idx = (idx - 1 + f->n_choices) % f->n_choices;
                    strncpy(f->buf, f->choices[idx], (size_t)(f->buflen - 1));
                    f->buf[f->buflen - 1] = '\0';
                }
            }
            continue;
        }

        /* Text field navigation and editing */
        int  *p   = &pos[active];
        int  *v   = &view[active];
        int   len3 = f->buf ? (int)strlen(f->buf) : 0;
        int   cap  = f->buflen - 1;

        if (key.code == PHOTON_KEY_LEFT) {
            if (*p > 0) (*p)--;
        } else if (key.code == PHOTON_KEY_RIGHT) {
            if (*p < len3) (*p)++;
        } else if (key.code == PHOTON_KEY_HOME) {
            *p = 0;
        } else if (key.code == PHOTON_KEY_END) {
            *p = len3;
        } else if (key.code == 1) {
            /* Ctrl-A: go to start */
            *p = 0;
        } else if (key.code == 5) {
            /* Ctrl-E: go to end */
            *p = len3;
        } else if (key.code == 11 && f->buf) {
            /* Ctrl-K: clear from cursor to end */
            f->buf[*p] = '\0';
        } else if (key.code == 21 && f->buf) {
            /* Ctrl-U: clear entire field */
            f->buf[0] = '\0';
            *p = 0;
        } else if (key.code == 127 || key.code == 8) {
            if (*p > 0 && f->buf) {
                memmove(f->buf + *p - 1, f->buf + *p, (size_t)(len3 - *p + 1));
                (*p)--;
            }
        } else if (key.code == PHOTON_KEY_DEL) {
            if (*p < len3 && f->buf) {
                memmove(f->buf + *p, f->buf + *p + 1, (size_t)(len3 - *p));
            }
        } else if (key.code >= ' ' && key.code <= 255 && f->buf) {
            /* Printable character */
            bool ok2 = true;
            if (f->type == PHOTON_FIELD_NUMBER && (key.code < '0' || key.code > '9'))
                ok2 = false;
            if (ok2 && len3 < cap) {
                memmove(f->buf + *p + 1, f->buf + *p, (size_t)(len3 - *p + 1));
                f->buf[*p] = (char)key.code;
                (*p)++;
            }
        }

        /* Scroll view */
        if (f->buf) {
            int ln = (int)strlen(f->buf);
            if (*p < *v) *v = *p;
            if (*p > *v + value_w - 1) *v = *p - value_w + 1;
            if (*v < 0) *v = 0;
            (void)ln;
        }
    }

    free(pos);
    free(view);
    photon_ui_restore_screen(ui, saved);
    photon_ui_free_screen(saved);
    return result;
}

/* ── Message popup ──────────────────────────────────────────────────────── */

void photon_ui_msg(photon_ui_t *ui, const char *message)
{
    if (!ui || !message) return;

    int msg_len = (int)strlen(message);
    int box_w   = msg_len + 4;
    if (box_w < 20) box_w = 20;
    if (box_w > vte_cols(ui->vte) - 2) box_w = vte_cols(ui->vte) - 2;

    int box_h   = 3;
    int col1, row1;
    center_box(ui, box_w, box_h, &col1, &row1);
    int col2 = col1 + box_w - 1;
    int row2 = row1 + box_h - 1;

    photon_ui_screen_t *saved = photon_ui_save_screen(ui);
    const photon_ui_colors_t *cl = &ui->colors;

    /* Repaint background from saved screen before drawing dialog */
    repaint_saved(ui, saved);

    ui_draw_box(ui, col1, row1, col2, row2, NULL);

    /* Center message text */
    int avail  = col2 - col1 - 2;
    char tmp[256];
    snprintf(tmp, sizeof(tmp), "%-*.*s", avail, avail, message);
    ui_put_str(ui, col1 + 2, row1 + 1, tmp, cl->normal_fg, cl->normal_bg);

    photon_sdl_present(ui->sdl);

    /* Wait for any key */
    photon_key_t key;
    for (;;) {
        if (photon_sdl_wait_key(ui->sdl, &key, 100)) {
            if (key.code != 0) break;
        }
        if (photon_sdl_quit_requested(ui->sdl)) break;
    }

    photon_ui_restore_screen(ui, saved);
    photon_ui_free_screen(saved);
}

/* ── Scrollable text viewer ─────────────────────────────────────────────── */

/* Split text into lines (caller frees lines[]) */
static int split_lines(const char *text, char ***lines_out)
{
    int cap = 64, n = 0;
    char **lines = malloc((size_t)cap * sizeof(char *));
    if (!lines) return 0;

    const char *p = text;
    while (*p) {
        const char *end = strchr(p, '\n');
        size_t len = end ? (size_t)(end - p) : strlen(p);
        if (n >= cap) {
            cap *= 2;
            char **tmp = realloc(lines, (size_t)cap * sizeof(char *));
            if (!tmp) break;
            lines = tmp;
        }
        lines[n] = malloc(len + 1);
        if (lines[n]) {
            memcpy(lines[n], p, len);
            lines[n][len] = '\0';
        }
        n++;
        if (!end) break;
        p = end + 1;
    }
    *lines_out = lines;
    return n;
}

void photon_ui_showbuf(photon_ui_t *ui,
                       const char *title,
                       const char *text,
                       int max_cols,
                       int max_rows)
{
    if (!ui || !text) return;
    if (max_cols <= 0) max_cols = 78;
    if (max_rows <= 0) max_rows = 20;
    if (max_cols > vte_cols(ui->vte) - 2) max_cols = vte_cols(ui->vte) - 2;
    if (max_rows > vte_rows(ui->vte) - 2) max_rows = vte_rows(ui->vte) - 2;

    char **lines = NULL;
    int   n_lines = split_lines(text, &lines);

    int vis_rows = n_lines < max_rows - 2 ? n_lines : max_rows - 2;
    int box_h    = vis_rows + 2;
    int box_w    = max_cols;
    int col1, row1;
    center_box(ui, box_w, box_h, &col1, &row1);
    int col2     = col1 + box_w - 1;
    int row2     = row1 + box_h - 1;
    int inner_w  = col2 - col1 - 2;

    photon_ui_screen_t *saved = photon_ui_save_screen(ui);
    const photon_ui_colors_t *cl = &ui->colors;
    int scroll = 0;

    for (;;) {
        repaint_saved(ui, saved);
        ui_draw_box(ui, col1, row1, col2, row2, title);

        for (int i = 0; i < vis_rows; i++) {
            int idx  = scroll + i;
            int row  = row1 + 1 + i;
            /* Clear line */
            ui_fill_rect(ui, col1+1, row, col2-1, row, cl->normal_fg, cl->normal_bg);
            if (idx < n_lines && lines[idx]) {
                char tmp[256];
                snprintf(tmp, sizeof(tmp), "%-*.*s", inner_w, inner_w, lines[idx]);
                ui_put_str(ui, col1 + 2, row, tmp, cl->normal_fg, cl->normal_bg);
            }
        }

        /* Scroll indicators */
        if (scroll > 0)
            ui_put_cell(ui, col2-1, row1+1, 0x25B2,
                        cl->border_fg, cl->border_bg, 0);
        if (scroll + vis_rows < n_lines)
            ui_put_cell(ui, col2-1, row2-1, 0x25BC,
                        cl->border_fg, cl->border_bg, 0);

        /* Status line at bottom of box */
        {
            char status[64];
            snprintf(status, sizeof(status), " [%d/%d] PgUp/PgDn ESC=close ",
                     scroll + 1, n_lines);
            int slen = (int)strlen(status);
            int scol = col1 + (box_w - slen) / 2;
            if (scol < col1+1) scol = col1+1;
            ui_put_str(ui, scol, row2, status, cl->border_fg, cl->border_bg);
        }

        photon_sdl_present(ui->sdl);

        photon_key_t key = {0};
        if (!photon_sdl_wait_key(ui->sdl, &key, 100)) continue;
        if (key.code == 0) continue;

        switch (key.code) {
        case PHOTON_KEY_UP:
            if (scroll > 0) scroll--;
            break;
        case PHOTON_KEY_DOWN:
            if (scroll + vis_rows < n_lines) scroll++;
            break;
        case PHOTON_KEY_PGUP:
            scroll -= vis_rows;
            if (scroll < 0) scroll = 0;
            break;
        case PHOTON_KEY_PGDN:
            scroll += vis_rows;
            if (scroll + vis_rows > n_lines) scroll = n_lines - vis_rows;
            if (scroll < 0) scroll = 0;
            break;
        case PHOTON_KEY_HOME: scroll = 0; break;
        case PHOTON_KEY_END:
            scroll = n_lines - vis_rows;
            if (scroll < 0) scroll = 0;
            break;
        case 27:
        case PHOTON_KEY_QUIT:
        case 'q':
        case 'Q':
            goto done;
        default:
            break;
        }
    }

done:
    photon_ui_restore_screen(ui, saved);
    photon_ui_free_screen(saved);
    for (int i = 0; i < n_lines; i++) free(lines[i]);
    free(lines);
}

/* ── Yes/No confirm ─────────────────────────────────────────────────────── */

bool photon_ui_confirm(photon_ui_t *ui, const char *question)
{
    const char *items[] = { "Yes", "No", NULL };
    int cur = 0;
    return photon_ui_list(ui, question, items, 2, &cur) == 0;
}

/* ── Status toast (pop) ─────────────────────────────────────────────────── */

/* Max width of a toast message */
#define TOAST_MAX 128

typedef struct {
    photon_ui_screen_t *saved;    /* screen region saved before drawing      */
    int                 col1, row1, col2, row2;   /* saved region             */
    char                msg[TOAST_MAX];
    bool                active;
} photon_ui_toast_t;

/* We store the toast state inside the ui context but declared as a
 * static in this TU to avoid extending the struct ABI. */
static photon_ui_toast_t g_toast;

void photon_ui_pop(photon_ui_t *ui, const char *msg)
{
    /* Dismiss previous toast */
    if (g_toast.active) {
        /* Restore saved cells */
        if (g_toast.saved) {
            photon_ui_restore_screen(ui, g_toast.saved);
            photon_ui_free_screen(g_toast.saved);
            g_toast.saved = NULL;
        }
        g_toast.active = false;
    }

    if (!msg || !*msg) {
        photon_sdl_present(ui->sdl);
        return;
    }

    /* Compute toast geometry */
    int msg_len = (int)strlen(msg);
    if (msg_len > TOAST_MAX - 1) msg_len = TOAST_MAX - 1;

    int toast_w = msg_len + 6;  /* "[ msg ]" */
    if (toast_w > vte_cols(ui->vte)) toast_w = vte_cols(ui->vte);

    int tcols = vte_cols(ui->vte);
    int trows = vte_rows(ui->vte);

    int col1 = (tcols - toast_w) / 2 + 1;
    int row1 = trows / 2;
    int col2 = col1 + toast_w - 1;
    int row2 = row1 + 2;  /* 3 rows tall */

    if (col1 < 1) col1 = 1;
    if (row1 < 1) row1 = 1;

    /* Save the region we're about to overwrite */
    /* For simplicity, save the full screen and record position */
    g_toast.saved = photon_ui_save_screen(ui);
    g_toast.col1  = col1;
    g_toast.row1  = row1;
    g_toast.col2  = col2;
    g_toast.row2  = row2;
    snprintf(g_toast.msg, sizeof(g_toast.msg), "%s", msg);
    g_toast.active = true;

    const photon_ui_colors_t *cl = &ui->colors;
    uint8_t fg = cl->title_fg;
    uint8_t bg = cl->border_bg;

    /* Draw toast: top border */
    ui_put_cell(ui, col1, row1, BOX_TL, fg, bg, 0);
    for (int c = col1+1; c < col2; c++)
        ui_put_cell(ui, c, row1, BOX_H, fg, bg, 0);
    ui_put_cell(ui, col2, row1, BOX_TR, fg, bg, 0);

    /* Middle row with message */
    ui_put_cell(ui, col1, row1+1, BOX_V, fg, bg, 0);
    ui_fill_rect(ui, col1+1, row1+1, col2-1, row1+1, fg, bg);
    char tmp[TOAST_MAX + 4];
    int avail = col2 - col1 - 2;
    snprintf(tmp, sizeof(tmp), "%-*.*s", avail, avail, msg);
    ui_put_str(ui, col1+1, row1+1, tmp, fg | 0x08 /* bright */, bg);
    ui_put_cell(ui, col2, row1+1, BOX_V, fg, bg, 0);

    /* Bottom border */
    ui_put_cell(ui, col1, row2, BOX_BL, fg, bg, 0);
    for (int c = col1+1; c < col2; c++)
        ui_put_cell(ui, c, row2, BOX_H, fg, bg, 0);
    ui_put_cell(ui, col2, row2, BOX_BR, fg, bg, 0);

    photon_sdl_present(ui->sdl);
}
