#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail
cd -- "$(dirname -- "$0")/.."
smithbox=${SMITHBOX_DIR:?Set SMITHBOX_DIR to your built Smithbox checkout}
source_game=${1:?Pass the Dark Souls Remastered root}
native_game=${2:?Pass the Bloodborne dvdroot_ps4}
export DOTNET_CLI_HOME="$PWD/out/map-editor/dotnet-home"
dotnet=${DOTNET:-/usr/local/share/dotnet/dotnet}
test_root="$PWD/out/map-editor/map-character-animation-tests"
"$dotnet" build tests/souls_import/MapCharacterAnimationsTests.csproj -p:UseSharedCompilation=false \
  -p:SmithboxOutput="$smithbox/Smithbox.Release/Output" -p:BaseIntermediateOutputPath="$test_root/obj/" \
  -p:MSBuildProjectExtensionsPath="$test_root/obj/" -p:BaseOutputPath="$test_root/bin/" --nologo
"$dotnet" "$test_root/bin/Debug/net10.0/MapCharacterAnimationsTests.dll" "$source_game" "$native_game" "$smithbox/src/Smithbox.Data/Assets/PARAM/DS1R/Defs"
