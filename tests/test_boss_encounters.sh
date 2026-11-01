#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
set -euo pipefail
cd -- "$(dirname -- "$0")/.."
smithbox=${SMITHBOX_DIR:?Set SMITHBOX_DIR to your built Smithbox checkout}
source_game=${1:?Pass the Dark Souls Remastered root}
native_game=${2:?Pass the Bloodborne dvdroot_ps4}
baseline_mod=${3:?Pass the connected Lordran mod before boss encounters}
export DOTNET_CLI_HOME="$PWD/out/map-editor/dotnet-home"
"${DOTNET:-/usr/local/share/dotnet/dotnet}" run --project tests/souls_import/MapBossEncountersTests.csproj \
  -p:UseSharedCompilation=false -p:SmithboxOutput="$smithbox/Smithbox.Release/Output" \
  -p:BaseIntermediateOutputPath="$PWD/out/map-editor/boss-encounter-tests/obj/" \
  -p:BaseOutputPath="$PWD/out/map-editor/boss-encounter-tests/bin/" -- \
  "$source_game" "$native_game" "$baseline_mod" "$smithbox/Smithbox.Release/Output/Assets/PARAM/DS1R/Defs"
