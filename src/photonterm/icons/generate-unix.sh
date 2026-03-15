#!/bin/sh

# Generate Linux hicolor PNG icons from photonterm.svg using rsvg-convert
# Run from the repo root: sh src/photonterm/icons/generate-unix.sh
set -e

SVG="src/photonterm/icons/photonterm.svg"

for size in 16 22 24 32 36 48 64 256; do
	rsvg-convert -w "$size" -h "$size" "$SVG" -o "src/photonterm/photonterm${size}.png"
	echo "Generated photonterm${size}.png"
done
