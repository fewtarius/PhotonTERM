/* early_init.c - process initialisation that must run before main()
 * Copyright (C) 2026 fewtarius and PhotonTERM contributors
 * SPDX-License-Identifier: GPL-3.0-or-later */

/* On Darwin, this is handled in DarwinWrappers.m.
 * This file covers Linux and other Unix platforms (but not Windows). */
#if !defined(__APPLE__) && !defined(_WIN32)

#define PHOTON_DEBUG_BUILD
#include "photon_debug.h"

#include <signal.h>
#include <stdlib.h>
#include <string.h>

/* Flag set by SIGTERM handler so the main loop can exit cleanly.
 * Steam / Gamescope sends SIGTERM to quit the application. */
static volatile sig_atomic_t g_sigterm_pending = 0;

int photonterm_sigterm_pending(void)
{
	PHOTON_DBG_ENTER();
	PHOTON_DBG("g_sigterm_pending=%d", (int)g_sigterm_pending);
	PHOTON_DBG_EXIT("returning %d", (int)g_sigterm_pending);
	return g_sigterm_pending;
}

static void sigterm_handler(int sig)
{
	(void)sig;
	g_sigterm_pending = 1;
}

/*
 * Prevent SDL2 from installing its own SIGINT handler.
 * SDL2's SDL_Init(SDL_INIT_VIDEO) calls SDL_InstallParinterruptHandlers()
 * which installs a SIGINT handler that generates SDL_QUIT and then may kill
 * the process.  SDL_Init runs before our code on the main thread, so
 * setenv() from inside our code is too late.  Use a constructor that
 * fires before main() itself.
 */
__attribute__((constructor))
static void
photonterm_early_init(void)
{
	PHOTON_DBG_ENTER();
	PHOTON_DBG("setting SDL_NO_SIGNAL_HANDLERS=1 to suppress SDL SIGINT handler");
	setenv("SDL_NO_SIGNAL_HANDLERS", "1", 1);
	PHOTON_DBG("unsetting LD_PRELOAD to avoid Steam 32-bit ld.so.preload conflict");
	unsetenv("LD_PRELOAD");
	PHOTON_DBG("installing signal handlers: SIGINT=ignore, SIGTERM=handler, SIGHUP=ignore, SIGPIPE=ignore");
	signal(SIGINT,  SIG_IGN);
	signal(SIGTERM, sigterm_handler);
	signal(SIGHUP,  SIG_IGN);
	signal(SIGPIPE, SIG_IGN);
	PHOTON_DBG_EXIT();
}

#endif /* !__APPLE__ && !_WIN32 */
