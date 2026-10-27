// SPDX-License-Identifier: GPL-2.0-or-later
// bbport co-op: party death's icons (gpu/art/party_icons.svg, drawn for the port; the atlas
// party_icons.bin is made from it by scripts/party_icons_atlas.py and embedded in the overlay).
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace BbPartyIcons {

// The atlas's icons, left to right, each Size pixels square.
enum class Icon { Hunter, Downed, Fallen, Elsewhere, Strike, GiveUp, ReviveRing, Count };
constexpr int Size = 128;

struct Atlas {
    int width = 0;
    int height = 0;
    int count = 0;
    std::vector<std::uint8_t> rgba;
};

// 'BBIC', width, height and icon count (little-endian u16 each), then zlib-compressed RGBA: a row of
// Size-pixel icons, as many as Icon has. Nothing for anything else.
std::optional<Atlas> Decode(const std::uint8_t* data, std::size_t size);

// The horizontal texture coordinates of an icon in the atlas.
constexpr float U0(Icon icon) {
    return float(int(icon)) / float(int(Icon::Count));
}
constexpr float U1(Icon icon) {
    return float(int(icon) + 1) / float(int(Icon::Count));
}

// The red share drawn live over the rings, in an icon's 128 units from its centre: the downed
// icon's ring and the revive ring's.
struct Ring {
    float radius;
    float width;
};
constexpr Ring DownedRing{46.0f, 7.0f};
constexpr Ring ReviveRing{48.0f, 12.0f};

} // namespace BbPartyIcons
