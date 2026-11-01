#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail
cd -- "$(dirname -- "$0")/.."
smithbox=${SMITHBOX_DIR:?Set SMITHBOX_DIR to your built Smithbox checkout}
export DOTNET_CLI_HOME="$PWD/out/map-editor/dotnet-home"
"${DOTNET:-/usr/local/share/dotnet/dotnet}" run --project tests/souls_import/HollowProjectilesTests.csproj -p:UseSharedCompilation=false -p:SmithboxOutput="$smithbox/Smithbox.Release/Output" -p:BaseIntermediateOutputPath="$PWD/out/map-editor/hollow-projectile-test/obj/" -p:BaseOutputPath="$PWD/out/map-editor/hollow-projectile-test/bin/" -- "${1:?Bloodborne dvdroot_ps4}" "$smithbox/src/Smithbox.Data/Assets/PARAM/BB/Defs" "$PWD/out/map-editor/enemy-fidelity/roles/projectile-test"
