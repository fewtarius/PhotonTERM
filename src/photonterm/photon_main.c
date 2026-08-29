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
const char *photonterm_version = "PhotonTERM 20260619.1";

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

/* Build tab bar info struct for passing to photon_term functions. */
static photon_tab_bar_t build_tab_bar(void)
{
    photon_tab_bar_t tb = { .ntabs = ntabs, .active = active_tab };
    tb.conn_type = tabs[active_tab].bbs ? tabs[active_tab].bbs->conn_type : PHOTON_CONN_TELNET;
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

    tabs[active_tab].activity = false;
    active_tab = idx;
    tabs[active_tab].activity = false;

    /* Switch connection and notify it of the current terminal size.
     * This is critical after a window resize: the active tab's VTE was
     * resized in the resize handler, but background tabs' connections
     * were never notified. Switching to them now must push the correct
     * SIGWINCH/NAWS so the remote terminal adjusts. */
    photon_conn_set_active(tabs[active_tab].conn);
    {
        int nc = photon_sdl_cols(sdl);
        int nr = photon_sdl_rows(sdl) - 1;
        if (nr < 1) nr = 1;
        photon_conn_resize(nc, nr);
    }

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
    photon_sdl_present(sdl);
    photon_term_render_force_next();
}

/* Resize all tab VTEs to account for status bar row.
 * The last row is always reserved for the status bar. */
static void resize_tabs_for_bar(photon_sdl_t *sdl)
{
    int nc = photon_sdl_cols(sdl);
    int nr = photon_sdl_rows(sdl) - 1;
    int term_rows = nr;
    if (term_rows < 1) term_rows = 1;
    for (int i = 0; i < ntabs; i++) {
        if (tabs[i].active) {
            vte_resize(tabs[i].vte, nc, term_rows);
            photon_conn_set_active(tabs[i].conn);
            photon_conn_resize(nc, term_rows);
        }
    }
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

    /* Tab count changed - resize VTEs for tab bar row */
    resize_tabs_for_bar(sdl);
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

    /* Tab count changed - resize VTEs for tab bar row */
    resize_tabs_for_bar(sdl);

    /* Auto-detect window size (account for tab bar row) */
    if (settings->cols == 0 && settings->rows == 0) {
        int term_rows = photon_sdl_rows(sdl) - 1;
        if (term_rows < 1) term_rows = 1;
        bbs->init_cols = photon_sdl_cols(sdl);
        bbs->init_rows = term_rows;
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
    return slot;
}

/* ── Entry point ─────────────────────────────────────────────────────── */

int main(int argc, char **argv)
{
    bool start_fullscreen = false;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--debug") == 0) {
            photon_debug_enabled = true;
            photon_debug_open();
        } else if (strcmp(argv[i], "--fullscreen") == 0) {
            start_fullscreen = true;
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

    if (start_fullscreen)
        photon_sdl_enter_fullscreen(sdl);

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

    /* Install tab bar mouse filter to intercept clicks in chrome area */
    photon_term_install_mouse_filter(sdl);
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

        /* Always process resize events, regardless of app state.
         * This ensures the SDL context dimensions are updated for
         * fullscreen transitions and window resizing, even when
         * the BBS directory is showing. */
        {
            int nc = 0, nr = 0;
            if (photon_sdl_check_resize(sdl, &nc, &nr)) {
                if (ntabs > 0 && tabs[active_tab].active) {
                    /* Reserve one row for status bar */
                    int term_rows = nr - 1;
                    if (term_rows < 1) term_rows = 1;
                    for (int i = 0; i < ntabs; i++) {
                        if (tabs[i].active)
                            vte_resize(tabs[i].vte, nc, term_rows);
                    }
                    /* Notify the active connection of the new size */
                    photon_conn_set_active(tabs[active_tab].conn);
                    photon_conn_resize(nc, term_rows);
                }
                /* Force immediate render after resize */
                photon_term_render_force_next();
            }
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
             * Priority: SDL events > active tab data > background tabs. *
             * SDL events are processed first so key input (tab switch,  *
             * quit) is never blocked by data processing. Background     *
             * tabs get one batch per iteration to limit CPU usage.       */

            photon_tab_bar_t tabbar = build_tab_bar();

            /* ── Decoupled main loop ────────────────────────────────── *
             * Events and data pumping run every iteration (1ms tick).  *
             * Rendering is gated by the 60fps frame timer so the GPU   *
             * is never double-booked.  This keeps tab switching and    *
             * key input responsive even when the GPU is busy with      *
             * local LLM inference or other heavy work.                 */

            /* 1. Process ALL pending SDL events (keys, resize, etc.)
             * This runs every iteration - never gated by render timing. */
            photon_key_t key;
            while (photon_sdl_poll_key(sdl, &key)) {
                if (key.code == 0) continue;
                if (ntabs == 0) continue;

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

            /* Persist font size changes from hotkey (Alt+Plus/Minus) */
            if (photon_sdl_font_size_changed(sdl)) {
                settings.ttf_size_pt = photon_sdl_get_font_size(sdl);
                photon_settings_save(&settings);
                photon_sdl_clear_font_size_changed(sdl);
                PHOTON_DBG("persisted font size to settings: %dpt",
                           settings.ttf_size_pt);
            }

            /* If font size changed (via hotkey or fullscreen auto-fit),
             * the grid dimensions may have changed.  Resize the active
             * tab's VTE and notify the connection (SIGWINCH/NAWS). */
            if (ntabs > 0 && tabs[active_tab].active) {
                int nc = photon_sdl_cols(sdl);
                int nr = photon_sdl_rows(sdl);
                /* Reserve last row for status bar */
                int term_rows = nr - 1;
                if (term_rows < 1) term_rows = 1;
                if (vte_cols(tabs[active_tab].vte) != nc ||
                    vte_rows(tabs[active_tab].vte) != term_rows) {
                    PHOTON_DBG("grid changed %dx%d -> %dx%d, resizing VTE",
                               vte_cols(tabs[active_tab].vte),
                               vte_rows(tabs[active_tab].vte),
                               nc, term_rows);
                    vte_resize(tabs[active_tab].vte, nc, term_rows);
                    photon_conn_set_active(tabs[active_tab].conn);
                    photon_conn_resize(nc, term_rows);
                }
            }

            if (state != STATE_RUNNING) continue;

            /* 2. Pump tab data: active tab first, then background tabs.
             * Active tab drains all available data (user is watching it).
             * Background tabs get one batch per iteration to limit CPU. */
            for (int i = 0; i < ntabs; i++) {
                if (!tabs[i].active) continue;

                photon_conn_set_active(tabs[i].conn);

                if (photon_term_pump_tab(tabs[i].vte, tabs[i].conn,
                                         i == active_tab)) {
                    tabs[i].dirty = true;
                    if (i != active_tab)
                        tabs[i].activity = true;
                }

                /* Check if tab disconnected */
                if (photon_term_check_connection(tabs[i].conn) == PHOTON_TERM_DISCONNECT) {
                    if (i == active_tab) {
                        PHOTON_DBG("active tab %d disconnected", i);
                        if (tabs[i].bbs) {
                            reselect_bbs = *tabs[i].bbs;
                            has_reselect = true;
                        }
                        close_tab(sdl, i);
                        photon_theme_apply(photon_active_theme, sdl, &settings);
                        if (ntabs == 0) {
                            vte_resize(ui_vte, photon_sdl_cols(sdl),
                                       photon_sdl_rows(sdl));
                            photon_sdl_set_ttf_mode(sdl, false);
                            show_directory = true;
                            state = STATE_DIRECTORY;
                        } else {
                            switch_tab(sdl, &settings, active_tab);
                        }
                        break;
                    } else {
                        PHOTON_DBG("background tab %d disconnected", i);
                        close_tab(sdl, i);
                        tabbar = build_tab_bar();
                        i--;
                        continue;
                    }
                }
            }

            if (state != STATE_RUNNING) continue;

            /* 3. Render active tab (gated by frame timer). */
            if (photon_sdl_take_expose(sdl))
                photon_term_render_force_next();

            if (ntabs > 0 && tabs[active_tab].active) {
                tabbar = build_tab_bar();
                photon_conn_set_active(tabs[active_tab].conn);
                photon_term_render(sdl, tabs[active_tab].vte, &tabbar,
                                   tabs[active_tab].dirty);
                tabs[active_tab].dirty = false;
            }

            /* 4. Brief sleep to avoid busy-looping when idle.
             * 1ms gives the OS a chance to schedule other work (including
             * GPU inference) while still being fast enough for responsive
             * input processing. */
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
