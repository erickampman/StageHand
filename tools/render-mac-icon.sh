#!/bin/sh
# Renders an Icon Composer document (.icon) to 1024 px PNGs, for making an
# icon set (.appiconset or .iconset) by hand:
#   NAME-macOS-1024.png       the icon as macOS draws it, filling the canvas
#   NAME-macOS-1024-grid.png  the same in Apple's 824 px grid with its margin,
#                             the size an icon set expects
# Usage: tools/render-mac-icon.sh path/to/NAME.icon [output folder]
# (default: the folder the .icon is in). Needs Icon Composer (its ictool)
# and ImageMagick (magick). The app's own build doesn't need this:
# it compiles resources/StageHand.icon with actool (CMakeLists.txt).
set -eu

icon=${1:?usage: $0 path/to/NAME.icon [output folder]}
icon=${icon%/}
out=${2:-$(dirname "$icon")}
name=$(basename "$icon" .icon)

# Icon Composer's ictool: Xcode's own (xcrun --find ictool) shares the name
# but is actool's front end, with no --export-image.
ictool="/Applications/Icon Composer.app/Contents/Executables/ictool"
[ -x "$ictool" ] || ictool="$(xcode-select -p)/../Applications/Icon Composer.app/Contents/Executables/ictool"
[ -x "$ictool" ] || { echo "Icon Composer's ictool not found: install Icon Composer (it comes with Xcode)" >&2; exit 1; }
command -v magick >/dev/null || { echo "magick not found: brew install imagemagick" >&2; exit 1; }

mkdir -p "$out"
full="$out/$name-macOS-1024.png"
grid="$out/$name-macOS-1024-grid.png"
"$ictool" "$icon" --export-image --output-file "$full" \
    --platform macOS --rendition Default --width 1024 --height 1024 --scale 1 >/dev/null
magick "$full" -resize 824x824 -background none -gravity center -extent 1024x1024 "PNG32:$grid"
echo "$full"
echo "$grid"
