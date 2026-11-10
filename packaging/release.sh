#!/usr/bin/env bash
# Build the Linux and Windows release files with the stand-alone launcher and upload them to a GitHub release.
#   packaging/release.sh v1.1-online.2          # creates the release (pre-release) if it doesn't exist yet
# The macOS zip is built on a Mac (packaging/build-macos.sh) and uploaded with:
#   gh release upload <tag> out/macos/TheBigs2-macos-arm64.zip -R MatRanc/The-Bigs-2-Recomp-Online --clobber
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
REPO="${REPO:-MatRanc/The-Bigs-2-Recomp-Online}"
TAG="${1:?usage: packaging/release.sh <tag>}"
"$ROOT/packaging/fetch-launcher.sh"
"$ROOT/packaging/build-linux.sh"
"$ROOT/packaging/build-windows.sh"
FILES=("$ROOT/out/linux/TheBigs2-linux-x64.tar.gz" "$ROOT/out/windows/TheBigs2-windows-x64.zip")
if gh release view "$TAG" -R "$REPO" >/dev/null 2>&1; then
  gh release upload "$TAG" -R "$REPO" --clobber "${FILES[@]}"
else
  gh release create "$TAG" -R "$REPO" --target online --prerelease --title "The Bigs 2 Online $TAG" \
    --notes "Bring your own disc image (.iso); everything else is in the download for your system." "${FILES[@]}"
fi
