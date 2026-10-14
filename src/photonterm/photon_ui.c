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

/* ── Saved screen ──────────────────────────────────────────────────────── */

struct photon_ui_screen {
    int        cols, rows;
    vte_cell_t cells[];   /* flexible array: cols*rows */
};

/* ── Context ───────────────────────────────────────────────────────────── */

struct photon_ui {
    photon_sdl_t       *sdl;
    vte_t              *vte;
};

/* Global handle set by photonterm.c */
photon_ui_t *photon_ui_global = NULL;

photon_ui_t *photon_ui_create(photon_sdl_t *sdl, vte_t *vte)
{
    photon_ui_t *ui = calloc(1, sizeof(*ui));
    if (!ui) return NULL;
    ui->sdl    = sdl;
    ui->vte    = vte;
    return ui;
}

void photon_ui_free(photon_ui_t *ui)
{
    free(ui);
}

photon_sdl_t *photon_ui_sdl(photon_ui_t *ui) { return ui ? ui->sdl : NULL; }
vte_t        *photon_ui_vte(photon_ui_t *ui) { return ui ? ui->vte : NULL; }


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

    /* Bulk copy from shadow buffer (fast path) */
    const vte_cell_t *shadow = photon_sdl_shadow_ptr(ui->sdl);
    int shadow_cols = photon_sdl_shadow_cols(ui->sdl);
    int shadow_rows = photon_sdl_shadow_rows(ui->sdl);
    if (shadow && shadow_cols == cols && shadow_rows == rows) {
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
    int cols = s->cols < photon_menu_cols(ui) ? s->cols : photon_menu_cols(ui);
    int rows = s->rows < photon_menu_rows(ui) ? s->rows : photon_menu_rows(ui);
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

/* ── Resize helper for modal dialogs ──────────────────────────────────────── */

/* Check for a pending window resize and, if one occurred, update the SDL
 * texture/shadow, resize the UI VTE to match, repaint the terminal, and
 * re-save the screen at the new dimensions.  Returns true if a resize
 * happened so the caller knows to redraw its dialog. */
static bool ui_check_resize(photon_ui_t *ui, photon_ui_screen_t **saved)
{
    int nc = 0, nr = 0;
    if (!photon_sdl_check_resize(ui->sdl, &nc, &nr))
        return false;
    if (*saved) {
        vte_resize(photon_ui_vte(ui), nc, nr);
        photon_sdl_repaint(ui->sdl, photon_ui_vte(ui));
        photon_sdl_present(ui->sdl);
        photon_ui_free_screen(*saved);
        *saved = photon_ui_save_screen(ui);
    }
    return true;
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
    int W = photon_menu_cols(ui);
    int H = photon_menu_rows(ui);

    /* Layout: title bar (row 1), items (rows 2..H-2), hints (row H-1), status (row H) */
    int list_top = 2;
    int hint_row = H - 1;
    int list_bot = hint_row - 1;
    int vis_rows = list_bot - list_top + 1;
    if (vis_rows < 1) vis_rows = 1;

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

        photon_key_t key = {0};
        if (!photon_sdl_wait_key(ui->sdl, &key, 100)) {
            if (ui_check_resize(ui, &saved))
                redraw = true;
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
            if (ui_check_resize(ui, &saved))
                redraw = true;
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

    /* Wait for any key, handling window resize while the popup is open */
    photon_key_t key;
    for (;;) {
        if (photon_sdl_wait_key(ui->sdl, &key, 100)) {
            if (key.code != 0) break;
        }
        if (photon_sdl_quit_requested(ui->sdl)) break;
        int rnc = 0, rnr = 0;
        if (photon_sdl_check_resize(ui->sdl, &rnc, &rnr)) {
            vte_resize(photon_ui_vte(ui), rnc, rnr);
            photon_sdl_repaint(ui->sdl, photon_ui_vte(ui));
            photon_sdl_present(ui->sdl);
            photon_ui_free_screen(saved);
            saved = photon_ui_save_screen(ui);
            /* Redraw the message at the new size */
            W = photon_menu_cols(ui);
            H = photon_menu_rows(ui);
            photon_menu_fill_rect(ui, 1, 1, W, H, PHOTON_MENU_A_NORM(t));
            photon_menu_draw_titlebar(ui, "PhotonTERM");
            photon_menu_draw_statusbar(ui);
            int avail = W - 4;
            if (avail < 10) avail = 10;
            int col = (W - (msg_len < avail ? msg_len : avail)) / 2;
            if (col < 2) col = 2;
            int row = H / 2;
            char tmp[512];
            snprintf(tmp, sizeof(tmp), "%-*.*s", avail, avail, message);
            photon_menu_put_str(ui, col, row, tmp, PHOTON_MENU_A_NORM(t));
            photon_menu_draw_hints(ui, H - 1, hints, 1);
            photon_sdl_present(ui->sdl);
        }
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
    int W = photon_menu_cols(ui);
    int H = photon_menu_rows(ui);

    char **lines = NULL;
    int   n_lines = split_lines(text, &lines);

    /* Layout: title bar (row 1), text (rows 2..H-2), hints (row H-1), status (row H) */
    int text_top = 2;
    int hint_row = H - 1;
    int text_bot = hint_row - 1;
    int vis_rows = text_bot - text_top + 1;
    if (vis_rows < 1) vis_rows = 1;
    int inner_w = W - 2;

    photon_ui_screen_t *saved = photon_ui_save_screen(ui);

    /* Push theme palette so dialog colors match the UI theme */
    uint8_t pal_buf[768];
    photon_theme_push_palette(ui->sdl, pal_buf);

    int scroll = 0;
    bool redraw = true;

    for (;;) {
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
            if (ui_check_resize(ui, &saved))
                redraw = true;
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
