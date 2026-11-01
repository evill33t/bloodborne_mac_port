#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail
cd -- "$(dirname -- "$0")/.."
smithbox=${SMITHBOX_DIR:?Set SMITHBOX_DIR}
source_game=${1:?Supply DS1R root}
native_game=${2:?Supply native dvdroot_ps4 root}
export DOTNET_CLI_HOME="$PWD/out/map-editor/dotnet-home"
test_root="$PWD/out/map-editor/merchant-talk-tests"
"${DOTNET:-/usr/local/share/dotnet/dotnet}" build tests/souls_import/MapMerchantTalkTests.csproj -p:UseSharedCompilation=false -p:SmithboxOutput="$smithbox/Smithbox.Release/Output" -p:BaseIntermediateOutputPath="$test_root/obj/" -p:MSBuildProjectExtensionsPath="$test_root/obj/" -p:BaseOutputPath="$test_root/bin/" --nologo
"${DOTNET:-/usr/local/share/dotnet/dotnet}" "$test_root/bin/Debug/net10.0/MapMerchantTalkTests.dll" "$source_game" "$native_game" "$smithbox/src/Smithbox.Data/Assets/PARAM/DS1R/Defs"
