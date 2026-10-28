#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
set -euo pipefail
cd -- "$(dirname -- "$0")/.."
smithbox=${SMITHBOX_DIR:?Set SMITHBOX_DIR to your Smithbox checkout}
export SMITHBOX_OUTPUT="$smithbox/Smithbox.Release/Output"
export SOULS_IMPORT_DLL="$PWD/out/map-editor/souls-import/SoulsImport.dll"
export DOTNET_CLI_HOME="$PWD/out/map-editor/dotnet-home"
dotnet=${DOTNET:-/usr/local/share/dotnet/dotnet}
"$dotnet" build tests/souls_import/SkinTests.csproj -p:SmithboxOutput="$SMITHBOX_OUTPUT" --nologo
"$dotnet" out/map-editor/skin-test-bin/Debug/net10.0/SkinTests.dll
