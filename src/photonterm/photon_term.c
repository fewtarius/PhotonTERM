/* photon_term.c - PhotonTERM terminal session (photon_vte + photon_sdl)
 *
 * Copyright (C) 2026 fewtarius and PhotonTERM contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Non-blocking pump/handle/render API for the unified main loop.
 * Each tab's data is drained independently; only the active tab renders.
 */

#include "photon_compat.h"
#include "photon_vte.h"
#include <SDL2/SDL.h>
#include "photon_sdl.h"
#include "photon_term.h"
#include "photon_conn.h"
#include "photon_ui.h"
#include "photon_menu.h"
#include "photon_bbslist.h"
#include "photon_settings.h"
#include "photon_xfer.h"

#define PHOTON_DEBUG_BUILD
#include "photon_debug.h"

#include <stdbool.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>

/* Tab switch target (0-based index), written before returning PHOTON_TERM_SWITCH_TAB */
int photon_switch_tab_target = 0;

/* Custom session menu callback (set by host apps like MIRA) */
static photon_session_menu_fn s_session_menu_fn = NULL;
static void *s_session_menu_userdata = NULL;

/* Render overlay callback (set by host apps for status indicators) */
static photon_render_overlay_fn s_render_overlay_fn = NULL;
static void *s_render_overlay_userdata = NULL;

/* Track whether the Alt-tab overlay was drawn last frame (to erase it) */
static bool alt_overlay_active = false;

/* Key hook callback (set by host apps to intercept keys before processing) */
static photon_key_hook_fn s_key_hook_fn = NULL;
static void *s_key_hook_userdata = NULL;

/* Frame timing state (persists across calls) */
static uint64_t s_last_render = 0;
static const uint64_t FRAME_MS = 16;   /* ~60 fps render cap */

/* Force next render to happen regardless of frame timer.
 * Set by tab switch, resize, and other UI events that need
 * immediate visual feedback. */
static bool s_force_render = false;

void photon_term_render_force_next(void)
{
    s_force_render = true;
}

void photon_term_set_session_menu(photon_session_menu_fn fn, void *userdata)
{
    s_session_menu_fn = fn;
    s_session_menu_userdata = userdata;
}

void photon_term_set_render_overlay(photon_render_overlay_fn fn, void *userdata)
{
    s_render_overlay_fn = fn;
    s_render_overlay_userdata = userdata;
}

void photon_term_set_key_hook(photon_key_hook_fn fn, void *userdata)
{
    s_key_hook_fn = fn;
    s_key_hook_userdata = userdata;
}

/* ── Alt-held tab bar overlay ────────────────────────────────────────── */

static void draw_alt_overlay(photon_sdl_t *sdl, const photon_tab_bar_t *tabbar)
{
    if (!sdl || !tabbar || tabbar->ntabs <= 1) return;

    int cols = photon_sdl_cols(sdl);
    int rows = photon_sdl_rows(sdl);
    int bar_row = rows;
    int col = 1;

    for (int i = 0; i < tabbar->ntabs && col <= cols; i++) {
        char label[72];
        snprintf(label, sizeof(label), " %d:%s%s ",
                 i + 1, tabbar->names[i],
                 tabbar->activity[i] ? "*" : "");

        bool is_active = (i == tabbar->active);
        uint8_t fg  = is_active ? 15 : 7;
        uint8_t bg  = is_active ?  4 : 0;
        uint8_t att = is_active ? VTE_ATTR_BOLD : 0;

        for (int j = 0; label[j] && col <= cols; j++, col++) {
            vte_cell_t cell = { (uint32_t)(unsigned char)label[j], fg, bg, att };
            photon_sdl_draw_cell(sdl, col, bar_row, &cell);
        }
        if (col <= cols) {
            vte_cell_t sep = { '|', 8, 0, 0 };
            photon_sdl_draw_cell(sdl, col++, bar_row, &sep);
        }
    }

    const char *hint = " W=New ";
    for (int j = 0; hint[j] && col <= cols; j++, col++) {
        vte_cell_t cell = { (uint32_t)(unsigned char)hint[j], 3, 0, 0 };
        photon_sdl_draw_cell(sdl, col, bar_row, &cell);
    }

    while (col <= cols) {
        vte_cell_t blank = { ' ', 7, 0, 0 };
        photon_sdl_draw_cell(sdl, col++, bar_row, &blank);
    }
}


#define SEQ_MAX 8

/* ── Mouse selection -> clipboard ───────────────────────────────────── */

static void copy_selection_to_clipboard(vte_t *vte, photon_sdl_t *sdl)
{
    int c0, r0, c1, r1;
    if (!photon_sdl_get_selection(sdl, &c0, &r0, &c1, &r1)) return;

    int cols = photon_sdl_cols(sdl);
    size_t bufsz = (size_t)(r1 - r0 + 1) * ((size_t)cols * 4 + 2) + 1;
    char *buf = malloc(bufsz);
    if (!buf) return;

    size_t pos = 0;
    for (int r = r0; r <= r1; r++) {
        int ca = (r == r0) ? c0 : 0;
        int cb = (r == r1) ? c1 : cols - 1;
        int last_nonsp = ca - 1;
        for (int c = ca; c <= cb; c++) {
            vte_cell_t cell;
            if (vte_get_cell(vte, c + 1, r + 1, &cell) && cell.codepoint > 0x20)
                last_nonsp = c;
        }
        for (int c = ca; c <= last_nonsp && pos + 5 < bufsz; c++) {
            vte_cell_t cell;
            Uint32 cp = ' ';
            if (vte_get_cell(vte, c + 1, r + 1, &cell) && cell.codepoint >= 0x20)
                cp = cell.codepoint;
            if (cp < 0x80) {
                buf[pos++] = (char)cp;
            } else if (cp < 0x800) {
                buf[pos++] = (char)(0xC0 | (cp >> 6));
                buf[pos++] = (char)(0x80 | (cp & 0x3F));
            } else if (cp < 0x10000) {
                buf[pos++] = (char)(0xE0 | (cp >> 12));
                buf[pos++] = (char)(0x80 | ((cp >> 6) & 0x3F));
                buf[pos++] = (char)(0x80 | (cp & 0x3F));
            } else {
                buf[pos++] = (char)(0xF0 | (cp >> 18));
                buf[pos++] = (char)(0x80 | ((cp >> 12) & 0x3F));
                buf[pos++] = (char)(0x80 | ((cp >> 6) & 0x3F));
                buf[pos++] = (char)(0x80 | (cp & 0x3F));
            }
        }
        if (r < r1 && pos + 1 < bufsz) buf[pos++] = '\n';
    }
    buf[pos] = '\0';
    if (pos > 0) SDL_SetClipboardText(buf);
    free(buf);
    photon_sdl_clear_selection(sdl);
}

static int key_to_bytes(const photon_key_t *k, uint8_t *out)
{
    if (k->text[0] && !(k->mod & PHOTON_MOD_CTRL) && !(k->mod & PHOTON_MOD_ALT)) {
        int len = (int)strnlen(k->text, sizeof(k->text));
        memcpy(out, k->text, (size_t)len);
        return len;
    }

    if (k->mod & PHOTON_MOD_CTRL) {
        if (k->code >= 1 && k->code <= 26) {
            out[0] = (uint8_t)k->code; return 1;
        }
        int base = -1;
        if (k->code >= 'a' && k->code <= 'z') base = k->code - 'a';
        if (k->code >= 'A' && k->code <= 'Z') base = k->code - 'A';
        if (base >= 0) { out[0] = (uint8_t)(base + 1); return 1; }
        if (k->code == '[') { out[0] = '\x1b'; return 1; }
    }

    switch (k->code) {
        case '\r': case '\n':
            out[0] = '\r'; return 1;
        case '\t':
            out[0] = '\t'; return 1;
        case '\x1b':
            out[0] = '\x1b'; return 1;
        case '\x7f':
            out[0] = '\x7f'; return 1;

        case PHOTON_KEY_UP:
            memcpy(out, "\x1b[A", 3); return 3;
        case PHOTON_KEY_DOWN:
            memcpy(out, "\x1b[B", 3); return 3;
        case PHOTON_KEY_RIGHT:
            memcpy(out, "\x1b[C", 3); return 3;
        case PHOTON_KEY_LEFT:
            memcpy(out, "\x1b[D", 3); return 3;

        case PHOTON_KEY_HOME:
            memcpy(out, "\x1b[H", 3); return 3;
        case PHOTON_KEY_END:
            memcpy(out, "\x1b[F", 3); return 3;
        case PHOTON_KEY_PGUP:
            memcpy(out, "\x1b[5~", 4); return 4;
        case PHOTON_KEY_PGDN:
            memcpy(out, "\x1b[6~", 4); return 4;
        case PHOTON_KEY_INS:
            memcpy(out, "\x1b[2~", 4); return 4;
        case PHOTON_KEY_DEL:
            memcpy(out, "\x1b[3~", 4); return 4;

        case PHOTON_KEY_F1:  memcpy(out, "\x1bOP",   3); return 3;
        case PHOTON_KEY_F2:  memcpy(out, "\x1bOQ",   3); return 3;
        case PHOTON_KEY_F3:  memcpy(out, "\x1bOR",   3); return 3;
        case PHOTON_KEY_F4:  memcpy(out, "\x1bOS",   3); return 3;
        case PHOTON_KEY_F5:  memcpy(out, "\x1b[15~", 5); return 5;
        case PHOTON_KEY_F6:  memcpy(out, "\x1b[17~", 5); return 5;
        case PHOTON_KEY_F7:  memcpy(out, "\x1b[18~", 5); return 5;
        case PHOTON_KEY_F8:  memcpy(out, "\x1b[19~", 5); return 5;
        case PHOTON_KEY_F9:  memcpy(out, "\x1b[20~", 5); return 5;
        case PHOTON_KEY_F10: memcpy(out, "\x1b[21~", 5); return 5;
        case PHOTON_KEY_F11: memcpy(out, "\x1b[23~", 5); return 5;
        case PHOTON_KEY_F12: memcpy(out, "\x1b[24~", 5); return 5;

        default: break;
    }

    if (k->code >= 32 && k->code <= 126) {
        out[0] = (uint8_t)k->code;
        return 1;
    }

    return 0;
}

/* ── Monotonic clock helper (ms) ─────────────────────────────────────── */

static uint64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)ts.tv_nsec / 1000000ULL;
}

/* ── In-session Alt-Z options menu ───────────────────────────────────── */

typedef enum {
    SESSION_MENU_NONE       = -1,
    SESSION_MENU_DISCONNECT =  0,
    SESSION_MENU_NEWTAB     =  1,
    SESSION_MENU_QUIT       =  2,
    SESSION_MENU_SETTINGS   =  3,
    SESSION_MENU_XFER       =  4,
} session_menu_result_t;

static void session_draw_item(photon_ui_t *ui,
                              void *items, int index,
                              int row, int col_start, int col_end,
                              bool selected,
                              const photon_theme_t *t)
{
    const char **labels = (const char **)items;
    int w = col_end - col_start + 1;
    uint8_t a = selected ? PHOTON_MENU_A_SEL(t) : PHOTON_MENU_A_NORM(t);

    photon_menu_fill_rect(ui, col_start, row, col_end, row, a);
    if (selected)
        photon_menu_put_cell(ui, col_start, row, 0x25B8, PHOTON_MENU_A_SEL_HI(t));
    else
        photon_menu_put_cell(ui, col_start, row, ' ', a);
    photon_menu_put_cell(ui, col_start + 1, row, ' ', a);
    photon_menu_put_padded(ui, col_start + 2, row, labels[index], w - 2, a);
}

static session_menu_result_t show_session_menu(photon_ui_t *ui,
                                               photon_sdl_t *sdl, vte_t *vte,
                                               const photon_bbs_t *bbs,
                                               photon_settings_t *settings)
{
    (void)settings;

    static const char *labels[] = {
        "Disconnect",
        "New Tab",
        "Exit PhotonTERM",
        "Settings",
        "File Transfer (ZModem/YModem/XModem)",
    };
    int nitems = 5;

    char title[128];
    snprintf(title, sizeof(title), "Session: %s",
             bbs && bbs->name[0] ? bbs->name : "Connected");

    static const photon_hint_t hints[] = {
        { "Return", " Select  " },
        { "Esc",    " Back" },
    };

    photon_menu_list_state_t state = { .cursor = 0, .scroll = 0 };

    bool redraw = true;
    int visible = 0;

    for (;;) {
        if (redraw) {
        photon_menu_list_t menu = {
            .title       = title,
            .items       = labels,
            .count       = nitems,
            .empty_msg   = NULL,
            .draw_item   = session_draw_item,
            .hints       = hints,
            .nhints      = sizeof(hints) / sizeof(hints[0]),
            .header_rows = 0,
            .draw_header = NULL,
        };

        visible = photon_menu_draw_list(ui, &menu, &state);
        photon_sdl_present(sdl);
        redraw = false;
        }

        photon_key_t key = {0};
        if (!photon_sdl_wait_key(sdl, &key, 100)) {
            if (photon_sdl_take_expose(sdl))
                redraw = true;
            continue;
        }
        if (key.code == 0) continue;

        if (key.code == '\x1b' || key.code == PHOTON_KEY_QUIT) {
            return SESSION_MENU_NONE;
        }

        if (photon_menu_list_handle_nav(&key, &state, nitems, visible)) {
            redraw = true;
            continue;
        }

        if (key.code == '\r' || key.code == ' ') {
            return (session_menu_result_t)state.cursor;
        }
    }
}

/* ── Scrollback viewer ───────────────────────────────────────────────── */

static bool sb_get_cell(vte_t *vte, int line, int col, int sb_lines,
                        int cols, vte_cell_t *row_buf, vte_cell_t *out)
{
    vte_cell_t blank = { ' ', VTE_COLOR_DEFAULT_FG, VTE_COLOR_DEFAULT_BG, 0 };
    if (col < 0 || col >= cols) { *out = blank; return false; }
    if (line < sb_lines) {
        if (!vte_scrollback_get(vte, line, row_buf, NULL)) { *out = blank; return false; }
        *out = row_buf[col];
        return true;
    }
    int screen_row = line - sb_lines;
    vte_cell_t cell;
    if (vte_get_cell(vte, col + 1, screen_row + 1, &cell)) {
        *out = cell;
        return true;
    }
    *out = blank;
    return false;
}

static void sb_copy_selection(vte_t *vte, photon_sdl_t *sdl,
                              int sc, int sr, int ec, int er,
                              int scroll_top, int sb_lines, int cols)
{
    if (sr > er || (sr == er && sc > ec)) {
        int t = sr; sr = er; er = t;
        t = sc; sc = ec; ec = t;
    }

    vte_cell_t *row_buf = calloc((size_t)cols, sizeof(vte_cell_t));
    if (!row_buf) return;

    size_t bufsz = (size_t)(er - sr + 1) * ((size_t)cols * 4 + 2) + 1;
    char *buf = malloc(bufsz);
    if (!buf) { free(row_buf); return; }

    size_t pos = 0;
    for (int r = sr; r <= er; r++) {
        int ca = (r == sr) ? sc : 0;
        int cb = (r == er) ? ec : cols - 1;
        int abs_line = scroll_top + r;

        int last_nonsp = ca - 1;
        for (int c = ca; c <= cb; c++) {
            vte_cell_t cell;
            if (sb_get_cell(vte, abs_line, c, sb_lines, cols, row_buf, &cell)
                && cell.codepoint > 0x20)
                last_nonsp = c;
        }
        for (int c = ca; c <= last_nonsp && pos + 5 < bufsz; c++) {
            vte_cell_t cell;
            uint32_t cp = ' ';
            if (sb_get_cell(vte, abs_line, c, sb_lines, cols, row_buf, &cell)
                && cell.codepoint >= 0x20)
                cp = cell.codepoint;
            if (cp < 0x80) {
                buf[pos++] = (char)cp;
            } else if (cp < 0x800) {
                buf[pos++] = (char)(0xC0 | (cp >> 6));
                buf[pos++] = (char)(0x80 | (cp & 0x3F));
            } else if (cp < 0x10000) {
                buf[pos++] = (char)(0xE0 | (cp >> 12));
                buf[pos++] = (char)(0x80 | ((cp >> 6) & 0x3F));
                buf[pos++] = (char)(0x80 | (cp & 0x3F));
            } else {
                buf[pos++] = (char)(0xF0 | (cp >> 18));
                buf[pos++] = (char)(0x80 | ((cp >> 12) & 0x3F));
                buf[pos++] = (char)(0x80 | ((cp >> 6) & 0x3F));
                buf[pos++] = (char)(0x80 | (cp & 0x3F));
            }
        }
        if (r < er && pos + 1 < bufsz) buf[pos++] = '\n';
    }
    buf[pos] = '\0';
    if (pos > 0) SDL_SetClipboardText(buf);
    free(buf);
    free(row_buf);
}

static void run_scrollback_viewer(vte_t *vte, photon_sdl_t *sdl)
{
    int sb_lines = vte_scrollback_lines(vte);
    int screen_rows = vte_rows(vte);
    int total = sb_lines + screen_rows;
    if (total <= 0) return;

    int cols = photon_sdl_cols(sdl);
    int rows = photon_sdl_rows(sdl);
    int cell_w = photon_sdl_cell_width(sdl);
    int cell_h = photon_sdl_cell_height(sdl);
    int visible = rows - 1;
    if (visible < 1) visible = 1;

    vte_cell_t *row_buf = calloc((size_t)cols, sizeof(vte_cell_t));
    if (!row_buf) return;

    int scroll_top = total - visible;
    if (scroll_top < 0) scroll_top = 0;
    scroll_top -= 3;
    if (scroll_top < 0) scroll_top = 0;

    bool redraw = true;
    bool done   = false;

    vte_cell_t sb_bar_bg = { ' ', 15, 4, VTE_ATTR_BOLD };
    vte_cell_t blank = { ' ', VTE_COLOR_DEFAULT_FG, VTE_COLOR_DEFAULT_BG, 0 };

    photon_sdl_clear_selection(sdl);

    while (!done) {
        if (redraw) {
            for (int r = 0; r < visible; r++) {
                int line = scroll_top + r;
                bool got = false;
                if (line < sb_lines) {
                    got = vte_scrollback_get(vte, line, row_buf, NULL);
                } else if (line < total) {
                    int screen_row = line - sb_lines;
                    for (int c = 0; c < cols; c++) {
                        vte_cell_t cell;
                        if (vte_get_cell(vte, c + 1, screen_row + 1, &cell))
                            row_buf[c] = cell;
                        else
                            row_buf[c] = blank;
                    }
                    got = true;
                }
                if (got) {
                    for (int c = 0; c < cols; c++)
                        photon_sdl_draw_cell(sdl, c + 1, r + 1, &row_buf[c]);
                } else {
                    for (int c = 0; c < cols; c++)
                        photon_sdl_draw_cell(sdl, c + 1, r + 1, &blank);
                }
            }

            char status[128];
            snprintf(status, sizeof(status),
                     " SCROLLBACK  %d/%d  Arrows  PgUp/Dn  Wheel  Home/End  ESC=exit",
                     scroll_top + 1, total);
            int slen = (int)strlen(status);
            for (int c = 0; c < cols; c++) {
                vte_cell_t cell = sb_bar_bg;
                if (c < slen) cell.codepoint = (unsigned char)status[c];
                photon_sdl_draw_cell(sdl, c + 1, rows, &cell);
            }
            photon_sdl_present(sdl);
            redraw = false;
        }

        SDL_Event ev;
        if (!SDL_WaitEventTimeout(&ev, 50)) {
            if (photon_sdl_sel_active(sdl) &&
                !(SDL_GetMouseState(NULL, NULL) & SDL_BUTTON(SDL_BUTTON_LEFT))) {
                photon_sdl_clear_selection(sdl);
                redraw = true;
            }
            continue;
        }
        if (ev.type == SDL_QUIT) { done = true; break; }

        int prev_top = scroll_top;

        if (ev.type == SDL_MOUSEWHEEL) {
            if (ev.wheel.y > 0)
                scroll_top -= 3;
            else if (ev.wheel.y < 0)
                scroll_top += 3;
            int max_top = total - visible;
            if (max_top < 0) max_top = 0;
            if (scroll_top < 0) scroll_top = 0;
            if (scroll_top > max_top) scroll_top = max_top;
            if (scroll_top != prev_top) redraw = true;
            continue;
        }

        if (ev.type == SDL_MOUSEBUTTONDOWN && ev.button.button == SDL_BUTTON_LEFT) {
            int mc = ev.button.x / cell_w;
            int mr = ev.button.y / cell_h;
            if (mc < 0) mc = 0;
            if (mr < 0) mr = 0;
            if (mc >= cols) mc = cols - 1;
            if (mr >= visible) mr = visible - 1;
            photon_sdl_start_selection(sdl, mc, mr);
            redraw = true;
            continue;
        }

        if (ev.type == SDL_MOUSEMOTION && photon_sdl_sel_active(sdl)) {
            int mc = ev.motion.x / cell_w;
            int mr = ev.motion.y / cell_h;
            if (mc < 0) mc = 0;
            if (mr < 0) mr = 0;
            if (mc >= cols) mc = cols - 1;
            if (mr >= visible) mr = visible - 1;
            photon_sdl_update_selection(sdl, mc, mr);
            redraw = true;
            continue;
        }

        if (ev.type == SDL_MOUSEBUTTONUP && ev.button.button == SDL_BUTTON_LEFT) {
            int mc = ev.button.x / cell_w;
            int mr = ev.button.y / cell_h;
            if (mc < 0) mc = 0;
            if (mr < 0) mr = 0;
            if (mc >= cols) mc = cols - 1;
            if (mr >= visible) mr = visible - 1;
            bool ok = photon_sdl_end_selection(sdl, mc, mr);
            if (ok) {
                int sc, sr, ec, er;
                if (photon_sdl_get_selection(sdl, &sc, &sr, &ec, &er)) {
                    sb_copy_selection(vte, sdl, sc, sr, ec, er,
                                      scroll_top, sb_lines, cols);
                }
            }
            redraw = true;
            continue;
        }

        if (ev.type == SDL_WINDOWEVENT && ev.window.event == SDL_WINDOWEVENT_LEAVE) {
            if (photon_sdl_sel_active(sdl)) {
                photon_sdl_clear_selection(sdl);
                redraw = true;
            }
        }

        if (ev.type != SDL_KEYDOWN) continue;

        SDL_Keycode sym = ev.key.keysym.sym;

        switch (sym) {
        case SDLK_ESCAPE: case SDLK_RETURN: case SDLK_q:
            done = true;
            break;
        case SDLK_UP:
            scroll_top--;
            break;
        case SDLK_DOWN:
            scroll_top++;
            break;
        case SDLK_PAGEUP:
            scroll_top -= visible;
            break;
        case SDLK_PAGEDOWN:
            scroll_top += visible;
            break;
        case SDLK_HOME:
            scroll_top = 0;
            break;
        case SDLK_END:
            scroll_top = total - visible;
            break;
        default:
            done = true;
            break;
        }

        int max_top = total - visible;
        if (max_top < 0) max_top = 0;
        if (scroll_top < 0) scroll_top = 0;
        if (scroll_top > max_top) scroll_top = max_top;

        if (scroll_top != prev_top) redraw = true;
    }

    free(row_buf);

    photon_sdl_repaint(sdl, vte);
    photon_sdl_present(sdl);
}

/* ── Public API: unified main loop helpers ──────────────────────────── */

bool photon_term_pump_tab(vte_t *vte, photon_conn_t *conn,
                          photon_sdl_t *sdl, bool is_active,
                          const photon_tab_bar_t *tabbar)
{
    (void)sdl;
    (void)tabbar;

    /* Drain data from this tab's connection into its VTE.
     * Active tab: drain all available data (user is watching it),
     * but cap iterations to avoid stalling the render loop.
     * Background tabs: drain one batch per iteration to limit CPU usage
     * and prevent busy background tabs from starving the active tab. */
    uint8_t buf[65536];
    bool got_data = false;

    if (is_active) {
        /* Active tab: drain everything, but cap iterations */
        int got;
        int iters = 0;
        while ((got = photon_conn_recv_for(conn, buf, sizeof(buf))) > 0) {
            vte_input(vte, buf, (size_t)got);
            got_data = true;
            if (++iters >= 4) break;
        }
    } else {
        /* Background tab: one batch per iteration */
        int got = photon_conn_recv_for(conn, buf, sizeof(buf));
        if (got > 0) {
            vte_input(vte, buf, (size_t)got);
            got_data = true;
        }
    }
    return got_data;
}

photon_term_result_t photon_term_check_connection(photon_conn_t *conn)
{
    if (!photon_conn_connected_for(conn))
        return PHOTON_TERM_DISCONNECT;
    return PHOTON_TERM_CONTINUE;
}

photon_term_result_t photon_term_handle_key(const photon_key_t *k,
                                            vte_t *vte, photon_conn_t *conn,
                                            photon_sdl_t *sdl, photon_ui_t *ui,
                                            const photon_bbs_t *bbs,
                                            photon_settings_t *settings,
                                            const photon_tab_bar_t *tabbar)
{
    (void)tabbar;

    if (k->code == 0)
        return PHOTON_TERM_CONTINUE;

    /* Key hook */
    if (s_key_hook_fn &&
        s_key_hook_fn(k, sdl, settings, s_key_hook_userdata)) {
        return PHOTON_TERM_CONTINUE;
    }

    /* Window close */
    if (k->code == PHOTON_KEY_QUIT)
        return PHOTON_TERM_QUIT;

    /* PageUp / scrollback */
    if ((k->code == PHOTON_KEY_PGUP && !(k->mod & ~PHOTON_MOD_SHIFT))
        || (k->code == PHOTON_KEY_UP && (k->mod & PHOTON_MOD_META))
        || k->code == PHOTON_KEY_SCROLL_UP) {
        run_scrollback_viewer(vte, sdl);
        return PHOTON_TERM_CONTINUE;
    }

    /* Alt-Z: session menu */
    if ((k->mod & PHOTON_MOD_ALT) && (k->code == 'z' || k->code == 'Z')) {
        bool was_ttf = photon_sdl_get_ttf_mode(sdl);
        uint8_t saved_pal[768];
        photon_sdl_save_palette(sdl, saved_pal);
        photon_sdl_set_ttf_mode(sdl, false);
        photon_theme_apply(photon_active_theme, sdl, NULL);

        if (s_session_menu_fn) {
            photon_term_result_t mr = s_session_menu_fn(
                ui, sdl, vte, bbs, settings, s_session_menu_userdata);
            photon_sdl_restore_palette(sdl, saved_pal);
            photon_sdl_set_ttf_mode(sdl, was_ttf);
            photon_sdl_invalidate(sdl);
            if (mr == PHOTON_TERM_RESUME)
                return PHOTON_TERM_CONTINUE;
            return mr;
        }

        session_menu_result_t m = show_session_menu(ui, sdl, vte, bbs, settings);

        photon_sdl_restore_palette(sdl, saved_pal);
        photon_sdl_set_ttf_mode(sdl, was_ttf);
        photon_sdl_invalidate(sdl);
        switch (m) {
            case SESSION_MENU_DISCONNECT:
                return PHOTON_TERM_DISCONNECT;
            case SESSION_MENU_NEWTAB:
                return PHOTON_TERM_NEWTAB;
            case SESSION_MENU_QUIT:
                return PHOTON_TERM_QUIT;
            case SESSION_MENU_SETTINGS:
                if (settings) {
                    photon_bbslist_run_settings(ui, settings);
                    photon_theme_apply(photon_active_theme, sdl, settings);
                }
                photon_sdl_invalidate(sdl);
                return PHOTON_TERM_CONTINUE;
            case SESSION_MENU_XFER:
                photon_conn_set_active(conn);
                photon_xfer_run(conn, sdl, ui);
                photon_sdl_invalidate(sdl);
                photon_sdl_repaint(sdl, vte);
                return PHOTON_TERM_CONTINUE;
            default:
                photon_sdl_invalidate(sdl);
                return PHOTON_TERM_CONTINUE;
        }
    }

    /* Alt-W: new tab */
    if ((k->mod & PHOTON_MOD_ALT) && (k->code == 'w' || k->code == 'W'))
        return PHOTON_TERM_NEWTAB;

    /* Alt-1..9: switch tab */
    if ((k->mod & PHOTON_MOD_ALT) && k->code >= '1' && k->code <= '9') {
        photon_switch_tab_target = k->code - '1';
        return PHOTON_TERM_SWITCH_TAB;
    }

    /* Alt-Left / Alt-Right: prev/next tab */
    if ((k->mod & PHOTON_MOD_ALT) && k->code == PHOTON_KEY_LEFT) {
        photon_switch_tab_target = (tabbar->active - 1 + tabbar->ntabs) % tabbar->ntabs;
        return PHOTON_TERM_SWITCH_TAB;
    }
    if ((k->mod & PHOTON_MOD_ALT) && k->code == PHOTON_KEY_RIGHT) {
        photon_switch_tab_target = (tabbar->active + 1) % tabbar->ntabs;
        return PHOTON_TERM_SWITCH_TAB;
    }

    /* Ctrl-\: force disconnect */
    if ((k->mod & PHOTON_MOD_CTRL) && k->code == '\\') {
        PHOTON_DBG("force disconnect via Ctrl-\\");
        return PHOTON_TERM_DISCONNECT;
    }

    /* Paste */
    bool is_paste = ((k->mod & PHOTON_MOD_META) && (k->code == 'v' || k->code == 'V'))
                 || ((k->mod & PHOTON_MOD_CTRL) && (k->mod & PHOTON_MOD_SHIFT)
                     && (k->code == 'v' || k->code == 'V'))
                 || (k->code == PHOTON_KEY_PASTE);
    if (is_paste) {
        char *clip = SDL_GetClipboardText();
        if (clip && clip[0]) {
            photon_conn_send_for(conn, (const uint8_t *)clip, strlen(clip));
        }
        SDL_free(clip);
        return PHOTON_TERM_CONTINUE;
    }

    /* Mouse selection copy */
    if (k->code == PHOTON_KEY_COPY_SEL) {
        copy_selection_to_clipboard(vte, sdl);
        return PHOTON_TERM_CONTINUE;
    }

    /* Any keypress clears mouse selection */
    if (photon_sdl_get_selection(sdl, NULL, NULL, NULL, NULL)) {
        photon_sdl_clear_selection(sdl);
    }

    /* Translate and send to remote */
    uint8_t seq[SEQ_MAX];
    int seqlen = key_to_bytes(k, seq);
    if (seqlen > 0)
        photon_conn_send_for(conn, seq, (size_t)seqlen);

    return PHOTON_TERM_CONTINUE;
}

void photon_term_render(photon_sdl_t *sdl, vte_t *vte,
                        const photon_tab_bar_t *tabbar, bool dirty)
{
    uint64_t t = now_ms();
    bool sel_live = photon_sdl_sel_active(sdl);
    bool alt_held = (SDL_GetModState() & (KMOD_LALT | KMOD_RALT)) != 0;

    if (dirty || sel_live || s_force_render || (t - s_last_render) >= FRAME_MS) {
        s_force_render = false;
        photon_sdl_repaint(sdl, vte);
        if (alt_held && tabbar && tabbar->ntabs > 1) {
            draw_alt_overlay(sdl, tabbar);
            alt_overlay_active = true;
        } else if (alt_overlay_active) {
            photon_sdl_invalidate_range(sdl,
                photon_sdl_rows(sdl) - 1, photon_sdl_rows(sdl) - 1);
            alt_overlay_active = false;
        }
        if (s_render_overlay_fn)
            s_render_overlay_fn(sdl, vte, s_render_overlay_userdata);
        photon_sdl_present(sdl);
        s_last_render = t;
    }
}
