#!/bin/sh
# Regenerate the app icon (src/app.ico) from tools/app_icon.svg - the V2
# "edge-glow" isometric cube: glowing teal wireframe cube on a dark rounded
# square, matching the app's dark-instrument identity.
# Requires rsvg-convert (librsvg) + ImageMagick.
#   sh tools/gen_icon.sh
set -e
svg=tools/app_icon.svg
out=src/app.ico
tmp=$(mktemp -d)
# Render each icon size directly from the vector for crisp small sizes.
for s in 16 32 48 64 128 256; do
  rsvg-convert -w "$s" -h "$s" "$svg" -o "$tmp/icon_$s.png"
done
magick "$tmp/icon_16.png" "$tmp/icon_32.png" "$tmp/icon_48.png" \
       "$tmp/icon_64.png" "$tmp/icon_128.png" "$tmp/icon_256.png" "$out"
rm -rf "$tmp"
echo "wrote $out"
