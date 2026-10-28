#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
set -euo pipefail
cd -- "$(dirname -- "$0")/../.."
smithbox=${SMITHBOX_DIR:?Set SMITHBOX_DIR to your Smithbox source checkout with a built release}
output=$PWD/out/map-editor/souls-import
library="$smithbox/Smithbox.Release/Output"
dotnet=${DOTNET:-$(command -v dotnet || true)}
[[ -n "$dotnet" ]] || dotnet=/usr/local/share/dotnet/dotnet
[[ -f "$library/Andre.SoulsFormats.dll" ]] || { echo "Build Smithbox first: $library/Andre.SoulsFormats.dll is missing" >&2; exit 1; }
[[ -f "$smithbox/src/Andre/Andre.Formats/Util/ArchiveKeys.cs" ]] || { echo "This adapter requires the Smithbox source checkout, including ArchiveKeys.cs" >&2; exit 1; }
export DOTNET_CLI_TELEMETRY_OPTOUT=1
export DOTNET_CLI_HOME="$PWD/out/map-editor/dotnet-home"
"$dotnet" publish editor/souls_import/SoulsImport.csproj -p:SmithboxOutput="$library" -p:SmithboxRoot="$smithbox" -o "$output" --nologo
# Copy runtime dependencies without requiring NuGet or bundling any game assets.
find "$library" -maxdepth 1 -name '*.dll' -exec cp -f {} "$output/" \;
if [[ -f "$smithbox/LICENSE" ]]; then cp "$smithbox/LICENSE" "$output/Smithbox-LICENSE"; fi
printf '%s\n' "$smithbox" > "$output/smithbox-path.txt"
echo "Built $output/SoulsImport.dll"
