#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
set -euo pipefail
cd -- "$(dirname -- "$0")/.."
bash scripts/macos/build_souls_import.sh
"${DOTNET:-/usr/local/share/dotnet/dotnet}" out/map-editor/souls-import/SoulsImport.dll self-test
bash tests/test_source_scene.sh "$@"
