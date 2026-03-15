/* photon_xfer.c – file transfer via lrzsz (sz/rz subprocess)
 *
 * Architecture:
 *   - User picks protocol (ZModem/YModem/XModem) and direction (send/recv)
 *   - For send: user enters source file path(s)
 *   - For recv: user chooses download directory
 *   - sz / rz is spawned with pipes for stdin/stdout; stderr is read for progress
 *   - A pump loop shuffles bytes between conn send/recv and the subprocess
 *   - Progress is shown in a status overlay on the bottom two rows
 *   - User can press Ctrl-X (5 times) or ESC to cancel
 *
 * Copyright (C) 2026 fewtarius and PhotonTERM contributors
 * GPL v2 or later
 */

/* File transfer requires fork/exec - not available on Windows */
#ifdef _WIN32

#include "photon_xfer.h"

bool photon_xfer_run(photon_conn_t *conn, photon_sdl_t *sdl, photon_ui_t *ui)
{
    (void)conn;
    (void)sdl;
    photon_ui_msg(ui, "File transfer is not supported on Windows.");
    return false;
}

#else /* POSIX */

#include "photon_xfer.h"
#include "photon_conn.h"
#include "photon_sdl.h"
#include "photon_vte.h"
#include "photon_ui.h"
#include "photon_compat.h"
#include "photon_debug.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <time.h>
#include <limits.h>

/* ── Progress overlay ───────────────────────────────────────────────── */

/* Draw a two-row status bar at the bottom of the screen.
 * row1 = transfer info (protocol, direction, filename)
 * row2 = bytes transferred / speed / press Ctrl-X to cancel
 */
static void draw_xfer_status(photon_sdl_t *sdl,
                              const char *line1,
                              const char *line2)
{
    int cols = photon_sdl_cols(sdl);
    int rows = photon_sdl_rows(sdl);
    if (cols < 10 || rows < 3) return;

    /* bg1: white text on dark cyan; bg2: white text on dark blue */
    vte_cell_t bg1 = { ' ', 7, 6, 0 };  /* fg=white, bg=cyan  */
    vte_cell_t bg2 = { ' ', 7, 4, 0 };  /* fg=white, bg=blue  */

    const char *lines[2] = { line1, line2 };
    vte_cell_t  bgs[2]   = { bg1, bg2 };

    for (int li = 0; li < 2; li++) {
        int row = rows - 2 + li;
        const char *text = lines[li];
        vte_cell_t bg   = bgs[li];
        int len = (int)strlen(text);
        for (int c = 0; c < cols; c++) {
            vte_cell_t cell = bg;
            if (c < len) cell.codepoint = (uint32_t)(unsigned char)text[c];
            photon_sdl_draw_cell(sdl, c + 1, row + 1, &cell);
        }
    }
    photon_sdl_present(sdl);
}

/* ── Locate sz / rz binary ──────────────────────────────────────────── */

/* Returns path to sz or rz.  Prefers the one bundled inside the .app,
 * then falls back to PATH. */
static void find_binary(const char *name, char *out, size_t outsz)
{
    char exe[PATH_MAX] = {0};
#if defined(__APPLE__)
    {
        uint32_t sz = sizeof(exe);
        extern int _NSGetExecutablePath(char *, uint32_t *);
        _NSGetExecutablePath(exe, &sz);
    }
#elif defined(__linux__)
    {
        ssize_t r = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
        if (r > 0) exe[r] = '\0';
    }
#endif
    if (exe[0]) {
        char *slash = strrchr(exe, '/');
        if (slash) {
            char try[PATH_MAX];
            *slash = '\0';
            snprintf(try, sizeof(try), "%s/%s", exe, name);
            if (access(try, X_OK) == 0) {
                strlcpy(out, try, outsz);
                return;
            }
        }
    }
    /* Fall through to PATH lookup */
    strlcpy(out, name, outsz);
}

/* ── Subprocess launch ──────────────────────────────────────────────── */

typedef struct {
    pid_t pid;
    int   stdin_fd;   /* write end: data to send to sz/rz */
    int   stdout_fd;  /* read end:  data received from sz/rz */
    int   stderr_fd;  /* read end:  progress text from sz/rz */
} xfer_proc_t;

/* Spawn sz or rz with the given argv.
 * argv[0] must be the binary name / path.
 * Returns true on success, fills *out.
 */
static bool spawn_xfer(const char **argv, xfer_proc_t *out)
{
    int in_pipe[2], out_pipe[2], err_pipe[2];
    if (pipe(in_pipe) < 0 || pipe(out_pipe) < 0 || pipe(err_pipe) < 0) {
        PHOTON_DBG("xfer: pipe failed: %s", strerror(errno));
        return false;
    }

    pid_t pid = fork();
    if (pid < 0) {
        PHOTON_DBG("xfer: fork failed: %s", strerror(errno));
        return false;
    }

    if (pid == 0) {
        /* Child */
        dup2(in_pipe[0],  STDIN_FILENO);
        dup2(out_pipe[1], STDOUT_FILENO);
        dup2(err_pipe[1], STDERR_FILENO);
        close(in_pipe[0]); close(in_pipe[1]);
        close(out_pipe[0]); close(out_pipe[1]);
        close(err_pipe[0]); close(err_pipe[1]);
        execvp(argv[0], (char *const *)argv);
        /* execvp failed */
        fprintf(stderr, "exec %s: %s\n", argv[0], strerror(errno));
        _exit(127);
    }

    /* Parent */
    close(in_pipe[0]);
    close(out_pipe[1]);
    close(err_pipe[1]);

    /* Make pipes non-blocking */
    int flags = fcntl(err_pipe[0], F_GETFL, 0);
    fcntl(err_pipe[0], F_SETFL, flags | O_NONBLOCK);
    flags = fcntl(out_pipe[0], F_GETFL, 0);
    fcntl(out_pipe[0], F_SETFL, flags | O_NONBLOCK);

    out->pid       = pid;
    out->stdin_fd  = in_pipe[1];
    out->stdout_fd = out_pipe[0];
    out->stderr_fd = err_pipe[0];
    return true;
}

/* ── Transfer pump ──────────────────────────────────────────────────── */

/* Parse lrzsz stderr progress and format into status line. */
static void parse_progress(const char *err_line, char *status, int status_sz)
{
    /* Just display the first 79 chars; lrzsz formats lines well already */
    snprintf(status, (size_t)status_sz, "%.79s", err_line);
    int len = (int)strlen(status);
    while (len > 0 && (status[len-1] == '\n' || status[len-1] == '\r' ||
                        status[len-1] == ' ')) {
        status[--len] = '\0';
    }
}

/*
 * Pump data between conn (photon_conn_send/recv) and the sz/rz subprocess.
 * Shows progress overlay.  Returns true on clean completion.
 */
static bool run_pump(xfer_proc_t *proc, photon_sdl_t *sdl,
                     const char *proto_name, bool sending)
{
    char line1[256], line2[256];
    char err_buf[1024];
    char progress[128] = { 0 };
    size_t total_bytes = 0;
    time_t start_time  = time(NULL);
    bool cancelled     = false;
    int  cancel_count  = 0;

    snprintf(line1, sizeof(line1), " %s %s  --  Ctrl-X x5 to cancel",
             proto_name, sending ? "Upload" : "Download");

    while (true) {
        /* Check if subprocess has exited */
        int wstatus = 0;
        pid_t r = waitpid(proc->pid, &wstatus, WNOHANG);
        if (r == proc->pid) {
            proc->pid = -1;
            PHOTON_DBG("xfer: subprocess exited status=%d", WEXITSTATUS(wstatus));
            break;
        }

        /* sz/rz stdout -> connection send */
        uint8_t xfer_to_conn[4096];
        ssize_t got = read(proc->stdout_fd, xfer_to_conn, sizeof(xfer_to_conn));
        if (got > 0) {
            photon_conn_send(xfer_to_conn, (size_t)got, 2000);
            total_bytes += (size_t)got;
        }

        /* Connection recv -> sz/rz stdin */
        uint8_t conn_to_xfer[4096];
        int recv = photon_conn_recv(conn_to_xfer, sizeof(conn_to_xfer), 50);
        if (recv > 0) {
            ssize_t written = 0;
            while (written < recv) {
                ssize_t wr = write(proc->stdin_fd, conn_to_xfer + written,
                                   (size_t)(recv - written));
                if (wr <= 0) break;
                written += wr;
            }
        } else if (recv < 0) {
            PHOTON_DBG("xfer: connection closed during transfer");
            break;
        }

        /* Progress from stderr (non-blocking) */
        ssize_t err_got = read(proc->stderr_fd, err_buf, sizeof(err_buf) - 1);
        if (err_got > 0) {
            err_buf[err_got] = '\0';
            char *nl = strrchr(err_buf, '\r');
            if (!nl) nl = strrchr(err_buf, '\n');
            if (nl) parse_progress(nl + 1, progress, sizeof(progress));
        }

        /* Build status display */
        time_t elapsed = time(NULL) - start_time;
        size_t kbps = elapsed > 0 ? (total_bytes / 1024 / (size_t)elapsed) : 0;
        if (progress[0]) {
            snprintf(line2, sizeof(line2), " %s", progress);
        } else {
            snprintf(line2, sizeof(line2), " %zu bytes  %zu KB/s  elapsed: %lds",
                     total_bytes, kbps, (long)elapsed);
        }
        draw_xfer_status(sdl, line1, line2);

        /* Check for cancel keypress */
        photon_key_t k;
        while (photon_sdl_poll_key(sdl, &k)) {
            if (k.code == 0x18) {  /* Ctrl-X */
                cancel_count++;
                if (cancel_count >= 5) {
                    cancelled = true;
                    /* ZModem cancel sequence */
                    const uint8_t cancel_seq[] = {
                        0x18,0x18,0x18,0x18,0x18,
                        0x08,0x08,0x08,0x08,0x08
                    };
                    photon_conn_send(cancel_seq, sizeof(cancel_seq), 1000);
                }
            } else if (k.code == 27 || k.code == PHOTON_KEY_QUIT) {
                cancelled = true;
            }
        }
        if (cancelled) {
            PHOTON_DBG("xfer: user cancelled");
            break;
        }
    }

    return !cancelled;
}

/* ── Public API ─────────────────────────────────────────────────────── */

bool photon_xfer_run(photon_conn_t *conn, photon_sdl_t *sdl, photon_ui_t *ui)
{
    (void)conn;  /* we use the active connection via photon_conn_send/recv */

    /* 1. Pick direction */
    static int dir_sel = 0;
    const char *dir_items[] = { "Download (receive from BBS)",
                                 "Upload (send to BBS)" };
    int dir = photon_ui_list(ui, "File Transfer", dir_items, 2, &dir_sel);
    if (dir < 0) return false;
    dir_sel = dir;
    bool sending = (dir == 1);

    /* 2. Pick protocol */
    static int proto_sel = 0;
    const char *proto_items[] = { "ZModem (recommended)",
                                   "YModem (batch)",
                                   "XModem" };
    int proto = photon_ui_list(ui, "Protocol", proto_items, 3, &proto_sel);
    if (proto < 0) return false;
    proto_sel = proto;

    photon_xfer_proto_t xproto = (photon_xfer_proto_t)proto;
    const char *proto_name = proto_items[proto];
    /* Short name for display (strip description in parens) */
    char proto_short[32];
    strlcpy(proto_short, proto_name, sizeof(proto_short));
    char *sp = strchr(proto_short, ' ');
    if (sp) *sp = '\0';

    /* 3. File / directory selection */
    char path_buf[1024] = { 0 };
    const char *home = getenv("HOME");

    if (sending) {
        snprintf(path_buf, sizeof(path_buf), "%s/", home ? home : "");
        int r = photon_ui_input(ui, "File to Upload (full path)", path_buf,
                                (int)sizeof(path_buf), PHOTON_INPUT_EDIT);
        if (r <= 0) return false;
        struct stat st;
        if (stat(path_buf, &st) != 0 || !S_ISREG(st.st_mode)) {
            photon_ui_msg(ui, "File not found or not a regular file.");
            return false;
        }
    } else {
        snprintf(path_buf, sizeof(path_buf), "%s/Downloads",
                 home ? home : "/tmp");
        mkdir(path_buf, 0755);
        int r = photon_ui_input(ui, "Download Directory", path_buf,
                                (int)sizeof(path_buf), PHOTON_INPUT_EDIT);
        if (r <= 0) return false;
        struct stat st;
        if (stat(path_buf, &st) != 0 || !S_ISDIR(st.st_mode)) {
            photon_ui_msg(ui, "Directory does not exist.");
            return false;
        }
    }

    /* 4. Build argv */
    char sz_bin[PATH_MAX], rz_bin[PATH_MAX];
    find_binary("sz", sz_bin, sizeof(sz_bin));
    find_binary("rz", rz_bin, sizeof(rz_bin));
    const char *argv[16];
    int argc = 0;

    if (sending) {
        argv[argc++] = sz_bin;
        switch (xproto) {
            case PHOTON_XFER_ZMODEM: argv[argc++] = "--zmodem"; break;
            case PHOTON_XFER_YMODEM: argv[argc++] = "--ymodem"; break;
            case PHOTON_XFER_XMODEM: argv[argc++] = "--xmodem"; break;
        }
        argv[argc++] = "--escape";
        argv[argc++] = path_buf;
    } else {
        argv[argc++] = rz_bin;
        switch (xproto) {
            case PHOTON_XFER_ZMODEM: argv[argc++] = "--zmodem"; break;
            case PHOTON_XFER_YMODEM: argv[argc++] = "--ymodem"; break;
            case PHOTON_XFER_XMODEM: argv[argc++] = "--xmodem"; break;
        }
        argv[argc++] = "--escape";
    }
    argv[argc] = NULL;

    /* 5. chdir to download dir for rz */
    char saved_cwd[PATH_MAX];
    saved_cwd[0] = '\0';
    if (!sending) {
        if (getcwd(saved_cwd, sizeof(saved_cwd)) == NULL)
            saved_cwd[0] = '\0';
        if (chdir(path_buf) != 0) {
            photon_ui_msg(ui, "Cannot change to download directory.");
            return false;
        }
    }

    /* 6. Spawn */
    xfer_proc_t proc = { .pid = -1, .stdin_fd = -1, .stdout_fd = -1, .stderr_fd = -1 };
    if (!spawn_xfer(argv, &proc)) {
        if (!sending && saved_cwd[0]) chdir(saved_cwd);
        photon_ui_msg(ui, "Failed to start sz/rz (lrzsz not found?).");
        return false;
    }

    PHOTON_DBG("xfer: spawned %s pid=%d", argv[0], (int)proc.pid);

    /* 7. Pump */
    bool ok = run_pump(&proc, sdl, proto_short, sending);

    /* 8. Cleanup */
    if (proc.pid > 0) {
        kill(proc.pid, SIGTERM);
        usleep(100000);
        waitpid(proc.pid, NULL, WNOHANG);
    }
    if (proc.stdin_fd  >= 0) close(proc.stdin_fd);
    if (proc.stdout_fd >= 0) close(proc.stdout_fd);
    if (proc.stderr_fd >= 0) close(proc.stderr_fd);

    if (!sending && saved_cwd[0]) chdir(saved_cwd);

    /* 9. Result */
    if (ok)
        photon_ui_msg(ui, "Transfer complete.");
    else
        photon_ui_msg(ui, "Transfer cancelled or failed.");

    return ok;
}

#endif /* !_WIN32 */
