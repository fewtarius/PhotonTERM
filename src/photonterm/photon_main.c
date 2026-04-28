/* photon_main.c - PhotonTERM entry point (full photon stack)
 *
 * Copyright (C) 2026 fewtarius and PhotonTERM contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Unified main loop: all tabs are pumped concurrently, only the active
 * tab renders.  SDL events are processed every iteration so tab switching
 * and UI interactions are always responsive regardless of connection load.
 *
 * On macOS, SDL2 is linked as a framework and SDL_main.h redirects
 * main() -> SDL_main().  We include SDL.h here to pick that up.
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

/* Called by the VTE when it needs to send a response to the remote.
 * Since each tab feeds its own VTE, we need per-tab response routing.
 * The main loop sets s_active before pumping each tab. */
static void conn_vte_response(vte_t *vte, const char *data, size_t len, void *user)
{
    (void)vte;
    (void)user;
    /* s_active is already set to this tab's conn by the pump loop */
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
bool g_bell_enabled = true;

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

/* Declared in early_init.c (Linux) / DarwinWrappers.m (macOS) */
extern int photonterm_sigterm_pending(void);

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
const char *photonterm_version = "PhotonTERM 20260420.2";

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
    char            name[64];
    bool            active;     /* slot in use */
    bool            activity;   /* unseen output since last focus */
    bool            dirty;      /* VTE content changed, needs render */
} photon_tab_t;

static photon_tab_t tabs[PHOTON_MAX_TABS];
static int          ntabs      = 0;
static int          active_tab = 0;  /* 0-based */

/* Render the tab bar (bottom row) for the active tab list. */
static void render_tab_bar(photon_sdl_t *sdl)
{
    if (ntabs <= 1) return;

    int cols = photon_sdl_cols(sdl);
    int rows = photon_sdl_rows(sdl);
    int bar_row = rows;

    int col = 1;
    for (int i = 0; i < ntabs; i++) {
        if (!tabs[i].active) continue;
        char label[72];
        snprintf(label, sizeof(label), "[%d:%s%s]",
                 i + 1, tabs[i].name,
                 tabs[i].activity ? "*" : "");

        bool is_active = (i == active_tab);
        uint8_t fg  = is_active ? 15 : 7;
        uint8_t bg  = is_active ?  4 : 0;
        uint8_t att = is_active ? VTE_ATTR_BOLD : 0;

        for (int j = 0; label[j] && col <= cols; j++, col++) {
            vte_cell_t cell = { (uint32_t)(unsigned char)label[j], fg, bg, att };
            photon_sdl_draw_cell(sdl, col, bar_row, &cell);
        }
        if (col <= cols) {
            vte_cell_t sp = { ' ', 7, 0, 0 };
            photon_sdl_draw_cell(sdl, col++, bar_row, &sp);
        }
    }
    while (col <= cols) {
        vte_cell_t blank = { ' ', 7, 0, 0 };
        photon_sdl_draw_cell(sdl, col++, bar_row, &blank);
    }
    photon_sdl_present(sdl);
}

/* Build tab bar info struct for passing to photon_term functions. */
static photon_tab_bar_t build_tab_bar(void)
{
    photon_tab_bar_t tb = { .ntabs = ntabs, .active = active_tab };
    for (int i = 0; i < ntabs && i < PHOTON_TAB_BAR_MAX; i++) {
        strlcpy(tb.names[i], tabs[i].name, sizeof(tb.names[0]));
        tb.activity[i] = tabs[i].activity;
    }
    return tb;
}

/* Switch active tab to idx (0-based). */
static void switch_tab(photon_sdl_t *sdl, photon_settings_t *settings, int idx)
{
    if (idx < 0 || idx >= PHOTON_MAX_TABS || !tabs[idx].active) return;
    if (idx == active_tab) return;

    tabs[active_tab].activity = false;
    active_tab = idx;
    tabs[active_tab].activity = false;

    /* Switch connection */
    photon_conn_set_active(tabs[active_tab].conn);

    /* Reapply render mode for this tab */
    {
        photon_bbs_t *bbs = tabs[active_tab].bbs;
        bool ttf;
        if (bbs->term_mode == PHOTON_TERM_MODE_AUTO)
            ttf = (bbs->conn_type == PHOTON_CONN_SHELL)
                  || (settings->font_mode == PHOTON_FONT_TTF);
        else
            ttf = (bbs->term_mode == PHOTON_TERM_MODE_UTF8);
        photon_sdl_set_ttf_mode(sdl, ttf);

        photon_palette_mode_t pm = (bbs->palette_mode != PHOTON_PALETTE_AUTO)
                                   ? bbs->palette_mode
                                   : settings->default_palette_mode;
        photon_sdl_apply_palette_mode(sdl, pm, bbs->conn_type);
    }

    photon_sdl_clear(sdl);
    photon_sdl_invalidate(sdl);
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

    for (int i = idx; i < PHOTON_MAX_TABS - 1; i++)
        tabs[i] = tabs[i + 1];
    memset(&tabs[PHOTON_MAX_TABS - 1], 0, sizeof(tabs[0]));

    if (active_tab >= ntabs) active_tab = ntabs - 1;
    if (active_tab < 0)      active_tab = 0;

    if (ntabs > 0)
        photon_conn_set_active(tabs[active_tab].conn);

    (void)sdl;
}

/* ── Connection creation helper ──────────────────────────────────────── */

/* Create and connect a new tab.  Returns the slot index, or -1 on failure.
 * On failure, the caller is responsible for showing an error and resuming. */
static int create_and_connect_tab(photon_sdl_t *sdl, photon_ui_t *ui,
                                  photon_settings_t *settings,
                                  photon_bbs_t *bbs, vte_t *ui_vte)
{
    if (ntabs >= PHOTON_MAX_TABS) {
        photon_ui_msg(ui, "Maximum number of tabs open.");
        photon_bbslist_free(bbs);
        return -1;
    }

    vte_callbacks_t cbs = photon_sdl_make_vte_callbacks(sdl);
    cbs.response = conn_vte_response;
    cbs.title    = photon_vte_title_cb;
    cbs.bell     = photon_vte_bell_cb;

    bool use_cp437;
    if (bbs->term_mode == PHOTON_TERM_MODE_AUTO)
        use_cp437 = (bbs->conn_type != PHOTON_CONN_SHELL);
    else
        use_cp437 = (bbs->term_mode == PHOTON_TERM_MODE_CP437);

    vte_t *vte = vte_create(80, 24, 1000, &cbs, use_cp437);
    if (!vte) {
        photon_ui_msg(ui, "Failed to create terminal emulator.");
        photon_bbslist_free(bbs);
        return -1;
    }

    photon_conn_t *conn = photon_conn_new();
    if (!conn) {
        photon_ui_msg(ui, "Out of memory.");
        vte_free(vte);
        photon_bbslist_free(bbs);
        return -1;
    }

    int slot = ntabs;
    tabs[slot].conn     = conn;
    tabs[slot].vte      = vte;
    tabs[slot].bbs      = bbs;
    tabs[slot].active   = true;
    tabs[slot].activity = false;
    tabs[slot].dirty    = false;
    strlcpy(tabs[slot].name, bbs->name[0] ? bbs->name : "New Tab",
            sizeof(tabs[slot].name));
    active_tab = slot;
    ntabs++;

    /* Auto-detect window size */
    if (settings->cols == 0 && settings->rows == 0) {
        int nc = photon_sdl_cols(sdl);
        int nr = photon_sdl_rows(sdl);
        bbs->init_cols = nc;
        bbs->init_rows = nr;
        vte_resize(vte, nc, nr);
    }

    PHOTON_DBG("tab %d: connecting to '%s' %s:%u type=%d",
               slot, bbs->name, bbs->addr, bbs->port, bbs->conn_type);

    /* Apply per-BBS render mode */
    {
        bool ttf;
        if (bbs->term_mode == PHOTON_TERM_MODE_AUTO)
            ttf = (bbs->conn_type == PHOTON_CONN_SHELL)
                  || (settings->font_mode == PHOTON_FONT_TTF);
        else
            ttf = (bbs->term_mode == PHOTON_TERM_MODE_UTF8);
        photon_sdl_set_ttf_mode(sdl, ttf);

        photon_palette_mode_t pm = (bbs->palette_mode != PHOTON_PALETTE_AUTO)
                                   ? bbs->palette_mode
                                   : settings->default_palette_mode;
        photon_sdl_apply_palette_mode(sdl, pm, bbs->conn_type);
    }

    photon_conn_set_active(conn);
    photon_sdl_show_connecting(sdl, bbs->name);

    if (!photon_conn_connect(bbs)) {
        char errmsg[256];
        snprintf(errmsg, sizeof(errmsg), "Connection failed: %s",
                 photon_conn_last_error());
        close_tab(sdl, slot);
        photon_theme_apply(photon_active_theme, sdl, settings);
        photon_sdl_set_ttf_mode(sdl, false);
        photon_ui_msg(ui, errmsg);
        return -1;
    }

    PHOTON_DBG("tab %d: connected", slot);
    render_tab_bar(sdl);
    return slot;
}

/* ── Entry point ─────────────────────────────────────────────────────── */

int main(int argc, char **argv)
{
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--debug") == 0) {
            photon_debug_enabled = true;
            photon_debug_open();
        }
    }
    PHOTON_DBG("PhotonTERM start (unified main loop) argc=%d", argc);

#ifndef _WIN32
    setenv("SDL_NO_SIGNAL_HANDLERS", "1", 1);
    ignore_signals();
#endif

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

    photon_sdl_t *sdl = photon_sdl_create("PhotonTERM", 80, 24, NULL, 32);
    if (!sdl) {
        fprintf(stderr, "photon_sdl_create failed: %s\n",
                photon_sdl_last_error());
        SDL_Quit();
        return 1;
    }
    photon_sdl_global = sdl;
    PHOTON_DBG("SDL context created");

    /* UI VTE (for directory/settings overlays) */
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

    photon_settings_t settings;
    photon_settings_load(&settings);
    photon_sdl_set_fixed_size(sdl, settings.cols, settings.rows);
    {
        int tidx = photon_theme_find(settings.theme_name);
        photon_theme_apply(tidx >= 0 ? tidx : 0, sdl, &settings);
    }
    g_bell_enabled = settings.bell_enabled;

    /* ── State machine ────────────────────────────────────────────────── */
    memset(tabs, 0, sizeof(tabs));

    typedef enum {
        STATE_DIRECTORY,   /* showing BBS directory / connecting */
        STATE_RUNNING,     /* unified main loop pumping all tabs */
        STATE_SHUTDOWN     /* exiting */
    } app_state_t;

    app_state_t state = STATE_DIRECTORY;
    bool show_directory = false;
    photon_bbs_t reselect_bbs;
    bool has_reselect = false;
    memset(&reselect_bbs, 0, sizeof(reselect_bbs));

    while (state != STATE_SHUTDOWN) {

        /* Steam / Gamescope sends SIGTERM to quit.  Check the flag
         * set by the signal handler so we exit cleanly.
         * On macOS, the window close button sets the quitting flag. */
        if (photonterm_sigterm_pending() || quitting) {
            PHOTON_DBG("SIGTERM received, shutting down");
            state = STATE_SHUTDOWN;
            break;
        }

        if (state == STATE_DIRECTORY) {
            /* ── BBS Directory ─────────────────────────────────────── */
            photon_bbs_t *bbs = photon_bbslist_run(ui, show_directory,
                                                    has_reselect ? &reselect_bbs : NULL);
            show_directory  = false;
            has_reselect    = false;
            photon_sdl_set_fixed_size(sdl, settings.cols, settings.rows);

            if (!bbs) {
                PHOTON_DBG("user cancelled BBS list");
                if (ntabs == 0) { state = STATE_SHUTDOWN; break; }
                /* Return to running tabs */
                switch_tab(sdl, &settings, active_tab);
                state = STATE_RUNNING;
                continue;
            }

            int slot = create_and_connect_tab(sdl, ui, &settings, bbs, ui_vte);
            if (slot < 0) {
                /* Connection failed, stay in directory or resume tabs */
                if (ntabs > 0) {
                    switch_tab(sdl, &settings, active_tab);
                    state = STATE_RUNNING;
                }
                continue;
            }

            /* Connection established - enter main loop */
            state = STATE_RUNNING;
            continue;
        }

        if (state == STATE_RUNNING) {
            /* ── Unified Main Loop ─────────────────────────────────── *
             * All tabs are pumped each iteration.  SDL events are      *
             * processed immediately so tab switching and key events     *
             * are never blocked by data from any connection.            */

            photon_tab_bar_t tabbar = build_tab_bar();

            /* 1. Pump ALL tabs (background tabs process data, no render) */
            for (int i = 0; i < ntabs; i++) {
                if (!tabs[i].active) continue;

                /* Set s_active so VTE response callbacks route correctly */
                photon_conn_set_active(tabs[i].conn);

                if (photon_term_pump_tab(tabs[i].vte, tabs[i].conn,
                                         sdl, i == active_tab, &tabbar)) {
                    tabs[i].dirty = true;
                    if (i != active_tab)
                        tabs[i].activity = true;
                }

                /* Check if background tab disconnected */
                if (i != active_tab &&
                    photon_term_check_connection(tabs[i].conn) == PHOTON_TERM_DISCONNECT) {
                    PHOTON_DBG("background tab %d disconnected", i);
                    /* Close the dead background tab */
                    close_tab(sdl, i);
                    /* Rebuild tabbar after close */
                    tabbar = build_tab_bar();
                    /* Adjust loop index since tabs shifted */
                    i--;
                    continue;
                }
            }

            /* 2. Process all pending SDL events via poll_key (non-blocking) *
             * translate_sdl_event handles mouse selection, wheel, resize,   *
             * window close, and keyboard - all translated to photon_key_t.  */
            photon_key_t key;
            while (photon_sdl_poll_key(sdl, &key)) {
                if (key.code == 0) continue;
                if (ntabs == 0) continue;

                /* Set s_active for the active tab */
                photon_conn_set_active(tabs[active_tab].conn);

                photon_term_result_t r = photon_term_handle_key(
                    &key,
                    tabs[active_tab].vte, tabs[active_tab].conn,
                    sdl, ui, tabs[active_tab].bbs, &settings, &tabbar);

                switch (r) {
                case PHOTON_TERM_CONTINUE:
                    break;

                case PHOTON_TERM_QUIT:
                    state = STATE_SHUTDOWN;
                    break;

                case PHOTON_TERM_DISCONNECT: {
                    int dead = active_tab;
                    if (tabs[dead].bbs) {
                        reselect_bbs = *tabs[dead].bbs;
                        has_reselect = true;
                    }
                    close_tab(sdl, dead);
                    photon_theme_apply(photon_active_theme, sdl, &settings);
                    if (ntabs == 0) {
                        vte_resize(ui_vte, photon_sdl_cols(sdl), photon_sdl_rows(sdl));
                        photon_sdl_set_ttf_mode(sdl, false);
                        show_directory = true;
                        state = STATE_DIRECTORY;
                    } else {
                        switch_tab(sdl, &settings, active_tab);
                    }
                    break;
                }

                case PHOTON_TERM_NEWTAB:
                    photon_theme_apply(photon_active_theme, sdl, &settings);
                    photon_sdl_set_ttf_mode(sdl, false);
                    vte_resize(ui_vte, photon_sdl_cols(sdl), photon_sdl_rows(sdl));
                    show_directory = true;
                    state = STATE_DIRECTORY;
                    break;

                case PHOTON_TERM_SWITCH_TAB: {
                    int target = photon_switch_tab_target;
                    PHOTON_DBG("switch_tab: target=%d current=%d ntabs=%d",
                               target, active_tab, ntabs);
                    if (target >= 0 && target < ntabs && target != active_tab)
                        switch_tab(sdl, &settings, target);
                    break;
                }

                case PHOTON_TERM_RESUME:
                    break;
                }

                if (state != STATE_RUNNING) break;
            }

            if (state != STATE_RUNNING) continue;

            /* 3. Render active tab */
            if (ntabs > 0 && tabs[active_tab].active) {
                tabbar = build_tab_bar();
                photon_conn_set_active(tabs[active_tab].conn);
                photon_term_render(sdl, tabs[active_tab].vte, &tabbar,
                                   tabs[active_tab].dirty);
                tabs[active_tab].dirty = false;
            }

            /* 4. Brief sleep to avoid busy-looping when idle */
            struct timespec ts = {0, 1000000L};  /* 1 ms */
            nanosleep(&ts, NULL);
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
