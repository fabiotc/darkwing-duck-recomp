#!/usr/bin/env bash
# Universal (Apple Silicon + Intel) .app bundle, zipped.
source "$(dirname "$0")/common.sh"
B="$ROOT/build-release/macos"
cmake -S "$ROOT" -B "$B" -DCMAKE_BUILD_TYPE=Release -DDWD_BUNDLED_SDL=ON -DDWD_PACKAGE=ON \
  -DCMAKE_OSX_ARCHITECTURES="arm64;x86_64" -DCMAKE_OSX_DEPLOYMENT_TARGET=11.0 -DROM_PATH="$ROM"
cmake --build "$B" -j
APP="$B/Darkwing Duck Recomp.app"
codesign --force --deep -s - "$APP"   # ad-hoc signature (no Apple developer account)
STAGE="$B/stage" && rm -rf "$STAGE" && mkdir -p "$STAGE"
cp -R "$APP" "$STAGE/"
cp "$ROOT/packaging/README-player.txt" "$STAGE/README.txt"
(cd "$STAGE" && ditto -c -k --sequesterRsrc . "$DIST/$NAME-macos-universal.zip")
echo "-> $DIST/$NAME-macos-universal.zip"
