#!/usr/bin/env bash
# PhotonTERM icon generation script
# Generates all required icon files from the two SVG masters:
#   photonterm.svg       -> 32px and larger
#   photonterm-mini.svg  -> 16-24px (small sizes)
#
# Requirements:
#   macOS: brew install librsvg imagemagick
#   Linux: apt install librsvg2-bin imagemagick
#
# Usage:
#   ./generate-icons.sh           # macOS + Linux PNGs + Windows ICO
#   ./generate-icons.sh macos     # macOS .icns only
#   ./generate-icons.sh linux     # Linux PNGs only
#   ./generate-icons.sh windows   # Windows .ico only

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PHOTONTERM_DIR="$(dirname "$SCRIPT_DIR")"

SVG_MAIN="${SCRIPT_DIR}/photonterm.svg"
SVG_MINI="${SCRIPT_DIR}/photonterm-mini.svg"

# Output locations (relative to src/photonterm/)
ICONSET_DIR="${PHOTONTERM_DIR}/PhotonTERM.iconset"
ICNS_OUT="${PHOTONTERM_DIR}/PhotonTERM.icns"
ICO_OUT="${PHOTONTERM_DIR}/photonterm.ico"

TARGET="${1:-all}"

check_deps() {
    local missing=0
    if ! command -v rsvg-convert &>/dev/null; then
        echo "ERROR: 'rsvg-convert' not found"
        echo "  macOS: brew install librsvg"
        echo "  Linux: sudo apt install librsvg2-bin"
        missing=1
    fi
    # ImageMagick 7 uses 'magick', IM6 uses 'convert'
    if command -v magick &>/dev/null; then
        CONVERT=magick
    elif command -v convert &>/dev/null; then
        CONVERT=convert
    else
        echo "ERROR: ImageMagick not found (need 'magick' or 'convert')"
        echo "  macOS: brew install imagemagick"
        echo "  Linux: sudo apt install imagemagick"
        missing=1
    fi
    [ "$missing" -eq 0 ] || exit 1
}

svg_to_png() {
    local svg="$1" size="$2" out="$3"
    rsvg-convert -w "$size" -h "$size" "$svg" -o "$out"
    echo "  ${size}x${size} -> $(basename "$out")"
}

# ---------- macOS .icns ----------
generate_macos() {
    echo "==> macOS: generating PhotonTERM.icns"
    rm -rf "$ICONSET_DIR"
    mkdir -p "$ICONSET_DIR"

    # icon_16x16.png   (mini SVG)
    svg_to_png "$SVG_MINI"  16  "${ICONSET_DIR}/icon_16x16.png"
    # icon_16x16@2x    (mini SVG at 32px)
    svg_to_png "$SVG_MINI"  32  "${ICONSET_DIR}/icon_16x16@2x.png"
    # icon_32x32       (main SVG)
    svg_to_png "$SVG_MAIN"  32  "${ICONSET_DIR}/icon_32x32.png"
    # icon_32x32@2x    (main SVG at 64px)
    svg_to_png "$SVG_MAIN"  64  "${ICONSET_DIR}/icon_32x32@2x.png"
    # icon_128x128
    svg_to_png "$SVG_MAIN"  128 "${ICONSET_DIR}/icon_128x128.png"
    # icon_128x128@2x
    svg_to_png "$SVG_MAIN"  256 "${ICONSET_DIR}/icon_128x128@2x.png"
    # icon_256x256
    svg_to_png "$SVG_MAIN"  256 "${ICONSET_DIR}/icon_256x256.png"
    # icon_256x256@2x
    svg_to_png "$SVG_MAIN"  512 "${ICONSET_DIR}/icon_256x256@2x.png"
    # icon_512x512
    svg_to_png "$SVG_MAIN"  512 "${ICONSET_DIR}/icon_512x512.png"
    # icon_512x512@2x  (1024px)
    svg_to_png "$SVG_MAIN" 1024 "${ICONSET_DIR}/icon_512x512@2x.png"

    iconutil -c icns "$ICONSET_DIR" -o "$ICNS_OUT"
    rm -rf "$ICONSET_DIR"
    echo "  -> ${ICNS_OUT}"
}

# ---------- Linux PNGs ----------
generate_linux() {
    echo "==> Linux: generating photonterm*.png"

    # Large sizes from main SVG
    for size in 32 36 48 64 256; do
        svg_to_png "$SVG_MAIN" "$size" "${PHOTONTERM_DIR}/photonterm${size}.png"
    done

    # Small sizes from mini SVG
    for size in 16 22 24; do
        svg_to_png "$SVG_MINI" "$size" "${PHOTONTERM_DIR}/photonterm${size}.png"
    done
}

# ---------- Windows .ico ----------
generate_windows() {
    echo "==> Windows: generating photonterm.ico"
    local tmpdir
    tmpdir=$(mktemp -d)

    # Sizes for Windows .ico (main SVG >= 32px, mini SVG for 16-30px)
    local sizes_mini=(16 20 24 30)
    local sizes_main=(32 36 40 48 60 64 72 80 96 100 125 150 200 256 400)

    for size in "${sizes_mini[@]}"; do
        svg_to_png "$SVG_MINI" "$size" "${tmpdir}/icon${size}.png"
    done
    for size in "${sizes_main[@]}"; do
        svg_to_png "$SVG_MAIN" "$size" "${tmpdir}/icon${size}.png"
    done

    # Build ordered list for convert
    local files=()
    for size in 16 20 24 30 32 36 40 48 60 64 72 80 96 100 125 150 200 256 400; do
        files+=("${tmpdir}/icon${size}.png")
    done

    $CONVERT "${files[@]}" "$ICO_OUT"
    rm -rf "$tmpdir"
    echo "  -> ${ICO_OUT}"
}

# ---------- main ----------
check_deps

case "$TARGET" in
    macos)
        generate_macos
        ;;
    linux)
        generate_linux
        ;;
    windows)
        generate_windows
        ;;
    all|"")
        generate_linux
        if [[ "$(uname)" == "Darwin" ]]; then
            generate_macos
        fi
        generate_windows
        ;;
    *)
        echo "Usage: $0 [macos|linux|windows|all]"
        exit 1
        ;;
esac

echo "Done."
