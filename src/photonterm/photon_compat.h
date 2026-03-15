/* photon_compat.h - Portable compatibility shims for PhotonTERM
 *
 * Copyright (C) 2026 fewtarius and PhotonTERM contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * POSIX portability layer providing safe string functions, path helpers,
 * strwrap.h, xpprintf.h, threadwrap.h, datewrap.h, filewrap.h, sockwrap.h,
 * netwrap.h, xpbeep.h) with a single portable header.
 *
 * All macros and inline functions use the same names as xpdev so existing
 * PhotonTERM source files can switch by replacing:
 *   #include "gen_defs.h"   (and the rest)
 * with:
 *   #include "photon_compat.h"
 *
 * POSIX (macOS, Linux) and Win32 (MinGW64) are supported.
 */

#pragma once

/* ── System headers ─────────────────────────────────────────────────── */

#include <assert.h>
#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#if defined(_WIN32)
    /* MinGW64 */
    #include <winsock2.h>
    #include <windows.h>
    #include <ws2tcpip.h>
    #include <io.h>
    #include <process.h>
    #include <direct.h>
#else
    #include <dirent.h>
    #include <fcntl.h>
    #include <netdb.h>
    #include <netinet/in.h>
    #include <pthread.h>
    #include <sys/socket.h>
    #include <sys/stat.h>
    #include <sys/time.h>
    #include <sys/types.h>
    #include <unistd.h>
#endif

/* ── Boolean ────────────────────────────────────────────────────────── */

#ifndef TRUE
#  define TRUE  1
#endif
#ifndef FALSE
#  define FALSE 0
#endif
#ifndef BOOL
#  define BOOL int
#endif

/* ── NULL guard ─────────────────────────────────────────────────────── */

#ifndef NULL
#  define NULL ((void *)0)
#endif

/* ── Path ───────────────────────────────────────────────────────────── */

#ifndef MAX_PATH
#  if defined(_WIN32)
#    define MAX_PATH 260
#  elif defined(PATH_MAX)
#    define MAX_PATH PATH_MAX
#  else
#    define MAX_PATH 4096
#  endif
#endif

/* Ensure a path ends with a directory separator (modifies in place, returns p) */
static inline char *backslash(char *path)
{
    size_t n = strlen(path);
    if (n == 0) return path;
    char last = path[n - 1];
#if defined(_WIN32)
    if (last != '/' && last != '\\') {
        path[n]   = '\\';
        path[n+1] = '\0';
    }
#else
    if (last != '/') {
        path[n]   = '/';
        path[n+1] = '\0';
    }
#endif
    return path;
}

/* Return a pointer to the filename component of path (basename without alloc) */
static inline char *getfname(const char *path)
{
    const char *p = path;
    const char *last = path;
    while (*p) {
        if (*p == '/' || *p == '\\') last = p + 1;
        p++;
    }
    return (char *)last;
}

/* Resolve relative path to absolute (cross-platform realpath wrapper) */
#if defined(_WIN32)
#  define FULLPATH(abs, rel, sz) \
    (void)(_fullpath((abs), (rel), (sz)) == NULL ? (abs)[0] = '\0', 0 : 0)
#else
static inline void photon_fullpath(char *abs, const char *rel, size_t sz)
{
    char *r = realpath(rel, NULL);
    if (r) {
        snprintf(abs, sz, "%s", r);
        free(r);
    } else {
        /* fallback: just copy if realpath fails (path may not exist yet) */
        snprintf(abs, sz, "%s", rel);
    }
}
#  define FULLPATH(abs, rel, sz) photon_fullpath((abs), (rel), (sz))
#endif

/* ── Socket types ───────────────────────────────────────────────────── */

#if defined(_WIN32)
    /* SOCKET, INVALID_SOCKET already defined by winsock2.h */
#else
    typedef int SOCKET;
#   ifndef INVALID_SOCKET
#     define INVALID_SOCKET (-1)
#   endif
#endif

/* ── Memory helpers ─────────────────────────────────────────────────── */

#define FREE_AND_NULL(p) do { free(p); (p) = NULL; } while (0)

/* ── Safe string copy/cat ───────────────────────────────────────────── */

/* strlcpy / strlcat - available on macOS/BSDs; provide for Linux/Windows */
/* glibc 2.38+ (Arch, Ubuntu 24.04+) provides strlcpy/strlcat in <string.h>
 * macOS/BSDs always have them. Provide our own only when the system doesn't. */
#if !defined(__APPLE__) && \
    !defined(__FreeBSD__) && !defined(__OpenBSD__) && !defined(__NetBSD__) && \
    !(defined(__GLIBC__) && (__GLIBC__ > 2 || (__GLIBC__ == 2 && __GLIBC_MINOR__ >= 38)))
static inline size_t strlcpy(char *dst, const char *src, size_t size)
{
    size_t srclen = strlen(src);
    if (size > 0) {
        size_t copy = srclen < size - 1 ? srclen : size - 1;
        memcpy(dst, src, copy);
        dst[copy] = '\0';
    }
    return srclen;
}

static inline size_t strlcat(char *dst, const char *src, size_t size)
{
    size_t dstlen = strnlen(dst, size);
    if (dstlen >= size) return size + strlen(src);
    return dstlen + strlcpy(dst + dstlen, src, size - dstlen);
}
#endif

/* SAFECOPY: safe strcpy into fixed-size array (sizeof(dst) must be known) */
#define SAFECOPY(dst, src)   (void)strlcpy((dst), (src), sizeof(dst))

/* SAFEPRINTF family: snprintf into fixed-size array */
#define SAFEPRINTF(dst, fmt, a1)             \
    (void)snprintf((dst), sizeof(dst), (fmt), (a1))
#define SAFEPRINTF2(dst, fmt, a1, a2)        \
    (void)snprintf((dst), sizeof(dst), (fmt), (a1), (a2))
#define SAFEPRINTF3(dst, fmt, a1, a2, a3)    \
    (void)snprintf((dst), sizeof(dst), (fmt), (a1), (a2), (a3))
#define SAFEPRINTF4(dst, fmt, a1, a2, a3, a4) \
    (void)snprintf((dst), sizeof(dst), (fmt), (a1), (a2), (a3), (a4))

/* ── asprintf (POSIX; needed on Windows) ───────────────────────────── */

#if defined(_MSC_VER) && !defined(vasprintf)
/* MSVC lacks vasprintf/asprintf; provide inline fallbacks. */
static inline int vasprintf(char **strp, const char *fmt, va_list ap)
{
    va_list ap2;
    va_copy(ap2, ap);
    int n = vsnprintf(NULL, 0, fmt, ap2);
    va_end(ap2);
    if (n < 0) { *strp = NULL; return n; }
    *strp = (char *)malloc((size_t)n + 1);
    if (!*strp) return -1;
    return vsnprintf(*strp, (size_t)n + 1, fmt, ap);
}
static inline int asprintf(char **strp, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int r = vasprintf(strp, fmt, ap);
    va_end(ap);
    return r;
}
#elif !defined(__APPLE__) && !defined(_WIN32) && defined(__linux__)
    /* Linux glibc has asprintf in stdio.h; just ensure _GNU_SOURCE is set */
    #ifndef _GNU_SOURCE
    #  define _GNU_SOURCE
    #endif
#endif
/* MinGW-w64 provides vasprintf/asprintf in stdio.h with _GNU_SOURCE. */

/* xp_asprintf compatibility alias */
static inline char *xp_asprintf(const char *fmt, ...)
{
    char *str = NULL;
    va_list ap;
    va_start(ap, fmt);
    if (vasprintf(&str, fmt, ap) < 0) str = NULL;
    va_end(ap);
    return str;
}
static inline void xp_asprintf_free(char *str) { free(str); }

/* ── Sleep ──────────────────────────────────────────────────────────── */

/* SLEEP(ms) - millisecond sleep, cross-platform */
#if defined(_WIN32)
#  define SLEEP(ms)   Sleep((DWORD)(ms))
#else
#  include <unistd.h>
#  define SLEEP(ms)   usleep((useconds_t)(ms) * 1000)
#endif

/* ── Monotonic timer (replaces xp_timer) ───────────────────────────── */

/* Returns seconds as long double, monotonic, high resolution */
#ifndef _GENWRAP_H
static inline long double xp_timer(void)
{
#if defined(_WIN32)
    LARGE_INTEGER freq, tick;
    if (QueryPerformanceFrequency(&freq) && QueryPerformanceCounter(&tick))
        return (long double)tick.QuadPart / (long double)freq.QuadPart;
    return (long double)GetTickCount64() / 1000.0L;
#else
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) == 0)
        return (long double)ts.tv_sec + (long double)ts.tv_nsec / 1000000000.0L;
    return -1.0L;
#endif
}
#endif /* _GENWRAP_H */

/* ── POSIX threading (replaces threadwrap) ──────────────────────────── */

/* PhotonTERM's own code uses pthreads directly; we just need the types
 * and THREAD_MUTEX_INITIALIZER available everywhere. */
#if defined(_WIN32)
    /* MinGW64 ships pthreads-win32 as pthread.h */
    #include <pthread.h>
#endif

/* ── File/directory ─────────────────────────────────────────────────── */

/* STDOUT_FILENO is guaranteed by <unistd.h> on POSIX; define for Windows */
#if defined(_WIN32) && !defined(STDOUT_FILENO)
#  define STDOUT_FILENO 1
#endif

/* ── Network ────────────────────────────────────────────────────────── */

/* getaddrinfo/freeaddrinfo are POSIX + WinSock2; already included above */

/* ── BBS emulation string helper (was in genwrap / bbslist) ────────── */

/* Defined in bbslist.c; declared here so conn_telnet.c and ssh.c can see it */
struct bbslist;
const char *get_emulation_str(struct bbslist *bbs);

/* ── xpmap stub (used only in term.c for key mapping files) ─────────── */

/* We keep the minimal xpmap API signature; actual implementation retained
 * in src/xpdev/xpmap.c until term.c is rewritten. */

/* ── Numeric limits (may be missing on some platforms) ─────────────── */

#ifndef LLONG_MAX
#  define LLONG_MAX  9223372036854775807LL
#endif
#ifndef LLONG_MIN
#  define LLONG_MIN  (-LLONG_MAX - 1LL)
#endif
#ifndef ULLONG_MAX
#  define ULLONG_MAX 18446744073709551615ULL
#endif

/* ── Misc gen_defs macros used in PhotonTERM ───────────────────────── */

/* TERMINATE: ensure buffer is NUL-terminated at last byte */
#define TERMINATE(str)   ((str)[sizeof(str) - 1] = '\0')

/* MIN / MAX */
#ifndef MIN
#  define MIN(a, b)  ((a) < (b) ? (a) : (b))
#endif
#ifndef MAX
#  define MAX(a, b)  ((a) > (b) ? (a) : (b))
#endif

/* ── Basename without allocation ────────────────────────────────────── */

/* ── Terminal emulation type ────────────────────────────────────────── */

/* photon_emulation_t: defined in photon_conn.h (avoids conflict with
 * cterm_emulation_t from cterm.h when both headers are used together).
 * If you need emulation types, include photon_conn.h, not photon_compat.h. */

/* ── nanosleep (Windows without POSIX layer) ────────────────────────── */
#if defined(_WIN32) && !defined(_POSIX_C_SOURCE) && !defined(__MINGW64_VERSION_MAJOR)
#include <windows.h>
static inline int nanosleep(const struct timespec *req, struct timespec *rem) {
    (void)rem;
    DWORD ms = (DWORD)(req->tv_sec * 1000 + req->tv_nsec / 1000000);
    if (ms == 0) ms = 1;
    Sleep(ms);
    return 0;
}
#endif

/* ── Audio beep stub (replaces xpbeep) ─────────────────────────────── */

/* PhotonTERM queries xpbeep_sound_devices_enabled; provide a global that
 * bbslist.c can use.  Actual beep via SDL_audio may be wired up later. */
#ifndef XPBEEP_DEFINED
#  define XPBEEP_DEFINED 1
typedef struct {
    const char *name;
    int         bit;
} xpbeep_audio_type_t;
extern unsigned int xpbeep_sound_devices_enabled;
#endif
