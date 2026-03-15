/* early_init.c - process initialisation that must run before main()
 * Copyright (C) 2026 fewtarius and PhotonTERM contributors
 * SPDX-License-Identifier: GPL-3.0-or-later */

/* On Darwin, this is handled in DarwinWrappers.m.
 * This file covers Linux and other Unix platforms (but not Windows). */
#if !defined(__APPLE__) && !defined(_WIN32)

#include <signal.h>
#include <stdlib.h>

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
	setenv("SDL_NO_SIGNAL_HANDLERS", "1", 1);
	signal(SIGINT,  SIG_IGN);
	signal(SIGTERM, SIG_IGN);
	signal(SIGHUP,  SIG_IGN);
	signal(SIGPIPE, SIG_IGN);
}

#endif /* !__APPLE__ && !_WIN32 */
