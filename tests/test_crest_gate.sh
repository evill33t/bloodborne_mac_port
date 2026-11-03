#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail
cd -- "$(dirname -- "$0")/.."
smithbox=${SMITHBOX_DIR:?Set SMITHBOX_DIR to your built Smithbox checkout}
export DOTNET_CLI_HOME="$PWD/out/map-editor/dotnet-home"
# Produce actual staged Havel/Griggs/bell parameter rows for preservation checks.
bash tests/test_parish_bell.sh "${1:?DS1R root}" "${2:?BB dvdroot_ps4}"
test_root="$PWD/out/map-editor/crest-gate-tests"
"${DOTNET:-/usr/local/share/dotnet/dotnet}" run --project tests/souls_import/MapCrestGateTests.csproj -p:UseSharedCompilation=false -p:SmithboxOutput="$smithbox/Smithbox.Release/Output" -p:BaseIntermediateOutputPath="$test_root/obj/" -p:MSBuildProjectExtensionsPath="$test_root/obj/" -p:BaseOutputPath="$test_root/bin/" -- "$1" "$2"
