/* photon_paths.h - PhotonTERM configuration file path management
 *
 * Copyright (C) 2026 fewtarius and PhotonTERM contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Provides:
 *   photon_path_type_t  - enum of well-known config paths
 *   photon_mkdir_p()    - portable recursive mkdir
 *   get_photon_filename() - resolve config path (implemented per-platform)
 */

#pragma once

#include <stddef.h>
#include <string.h>
#include <sys/stat.h>
#include <errno.h>

/* Well-known path types used by get_photon_filename() */
typedef enum {
    PHOTON_PATH_INI = 0,          /* settings file: PhotonTERM.ini       */
    PHOTON_PATH_LIST,             /* BBS list: PhotonTERM.lst            */
    PHOTON_PATH_DOWNLOADS,        /* default download directory          */
    PHOTON_PATH_CACHE,            /* cache directory                     */
    PHOTON_PATH_KEYS,             /* SSH keys store: PhotonTERM.ssh      */
} photon_path_type_t;

/* Keep old names as aliases so DarwinWrappers.m can use either. */
#define PHOTONTERM_PATH_INI                  PHOTON_PATH_INI
#define PHOTONTERM_PATH_LIST                 PHOTON_PATH_LIST
#define PHOTONTERM_DEFAULT_TRANSFER_PATH     PHOTON_PATH_DOWNLOADS
#define PHOTONTERM_PATH_CACHE                PHOTON_PATH_CACHE
#define PHOTONTERM_PATH_KEYS                 PHOTON_PATH_KEYS

/* Portable recursive mkdir (equivalent to mkdir -p).
 * Returns 0 on success, -1 on error (errno set). */
static inline int photon_mkdir_p(const char *path)
{
    char   tmp[4096];
    char  *p;
    size_t len;

    if (!path || !*path) return -1;
    snprintf(tmp, sizeof(tmp), "%s", path);
    len = strlen(tmp);
    if (len == 0) return -1;
    if (tmp[len-1] == '/') tmp[len-1] = '\0';

    for (p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            if (mkdir(tmp, 0755) != 0 && errno != EEXIST) return -1;
            *p = '/';
        }
    }
    return (mkdir(tmp, 0755) == 0 || errno == EEXIST) ? 0 : -1;
}

/* get_photon_filename() - resolve a path type to an absolute filesystem path.
 *
 * On macOS: implemented in DarwinWrappers.m using NSFileManager.
 * On Linux/other: implemented in photon_paths.c using XDG base dirs.
 *
 * fn     - output buffer
 * fnlen  - size of fn
 * type   - which path to resolve
 * shared - true for system-wide paths, false for per-user paths
 *
 * Returns fn on success, NULL on error.
 */
char *get_photon_filename(char *fn, int fnlen, photon_path_type_t type, int shared);

/* Backward-compat alias used by existing code */
static inline char *get_photonterm_filename_compat(char *fn, int fnlen, int type, int shared)
{
    return get_photon_filename(fn, fnlen, (photon_path_type_t)type, shared);
}
