#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
set -euo pipefail
cd -- "$(dirname -- "$0")/.."
smithbox=${SMITHBOX_DIR:?Set SMITHBOX_DIR to your built Smithbox checkout}
native_game=${1:?Pass the Bloodborne dvdroot_ps4}
export DOTNET_CLI_HOME="$PWD/out/map-editor/dotnet-home"
"${DOTNET:-/usr/local/share/dotnet/dotnet}" run --project tests/souls_import/MapTravelTests.csproj \
  -p:UseSharedCompilation=false -p:SmithboxOutput="$smithbox/Smithbox.Release/Output" \
  -p:BaseIntermediateOutputPath="$PWD/out/map-editor/map-travel-tests/obj/" \
  -p:BaseOutputPath="$PWD/out/map-editor/map-travel-tests/bin/" -- "$native_game" \
  "$smithbox/src/Smithbox.Data/Assets/PARAM/BB/Defs" "$smithbox/Smithbox.Release/Output" "${2:-}"
