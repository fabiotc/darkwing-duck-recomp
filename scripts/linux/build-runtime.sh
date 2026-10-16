#!/usr/bin/env bash
# Builds the AppImage runtime (AppImage/type2-runtime, MIT) with our patch that
# falls back to extract-and-run when FUSE can't be used, e.g. when Steam starts
# the game in Game Mode. Runs on the host; the build itself happens in the
# project's own Docker container. The result is cached.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
COMMIT=8f39b89
OUT="$ROOT/build-release/runtime/runtime-x86_64-$COMMIT-patched"
if [ -f "$OUT" ]; then
  echo "runtime: using cached $OUT"
  exit 0
fi
SRC="$ROOT/build-release/runtime/type2-runtime"
rm -rf "$SRC"
git clone -q https://github.com/AppImage/type2-runtime.git "$SRC"
git -C "$SRC" checkout -q "$COMMIT"
git -C "$SRC" apply "$ROOT/packaging/linux/runtime-fuse-fallback.patch"
(cd "$SRC" && env ARCH=x86_64 bash scripts/docker/build-with-docker.sh)
cp "$SRC/runtime-x86_64" "$OUT"
echo "runtime: built $OUT"
