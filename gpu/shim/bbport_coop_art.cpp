// SPDX-License-Identifier: GPL-2.0-or-later
#include "bbport_coop_art.h"

#include <algorithm>
#include <cstring>

#include "miniz.h"

namespace BbCoopArt {

std::optional<Atlas> Decode(const std::uint8_t* data, std::size_t size) {
    constexpr std::size_t Header = 10;
    if (data == nullptr || size < Header || std::memcmp(data, "BBCA", 4) != 0) {
        return std::nullopt;
    }
    const auto u16 = [&](std::size_t at) { return int(data[at] | data[at + 1] << 8); };
    Atlas atlas{.width = u16(4), .height = u16(6), .rects = {}, .rgba = {}};
    const int count = u16(8);
    const std::size_t pixels_at = Header + std::size_t(count) * 8;
    if (count != int(Piece::Count) || size < pixels_at || atlas.width == 0 || atlas.height == 0) {
        return std::nullopt;
    }
    for (int i = 0; i < count; ++i) {
        const std::size_t at = Header + std::size_t(i) * 8;
        const Rect rect{u16(at), u16(at + 2), u16(at + 4), u16(at + 6)};
        if (rect.width == 0 || rect.height == 0 || rect.x + rect.width > atlas.width ||
            rect.y + rect.height > atlas.height) {
            return std::nullopt;
        }
        atlas.rects.push_back(rect);
    }
    std::size_t length = 0;
    void* pixels = tinfl_decompress_mem_to_heap(data + pixels_at, size - pixels_at, &length,
                                                TINFL_FLAG_PARSE_ZLIB_HEADER);
    if (pixels == nullptr) {
        return std::nullopt;
    }
    if (length == std::size_t(atlas.width) * std::size_t(atlas.height) * 4) {
        const auto* bytes = static_cast<const std::uint8_t*>(pixels);
        atlas.rgba.assign(bytes, bytes + length);
    }
    mz_free(pixels);
    if (atlas.rgba.empty()) {
        return std::nullopt;
    }
    return atlas;
}

void Greyed(Atlas& atlas, const std::uint8_t grey[3]) {
    // The gold's own brightness (the art's flat gold, 0xC8B078) is the grey's.
    constexpr float GoldLuma = 0.299f * 200 + 0.587f * 176 + 0.114f * 120;
    for (std::size_t at = 0; at + 3 < atlas.rgba.size(); at += 4) {
        std::uint8_t* px = &atlas.rgba[at];
        const int r = px[0], g = px[1], b = px[2];
        // Warm: red over green over blue, the blue well below; yellow to orange, not red (where green
        // is about as low as blue).
        if (!(r >= g && g >= b && r - b > 12 && (r - g) * 10 < (r - b) * 7)) {
            continue;
        }
        const float luma = 0.299f * float(r) + 0.587f * float(g) + 0.114f * float(b);
        for (int c = 0; c < 3; ++c) {
            px[c] = std::uint8_t(std::min(255.0f, float(grey[c]) * luma / GoldLuma + 0.5f));
        }
    }
}

} // namespace BbCoopArt
