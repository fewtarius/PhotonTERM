/* photon_debug.h - debug logging for PhotonTERM
 * Copyright (C) 2026 fewtarius and PhotonTERM contributors
 * SPDX-License-Identifier: GPL-3.0-or-later */
/*
 *
 * Usage:
 *   PHOTON_DBG("connected to %s port %d", host, port);
 *   PHOTON_DBG_ENTER();  // at function entry
 *   PHOTON_DBG_EXIT("returned %d", rc);  // at function exit
 *
 * Output goes to a per-process log file when --debug is passed.
 *   macOS/Linux: /tmp/photonterm-<PID>.log
 *   Windows:     %TEMP%\photonterm-<PID>.log
 * Thread-safe.  Zero cost when disabled (all macros expand to nothing).
 */

#ifndef PHOTON_DEBUG_H
#define PHOTON_DEBUG_H

#include <stdbool.h>
#include <stdarg.h>
#include <stdio.h>
#include <time.h>

#ifdef _WIN32
# include <windows.h>
# include <process.h>
# define photon_getpid() ((int)_getpid())
#else
# include <pthread.h>
# include <unistd.h>
# define photon_getpid() ((int)getpid())
#endif

/* Windows doesn't have clock_gettime(CLOCK_MONOTONIC) pre-VS2019.
   Fall back to time(NULL) on Windows. */
#ifdef _WIN32
# define PHOTON_CLOCK_GET(ts) do { \
	(ts).tv_sec  = (long)time(NULL); \
	(ts).tv_nsec = 0; \
} while (0)
#else
# define PHOTON_CLOCK_GET(ts) clock_gettime(CLOCK_MONOTONIC, &(ts))
#endif

/* Set by main() when --debug is passed */
extern bool photon_debug_enabled;
/* Set by photon_debug_open() */
extern FILE *photon_debug_fp;

/* Open the debug log file.  Called once from main() if --debug is set.
 * Prints the log path to stderr so the user knows where to look. */
static inline void
photon_debug_open(void)
{
	char path[512];
#ifdef _WIN32
	char tmpdir[MAX_PATH];
	if (!GetTempPathA(sizeof(tmpdir), tmpdir))
		strcpy(tmpdir, "C:\\Temp\\");
	/* Use a fixed name on Windows so the user always knows where to look
	 * (the GUI subsystem has no console for stderr). */
	snprintf(path, sizeof(path), "%sphotonterm.log", tmpdir);
#else
	snprintf(path, sizeof(path), "/tmp/photonterm-%d.log", photon_getpid());
#endif
	photon_debug_fp = fopen(path, "w");
	if (photon_debug_fp) {
		setvbuf(photon_debug_fp, NULL, _IOLBF, 0); /* line-buffered */
		fprintf(stderr, "[photonterm] debug log: %s\n", path);
#ifdef _WIN32
		/* GUI subsystem has no stderr; announce via Win32 debug output
		 * (visible in DebugView / Visual Studio output window). */
		{
			char msg[600];
			snprintf(msg, sizeof(msg), "[photonterm] debug log: %s\n", path);
			OutputDebugStringA(msg);
		}
#endif
	} else {
		fprintf(stderr, "[photonterm] warning: could not open debug log %s\n", path);
	}
}

/* Internal: write one log line.  Not called directly - use PHOTON_DBG(). */
static inline void
photon_debug_write(const char *file, int line, const char *func,
                   const char *fmt, ...)
#if defined(__GNUC__) || defined(__clang__)
#if defined(__MINGW32__)
	__attribute__((format(__MINGW_PRINTF_FORMAT, 4, 5)))
#else
	__attribute__((format(printf, 4, 5)))
#endif
#endif
	;

static inline void
photon_debug_write(const char *file, int line, const char *func,
                   const char *fmt, ...)
{
#ifdef _WIN32
	/* Use a CRITICAL_SECTION to avoid pulling in pthreads on Windows.
	 * One-time init via the Win32 once-init pattern. */
	static CRITICAL_SECTION dbg_cs;
	static INIT_ONCE        dbg_once = INIT_ONCE_STATIC_INIT;
	BOOL pending;
	InitOnceBeginInitialize(&dbg_once, 0, &pending, NULL);
	if (pending)
		InitializeCriticalSection(&dbg_cs);
	InitOnceComplete(&dbg_once, 0, NULL);
#else
	static pthread_mutex_t dbg_lock = PTHREAD_MUTEX_INITIALIZER;
#endif
	struct timespec ts;
	va_list ap;
	char    buf[2048];

	if (!photon_debug_fp)
		return;

	PHOTON_CLOCK_GET(ts);

	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);

#ifdef _WIN32
	EnterCriticalSection(&dbg_cs);
#else
	pthread_mutex_lock(&dbg_lock);
#endif
	fprintf(photon_debug_fp, "[%6ld.%03ld] %-20s +%-4d %-30s | %s\n",
	        (long)ts.tv_sec,
	        (long)(ts.tv_nsec / 1000000L),
	        file, line, func, buf);
#ifdef _WIN32
	LeaveCriticalSection(&dbg_cs);
#else
	pthread_mutex_unlock(&dbg_lock);
#endif
}

/* Strip directory from __FILE__ for brevity */
#define PHOTON_BASENAME(f) (strrchr((f), '/') ? strrchr((f), '/') + 1 : \
                            (strrchr((f), '\\') ? strrchr((f), '\\') + 1 : (f)))

/*
 * PHOTON_DBG(fmt, ...) - the primary logging macro.
 * Compiles to nothing when debug is disabled.
 */
#ifdef PHOTON_DEBUG_BUILD
# define PHOTON_DBG(fmt, ...) \
	do { \
		if (photon_debug_enabled) \
			photon_debug_write(PHOTON_BASENAME(__FILE__), __LINE__, \
			                   __func__, fmt, ##__VA_ARGS__); \
	} while (0)
# define PHOTON_DBG_ENTER() \
	PHOTON_DBG(">>> enter")
# define PHOTON_DBG_EXIT(fmt, ...) \
	PHOTON_DBG("<<< exit " fmt, ##__VA_ARGS__)
#else
# define PHOTON_DBG(fmt, ...)    do {} while (0)
# define PHOTON_DBG_ENTER()      do {} while (0)
# define PHOTON_DBG_EXIT(...)    do {} while (0)
#endif

#endif /* PHOTON_DEBUG_H */
