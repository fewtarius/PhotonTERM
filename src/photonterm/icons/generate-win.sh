#!/bin/sh
# Generate Windows ICO from photonterm.svg using rsvg-convert + ImageMagick
# Run from the repo root: sh src/photonterm/icons/generate-win.sh
set -e

SVG="src/photonterm/icons/photonterm.svg"
TMPD=$(mktemp -d)

for size in 16 20 24 30 32 40 48 64 96 128 256; do
	rsvg-convert -w "$size" -h "$size" "$SVG" -o "$TMPD/ico${size}.png"
done

magick "$TMPD"/ico*.png src/photonterm/photonterm.ico
rm -rf "$TMPD"
echo "Generated src/photonterm/photonterm.ico"
