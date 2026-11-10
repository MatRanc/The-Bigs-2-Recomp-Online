#!/usr/bin/env bash
# Linux release: the AppImages + the launcher in one archive.
#
#   packaging/build-linux.sh      # -> out/linux/TheBigs2-linux-x64.tar.gz
#
# Builds TheBigs2-x86_64.AppImage and the Steam Deck AppImage from the Linux release build (see build-appimage.sh)
# and puts them next to the stand-alone launcher (from packaging/fetch-launcher.sh). No game data: the player brings
# the ISO.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT_DIR="${OUT_DIR:-$ROOT/out/linux}"
NAME=TheBigs2-linux-x64
STAGE="$OUT_DIR/$NAME"

"$ROOT/packaging/build-appimage.sh"
PROFILE=steamdeck "$ROOT/packaging/build-appimage.sh"

LAUNCHER="$ROOT/out/launcher/launcher-linux/The Bigs 2 Launcher"
[ -f "$LAUNCHER" ] || { echo "No launcher at $LAUNCHER: run packaging/fetch-launcher.sh first." >&2; exit 1; }
rm -rf "$STAGE"
mkdir -p "$STAGE"
cp "$ROOT/out/appimage/TheBigs2-x86_64.AppImage" "$ROOT/out/appimage/TheBigs2-SteamDeck-x86_64.AppImage" "$STAGE/"
cp "$LAUNCHER" "$STAGE/"
chmod +x "$STAGE"/*

rm -f "$OUT_DIR/$NAME.tar.gz"
tar -C "$OUT_DIR" -czf "$OUT_DIR/$NAME.tar.gz" "$NAME"
echo
echo "Built $OUT_DIR/$NAME.tar.gz"
