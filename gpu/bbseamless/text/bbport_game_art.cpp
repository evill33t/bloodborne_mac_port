// SPDX-License-Identifier: GPL-2.0-or-later
#include "bbport_game_art.h"

#include <algorithm>
#include <climits>
#include <cstdlib>
#include <cstring>
#include "miniz.h"

namespace BbGameArt {

namespace {

std::uint32_t Le32(std::span<const std::uint8_t> bytes, std::size_t at) {
    return std::uint32_t(bytes[at]) | std::uint32_t(bytes[at + 1]) << 8 |
           std::uint32_t(bytes[at + 2]) << 16 | std::uint32_t(bytes[at + 3]) << 24;
}

std::uint16_t Le16(std::span<const std::uint8_t> bytes, std::size_t at) {
    return std::uint16_t(bytes[at] | bytes[at + 1] << 8);
}

// BC7 tables (the format's specification): two- and three-subset partitions, anchor indices and
// interpolation weights.
constexpr std::uint16_t Partitions2[64] = {
    0xCCCC, 0x8888, 0xEEEE, 0xECC8, 0xC880, 0xFEEC, 0xFEC8, 0xEC80, 0xC800, 0xFFEC, 0xFE80,
    0xE800, 0xFFE8, 0xFF00, 0xFFF0, 0xF000, 0xF710, 0x008E, 0x7100, 0x08CE, 0x008C, 0x7310,
    0x3100, 0x8CCE, 0x088C, 0x3110, 0x6666, 0x366C, 0x17E8, 0x0FF0, 0x718E, 0x399C, 0xAAAA,
    0xF0F0, 0x5A5A, 0x33CC, 0x3C3C, 0x55AA, 0x9696, 0xA55A, 0x73CE, 0x13C8, 0x324C, 0x3BDC,
    0x6996, 0xC33C, 0x9966, 0x0660, 0x0272, 0x04E4, 0x4E40, 0x2720, 0xC936, 0x936C, 0x39C6,
    0x639C, 0x9336, 0x9CC6, 0x817E, 0xE718, 0xCCF0, 0x0FCC, 0x7744, 0xEE22};
constexpr const char* Partitions3[64] = {
    "0011001102210222", "0001001122112221", "0000200122112211", "0222002200110111",
    "0000000011221122", "0011001100220022", "0022002211111111", "0011001122112211",
    "0000000011112222", "0000111111112222", "0000111122222222", "0012001200120012",
    "0112011201120112", "0122012201220122", "0011011211221222", "0011200122002220",
    "0001001101120112", "0111001120012200", "0000112211221122", "0022002200221111",
    "0111011102220222", "0001000122212221", "0000001101220122", "0000110011222120",
    "0122001200110000", "0012001211222222", "0110122112210110", "0000011012211221",
    "0022110211020022", "0110011020022222", "0011012201220011", "0000200022112221",
    "0000000211221222", "0222002200120011", "0011001200220222", "0120012001200120",
    "0000111122220000", "0120120120120120", "0120201212010120", "0011220011220011",
    "0011112222000011", "0101010122222222", "0000000021212121", "0022112200221122",
    "0022001100220011", "0220122102201221", "0101222222220101", "0000212121212121",
    "0101010101012222", "0222011102220111", "0002111200021112", "0000211221122112",
    "0222011101110222", "0002111211120002", "0110011001102222", "0000000021122112",
    "0110011022222222", "0022001100110022", "0022112211220022", "0000000000002112",
    "0002000100020001", "0222122202221222", "0101222222222222", "0111201122012220"};
constexpr std::uint8_t Anchor2[64] = {15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15,
                                      15, 15, 15, 15, 2,  8,  2,  2,  8,  8,  15, 2,  8,
                                      2,  2,  8,  8,  2,  2,  15, 15, 6,  8,  2,  8,  15,
                                      15, 2,  8,  2,  2,  2,  15, 15, 6,  6,  2,  6,  8,
                                      15, 15, 2,  2,  15, 15, 15, 15, 15, 2,  2,  15};
constexpr std::uint8_t Anchor3a[64] = {3,  3,  15, 15, 8,  3,  15, 15, 8,  8,  6,  6,  6,
                                       5,  3,  3,  3,  3,  8,  15, 3,  3,  6,  10, 5,  8,
                                       8,  6,  8,  5,  15, 15, 8,  15, 3,  5,  6,  10, 8,
                                       15, 15, 3,  15, 5,  15, 15, 15, 15, 3,  15, 5,  5,
                                       5,  8,  5,  10, 5,  10, 8,  13, 15, 12, 3,  3};
constexpr std::uint8_t Anchor3b[64] = {15, 8,  8,  3,  15, 15, 3,  8,  15, 15, 15, 15, 15,
                                       15, 15, 8,  15, 8,  15, 3,  15, 8,  15, 8,  3,  15,
                                       6,  10, 15, 15, 10, 8,  15, 3,  15, 10, 10, 8,  9,
                                       10, 6,  15, 8,  15, 3,  6,  6,  8,  15, 3,  15, 15,
                                       15, 15, 15, 15, 15, 15, 15, 15, 3,  15, 15, 8};
constexpr std::uint8_t Weights2[4] = {0, 21, 43, 64};
constexpr std::uint8_t Weights3[8] = {0, 9, 18, 27, 37, 46, 55, 64};
constexpr std::uint8_t Weights4[16] = {0, 4, 9, 13, 17, 21, 26, 30, 34, 38, 43, 47, 51, 55, 60, 64};

const std::uint8_t* WeightsFor(int bits) {
    return bits == 2 ? Weights2 : bits == 3 ? Weights3 : Weights4;
}

struct Bc7Mode {
    int subsets, partition_bits, rotation_bits, selector_bits, color_bits, alpha_bits;
    bool endpoint_pbits, shared_pbits;
    int index_bits, index2_bits;
};
constexpr Bc7Mode Modes[8] = {
    {3, 4, 0, 0, 4, 0, true, false, 3, 0}, {2, 6, 0, 0, 6, 0, false, true, 3, 0},
    {3, 6, 0, 0, 5, 0, false, false, 2, 0}, {2, 6, 0, 0, 7, 0, true, false, 2, 0},
    {1, 0, 2, 1, 5, 6, false, false, 2, 3}, {1, 0, 2, 0, 7, 8, false, false, 2, 2},
    {1, 0, 0, 0, 7, 7, true, false, 4, 0},  {2, 6, 0, 0, 5, 5, true, false, 2, 0}};

class Bits {
public:
    explicit Bits(std::span<const std::uint8_t, 16> block) : block{block} {}
    int Take(int count) {
        int value = 0;
        for (int i = 0; i < count; ++i, ++at) {
            value |= ((block[at >> 3] >> (at & 7)) & 1) << i;
        }
        return value;
    }

private:
    std::span<const std::uint8_t, 16> block;
    int at = 0;
};

int SubsetOf(int subsets, int partition, int pixel) {
    if (subsets == 1) {
        return 0;
    }
    if (subsets == 2) {
        return (Partitions2[partition] >> pixel) & 1;
    }
    return Partitions3[partition][pixel] - '0';
}

int AnchorOf(int subsets, int partition, int subset) {
    if (subset == 0) {
        return 0;
    }
    if (subsets == 2) {
        return Anchor2[partition];
    }
    return subset == 1 ? Anchor3a[partition] : Anchor3b[partition];
}

} // namespace

std::optional<std::vector<std::uint8_t>> InflateDcx(std::span<const std::uint8_t> file) {
    if (file.size() < 16 || std::memcmp(file.data(), "DCX\0", 4) != 0) {
        return std::nullopt;
    }
    static constexpr std::uint8_t Dca[] = {'D', 'C', 'A', 0};
    const auto dca = std::search(file.begin(), file.end(), std::begin(Dca), std::end(Dca));
    if (dca == file.end() || std::size_t(file.end() - dca) < 8) {
        return std::nullopt;
    }
    const std::size_t start = std::size_t(dca - file.begin()) + 8;
    std::size_t size = 0;
    void* out = tinfl_decompress_mem_to_heap(file.data() + start, file.size() - start, &size,
                                             TINFL_FLAG_PARSE_ZLIB_HEADER);
    if (out == nullptr) {
        return std::nullopt;
    }
    std::vector<std::uint8_t> payload(static_cast<std::uint8_t*>(out),
                                      static_cast<std::uint8_t*>(out) + size);
    mz_free(out);
    return payload;
}

std::optional<std::vector<TpfTexture>> ParseTpf(std::span<const std::uint8_t> tpf) {
    if (tpf.size() < 16 || std::memcmp(tpf.data(), "TPF\0", 4) != 0) {
        return std::nullopt;
    }
    const std::uint32_t count = Le32(tpf, 8);
    if (count > 4096 || 16 + std::size_t(count) * 0x24 > tpf.size()) {
        return std::nullopt;
    }
    std::vector<TpfTexture> textures;
    for (std::uint32_t index = 0; index < count; ++index) {
        const std::size_t entry = 16 + std::size_t(index) * 0x24;
        TpfTexture texture{.name = {},
                           .dxgi = Le32(tpf, entry + 0x20),
                           .width = Le16(tpf, entry + 12),
                           .height = Le16(tpf, entry + 14),
                           .offset = Le32(tpf, entry),
                           .size = Le32(tpf, entry + 4)};
        if (std::size_t(texture.offset) + texture.size > tpf.size()) {
            return std::nullopt;
        }
        for (std::size_t at = Le32(tpf, entry + 0x18); at + 1 < tpf.size(); at += 2) {
            const std::uint16_t unit = Le16(tpf, at);
            if (unit == 0) {
                break;
            }
            texture.name += unit < 0x80 ? char(unit) : '?';
        }
        textures.push_back(std::move(texture));
    }
    return textures;
}

std::vector<std::uint8_t> DeswizzleBlocks(std::span<const std::uint8_t> data, int width_blocks,
                                          int height_blocks, int block_bytes) {
    std::vector<std::uint8_t> out(std::size_t(width_blocks) * height_blocks * block_bytes);
    const int tiles_x = (width_blocks + 7) / 8, tiles_y = (height_blocks + 7) / 8;
    std::size_t source = 0;
    for (int tile_y = 0; tile_y < tiles_y; ++tile_y) {
        for (int tile_x = 0; tile_x < tiles_x; ++tile_x) {
            for (int i = 0; i < 64; ++i, source += block_bytes) {
                int x = 0, y = 0;
                for (int bit = 0; bit < 3; ++bit) {
                    x |= ((i >> (2 * bit)) & 1) << bit;
                    y |= ((i >> (2 * bit + 1)) & 1) << bit;
                }
                x += tile_x * 8;
                y += tile_y * 8;
                if (x < width_blocks && y < height_blocks && source + block_bytes <= data.size()) {
                    std::memcpy(out.data() + (std::size_t(y) * width_blocks + x) * block_bytes,
                                data.data() + source, block_bytes);
                }
            }
        }
    }
    return out;
}

std::array<std::uint8_t, 64> DecodeBc7Block(std::span<const std::uint8_t, 16> block) {
    std::array<std::uint8_t, 64> out{};
    int mode = 0;
    while (mode < 8 && ((block[0] >> mode) & 1) == 0) {
        ++mode;
    }
    if (mode == 8) {
        return out; // reserved: transparent black
    }
    const Bc7Mode& m = Modes[mode];
    Bits bits{block};
    bits.Take(mode + 1);
    const int partition = bits.Take(m.partition_bits);
    const int rotation = bits.Take(m.rotation_bits);
    const int selector = bits.Take(m.selector_bits);
    const int endpoints = m.subsets * 2;
    int color[6][4] = {};
    for (int channel = 0; channel < 3; ++channel) {
        for (int e = 0; e < endpoints; ++e) {
            color[e][channel] = bits.Take(m.color_bits);
        }
    }
    for (int e = 0; e < endpoints; ++e) {
        color[e][3] = m.alpha_bits ? bits.Take(m.alpha_bits) : 255;
    }
    int pbit[6] = {};
    if (m.endpoint_pbits) {
        for (int e = 0; e < endpoints; ++e) {
            pbit[e] = bits.Take(1);
        }
    } else if (m.shared_pbits) {
        for (int s = 0; s < m.subsets; ++s) {
            pbit[2 * s] = pbit[2 * s + 1] = bits.Take(1);
        }
    }
    const bool with_pbits = m.endpoint_pbits || m.shared_pbits;
    for (int e = 0; e < endpoints; ++e) {
        for (int channel = 0; channel < 4; ++channel) {
            if (channel == 3 && m.alpha_bits == 0) {
                continue;
            }
            int value = color[e][channel];
            int width = channel < 3 ? m.color_bits : m.alpha_bits;
            if (with_pbits) {
                value = value << 1 | pbit[e];
                ++width;
            }
            color[e][channel] = width < 8 ? (value << (8 - width)) | (value >> (2 * width - 8))
                                          : value;
        }
    }
    int index[16] = {}, index2[16] = {};
    for (int pixel = 0; pixel < 16; ++pixel) {
        const int subset = SubsetOf(m.subsets, partition, pixel);
        index[pixel] =
            bits.Take(m.index_bits - (pixel == AnchorOf(m.subsets, partition, subset) ? 1 : 0));
    }
    if (m.index2_bits) {
        for (int pixel = 0; pixel < 16; ++pixel) {
            index2[pixel] = bits.Take(m.index2_bits - (pixel == 0 ? 1 : 0));
        }
    }
    for (int pixel = 0; pixel < 16; ++pixel) {
        const int subset = SubsetOf(m.subsets, partition, pixel);
        const int* a = color[2 * subset];
        const int* b = color[2 * subset + 1];
        int color_index = index[pixel], alpha_index = index[pixel];
        const std::uint8_t* color_weights = WeightsFor(m.index_bits);
        const std::uint8_t* alpha_weights = color_weights;
        if (m.index2_bits) {
            if (selector) {
                color_index = index2[pixel];
                color_weights = WeightsFor(m.index2_bits);
            } else {
                alpha_index = index2[pixel];
                alpha_weights = WeightsFor(m.index2_bits);
            }
        }
        int px[4];
        for (int channel = 0; channel < 3; ++channel) {
            const int w = color_weights[color_index];
            px[channel] = ((64 - w) * a[channel] + w * b[channel] + 32) >> 6;
        }
        const int w = alpha_weights[alpha_index];
        px[3] = ((64 - w) * a[3] + w * b[3] + 32) >> 6;
        if (rotation) {
            std::swap(px[3], px[rotation - 1]);
        }
        for (int channel = 0; channel < 4; ++channel) {
            out[pixel * 4 + channel] = std::uint8_t(px[channel]);
        }
    }
    return out;
}

std::vector<std::uint8_t> DecodeBc7Rect(std::span<const std::uint8_t> blocks, int width,
                                        const PixelRect& rect) {
    const int w = rect.x1 - rect.x0, h = rect.y1 - rect.y0, width_blocks = (width + 3) / 4;
    std::vector<std::uint8_t> rgba(std::size_t(std::max(w, 0)) * std::max(h, 0) * 4);
    for (int y = rect.y0; y < rect.y1; ++y) {
        for (int x = rect.x0; x < rect.x1; ++x) {
            const std::size_t at = (std::size_t(y / 4) * width_blocks + x / 4) * 16;
            if (at + 16 > blocks.size()) {
                continue;
            }
            // Decoding a block per pixel is wasteful but this runs once, on a few thousand pixels.
            const auto pixels = DecodeBc7Block(std::span<const std::uint8_t, 16>{blocks.data() + at, 16});
            std::memcpy(rgba.data() + (std::size_t(y - rect.y0) * w + (x - rect.x0)) * 4,
                        pixels.data() + ((y % 4) * 4 + x % 4) * 4, 4);
        }
    }
    return rgba;
}

std::optional<GaugeArt> LoadGaugeArt(std::span<const std::uint8_t> common_tpf_dcx) {
    const auto tpf = InflateDcx(common_tpf_dcx);
    const auto textures = tpf ? ParseTpf(*tpf) : std::nullopt;
    if (!textures) {
        return std::nullopt;
    }
    const auto texture = std::ranges::find(*textures, GaugeTexture, &TpfTexture::name);
    if (texture == textures->end() || texture->dxgi != 98 || texture->width != 1024 ||
        texture->height != 512) {
        return std::nullopt;
    }
    const int width_blocks = texture->width / 4, height_blocks = texture->height / 4;
    if (texture->size < std::uint32_t(width_blocks * height_blocks * 16)) {
        return std::nullopt;
    }
    const auto blocks = DeswizzleBlocks(
        std::span<const std::uint8_t>{tpf->data() + texture->offset, texture->size}, width_blocks,
        height_blocks, 16);
    const PixelRect rows{0, 0, 656, 48};
    return GaugeArt{.width = rows.x1, .height = rows.y1,
                    .rgba = DecodeBc7Rect(blocks, texture->width, rows)};
}

std::array<std::uint8_t, 64> DecodeBc1Block(std::span<const std::uint8_t, 8> block) {
    const auto colour = [&](int at) {
        const int v = block[at] | block[at + 1] << 8;
        const int r = (v >> 11) & 31, g = (v >> 5) & 63, b = v & 31;
        return std::array<int, 4>{(r << 3) | (r >> 2), (g << 2) | (g >> 4), (b << 3) | (b >> 2), 255};
    };
    const int first = block[0] | block[1] << 8, second = block[2] | block[3] << 8;
    std::array<std::array<int, 4>, 4> palette{colour(0), colour(2), {}, {}};
    for (int channel = 0; channel < 3; ++channel) {
        const int c0 = palette[0][channel], c1 = palette[1][channel];
        if (first > second) {
            palette[2][channel] = (2 * c0 + c1) / 3;
            palette[3][channel] = (c0 + 2 * c1) / 3;
        } else {
            palette[2][channel] = (c0 + c1 + 1) / 2;
            palette[3][channel] = 0;
        }
    }
    palette[2][3] = 255;
    palette[3][3] = first > second ? 255 : 0;
    std::array<std::uint8_t, 64> out{};
    for (int pixel = 0; pixel < 16; ++pixel) {
        const int index = (block[4 + pixel / 4] >> (2 * (pixel % 4))) & 3;
        for (int channel = 0; channel < 4; ++channel) {
            out[pixel * 4 + channel] = std::uint8_t(palette[index][channel]);
        }
    }
    return out;
}

std::optional<TpfFile> TpfFile::Read(std::span<const std::uint8_t> tpf_dcx) {
    auto tpf = InflateDcx(tpf_dcx);
    auto textures = tpf ? ParseTpf(*tpf) : std::nullopt;
    if (!textures) {
        return std::nullopt;
    }
    TpfFile file;
    file.tpf = std::move(*tpf);
    file.textures = std::move(*textures);
    return file;
}

std::optional<Image> TpfFile::Piece(std::string_view name, const PixelRect& wanted) const {
    const auto texture = std::ranges::find(textures, name, &TpfTexture::name);
    if (texture == textures.end() || (texture->dxgi != 98 && texture->dxgi != 71)) {
        return std::nullopt;
    }
    const PixelRect rect = wanted.x1 > wanted.x0 && wanted.y1 > wanted.y0
                               ? wanted
                               : PixelRect{0, 0, texture->width, texture->height};
    if (rect.x0 < 0 || rect.y0 < 0 || rect.x1 > texture->width || rect.y1 > texture->height) {
        return std::nullopt;
    }
    const int block_bytes = texture->dxgi == 98 ? 16 : 8;
    const int width_blocks = (texture->width + 3) / 4, height_blocks = (texture->height + 3) / 4;
    if (texture->size < std::uint32_t(width_blocks * height_blocks * block_bytes)) {
        return std::nullopt;
    }
    const auto blocks = DeswizzleBlocks(
        std::span<const std::uint8_t>{tpf.data() + texture->offset, texture->size}, width_blocks,
        height_blocks, block_bytes);
    Image image{.width = rect.x1 - rect.x0, .height = rect.y1 - rect.y0, .rgba = {}};
    image.rgba.resize(std::size_t(image.width) * image.height * 4);
    // Each block the piece touches, decoded once.
    for (int by = rect.y0 / 4; by <= (rect.y1 - 1) / 4; ++by) {
        for (int bx = rect.x0 / 4; bx <= (rect.x1 - 1) / 4; ++bx) {
            const std::uint8_t* at = blocks.data() + (std::size_t(by) * width_blocks + bx) * block_bytes;
            const auto pixels =
                block_bytes == 16 ? DecodeBc7Block(std::span<const std::uint8_t, 16>{at, 16})
                                  : DecodeBc1Block(std::span<const std::uint8_t, 8>{at, 8});
            for (int py = 0; py < 4; ++py) {
                const int y = by * 4 + py;
                if (y < rect.y0 || y >= rect.y1) {
                    continue;
                }
                for (int px = 0; px < 4; ++px) {
                    const int x = bx * 4 + px;
                    if (x < rect.x0 || x >= rect.x1) {
                        continue;
                    }
                    std::memcpy(image.rgba.data() +
                                    (std::size_t(y - rect.y0) * image.width + (x - rect.x0)) * 4,
                                pixels.data() + (py * 4 + px) * 4, 4);
                }
            }
        }
    }
    return image;
}

namespace {

// SWF bit fields: most significant bit first.
struct SwfBits {
    std::span<const std::uint8_t> data;
    std::size_t at = 0;
    int bit = 0;
    bool overrun = false;
    std::uint32_t Unsigned(int count) {
        std::uint32_t value = 0;
        for (int i = 0; i < count; ++i) {
            if (at >= data.size()) {
                overrun = true;
                return 0;
            }
            value = value << 1 | ((data[at] >> (7 - bit)) & 1);
            if (++bit == 8) {
                bit = 0;
                ++at;
            }
        }
        return value;
    }
    std::int32_t Signed(int count) {
        const std::uint32_t value = Unsigned(count);
        return count > 0 && (value >> (count - 1)) & 1 ? std::int32_t(value) - (1 << count)
                                                        : std::int32_t(value);
    }
};

struct GlyphPoint {
    bool on = true;
    std::int32_t x = 0, y = 0;
};
using Contour = std::vector<GlyphPoint>;

// A DefineFont3 glyph's SHAPE: its contours (a move starts one), in twips (20 to a font unit).
std::optional<std::vector<Contour>> GlyphContours(std::span<const std::uint8_t> shape) {
    SwfBits bits{shape};
    const int fill_bits = int(bits.Unsigned(4)), line_bits = int(bits.Unsigned(4));
    std::vector<Contour> contours;
    Contour current;
    std::int32_t x = 0, y = 0;
    for (int records = 0; records < 100000 && !bits.overrun; ++records) {
        if (bits.Unsigned(1) == 0) {
            const std::uint32_t flags = bits.Unsigned(5);
            if (flags == 0) {
                break;
            }
            if (flags & 16) {
                return std::nullopt; // new styles: not a font glyph
            }
            if (flags & 1) {
                const int count = int(bits.Unsigned(5));
                x = bits.Signed(count);
                y = bits.Signed(count);
                if (!current.empty()) {
                    contours.push_back(std::move(current));
                }
                current = {GlyphPoint{true, x, y}};
            }
            bits.Unsigned(flags & 2 ? fill_bits : 0);
            bits.Unsigned(flags & 4 ? fill_bits : 0);
            bits.Unsigned(flags & 8 ? line_bits : 0);
            continue;
        }
        if (current.empty()) {
            current.push_back(GlyphPoint{true, x, y});
        }
        const bool straight = bits.Unsigned(1) != 0;
        const int count = int(bits.Unsigned(4)) + 2;
        if (straight) {
            if (bits.Unsigned(1)) {
                x += bits.Signed(count);
                y += bits.Signed(count);
            } else if (bits.Unsigned(1)) {
                y += bits.Signed(count);
            } else {
                x += bits.Signed(count);
            }
            current.push_back(GlyphPoint{true, x, y});
        } else {
            const std::int32_t cx = x + bits.Signed(count), cy = y + bits.Signed(count);
            x = cx + bits.Signed(count);
            y = cy + bits.Signed(count);
            current.push_back(GlyphPoint{false, cx, cy});
            current.push_back(GlyphPoint{true, x, y});
        }
    }
    if (bits.overrun) {
        return std::nullopt;
    }
    if (!current.empty()) {
        contours.push_back(std::move(current));
    }
    std::vector<Contour> closed;
    for (Contour& contour : contours) {
        if (contour.size() > 1 && contour.front().x == contour.back().x &&
            contour.front().y == contour.back().y) {
            contour.pop_back();
        }
        if (contour.size() >= 3) {
            closed.push_back(std::move(contour));
        }
    }
    return closed;
}

struct Bytes {
    std::vector<std::uint8_t> data;
    void U8(std::uint32_t v) {
        data.push_back(std::uint8_t(v));
    }
    void U16(std::uint32_t v) {
        U8(v >> 8);
        U8(v);
    }
    void U32(std::uint32_t v) {
        U16(v >> 16);
        U16(v & 0xFFFF);
    }
    void Pad() {
        while (data.size() % 4) {
            U8(0);
        }
    }
};

std::uint32_t TableSum(const std::vector<std::uint8_t>& data) {
    std::uint32_t sum = 0;
    for (std::size_t i = 0; i < data.size(); i += 4) {
        std::uint32_t word = 0;
        for (std::size_t j = 0; j < 4; ++j) {
            word = word << 8 | (i + j < data.size() ? data[i + j] : 0);
        }
        sum += word;
    }
    return sum;
}

} // namespace

std::optional<std::vector<std::uint8_t>> TtfFromGfxFont(std::span<const std::uint8_t> gfx) {
    // The file: "GFX" (or "FWS"), version, size, the stage RECT, frame rate and count; then tags.
    if (gfx.size() < 13 || !((gfx[0] == 'G' && gfx[1] == 'F' && gfx[2] == 'X') ||
                             (gfx[0] == 'F' && gfx[1] == 'W' && gfx[2] == 'S'))) {
        return std::nullopt;
    }
    std::size_t at = 8 + std::size_t(5 + 4 * (gfx[8] >> 3) + 7) / 8 + 4;
    std::span<const std::uint8_t> font;
    while (at + 2 <= gfx.size()) {
        const std::uint16_t code_size = Le16(gfx, at);
        at += 2;
        std::size_t size = code_size & 0x3F;
        if (size == 0x3F) {
            if (at + 4 > gfx.size()) {
                return std::nullopt;
            }
            size = Le32(gfx, at);
            at += 4;
        }
        if (at + size > gfx.size()) {
            return std::nullopt;
        }
        if (code_size >> 6 == 75) {
            font = gfx.subspan(at, size);
            break;
        }
        if (code_size >> 6 == 0) {
            break;
        }
        at += size;
    }
    if (font.size() < 8) {
        return std::nullopt;
    }
    // DefineFont3: id, flags (0x80 layout, 0x08 wide offsets, 0x04 wide codes, 0x01 bold),
    // language, name, glyph count, offsets (from the table's start), code table, layout.
    const std::uint8_t flags = font[2];
    std::size_t p = 5 + font[4];
    if (p + 2 > font.size() || !(flags & 0x80) || !(flags & 0x04)) {
        return std::nullopt;
    }
    const std::size_t count = Le16(font, p);
    p += 2;
    const std::size_t table = p, offset_bytes = flags & 0x08 ? 4 : 2;
    if (table + (count + 1) * offset_bytes > font.size()) {
        return std::nullopt;
    }
    const auto offset = [&](std::size_t index) {
        return offset_bytes == 4 ? std::size_t(Le32(font, table + index * 4))
                                 : std::size_t(Le16(font, table + index * 2));
    };
    const std::size_t codes = table + offset(count);
    if (codes + count * 4 + 6 > font.size()) {
        return std::nullopt;
    }
    const auto s16 = [&](std::size_t at_) { return std::int16_t(Le16(font, at_)); };
    const std::int32_t ascent = s16(codes + count * 2), descent = s16(codes + count * 2 + 2),
                       leading = s16(codes + count * 2 + 4);
    const std::size_t advances = codes + count * 2 + 6;
    // The glyphs in TrueType units (twips / 20; y up), .notdef first.
    struct Glyph {
        std::vector<std::vector<GlyphPoint>> contours;
        int advance = 0;
    };
    std::vector<Glyph> glyphs(1);
    std::vector<std::pair<std::uint32_t, std::uint32_t>> map; // code -> glyph
    for (std::size_t i = 0; i < count; ++i) {
        const std::size_t begin = table + offset(i), end = i + 1 < count ? table + offset(i + 1) : codes;
        if (begin > end || end > font.size()) {
            return std::nullopt;
        }
        auto contours = GlyphContours(font.subspan(begin, end - begin));
        if (!contours) {
            continue;
        }
        Glyph glyph;
        for (const Contour& contour : *contours) {
            auto& out = glyph.contours.emplace_back();
            for (const GlyphPoint& point : contour) {
                out.push_back(GlyphPoint{point.on, (point.x + (point.x >= 0 ? 10 : -10)) / 20,
                                         -((point.y + (point.y >= 0 ? 10 : -10)) / 20)});
            }
        }
        glyph.advance = advances + i * 2 + 2 <= font.size() ? s16(advances + i * 2) / 20 : 0;
        map.emplace_back(Le16(font, codes + i * 2), std::uint32_t(glyphs.size()));
        glyphs.push_back(std::move(glyph));
    }
    std::ranges::sort(map);
    if (map.empty()) {
        return std::nullopt;
    }
    // glyf and loca.
    Bytes glyf;
    std::vector<std::uint32_t> loca;
    int x_min = 0, y_min = 0, x_max = 0, y_max = 0, max_points = 0, max_contours = 0, max_advance = 0;
    bool any = false;
    for (const Glyph& glyph : glyphs) {
        loca.push_back(std::uint32_t(glyf.data.size()));
        max_advance = std::max(max_advance, glyph.advance);
        if (glyph.contours.empty()) {
            continue;
        }
        int gx0 = INT32_MAX, gy0 = INT32_MAX, gx1 = INT32_MIN, gy1 = INT32_MIN, points = 0;
        for (const auto& contour : glyph.contours) {
            for (const GlyphPoint& point : contour) {
                gx0 = std::min(gx0, point.x);
                gy0 = std::min(gy0, point.y);
                gx1 = std::max(gx1, point.x);
                gy1 = std::max(gy1, point.y);
                ++points;
            }
        }
        x_min = any ? std::min(x_min, gx0) : gx0;
        y_min = any ? std::min(y_min, gy0) : gy0;
        x_max = any ? std::max(x_max, gx1) : gx1;
        y_max = any ? std::max(y_max, gy1) : gy1;
        any = true;
        max_points = std::max(max_points, points);
        max_contours = std::max(max_contours, int(glyph.contours.size()));
        glyf.U16(std::uint32_t(glyph.contours.size()));
        glyf.U16(std::uint16_t(gx0));
        glyf.U16(std::uint16_t(gy0));
        glyf.U16(std::uint16_t(gx1));
        glyf.U16(std::uint16_t(gy1));
        int end = -1;
        for (const auto& contour : glyph.contours) {
            end += int(contour.size());
            glyf.U16(std::uint32_t(end));
        }
        glyf.U16(0); // no instructions
        for (const auto& contour : glyph.contours) {
            for (const GlyphPoint& point : contour) {
                glyf.U8(point.on ? 1 : 0);
            }
        }
        int previous = 0;
        for (const auto& contour : glyph.contours) {
            for (const GlyphPoint& point : contour) {
                glyf.U16(std::uint16_t(point.x - previous));
                previous = point.x;
            }
        }
        previous = 0;
        for (const auto& contour : glyph.contours) {
            for (const GlyphPoint& point : contour) {
                glyf.U16(std::uint16_t(point.y - previous));
                previous = point.y;
            }
        }
        glyf.Pad();
    }
    loca.push_back(std::uint32_t(glyf.data.size()));
    const std::uint32_t glyph_count = std::uint32_t(glyphs.size());
    const bool bold = flags & 0x01;
    const int asc = ascent / 20, desc = descent / 20;
    std::vector<std::pair<std::string, std::vector<std::uint8_t>>> tables;
    {
        Bytes head;
        head.U32(0x10000);
        head.U32(0x10000);
        head.U32(0); // checksum adjustment
        head.U32(0x5F0F3CF5);
        head.U16(0x000B);
        head.U16(1024);
        for (int i = 0; i < 4; ++i) {
            head.U32(0); // created, modified
        }
        head.U16(std::uint16_t(x_min));
        head.U16(std::uint16_t(y_min));
        head.U16(std::uint16_t(x_max));
        head.U16(std::uint16_t(y_max));
        head.U16(bold ? 1 : 0);
        head.U16(8);
        head.U16(2);
        head.U16(1); // long loca
        head.U16(0);
        tables.emplace_back("head", std::move(head.data));
    }
    {
        Bytes hhea;
        hhea.U32(0x10000);
        hhea.U16(std::uint16_t(asc));
        hhea.U16(std::uint16_t(-desc));
        hhea.U16(std::uint16_t(leading / 20));
        hhea.U16(std::uint16_t(max_advance));
        for (int i = 0; i < 3; ++i) {
            hhea.U16(0);
        }
        hhea.U16(1); // caret slope rise
        for (int i = 0; i < 7; ++i) {
            hhea.U16(0);
        }
        hhea.U16(glyph_count);
        tables.emplace_back("hhea", std::move(hhea.data));
    }
    {
        Bytes maxp;
        maxp.U32(0x10000);
        maxp.U16(glyph_count);
        maxp.U16(std::uint32_t(max_points));
        maxp.U16(std::uint32_t(max_contours));
        maxp.U16(0);
        maxp.U16(0);
        maxp.U16(2);
        for (int i = 0; i < 8; ++i) {
            maxp.U16(0);
        }
        tables.emplace_back("maxp", std::move(maxp.data));
    }
    {
        Bytes hmtx;
        for (const Glyph& glyph : glyphs) {
            hmtx.U16(std::uint32_t(std::max(glyph.advance, 0)));
            hmtx.U16(0);
        }
        tables.emplace_back("hmtx", std::move(hmtx.data));
    }
    {
        Bytes out;
        for (const std::uint32_t value : loca) {
            out.U32(value);
        }
        tables.emplace_back("loca", std::move(out.data));
    }
    tables.emplace_back("glyf", std::move(glyf.data));
    {
        // Format 12, runs of codes that map to runs of glyphs.
        std::vector<std::array<std::uint32_t, 3>> groups;
        for (const auto& [code, glyph] : map) {
            if (!groups.empty() && groups.back()[1] + 1 == code &&
                groups.back()[2] + (code - groups.back()[0]) == glyph) {
                groups.back()[1] = code;
            } else if (groups.empty() || groups.back()[1] != code) {
                groups.push_back({code, code, glyph});
            }
        }
        Bytes cmap;
        cmap.U16(0);
        cmap.U16(1);
        cmap.U16(3);
        cmap.U16(10);
        cmap.U32(12);
        cmap.U16(12);
        cmap.U16(0);
        cmap.U32(std::uint32_t(16 + 12 * groups.size()));
        cmap.U32(0);
        cmap.U32(std::uint32_t(groups.size()));
        for (const auto& group : groups) {
            cmap.U32(group[0]);
            cmap.U32(group[1]);
            cmap.U32(group[2]);
        }
        tables.emplace_back("cmap", std::move(cmap.data));
    }
    {
        Bytes post;
        post.U32(0x30000);
        for (int i = 0; i < 7; ++i) {
            post.U32(0);
        }
        tables.emplace_back("post", std::move(post.data));
    }
    {
        static constexpr std::string_view Family = "Bloodborne Menu";
        const std::pair<int, std::string_view> names[] = {
            {1, Family}, {2, "Regular"}, {4, Family}, {6, "BloodborneMenu"}};
        Bytes name, strings;
        name.U16(0);
        name.U16(4);
        name.U16(6 + 12 * 4);
        for (const auto& [id, text] : names) {
            name.U16(3);
            name.U16(1);
            name.U16(0x409);
            name.U16(std::uint32_t(id));
            name.U16(std::uint32_t(text.size() * 2));
            name.U16(std::uint32_t(strings.data.size()));
            for (const char c : text) {
                strings.U16(std::uint8_t(c));
            }
        }
        name.data.insert(name.data.end(), strings.data.begin(), strings.data.end());
        tables.emplace_back("name", std::move(name.data));
    }
    {
        Bytes os2;
        os2.U16(1);
        os2.U16(500);
        os2.U16(bold ? 700 : 400);
        os2.U16(5);
        for (int i = 0; i < 12; ++i) {
            os2.U16(0);
        }
        for (int i = 0; i < 10; ++i) {
            os2.U8(0); // panose
        }
        for (int i = 0; i < 4; ++i) {
            os2.U32(0);
        }
        os2.U32(0); // vendor
        os2.U16(0x40);
        os2.U16(std::min<std::uint32_t>(map.front().first, 0xFFFF));
        os2.U16(std::min<std::uint32_t>(map.back().first, 0xFFFF));
        os2.U16(std::uint16_t(asc));
        os2.U16(std::uint16_t(-desc));
        os2.U16(0);
        os2.U16(std::uint16_t(asc));
        os2.U16(std::uint16_t(desc));
        os2.U32(1);
        os2.U32(0);
        tables.emplace_back("OS/2", std::move(os2.data));
    }
    std::ranges::sort(tables, {}, &decltype(tables)::value_type::first);
    Bytes out;
    out.U32(0x10000);
    out.U16(std::uint32_t(tables.size()));
    out.U16(0);
    out.U16(0);
    out.U16(0);
    std::uint32_t offset_now = std::uint32_t(12 + 16 * tables.size());
    for (const auto& [tag, data] : tables) {
        for (const char c : tag) {
            out.U8(std::uint8_t(c));
        }
        out.U32(TableSum(data));
        out.U32(offset_now);
        out.U32(std::uint32_t(data.size()));
        offset_now += std::uint32_t((data.size() + 3) / 4 * 4);
    }
    for (const auto& [tag, data] : tables) {
        out.data.insert(out.data.end(), data.begin(), data.end());
        out.Pad();
    }
    return out.data;
}

std::string_view KeyGuideTexture(int input) {
    static constexpr std::string_view textures[] = {
        "KG_Cancel", "KG_OK",   "KG_R_L",  "KG_R_U",  "KG_L1",   "KG_R1",   "KG_L2",
        "KG_R2",     "KG_L3",   "KG_R3",   "KG_Start", "KG_TP_L", "KG_TP_R", "KG_L_U",
        "KG_L_D",    "KG_L_LR", "KG_L_LR", "KG_LS",   "KG_LS",   "KG_LS",   "KG_LS",
        "KG_RS",     "KG_RS",   "KG_RS",   "KG_RS"};
    return input >= 0 && input < int(std::size(textures)) ? textures[input] : std::string_view{};
}

std::string_view KeyGuideTextureNamed(std::string_view name) {
    static constexpr std::pair<std::string_view, std::string_view> names[] = {
        {"Cross", "KG_Cancel"},  {"Circle", "KG_OK"},   {"Square", "KG_R_L"},  {"Triangle", "KG_R_U"},
        {"L1", "KG_L1"},         {"R1", "KG_R1"},       {"L2", "KG_L2"},       {"R2", "KG_R2"},
        {"L3", "KG_L3"},         {"R3", "KG_R3"},       {"Options", "KG_Start"}, {"TouchLeft", "KG_TP_L"},
        {"TouchRight", "KG_TP_R"}, {"Up", "KG_L_U"},    {"Down", "KG_L_D"},    {"LeftRight", "KG_L_LR"},
        {"UpDown", "KG_L_UD"},   {"Dpad", "KG_L_UDLR"}, {"LS", "KG_LS"},       {"RS", "KG_RS"}};
    for (const auto& [text, texture] : names) {
        if (text == name) {
            return texture;
        }
    }
    return {};
}

std::string GameMenuIcons(std::string_view text) {
    std::string out;
    std::size_t at = 0;
    while (at < text.size()) {
        const std::size_t open = text.find('{', at), close = text.find('}', open);
        const std::string_view texture = open != std::string_view::npos && close != std::string_view::npos
                                             ? KeyGuideTextureNamed(text.substr(open + 1, close - open - 1))
                                             : std::string_view{};
        if (texture.empty()) {
            const std::size_t next = open == std::string_view::npos ? text.size() : open + 1;
            out.append(text.substr(at, next - at));
            at = next;
            continue;
        }
        out.append(text.substr(at, open - at));
        out.append("<img src=\"").append(texture).append("\" vspace='-8'>");
        at = close + 1;
    }
    return out;
}

} // namespace BbGameArt
