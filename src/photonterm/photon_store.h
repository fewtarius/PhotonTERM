/* photon_store.h - Dialing directory persistence
 *
 * Copyright (C) 2026 fewtarius and PhotonTERM contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Reads and writes the bbslist.ini file under ~/.config/photonterm/
 * (with fallback to ~/.photonterm/ for compatibility).
 */

#pragma once

#include "photon_bbs.h"
#include <stddef.h>
#include <stdbool.h>

/* Maximum entries in the directory */
#define PHOTON_STORE_MAX_ENTRIES 4096

/* Load all entries from disk into caller-allocated array.
 * Returns number of entries loaded, or -1 on error.
 * path_out: if not NULL, filled with the path used. */
int  photon_store_load(photon_bbs_t *entries, int max, char *path_out, size_t path_max);

/* Save all entries to disk.
 * Returns true on success. */
bool photon_store_save(const photon_bbs_t *entries, int count);

/* Returns the config directory path (~/.config/photonterm or ~/.photonterm).
 * Never returns NULL (falls back to "."). */
const char *photon_store_config_dir(void);
