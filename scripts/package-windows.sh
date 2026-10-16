#!/usr/bin/env bash
# 64-bit Windows build, cross-compiled with mingw-w64 (brew install mingw-w64).
source "$(dirname "$0")/common.sh"
B="$ROOT/build-release/windows"
cmake -S "$ROOT" -B "$B" -DCMAKE_BUILD_TYPE=Release -DDWD_BUNDLED_SDL=ON \
  -DCMAKE_TOOLCHAIN_FILE="$ROOT/cmake/mingw-w64-x86_64.cmake" -DROM_PATH="$ROM"
cmake --build "$B" -j
STAGE="$B/stage/$NAME-windows" && rm -rf "$B/stage" && mkdir -p "$STAGE"
cp "$B/darkwing-duck.exe" "$STAGE/DarkwingDuck.exe"
x86_64-w64-mingw32-strip "$STAGE/DarkwingDuck.exe"
sed 's/$/\r/' "$ROOT/packaging/README-player.txt" > "$STAGE/README.txt"
rm -f "$DIST/$NAME-windows-x64.zip"
(cd "$B/stage" && zip -qr "$DIST/$NAME-windows-x64.zip" "$NAME-windows")
echo "-> $DIST/$NAME-windows-x64.zip"
