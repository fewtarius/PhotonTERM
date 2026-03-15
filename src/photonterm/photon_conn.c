/* photon_conn.c - PhotonTERM connection transport layer
 *
 * Copyright (C) 2026 fewtarius and PhotonTERM contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Implements photon_conn_connect / _close / _recv / _send.
 * Supports PHOTON_CONN_TELNET (RFC 854), PHOTON_CONN_SSH (libssh2),
 * and PHOTON_CONN_SHELL (local PTY, POSIX only).
 *
 * All I/O runs in background threads; the public API is thread-safe.
 * The ring-buffer design is based on the original conn.c ring-buffer
 * Clean connection transport layer for PhotonTERM.
 */

#include "photon_conn.h"
#include "photon_bbs.h"
#include "photon_compat.h"

#define PHOTON_DEBUG_BUILD
#include "photon_debug.h"

/* Networking headers */
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <io.h>
#include <sys/types.h>    /* ssize_t */
#define close(fd)     closesocket(fd)
#define poll(fds,n,t) WSAPoll(fds,n,t)
#define SHUT_RDWR     SD_BOTH
#define EINPROGRESS   WSAEINPROGRESS
#define EWOULDBLOCK   WSAEWOULDBLOCK
#define EHOSTUNREACH  WSAEHOSTUNREACH
#define ENETUNREACH   WSAENETUNREACH
typedef int socklen_t;
static inline void sock_set_nonblock(int fd, int nonblock) {
    u_long mode = nonblock;
    ioctlsocket(fd, FIONBIO, &mode);
}
/* On Windows, connect() on non-blocking socket sets WSAGetLastError() to
   WSAEWOULDBLOCK, not errno. Override errno for connect error checks. */
#define CONN_ERRNO()  WSAGetLastError()
#else
#include <sys/socket.h>
#include <sys/select.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <pwd.h>
static inline void sock_set_nonblock(int fd, int nonblock) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (nonblock) flags |= O_NONBLOCK; else flags &= ~O_NONBLOCK;
    fcntl(fd, F_SETFL, flags);
}
#define CONN_ERRNO()  errno
#endif
#include <errno.h>

/* POSIX threads */
#include <pthread.h>
/* Use pthread condvar for cross-platform timed wait (avoids macOS sem_init deprecation) */

/* libssh2 */
#include <libssh2.h>

/* C stdlib */
#include <assert.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <limits.h>

/* ── Portable socket type ───────────────────────────────────────────── */

typedef int photon_sock_t;
#define PHOTON_INVALID_SOCK (-1)

/* ── Monotonic clock helper (replaces xp_timer) ─────────────────────── */

static long double photon_mono_time(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) == 0)
        return (long double)ts.tv_sec + (long double)ts.tv_nsec / 1e9L;
    return -1.0L;
}

/* ── Error string ───────────────────────────────────────────────────── */

static char s_last_error[256] = "";

/* SSH password prompt callback (set by photon_conn_set_ssh_prompt) */
static photon_conn_prompt_fn s_ssh_prompt_fn = NULL;
static void                 *s_ssh_prompt_ud = NULL;

static void set_error(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(s_last_error, sizeof(s_last_error), fmt, ap);
    va_end(ap);
    PHOTON_DBG("photon_conn error: %s", s_last_error);
}

const char *photon_conn_last_error(void) { return s_last_error; }

void photon_conn_set_ssh_prompt(photon_conn_prompt_fn fn, void *userdata)
{
    s_ssh_prompt_fn = fn;
    s_ssh_prompt_ud = userdata;
}
/* ── Telnet protocol constants ──────────────────────────────────────── */

#define TELNET_IAC  255
#define TELNET_WILL 251
#define TELNET_WONT 252
#define TELNET_DO   253
#define TELNET_DONT 254
#define TELNET_SB   250
#define TELNET_SE   240

#define TELOPT_BINARY  0
#define TELOPT_ECHO    1
#define TELOPT_SGA     3    /* Suppress Go Ahead */
#define TELOPT_NAWS    31   /* Negotiate About Window Size */

/* ── Ring buffer ────────────────────────────────────────────────────── */

#define RING_SIZE (256 * 1024)

typedef struct {
    unsigned char  *buf;
    size_t          bufsize;
    size_t          buftop;   /* write position */
    size_t          bufbot;   /* read position */
    int             isempty;
    pthread_mutex_t mu;
    pthread_cond_t  in_cond;  /* signalled when bytes written */
    pthread_cond_t  out_cond; /* signalled when bytes freed */
    int             in_waiters;
    int             out_waiters;
} ring_t;

/* Minimum stack size for I/O threads.  macOS default is 512 KB which is
   insufficient when the malloc(RING_SIZE) frame is laid out in debug builds.
   2 MB provides a generous margin on all platforms. */
#define IO_THREAD_STACK_SIZE (2 * 1024 * 1024)

static int create_io_thread(pthread_t *t, void *(*fn)(void *), void *arg)
{
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, IO_THREAD_STACK_SIZE);
    int rc = pthread_create(t, &attr, fn, arg);
    pthread_attr_destroy(&attr);
    return rc;
}

static ring_t *ring_alloc(size_t size)
{
    ring_t *r = calloc(1, sizeof(*r));
    if (!r) return NULL;
    r->buf = malloc(size);
    if (!r->buf) { free(r); return NULL; }
    r->bufsize = size;
    r->isempty = 1;
    if (pthread_mutex_init(&r->mu, NULL) ||
        pthread_cond_init(&r->in_cond, NULL) ||
        pthread_cond_init(&r->out_cond, NULL)) {
        free(r->buf);
        free(r);
        return NULL;
    }
    return r;
}

static void ring_free(ring_t *r)
{
    if (!r) return;
    free(r->buf);
    pthread_mutex_destroy(&r->mu);
    pthread_cond_destroy(&r->in_cond);
    pthread_cond_destroy(&r->out_cond);
    free(r);
}

/* Bytes available (call with mu held) */
static size_t ring_bytes(ring_t *r)
{
    if (r->isempty) return 0;
    if (r->buftop > r->bufbot) return r->buftop - r->bufbot;
    return r->bufsize - r->bufbot + r->buftop;
}

/* Free space (call with mu held) */
static size_t ring_free_bytes(ring_t *r)
{
    return r->bufsize - ring_bytes(r);
}

/* Peek without consuming (mu held) */
static size_t ring_peek(ring_t *r, void *out, size_t len)
{
    unsigned char *ob = out;
    size_t avail = ring_bytes(r);
    if (len > avail) len = avail;
    size_t chunk = r->bufsize - r->bufbot;
    if (chunk > len) chunk = len;
    if (chunk) memcpy(ob, r->buf + r->bufbot, chunk);
    if (chunk < len) memcpy(ob + chunk, r->buf, len - chunk);
    return len;
}

/* Consume (mu held) */
static size_t ring_get(ring_t *r, void *out, size_t len)
{
    size_t n = ring_peek(r, out, len);
    if (n) {
        r->bufbot = (r->bufbot + n) % r->bufsize;
        /* If bot caught up to top, the ring is now empty. */
        if (r->bufbot == r->buftop) r->isempty = 1;
        if (r->out_waiters > 0)
            pthread_cond_signal(&r->out_cond);
    }
    return n;
}

/* Write (mu held) */
static size_t ring_put(ring_t *r, const void *in, size_t len)
{
    size_t avail = ring_free_bytes(r);
    if (len > avail) len = avail;
    if (len == 0) return 0;
    size_t chunk = r->bufsize - r->buftop;
    if (chunk > len) chunk = len;
    memcpy(r->buf + r->buftop, in, chunk);
    if (chunk < len) memcpy(r->buf, (const char *)in + chunk, len - chunk);
    r->buftop = (r->buftop + len) % r->bufsize;
    r->isempty = 0;
    if (r->in_waiters > 0)
        pthread_cond_signal(&r->in_cond);
    return len;
}

/* Wait for bcount bytes (>0) or timeout_ms=0 for non-blocking.
 * Returns how many bytes are available (up to bcount).
 * Call with mu UNLOCKED; re-acquires mu before returning. */
static size_t ring_wait_bytes(ring_t *r, size_t bcount, unsigned long timeout_ms)
{
    size_t found;
    pthread_mutex_lock(&r->mu);
    found = ring_bytes(r);
    if (found >= bcount || timeout_ms == 0) {
        if (found > bcount) found = bcount;
        pthread_mutex_unlock(&r->mu);
        return found;
    }

    /* Compute absolute deadline */
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec  += timeout_ms / 1000;
    ts.tv_nsec += (timeout_ms % 1000) * 1000000L;
    if (ts.tv_nsec >= 1000000000L) {
        ts.tv_sec++;
        ts.tv_nsec -= 1000000000L;
    }

    r->in_waiters++;
    while ((found = ring_bytes(r)) < bcount) {
        int rc = pthread_cond_timedwait(&r->in_cond, &r->mu, &ts);
        if (rc != 0) break; /* timeout or error */
    }
    r->in_waiters--;
    if (found > bcount) found = bcount;
    pthread_mutex_unlock(&r->mu);
    return found;
}

/* ── Connection state ───────────────────────────────────────────────── */

/* I/O thread state */
typedef struct {
    pthread_t       thread;
    atomic_int      running;  /* 0=not started, 1=running, 2=done */
    atomic_bool     terminate;
} thread_state_t;

/* Per-connection state (each tab has one) */
struct photon_conn {
    ring_t         *inbuf;
    ring_t         *outbuf;

    thread_state_t  rx;
    thread_state_t  tx;
    atomic_bool     terminate;

    /* Telnet */
    photon_sock_t   telnet_sock;
    pthread_mutex_t  telnet_send_mu; /* serializes direct-send and TX thread */

    /* SSH */
    photon_sock_t   ssh_sock;
    LIBSSH2_SESSION *ssh_session;
    LIBSSH2_CHANNEL *ssh_channel;
    pthread_mutex_t  ssh_mu;

    /* PTY */
    int             pty_fd;

    int             type;  /* PHOTON_CONN_* */
};

/* Active connection - set by photon_conn_set_active() */
static photon_conn_t *s_active = NULL;

int photon_conn_raw_fd(void)
{
    if (!s_active) return -1;
    switch (s_active->type) {
        case PHOTON_CONN_SHELL:  return s_active->pty_fd;
        case PHOTON_CONN_TELNET: return (int)s_active->telnet_sock;
        case PHOTON_CONN_SSH:    return -1;
        default:                 return -1;
    }
}

/* ── Telnet option state ─────────────────────────────────────────────── */

static unsigned char tl_local[256];   /* local option states */
static unsigned char tl_remote[256];  /* remote option states */

static void telnet_request_opt(unsigned char what, unsigned char opt)
{
    unsigned char buf[3] = { TELNET_IAC, what, opt };
    if (s_active->telnet_sock != PHOTON_INVALID_SOCK) {
        pthread_mutex_lock(&s_active->telnet_send_mu);
        send(s_active->telnet_sock, buf, 3, 0);
        pthread_mutex_unlock(&s_active->telnet_send_mu);
    }
}

/* Minimal telnet negotiation: parse IAC sequences out of the stream.
 * Returns the number of bytes placed into out (always <= inlen).
 * Handles WILL/WONT/DO/DONT and SB..SE (skipped).
 */
static size_t telnet_interpret(const unsigned char *in, size_t inlen,
                                unsigned char *out, size_t outmax)
{
    size_t oi = 0;
    size_t i  = 0;

    while (i < inlen && oi < outmax) {
        if (in[i] != TELNET_IAC) {
            out[oi++] = in[i++];
            continue;
        }
        i++; /* consume IAC */
        if (i >= inlen) break;

        unsigned char cmd = in[i++];
        if (cmd == TELNET_IAC) {
            /* Escaped IAC -> literal 0xFF */
            out[oi++] = TELNET_IAC;
            continue;
        }
        if (cmd == TELNET_SB) {
            /* Subnegotiation: skip until IAC SE */
            while (i < inlen) {
                if (in[i] == TELNET_IAC && i + 1 < inlen && in[i+1] == TELNET_SE) {
                    i += 2;
                    break;
                }
                i++;
            }
            continue;
        }
        if (cmd == TELNET_WILL || cmd == TELNET_WONT ||
            cmd == TELNET_DO   || cmd == TELNET_DONT) {
            if (i >= inlen) break;
            unsigned char opt = in[i++];
            /* Simple auto-respond */
            if (cmd == TELNET_DO) {
                if (opt == TELOPT_SGA || opt == TELOPT_BINARY) {
                    if (!tl_local[opt]) {
                        tl_local[opt] = 1;
                        telnet_request_opt(TELNET_WILL, opt);
                    }
                } else {
                    telnet_request_opt(TELNET_WONT, opt);
                }
            } else if (cmd == TELNET_WILL) {
                if (opt == TELOPT_ECHO || opt == TELOPT_SGA || opt == TELOPT_BINARY) {
                    if (!tl_remote[opt]) {
                        tl_remote[opt] = 1;
                        telnet_request_opt(TELNET_DO, opt);
                    }
                } else {
                    telnet_request_opt(TELNET_DONT, opt);
                }
            }
            continue;
        }
        /* Other IAC commands: skip */
    }
    return oi;
}

static void telnet_send_initial_options(void)
{
    memset(tl_local,  0, sizeof(tl_local));
    memset(tl_remote, 0, sizeof(tl_remote));
    /* Suppress Go Ahead both ways */
    telnet_request_opt(TELNET_WILL, TELOPT_SGA);
    tl_local[TELOPT_SGA] = 1;
    telnet_request_opt(TELNET_DO,   TELOPT_SGA);
    tl_remote[TELOPT_SGA] = 1;
    /* Binary mode both ways */
    telnet_request_opt(TELNET_WILL, TELOPT_BINARY);
    tl_local[TELOPT_BINARY] = 1;
    telnet_request_opt(TELNET_DO,   TELOPT_BINARY);
    tl_remote[TELOPT_BINARY] = 1;
    /* Request server echo */
    telnet_request_opt(TELNET_DO,   TELOPT_ECHO);
    tl_remote[TELOPT_ECHO] = 1;
}

/* Expand outgoing data: double IAC bytes, convert lone CR -> CRNUL */
static size_t telnet_expand(const unsigned char *in, size_t inlen,
                             unsigned char *out, size_t outmax)
{
    size_t oi = 0;
    for (size_t i = 0; i < inlen && oi + 1 < outmax; i++) {
        if (in[i] == TELNET_IAC) {
            out[oi++] = TELNET_IAC;
            if (oi >= outmax) break;
        }
        out[oi++] = in[i];
    }
    return oi;
}

/* ── Socket helpers ─────────────────────────────────────────────────── */

static bool sock_readable(photon_sock_t fd, int timeout_ms)
{
    fd_set rfds;
    FD_ZERO(&rfds);
    FD_SET(fd, &rfds);
    struct timeval tv = { timeout_ms / 1000, (timeout_ms % 1000) * 1000 };
    return select(fd + 1, &rfds, NULL, NULL, &tv) > 0;
}

static bool sock_writable(photon_sock_t fd, int timeout_ms)
{
    fd_set wfds;
    FD_ZERO(&wfds);
    FD_SET(fd, &wfds);
    struct timeval tv = { timeout_ms / 1000, (timeout_ms % 1000) * 1000 };
    return select(fd + 1, NULL, &wfds, NULL, &tv) > 0;
}

/* DNS resolve + TCP connect.  Returns PHOTON_INVALID_SOCK on failure. */
static photon_sock_t tcp_connect(const char *host, uint16_t port)
{
    char portstr[8];
    snprintf(portstr, sizeof(portstr), "%hu", port);

    struct addrinfo hints = {0};
    hints.ai_family   = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    hints.ai_flags    = AI_NUMERICSERV;

    struct addrinfo *res = NULL;
    if (getaddrinfo(host, portstr, &hints, &res) != 0 || !res) {
        set_error("DNS lookup failed for %s", host);
        return PHOTON_INVALID_SOCK;
    }

    photon_sock_t sock = PHOTON_INVALID_SOCK;
    int retries = 0;
retry:
    for (struct addrinfo *cur = res; cur; cur = cur->ai_next) {
        int fd = socket(cur->ai_family, cur->ai_socktype, cur->ai_protocol);
        if (fd < 0) continue;

        /* Set non-blocking for connect */
        sock_set_nonblock(fd, 1);

        int r = connect(fd, cur->ai_addr, cur->ai_addrlen);
        if (r == 0) {
            sock = fd;
            break;
        }
        if (r < 0 && (CONN_ERRNO() == EINPROGRESS || CONN_ERRNO() == EWOULDBLOCK)) {
            /* Wait up to 15s */
            if (sock_writable(fd, 15000)) {
                int err = 0;
                socklen_t el = sizeof(err);
                getsockopt(fd, SOL_SOCKET, SO_ERROR, (char *)&err, &el);
                if (err == 0) { sock = fd; break; }
            }
        }
        int ce = CONN_ERRNO();
        if (ce == EHOSTUNREACH || ce == ENETUNREACH) {
            close(fd);
            if (retries++ < 2) {
                struct timespec ts = {0, 750000000L};
                nanosleep(&ts, NULL);
                goto retry;
            }
        }
        close(fd);
    }
    freeaddrinfo(res);

    if (sock == PHOTON_INVALID_SOCK) {
        set_error("TCP connect to %s:%hu failed", host, port);
        return PHOTON_INVALID_SOCK;
    }

    /* Restore blocking */
    sock_set_nonblock(sock, 0);

    /* Enable TCP keepalive */
    int ka = 1;
    setsockopt(sock, SOL_SOCKET, SO_KEEPALIVE, (const char *)&ka, sizeof(ka));

    return sock;
}

/* ── Telnet I/O threads ─────────────────────────────────────────────── */

static void *telnet_rx_thread(void *arg)
{
    photon_conn_t *C = arg;
    unsigned char *raw    = malloc(RING_SIZE);
    unsigned char *parsed = malloc(RING_SIZE + 4);
    if (!raw || !parsed) { free(raw); free(parsed); return NULL; }

    atomic_store(&C->rx.running, 1);

    while (!atomic_load(&C->terminate) && C->telnet_sock != PHOTON_INVALID_SOCK) {
        if (!sock_readable(C->telnet_sock, 100))
            continue;

        ssize_t rd = recv(C->telnet_sock, raw, RING_SIZE, 0);
        if (rd <= 0) {
            PHOTON_DBG("telnet_rx: recv=%zd errno=%d (%s)", rd, errno, strerror(errno));
            break;
        }

        size_t outlen = telnet_interpret(raw, (size_t)rd, parsed, RING_SIZE + 4);
        size_t sent = 0;
        while (sent < outlen && !atomic_load(&C->terminate)) {
            ring_wait_bytes(C->inbuf, 0, 0); /* ensure lock */
            pthread_mutex_lock(&C->inbuf->mu);
            sent += ring_put(C->inbuf, parsed + sent, outlen - sent);
            pthread_mutex_unlock(&C->inbuf->mu);
            if (sent < outlen) {
                struct timespec ts = {0, 1000000L};
                nanosleep(&ts, NULL);
            }
        }
    }

    atomic_store(&C->rx.running, 2);
    free(raw);
    free(parsed);
    return NULL;
}

static void *telnet_tx_thread(void *arg)
{
    photon_conn_t *C = arg;
    unsigned char *raw      = malloc(RING_SIZE);
    unsigned char *expanded = malloc(RING_SIZE * 2);
    if (!raw || !expanded) { free(raw); free(expanded); return NULL; }

    atomic_store(&C->tx.running, 1);

    while (!atomic_load(&C->terminate)) {
        size_t avail = ring_wait_bytes(C->outbuf, 1, 100);
        if (avail == 0) {
            if (C->telnet_sock == PHOTON_INVALID_SOCK)
                break;
            continue;
        }

        pthread_mutex_lock(&C->outbuf->mu);
        size_t n = ring_get(C->outbuf, raw, RING_SIZE);
        pthread_mutex_unlock(&C->outbuf->mu);
        if (n == 0) continue;

        size_t exlen = telnet_expand(raw, n, expanded, RING_SIZE * 2);
        ssize_t sent = 0;
        bool err = false;
        while ((size_t)sent < exlen && !atomic_load(&C->terminate)) {
            if (!sock_writable(C->telnet_sock, 100)) continue;
            pthread_mutex_lock(&C->telnet_send_mu);
            ssize_t r = send(C->telnet_sock, expanded + sent, exlen - (size_t)sent, 0);
            pthread_mutex_unlock(&C->telnet_send_mu);
            if (r <= 0) { PHOTON_DBG("telnet_tx: send error %zd errno=%d (%s)", r, errno, strerror(errno)); err = true; break; }
            sent += r;
        }
        if (err) break;
    }

    /* Close the socket so the RX thread terminates */
    photon_sock_t fd = C->telnet_sock;
    C->telnet_sock = PHOTON_INVALID_SOCK;
    if (fd != PHOTON_INVALID_SOCK) {
        shutdown(fd, SHUT_RDWR);
        close(fd);
    }

    free(raw);
    free(expanded);
    atomic_store(&C->tx.running, 2);
    return NULL;
}

/* ── SSH I/O threads ────────────────────────────────────────────────── */

static void *ssh_rx_thread(void *arg)
{
    photon_conn_t *C = arg;
    unsigned char *buf = malloc(RING_SIZE);
    if (!buf) return NULL;

    atomic_store(&C->rx.running, 1);

    while (!atomic_load(&C->terminate) && C->ssh_channel) {
        pthread_mutex_lock(&C->ssh_mu);
        ssize_t rd = libssh2_channel_read(C->ssh_channel, (char *)buf, RING_SIZE);
        pthread_mutex_unlock(&C->ssh_mu);

        if (rd == LIBSSH2_ERROR_EAGAIN) {
            sock_readable(C->ssh_sock, 50);
            continue;
        }
        if (rd < 0) {
            PHOTON_DBG("ssh_rx: read error %zd", rd);
            break;
        }
        if (rd == 0) {
            PHOTON_DBG("ssh_rx: EOF");
            break;
        }

        size_t sent = 0;
        while (sent < (size_t)rd && !atomic_load(&C->terminate)) {
            pthread_mutex_lock(&C->inbuf->mu);
            sent += ring_put(C->inbuf, buf + sent, (size_t)rd - sent);
            pthread_mutex_unlock(&C->inbuf->mu);
            if (sent < (size_t)rd) {
                struct timespec ts = {0, 1000000L};
                nanosleep(&ts, NULL);
            }
        }
    }

    atomic_store(&C->rx.running, 2);
    free(buf);
    return NULL;
}

static void *ssh_tx_thread(void *arg)
{
    photon_conn_t *C = arg;
    unsigned char *buf = malloc(RING_SIZE);
    if (!buf) return NULL;

    atomic_store(&C->tx.running, 1);

    while (!atomic_load(&C->terminate)) {
        size_t avail = ring_wait_bytes(C->outbuf, 1, 100);
        if (avail == 0) {
            if (!C->ssh_channel || atomic_load(&C->rx.running) == 2)
                break;
            continue;
        }

        pthread_mutex_lock(&C->outbuf->mu);
        size_t n = ring_get(C->outbuf, buf, RING_SIZE);
        pthread_mutex_unlock(&C->outbuf->mu);
        if (n == 0) continue;

        size_t sent = 0;
        bool err = false;
        while (sent < n && !atomic_load(&C->terminate)) {
            pthread_mutex_lock(&C->ssh_mu);
            ssize_t r = libssh2_channel_write(C->ssh_channel,
                                              (char *)buf + sent, n - sent);
            pthread_mutex_unlock(&C->ssh_mu);
            if (r == LIBSSH2_ERROR_EAGAIN) {
                sock_writable(C->ssh_sock, 50);
                continue;
            }
            if (r < 0) { PHOTON_DBG("ssh_tx: write error %zd", r); err = true; break; }
            sent += (size_t)r;
        }
        if (err) break;
    }

    atomic_store(&C->tx.running, 2);
    free(buf);
    return NULL;
}

/* ── SSH authentication helpers ─────────────────────────────────────── */

static bool ssh_authenticate(LIBSSH2_SESSION *sess, const char *user,
                              const char *pass)
{
    /* Use blocking mode during auth - much simpler than EAGAIN loops */
    libssh2_session_set_blocking(sess, 1);

    /* 1. ssh-agent */
    {
        LIBSSH2_AGENT *agent = libssh2_agent_init(sess);
        PHOTON_DBG("ssh_auth: agent=%p", (void *)agent);
        if (agent) {
            int ac = libssh2_agent_connect(agent);
            PHOTON_DBG("ssh_auth: agent_connect=%d", ac);
            if (ac == 0) {
            int al = libssh2_agent_list_identities(agent);
            PHOTON_DBG("ssh_auth: agent_list_identities=%d", al);
            if (al == 0) {
                struct libssh2_agent_publickey *id = NULL, *prev = NULL;
                int gi;
                while ((gi = libssh2_agent_get_identity(agent, &id, prev)) == 0) {
                    PHOTON_DBG("ssh_auth: trying agent key comment='%s'", id ? id->comment : "(null)");
                    int ua = libssh2_agent_userauth(agent, user, id);
                    PHOTON_DBG("ssh_auth: agent_userauth=%d", ua);
                    if (ua == 0) {
                        libssh2_agent_disconnect(agent);
                        libssh2_agent_free(agent);
                        libssh2_session_set_blocking(sess, 0);
                        PHOTON_DBG("ssh_auth: agent SUCCESS");
                        return true;
                    }
                    prev = id;
                }
                PHOTON_DBG("ssh_auth: agent exhausted (get_identity last=%d)", gi);
            } /* list_identities ok */
            } /* agent_connect ok */
            libssh2_agent_disconnect(agent);
            libssh2_agent_free(agent);
        }
    }

    /* 2. Key files in ~/.ssh */
    const char *home = getenv("HOME");
    if (home) {
        static const char *key_bases[] = {
            "id_ed25519", "id_ecdsa", "id_rsa", NULL
        };
        for (int i = 0; key_bases[i]; i++) {
            char priv[512], pub[512];
            snprintf(priv, sizeof(priv), "%s/.ssh/%s",   home, key_bases[i]);
            snprintf(pub,  sizeof(pub),  "%s/.ssh/%s_active->pub", home, key_bases[i]);
            if (access(priv, R_OK) == 0) {
                PHOTON_DBG("ssh_auth: trying keyfile %s", priv);
                int rc = libssh2_userauth_publickey_fromfile(sess, user,
                                                              pub, priv,
                                                              pass ? pass : "");
                PHOTON_DBG("ssh_auth: keyfile rc=%d", rc);
                if (rc == 0) {
                    libssh2_session_set_blocking(sess, 0);
                    PHOTON_DBG("ssh_auth: keyfile SUCCESS");
                    return true;
                }
            }
        }
    }

    /* 3. Password */
    if (pass && *pass) {
        if (libssh2_userauth_password(sess, user, pass) == 0) {
            libssh2_session_set_blocking(sess, 0);
            return true;
        }
    }

    /* 4. Prompt for password if none stored */
    if (s_ssh_prompt_fn) {
        char prompted[256] = {0};
        if (s_ssh_prompt_fn("SSH Password", prompted, sizeof(prompted),
                             s_ssh_prompt_ud) && prompted[0]) {
            int rc = libssh2_userauth_password(sess, user, prompted);
            memset(prompted, 0, sizeof(prompted));
            if (rc == 0) {
                libssh2_session_set_blocking(sess, 0);
                return true;
            }
        }
    }

    libssh2_session_set_blocking(sess, 0);
    return false;
}

/* ── SSH connect ────────────────────────────────────────────────────── */

static bool conn_ssh_connect(const photon_bbs_t *bbs)
{
    uint16_t port = photon_bbs_port(bbs);
    photon_sock_t fd = tcp_connect(bbs->addr, port);
    if (fd == PHOTON_INVALID_SOCK) return false;

    LIBSSH2_SESSION *sess = libssh2_session_init();
    if (!sess) { close(fd); set_error("libssh2_session_init failed"); return false; }

    libssh2_session_set_blocking(sess, 0);

    int rc;
    for (;;) {
        rc = libssh2_session_handshake(sess, fd);
        if (rc != LIBSSH2_ERROR_EAGAIN) break;
        sock_readable(fd, 50);
    }
    if (rc) { set_error("SSH handshake failed: %d", rc); goto fail; }

    /* Fingerprint check */
    const char *fp = libssh2_hostkey_hash(sess, LIBSSH2_HOSTKEY_HASH_SHA1);
    if (bbs->has_fingerprint && fp) {
        if (memcmp(fp, bbs->ssh_fingerprint, PHOTON_FINGERPRINT_LEN) != 0) {
            set_error("SSH host key mismatch for %s", bbs->addr);
            goto fail;
        }
    }

    if (!ssh_authenticate(sess,
                          bbs->user[0] ? bbs->user : getenv("USER"),
                          bbs->pass[0] ? bbs->pass : NULL)) {
        set_error("SSH authentication failed for %s@%s", bbs->user, bbs->addr);
        goto fail;
    }

    LIBSSH2_CHANNEL *ch = NULL;
    for (;;) {
        ch = libssh2_channel_open_session(sess);
        if (ch || libssh2_session_last_error(sess, NULL, NULL, 0) != LIBSSH2_ERROR_EAGAIN)
            break;
        sock_readable(fd, 50);
    }
    if (!ch) { set_error("SSH channel open failed"); goto fail; }

    /* Request PTY */
    for (;;) {
        rc = libssh2_channel_request_pty(ch, "xterm-256color");
        if (rc != LIBSSH2_ERROR_EAGAIN) break;
        sock_readable(fd, 50);
    }

    /* Start shell */
    for (;;) {
        rc = libssh2_channel_shell(ch);
        if (rc != LIBSSH2_ERROR_EAGAIN) break;
        sock_readable(fd, 50);
    }
    if (rc) { set_error("SSH shell request failed: %d", rc); libssh2_channel_free(ch); goto fail; }

    libssh2_channel_set_blocking(ch, 0);

    s_active->ssh_sock    = fd;
    s_active->ssh_session = sess;
    s_active->ssh_channel = ch;
    pthread_mutex_init(&s_active->ssh_mu, NULL);

    create_io_thread(&s_active->rx.thread, ssh_rx_thread, s_active);
    create_io_thread(&s_active->tx.thread, ssh_tx_thread, s_active);

    /* Wait for threads to start */
    while (!atomic_load(&s_active->rx.running) || !atomic_load(&s_active->tx.running)) {
        struct timespec ts = {0, 1000000L};
        nanosleep(&ts, NULL);
    }
    return true;

fail:
    libssh2_session_disconnect(sess, "");
    libssh2_session_free(sess);
    close(fd);
    return false;
}

/* ── Telnet connect ─────────────────────────────────────────────────── */

static bool conn_telnet_connect(const photon_bbs_t *bbs)
{
    uint16_t port = photon_bbs_port(bbs);
    photon_sock_t fd = tcp_connect(bbs->addr, port);
    if (fd == PHOTON_INVALID_SOCK) return false;

    s_active->telnet_sock = fd;
    pthread_mutex_init(&s_active->telnet_send_mu, NULL);

    create_io_thread(&s_active->rx.thread, telnet_rx_thread, s_active);
    create_io_thread(&s_active->tx.thread, telnet_tx_thread, s_active);

    /* Wait for threads to start */
    while (!atomic_load(&s_active->rx.running) || !atomic_load(&s_active->tx.running)) {
        struct timespec ts = {0, 1000000L};
        nanosleep(&ts, NULL);
    }

    telnet_send_initial_options();
    return true;
}

/* ── PTY / shell connect (POSIX) ────────────────────────────────────── */

#ifdef __unix__
#include <termios.h>
#include <sys/ioctl.h>

/* PTY I/O threads */
static void *pty_rx_thread(void *arg)
{
    photon_conn_t *C = arg;
    unsigned char buf[65536];
    atomic_store(&C->rx.running, 1);
    while (!atomic_load(&C->terminate) && C->pty_fd >= 0) {
        if (!sock_readable(C->pty_fd, 100)) continue;
        ssize_t rd = read(C->pty_fd, buf, sizeof(buf)); /* drain as much as kernel has */
        if (rd <= 0) break;
        size_t sent = 0;
        while (sent < (size_t)rd && !atomic_load(&C->terminate)) {
            pthread_mutex_lock(&C->inbuf->mu);
            sent += ring_put(C->inbuf, buf + sent, (size_t)rd - sent);
            pthread_mutex_unlock(&C->inbuf->mu);
        }
    }
    atomic_store(&C->rx.running, 2);
    return NULL;
}

static void *pty_tx_thread(void *arg)
{
    photon_conn_t *C = arg;
    unsigned char buf[4096];
    atomic_store(&C->tx.running, 1);
    while (!atomic_load(&C->terminate)) {
        size_t n = ring_wait_bytes(C->outbuf, 1, 100);
        if (n == 0) {
            if (C->pty_fd < 0 || atomic_load(&C->rx.running) == 2) break;
            continue;
        }
        pthread_mutex_lock(&C->outbuf->mu);
        n = ring_get(C->outbuf, buf, sizeof(buf));
        pthread_mutex_unlock(&C->outbuf->mu);
        if (n == 0) continue;
        write(C->pty_fd, buf, n);
    }
    atomic_store(&C->tx.running, 2);
    return NULL;
}

static bool conn_shell_connect(const photon_bbs_t *bbs)
{
#ifdef __APPLE__
    int master;
    char slave_name[128];
    master = posix_openpt(O_RDWR | O_NOCTTY);
    if (master < 0) { set_error("posix_openpt failed: %s", strerror(errno)); return false; }
    grantpt(master);
    unlockpt(master);
    ptsname_r(master, slave_name, sizeof(slave_name));
#else
    int master = posix_openpt(O_RDWR | O_NOCTTY);
    char *slave_name_p;
    if (master < 0) { set_error("posix_openpt failed: %s", strerror(errno)); return false; }
    grantpt(master);
    unlockpt(master);
    slave_name_p = ptsname(master);
    char slave_name[256];
    strlcpy(slave_name, slave_name_p ? slave_name_p : "", sizeof(slave_name));
#endif

    pid_t pid = fork();
    if (pid < 0) { set_error("fork failed"); close(master); return false; }
    if (pid == 0) {
        /* Child */
        setsid();
        int slave = open(slave_name, O_RDWR);
#ifdef TIOCSCTTY
        ioctl(slave, TIOCSCTTY, 0);
#endif
        dup2(slave, STDIN_FILENO);
        dup2(slave, STDOUT_FILENO);
        dup2(slave, STDERR_FILENO);
        if (slave > 2) close(slave);
        close(master);

        /* Ensure the child sees a sane environment.  When launched from
         * Finder / Spotlight the parent inherits launchd's minimal env.
         * macOS /etc/bashrc and /etc/zshrc source /etc/{bash,zsh}rc_$TERM_PROGRAM
         * so setting TERM_PROGRAM lets the system rc files do their job. */
        setenv("TERM", "xterm-256color", 1);
        setenv("COLORTERM", "truecolor", 1);
        setenv("TERM_PROGRAM", "PhotonTERM", 1);
        if (!getenv("LANG"))
            setenv("LANG", "en_US.UTF-8", 1);
        if (!getenv("HOME")) {
            struct passwd *pw = getpwuid(getuid());
            if (pw && pw->pw_dir) setenv("HOME", pw->pw_dir, 1);
        }
        if (!getenv("SHELL")) {
            struct passwd *pw = getpwuid(getuid());
            if (pw && pw->pw_shell && pw->pw_shell[0])
                setenv("SHELL", pw->pw_shell, 1);
        }

        if (bbs && bbs->addr[0]) {
            /* Custom command via shell */
            execl("/bin/sh", "/bin/sh", "-c", bbs->addr, (char *)NULL);
        } else {
            /* Prefer SHELL env, fall back to the user's login shell from
             * the password database.  When launched from Finder, launchd
             * may not set SHELL at all. */
            const char *shell = getenv("SHELL");
            if (!shell || !shell[0]) {
                struct passwd *pw = getpwuid(getuid());
                if (pw && pw->pw_shell && pw->pw_shell[0])
                    shell = pw->pw_shell;
                else
                    shell = "/bin/sh";
            }
            execl(shell, shell, "-l", (char *)NULL);
        }
        _exit(1);
    }
    s_active->pty_fd = master;
    create_io_thread(&s_active->rx.thread, pty_rx_thread, s_active);
    create_io_thread(&s_active->tx.thread, pty_tx_thread, s_active);
    while (!atomic_load(&s_active->rx.running) || !atomic_load(&s_active->tx.running)) {
        struct timespec ts = {0, 1000000L};
        nanosleep(&ts, NULL);
    }
    return true;
}
#endif /* __unix__ */

/* ── Public API ─────────────────────────────────────────────────────── */

bool photon_conn_connect(const photon_bbs_t *bbs)
{
#ifdef _WIN32
    /* Ensure Winsock is initialised */
    static int wsa_ok = 0;
    if (!wsa_ok) {
        WSADATA wd;
        if (WSAStartup(MAKEWORD(2, 2), &wd) != 0) {
            set_error("WSAStartup failed");
            return false;
        }
        wsa_ok = 1;
    }
#endif

    /* Allocate a fresh connection handle if needed */
    if (!s_active) {
        s_active = calloc(1, sizeof(photon_conn_t));
        if (!s_active) { set_error("out of memory"); return false; }
    } else {
        /* Reset existing handle - caller must have closed the prior connection */
        memset(s_active, 0, sizeof(photon_conn_t));
    }
    s_active->telnet_sock = PHOTON_INVALID_SOCK;
    s_active->ssh_sock    = PHOTON_INVALID_SOCK;
    s_active->pty_fd      = -1;
    atomic_store(&s_active->terminate, false);
    atomic_store(&s_active->rx.running, 0);
    atomic_store(&s_active->tx.running, 0);

    s_active->inbuf  = ring_alloc(RING_SIZE);
    s_active->outbuf = ring_alloc(RING_SIZE);
    if (!s_active->inbuf || !s_active->outbuf) {
        set_error("out of memory");
        ring_free(s_active->inbuf);
        ring_free(s_active->outbuf);
        s_active->inbuf = s_active->outbuf = NULL;
        return false;
    }
    s_active->type = bbs->conn_type;

    bool ok = false;
    switch (bbs->conn_type) {
        case PHOTON_CONN_TELNET: ok = conn_telnet_connect(bbs); break;
        case PHOTON_CONN_SSH:    ok = conn_ssh_connect(bbs);    break;
#ifdef __unix__
        case PHOTON_CONN_SHELL:  ok = conn_shell_connect(bbs);  break;
#endif
        default:
            set_error("unsupported connection type %d", bbs->conn_type);
            break;
    }
    if (!ok) {
        ring_free(s_active->inbuf);
        ring_free(s_active->outbuf);
        s_active->inbuf = s_active->outbuf = NULL;
    }
    return ok;
}

void photon_conn_close(void)
{
    if (!s_active) return;
    atomic_store(&s_active->terminate, true);

    /* Unblock any ring waits */
    if (s_active->inbuf) {
        pthread_mutex_lock(&s_active->inbuf->mu);
        pthread_cond_broadcast(&s_active->inbuf->in_cond);
        pthread_cond_broadcast(&s_active->inbuf->out_cond);
        pthread_mutex_unlock(&s_active->inbuf->mu);
    }
    if (s_active->outbuf) {
        pthread_mutex_lock(&s_active->outbuf->mu);
        pthread_cond_broadcast(&s_active->outbuf->in_cond);
        pthread_cond_broadcast(&s_active->outbuf->out_cond);
        pthread_mutex_unlock(&s_active->outbuf->mu);
    }

    /* Close sockets / fds */
    if (s_active->telnet_sock != PHOTON_INVALID_SOCK) {
        shutdown(s_active->telnet_sock, SHUT_RDWR);
        close(s_active->telnet_sock);
        s_active->telnet_sock = PHOTON_INVALID_SOCK;
        pthread_mutex_destroy(&s_active->telnet_send_mu);
    }
    if (s_active->ssh_sock != PHOTON_INVALID_SOCK) {
        if (s_active->ssh_channel) {
            libssh2_channel_close(s_active->ssh_channel);
            libssh2_channel_free(s_active->ssh_channel);
            s_active->ssh_channel = NULL;
        }
        if (s_active->ssh_session) {
            libssh2_session_disconnect(s_active->ssh_session, "Goodbye");
            libssh2_session_free(s_active->ssh_session);
            s_active->ssh_session = NULL;
        }
        close(s_active->ssh_sock);
        s_active->ssh_sock = PHOTON_INVALID_SOCK;
        pthread_mutex_destroy(&s_active->ssh_mu);
    }
#ifdef __unix__
    if (s_active->pty_fd >= 0) {
        close(s_active->pty_fd);
        s_active->pty_fd = -1;
    }
#endif

    /* Join threads */
    if (atomic_load(&s_active->rx.running) > 0) pthread_join(s_active->rx.thread, NULL);
    if (atomic_load(&s_active->tx.running) > 0) pthread_join(s_active->tx.thread, NULL);

    ring_free(s_active->inbuf);
    ring_free(s_active->outbuf);
    s_active->inbuf = s_active->outbuf = NULL;
}

bool photon_conn_connected(void)
{
    if (!s_active) return false;
    return s_active->inbuf &&
           atomic_load(&s_active->rx.running) == 1 &&
           atomic_load(&s_active->tx.running) == 1;
}

size_t photon_conn_data_waiting(void)
{
    if (!s_active || !s_active->inbuf) return 0;
    pthread_mutex_lock(&s_active->inbuf->mu);
    size_t n = ring_bytes(s_active->inbuf);
    pthread_mutex_unlock(&s_active->inbuf->mu);
    return n;
}

int photon_conn_recv(void *buf, size_t buflen, unsigned int timeout_ms)
{
    if (!s_active || !s_active->inbuf) return -1;
    size_t avail = ring_wait_bytes(s_active->inbuf, 1, timeout_ms);
    if (avail == 0) return 0;
    pthread_mutex_lock(&s_active->inbuf->mu);
    size_t n = ring_get(s_active->inbuf, buf, buflen);
    pthread_mutex_unlock(&s_active->inbuf->mu);
    return (int)n;
}

int photon_conn_send(const void *buf, size_t buflen, unsigned int timeout_ms)
{
    if (!s_active || !s_active->outbuf) return -1;
    size_t sent = 0;
    long double deadline = photon_mono_time() + (long double)timeout_ms / 1000.0L;
    while (sent < buflen) {
        pthread_mutex_lock(&s_active->outbuf->mu);
        size_t n = ring_put(s_active->outbuf, (const char *)buf + sent, buflen - sent);
        pthread_mutex_unlock(&s_active->outbuf->mu);
        sent += n;
        if (sent < buflen) {
            if (timeout_ms == 0) break;
            if (photon_mono_time() >= deadline) break;
            struct timespec ts = {0, 1000000L};
            nanosleep(&ts, NULL);
        }
    }
    return (int)sent;
}

/* Notify the remote that the terminal window has been resized.
 *
 * SSH: sends channel_request_pty_size (RFC 4254 §6.7).
 * Telnet: sends NAWS SB (RFC 1073).
 * PTY: sets TIOCSWINSZ on the master fd.
 */
void photon_conn_resize(int cols, int rows)
{
    if (cols < 1 || rows < 1) return;

    if (s_active->type == PHOTON_CONN_SSH && s_active->ssh_channel) {
        /* Non-blocking: best-effort, ignore EAGAIN */
        libssh2_channel_request_pty_size(s_active->ssh_channel, cols, rows);
    }

    if (s_active->type == PHOTON_CONN_TELNET &&
        s_active->telnet_sock != PHOTON_INVALID_SOCK) {
        unsigned char naws[9] = {
            TELNET_IAC, TELNET_SB, TELOPT_NAWS,
            (unsigned char)(cols >> 8), (unsigned char)(cols & 0xFF),
            (unsigned char)(rows >> 8), (unsigned char)(rows & 0xFF),
            TELNET_IAC, TELNET_SE
        };
        pthread_mutex_lock(&s_active->telnet_send_mu);
        send(s_active->telnet_sock, naws, sizeof(naws), 0);
        pthread_mutex_unlock(&s_active->telnet_send_mu);
    }

#if !defined(_WIN32)
    if (s_active->type == PHOTON_CONN_SHELL && s_active->pty_fd >= 0) {
        struct winsize ws;
        memset(&ws, 0, sizeof(ws));
        ws.ws_col = (unsigned short)cols;
        ws.ws_row = (unsigned short)rows;
        ioctl(s_active->pty_fd, TIOCSWINSZ, &ws);
    }
#endif
}


/* ── Multi-connection handle API ─────────────────────────────────────── */

/* Allocate a new, unconnected connection handle (for multi-tab). */
photon_conn_t *photon_conn_new(void)
{
    photon_conn_t *C = calloc(1, sizeof(photon_conn_t));
    if (!C) return NULL;
    C->telnet_sock = PHOTON_INVALID_SOCK;
    C->ssh_sock    = PHOTON_INVALID_SOCK;
    C->pty_fd      = -1;
    return C;
}

/* Free a connection handle.  Connection must already be closed. */
void photon_conn_free(photon_conn_t *C)
{
    if (!C) return;
    ring_free(C->inbuf);
    ring_free(C->outbuf);
    free(C);
}

/* Switch which handle send/recv/connected/etc. operate on. */
void photon_conn_set_active(photon_conn_t *C)
{
    s_active = C;
}

/* Return the currently active connection handle. */
photon_conn_t *photon_conn_get_active(void)
{
    return s_active;
}
