/* DarwinWrappers.m - macOS Objective-C utility wrappers for PhotonTERM
 * Copyright (C) 2026 fewtarius and PhotonTERM contributors
 * SPDX-License-Identifier: GPL-3.0-or-later */

#include <stdbool.h>
#include <stdlib.h>
#include <signal.h>
#include <string.h>
#include "photon_compat.h"
#include "photon_paths.h"
/* quitting extern - declared in photon_main.c */
extern bool quitting;
#undef BOOL
#import <AppKit/AppKit.h>
#import <objc/runtime.h>

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
	/* Ignore SIGINT/SIGTERM/SIGHUP/SIGPIPE; Ctrl-C passthrough is
	 * handled via NSApp delegate swizzle (photonterm_install_app_delegate). */
	signal(SIGINT,  SIG_IGN);
	signal(SIGTERM, SIG_IGN);
	signal(SIGHUP,  SIG_IGN);
	signal(SIGPIPE, SIG_IGN);
}

#import <Foundation/Foundation.h>

/*
 * Prevent Ctrl-C from killing the app via macOS NSApplication.
 *
 * macOS routes Ctrl-C through NSApplication's -applicationShouldTerminate:
 * delegate call.  SDL2 implements this delegate and posts SDL_QUIT when
 * it returns YES.  Our doterm() treats SDL_QUIT as CIO_KEY_QUIT which
 * closes the active session, breaking Ctrl-C pass-through to BBS sessions.
 *
 * We override -applicationShouldTerminate: via a category on NSObject
 * and method swizzling to cancel app termination.  Instead we use a
 * volatile flag that the main connection loop reads to send ^C.
 * The window close button and Cmd-Q paths remain unaffected.
 *
 * photonterm_ctrl_c_pending is set here and cleared in doterm() after
 * the ^C byte is sent to the connection.
 */
volatile int photonterm_ctrl_c_pending = 0;

/*
 * We use Objective-C method swizzling to intercept SDL2's NSApplication
 * delegate -applicationShouldTerminate: without replacing SDL's delegate
 * (which would break SDL's keyboard and event processing entirely).
 *
 * At runtime we look up the class of NSApp.delegate (which is SDL's
 * SDLAppDelegate), add a swizzled implementation of the method, and
 * inject our Ctrl-C passthrough logic there.
 */

/* Swizzled implementation: called INSTEAD of SDL's original method. */
static NSApplicationTerminateReply
photon_applicationShouldTerminate(id self, SEL _cmd, NSApplication *sender)
{
	/* If PhotonTERM is already in the process of quitting, honour it. */
	if (quitting)  /* from photonterm.h */
		return NSTerminateNow;
	/* Ctrl-C from macOS: inject ^C into the BBS session. */
	photonterm_ctrl_c_pending = 1;
	return NSTerminateCancel;
}

/*
 * Called after SDL initialises NSApp.  We swizzle the delegate's
 * applicationShouldTerminate: rather than replacing the delegate object.
 */
void
photonterm_install_app_delegate(void)
{
	id delegate = NSApp.delegate;
	if (!delegate)
		return;
	Class cls = [delegate class];
	SEL sel = @selector(applicationShouldTerminate:);
	Method orig = class_getInstanceMethod(cls, sel);
	if (orig) {
		method_setImplementation(orig, (IMP)photon_applicationShouldTerminate);
	} else {
		class_addMethod(cls, sel, (IMP)photon_applicationShouldTerminate, "@l:@");
	}
}

char *
get_photon_filename(char *fn, int fnlen, photon_path_type_t type, int shared)
{
	NSSearchPathDomainMask domain = shared ? NSLocalDomainMask : NSUserDomainMask;
	NSSearchPathDirectory path;
	switch(type) {
		case PHOTONTERM_PATH_INI:
			path = NSLibraryDirectory;
			break;
		case PHOTONTERM_PATH_LIST:
			path = NSLibraryDirectory;
			break;
		case PHOTONTERM_DEFAULT_TRANSFER_PATH:
			path = NSDownloadsDirectory;
			break;
		case PHOTONTERM_PATH_CACHE:
			path = NSCachesDirectory;
			break;
		case PHOTONTERM_PATH_KEYS:
			path = NSLibraryDirectory;
			break;
		default:
			path = NSLibraryDirectory;
			break;
	}

	NSError *error = nil;
	NSURL *none = nil;
	NSURL *result = [[NSFileManager defaultManager] URLForDirectory:path inDomain:domain appropriateForURL:none create:NO error:&error];
	if (result == nil) {
		strlcpy(fn, error.localizedDescription.UTF8String, fnlen);
		return NULL;
	}
	strlcpy(fn, result.path.UTF8String, fnlen);

	if (type == PHOTONTERM_DEFAULT_TRANSFER_PATH) {
		strlcat(fn, "/", fnlen);
		return fn;
	}

	switch(type) {
		case PHOTONTERM_PATH_INI:
			strlcat(fn, "/Preferences/PhotonTERM", fnlen);
			photon_mkdir_p(fn);
			strlcat(fn, "/PhotonTERM.ini", fnlen);
			break;
		case PHOTONTERM_PATH_LIST:
			strlcat(fn, "/Preferences/PhotonTERM", fnlen);
			photon_mkdir_p(fn);
			strlcat(fn, "/PhotonTERM.lst", fnlen);
			break;
		case PHOTONTERM_PATH_CACHE:
			strlcat(fn, "/PhotonTERM", fnlen);
			photon_mkdir_p(fn);
			strlcat(fn, "/", fnlen);
			break;
		case PHOTONTERM_PATH_KEYS:
			strlcat(fn, "/Preferences/PhotonTERM", fnlen);
			photon_mkdir_p(fn);
			strlcat(fn, "/PhotonTERM.ssh", fnlen);
			break;
		default:
			break;
	}

	return fn;
}

