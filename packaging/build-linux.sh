#!/usr/bin/env bash
# Linux release: the AppImages + the launcher in one archive.
#
#   packaging/build-linux.sh      # -> out/linux/TheBigs2-linux-x64.tar.gz
#
# Builds TheBigs2-x86_64.AppImage and the Steam Deck AppImage from the Linux release build (see build-appimage.sh)
# and puts them next to the launcher (thebigs2_gui.py, needs Python 3 with Tk). No game data: the player brings the ISO.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT_DIR="${OUT_DIR:-$ROOT/out/linux}"
NAME=TheBigs2-linux-x64
STAGE="$OUT_DIR/$NAME"

"$ROOT/packaging/build-appimage.sh"
PROFILE=steamdeck "$ROOT/packaging/build-appimage.sh"

rm -rf "$STAGE"
mkdir -p "$STAGE/vendor" "$STAGE/assets"
cp "$ROOT/out/appimage/TheBigs2-x86_64.AppImage" "$ROOT/out/appimage/TheBigs2-SteamDeck-x86_64.AppImage" "$STAGE/"
cp "$ROOT/thebigs2_gui.py" "$STAGE/"
cp -r "$ROOT/vendor/sv_ttk" "$STAGE/vendor/" && rm -rf "$STAGE/vendor/sv_ttk/__pycache__"
cp -r "$ROOT/assets/theme-icons" "$ROOT/assets/bigs2-logo-128.png" "$ROOT/assets/bigs2-logo.png" "$STAGE/assets/"
chmod +x "$STAGE"/*.AppImage "$STAGE/thebigs2_gui.py"

rm -f "$OUT_DIR/$NAME.tar.gz"
tar -C "$OUT_DIR" -czf "$OUT_DIR/$NAME.tar.gz" "$NAME"
echo
echo "Built $OUT_DIR/$NAME.tar.gz"
