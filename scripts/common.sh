# Shared settings for the packaging scripts.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
ROM="${ROM:-$ROOT/rom/darkwing-duck.nes}"
VERSION="$(sed -n 's/^project(.*VERSION \([0-9.]*\).*/\1/p' "$ROOT/CMakeLists.txt")"
DIST="$ROOT/dist"
NAME="DarkwingDuckRecomp-$VERSION"
mkdir -p "$DIST"
if [ ! -f "$ROM" ]; then
  echo "error: ROM not found at $ROM (set ROM=/path/to/rom.nes)" >&2
  exit 1
fi
