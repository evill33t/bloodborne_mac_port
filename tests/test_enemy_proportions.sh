#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail
cd -- "$(dirname -- "$0")/.."
smithbox=${SMITHBOX_DIR:?Set SMITHBOX_DIR to your built Smithbox checkout}
export DOTNET_CLI_HOME="$PWD/out/map-editor/dotnet-home"
"${DOTNET:-/usr/local/share/dotnet/dotnet}" run --project tests/souls_import/EnemyProportionsTests.csproj -p:UseSharedCompilation=false -p:SmithboxOutput="$smithbox/Smithbox.Release/Output" -p:BaseIntermediateOutputPath="$PWD/out/map-editor/enemy-proportions-test/obj/" -p:BaseOutputPath="$PWD/out/map-editor/enemy-proportions-test/bin/" -- "${1:?DS1R root}" "${2:?Bloodborne dvdroot_ps4}" "$PWD/out/map-editor/corridor-fixes/enemies"
