#!/usr/bin/env bash
# Build the embedded engine (libHcEngine.so) and the app package for each architecture.
#
#   scripts/build.sh [Release|Debug] [arch ...]     default: Release armv7 aarch64
#
# Needs the Vega SDK (`source ~/vega/env`) and `npm ci` in vega/ and vega/hcengine/.
# Version and build number come from scripts/version.js (TAG env, or the git tag; git commit
# count). Packages end up in build/<arch>-<release|debug>/hamclock_<arch>.vpkg.
set -euo pipefail
cd "$(dirname "$0")/.."

BTYPE=${1:-Release}
shift || true
ARCHS=${*:-armv7 aarch64}
case "$BTYPE" in Release|Debug) ;; *) echo "build type must be Release or Debug" >&2; exit 2 ;; esac

read -r -a VERSION_ARGS <<< "$(node scripts/version.js)"
echo "build: $BTYPE, archs: $ARCHS, ${VERSION_ARGS[*]}"

(cd hcengine && npm run -s build:js)
for arch in $ARCHS; do
  echo "== engine $arch"
  (cd hcengine && vega build -t "$arch" -b "$BTYPE")
  echo "== app $arch"
  npx react-native build-vega --build-type "$BTYPE" --target "$arch" "${VERSION_ARGS[@]}"
done

lower=$(echo "$BTYPE" | tr '[:upper:]' '[:lower:]')
for arch in $ARCHS; do
  ls -l "build/$arch-$lower/hamclock_$arch.vpkg"
done
