#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail
cd -- "$(dirname -- "$0")/.."
smithbox=${SMITHBOX_DIR:?Set SMITHBOX_DIR to your built Smithbox checkout}
export DOTNET_CLI_HOME="$PWD/out/map-editor/dotnet-home"
test_root="$PWD/out/map-editor/thorolund-quest-tests"
"${DOTNET:-/usr/local/share/dotnet/dotnet}" run --project tests/souls_import/MapThorolundQuestTests.csproj -p:UseSharedCompilation=false -p:SmithboxOutput="$smithbox/Smithbox.Release/Output" -p:BaseIntermediateOutputPath="$test_root/obj/" -p:MSBuildProjectExtensionsPath="$test_root/obj/" -p:BaseOutputPath="$test_root/bin/" -- "${1:?DS1R root}" "${2:?Bloodborne dvdroot_ps4}" "$smithbox/src/Smithbox.Data/Assets/PARAM/DS1R/Defs" "${3:-$test_root/profile}"
