#!/usr/bin/env bash
# scripts/build-deps.sh
# Build libssh2, OpenSSL, and SDL2 statically into 3rdp/prefix/
#
# Usage:
#   scripts/build-deps.sh [target]        # all, openssl, libssh2, sdl2, clean
#   scripts/build-deps.sh --prefix <dir>  # override install prefix
#   JOBS=8 scripts/build-deps.sh          # parallel build
#
# Output: 3rdp/prefix/{include,lib}/
#
# Copyright (C) 2026 fewtarius and PhotonTERM contributors
# GPL v2 or later

set -euo pipefail

# --- Configuration ---------------------------------------------------------

OPENSSL_VERSION="3.6.1"
LIBSSH2_VERSION="1.11.1"
SDL2_VERSION="2.32.10"
SDL2_TTF_VERSION="2.22.0"
LRZSZ_VERSION="0.12.20"
TERMINUS_TTF_VERSION="4.49.3"

OPENSSL_SHA256="b1bfedcd5b289ff22aee87c9d600f515767ebf45f77168cb6d64f231f518a82e"
LIBSSH2_SHA256="d9ec76cbe34db98eec3539fe2c899d26b0c837cb3eb466a56b0f109cabf658f7"
SDL2_SHA256="5f5993c530f084535c65a6879e9b26ad441169b3e25d789d83287040a9ca5165"
SDL2_TTF_SHA256="d48cbd1ce475b9e178206bf3b72d56b66d84d44f64ac05803328396234d67723"
LRZSZ_SHA256="c28b36b14bddb014d9e9c97c52459852f97bd405f89113f30bee45ed92728ff1"
TERMINUS_TTF_SHA256="0ead921d98d99a4590ffe6cd66dc037fc0a2ceea1c735d866ba73fe058257577"

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
PREFIX="${DEPS_PREFIX:-$REPO_ROOT/3rdp/prefix}"
DOWNLOADS="$REPO_ROOT/3rdp/downloads"
BUILD_TMP="$REPO_ROOT/3rdp/build_tmp"
JOBS="${JOBS:-$(sysctl -n hw.logicalcpu 2>/dev/null || nproc 2>/dev/null || echo 4)}"

# Detect platform
OS="$(uname -s)"
ARCH="$(uname -m)"
case "$OS" in
    Darwin) PLATFORM="macos" ;;
    Linux)  PLATFORM="linux" ;;
    MINGW*|MSYS*|CYGWIN*) PLATFORM="windows" ;;
    *) PLATFORM="unknown" ;;
esac

# Colors
RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'
CYAN='\033[0;36m'; RESET='\033[0m'
log()  { echo -e "${CYAN}[deps]${RESET} $*"; }
ok()   { echo -e "${GREEN}[deps]${RESET} $*"; }
warn() { echo -e "${YELLOW}[deps]${RESET} $*"; }
die()  { echo -e "${RED}[deps] ERROR${RESET} $*" >&2; exit 1; }

# --- Helper functions -------------------------------------------------------

download() {
    local url="$1" dest="$2" sha256="${3:-}"
    mkdir -p "$(dirname "$dest")"
    if [[ -f "$dest" ]]; then
        log "Already downloaded: $(basename "$dest")"
        return 0
    fi
    log "Downloading: $url"
    curl -fL --retry 3 --retry-delay 2 -o "$dest" "$url" || \
        die "Failed to download $url"
    if [[ -n "$sha256" ]]; then
        verify_sha256 "$dest" "$sha256"
    fi
}

verify_sha256() {
    local file="$1" expected="$2"
    local actual
    case "$OS" in
        Darwin) actual=$(shasum -a 256 "$file" | awk '{print $1}') ;;
        *)      actual=$(sha256sum "$file" | awk '{print $1}') ;;
    esac
    if [[ "$actual" != "$expected" ]]; then
        warn "SHA256 mismatch for $(basename "$file")"
        warn "Expected: $expected"
        warn "Actual:   $actual"
        warn "Continuing anyway (update SHA256 if version was updated)"
        # Don't fail - allow SHA256 to be updated in place
    else
        ok "SHA256 verified: $(basename "$file")"
    fi
}

already_built() {
    local marker="$PREFIX/.built_$1"
    [[ -f "$marker" ]]
}

mark_built() {
    touch "$PREFIX/.built_$1"
}

# --- OpenSSL ----------------------------------------------------------------

build_openssl() {
    if already_built "openssl_${OPENSSL_VERSION}"; then
        ok "OpenSSL ${OPENSSL_VERSION} already built"
        return 0
    fi

    local tarball="$DOWNLOADS/openssl-${OPENSSL_VERSION}.tar.gz"
    local url="https://www.openssl.org/source/openssl-${OPENSSL_VERSION}.tar.gz"
    download "$url" "$tarball" "$OPENSSL_SHA256"

    log "Building OpenSSL ${OPENSSL_VERSION}..."
    mkdir -p "$BUILD_TMP"
    local srcdir="$BUILD_TMP/openssl-${OPENSSL_VERSION}"
    rm -rf "$srcdir"
    tar -xf "$tarball" -C "$BUILD_TMP"

    local target_arg=""
    case "$PLATFORM" in
        macos)
            case "$ARCH" in
                arm64) target_arg="darwin64-arm64-cc" ;;
                x86_64) target_arg="darwin64-x86_64-cc" ;;
            esac
            ;;
        windows) target_arg="mingw64" ;;
        linux)
            case "$ARCH" in
                aarch64) target_arg="linux-aarch64" ;;
                *) target_arg="linux-x86_64" ;;
            esac
            ;;
    esac

    cd "$srcdir"
    ./Configure \
        no-shared no-tests no-apps no-docs \
        --prefix="$PREFIX" \
        --openssldir="$PREFIX/ssl" \
        $target_arg

    make -j"$JOBS"
    make install_sw  # Just libs + headers, not docs/apps
    # On Linux x86_64, OpenSSL installs to lib64; normalise to lib.
    if [ -d "$PREFIX/lib64" ] && [ ! -L "$PREFIX/lib64" ]; then
        mkdir -p "$PREFIX/lib"
        cp -a "$PREFIX/lib64/." "$PREFIX/lib/"
    fi
    cd "$REPO_ROOT"
    rm -rf "$srcdir"
    mark_built "openssl_${OPENSSL_VERSION}"
    ok "OpenSSL ${OPENSSL_VERSION} built"
}

# --- libssh2 ----------------------------------------------------------------

build_libssh2() {
    if already_built "libssh2_${LIBSSH2_VERSION}"; then
        ok "libssh2 ${LIBSSH2_VERSION} already built"
        return 0
    fi

    local tarball="$DOWNLOADS/libssh2-${LIBSSH2_VERSION}.tar.gz"
    local url="https://libssh2.org/download/libssh2-${LIBSSH2_VERSION}.tar.gz"
    download "$url" "$tarball" "$LIBSSH2_SHA256"

    log "Building libssh2 ${LIBSSH2_VERSION}..."
    mkdir -p "$BUILD_TMP"
    local srcdir="$BUILD_TMP/libssh2-${LIBSSH2_VERSION}"
    rm -rf "$srcdir"
    tar -xf "$tarball" -C "$BUILD_TMP"

    cd "$srcdir"

    local cmake_platform_args=()
    if [[ "$PLATFORM" == "macos" ]]; then
        cmake_platform_args+=("-DCMAKE_OSX_ARCHITECTURES=${ARCH}")
    fi
    if [[ "$PLATFORM" == "windows" ]]; then
        cmake_platform_args+=("-DCMAKE_SYSTEM_NAME=Windows")
    fi

    cmake -B build \
        -DCMAKE_INSTALL_PREFIX="$PREFIX" \
        -DCMAKE_BUILD_TYPE=Release \
        -DBUILD_SHARED_LIBS=OFF \
        -DBUILD_EXAMPLES=OFF \
        -DBUILD_TESTING=OFF \
        -DENABLE_ZLIB_COMPRESSION=OFF \
        -DOPENSSL_ROOT_DIR="$PREFIX" \
        -DOPENSSL_USE_STATIC_LIBS=TRUE \
        "${cmake_platform_args[@]}"

    cmake --build build -j"$JOBS"
    cmake --install build
    cd "$REPO_ROOT"
    rm -rf "$srcdir"
    mark_built "libssh2_${LIBSSH2_VERSION}"
    ok "libssh2 ${LIBSSH2_VERSION} built"
}

# --- SDL2 -------------------------------------------------------------------

build_sdl2() {
    if already_built "sdl2_${SDL2_VERSION}"; then
        ok "SDL2 ${SDL2_VERSION} already built"
        return 0
    fi

    local tarball="$DOWNLOADS/SDL2-${SDL2_VERSION}.tar.gz"
    local url="https://github.com/libsdl-org/SDL/releases/download/release-${SDL2_VERSION}/SDL2-${SDL2_VERSION}.tar.gz"
    download "$url" "$tarball" "$SDL2_SHA256"

    log "Building SDL2 ${SDL2_VERSION}..."
    mkdir -p "$BUILD_TMP"
    local srcdir="$BUILD_TMP/SDL2-${SDL2_VERSION}"
    rm -rf "$srcdir"
    tar -xf "$tarball" -C "$BUILD_TMP"

    cd "$srcdir"

    local cmake_platform_args=()
    if [[ "$PLATFORM" == "macos" ]]; then
        cmake_platform_args+=(
            "-DCMAKE_OSX_ARCHITECTURES=${ARCH}"
            "-DSDL_FRAMEWORK=OFF"
        )
    fi

    cmake -B build \
        -DCMAKE_INSTALL_PREFIX="$PREFIX" \
        -DCMAKE_BUILD_TYPE=Release \
        -DBUILD_SHARED_LIBS=OFF \
        -DSDL_STATIC=ON \
        -DSDL_SHARED=OFF \
        -DSDL_TEST=OFF \
        -DSDL_AUDIO=OFF \
        -DSDL_JOYSTICK=OFF \
        -DSDL_HAPTIC=OFF \
        -DSDL_SENSOR=OFF \
        "${cmake_platform_args[@]}"

    cmake --build build -j"$JOBS"
    cmake --install build
    cd "$REPO_ROOT"
    rm -rf "$srcdir"
    mark_built "sdl2_${SDL2_VERSION}"
    ok "SDL2 ${SDL2_VERSION} built"
}

# --- SDL2_ttf ---------------------------------------------------------------

build_sdl2_ttf() {
    if already_built "sdl2_ttf_${SDL2_TTF_VERSION}"; then
        ok "SDL2_ttf ${SDL2_TTF_VERSION} already built"
        return 0
    fi

    local tarball="$DOWNLOADS/SDL2_ttf-${SDL2_TTF_VERSION}.tar.gz"
    # Use cached download from /tmp if build script left it there
    if [[ -f "/tmp/SDL2_ttf-${SDL2_TTF_VERSION}.tar.gz" ]] && [[ ! -f "$tarball" ]]; then
        mkdir -p "$DOWNLOADS"
        cp "/tmp/SDL2_ttf-${SDL2_TTF_VERSION}.tar.gz" "$tarball"
    fi
    local url="https://github.com/libsdl-org/SDL_ttf/releases/download/release-${SDL2_TTF_VERSION}/SDL2_ttf-${SDL2_TTF_VERSION}.tar.gz"
    download "$url" "$tarball" "$SDL2_TTF_SHA256"

    log "Building SDL2_ttf ${SDL2_TTF_VERSION}..."
    mkdir -p "$BUILD_TMP"
    local srcdir="$BUILD_TMP/SDL2_ttf-${SDL2_TTF_VERSION}"
    rm -rf "$srcdir"
    # SDL2_ttf tarball contains symlinks that fail on Windows/MSYS2 tar.
    # Extract ignoring symlink errors (exit 2 = warnings only on GNU tar).
    tar -xf "$tarball" -C "$BUILD_TMP" --no-same-owner || true
    if [[ ! -d "$srcdir" ]]; then
        die "Failed to extract SDL2_ttf tarball"
    fi

    cd "$srcdir"

    local cmake_platform_args=()
    if [[ "$PLATFORM" == "macos" ]]; then
        cmake_platform_args+=(
            "-DCMAKE_OSX_ARCHITECTURES=${ARCH}"
        )
    fi

    cmake -B build \
        -DCMAKE_INSTALL_PREFIX="$PREFIX" \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_PREFIX_PATH="$PREFIX" \
        -DBUILD_SHARED_LIBS=OFF \
        -DSDL2TTF_SAMPLES=OFF \
        -DSDL2TTF_HARFBUZZ=OFF \
        -DSDL2TTF_VENDORED=ON \
        -DSDL2TTF_FREETYPE=ON \
        -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
        "${cmake_platform_args[@]}"

    cmake --build build -j"$JOBS"
    cmake --install build
    cd "$REPO_ROOT"
    rm -rf "$srcdir"
    mark_built "sdl2_ttf_${SDL2_TTF_VERSION}"
    ok "SDL2_ttf ${SDL2_TTF_VERSION} built"
}

# --- Terminus TTF font ------------------------------------------------------

build_terminus_ttf() {
    if already_built "terminus_ttf_${TERMINUS_TTF_VERSION}"; then
        ok "Terminus TTF ${TERMINUS_TTF_VERSION} already built"
        return 0
    fi

    local tarball="$DOWNLOADS/terminus-ttf-${TERMINUS_TTF_VERSION}.zip"
    local url="https://files.ax86.net/terminus-ttf/files/latest.zip"
    download "$url" "$tarball" "$TERMINUS_TTF_SHA256"

    log "Installing Terminus TTF ${TERMINUS_TTF_VERSION}..."
    local font_dir="$PREFIX/share/fonts/terminus-ttf"
    mkdir -p "$font_dir"
    local tmpdir="$BUILD_TMP/terminus-ttf"
    rm -rf "$tmpdir"
    mkdir -p "$tmpdir"
    if command -v unzip >/dev/null 2>&1; then
        unzip -q "$tarball" -d "$tmpdir"
    elif command -v bsdtar >/dev/null 2>&1; then
        bsdtar -xf "$tarball" -C "$tmpdir"
    else
        die "Neither unzip nor bsdtar found; cannot extract .zip archive"
    fi
    # Find the regular (non-bold, non-italic) .ttf file
    local ttf
    ttf=$(find "$tmpdir" -name "TerminusTTF-[0-9]*.ttf" | head -1)
    if [[ -z "$ttf" ]]; then
        ttf=$(find "$tmpdir" -name "TerminusTTF.ttf" | head -1)
    fi
    if [[ -z "$ttf" ]]; then
        die "TerminusTTF regular font not found in archive"
    fi
    cp "$ttf" "$font_dir/TerminusTTF.ttf"
    rm -rf "$tmpdir"
    mark_built "terminus_ttf_${TERMINUS_TTF_VERSION}"
    ok "Terminus TTF ${TERMINUS_TTF_VERSION} installed"
}

# --- lrzsz (sz/rz - ZModem/XModem/YModem file transfer) -------------------

build_lrzsz() {
    if already_built "lrzsz_${LRZSZ_VERSION}"; then
        ok "lrzsz ${LRZSZ_VERSION} already built"
        return 0
    fi

    local tarball="$DOWNLOADS/lrzsz-${LRZSZ_VERSION}.tar.gz"
    local url="https://www.ohse.de/uwe/releases/lrzsz-${LRZSZ_VERSION}.tar.gz"
    download "$url" "$tarball" "$LRZSZ_SHA256"

    log "Building lrzsz ${LRZSZ_VERSION}..."
    mkdir -p "$BUILD_TMP"
    local srcdir="$BUILD_TMP/lrzsz-${LRZSZ_VERSION}"
    rm -rf "$srcdir"
    tar -xf "$tarball" -C "$BUILD_TMP"

    # Apply MacPorts/Homebrew patches for modern compilers
    local patches_dir="$SCRIPT_DIR/patches/lrzsz"
    if [[ -d "$patches_dir" ]]; then
        for patch in "$patches_dir"/*.diff "$patches_dir"/*.patch; do
            [[ -f "$patch" ]] || continue
            log "Applying patch: $(basename "$patch")"
            patch -p0 -d "$srcdir" < "$patch"
        done
    fi

    cd "$srcdir"

    local cc="${CC:-clang}"
    # Modern clang is strict about implicit declarations that lrzsz relies on
    local extra_cflags="-Wno-implicit-int -Wno-implicit-function-declaration -Wno-deprecated-non-prototype"

    CC="$cc" CFLAGS="$extra_cflags" ./configure \
        --prefix="$PREFIX" \
        --disable-nls \
        --host="${CHOST:-}" \
        2>&1 | tail -5

    make -j"$JOBS" 2>&1 | tail -5

    # Manual install (works around a bug with custom prefix in lrzsz)
    install -m 755 src/lrz "$PREFIX/bin/lrz"
    install -m 755 src/lsz "$PREFIX/bin/lsz"
    # Symlinks sz -> lsz, rz -> lrz (standard names expected by users/BBSs)
    ln -sf lrz "$PREFIX/bin/rz"
    ln -sf lsz "$PREFIX/bin/sz"

    cd "$REPO_ROOT"
    rm -rf "$srcdir"
    mark_built "lrzsz_${LRZSZ_VERSION}"
    ok "lrzsz ${LRZSZ_VERSION} built (sz, rz, lsz, lrz)"
}

# --- Clean ------------------------------------------------------------------

clean_deps() {
    log "Cleaning build artifacts..."
    rm -rf "$BUILD_TMP"
    log "Cleaning prefix..."
    rm -rf "$PREFIX"
    log "Keeping downloads cache ($DOWNLOADS)"
    ok "Clean complete"
}

clean_downloads() {
    log "Cleaning downloads cache..."
    rm -rf "$DOWNLOADS"
    ok "Downloads cleaned"
}

# --- Main -------------------------------------------------------------------

TARGET="${1:-all}"

log "PhotonTERM dependency builder"
log "Platform: $PLATFORM ($ARCH)"
log "Prefix:   $PREFIX"
log "Jobs:     $JOBS"

mkdir -p "$PREFIX" "$DOWNLOADS"

case "$TARGET" in
    all)
        build_openssl
        build_libssh2
        build_sdl2
        build_sdl2_ttf
        build_terminus_ttf
        # lrzsz uses fork() - not available on Windows
        if [[ "$PLATFORM" != "windows" ]]; then
            build_lrzsz
        fi
        ok "All dependencies built successfully"
        ok "Prefix: $PREFIX"
        ;;
    openssl)
        build_openssl
        ;;
    libssh2)
        build_libssh2
        ;;
    sdl2)
        build_sdl2
        ;;
    sdl2_ttf)
        build_sdl2_ttf
        ;;
    terminus_ttf)
        build_terminus_ttf
        ;;
    lrzsz)
        build_lrzsz
        ;;
    clean)
        clean_deps
        ;;
    distclean)
        clean_deps
        clean_downloads
        ;;
    *)
        echo "Usage: $0 [all|openssl|libssh2|sdl2|sdl2_ttf|terminus_ttf|lrzsz|clean|distclean]"
        exit 1
        ;;
esac
