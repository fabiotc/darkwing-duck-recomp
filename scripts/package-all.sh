#!/usr/bin/env bash
# Builds every release package into dist/ and writes SHA256SUMS.txt.
source "$(dirname "$0")/common.sh"
"$ROOT/scripts/package-macos.sh"
"$ROOT/scripts/package-windows.sh"
"$ROOT/scripts/package-linux.sh"
(cd "$DIST" && shasum -a 256 "$NAME"-* > SHA256SUMS.txt && cat SHA256SUMS.txt)
