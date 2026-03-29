/* photon_term.c - PhotonTERM terminal session loop (photon_vte + photon_sdl)
 *
 * Copyright (C) 2026 fewtarius and PhotonTERM contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * photon_doterm() - run one connected terminal session.
 *
 * Key bindings:
 *   Alt-Z          - in-session options menu
 *   Ctrl-C         - send 0x03 to remote (interrupt, BBS standard)
 *   Ctrl-\         - force-disconnect and return to BBS list
 *
 * Return values:
 *   PHOTON_TERM_DISCONNECT   - remote disconnected / user disconnected
 *   PHOTON_TERM_QUIT         - user closed window / requested exit
 *   PHOTON_TERM_NEWTAB       - user requested new tab (Alt-W)
 */

#include "photon_compat.h"
#include "photon_vte.h"
#include <SDL2/SDL.h>
#include "photon_sdl.h"
#include "photon_term.h"
#include "photon_conn.h"
#include "photon_ui.h"
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

/* ── Alt-held tab bar overlay ────────────────────────────────────────── */

/* Draw a one-row overlay on the bottom row showing tab list.
 * Shown while Alt/Option is held when ntabs > 1. */
static void draw_alt_overlay(photon_sdl_t *sdl, const photon_tab_bar_t *tabbar)
{
    if (!sdl || !tabbar || tabbar->ntabs <= 1) return;

    int cols = photon_sdl_cols(sdl);
    int rows = photon_sdl_rows(sdl);
    int bar_row = rows;   /* 1-indexed: last row */
    int col = 1;

    /* Tab entries */
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
        /* Separator */
        if (col <= cols) {
            vte_cell_t sep = { '|', 8, 0, 0 };
            photon_sdl_draw_cell(sdl, col++, bar_row, &sep);
        }
    }

    /* Hint: W=New */
    const char *hint = " W=New ";
    for (int j = 0; hint[j] && col <= cols; j++, col++) {
        vte_cell_t cell = { (uint32_t)(unsigned char)hint[j], 3, 0, 0 };
        photon_sdl_draw_cell(sdl, col, bar_row, &cell);
    }

    /* Fill remainder */
    while (col <= cols) {
        vte_cell_t blank = { ' ', 7, 0, 0 };
        photon_sdl_draw_cell(sdl, col++, bar_row, &blank);
    }
}


#define SEQ_MAX 8

/* ── Mouse selection -> clipboard ───────────────────────────────────── */

/* Extract text from VTE for a rectangular selection and copy to clipboard.
 * Uses vte_get_cell() to read each cell, encodes codepoints to UTF-8. */
static void copy_selection_to_clipboard(vte_t *vte, photon_sdl_t *sdl)
{
    int c0, r0, c1, r1;
    if (!photon_sdl_get_selection(sdl, &c0, &r0, &c1, &r1)) return;

    int cols = photon_sdl_cols(sdl);
    /* Allocate worst case: (c1-c0+1) cols * (r1-r0+1) rows * 4 bytes/char + newlines */
    size_t bufsz = (size_t)(r1 - r0 + 1) * ((size_t)cols * 4 + 2) + 1;
    char *buf = malloc(bufsz);
    if (!buf) return;

    size_t pos = 0;
    for (int r = r0; r <= r1; r++) {
        int ca = (r == r0) ? c0 : 0;
        int cb = (r == r1) ? c1 : cols - 1;
        /* Collect cells, trim trailing spaces */
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
            /* Encode UTF-8 */
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
    /* Printable UTF-8 text (from SDL_TEXTINPUT) - send as-is */
    if (k->text[0] && !(k->mod & PHOTON_MOD_CTRL) && !(k->mod & PHOTON_MOD_ALT)) {
        int len = (int)strnlen(k->text, sizeof(k->text));
        memcpy(out, k->text, (size_t)len);
        return len;
    }

    /* Ctrl+letter -> 0x01-0x1A */
    if (k->mod & PHOTON_MOD_CTRL) {
        /* SDL already translated Ctrl+letter to the control code (1-26) */
        if (k->code >= 1 && k->code <= 26) {
            out[0] = (uint8_t)k->code; return 1;
        }
        /* Also handle if SDL sends the letter itself */
        int base = -1;
        if (k->code >= 'a' && k->code <= 'z') base = k->code - 'a';
        if (k->code >= 'A' && k->code <= 'Z') base = k->code - 'A';
        if (base >= 0) { out[0] = (uint8_t)(base + 1); return 1; }
        /* Ctrl-[ = ESC */
        if (k->code == '[') { out[0] = '\x1b'; return 1; }
    }

    /* Special keys -> ANSI/VT100 sequences */
    switch (k->code) {
        /* ASCII control range - send directly */
        case '\r': case '\n':
            out[0] = '\r'; return 1;
        case '\t':
            out[0] = '\t'; return 1;
        case '\x1b':
            out[0] = '\x1b'; return 1;
        case '\x7f':
            out[0] = '\x7f'; return 1;

        /* cursor keys */
        case PHOTON_KEY_UP:
            memcpy(out, "\x1b[A", 3); return 3;
        case PHOTON_KEY_DOWN:
            memcpy(out, "\x1b[B", 3); return 3;
        case PHOTON_KEY_RIGHT:
            memcpy(out, "\x1b[C", 3); return 3;
        case PHOTON_KEY_LEFT:
            memcpy(out, "\x1b[D", 3); return 3;

        /* navigation */
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

        /* function keys */
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

    /* Plain printable ASCII (code 32-126) */
    if (k->code >= 32 && k->code <= 126) {
        out[0] = (uint8_t)k->code;
        return 1;
    }

    return 0;  /* unhandled */
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

static session_menu_result_t show_session_menu(photon_ui_t *ui,
                                               photon_sdl_t *sdl, vte_t *vte,
                                               const photon_bbs_t *bbs,
                                               photon_settings_t *settings)
{
    const char *items[] = {
        "Disconnect",
        "New Tab",
        "Exit PhotonTERM",
        "Settings",
        "File Transfer (ZModem/YModem/XModem)",
        NULL
    };

    char title[128];
    snprintf(title, sizeof(title), "Session: %s",
             bbs && bbs->name[0] ? bbs->name : "Connected");

    /* photon_ui_list saves/restores screen internally */
    int choice = photon_ui_list(ui, title, items, 5, 0);

    /* Repaint the terminal under the closed menu */
    photon_sdl_repaint(sdl, vte);
    photon_sdl_present(sdl);

    switch (choice) {
        case 0:  return SESSION_MENU_DISCONNECT;
        case 1:  return SESSION_MENU_NEWTAB;
        case 2:  return SESSION_MENU_QUIT;
        case 3:  return SESSION_MENU_SETTINGS;
        case 4:  return SESSION_MENU_XFER;
        default: return SESSION_MENU_NONE;  /* cancelled */
    }
}

/* ── Scrollback viewer ───────────────────────────────────────────────── */

/* Display the scrollback buffer.  The terminal content is not scrolled -
 * we paint scrollback lines directly over the SDL surface and let the user
 * navigate with arrow keys / PgUp / PgDn / Home / End.  ESC or any other
 * key returns to live terminal. */
static void run_scrollback_viewer(vte_t *vte, photon_sdl_t *sdl)
{
    int sb_lines = vte_scrollback_lines(vte);
    int screen_rows = vte_rows(vte);
    int total = sb_lines + screen_rows;  /* scrollback + live screen */
    if (total <= 0) return;

    int cols = photon_sdl_cols(sdl);
    int rows = photon_sdl_rows(sdl);
    int visible = rows - 1;  /* reserve bottom row for status bar */
    if (visible < 1) visible = 1;

    vte_cell_t *row_buf = calloc((size_t)cols, sizeof(vte_cell_t));
    if (!row_buf) return;

    /* Start at bottom (live screen visible) */
    int scroll_top = total - visible;
    if (scroll_top < 0) scroll_top = 0;

    bool redraw = true;
    bool done   = false;

    /* Status bar colours (bright white on blue, bold) */
    vte_cell_t sb_bar_bg = { ' ', 15, 4, VTE_ATTR_BOLD };
    vte_cell_t blank = { ' ', VTE_COLOR_DEFAULT_FG, VTE_COLOR_DEFAULT_BG, 0 };

    while (!done) {
        if (redraw) {
            for (int r = 0; r < visible; r++) {
                int line = scroll_top + r;  /* virtual line in combined buffer */
                bool got = false;
                if (line < sb_lines) {
                    /* In scrollback region */
                    got = vte_scrollback_get(vte, line, row_buf, NULL);
                } else if (line < total) {
                    /* In live screen region */
                    int screen_row = line - sb_lines;  /* 0-based */
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
            /* Status bar on bottom row */
            char status[128];
            snprintf(status, sizeof(status),
                     " SCROLLBACK  Line %d/%d  Up/Dn  PgUp/PgDn  Home/End  ESC=exit",
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
        if (!SDL_WaitEventTimeout(&ev, 50)) continue;
        if (ev.type == SDL_QUIT) { done = true; break; }
        if (ev.type != SDL_KEYDOWN) continue;

        SDL_Keycode sym = ev.key.keysym.sym;
        int prev_top = scroll_top;

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
            done = true;  /* any other key exits */
            break;
        }

        /* Clamp */
        int max_top = total - visible;
        if (max_top < 0) max_top = 0;
        if (scroll_top < 0) scroll_top = 0;
        if (scroll_top > max_top) scroll_top = max_top;

        if (scroll_top != prev_top) redraw = true;
    }

    free(row_buf);

    /* Repaint live terminal over the viewer */
    photon_sdl_repaint(sdl, vte);
    photon_sdl_present(sdl);
}

/* ── Main session loop ──────────────────────────────────────────────── */

photon_term_result_t photon_doterm(vte_t *vte, photon_sdl_t *sdl,
                                   photon_ui_t *ui, const photon_bbs_t *bbs,
                                   photon_settings_t *settings,
                                   const photon_tab_bar_t *tabbar)
{
    PHOTON_DBG("photon_doterm: ENTER bbs='%s'", bbs ? bbs->name : "(null)");

    /* Apply ANSI SGR 16-colour palette so SGR colours are accurate regardless
     * of the current UI theme.  xterm-256 or overrides applied below. */
    photon_sdl_reset_to_ansi_palette(sdl);

    photon_term_result_t result = PHOTON_TERM_DISCONNECT;
    bool done = false;

    const uint64_t FRAME_MS = 16;   /* ~60 fps render cap */
    uint64_t last_render = 0;
    bool     dirty       = false;

    while (!done) {
        bool got_data = false;
        /* 1. Read incoming remote data and feed to VTE */
        {
            uint8_t buf[65536];
            /* Drain data for up to FRAME_MS before rendering, so large output
             * scrolls in real-time rather than buffering then dumping. */
            uint64_t drain_deadline = now_ms() + FRAME_MS;
            int got;
            while ((got = photon_conn_recv(buf, sizeof(buf), 0)) > 0) {
                vte_input(vte, buf, (size_t)got);
                dirty = true;
                got_data = true; /* remember we were busy; skip idle sleep */
                if (now_ms() >= drain_deadline) break; /* render what we have */
            }
        }

        /* 2. Connection drop detection */
        if (!photon_conn_connected()) {
            PHOTON_DBG("photon_doterm: connection dropped");
            result = PHOTON_TERM_DISCONNECT;
            done   = true;
            break;
        }

        /* 3. Window resize detection */
        {
            int nc, nr;
            if (photon_sdl_check_resize(sdl, &nc, &nr)) {
                PHOTON_DBG("photon_doterm: window resize -> %dx%d", nc, nr);
                vte_resize(vte, nc, nr);
                photon_conn_resize(nc, nr);
                dirty = true;
            }
        }

        /* 4. Process keyboard / window events */
        {
            photon_key_t k;
            while (!done && photon_sdl_poll_key(sdl, &k)) {
                /* Window close button */
                if (k.code == PHOTON_KEY_QUIT) {
                    result = PHOTON_TERM_QUIT;
                    done   = true;
                    break;
                }

                /* PageUp (optionally with Shift) or Cmd+Up: open scrollback viewer */
                if ((k.code == PHOTON_KEY_PGUP && !(k.mod & ~PHOTON_MOD_SHIFT))
                    || (k.code == PHOTON_KEY_UP && (k.mod & PHOTON_MOD_META))) {
                    run_scrollback_viewer(vte, sdl);
                    dirty = true;
                    continue;
                }

                /* Alt-Z: in-session options menu */
                if ((k.mod & PHOTON_MOD_ALT) && (k.code == 'z' || k.code == 'Z')) {
                    /* Save rendering state so the menu uses theme colours
                     * and bitmap font, then restore after. */
                    bool was_ttf = photon_sdl_get_ttf_mode(sdl);
                    uint8_t saved_pal[768];
                    photon_sdl_save_palette(sdl, saved_pal);
                    photon_sdl_set_ttf_mode(sdl, false);
                    photon_theme_apply(photon_active_theme, sdl, NULL);

                    session_menu_result_t m = show_session_menu(ui, sdl, vte, bbs, settings);

                    photon_sdl_restore_palette(sdl, saved_pal);
                    photon_sdl_set_ttf_mode(sdl, was_ttf);
                    switch (m) {
                        case SESSION_MENU_DISCONNECT:
                            result = PHOTON_TERM_DISCONNECT;
                            done   = true;
                            break;
                        case SESSION_MENU_NEWTAB:
                            result = PHOTON_TERM_NEWTAB;
                            done   = true;
                            break;
                        case SESSION_MENU_QUIT:
                            result = PHOTON_TERM_QUIT;
                            done   = true;
                            break;
                        case SESSION_MENU_SETTINGS:
                            if (settings) {
                                photon_bbslist_run_settings(ui, settings);
                                /* Re-apply theme in case user changed it */
                                photon_theme_apply(photon_active_theme, sdl, settings);
                            }
                            dirty = true;
                            break;
                        case SESSION_MENU_XFER:
                            photon_xfer_run(photon_conn_get_active(), sdl, ui);
                            photon_sdl_repaint(sdl, vte);
                            dirty = true;
                            break;
                        default:
                            dirty = true;   /* menu closed, repaint terminal */
                            break;
                    }
                    break;  /* restart key loop after menu */
                }

                /* Alt-W: open new tab (shortcut) */
                if ((k.mod & PHOTON_MOD_ALT) && (k.code == 'w' || k.code == 'W')) {
                    result = PHOTON_TERM_NEWTAB;
                    done   = true;
                    break;
                }

                /* Alt-1..9: switch to tab N */
                if ((k.mod & PHOTON_MOD_ALT) && k.code >= '1' && k.code <= '9') {
                    photon_switch_tab_target = k.code - '1';  /* 0-based */
                    result = PHOTON_TERM_SWITCH_TAB;
                    done   = true;
                    break;
                }

                /* Ctrl-\ (ASCII 0x1c): force disconnect */
                if ((k.mod & PHOTON_MOD_CTRL) && k.code == '\\') {
                    PHOTON_DBG("photon_doterm: Ctrl-\\ force disconnect");
                    result = PHOTON_TERM_DISCONNECT;
                    done   = true;
                    break;
                }

                /* Cmd+V (macOS) or Ctrl+Shift+V (Linux/Win): paste clipboard */
                bool is_paste = ((k.mod & PHOTON_MOD_META) && (k.code == 'v' || k.code == 'V'))
                             || ((k.mod & PHOTON_MOD_CTRL) && (k.mod & PHOTON_MOD_SHIFT)
                                 && (k.code == 'v' || k.code == 'V'))
                             || (k.code == PHOTON_KEY_PASTE);
                if (is_paste) {
                    char *clip = SDL_GetClipboardText();
                    if (clip && clip[0]) {
                        photon_conn_send((const uint8_t *)clip, strlen(clip), 2000);
                        dirty = true;
                    }
                    SDL_free(clip);
                    continue;
                }

                /* Mouse selection complete: copy to clipboard */
                if (k.code == PHOTON_KEY_COPY_SEL) {
                    copy_selection_to_clipboard(vte, sdl);
                    dirty = true;   /* repaint to clear highlight */
                    continue;
                }

                /* Any regular keypress clears the mouse selection */
                if (photon_sdl_get_selection(sdl, NULL, NULL, NULL, NULL)) {
                    photon_sdl_clear_selection(sdl);
                    dirty = true;
                }

                /* Translate and send to remote */
                uint8_t seq[SEQ_MAX];
                int seqlen = key_to_bytes(&k, seq);
                if (seqlen > 0)
                    photon_conn_send(seq, (size_t)seqlen, 1000);
            }
        }

        /* 5. Render if dirty, frame timer expired, or mouse selection is live */
        {
            uint64_t t = now_ms();
            bool sel_live = photon_sdl_sel_active(sdl);
            bool alt_held = (SDL_GetModState() & (KMOD_LALT | KMOD_RALT)) != 0;

            /* Render whenever dirty (including mid-stream data) or on frame timer.
             * got_data means we hit the drain deadline - render now so the user
             * sees incremental progress, then loop to drain more. */
            if (dirty || got_data || sel_live || (t - last_render) >= FRAME_MS) {
                photon_sdl_repaint(sdl, vte);
                if (alt_held && tabbar && tabbar->ntabs > 1)
                    draw_alt_overlay(sdl, tabbar);
                photon_sdl_present(sdl);
                last_render = t;
                dirty       = false;
            }
        }

        /* 6. Short sleep to avoid busy-spin */
        {
            /* Sleep only when idle - if we got data, loop immediately */
            if (!got_data && !dirty) {
                struct timespec sl = { 0, 2000000L };  /* 2 ms */
                nanosleep(&sl, NULL);
            }
        }
    }

    /* Final repaint before returning */
    photon_sdl_repaint(sdl, vte);
    photon_sdl_present(sdl);

    PHOTON_DBG("photon_doterm: EXIT result=%d", (int)result);
    return result;
}
