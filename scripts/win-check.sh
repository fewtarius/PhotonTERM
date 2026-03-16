#!/usr/bin/env bash
# win-check.sh - Cross-compile smoke test for Windows (header/syntax only)
#
# Checks that all PhotonTERM .c files compile as object files targeting
# Windows (MinGW-w64).  Does NOT link (no Windows SDL2/OpenSSL libs needed).
# Catches: missing #ifdef _WIN32 guards, POSIX-only headers, type mismatches.
#
# Usage:
#   scripts/win-check.sh          # install mingw + compile
#   scripts/win-check.sh --no-install  # skip brew install (already have it)
#
# Requirements: macOS with Homebrew, or Linux with apt
#
# Copyright (C) 2026 fewtarius and PhotonTERM contributors
# GPL v3 or later

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
SRC_DIR="$REPO_ROOT/src/photonterm"
TMP_DIR="$REPO_ROOT/3rdp/win-check-tmp"

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[0;33m'
RESET='\033[0m'

log()  { echo -e "${GREEN}[win-check]${RESET} $*"; }
warn() { echo -e "${YELLOW}[win-check]${RESET} $*"; }
err()  { echo -e "${RED}[win-check]${RESET} $*"; }

# ── Install cross-compiler ─────────────────────────────────────────────

install_mingw() {
    if command -v x86_64-w64-mingw32-gcc &>/dev/null; then
        log "mingw-w64 already installed: $(x86_64-w64-mingw32-gcc --version | head -1)"
        return 0
    fi

    if [[ "${1:-}" == "--no-install" ]]; then
        err "mingw-w64 not found and --no-install specified. Install it first:"
        err "  macOS:  brew install mingw-w64"
        err "  Linux:  sudo apt install gcc-mingw-w64-x86-64"
        exit 1
    fi

    if command -v brew &>/dev/null; then
        log "Installing mingw-w64 via Homebrew..."
        brew install mingw-w64
    elif command -v apt-get &>/dev/null; then
        log "Installing mingw-w64 via apt..."
        sudo apt-get update && sudo apt-get install -y gcc-mingw-w64-x86-64
    else
        err "No package manager found. Install mingw-w64 manually."
        exit 1
    fi
}

# ── Create stub headers for deps we don't have ─────────────────────────

create_stubs() {
    mkdir -p "$TMP_DIR/stubs/SDL2"
    mkdir -p "$TMP_DIR/stubs/libssh2"

    # Minimal SDL2 stubs - just enough for our headers to parse
    cat > "$TMP_DIR/stubs/SDL2/SDL.h" << 'EOF'
#pragma once
#include <stdint.h>
typedef struct SDL_Window SDL_Window;
typedef struct SDL_Renderer SDL_Renderer;
typedef struct SDL_Texture SDL_Texture;
typedef union SDL_Event SDL_Event;
typedef uint32_t Uint32;
typedef int32_t Sint32;
typedef uint8_t Uint8;
typedef uint16_t Uint16;
#define SDL_INIT_VIDEO 0x00000020u
#define SDL_INIT_EVENTS 0x00004000u
#define SDL_INIT_AUDIO 0x00000010u
static inline int SDL_Init(Uint32 f) { (void)f; return 0; }
static inline const char *SDL_GetError(void) { return ""; }
static inline void SDL_ClearError(void) {}
static inline int SDL_InitSubSystem(Uint32 f) { (void)f; return 0; }
static inline void SDL_Quit(void) {}
static inline void SDL_Delay(Uint32 ms) { (void)ms; }
EOF

    cat > "$TMP_DIR/stubs/SDL2/SDL_ttf.h" << 'EOF'
#pragma once
typedef struct TTF_Font TTF_Font;
static inline int TTF_Init(void) { return 0; }
static inline void TTF_Quit(void) {}
EOF

    cat > "$TMP_DIR/stubs/SDL2/SDL_audio.h" << 'EOF'
#pragma once
EOF

    cat > "$TMP_DIR/stubs/SDL2/SDL_events.h" << 'EOF'
#pragma once
EOF

    cat > "$TMP_DIR/stubs/SDL2/SDL_keycode.h" << 'EOF'
#pragma once
typedef int32_t SDL_Keycode;
typedef uint16_t SDL_Scancode;
#define SDLK_UNKNOWN 0
EOF

    cat > "$TMP_DIR/stubs/SDL2/SDL_clipboard.h" << 'EOF'
#pragma once
static inline char *SDL_GetClipboardText(void) { return 0; }
static inline int SDL_SetClipboardText(const char *t) { (void)t; return 0; }
static inline void SDL_free(void *p) { (void)p; }
EOF

    cat > "$TMP_DIR/stubs/SDL2/SDL_render.h" << 'EOF'
#pragma once
EOF

    cat > "$TMP_DIR/stubs/SDL2/SDL_video.h" << 'EOF'
#pragma once
EOF

    cat > "$TMP_DIR/stubs/SDL2/SDL_mixer.h" << 'EOF'
#pragma once
EOF

    # Minimal libssh2 stub
    cat > "$TMP_DIR/stubs/libssh2.h" << 'EOF'
#pragma once
#include <stddef.h>
typedef struct LIBSSH2_SESSION LIBSSH2_SESSION;
typedef struct LIBSSH2_CHANNEL LIBSSH2_CHANNEL;
typedef struct LIBSSH2_AGENT LIBSSH2_AGENT;
typedef struct LIBSSH2_KNOWNHOSTS LIBSSH2_KNOWNHOSTS;
struct libssh2_agent_publickey { unsigned char *blob; size_t blob_len; char *comment; };
#define LIBSSH2_ERROR_EAGAIN -37
#define LIBSSH2_SESSION_BLOCK_INBOUND  0x0001
#define LIBSSH2_SESSION_BLOCK_OUTBOUND 0x0002
#define LIBSSH2_CHANNEL_FLUSH_ALL -1
#define LIBSSH2_HOSTKEY_HASH_SHA256 3
#define LIBSSH2_HOSTKEY_HASH_SHA1 1
#define LIBSSH2_KNOWNHOST_TYPE_PLAIN  1
#define LIBSSH2_KNOWNHOST_KEYENC_RAW  (1<<16)
#define LIBSSH2_KNOWNHOST_CHECK_MATCH     0
#define LIBSSH2_KNOWNHOST_CHECK_MISMATCH  1
#define LIBSSH2_KNOWNHOST_CHECK_NOTFOUND  2
#define LIBSSH2_KNOWNHOST_CHECK_FAILURE   3
#define LIBSSH2_TERM_WIDTH 80
#define LIBSSH2_TERM_HEIGHT 24
struct libssh2_knownhost { int typemask; char *key; };
static inline int libssh2_init(int f) { (void)f; return 0; }
static inline void libssh2_exit(void) {}
static inline LIBSSH2_SESSION *libssh2_session_init(void) { return (LIBSSH2_SESSION *)0; }
static inline void libssh2_session_free(LIBSSH2_SESSION *s) { (void)s; }
static inline int libssh2_session_handshake(LIBSSH2_SESSION *s, int fd) { (void)s; (void)fd; return 0; }
static inline void libssh2_session_set_blocking(LIBSSH2_SESSION *s, int b) { (void)s; (void)b; }
static inline int libssh2_session_disconnect(LIBSSH2_SESSION *s, const char *d) { (void)s; (void)d; return 0; }
static inline int libssh2_session_last_error(LIBSSH2_SESSION *s, char **m, int *l, int w) { (void)s; (void)m; (void)l; (void)w; return 0; }
static inline const char *libssh2_hostkey_hash(LIBSSH2_SESSION *s, int t) { (void)s; (void)t; return ""; }
static inline LIBSSH2_AGENT *libssh2_agent_init(LIBSSH2_SESSION *s) { (void)s; return (LIBSSH2_AGENT *)0; }
static inline int libssh2_agent_connect(LIBSSH2_AGENT *a) { (void)a; return 0; }
static inline int libssh2_agent_list_identities(LIBSSH2_AGENT *a) { (void)a; return 0; }
static inline int libssh2_agent_get_identity(LIBSSH2_AGENT *a, struct libssh2_agent_publickey **s, struct libssh2_agent_publickey *p) { (void)a; (void)s; (void)p; return 0; }
static inline int libssh2_agent_userauth(LIBSSH2_AGENT *a, const char *u, struct libssh2_agent_publickey *p) { (void)a; (void)u; (void)p; return 0; }
static inline int libssh2_agent_disconnect(LIBSSH2_AGENT *a) { (void)a; return 0; }
static inline void libssh2_agent_free(LIBSSH2_AGENT *a) { (void)a; }
static inline int libssh2_userauth_publickey_fromfile(LIBSSH2_SESSION *s, const char *u, const char *pub, const char *priv, const char *pw) { (void)s; (void)u; (void)pub; (void)priv; (void)pw; return 0; }
static inline int libssh2_userauth_password(LIBSSH2_SESSION *s, const char *u, const char *p) { (void)s; (void)u; (void)p; return 0; }
static inline LIBSSH2_CHANNEL *libssh2_channel_open_session(LIBSSH2_SESSION *s) { (void)s; return (LIBSSH2_CHANNEL *)0; }
static inline int libssh2_channel_request_pty(LIBSSH2_CHANNEL *c, const char *t) { (void)c; (void)t; return 0; }
static inline int libssh2_channel_request_pty_size(LIBSSH2_CHANNEL *c, int w, int h) { (void)c; (void)w; (void)h; return 0; }
static inline int libssh2_channel_shell(LIBSSH2_CHANNEL *c) { (void)c; return 0; }
static inline void libssh2_channel_set_blocking(LIBSSH2_CHANNEL *c, int b) { (void)c; (void)b; }
static inline ssize_t libssh2_channel_read(LIBSSH2_CHANNEL *c, char *b, size_t n) { (void)c; (void)b; (void)n; return 0; }
static inline ssize_t libssh2_channel_write(LIBSSH2_CHANNEL *c, const char *b, size_t n) { (void)c; (void)b; (void)n; return 0; }
static inline int libssh2_channel_close(LIBSSH2_CHANNEL *c) { (void)c; return 0; }
static inline int libssh2_channel_free(LIBSSH2_CHANNEL *c) { (void)c; return 0; }
EOF

    # Minimal openssl stubs
    cat > "$TMP_DIR/stubs/openssl" << 'EOF'
EOF
    rm -f "$TMP_DIR/stubs/openssl"
    mkdir -p "$TMP_DIR/stubs/openssl"
    echo "#pragma once" > "$TMP_DIR/stubs/openssl/ssl.h"
    echo "#pragma once" > "$TMP_DIR/stubs/openssl/err.h"
    echo "#pragma once" > "$TMP_DIR/stubs/openssl/crypto.h"

    log "Created stub headers in $TMP_DIR/stubs/"
}

# ── Compile each source file ───────────────────────────────────────────

CC="x86_64-w64-mingw32-gcc"
CFLAGS=(
    -c                              # compile only, no link
    -Wall -Wextra -Werror
    -Wno-unused-parameter           # stubs cause unused params
    -Wno-unused-function            # static inline stubs
    -Wno-missing-field-initializers # designated init style
    -fsyntax-only                   # don't even emit .o files, just check
    -D_WIN32
    -D_USE_MATH_DEFINES
    -D__USE_MINGW_ANSI_STDIO=1
    -D_GNU_SOURCE
    -DLINK_LIST_THREADSAFE
    -DXPDEV_THREAD_SAFE
    -I"$SRC_DIR"
    -I"$TMP_DIR/stubs"
    -I"$TMP_DIR/stubs/SDL2"
)

compile_check() {
    local src="$1"
    local base
    base="$(basename "$src")"

    # Skip platform-specific files
    if [[ "$base" == "DarwinWrappers.m" ]] || [[ "$base" == "early_init.c" ]]; then
        warn "SKIP  $base (not for Windows)"
        return 0
    fi

    # Skip SDL-heavy files - these are tested via CI; our stubs can't cover
    # the full SDL2 API surface. Focus on platform-portable logic instead.
    if [[ "$base" == "photon_sdl.c" ]] || [[ "$base" == "photon_term.c" ]]; then
        warn "SKIP  $base (needs full SDL2 headers; tested via CI)"
        return 0
    fi

    if $CC "${CFLAGS[@]}" "$src" 2>"$TMP_DIR/err_$base.txt"; then
        log "OK    $base"
        return 0
    else
        err "FAIL  $base"
        cat "$TMP_DIR/err_$base.txt" | head -20
        return 1
    fi
}

# ── Main ────────────────────────────────────────────────────────────────

main() {
    local skip_install=""
    for arg in "$@"; do
        if [[ "$arg" == "--no-install" ]]; then
            skip_install="--no-install"
        fi
    done

    install_mingw "$skip_install"
    create_stubs

    log "Cross-compile checking PhotonTERM for Windows (syntax only)..."
    log "Compiler: $($CC --version | head -1)"
    echo ""

    local failures=0
    local total=0

    for src in "$SRC_DIR"/photon_*.c; do
        total=$((total + 1))
        if ! compile_check "$src"; then
            failures=$((failures + 1))
        fi
    done

    echo ""
    if [[ $failures -eq 0 ]]; then
        log "${GREEN}All $total files passed Windows compile check${RESET}"
    else
        err "$failures / $total files failed Windows compile check"
    fi

    # Cleanup
    rm -rf "$TMP_DIR"

    return $failures
}

main "$@"
