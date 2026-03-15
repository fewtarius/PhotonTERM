/* photon_xfer.h – file transfer (ZModem / YModem / XModem) via lrzsz */

#ifndef PHOTON_XFER_H
#define PHOTON_XFER_H

#include <stdbool.h>
#include "photon_conn.h"
#include "photon_sdl.h"
#include "photon_ui.h"

/* Transfer protocol */
typedef enum {
    PHOTON_XFER_ZMODEM = 0,
    PHOTON_XFER_YMODEM,
    PHOTON_XFER_XMODEM,
} photon_xfer_proto_t;

/* Transfer direction */
typedef enum {
    PHOTON_XFER_SEND = 0,
    PHOTON_XFER_RECV,
} photon_xfer_dir_t;

/*
 * Run the file-transfer UI.
 *
 * Shows a protocol/direction picker, a file-chooser (for send), then
 * launches lrzsz (sz / rz) piped through the live conn_t.
 *
 * conn must be an active, connected session.
 * Blocks until the transfer completes or is cancelled.
 *
 * Returns true on success, false on error or cancellation.
 */
bool photon_xfer_run(photon_conn_t *conn, photon_sdl_t *sdl, photon_ui_t *ui);

#endif /* PHOTON_XFER_H */
