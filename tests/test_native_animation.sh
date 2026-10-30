#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail
cd -- "$(dirname -- "$0")/.."
smithbox=${SMITHBOX_DIR:?Set SMITHBOX_DIR to your Smithbox checkout}
source_game=${1:?Supply the DS1 Remastered source directory}
native_game=${2:?Supply the Bloodborne dvdroot_ps4 directory}
export DOTNET_CLI_HOME="$PWD/out/map-editor/dotnet-home"
dotnet=${DOTNET:-/usr/local/share/dotnet/dotnet}
test_root="$PWD/out/map-editor/native-animation-tests"
"$dotnet" build tests/souls_import/NativeAnimationTests.csproj -p:SmithboxOutput="$smithbox/Smithbox.Release/Output" -p:BaseIntermediateOutputPath="$test_root/obj/" -p:MSBuildProjectExtensionsPath="$test_root/obj/" -p:BaseOutputPath="$test_root/bin/" -p:UseSharedCompilation=false --nologo
"$dotnet" "$test_root/bin/Debug/net10.0/NativeAnimationTests.dll" "$source_game" "$native_game"
