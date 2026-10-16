#!/usr/bin/env bash
# x86_64 AppImage (Linux, Steam Deck, SteamOS), built in an Ubuntu 22.04
# container so it runs on any distro from 2022 onwards.
source "$(dirname "$0")/common.sh"
docker run --rm --platform linux/amd64 \
  -v "$ROOT":/src -v "$ROM":/rom.nes:ro -e NAME="$NAME" \
  ubuntu:22.04 bash /src/scripts/linux/build-appimage.sh
echo "-> $DIST/$NAME-linux-x86_64.AppImage"
