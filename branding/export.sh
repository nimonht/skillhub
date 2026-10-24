#!/bin/sh
# Rebuild every raster asset from the SVG masters. Needs rsvg-convert, ImageMagick (magick) and Chrome/Chromium.
set -eu
cd "$(dirname "$0")"
ICONS=../data/icons/hicolor
mkdir -p png "$ICONS/scalable/apps"
for s in 16 24 32 48; do rsvg-convert -w $s -h $s skillhub-icon-small.svg -o png/skillhub-$s.png; done
for s in 64 128 256 512; do rsvg-convert -w $s -h $s skillhub-icon.svg -o png/skillhub-$s.png; done
magick png/skillhub-16.png png/skillhub-32.png png/skillhub-48.png png/skillhub-256.png favicon.ico
cp skillhub-icon.svg "$ICONS/scalable/apps/skillhub.svg"
python3 -I ../tools/make-icns.py
for s in 16 24 32 48 64 128 256 512; do
  mkdir -p "$ICONS/${s}x${s}/apps"; cp png/skillhub-$s.png "$ICONS/${s}x${s}/apps/skillhub.png"
done
CHROME=$(command -v google-chrome-stable || command -v chromium || command -v chrome)
"$CHROME" --headless=new --no-sandbox --disable-gpu --hide-scrollbars --window-size=1280,640 \
  --allow-file-access-from-files --screenshot="$PWD/banner.png" "file://$PWD/banner.html" >/dev/null 2>&1
echo done
