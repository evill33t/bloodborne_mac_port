// SPDX-License-Identifier: GPL-2.0-or-later
#include "bbport_party_icons.h"

#include <cstdlib>
#include <cstring>

#include "miniz.h"

namespace BbPartyIcons {

std::optional<Atlas> Decode(const std::uint8_t* data, std::size_t size) {
    constexpr std::size_t Header = 10;
    if (data == nullptr || size < Header || std::memcmp(data, "BBIC", 4) != 0) {
        return std::nullopt;
    }
    const auto u16 = [&](std::size_t at) { return int(data[at] | data[at + 1] << 8); };
    Atlas atlas{.width = u16(4), .height = u16(6), .count = u16(8), .rgba = {}};
    if (atlas.count != int(Icon::Count) || atlas.height != Size || atlas.width != Size * atlas.count) {
        return std::nullopt;
    }
    std::size_t length = 0;
    void* pixels = tinfl_decompress_mem_to_heap(data + Header, size - Header, &length,
                                                TINFL_FLAG_PARSE_ZLIB_HEADER);
    if (pixels == nullptr) {
        return std::nullopt;
    }
    const std::size_t expected = std::size_t(atlas.width) * std::size_t(atlas.height) * 4;
    if (length == expected) {
        const auto* bytes = static_cast<const std::uint8_t*>(pixels);
        atlas.rgba.assign(bytes, bytes + length);
    }
    mz_free(pixels);
    if (atlas.rgba.empty()) {
        return std::nullopt;
    }
    return atlas;
}

} // namespace BbPartyIcons
