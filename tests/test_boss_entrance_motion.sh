#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
source_root="${1:?Supply DS1 Remastered root}"
native_root="${2:?Supply native dvdroot_ps4}"
input_anibnd="${3:?Supply a Taurus anibnd exported before entrance adaptation}"
smithbox_output="${SMITHBOX_OUTPUT:-/Users/ronnystiftel/dev/smithbox/Smithbox.Release/Output}"
export DOTNET_CLI_HOME="$PWD/out/map-editor/dotnet-home"
dotnet_bin="${DOTNET:-/usr/local/share/dotnet/dotnet}"
"$dotnet_bin" run --project tests/souls_import/BossEntranceMotionTests.csproj \
  -p:UseSharedCompilation=false -p:SmithboxOutput="$smithbox_output" \
  -p:BaseIntermediateOutputPath="$PWD/out/map-editor/boss-entrance-test/obj/" \
  -p:BaseOutputPath="$PWD/out/map-editor/boss-entrance-test/bin/" \
  -- "$source_root" "$native_root" "$input_anibnd" out/map-editor/boss-encounters/entrance/fixture
