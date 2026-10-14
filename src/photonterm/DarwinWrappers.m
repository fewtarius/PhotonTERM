/* DarwinWrappers.m - macOS Objective-C utility wrappers for PhotonTERM
 * Copyright (C) 2026 fewtarius and PhotonTERM contributors
 * SPDX-License-Identifier: GPL-3.0-or-later */

#include <stdbool.h>
#include <stdlib.h>
#include <signal.h>
#include <string.h>
#include "photon_compat.h"
#undef BOOL
#import <Foundation/Foundation.h>

/* Flag set by SIGTERM handler so the main loop can exit cleanly. */
static volatile sig_atomic_t g_sigterm_pending = 0;

int photonterm_sigterm_pending(void)
{
	return g_sigterm_pending;
}

static void sigterm_handler(int sig)
{
	(void)sig;
	g_sigterm_pending = 1;
}

/*
 * Prevent SDL2 from installing its own signal handlers.  SDL's
 * SDL_Init(SDL_INIT_VIDEO) calls SDL_InstallParachute() which would
 * override any signal() calls we make later.  The only reliable fix
 * is to set SDL_NO_SIGNAL_HANDLERS before SDL_Init runs.  On macOS
 * SDL_Init runs on the OS main thread before our code, so setenv()
 * from inside our code is too late.
 */
__attribute__((constructor))
static void
photonterm_early_init(void)
{
	setenv("SDL_NO_SIGNAL_HANDLERS", "1", 1 /* override */);
	signal(SIGINT,  SIG_IGN);
	signal(SIGTERM, sigterm_handler);
	signal(SIGHUP,  SIG_IGN);
	signal(SIGPIPE, SIG_IGN);
}
