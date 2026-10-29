#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
set -euo pipefail
cd -- "$(dirname -- "$0")/.."
smithbox=${SMITHBOX_DIR:?Set SMITHBOX_DIR to your built Smithbox checkout}
source_game=${1:?Pass the Dark Souls Remastered root}
native_game=${2:?Pass the Bloodborne game root}
export DOTNET_CLI_HOME="$PWD/out/map-editor/dotnet-home"
"${DOTNET:-/usr/local/share/dotnet/dotnet}" run --project tests/souls_import/MapBreakablesTests.csproj \
  -p:UseSharedCompilation=false -p:SmithboxOutput="$smithbox/Smithbox.Release/Output" \
  -p:BaseIntermediateOutputPath="$PWD/out/map-editor/map-breakables-tests/obj/" \
  -p:BaseOutputPath="$PWD/out/map-editor/map-breakables-tests/bin/" -- "$source_game" "$native_game"
