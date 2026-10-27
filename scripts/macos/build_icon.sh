#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
# Convert the selected artwork into a macOS icon without changing its composition.
# Bundled fallback: Supermedo/bloodborne_pc, launcher/bloodborne.ico (nick24051998).
# Exact source and hashes: assets/branding/README.md.
set -euo pipefail
source_icon=$1
destination=$2
temporary=$(mktemp -d)
trap 'rm -rf "$temporary"' EXIT
iconset=$temporary/Bloodborne.iconset
mkdir -p "$iconset"
sips -s format png "$source_icon" --out "$temporary/source.png" >/dev/null
for size in 16 32 128 256 512; do
    sips -z "$size" "$size" "$temporary/source.png" --out "$iconset/icon_${size}x${size}.png" >/dev/null
    double=$((size * 2))
    sips -z "$double" "$double" "$temporary/source.png" --out "$iconset/icon_${size}x${size}@2x.png" >/dev/null
done
iconutil -c icns "$iconset" -o "$destination"
