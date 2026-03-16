/* photon_main.c - PhotonTERM entry point (full photon stack)
 *
 * Copyright (C) 2026 fewtarius and PhotonTERM contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This is the single entry point for PhotonTERM.  It uses:
 *   photon_sdl    - SDL2 + SDL2_ttf rendering
 *   photon_vte    - VTE terminal emulator
 *   photon_ui     - text UI
 *   photon_bbslist - dialing directory
 *   photon_conn   - network transport (telnet + SSH)
 *   photon_term   - terminal session loop
 *
 * On macOS, SDL2 is linked as a framework and SDL_main.h redirects
 * main() -> SDL_main().  We include SDL.h here to pick that up.
 *
 * Multi-tab:
 *   Up to PHOTON_MAX_TABS simultaneous sessions.  Each tab has its own
 *   vte_t and photon_conn_t.  Background connections keep running; the
 *   active tab's VTE drives the display.  Alt-W opens a new tab from the
 *   BBS directory.  Alt-1..9 switches tabs.  Closing all tabs exits.
 */

#include "photon_compat.h"
#include "photon_vte.h"
#include "photon_sdl.h"
#include "photon_ui.h"
#include "photon_bbslist.h"
#include "photon_conn.h"
#include "photon_term.h"
#include "photon_settings.h"

/* ── VTE response callback ───────────────────────────────────────────── */

/* Called by the VTE when it needs to send a response to the remote (e.g.
   DA response to ESC[c, CPR to ESC[6n, etc.)
   We only feed data into the active tab's VTE, so s_active is always correct. */
static void conn_vte_response(vte_t *vte, const char *data, size_t len, void *user)
{
    (void)vte;
    (void)user;
    if (len > 0)
        photon_conn_send((const uint8_t *)data, len, 500);
}

/* OSC 0/2 window title callback.  user is the photon_sdl_t context. */
static void photon_vte_title_cb(vte_t *vte, const char *title, void *user)
{
    (void)vte;
    photon_sdl_set_title((photon_sdl_t *)user, title);
}

/* BEL callback: visual flash (if bell is enabled in settings). */
bool g_bell_enabled = true;  /* updated from settings at startup; exported */

static void photon_vte_bell_cb(vte_t *vte, void *user)
{
    (void)vte;
    if (g_bell_enabled)
        photon_sdl_bell_flash((photon_sdl_t *)user);
}

#define PHOTON_DEBUG_BUILD
#include "photon_debug.h"

#include <SDL2/SDL.h>

#include <signal.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* SSH password prompt: called by photon_conn when no other auth works */
static bool ssh_password_prompt(const char *prompt, char *out, size_t outsz,
                                 void *userdata)
{
    (void)userdata;
    if (!photon_ui_global) return false;
    int len = photon_ui_input(photon_ui_global, prompt, out, (int)outsz - 1,
                              PHOTON_INPUT_PASSWORD);
    return len > 0;
}

/* Global quit flag - used by DarwinWrappers.m (Ctrl-C / window close). */
bool quitting = false;

/* Version string - updated by scripts/release.sh */
const char *photonterm_version = "PhotonTERM 20260316.2";

/* ── Signal handling ─────────────────────────────────────────────────── */

#ifndef _WIN32
static void ignore_signals(void)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = SIG_IGN;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT,  &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGHUP,  &sa, NULL);
    sigaction(SIGPIPE, &sa, NULL);
}
#endif

/* ── Tab management ──────────────────────────────────────────────────── */

#define PHOTON_MAX_TABS 9

typedef struct {
    photon_conn_t  *conn;
    vte_t          *vte;
    photon_bbs_t   *bbs;
    char            name[64];   /* display name for tab bar */
    bool            active;     /* slot in use */
    bool            activity;   /* unseen output since last focus */
} photon_tab_t;

static photon_tab_t tabs[PHOTON_MAX_TABS];
static int          ntabs      = 0;
static int          active_tab = 0;  /* 0-based */

/* Tab bar: rendered as a one-line strip at the bottom of the window.
 * Shows [1:Name] [2:Other*] where * = unread activity.
 * Colours: active tab bright-white on blue; others white on black. */
static void render_tab_bar(photon_sdl_t *sdl)
{
    if (ntabs <= 1) return;  /* no bar if only one tab */

    int cols = photon_sdl_cols(sdl);
    int rows = photon_sdl_rows(sdl);

    /* Use bottom row for tab bar */
    int bar_row = rows;

    /* Draw each tab label */
    int col = 1;
    for (int i = 0; i < ntabs; i++) {
        if (!tabs[i].active) continue;
        char label[72];
        snprintf(label, sizeof(label), "[%d:%s%s]",
                 i + 1, tabs[i].name,
                 tabs[i].activity ? "*" : "");

        bool is_active = (i == active_tab);
        /* Active: bright-white (15) on blue (4); others: white (7) on black (0) */
        uint8_t fg = is_active ? 15 : 7;
        uint8_t bg = is_active ?  4 : 0;
        uint8_t attr = is_active ? VTE_ATTR_BOLD : 0;

        for (int j = 0; label[j] && col <= cols; j++, col++) {
            vte_cell_t cell = { (uint32_t)(unsigned char)label[j], fg, bg, attr };
            photon_sdl_draw_cell(sdl, col, bar_row, &cell);
        }

        /* Space separator */
        if (col <= cols) {
            vte_cell_t sp = { ' ', 7, 0, 0 };
            photon_sdl_draw_cell(sdl, col++, bar_row, &sp);
        }
    }

    /* Fill remainder of bar row with blanks */
    while (col <= cols) {
        vte_cell_t blank = { ' ', 7, 0, 0 };
        photon_sdl_draw_cell(sdl, col++, bar_row, &blank);
    }

    photon_sdl_present(sdl);
}

/* Switch active tab to idx (0-based).  Repaints the screen from the
 * new tab's VTE buffer and updates the connection handle. */
static void switch_tab(photon_sdl_t *sdl, photon_settings_t *settings,
                        int idx)
{
    if (idx < 0 || idx >= PHOTON_MAX_TABS || !tabs[idx].active) return;
    if (idx == active_tab) return;

    tabs[active_tab].activity = false;  /* clear activity on old tab on leave */
    active_tab = idx;
    tabs[active_tab].activity = false;  /* clear activity on new tab */

    /* Switch connection */
    photon_conn_set_active(tabs[active_tab].conn);

    /* Reapply render mode for this tab */
    {
        photon_settings_t *s = settings;
        photon_bbs_t *bbs = tabs[active_tab].bbs;
        bool ttf;
        if (bbs->term_mode == PHOTON_TERM_MODE_AUTO)
            ttf = (bbs->conn_type == PHOTON_CONN_SHELL)
                  || (s->font_mode == PHOTON_FONT_TTF);
        else
            ttf = (bbs->term_mode == PHOTON_TERM_MODE_UTF8);
        photon_sdl_set_ttf_mode(sdl, ttf);

        /* Reapply palette mode */
        photon_palette_mode_t pm = (bbs->palette_mode != PHOTON_PALETTE_AUTO)
                                   ? bbs->palette_mode
                                   : s->default_palette_mode;
        photon_sdl_apply_palette_mode(sdl, pm, bbs->conn_type);
    }

    /* Repaint from VTE buffer */
    vte_repaint(tabs[active_tab].vte);
    render_tab_bar(sdl);
    photon_sdl_present(sdl);
}

/* Close a tab slot and shift remaining tabs down. */
static void close_tab(photon_sdl_t *sdl, int idx)
{
    if (idx < 0 || idx >= PHOTON_MAX_TABS || !tabs[idx].active) return;

    photon_conn_set_active(tabs[idx].conn);
    photon_conn_close();
    photon_conn_free(tabs[idx].conn);
    vte_free(tabs[idx].vte);
    photon_bbslist_free(tabs[idx].bbs);
    memset(&tabs[idx], 0, sizeof(tabs[idx]));
    ntabs--;

    /* Compact the array: shift live tabs left to fill the gap */
    for (int i = idx; i < PHOTON_MAX_TABS - 1; i++)
        tabs[i] = tabs[i + 1];
    memset(&tabs[PHOTON_MAX_TABS - 1], 0, sizeof(tabs[0]));

    /* Adjust active_tab */
    if (active_tab >= ntabs) active_tab = ntabs - 1;
    if (active_tab < 0)      active_tab = 0;

    /* Restore correct connection context */
    if (ntabs > 0)
        photon_conn_set_active(tabs[active_tab].conn);

    (void)sdl;
}

/* ── Entry point ─────────────────────────────────────────────────────── */

int main(int argc, char **argv)
{
    /* First pass: --debug flag (before SDL init so debug covers everything) */
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--debug") == 0) {
            photon_debug_enabled = true;
            photon_debug_open();
        }
    }
    PHOTON_DBG("PhotonTERM start (photon stack) argc=%d", argc);

    /* Suppress SDL SIGINT handler before SDL_Init */
#ifndef _WIN32
    setenv("SDL_NO_SIGNAL_HANDLERS", "1", 1);
    ignore_signals();
#endif

    /* Initialise SDL2 (must be done before photon_sdl_create) */
    /* Init VIDEO+EVENTS first; audio is optional (BEL only) */
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS) != 0) {
        fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }
    if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) {
        fprintf(stderr, "Note: SDL audio unavailable (%s) - BEL will be visual only\n",
                SDL_GetError());
        SDL_ClearError();
    }
    PHOTON_DBG("SDL_Init OK");

    /* Create SDL rendering context */
    photon_sdl_t *sdl = photon_sdl_create("PhotonTERM", 80, 24, NULL, 32);
    if (!sdl) {
        fprintf(stderr, "photon_sdl_create failed: %s\n",
                photon_sdl_last_error());
        SDL_Quit();
        return 1;
    }
    photon_sdl_global = sdl;
    PHOTON_DBG("SDL context created");

    /* Create UI context (shared across all tabs) */
    /* We create a dummy VTE for the UI; each tab has its own VTE. */
    vte_callbacks_t dummy_cbs = photon_sdl_make_vte_callbacks(sdl);
    dummy_cbs.response = conn_vte_response;
    vte_t *ui_vte = vte_create(80, 24, 1000, &dummy_cbs, false);
    if (!ui_vte) {
        fprintf(stderr, "vte_create failed\n");
        photon_sdl_free(sdl);
        SDL_Quit();
        return 1;
    }

    photon_ui_t *ui = photon_ui_create(sdl, ui_vte);
    if (!ui) {
        fprintf(stderr, "photon_ui_create failed\n");
        vte_free(ui_vte);
        photon_sdl_free(sdl);
        SDL_Quit();
        return 1;
    }
    photon_ui_global = ui;
    photon_conn_set_ssh_prompt(ssh_password_prompt, NULL);
    PHOTON_DBG("UI context created");

    /* Load settings and apply theme */
    photon_settings_t settings;
    photon_settings_load(&settings);
    {
        int tidx = photon_theme_find(settings.theme_name);
        photon_theme_apply(tidx >= 0 ? tidx : 0, sdl, &settings);
    }
    g_bell_enabled = settings.bell_enabled;

    /* ── Main loop ───────────────────────────────────────────────────── */
    memset(tabs, 0, sizeof(tabs));

    bool running = true;
    bool show_directory = false;  /* true = skip splash, open directory directly */
    while (running) {

        /* Show BBS list to pick a connection */
        photon_bbs_t *bbs = photon_bbslist_run(ui, show_directory);
        show_directory = false;  /* reset: only skip splash for the next call */
        if (!bbs) {
            PHOTON_DBG("user cancelled BBS list");
            if (ntabs == 0) break;  /* no tabs open -> exit */
            /* Switch to last active tab instead of exiting */
            switch_tab(sdl, &settings, active_tab);
            /* Fall through to run session for current tab */
            goto run_session;
        }

        /* Find a free tab slot */
        if (ntabs >= PHOTON_MAX_TABS) {
            photon_ui_msg(ui, "Maximum number of tabs open.");
            photon_bbslist_free(bbs);
            if (ntabs > 0) goto run_session;
            break;
        }

        /* Create VTE for this tab */
        vte_callbacks_t cbs = photon_sdl_make_vte_callbacks(sdl);
        cbs.response = conn_vte_response;
        /* Wire OSC title and BEL callbacks */
        cbs.title = photon_vte_title_cb;
        cbs.bell  = photon_vte_bell_cb;
        /* CP437 mode: respect user's term_mode setting.  AUTO uses CP437
         * for Telnet/SSH (raw 8-bit ANSI art) and UTF-8 for local shell. */
        bool use_cp437;
        if (bbs->term_mode == PHOTON_TERM_MODE_AUTO)
            use_cp437 = (bbs->conn_type != PHOTON_CONN_SHELL);
        else
            use_cp437 = (bbs->term_mode == PHOTON_TERM_MODE_CP437);
        vte_t *vte = vte_create(80, 24, 1000, &cbs, use_cp437);
        if (!vte) {
            photon_ui_msg(ui, "Failed to create terminal emulator.");
            photon_bbslist_free(bbs);
            if (ntabs > 0) goto run_session;
            break;
        }

        /* Create connection handle */
        photon_conn_t *conn = photon_conn_new();
        if (!conn) {
            photon_ui_msg(ui, "Out of memory.");
            vte_free(vte);
            photon_bbslist_free(bbs);
            if (ntabs > 0) goto run_session;
            break;
        }

        /* Find a free slot (should always be ntabs since we checked above) */
        int slot = ntabs;
        tabs[slot].conn     = conn;
        tabs[slot].vte      = vte;
        tabs[slot].bbs      = bbs;
        tabs[slot].active   = true;
        tabs[slot].activity = false;
        strlcpy(tabs[slot].name, bbs->name[0] ? bbs->name : "New Tab",
                sizeof(tabs[slot].name));
        active_tab = slot;
        ntabs++;

        PHOTON_DBG("tab %d: connecting to '%s' %s:%u type=%d",
                   slot, bbs->name, bbs->addr, bbs->port, bbs->conn_type);

        /* Apply per-BBS render mode */
        {
            bool ttf;
            if (bbs->term_mode == PHOTON_TERM_MODE_AUTO)
                ttf = (bbs->conn_type == PHOTON_CONN_SHELL)
                      || (settings.font_mode == PHOTON_FONT_TTF);
            else
                ttf = (bbs->term_mode == PHOTON_TERM_MODE_UTF8);
            photon_sdl_set_ttf_mode(sdl, ttf);

            /* Apply per-BBS palette mode (AUTO resolves by conn_type) */
            photon_palette_mode_t pm = (bbs->palette_mode != PHOTON_PALETTE_AUTO)
                                       ? bbs->palette_mode
                                       : settings.default_palette_mode;
            photon_sdl_apply_palette_mode(sdl, pm, bbs->conn_type);
        }

        /* Make this conn active and connect */
        photon_conn_set_active(conn);
        photon_sdl_show_connecting(sdl, bbs->name);

        if (!photon_conn_connect(bbs)) {
            char errmsg[256];
            snprintf(errmsg, sizeof(errmsg), "Connection failed: %s",
                     photon_conn_last_error());
            close_tab(sdl, slot);
            /* Restore theme */
            photon_theme_apply(photon_active_theme, sdl, &settings);
            photon_sdl_set_ttf_mode(sdl, false);  /* reset to bitmap for UI */
            photon_ui_msg(ui, errmsg);
            if (ntabs > 0) goto run_session;
            continue;
        }
        PHOTON_DBG("tab %d: connected", slot);
        render_tab_bar(sdl);

    run_session:;
        /* Build tab bar info for Alt overlay */
        photon_tab_bar_t tabbar = { .ntabs = ntabs, .active = active_tab };
        for (int _ti = 0; _ti < ntabs && _ti < PHOTON_TAB_BAR_MAX; _ti++) {
            strlcpy(tabbar.names[_ti], tabs[_ti].name, sizeof(tabbar.names[0]));
            tabbar.activity[_ti] = tabs[_ti].activity;
        }

        /* Run the terminal loop for the active tab */
        photon_term_result_t r = photon_doterm(
            tabs[active_tab].vte, sdl, ui,
            tabs[active_tab].bbs, &settings, &tabbar);
        PHOTON_DBG("tab %d: doterm returned %d", active_tab, (int)r);

        switch (r) {
        case PHOTON_TERM_QUIT:
            running = false;
            break;

        case PHOTON_TERM_DISCONNECT:
            /* Session ended - close this tab */
            close_tab(sdl, active_tab);
            photon_theme_apply(photon_active_theme, sdl, &settings);
            if (ntabs == 0) {
                /* No tabs left: go back to BBS list */
                /* Sync ui_vte to current window grid (may have been resized) */
                vte_resize(ui_vte, photon_sdl_cols(sdl), photon_sdl_rows(sdl));
                photon_sdl_set_ttf_mode(sdl, false);  /* reset to bitmap for UI */
                show_directory = true;  /* return to dialer, not splash */
                continue;
            }
            /* Repaint remaining tab */
            vte_repaint(tabs[active_tab].vte);
            render_tab_bar(sdl);
            photon_sdl_present(sdl);
            goto run_session;

        case PHOTON_TERM_NEWTAB:
            /* User wants a new tab - loop back to BBS list */
            photon_theme_apply(photon_active_theme, sdl, &settings);
            photon_sdl_set_ttf_mode(sdl, false);  /* reset to bitmap for UI */
            /* Sync ui_vte to current window grid (may have been resized) */
            vte_resize(ui_vte, photon_sdl_cols(sdl), photon_sdl_rows(sdl));
            show_directory = true;  /* open directory directly for new tab */
            continue;

        case PHOTON_TERM_SWITCH_TAB: {
            int target = photon_switch_tab_target;
            PHOTON_DBG("switch_tab request: target=%d (current=%d ntabs=%d)",
                       target, active_tab, ntabs);
            if (target >= 0 && target < ntabs && target != active_tab) {
                switch_tab(sdl, &settings, target);
            }
            goto run_session;
        }

        default:
            break;
        }
    }

    /* ── Cleanup ─────────────────────────────────────────────────────── */
    for (int i = 0; i < PHOTON_MAX_TABS; i++) {
        if (!tabs[i].active) continue;
        photon_conn_set_active(tabs[i].conn);
        photon_conn_close();
        photon_conn_free(tabs[i].conn);
        vte_free(tabs[i].vte);
        photon_bbslist_free(tabs[i].bbs);
    }

    photon_ui_free(ui);
    vte_free(ui_vte);
    photon_sdl_free(sdl);
    SDL_Quit();

    PHOTON_DBG("EXIT clean");
    return 0;
}
