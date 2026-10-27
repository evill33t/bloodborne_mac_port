#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
set -euo pipefail
cd -- "$(dirname -- "$0")/.."
mkdir -p out/map-tests
arch=${1:-arm64}
prefix="$PWD/deps/$arch"
objects=()
for source in bbport_map_art bbport_map_collision bbport_game_art bbport_map_charts; do
    /usr/bin/clang++ -arch "$arch" -std=c++20 -O2 -Wall -Wextra -Igpu/shim -I"$prefix/include/miniz" \
        -c "gpu/shim/$source.cpp" -o "out/map-tests/$source-$arch.o"
    objects+=("out/map-tests/$source-$arch.o")
done
for name in map_art map_view map_worker; do
    /usr/bin/clang++ -arch "$arch" -std=c++20 -O2 -Wall -Wextra -I. -Igpu/shim \
        "tests/test_$name.cpp" "${objects[@]}" "$prefix/lib/libminiz.a" -o "out/map-tests/$name-$arch"
    "out/map-tests/$name-$arch"
done
