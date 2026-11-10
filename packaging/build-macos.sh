#!/usr/bin/env bash
# Build The Bigs 2 on a Mac (experimental; tested on Apple Silicon). Run this ON the Mac, from a copy of this project
# folder that has game/default.xex (the build recompiles it; to run the dev build, extract the whole disc into game/).
#
#   packaging/build-macos.sh          # SDK -> ~/rexsdk-install, game -> out/build/mac-release/thebigs2, release zip
#   JOBS=4 packaging/build-macos.sh   # fewer parallel compiles (default: one per 4 GB of RAM)
#
# Needs: Xcode command line tools (xcode-select --install), Homebrew cmake + ninja + git + gh (gh auth login: the
# launcher app is downloaded from this repo's GitHub Actions). Downloads the ReXGlue SDK fork with the online-play
# changes (github.com/MatRanc/rexglue-sdk, branch tb2-online; ~550 MB with submodules).
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SDK_SRC="${SDK_SRC:-$HOME/rexglue-sdk}"
SDK_INSTALL="${SDK_INSTALL:-$HOME/rexsdk-install}"
SDK_REPO="${SDK_REPO:-https://github.com/MatRanc/rexglue-sdk.git}"
SDK_BRANCH="${SDK_BRANCH:-tb2-online}"
case "$(uname -m)" in
  arm64) PRESET=mac-arm64 ;;
  x86_64) PRESET=mac-amd64 ;;
  *) echo "Unsupported Mac CPU: $(uname -m)" >&2; exit 1 ;;
esac

# Parallel compile jobs. The game's ~80 generated C++ files need ~3-4 GB of RAM each to compile, so one job per core
# can eat 60 GB+ and swap the Mac to a crawl: allow one job per 4 GB of RAM (override with JOBS=n).
RAM_GB=$(( $(sysctl -n hw.memsize) / 1073741824 ))
CORES=$(sysctl -n hw.ncpu)
JOBS="${JOBS:-$(( RAM_GB / 4 ))}"
[ "$JOBS" -lt 1 ] && JOBS=1
[ "$JOBS" -gt "$CORES" ] && JOBS=$CORES
echo "Building with $JOBS parallel jobs ($RAM_GB GB RAM, $CORES cores)"

xcrun --version >/dev/null 2>&1 || { echo "Install the Xcode command line tools: xcode-select --install" >&2; exit 1; }
for t in cmake ninja git; do
  command -v $t >/dev/null || { echo "Missing $t. With Homebrew: brew install cmake ninja git" >&2; exit 1; }
done
[ -f "$ROOT/game/default.xex" ] || { echo "Put the game's default.xex in $ROOT/game/ first." >&2; exit 1; }

# 1. The ReXGlue SDK fork (cloned once, then updated).
if [ ! -d "$SDK_SRC/.git" ]; then
  git clone --recursive -b "$SDK_BRANCH" "$SDK_REPO" "$SDK_SRC"
else
  git -C "$SDK_SRC" pull --ff-only && git -C "$SDK_SRC" submodule update --init --recursive
fi
cmake -S "$SDK_SRC" --preset "$PRESET"
cmake --build "$SDK_SRC/out/build/$PRESET" --config Release -j "$JOBS"
cmake --install "$SDK_SRC/out/build/$PRESET" --config Release --prefix "$SDK_INSTALL"

# 2. The game (the recompiler runs as part of the build).
BUILD="$ROOT/out/build/mac-release"
cmake -S "$ROOT" -B "$BUILD" -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH="$SDK_INSTALL" \
  -DCMAKE_OSX_ARCHITECTURES="$(uname -m)"
cmake --build "$BUILD" -j "$JOBS"

# 3. Release zip: the game folder (exe, dylibs, Vulkan/MoltenVK) + the launcher. No game data.
NAME="TheBigs2-macos-$(uname -m)"
STAGE="$ROOT/out/macos/$NAME"
rm -rf "$STAGE" && mkdir -p "$STAGE"
cp "$BUILD/thebigs2" "$BUILD"/*.dylib "$STAGE/"
[ -d "$BUILD/vulkan" ] && cp -R "$BUILD/vulkan" "$STAGE/"
[ -f "$BUILD/gamecontrollerdb.txt" ] && cp "$BUILD/gamecontrollerdb.txt" "$STAGE/"
# The stand-alone launcher app (GitHub Actions build): out/launcher/launcher-macos/, from fetch-launcher.sh (needs
# the gh CLI) or copied over from another machine.
LAUNCHER_ZIP="${LAUNCHER_ZIP:-$ROOT/out/launcher/launcher-macos/The Bigs 2 Launcher.app.zip}"
if [ ! -f "$LAUNCHER_ZIP" ] && command -v gh >/dev/null; then "$ROOT/packaging/fetch-launcher.sh" || true; fi
[ -f "$LAUNCHER_ZIP" ] || { echo "No launcher at $LAUNCHER_ZIP: run packaging/fetch-launcher.sh (gh CLI) first." >&2; exit 1; }
ditto -x -k "$LAUNCHER_ZIP" "$STAGE"
(cd "$ROOT/out/macos" && rm -f "$NAME.zip" && zip -qry "$NAME.zip" "$NAME")

echo
echo "Built $BUILD/thebigs2 and $ROOT/out/macos/$NAME.zip"
echo "Start the launcher with: python3 $ROOT/thebigs2_gui.py (dev) or open the .app in the zip"
