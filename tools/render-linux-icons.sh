#!/bin/sh
# Renders the Linux app icons from the Icon Composer document's artwork
# (resources/StageHand.icon/Assets/StageHand.1024.bars.png): the square with
# rounded corners and a margin, as GNOME's app icons are, at the sizes in
# resources/linux/icons. The window's own icon (_NET_WM_ICON, src/Ui/WindowIcon.cpp)
# is the 128 px one, embedded; tools/install-desktop-entry.sh installs them all.
# Usage: tools/render-linux-icons.sh. Needs ImageMagick (magick).
set -eu

root=$(cd "$(dirname "$0")/.." && pwd)
art="$root/resources/StageHand.icon/Assets/StageHand.1024.bars.png"
out="$root/resources/linux/icons"
command -v magick >/dev/null || { echo "magick not found: sudo apt install imagemagick" >&2; exit 1; }

mkdir -p "$out"
# 1024 px: the artwork in 896 px with corners of 160 px, centred.
full=$(mktemp --suffix=.png)
trap 'rm -f "$full"' EXIT
magick "$art" -resize 896x896 -alpha off \
    \( -size 896x896 xc:black -fill white -draw "roundrectangle 0,0 895,895 160,160" \) \
    -compose CopyOpacity -composite -compose Over \
    -background none -gravity center -extent 1024x1024 "PNG32:$full"
for size in 48 64 128 256 512; do
    magick "$full" -filter Lanczos -resize "${size}x${size}" "PNG32:$out/stagehand-$size.png"
done
