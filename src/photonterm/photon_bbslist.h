/* photon_bbslist.h - PhotonTERM dialing directory (photon_ui implementation)
 *
 * Copyright (C) 2026 fewtarius and PhotonTERM contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include "photon_ui.h"
#include "photon_bbs.h"
#include "photon_settings.h"

/* Show the dialing directory and return a heap-allocated photon_bbs_t entry
 * for the user's selection, or NULL if the user cancelled.
 * The caller must free the result with photon_bbslist_free(). */
photon_bbs_t *photon_bbslist_run(photon_ui_t *ui);
void          photon_bbslist_free(photon_bbs_t *bbs);

/* Run the settings menu overlay. Can be called from any context
 * (directory, splash, or in-session via Alt-Z). */
void photon_bbslist_run_settings(photon_ui_t *ui, photon_settings_t *s);
