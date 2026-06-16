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
#include "photon_settings.h"
#include "photon_menu.h"

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
    int        cell_w, cell_h;  /* cell dimensions at save time */
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

/* ── Grid dimension helpers ────────────────────────────────────────────── */

/* Grid dimensions: prefer SDL (live, reflects fullscreen) over VTE */
static int ui_grid_cols(const photon_ui_t *ui)
{
    return (ui->sdl && photon_sdl_cols(ui->sdl) > 0)
           ? photon_sdl_cols(ui->sdl) : vte_cols(ui->vte);
}
static int ui_grid_rows(const photon_ui_t *ui)
{
    return (ui->sdl && photon_sdl_rows(ui->sdl) > 0)
           ? photon_sdl_rows(ui->sdl) : vte_rows(ui->vte);
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
    s->cell_w = photon_sdl_cell_width(ui->sdl);
    s->cell_h = photon_sdl_cell_height(ui->sdl);

    /* Bulk copy from shadow buffer (fast path) */
    const vte_cell_t *shadow = photon_sdl_shadow_ptr(ui->sdl);
    int shadow_cols = photon_sdl_shadow_cols(ui->sdl);
    if (shadow && shadow_cols == cols) {
        memcpy(s->cells, shadow, n * sizeof(vte_cell_t));
    } else {
        for (int r = 1; r <= rows; r++)
            for (int c = 1; c <= cols; c++) {
                vte_cell_t cell = {0};
                photon_sdl_get_cell(ui->sdl, c, r, &cell);
                s->cells[(r - 1) * cols + (c - 1)] = cell;
            }
    }
    return s;
}

void photon_ui_restore_screen(photon_ui_t *ui, photon_ui_screen_t *s)
{
    if (!ui || !s) return;
    /* If grid or cell dimensions changed (e.g. font resize), clear and skip
     * restore to avoid drawing stale content at wrong scale */
    if (s->cols != ui_grid_cols(ui) || s->rows != ui_grid_rows(ui) ||
        s->cell_w != photon_sdl_cell_width(ui->sdl) ||
        s->cell_h != photon_sdl_cell_height(ui->sdl)) {
        photon_sdl_clear(ui->sdl);
        photon_sdl_present(ui->sdl);
        return;
    }
    int cols = s->cols < ui_grid_cols(ui) ? s->cols : ui_grid_cols(ui);
    int rows = s->rows < ui_grid_rows(ui) ? s->rows : ui_grid_rows(ui);
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
    photon_sdl_fill_rect(ui->sdl, col1, row1, col2, row2, fg, bg);
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

    const photon_theme_t *t = photon_menu_theme();

    /* Save screen */
    photon_ui_screen_t *saved = photon_ui_save_screen(ui);

    /* Push theme palette so dialog colors match the UI theme */
    uint8_t pal_buf[768];
    photon_theme_push_palette(ui->sdl, pal_buf);

    int sel    = cur ? *cur : 0;
    int scroll = 0;

    if (sel < 0) sel = 0;
    if (sel >= n_items) sel = n_items - 1;

    bool redraw = true;

    for (;;) {
        /* Recompute layout on each redraw so font resize takes effect */
        int W = photon_menu_cols(ui);
        int H = photon_menu_rows(ui);
        int list_top = 2;
        int hint_row = H - 1;
        int list_bot = hint_row - 1;
        int vis_rows = list_bot - list_top + 1;
        if (vis_rows < 1) vis_rows = 1;

        /* Adjust scroll so sel is visible */
        if (sel < scroll) scroll = sel;
        if (sel >= scroll + vis_rows) scroll = sel - vis_rows + 1;

        if (redraw) {
        /* Full-screen layout */
        photon_menu_fill_rect(ui, 1, 1, W, H, PHOTON_MENU_A_NORM(t));
        photon_menu_draw_titlebar(ui, title ? title : "Select");
        photon_menu_draw_statusbar(ui);

        /* Draw items */
        for (int i = 0; i < vis_rows; i++) {
            int idx = scroll + i;
            int row = list_top + i;

            if (idx >= n_items) {
                photon_menu_fill_rect(ui, 1, row, W, row, PHOTON_MENU_A_NORM(t));
                continue;
            }

            uint8_t attr = (idx == sel)
                           ? PHOTON_MENU_A_SEL(t)
                           : PHOTON_MENU_A_NORM(t);

            /* Truncate item to screen width */
            char tmp[512];
            int avail = W - 2;
            if (avail > (int)sizeof(tmp) - 1) avail = (int)sizeof(tmp) - 1;
            snprintf(tmp, sizeof(tmp), "%-*.*s", avail, avail, items[idx]);
            photon_menu_put_str(ui, 2, row, tmp, attr);
        }

        /* Scroll indicators */
        if (scroll > 0)
            photon_menu_put_cell(ui, W, list_top, 0x25B2 /* ▲ */,
                                PHOTON_MENU_A_DIM(t));
        if (scroll + vis_rows < n_items)
            photon_menu_put_cell(ui, W, list_bot, 0x25BC /* ▼ */,
                                PHOTON_MENU_A_DIM(t));

        /* Hints */
        static const photon_hint_t hints[] = {
            { "Enter", " Select  " },
            { "Esc", " Cancel" },
        };
        photon_menu_draw_hints(ui, hint_row, hints, 2);

        photon_sdl_present(ui->sdl);
        redraw = false;
        }

        /* Wait for key */
        photon_key_t key = {0};
        if (!photon_sdl_wait_key(ui->sdl, &key, 100)) {
            if (photon_sdl_take_expose(ui->sdl))
                redraw = true;
            continue;
        }
        if (key.code == 0) continue;

        switch (key.code) {
        case PHOTON_KEY_UP:
            if (sel > 0) { sel--; redraw = true; }
            break;
        case PHOTON_KEY_DOWN:
            if (sel < n_items - 1) { sel++; redraw = true; }
            break;
        case PHOTON_KEY_PGUP:
            sel -= vis_rows - 1;
            if (sel < 0) sel = 0;
            redraw = true;
            break;
        case PHOTON_KEY_PGDN:
            sel += vis_rows - 1;
            if (sel >= n_items) sel = n_items - 1;
            redraw = true;
            break;
        case PHOTON_KEY_HOME:
            sel = 0;
            redraw = true;
            break;
        case PHOTON_KEY_END:
            sel = n_items - 1;
            redraw = true;
            break;
        case '\r':
           if (cur) *cur = sel;
            photon_theme_pop_palette(ui->sdl, pal_buf);
            photon_ui_restore_screen(ui, saved);
            photon_ui_free_screen(saved);
            return sel;
        case 27:  /* ESC */
        case PHOTON_KEY_QUIT:
            photon_theme_pop_palette(ui->sdl, pal_buf);
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
                        redraw = true;
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

    const photon_theme_t *t = photon_menu_theme();
    int W = photon_menu_cols(ui);
    int H = photon_menu_rows(ui);

    int max_field = W - 4;
    if (max_field > buflen - 1) max_field = buflen - 1;
    if (max_field < 10) max_field = 10;

    /* Layout: title bar (row 1), input field (row H/2), hints (row H-1), status (row H) */
    int field_row = H / 2;
    int field_col = 2;

    photon_ui_screen_t *saved = (flags & PHOTON_INPUT_NOSAVE)
                                ? NULL
                                : photon_ui_save_screen(ui);

    /* Push theme palette so dialog colors match the UI theme */
    uint8_t pal_buf[768];
    photon_theme_push_palette(ui->sdl, pal_buf);

    int pos   = (int)strlen(buf);
    int view  = 0;  /* scroll offset within buf */
    bool redraw = true;

    for (;;) {
        /* Keep cursor visible */
        if (pos < view) view = pos;
        if (pos > view + max_field - 1) view = pos - max_field + 1;

        if (redraw) {
        /* Full-screen layout */
        photon_menu_fill_rect(ui, 1, 1, W, H, PHOTON_MENU_A_NORM(t));
        photon_menu_draw_titlebar(ui, title ? title : "Input");
        photon_menu_draw_statusbar(ui);

        /* Draw field background */
        photon_menu_fill_rect(ui, field_col, field_row,
                              field_col + max_field + 1, field_row,
                              PHOTON_MENU_ATTR(CGA_BLACK, t->hclr));

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
            photon_menu_put_cell(ui, field_col + i, field_row, cp,
                                 PHOTON_MENU_ATTR(CGA_BLACK, t->hclr));
        }

        /* Cursor indicator: inverted at cursor pos */
        int cursor_screen_col = field_col + (pos - view);
        uint32_t cursor_cp = (pos < len)
            ? ((flags & PHOTON_INPUT_PASSWORD) ? '*' : (unsigned char)buf[pos])
            : ' ';
        photon_menu_put_cell(ui, cursor_screen_col, field_row, cursor_cp,
                             PHOTON_MENU_ATTR(t->hclr, CGA_BLACK));

        /* Hints */
        static const photon_hint_t hints[] = {
            { "Enter", " Accept  " },
            { "Esc", " Cancel" },
        };
        photon_menu_draw_hints(ui, H - 1, hints, 2);

        photon_sdl_present(ui->sdl);
        redraw = false;
        }

        photon_key_t key = {0};
        if (!photon_sdl_wait_key(ui->sdl, &key, 100)) {
            if (photon_sdl_take_expose(ui->sdl))
                redraw = true;
            continue;
        }
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
            photon_theme_pop_palette(ui->sdl, pal_buf);
            photon_ui_restore_screen(ui, saved);
            photon_ui_free_screen(saved);
            return ret;
        }

        if (key.code == 27 || key.code == PHOTON_KEY_QUIT) {
            photon_theme_pop_palette(ui->sdl, pal_buf);
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
            redraw = true;
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
        redraw = true;
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

    const photon_theme_t *t = photon_menu_theme();
    int W = photon_menu_cols(ui);
    int H = photon_menu_rows(ui);

    /* Layout: title bar (row 1), fields (rows 2..H-2), hints (row H-1), status (row H) */
    int label_w = 0;
    for (int i = 0; i < n_fields; i++) {
        int lw = fields[i].label ? (int)strlen(fields[i].label) : 0;
        if (lw > label_w) label_w = lw;
    }
    if (label_w < 8) label_w = 8;

    int value_w = W - label_w - 6;  /* label + ": " + value + margins */
    if (value_w < 8) value_w = 8;
    if (value_w > 60) value_w = 60;

    int val_col = 2 + label_w + 2;
    int field_r1 = 2;
    int hint_row = H - 1;

    /* Per-field edit state: cursor position and view offset */
    int *pos  = calloc((size_t)n_fields, sizeof(int));
    int *view = calloc((size_t)n_fields, sizeof(int));
    if (!pos || !view) { free(pos); free(view); return false; }

    /* Initialise cursors at end of pre-filled text */
    for (int i = 0; i < n_fields; i++)
        pos[i] = fields[i].buf ? (int)strlen(fields[i].buf) : 0;

    photon_ui_screen_t *saved = photon_ui_save_screen(ui);

    int active = 0;
    /* Push theme palette so dialog colors match the UI theme */
    uint8_t pal_buf[768];
    photon_theme_push_palette(ui->sdl, pal_buf);

    bool result = false;
    bool redraw = true;

    for (;;) {
        if (redraw) {
        /* Full-screen layout */
        photon_menu_fill_rect(ui, 1, 1, W, H, PHOTON_MENU_A_NORM(t));
        photon_menu_draw_titlebar(ui, title ? title : "Form");
        photon_menu_draw_statusbar(ui);

        /* Draw all fields */
        for (int i = 0; i < n_fields; i++) {
            bool act = (i == active);
            int row = field_r1 + i;
            if (row >= hint_row) break;  /* no room */

            uint8_t norm_attr = PHOTON_MENU_A_NORM(t);
            uint8_t sel_attr  = PHOTON_MENU_A_SEL(t);

            /* Label */
            char lbuf[64];
            snprintf(lbuf, sizeof(lbuf), "%-*s:", label_w, fields[i].label ? fields[i].label : "");
            photon_menu_put_str(ui, 2, row, lbuf, norm_attr);

            /* Value area background */
            photon_menu_fill_rect(ui, val_col, row, val_col + value_w - 1, row,
                                  act ? sel_attr : norm_attr);

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
                photon_menu_put_padded(ui, val_col, row, cv, value_w,
                                       act ? sel_attr : norm_attr);
            } else {
                /* Text/number/password: scrolled view */
                int v = act ? view[i] : 0;
                if (!act && vlen > value_w) v = 0;
                for (int c = 0; c < value_w; c++) {
                    int ci = v + c;
                    char ch = (ci < vlen) ? buf[ci] : ' ';
                    if (is_pw && ci < vlen) ch = '*';
                    photon_menu_put_cell(ui, val_col + c, row, (unsigned char)ch,
                                         act ? sel_attr : norm_attr);
                }
            }
        }

        /* Draw cursor in active text field */
        {
            int i = active;
            int row = field_r1 + i;
            if (row < hint_row && fields[i].type != PHOTON_FIELD_CHOICE) {
                int v = view[i];
                int p = pos[i];
                int cx = val_col + (p - v);
                if (cx >= val_col && cx < val_col + value_w) {
                    const char *buf = fields[i].buf ? fields[i].buf : "";
                    int vlen = (int)strlen(buf);
                    char ch = (p < vlen) ? buf[p] : ' ';
                    if (fields[i].type == PHOTON_FIELD_PASSWORD && p < vlen) ch = '*';
                    /* Inverted cursor */
                    photon_menu_put_cell(ui, cx, row, (unsigned char)ch,
                                         PHOTON_MENU_ATTR(t->lbbclr, t->lbclr));
                }
            }
        }

        /* Hints */
        static const photon_hint_t hints[] = {
            { "Tab", " Next  " },
            { "Enter", " Save  " },
            { "Esc", " Cancel" },
        };
        photon_menu_draw_hints(ui, hint_row, hints, 3);

        photon_sdl_present(ui->sdl);
        redraw = false;
        }

        photon_key_t key = {0};
        if (!photon_sdl_wait_key(ui->sdl, &key, 100)) {
            if (photon_sdl_take_expose(ui->sdl))
                redraw = true;
            continue;
        }
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
            redraw = true;
            continue;
        }

        /* Tab or Down - next field */
        if (key.code == '\t' || key.code == PHOTON_KEY_DOWN) {
            active = (active + 1) % n_fields;
            redraw = true;
            continue;
        }

        /* Enter: advance to next field, or submit from last field */
        if (key.code == '\r') {
            if (active < n_fields - 1) {
                active++;
                redraw = true;
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
            redraw = true;
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
            if (*p < *v) *v = *p;
            if (*p > *v + value_w - 1) *v = *p - value_w + 1;
            if (*v < 0) *v = 0;
        }
        redraw = true;
    }

    free(pos);
    free(view);
    photon_theme_pop_palette(ui->sdl, pal_buf);
    photon_ui_restore_screen(ui, saved);
    photon_ui_free_screen(saved);
    return result;
}

/* ── Message popup ──────────────────────────────────────────────────────── */

void photon_ui_msg(photon_ui_t *ui, const char *message)
{
    if (!ui || !message) return;

    const photon_theme_t *t = photon_menu_theme();
    int W = photon_menu_cols(ui);
    int H = photon_menu_rows(ui);

    photon_ui_screen_t *saved = photon_ui_save_screen(ui);

    /* Push theme palette so dialog colors match the UI theme */
    uint8_t pal_buf[768];
    photon_theme_push_palette(ui->sdl, pal_buf);

    /* Full-screen layout: title bar, centered message, hint bar, status bar */
    photon_menu_fill_rect(ui, 1, 1, W, H, PHOTON_MENU_A_NORM(t));
    photon_menu_draw_titlebar(ui, "PhotonTERM");
    photon_menu_draw_statusbar(ui);

    /* Center message text in the middle of the screen */
    int msg_len = (int)strlen(message);
    int avail = W - 4;
    if (avail < 10) avail = 10;
    int col = (W - (msg_len < avail ? msg_len : avail)) / 2;
    if (col < 2) col = 2;
    int row = H / 2;
    char tmp[512];
    snprintf(tmp, sizeof(tmp), "%-*.*s", avail, avail, message);
    photon_menu_put_str(ui, col, row, tmp, PHOTON_MENU_A_NORM(t));

    /* Hint bar */
    static const photon_hint_t hints[] = {
        { "Any key", " continue" },
    };
    photon_menu_draw_hints(ui, H - 1, hints, 1);

    photon_sdl_present(ui->sdl);

    /* Wait for any key */
    photon_key_t key;
    for (;;) {
        if (photon_sdl_wait_key(ui->sdl, &key, 100)) {
            if (key.code != 0) break;
        }
        if (photon_sdl_quit_requested(ui->sdl)) break;
    }

    photon_theme_pop_palette(ui->sdl, pal_buf);
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

    const photon_theme_t *t = photon_menu_theme();

    char **lines = NULL;
    int   n_lines = split_lines(text, &lines);

    photon_ui_screen_t *saved = photon_ui_save_screen(ui);

    /* Push theme palette so dialog colors match the UI theme */
    uint8_t pal_buf[768];
    photon_theme_push_palette(ui->sdl, pal_buf);

    int scroll = 0;
    bool redraw = true;

    for (;;) {
        /* Recompute layout on each redraw so font resize takes effect */
        int W = photon_menu_cols(ui);
        int H = photon_menu_rows(ui);
        int text_top = 2;
        int hint_row = H - 1;
        int text_bot = hint_row - 1;
        int vis_rows = text_bot - text_top + 1;
        if (vis_rows < 1) vis_rows = 1;
        int inner_w = W - 2;

        if (redraw) {
        /* Full-screen layout */
        photon_menu_fill_rect(ui, 1, 1, W, H, PHOTON_MENU_A_NORM(t));
        photon_menu_draw_titlebar(ui, title ? title : "Text");
        photon_menu_draw_statusbar(ui);

        for (int i = 0; i < vis_rows; i++) {
            int idx = scroll + i;
            int row = text_top + i;
            if (idx < n_lines && lines[idx]) {
                char tmp[512];
                snprintf(tmp, sizeof(tmp), "%-*.*s", inner_w, inner_w, lines[idx]);
                photon_menu_put_str(ui, 2, row, tmp, PHOTON_MENU_A_NORM(t));
            } else {
                photon_menu_fill_rect(ui, 1, row, W, row, PHOTON_MENU_A_NORM(t));
            }
        }

        /* Scroll indicators */
        if (scroll > 0)
            photon_menu_put_cell(ui, W, text_top, 0x25B2,
                                 PHOTON_MENU_A_DIM(t));
        if (scroll + vis_rows < n_lines)
            photon_menu_put_cell(ui, W, text_bot, 0x25BC,
                                 PHOTON_MENU_A_DIM(t));

        /* Hints */
        static const photon_hint_t hints[] = {
            { "PgUp/PgDn", " Scroll  " },
            { "Esc", " Close" },
        };
        photon_menu_draw_hints(ui, hint_row, hints, 2);

        photon_sdl_present(ui->sdl);
        redraw = false;
        }

        photon_key_t key = {0};
        if (!photon_sdl_wait_key(ui->sdl, &key, 100)) {
            if (photon_sdl_take_expose(ui->sdl))
                redraw = true;
            continue;
        }
        if (key.code == 0) continue;

        switch (key.code) {
        case PHOTON_KEY_UP:
            if (scroll > 0) { scroll--; redraw = true; }
            break;
        case PHOTON_KEY_DOWN:
            if (scroll + vis_rows < n_lines) { scroll++; redraw = true; }
            break;
        case PHOTON_KEY_PGUP:
            scroll -= vis_rows;
            if (scroll < 0) scroll = 0;
            redraw = true;
            break;
        case PHOTON_KEY_PGDN:
            scroll += vis_rows;
            if (scroll + vis_rows > n_lines) scroll = n_lines - vis_rows;
            if (scroll < 0) scroll = 0;
            redraw = true;
            break;
        case PHOTON_KEY_HOME: scroll = 0; redraw = true; break;
        case PHOTON_KEY_END:
            scroll = n_lines - vis_rows;
            if (scroll < 0) scroll = 0;
            redraw = true;
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
    photon_theme_pop_palette(ui->sdl, pal_buf);
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
    if (toast_w > ui_grid_cols(ui)) toast_w = ui_grid_cols(ui);

    int tcols = ui_grid_cols(ui);
    int trows = ui_grid_rows(ui);

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

    /* Push theme palette so toast colors are consistent */
    uint8_t pal_buf[768];
    photon_theme_push_palette(ui->sdl, pal_buf);

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

    photon_theme_pop_palette(ui->sdl, pal_buf);
    photon_sdl_present(ui->sdl);
}
