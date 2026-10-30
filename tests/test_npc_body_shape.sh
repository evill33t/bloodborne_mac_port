#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
set -euo pipefail
cd -- "$(dirname -- "$0")/.."
smithbox=${SMITHBOX_DIR:?Set SMITHBOX_DIR to your built Smithbox checkout}
source_game=${1:?Pass the Dark Souls Remastered root}
export DOTNET_CLI_HOME="$PWD/out/map-editor/dotnet-home"
"${DOTNET:-/usr/local/share/dotnet/dotnet}" run --project tests/souls_import/NpcBodyShapeTests.csproj \
  -p:UseSharedCompilation=false -p:SmithboxOutput="$smithbox/Smithbox.Release/Output" \
  -p:BaseIntermediateOutputPath="$PWD/out/map-editor/npc-body-shape-tests/obj/" \
  -p:BaseOutputPath="$PWD/out/map-editor/npc-body-shape-tests/bin/" -- "$source_game" "$smithbox/src/Smithbox.Data/Assets/PARAM/DS1R/Defs"
