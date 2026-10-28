#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
# Standalone arm64 Metal editor; shares the in-game map's asset pipeline, without building HLE.
set -euo pipefail
cd -- "$(dirname -- "$0")/../.."
prefix=${BB_DEPS_DIR:-$PWD/deps/arm64}
imgui=$PWD/gpu/third_party/imgui
output=$PWD/out/map-editor
mkdir -p "$output/objects"
if [[ ! -f $imgui/imgui.cpp || ! -f $prefix/lib/libminiz.a ]]; then
    echo 'Initialize the ImGui submodule and run scripts/macos/bootstrap_deps.sh first.' >&2
    exit 1
fi
flags=(-arch arm64 -mmacosx-version-min=15.0 -std=c++20 -O2 -g -Wall -Wextra
    -Ieditor -Igpu/shim -isystem "$prefix/include/miniz" -I"$imgui" -I"$imgui/backends")
sources=(editor/map_document.cpp editor/game_params.cpp editor/event_document.cpp editor/map_scene.cpp editor/source_scene.cpp editor/collision_scene.cpp editor/map_editor.mm
    gpu/shim/bbport_map_art.cpp gpu/shim/bbport_map_collision.cpp gpu/shim/bbport_game_art.cpp
    "$imgui/imgui.cpp" "$imgui/imgui_draw.cpp" "$imgui/imgui_tables.cpp" "$imgui/imgui_widgets.cpp"
    "$imgui/backends/imgui_impl_metal.mm" "$imgui/backends/imgui_impl_osx.mm")
objects=()
for source in "${sources[@]}"; do
    object="$output/objects/$(basename "${source%.*}").o"
    if [[ $source == *.mm ]]; then
        "${CXX:-/usr/bin/clang++}" "${flags[@]}" -fobjc-arc -c "$source" -o "$object"
    else
        "${CXX:-/usr/bin/clang++}" "${flags[@]}" -c "$source" -o "$object"
    fi
    objects+=("$object")
done
"${CXX:-/usr/bin/clang++}" -arch arm64 -mmacosx-version-min=15.0 "${objects[@]}" "$prefix/lib/libminiz.a" \
    -framework AppKit -framework Metal -framework MetalKit -framework QuartzCore -framework GameController \
    -o "$output/bbmap-editor"
echo "Built $output/bbmap-editor"
app="$output/Bloodborne Map Editor.app"
mkdir -p "$app/Contents/MacOS"
cp "$output/bbmap-editor" "$app/Contents/MacOS/bbmap-editor"
cat > "$app/Contents/Info.plist" <<'PLIST'
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict>
<key>CFBundleExecutable</key><string>bbmap-editor</string>
<key>CFBundleIdentifier</key><string>org.bbport.map-editor</string>
<key>CFBundleName</key><string>Bloodborne Map Editor</string>
<key>CFBundlePackageType</key><string>APPL</string>
<key>CFBundleVersion</key><string>1</string>
<key>CFBundleShortVersionString</key><string>0.1</string>
<key>LSMinimumSystemVersion</key><string>15.0</string>
<key>NSHighResolutionCapable</key><true/>
<key>NSPrincipalClass</key><string>NSApplication</string>
</dict></plist>
PLIST
if [[ -f "$output/souls-import/SoulsImport.dll" ]]; then
    mkdir -p "$app/Contents/Resources"
    /usr/bin/ditto "$output/souls-import" "$app/Contents/Resources/souls-import"
fi
/usr/bin/codesign --force --sign - "$app"
echo "Built $app"
