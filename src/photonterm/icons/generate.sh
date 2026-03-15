#!/bin/sh

# Generate macOS ICNS from photonterm.svg using rsvg-convert + iconutil
# Run from the repo root: sh src/photonterm/icons/generate.sh
set -e

SVG="src/photonterm/icons/photonterm.svg"
ICONSET="src/photonterm/PhotonTERM.iconset"

mkdir -p "$ICONSET"

rsvg-convert -w 16   -h 16   "$SVG" -o "$ICONSET/icon_16x16.png"
rsvg-convert -w 32   -h 32   "$SVG" -o "$ICONSET/icon_16x16@2x.png"
rsvg-convert -w 32   -h 32   "$SVG" -o "$ICONSET/icon_32x32.png"
rsvg-convert -w 64   -h 64   "$SVG" -o "$ICONSET/icon_32x32@2x.png"
rsvg-convert -w 128  -h 128  "$SVG" -o "$ICONSET/icon_128x128.png"
rsvg-convert -w 256  -h 256  "$SVG" -o "$ICONSET/icon_128x128@2x.png"
rsvg-convert -w 256  -h 256  "$SVG" -o "$ICONSET/icon_256x256.png"
rsvg-convert -w 512  -h 512  "$SVG" -o "$ICONSET/icon_256x256@2x.png"
rsvg-convert -w 512  -h 512  "$SVG" -o "$ICONSET/icon_512x512.png"
rsvg-convert -w 1024 -h 1024 "$SVG" -o "$ICONSET/icon_512x512@2x.png"

iconutil -c icns "$ICONSET" -o src/photonterm/PhotonTERM.icns
echo "Generated src/photonterm/PhotonTERM.icns"
