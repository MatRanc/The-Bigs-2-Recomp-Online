#!/usr/bin/env bash
# Download the stand-alone launcher apps (built by .github/workflows/build-launcher.yml) into out/launcher/.
#   packaging/fetch-launcher.sh [run-id]      # default: the latest successful run on the online branch
#   packaging/fetch-launcher.sh --tag v1.2    # the run for that tag (its launcher carries the version)
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
REPO="${REPO:-MatRanc/The-Bigs-2-Recomp-Online}"
if [ "${1:-}" = "--tag" ]; then
  TAG="${2:?usage: packaging/fetch-launcher.sh --tag <tag>}"
  RUN="$(gh run list -R "$REPO" --workflow build-launcher.yml --branch "$TAG" --status success --limit 1 \
    --json databaseId -q '.[0].databaseId')"
  [ -n "$RUN" ] || { echo "No successful launcher build for $TAG; push the tag and wait for the workflow." >&2; exit 1; }
else
  RUN="${1:-$(gh run list -R "$REPO" --workflow build-launcher.yml --branch online --status success --limit 1 \
    --json databaseId -q '.[0].databaseId')}"
  [ -n "$RUN" ] || { echo "No successful launcher build found; run the 'Build launcher' workflow first." >&2; exit 1; }
fi
rm -rf "$ROOT/out/launcher" && mkdir -p "$ROOT/out/launcher"
gh run download "$RUN" -R "$REPO" -D "$ROOT/out/launcher"
chmod +x "$ROOT/out/launcher/launcher-linux/The Bigs 2 Launcher"
echo "Launchers from run $RUN in $ROOT/out/launcher"
