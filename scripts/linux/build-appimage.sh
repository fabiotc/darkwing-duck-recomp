#!/usr/bin/env bash
# Runs inside the ubuntu:22.04 container started by scripts/package-linux.sh.
set -euo pipefail
export DEBIAN_FRONTEND=noninteractive
apt-get update -qq
# SDL3 loads X11/Wayland/audio libraries at runtime; only the headers are needed here.
apt-get install -y -qq --no-install-recommends \
  build-essential cmake git ca-certificates curl file python3.11 desktop-file-utils \
  libx11-dev libxext-dev libxrandr-dev libxcursor-dev libxi-dev libxss-dev libxfixes-dev \
  libxkbcommon-dev libwayland-dev wayland-protocols libdecor-0-dev \
  libegl1-mesa-dev libgl1-mesa-dev libgles2-mesa-dev libdrm-dev libgbm-dev \
  libpulse-dev libasound2-dev libpipewire-0.3-dev libdbus-1-dev libudev-dev >/dev/null

B=/src/build-release/linux
cmake -S /src -B "$B" -DCMAKE_BUILD_TYPE=Release -DDWD_BUNDLED_SDL=ON \
  -DROM_PATH=/rom.nes -DPython3_EXECUTABLE=/usr/bin/python3.11
cmake --build "$B" -j"$(nproc)"

APPDIR="$B/AppDir"
rm -rf "$APPDIR" && mkdir -p "$APPDIR/usr/bin"
cp "$B/darkwing-duck" "$APPDIR/usr/bin/"
strip "$APPDIR/usr/bin/darkwing-duck"
cp /src/packaging/linux/darkwing-duck.desktop "$APPDIR/"
cp /src/packaging/icon.png "$APPDIR/darkwing-duck.png"
cat > "$APPDIR/AppRun" <<'RUN'
#!/bin/sh
HERE="$(dirname "$(readlink -f "$0")")"
exec "$HERE/usr/bin/darkwing-duck" "$@"
RUN
chmod +x "$APPDIR/AppRun"

curl -fsSL -o /tmp/appimagetool \
  https://github.com/AppImage/appimagetool/releases/download/1.9.0/appimagetool-x86_64.AppImage
chmod +x /tmp/appimagetool
# AppImages carry a magic marker at byte 8 of the ELF header that breaks
# emulated (binfmt) execution inside Docker; blank it and run unpacked.
printf '\0\0\0' | dd of=/tmp/appimagetool bs=1 seek=8 count=3 conv=notrunc 2>/dev/null
(cd /tmp && ./appimagetool --appimage-extract >/dev/null)
mkdir -p /src/dist
ARCH=x86_64 /tmp/squashfs-root/AppRun --no-appstream "$APPDIR" "/src/dist/$NAME-linux-x86_64.AppImage"

# Plain archive too: the bare executable needs no FUSE/AppImage mounting,
# which can fail when Steam launches games in a restricted environment.
TAR="$B/tar/$NAME-linux"
rm -rf "$B/tar" && mkdir -p "$TAR"
cp "$APPDIR/usr/bin/darkwing-duck" "$TAR/"
cp /src/packaging/icon.png "$TAR/darkwing-duck.png"
cp /src/packaging/README-player.txt "$TAR/README.txt"
tar -C "$B/tar" -czf "/src/dist/$NAME-linux-x86_64.tar.gz" "$NAME-linux"
