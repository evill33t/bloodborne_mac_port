#!/usr/bin/env bash
# SPDX-FileCopyrightText: Copyright 2026 Mark Hall
# macOS (Apple Silicon) build: everything is x86-64 and runs under Rosetta 2, because the game's
# eboot.bin is x86-64 code executed directly. Dependencies come from deps/x86_64
# (scripts/macos/bootstrap_deps.sh); nothing from the arm64 Homebrew may be linked.
#
#   bash build_macos.sh            # libbbgpu.dylib, bb-probe, bbgpu-host (arm64 dependencies)
#   bash build_macos.sh --test     # also build and run the runtime tests
#   bash build_macos.sh --gpu-only # only the GPU library
#   BB_BUILD_TYPE=Release BB_LTO=ON bash build_macos.sh # -O3, LTO, native release hosts
#   CMAKE_GENERATOR='Unix Makefiles' bash build_macos.sh # reuse a Makefiles cache
set -euo pipefail
cd -- "$(dirname -- "$0")"
ROOT=$PWD
PREFIX=$ROOT/deps/x86_64
export MACOSX_DEPLOYMENT_TARGET=15.0
if [[ ! -f $PREFIX/.built-SDL ]]; then
    echo 'x86-64 dependencies missing: run bash scripts/macos/bootstrap_deps.sh' >&2; exit 1
fi
if [[ ! -f $PREFIX/.built-SPIRV-Cross ]]; then # the Metal renderer, in both libraries since M3
    echo 'x86-64 SPIRV-Cross missing: run bash scripts/macos/bootstrap_deps.sh again' >&2; exit 1
fi
for tool in cmake ninja glslang pkg-config; do
    command -v $tool >/dev/null || { echo "Missing $tool (brew install cmake ninja glslang pkgconf)" >&2; exit 1; }
done
mkdir -p out
CC=${CC:-/usr/bin/clang}
build_type=${BB_BUILD_TYPE:-RelWithDebInfo}
opt=(-O2 -g)
if [[ $build_type == Release ]]; then
    opt=(-O3 -DNDEBUG)
    [[ ${BB_LTO:-ON} == OFF ]] || opt+=(-flto)
fi
ARCH=(-arch x86_64 -mmacosx-version-min=$MACOSX_DEPLOYMENT_TARGET)
export PKG_CONFIG_LIBDIR=$PREFIX/lib/pkgconfig:$PREFIX/share/pkgconfig
unset PKG_CONFIG_PATH
read -r -a includes <<< "$(pkg-config --cflags sdl3)"
read -r -a libraries <<< "$(pkg-config --libs sdl3)"

# GPU library (shadPS4 video core + drivers): out/gpu/libbbgpu.dylib. PGO is GCC-specific
# (.gcda profiles) and stays off on macOS; BB_LTO=OFF disables link-time optimization.
if [[ ! -f gpu/third_party/fsr-vulkan/CMakeLists.txt || ! -f gpu/third_party/imgui/imgui.h ]]; then
    git submodule update --init --recursive
fi
# Boost is header-only: the arm64 Homebrew headers are fine (nothing is linked from there).
boost=$(brew --prefix boost 2>/dev/null || echo /opt/homebrew/opt/boost)
# Online module (gpu/bbnet; BB_NET=OFF skips it): its libraries come from deps/x86_64 too, and
# protoc from deps/host (arm64) when bootstrap_deps.sh built it.
protoc=; [[ -x $ROOT/deps/host/bin/protoc ]] && protoc=$ROOT/deps/host/bin/protoc
cmake -S gpu -B out/gpu -G "${CMAKE_GENERATOR:-Ninja}" -DCMAKE_BUILD_TYPE="$build_type" \
    -DCMAKE_C_COMPILER="$CC" -DCMAKE_CXX_COMPILER="${CXX:-/usr/bin/clang++}" \
    -DCMAKE_OSX_ARCHITECTURES=x86_64 -DCMAKE_OSX_DEPLOYMENT_TARGET=$MACOSX_DEPLOYMENT_TARGET \
    -DCMAKE_PREFIX_PATH="$PREFIX;$boost" -DCMAKE_IGNORE_PREFIX_PATH=/opt/homebrew \
    -DBoost_INCLUDE_DIR="$boost/include" \
    -DBB_PGO=off -DBB_LTO="${BB_LTO:-ON}" -DBB_PGO_DIR="$PWD/pgo" \
    -DBB_NET="${BB_NET:-ON}" -DBB_SEAMLESS="${BB_SEAMLESS:-ON}" ${protoc:+-DBB_PROTOC="$protoc"} -Wno-dev >/dev/null
echo "GPU library: LTO ${BB_LTO:-ON}"
if [[ ${BB_SEAMLESS:-ON} == ON ]]; then
    if ! cmake --build out/gpu --parallel "${BB_BUILD_JOBS:-6}" --target bbseamless > out/seamless-build.log 2>&1; then
        tail -35 out/seamless-build.log >&2
        echo 'Seamless client build failed (out/seamless-build.log).' >&2; exit 1
    fi
    echo "Seamless client: $PWD/out/gpu/libbbseamless.dylib"
else
    rm -f out/gpu/libbbseamless.dylib
fi
# A failed GPU build must stop here: an older libbbgpu.dylib would otherwise be used silently.
if ! cmake --build out/gpu --parallel "${BB_BUILD_JOBS:-6}" --target bbgpu > out/gpu-build.log 2>&1; then
    grep -v '^\[' out/gpu-build.log | tail -40 >&2
    echo 'GPU library build failed (full log: out/gpu-build.log)' >&2; exit 1
fi
# The online module, when CMake found its libraries (otherwise the port plays offline). A failed
# module build is a warning, not a stop: the offline game is whole without it, so a stale module
# is removed and the build goes on.
if cmake --build out/gpu --target help 2>/dev/null | grep -E '(^|[ /])bbnet([: ]|$)' >/dev/null; then # no -q: pipefail
    if cmake --build out/gpu --parallel "${BB_BUILD_JOBS:-6}" --target bbnet > out/gpu-net-build.log 2>&1; then
        echo "Online module: $PWD/out/gpu/libbbnet.dylib"
    else
        grep -v '^\[' out/gpu-net-build.log | tail -40 >&2
        rm -f out/gpu/libbbnet.dylib
        echo 'Online module: build failed (full log: out/gpu-net-build.log); the game plays offline without it' >&2
    fi
else
    # BB_NET=OFF or missing dependencies must not leave a previously built module loadable.
    rm -f out/gpu/libbbnet.dylib
    echo 'Online module: not built (disabled or libraries missing; run bash scripts/macos/bootstrap_deps.sh)'
fi
if [[ ${1:-} == --gpu-only ]]; then echo "Built $PWD/out/gpu/libbbgpu.dylib"; exit 0; fi

# @executable_path/gpu: packaged copies keep the library next to the binary without patching it.
# The dylib resolves runtime hooks from its executable through the flat namespace. Preserve
# those externally visible definitions when LTO cannot see their uses across that boundary.
gpu=(-Wl,-export_dynamic -Lout/gpu -lbbgpu -Wl,-rpath,@executable_path/gpu -Wl,-rpath,"$PWD/out/gpu"
    -Wl,-rpath,"$PREFIX/lib")
runtime=(src/runtime*.c)
# Third-party decoders: compiled once, without this project's -Werror policy.
atrac9=(third_party/LibAtrac9/C/src/*.c)
if [[ ! -f out/libatrac9.a || $(cat out/atrac9-build-flags 2>/dev/null || true) != "${opt[*]}" || -n $(find third_party/LibAtrac9/C/src -newer out/libatrac9.a -name '*.c') ]]; then
    rm -rf out/atrac9 && mkdir -p out/atrac9
    for source in "${atrac9[@]}"; do "$CC" "${ARCH[@]}" -std=c99 "${opt[@]}" -w -c "$source" -o "out/atrac9/$(basename "${source%.c}").o"; done
    ar rcs out/libatrac9.a out/atrac9/*.o
    printf '%s\n' "${opt[*]}" > out/atrac9-build-flags
fi
cflags=(-std=c11 "${opt[@]}" -Wall -Wextra -Werror -pthread "${ARCH[@]}")
# The timing hook runs beside the x86-64 game, including in split GPU mode.
"${CXX:-/usr/bin/clang++}" "${ARCH[@]}" -std=c++20 "${opt[@]}" -Wall -Wextra -Werror \
    -ffp-contract=off -fno-fast-math -c src/fps/timing.cpp -o out/fps-timing.o
"$CC" "${cflags[@]}" "${includes[@]}" -I. -Isrc src/probe.c "${runtime[@]}" src/gpu_process.c out/fps-timing.o out/libatrac9.a -lc++ -lm "${gpu[@]}" "${libraries[@]}" -framework CoreFoundation -o out/bb-probe
echo "Built $PWD/out/bb-probe"
# Native arm64 GPU process (docs/DESIGN_NATIVE_GPU_PROCESS.md): the GPU
# library built arm64 (scripts/macos/build_gpu_arm64.sh, needs deps/arm64 from
# BB_DEPS_ARCH=arm64 scripts/macos/bootstrap_deps.sh) linked into out/bbgpu-host.
ARM64_PREFIX=$ROOT/deps/arm64
if [[ -f $ARM64_PREFIX/lib/pkgconfig/sdl3.pc ]]; then
    bash scripts/macos/build_gpu_arm64.sh
    host_cc=(/usr/bin/clang -arch arm64 -mmacosx-version-min=$MACOSX_DEPLOYMENT_TARGET -std=c11 "${opt[@]}" -Wall -Wextra -Werror
        -I. host/main.c host/host_runtime.c -Wl,-export_dynamic -Lout/gpu-arm64 -lbbgpu -Wl,-rpath,"$ROOT/out/gpu-arm64"
        -Wl,-rpath,"$ARM64_PREFIX/lib" -framework CoreFoundation)
    "${host_cc[@]}" -o out/bbgpu-host
    # macOS 27: libmalloc takes addresses in the guest range before main(), so a second host
    # carries the range as a zero-fill segment the kernel maps at exec (host/guest_reservation.s,
    # host/main.c reserve_guest_range). bb-probe runs it on macOS 27 and later only: on 26 the
    # pinned segment breaks dyld's flat-namespace lookups into the executable (PR #7).
    "${host_cc[@]}" host/guest_reservation.s -Wl,-segaddr,__GUESTRSV,0x7000000000 -Wl,-segprot,__GUESTRSV,---,--- \
        -o out/bbgpu-host-macos27
    echo "Built $PWD/out/bbgpu-host and out/bbgpu-host-macos27 (arm64, renderer: out/gpu-arm64/libbbgpu.dylib)"
else
    echo "Skipped out/bbgpu-host: no arm64 dependencies (BB_DEPS_ARCH=arm64 bash scripts/macos/bootstrap_deps.sh)"
fi
if [[ ${1:-} == --test ]]; then
    # Test assertions remain active even when checking a Release build.
    cflags+=(-UNDEBUG)
    "$CC" "${cflags[@]}" "${includes[@]}" -I. -Isrc tests/test_macos_trophies.c "${runtime[@]}" \
        out/libatrac9.a -lm "${gpu[@]}" "${libraries[@]}" -o out/trophies-test
    out/trophies-test
    /usr/bin/clang -arch arm64 -arch x86_64 -std=c11 -O2 -Wall -Wextra -Werror -I. \
        tests/test_trophy_bridge.c -o out/trophy-bridge-test
    arch -arm64 out/trophy-bridge-test
    arch -x86_64 out/trophy-bridge-test
    python3 -m unittest discover -s tests -p test_trophies.py
    bash scripts/test_fps_timing.sh
    python3 tests/test_launcher_stop.py
    "$CC" "${cflags[@]}" "${includes[@]}" -I. -Isrc tests/test_pad.c src/runtime_bindings.c src/runtime_host.c "${libraries[@]}" -Wl,-rpath,"$PREFIX/lib" -o out/pad-test
    out/pad-test
    "$CC" "${cflags[@]}" "${includes[@]}" -I. -Isrc tests/test_runtime.c "${runtime[@]}" out/libatrac9.a -lm "${gpu[@]}" "${libraries[@]}" -o out/runtime-test
    out/runtime-test
    "$CC" "${cflags[@]}" -I. -Isrc tests/test_render_guard.c src/runtime_render_guard.c -o out/render-guard-test
    out/render-guard-test
    "$CC" "${cflags[@]}" -Isrc tests/test_file_mods.c -o out/file-mods-test
    out/file-mods-test
    "$CC" "${cflags[@]}" -DBB_SAVE_TXN_TESTING -Isrc tests/test_save_txn.c src/runtime_save_txn.c -o out/save-txn-test
    out/save-txn-test
    "$CC" "${cflags[@]}" "${includes[@]}" -I. -Isrc tests/test_sema.c "${runtime[@]}" out/libatrac9.a -lm "${gpu[@]}" "${libraries[@]}" -o out/sema-test
    out/sema-test
    "$CC" "${cflags[@]}" -I. -Isrc tests/test_content.c src/runtime_content.c -o out/content-test
    out/content-test
    # Online imports go to the online module when it is there (skips without it).
    "$CC" "${cflags[@]}" "${includes[@]}" -I. -Isrc tests/test_online.c "${runtime[@]}" out/libatrac9.a -lm "${gpu[@]}" "${libraries[@]}" -framework CoreFoundation -o out/online-test
    out/online-test
    "$CC" "${cflags[@]}" -I. -Isrc tests/test_coop_trophy.c src/runtime_coop_trophy.c src/runtime_trophy.c -o out/coop-trophy-test
    out/coop-trophy-test
    PYTHONPYCACHEPREFIX="$ROOT/out/pycache" python3 tests/test_seamless_launch.py
    /usr/bin/clang++ -std=c++23 "${ARCH[@]}" -Igpu/bbseamless/include -Igpu/bbseamless \
        tests/test_seamless_mac.cpp gpu/bbseamless/mac_epoll.cpp -o out/seamless-mac-test
    out/seamless-mac-test
    bash tests/test_map.sh arm64
    bash tests/test_map.sh x86_64
    for art in coop_art party_icons; do
        /usr/bin/clang++ -std=c++20 -O2 -I. -Igpu/shim -I"$ROOT/deps/arm64/include/miniz" \
            -DBB_COOP_ART_PATH="\"$ROOT/gpu/art/coop_art.bin\"" -DBB_PARTY_ICONS_PATH="\"$ROOT/gpu/art/party_icons.bin\"" \
            "tests/test_$art.cpp" "gpu/shim/bbport_$art.cpp" "$ROOT/deps/arm64/lib/libminiz.a" -o "out/$art-test"
        "out/$art-test"
    done
    if [[ -f out/gpu/libbbseamless.dylib ]]; then
        /usr/bin/clang++ -std=c++20 "${ARCH[@]}" -I. tests/test_seamless_module.cpp -o out/seamless-module-test
        out/seamless-module-test "$ROOT/out/gpu/libbbseamless.dylib" default
        out/seamless-module-test "$ROOT/out/gpu/libbbseamless.dylib" custom
    fi
    # The online module's P2P frames match shadPS4 v0.20.0's codec (skips without the module).
    if [[ -f out/gpu/libbbnet.dylib ]]; then
        cmake --build out/gpu --parallel "${BB_BUILD_JOBS:-6}" --target bbnet-codec-test bbnet-session-test > out/gpu-net-test-build.log 2>&1 || { tail -40 out/gpu-net-test-build.log >&2; exit 1; }
        out/gpu/bbnet-codec-test
        out/gpu/bbnet-session-test
    fi
    /usr/bin/clang++ -std=c++20 -O1 -Wall -Wextra -Igpu/shim tests/test_settings_keep.cpp \
        gpu/shim/bbport_settings.cpp -o out/settings-keep-test
    out/settings-keep-test
    /usr/bin/clang++ -std=c++20 -O1 -Wall -Wextra -Werror -pthread -Igpu/shim \
        tests/test_frame_rates.cpp -o out/frame-rates-test
    out/frame-rates-test
    /usr/bin/clang++ -std=c++20 -O1 -Wall -Wextra -Werror -Igpu/shim tests/test_mac_settings.cpp \
        gpu/shim/bbport_settings.cpp gpu/shim/bbport_mac_settings.cpp -o out/mac-settings-test
    out/mac-settings-test
    "${PYTHON:-python3}" -m unittest discover -s tests -p test_mac_menu_assets.py
    "${PYTHON:-python3}" -m unittest discover -s tests -p test_exit_menu_assets.py
    "$CC" "${cflags[@]}" -I. -Isrc tests/test_mac_menu.c -o out/mac-menu-test
    out/mac-menu-test
    "$CC" "${cflags[@]}" -I. -Isrc tests/test_exit_menu.c -o out/exit-menu-test
    out/exit-menu-test
    bash tests/test_launch_settings.sh
    bash scripts/macos/build_launcher.sh --test
    # Shared control block rings between bb-probe (x86-64) and bbgpu-host (arm64).
    /usr/bin/clang -arch x86_64 -arch arm64 -mmacosx-version-min=$MACOSX_DEPLOYMENT_TARGET -std=c11 -O2 -g \
        -Wall -Wextra -Werror -I. tests/test_bbgpu_rings.c -o out/bbgpu-rings-test
    arch -x86_64 out/bbgpu-rings-test
    # The GPU process split end to end (opens the host's window for a moment).
    if [[ -x out/bbgpu-host ]]; then
        "$CC" "${cflags[@]}" "${includes[@]}" -I. -Isrc tests/test_gpu_split.c "${runtime[@]}" src/gpu_process.c \
            out/libatrac9.a -lm "${gpu[@]}" "${libraries[@]}" -o out/gpu-split-test
        out/gpu-split-test
    fi
    # Portable SRT walker, on both architectures (arm64 when build_gpu_arm64.sh has run).
    for walker in "x86_64:out/gpu" "arm64:out/gpu-arm64"; do
        [[ -f ${walker#*:}/libbbgpu.dylib ]] || continue
        /usr/bin/clang++ -arch "${walker%%:*}" -mmacosx-version-min=$MACOSX_DEPLOYMENT_TARGET -std=c++20 -O2 -g \
            -Wall -Wextra -Werror tests/test_srt_walker.cpp -L"${walker#*:}" -lbbgpu \
            -Wl,-rpath,"$ROOT/${walker#*:}" -o "out/srt-walker-test-${walker%%:*}"
        "out/srt-walker-test-${walker%%:*}"
        # Vertex/tessellation descriptor reads use the same bounded guest-fault recovery.
        cmake --build "${walker#*:}" --target shader-userdata-test >/dev/null
        "${walker#*:}/shader-userdata-test"
    done
    # Metal-native M1: Metal::Object, Device ticks and limits, Metal::Encoder (arm64, no window);
    # again under Metal's validation layer.
    if [[ -f out/gpu-arm64/libbbmetal_core.a ]]; then
        /usr/bin/clang++ -arch arm64 -mmacosx-version-min=$MACOSX_DEPLOYMENT_TARGET -std=c++20 -fobjc-arc -O2 -g \
            -Wall -Wextra -Werror -Igpu/shim -Igpu/shadps4 -I"$ARM64_PREFIX/include" tests/test_mtl_encoder.mm gpu/shim/common/logging/log.cpp gpu/shim/common/assert.cpp out/gpu-arm64/libbbmetal_core.a -L"$ARM64_PREFIX/lib" -lfmt \
            -framework Metal -framework Foundation -o out/mtl-encoder-test
        glslang -V --quiet tests/data/spec_const.comp -o out/spec_const.spv  # the Specialize test's module
        out/mtl-encoder-test
        BB_ENCODER_STATS=1 out/mtl-encoder-test
        BB_ENCODER_STATS=1 MTL_DEBUG_LAYER=1 out/mtl-encoder-test
        BB_ENCODER_STATS=1 BB_METAL_BINDING_CACHE=0 out/mtl-encoder-test
        # Metal-native M3: the rasterizer's draw helper (core part), plain and under validation.
        /usr/bin/clang++ -arch arm64 -mmacosx-version-min=$MACOSX_DEPLOYMENT_TARGET -std=c++20 -fobjc-arc -O2 -g \
            -Wall -Wextra -Werror -Igpu/shim -Igpu/shadps4 -I"$ARM64_PREFIX/include" tests/test_mtl_draw.mm gpu/shim/common/logging/log.cpp gpu/shim/common/assert.cpp out/gpu-arm64/libbbmetal_core.a -L"$ARM64_PREFIX/lib" -lfmt \
            -framework Metal -framework Foundation -o out/mtl-draw-test
        out/mtl-draw-test
        MTL_DEBUG_LAYER=1 out/mtl-draw-test
        # Metal-native M4: the present helper (offscreen CAMetalLayer), plain and under validation.
        /usr/bin/clang++ -arch arm64 -mmacosx-version-min=$MACOSX_DEPLOYMENT_TARGET -std=c++20 -fobjc-arc -O2 -g \
            -Wall -Wextra -Werror -Igpu/shim -Igpu/shadps4 -I"$ARM64_PREFIX/include" tests/test_mtl_present.mm gpu/shim/common/logging/log.cpp gpu/shim/common/assert.cpp out/gpu-arm64/libbbmetal_core.a -L"$ARM64_PREFIX/lib" -lfmt \
            -framework Metal -framework QuartzCore -framework CoreGraphics -framework ImageIO -framework Foundation -o out/mtl-present-test
        out/mtl-present-test
        MTL_DEBUG_LAYER=1 out/mtl-present-test
        /usr/bin/clang++ -arch arm64 -mmacosx-version-min=$MACOSX_DEPLOYMENT_TARGET -std=c++20 -fobjc-arc -O2 -g \
            -Wall -Wextra -Werror -Igpu/shim -Igpu/shadps4 -I"$ARM64_PREFIX/include" tests/test_mtl_hdr.mm gpu/shim/common/logging/log.cpp gpu/shim/common/assert.cpp out/gpu-arm64/libbbmetal_core.a -L"$ARM64_PREFIX/lib" -lfmt \
            -framework Metal -framework AppKit -framework QuartzCore -framework ImageIO -framework Foundation -o out/mtl-hdr-test
        out/mtl-hdr-test
        MTL_DEBUG_LAYER=1 out/mtl-hdr-test
        cmake --build out/gpu-arm64 --target native-hdr-shader-test >/dev/null
        out/gpu-arm64/native-hdr-shader-test
        MTL_DEBUG_LAYER=1 out/gpu-arm64/native-hdr-shader-test
        # Native screen-space AO/GI: production trace/blend readbacks, no game assets.
        cmake --build out/gpu-arm64 --target screen-lighting-test >/dev/null
        out/gpu-arm64/screen-lighting-test
        MTL_DEBUG_LAYER=1 out/gpu-arm64/screen-lighting-test
        # Metal-native M5: the scale helper (scaled colour copies, depth resample and clear).
        /usr/bin/clang++ -arch arm64 -mmacosx-version-min=$MACOSX_DEPLOYMENT_TARGET -std=c++20 -fobjc-arc -O2 -g \
            -Wall -Wextra -Werror -Igpu/shim -Igpu/shadps4 -I"$ARM64_PREFIX/include" tests/test_mtl_scale.mm gpu/shim/common/logging/log.cpp gpu/shim/common/assert.cpp out/gpu-arm64/libbbmetal_core.a -L"$ARM64_PREFIX/lib" -lfmt \
            -framework Metal -framework Foundation -o out/mtl-scale-test
        out/mtl-scale-test
        MTL_DEBUG_LAYER=1 out/mtl-scale-test
        # Metal-native M5: MetalFX on native textures (the upscaler's formats, one frame encoded
        # into an Encoder's command buffer), plain and under validation (MetalFX's own included).
        /usr/bin/clang++ -arch arm64 -mmacosx-version-min=$MACOSX_DEPLOYMENT_TARGET -std=c++20 -fobjc-arc -O2 -g \
            -Wall -Wextra -Werror -Igpu/shim -Igpu/shadps4 -I"$ARM64_PREFIX/include" tests/test_metalfx.mm gpu/shim/bbport_metalfx.mm gpu/shim/common/logging/log.cpp gpu/shim/common/assert.cpp out/gpu-arm64/libbbmetal_core.a -L"$ARM64_PREFIX/lib" -lfmt \
            -framework Metal -framework MetalFX -framework Foundation -o out/metalfx-test
        out/metalfx-test
        MTL_DEBUG_LAYER=1 out/metalfx-test
        # Experimental MetalFX frame interpolation: snapshots, history resets, HUD and pool bounds.
        cmake --build out/gpu-arm64 --target framegen-test >/dev/null
        out/gpu-arm64/framegen-test
        MTL_DEBUG_LAYER=1 out/gpu-arm64/framegen-test
    fi
    # Metal-native M4: what an MTLBinaryArchive can serialize (pure Metal), plain and under
    # validation.
    /usr/bin/clang++ -arch arm64 -mmacosx-version-min=$MACOSX_DEPLOYMENT_TARGET -std=c++20 -fobjc-arc -O2 -g \
        -Wall -Wextra -Werror tests/test_mtl_archive.mm -framework Metal -framework Foundation -o out/mtl-archive-test
    out/mtl-archive-test
    MTL_DEBUG_LAYER=1 out/mtl-archive-test
    # Metal-native M3: scene targets (scaled copies, depth resample, stencil export) through the
    # linked arm64 library; again writing stencil one bit at a time.
    if [[ -f out/gpu-arm64/libbbgpu.dylib ]]; then
        cmake --build out/gpu-arm64 --parallel "${BB_BUILD_JOBS:-6}" --target scene-resolution-test >/dev/null
        out/gpu-arm64/scene-resolution-test 2>&1 | grep -E 'PASS|FAIL'
        BB_SCENE_STENCIL_BITS=1 out/gpu-arm64/scene-resolution-test 2>&1 | grep -E 'PASS|FAIL'
        # Metal-native M5: host kernels with combined image samplers, plain and under validation.
        cmake --build out/gpu-arm64 --parallel "${BB_BUILD_JOBS:-6}" --target host-kernel-test >/dev/null
        out/gpu-arm64/host-kernel-test
        MTL_DEBUG_LAYER=1 out/gpu-arm64/host-kernel-test 2>&1 | grep -E 'PASS|FAIL' || { echo 'FAIL: host-kernel-test (validation)'; exit 1; }
        # Metal-native M6: the presenter's FSR 1 pass (EASU + RCAS), plain and under validation.
        cmake --build out/gpu-arm64 --parallel "${BB_BUILD_JOBS:-6}" --target fsr-pass-test >/dev/null
        out/gpu-arm64/fsr-pass-test 2>&1 | grep -E 'PASS|FAIL' || { echo 'FAIL: fsr-pass-test'; exit 1; }
        MTL_DEBUG_LAYER=1 out/gpu-arm64/fsr-pass-test 2>&1 | grep -E 'PASS|FAIL' || { echo 'FAIL: fsr-pass-test (validation)'; exit 1; }
        # Metal-native M6: every FSR 3.1 blob compiles to a Metal compute pipeline through the
        # FidelityFX Metal back end, and FSR 3 on it matches the Vulkan path's golden outputs
        # (tests/data/fsr3), plain and under validation.
        cmake --build out/gpu-arm64 --parallel "${BB_BUILD_JOBS:-6}" --target ffx-metal-test >/dev/null
        out/gpu-arm64/ffx-metal-test || { echo 'FAIL: ffx-metal-test'; exit 1; }
        MTL_DEBUG_LAYER=1 out/gpu-arm64/ffx-metal-test 2>&1 | grep -E '^(PASS|FAIL)'
        [[ ${PIPESTATUS[0]} == 0 ]] || { echo 'FAIL: ffx-metal-test (validation)'; exit 1; }
        # Metal-native M5: the settings overlay (ImGui's Metal backend) into an offscreen BGRA8
        # target, plain and under validation.
        cmake --build out/gpu-arm64 --parallel "${BB_BUILD_JOBS:-6}" --target overlay-test >/dev/null
        out/gpu-arm64/overlay-test 2>&1 | grep -E '^(PASS|FAIL)'
        [[ ${PIPESTATUS[0]} == 0 ]] || { echo 'FAIL: overlay-test'; exit 1; }
        MTL_DEBUG_LAYER=1 out/gpu-arm64/overlay-test 2>&1 | grep -E '^(PASS|FAIL)'
        [[ ${PIPESTATUS[0]} == 0 ]] || { echo 'FAIL: overlay-test (validation)'; exit 1; }
        cmake --build out/gpu-arm64 --parallel "${BB_BUILD_JOBS:-6}" --target coop-presentation-test >/dev/null
        MTL_DEBUG_LAYER=1 out/gpu-arm64/coop-presentation-test
        # Saved upscaler choices (unavailable providers run as a fallback, the ini keeps the
        # user's value).
        cmake --build out/gpu-arm64 --parallel "${BB_BUILD_JOBS:-6}" --target native-motion-test >/dev/null
        out/gpu-arm64/native-motion-test
        cmake --build out/gpu-arm64 --parallel "${BB_BUILD_JOBS:-6}" --target upscaler-support-test >/dev/null
        out/gpu-arm64/upscaler-support-test 2>&1 | grep -E 'PASS|FAIL' || { echo 'FAIL: upscaler-support-test'; exit 1; }
        # Metal-native M5: the motion shader variants compile to MSL; object motion on Metal
        # (history buffers by GPU address, the motion attachment), plain and under validation.
        cmake --build out/gpu-arm64 --parallel "${BB_BUILD_JOBS:-6}" --target motion-shader-test >/dev/null
        out/gpu-arm64/motion-shader-test 2>&1 | grep -E '^(FAIL|motion-shader-test)' || { echo 'FAIL: motion-shader-test'; exit 1; }
        MTL_DEBUG_LAYER=1 out/gpu-arm64/motion-shader-test 2>&1 | grep -E '^(FAIL|motion-shader-test)' || { echo 'FAIL: motion-shader-test (validation)'; exit 1; }
        # Metal-native M5: the camera motion kernel (production SPIR-V through BuildHostKernel and
        # CameraMotion::Record), plain and under validation.
        cmake --build out/gpu-arm64 --parallel "${BB_BUILD_JOBS:-6}" --target camera-motion-test >/dev/null
        out/gpu-arm64/camera-motion-test 2>&1 | grep -E 'PASS|FAIL|Assertion' || { echo 'FAIL: camera-motion-test'; exit 1; }
        MTL_DEBUG_LAYER=1 out/gpu-arm64/camera-motion-test 2>&1 | grep -E 'PASS|FAIL|Assertion' || { echo 'FAIL: camera-motion-test (validation)'; exit 1; }
        # Metal-native M5: the TAA kernel and its sharpen (Taa::Record), plain and under validation.
        cmake --build out/gpu-arm64 --parallel "${BB_BUILD_JOBS:-6}" --target taa-shader-test >/dev/null
        out/gpu-arm64/taa-shader-test 2>&1 | grep -E 'PASS|FAIL|Assertion|regression' || { echo 'FAIL: taa-shader-test'; exit 1; }
        MTL_DEBUG_LAYER=1 out/gpu-arm64/taa-shader-test 2>&1 | grep -E 'PASS|FAIL|Assertion|regression' || { echo 'FAIL: taa-shader-test (validation)'; exit 1; }
        # Metal-native M5: fragment inputs the vertex stage does not write read as zero.
        cmake --build out/gpu-arm64 --parallel "${BB_BUILD_JOBS:-6}" --target spirv-inputs-test >/dev/null
        out/gpu-arm64/spirv-inputs-test 2>&1 | grep -E 'PASS|FAIL|Assertion' || { echo 'FAIL: spirv-inputs-test'; exit 1; }
        # Metal-native M5: the pipeline key's depth/stencil formats follow the pass's attachment.
        cmake --build out/gpu-arm64 --parallel "${BB_BUILD_JOBS:-6}" --target depth-attachment-test >/dev/null
        out/gpu-arm64/depth-attachment-test || { echo 'FAIL: depth-attachment-test'; exit 1; }
    fi
    # Metal-native M3: every pipeline of a stored pipeline cache built through the renderer's
    # pipeline cache (needs a cache from a game run; BB_MSL_CHECK_DIR picks another one).
    msl_dir=${BB_MSL_CHECK_DIR:-out/macdata/user/cache/CUSA03173}
    if [[ -x out/bbgpu-host && -d $msl_dir ]]; then
        out/bbgpu-host --msl-check "$msl_dir" 2>&1 | grep -E '^(PASS|FAIL|MSL check: FAILED)'
    fi
fi
