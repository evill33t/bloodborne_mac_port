#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
set -euo pipefail
cd -- "$(dirname -- "$0")/.."
prefix=${BB_DEPS_DIR:-$PWD/deps/arm64}
mkdir -p out/map-editor
/usr/bin/clang++ -arch arm64 -mmacosx-version-min=15.0 -std=c++20 -O2 -g -UNDEBUG -Wall -Wextra -Werror \
    -Ieditor -Igpu/shim -isystem "$prefix/include/miniz" tests/test_game_params.cpp editor/game_params.cpp editor/map_document.cpp editor/event_document.cpp \
    "$prefix/lib/libminiz.a" -o out/map-editor/game-params-test
out/map-editor/game-params-test "$@"
