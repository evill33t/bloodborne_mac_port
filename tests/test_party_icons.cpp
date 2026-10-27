// SPDX-License-Identifier: GPL-2.0-or-later
// bbport co-op: the party death icons the overlay embeds (gpu/art/party_icons.bin) decode to a row of
// 128-pixel icons with clear corners and drawn middles; a damaged atlas decodes to nothing.
#include <cassert>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <vector>

#include "gpu/shim/bbport_party_icons.h"

using namespace BbPartyIcons;

namespace {

std::vector<std::uint8_t> ReadFile(const char* path) {
    std::ifstream file(path, std::ios::binary);
    return {std::istreambuf_iterator<char>{file}, {}};
}

std::uint8_t AlphaAt(const Atlas& atlas, int x, int y) {
    return atlas.rgba[(std::size_t(y) * std::size_t(atlas.width) + std::size_t(x)) * 4 + 3];
}

} // namespace

int main() {
    const std::vector<std::uint8_t> file = ReadFile(BB_PARTY_ICONS_PATH);
    assert(!file.empty());
    const auto atlas = Decode(file.data(), file.size());
    assert(atlas);
    assert(atlas->width == Size * int(Icon::Count) && atlas->height == Size);
    for (int icon = 0; icon < int(Icon::Count); ++icon) {
        const int left = icon * Size;
        assert(AlphaAt(*atlas, left + 1, 1) == 0);                // a clear corner
        assert(AlphaAt(*atlas, left + Size - 2, Size - 2) == 0);  // and another
        assert(AlphaAt(*atlas, left + Size / 2, Size / 2) > 100); // a drawn middle
    }
    assert(U0(Icon::Hunter) == 0.0f && U1(Icon::ReviveRing) == 1.0f);

    std::vector<std::uint8_t> damaged = file;
    damaged[0] = 'X';
    assert(!Decode(damaged.data(), damaged.size()));
    damaged = file;
    damaged.resize(file.size() / 2);
    assert(!Decode(damaged.data(), damaged.size()));
    damaged = file;
    damaged[8] = 3; // another icon count
    assert(!Decode(damaged.data(), damaged.size()));
    assert(!Decode(nullptr, 0));
    std::puts("party-icons-test: ok");
    return 0;
}
