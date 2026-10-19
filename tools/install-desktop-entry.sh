#!/bin/sh
# Installs StageHand for the desktop, for this user: a launcher
# (~/.local/share/applications/stagehand.desktop) running this checkout's
# build/stagehand, and the icons (~/.local/share/icons/hicolor). GNOME's dock
# then shows StageHand's icon and can keep it in the dock: the launcher's
# StartupWMClass matches the windows' WM_CLASS (src/Ui/RackUi.cpp).
# Usage: tools/install-desktop-entry.sh [path to the stagehand executable]
# (default: build/stagehand in this checkout). Run it again after moving the checkout.
set -eu

root=$(cd "$(dirname "$0")/.." && pwd)
exe=$(realpath -m "${1:-$root/build/stagehand}")
[ -x "$exe" ] || echo "note: $exe doesn't exist yet (build it before launching)" >&2
data=${XDG_DATA_HOME:-$HOME/.local/share}

for size in 48 64 128 256 512; do
    install -Dm644 "$root/resources/linux/icons/stagehand-$size.png" \
        "$data/icons/hicolor/${size}x${size}/apps/stagehand.png"
done
mkdir -p "$data/applications"
# Exec quotes the path, so a checkout in a folder with spaces still runs.
sed "s|@EXEC@|\"$exe\"|" "$root/resources/linux/stagehand.desktop.in" > "$data/applications/stagehand.desktop"

command -v gtk-update-icon-cache >/dev/null && gtk-update-icon-cache -q -t "$data/icons/hicolor" || true
command -v update-desktop-database >/dev/null && update-desktop-database -q "$data/applications" || true
echo "Installed $data/applications/stagehand.desktop (runs $exe)"
