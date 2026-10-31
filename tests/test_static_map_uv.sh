#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
set -euo pipefail
cd -- "$(dirname -- "$0")/.."
smithbox=${SMITHBOX_DIR:?Set SMITHBOX_DIR to your built Smithbox checkout}
native_game=${1:?Pass the Bloodborne dvdroot_ps4}
source_game=${2:?Pass the Dark Souls Remastered root}
exported_mod=${3:-out/map-editor/lordran-additional/Mods/Lordran}
export DOTNET_CLI_HOME="$PWD/out/map-editor/dotnet-home"
"${DOTNET:-/usr/local/share/dotnet/dotnet}" run --project tests/souls_import/StaticMapUVTests.csproj \
  -p:UseSharedCompilation=false -p:SmithboxOutput="$smithbox/Smithbox.Release/Output" \
  -p:BaseIntermediateOutputPath="$PWD/out/map-editor/static-map-uv-tests/obj/" \
  -p:BaseOutputPath="$PWD/out/map-editor/static-map-uv-tests/bin/" -- "$native_game" "$source_game" "$exported_mod" "$smithbox/Smithbox.Release/Output"
