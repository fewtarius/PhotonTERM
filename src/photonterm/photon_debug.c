/* photon_debug.c - debug log global state
 *
 * Copyright (C) 2026 fewtarius and PhotonTERM contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Provides storage for the extern variables declared in photon_debug.h.
 */

#include <stdbool.h>
#include <stdio.h>

bool  photon_debug_enabled = false;
FILE *photon_debug_fp      = NULL;
