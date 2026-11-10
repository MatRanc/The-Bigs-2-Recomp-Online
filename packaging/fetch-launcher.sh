#!/usr/bin/env bash
# Download the stand-alone launcher apps (built by .github/workflows/build-launcher.yml) into out/launcher/.
#   packaging/fetch-launcher.sh [run-id]     # default: the latest successful run on the online branch
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
REPO="${REPO:-MatRanc/The-Bigs-2-Recomp-Online}"
RUN="${1:-$(gh run list -R "$REPO" --workflow build-launcher.yml --branch online --status success --limit 1 \
  --json databaseId -q '.[0].databaseId')}"
[ -n "$RUN" ] || { echo "No successful launcher build found; run the 'Build launcher' workflow first." >&2; exit 1; }
rm -rf "$ROOT/out/launcher" && mkdir -p "$ROOT/out/launcher"
gh run download "$RUN" -R "$REPO" -D "$ROOT/out/launcher"
chmod +x "$ROOT/out/launcher/launcher-linux/The Bigs 2 Launcher"
echo "Launchers from run $RUN in $ROOT/out/launcher"
