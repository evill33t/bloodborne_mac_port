#!/usr/bin/env bash
# SPDX-FileCopyrightText: Copyright 2026 Ronny Stiftel
# SPDX-License-Identifier: GPL-2.0-or-later
# Signs the bundle assembled by Mark Hall's package_app.sh, inside out.
# See docs/MACOS_RELEASE.md for original sources and Apple's signing requirements.
set -euo pipefail
app=${1:?Usage: bash scripts/macos/sign_app.sh /path/to/Bloodborne.app}
identity=${BB_SIGN_IDENTITY:--}
root=$(cd -- "$(dirname -- "$0")/../.." && pwd)
args=(--force --sign "$identity")
if [[ $identity != - ]]; then args+=(--options runtime --timestamp); fi
find "$app/Contents/MacOS" "$app/Contents/Frameworks" "$app/Contents/Resources/python" \
    -type f \( -perm -u+x -o -name '*.dylib' -o -name '*.so' \) -print0 |
    while IFS= read -r -d '' binary; do
        [[ $binary == "$app/Contents/MacOS/Bloodborne" ]] && continue
        file -b "$binary" | grep -q Mach-O || continue
        if [[ $identity != - && $binary == "$app/Contents/MacOS/bb-probe" ]]; then
            # The x86-64 guest and its generated trampolines execute from anonymous mappings.
            # Only the guest process needs this exception; other hosts retain the defaults.
            codesign "${args[@]}" --entitlements "$root/scripts/macos/guest-entitlements.plist" "$binary"
        else
            codesign "${args[@]}" "$binary"
        fi
    done
codesign "${args[@]}" "$app"
codesign --verify --deep --strict "$app"
if [[ $identity != - ]]; then
    codesign --display --verbose=4 "$app" 2>&1 | grep '^Authority=Developer ID Application:' >/dev/null || {
        echo 'Release signing requires a Developer ID Application certificate.' >&2; exit 1;
    }
fi
