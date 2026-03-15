/* DarwinWrappers.h - macOS Objective-C utility wrappers for PhotonTERM
 * Copyright (C) 2026 fewtarius and PhotonTERM contributors
 * SPDX-License-Identifier: GPL-3.0-or-later */

#ifndef DARWINWRAPPERS_H
#define DARWINWRAPPERS_H

#include "photon_paths.h"

/* Resolves a well-known config path on macOS (defined in DarwinWrappers.m) */
char *get_photon_filename(char *fn, int fnlen, photon_path_type_t type, int shared);

/* Installs swizzled NSApplicationDelegate for Ctrl-C / window-close handling */
void photonterm_install_app_delegate(void);
#endif /* DARWINWRAPPERS_H */
