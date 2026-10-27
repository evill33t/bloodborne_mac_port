// SPDX-License-Identifier: GPL-2.0-or-later
// bbport co-op: the map screen's pictures of the areas, drawn from the game's map files
// (bbport_map_art.h).
#include "bbport_map_art.h"

#include <algorithm>
#include <atomic>
#include <cfloat>
#include <cmath>
#include <cstring>
#include <limits>
#include <map>
#include <mutex>
#include <thread>
#include <utility>

#include "bbport_game_art.h"
#include "miniz.h"

namespace BbMapArt {

namespace {

template <typename T>
bool Get(std::span<const std::uint8_t> bytes, std::size_t at, T& out) {
    if (at > bytes.size() || bytes.size() - at < sizeof(T)) {
        return false;
    }
    std::memcpy(&out, bytes.data() + at, sizeof(T));
    return true;
}

/// A UTF-16 name at `at` as the ASCII the game's file names are ("" past the end).
std::string Utf16At(std::span<const std::uint8_t> bytes, std::size_t at) {
    std::string text;
    for (std::uint16_t c = 0; at + 1 < bytes.size() && text.size() < 256; at += 2) {
        std::memcpy(&c, bytes.data() + at, 2);
        if (c == 0) {
            break;
        }
        text += c < 0x80 ? char(c) : '?';
    }
    return text;
}

/// The MSB's param lists: name -> the records' offsets.
std::map<std::string, std::vector<std::size_t>> ParamLists(std::span<const std::uint8_t> msb) {
    std::map<std::string, std::vector<std::size_t>> lists;
    std::int64_t at = 0x10;
    for (int guard = 0; at != 0 && guard < 64; ++guard) {
        std::int32_t version = 0, count = 0;
        std::int64_t name = 0;
        if (!Get(msb, std::size_t(at), version) || !Get(msb, std::size_t(at) + 4, count) ||
            !Get(msb, std::size_t(at) + 8, name) || count < 1 || count > 1'000'000) {
            break;
        }
        std::vector<std::size_t> records;
        std::int64_t next = 0;
        for (std::int32_t i = 0; i < count; ++i) {
            std::int64_t record = 0;
            if (!Get(msb, std::size_t(at) + 0x10 + std::size_t(i) * 8, record)) {
                return lists;
            }
            if (i + 1 == count) {
                next = record;
            } else {
                records.push_back(std::size_t(record));
            }
        }
        lists[Utf16At(msb, std::size_t(name))] = std::move(records);
        at = next;
    }
    return lists;
}

Vec3 Vec3At(std::span<const std::uint8_t> bytes, std::size_t at) {
    Vec3 v{};
    Get(bytes, at, v);
    return v;
}

/// Row vector times the rotation the game's map tools apply: x, then z, then y (degrees).
std::array<float, 9> RotationOf(const Vec3& degrees, const Vec3& scale) {
    const double rad = 3.14159265358979323846 / 180.0;
    const double cx = std::cos(degrees[0] * rad), sx = std::sin(degrees[0] * rad);
    const double cy = std::cos(degrees[1] * rad), sy = std::sin(degrees[1] * rad);
    const double cz = std::cos(degrees[2] * rad), sz = std::sin(degrees[2] * rad);
    const double rx[9] = {1, 0, 0, 0, cx, sx, 0, -sx, cx};
    const double rz[9] = {cz, sz, 0, -sz, cz, 0, 0, 0, 1};
    const double ry[9] = {cy, 0, -sy, 0, 1, 0, sy, 0, cy};
    const auto mul = [](const double* a, const double* b, double* out) {
        for (int r = 0; r < 3; ++r) {
            for (int c = 0; c < 3; ++c) {
                out[r * 3 + c] = a[r * 3] * b[c] + a[r * 3 + 1] * b[3 + c] + a[r * 3 + 2] * b[6 + c];
            }
        }
    };
    double xz[9], xzy[9];
    mul(rx, rz, xz);
    mul(xz, ry, xzy);
    std::array<float, 9> m{};
    for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 3; ++c) {
            m[r * 3 + c] = float(xzy[r * 3 + c] * scale[r]);
        }
    }
    return m;
}

float Percentile(std::vector<float> values, double share) {
    const std::size_t at = std::min(values.size() - 1, std::size_t(share * double(values.size() - 1) + 0.5));
    std::nth_element(values.begin(), values.begin() + std::ptrdiff_t(at), values.end());
    return values[at];
}

constexpr std::uint32_t WaterColor = 0xFF4E6672u;

bool SkippedMaterial(const std::string& mtd) {
    // Kaniel 2026-10-18 ("a lot of noise"): also the alpha-cut decals (_al: litter, grime, leaves on
    // the ground), grass cards and blood splats, specks from above.
    static constexpr std::string_view Skipped[] = {"Sky", "sky", "Moon", "Sun_", "_al", "grass", "Bloody"};
    return std::ranges::any_of(Skipped, [&](std::string_view word) { return mtd.find(word) != std::string::npos; });
}

float TriangleArea(const float* t) {
    const float ax = t[3] - t[0], ay = t[4] - t[1], az = t[5] - t[2];
    const float bx = t[6] - t[0], by = t[7] - t[1], bz = t[8] - t[2];
    const float cx = ay * bz - az * by, cy = az * bx - ax * bz, cz = ax * by - ay * bx;
    return 0.5f * std::sqrt(cx * cx + cy * cy + cz * cz);
}

} // namespace

std::vector<float> Floors(std::span<const float> triangles) {
    std::vector<float> floors;
    for (std::size_t t = 0; t + 9 <= triangles.size(); t += 9) {
        const float* v = triangles.data() + t;
        const float ax = v[3] - v[0], ay = v[4] - v[1], az = v[5] - v[2];
        const float bx = v[6] - v[0], by = v[7] - v[1], bz = v[8] - v[2];
        const float nx = ay * bz - az * by, ny = az * bx - ax * bz, nz = ax * by - ay * bx;
        const float length = std::sqrt(nx * nx + ny * ny + nz * nz);
        if (length > 0.0f && std::abs(ny) > 0.45f * length) {
            floors.insert(floors.end(), v, v + 9);
        }
    }
    return floors;
}

std::optional<std::vector<std::uint8_t>> ReadColorGrade(std::span<const std::uint8_t> tpf, const std::string& block) {
    const auto textures = BbGameArt::ParseTpf(tpf);
    if (!textures) {
        return std::nullopt;
    }
    const BbGameArt::TpfTexture* found = nullptr;
    for (const BbGameArt::TpfTexture& t : *textures) {
        if (t.name == block + "_CGrading0001" || (!found && t.name.starts_with(block.substr(0, 3)))) {
            found = &t;
        }
    }
    // 16 wide, 256 tall, RGBA8 (DXGI 28/29), 4 bytes a texel in 8x8 tiles of Morton order.
    if (!found || found->width != 16 || found->height != 256 || found->size < 16 * 256 * 4) {
        return std::nullopt;
    }
    std::vector<std::uint8_t> grade(16 * 16 * 16 * 3);
    std::size_t source = found->offset;
    for (int ty = 0; ty < 256 / 8; ++ty) {
        for (int tx = 0; tx < 16 / 8; ++tx) {
            for (int i = 0; i < 64; ++i, source += 4) {
                int x = 0, y = 0;
                for (int bit = 0; bit < 3; ++bit) {
                    x |= ((i >> (2 * bit)) & 1) << bit;
                    y |= ((i >> (2 * bit + 1)) & 1) << bit;
                }
                const int row = ty * 8 + y, column = tx * 8 + x; // row: blue slice * 16 + green
                std::memcpy(grade.data() + (std::size_t(row) * 16 + std::size_t(column)) * 3, tpf.data() + source, 3);
            }
        }
    }
    return grade;
}

std::array<float, 3> Graded(std::span<const std::uint8_t> grade, const float rgb[3]) {
    if (grade.size() < 16 * 16 * 16 * 3) {
        return {rgb[0], rgb[1], rgb[2]};
    }
    float f[3];
    int i0[3], i1[3];
    for (int c = 0; c < 3; ++c) {
        const float v = std::clamp(rgb[c], 0.0f, 255.0f) / 255.0f * 15.0f;
        i0[c] = std::min(14, int(v));
        i1[c] = i0[c] + 1;
        f[c] = v - float(i0[c]);
    }
    const auto at = [&](int r, int g, int b, int c) {
        return float(grade[(std::size_t(b) * 256 + std::size_t(g) * 16 + std::size_t(r)) * 3 + std::size_t(c)]);
    };
    std::array<float, 3> out{};
    for (int c = 0; c < 3; ++c) {
        float v = 0;
        for (int corner = 0; corner < 8; ++corner) {
            const int r = corner & 1 ? i1[0] : i0[0], g = corner & 2 ? i1[1] : i0[1], b = corner & 4 ? i1[2] : i0[2];
            const float w = (corner & 1 ? f[0] : 1 - f[0]) * (corner & 2 ? f[1] : 1 - f[1]) * (corner & 4 ? f[2] : 1 - f[2]);
            v += w * at(r, g, b, c);
        }
        out[std::size_t(c)] = v;
    }
    return out;
}

std::uint32_t AverageColor(std::span<const std::uint8_t> tpf) {
    const auto textures = BbGameArt::ParseTpf(tpf);
    if (!textures || textures->empty()) {
        return 0;
    }
    const BbGameArt::TpfTexture& texture = textures->front();
    // DXGI BC1 (71, 72 sRGB) and BC7 (98, 99 sRGB); the blocks in any order make the same average.
    const bool bc1 = texture.dxgi == 71 || texture.dxgi == 72, bc7 = texture.dxgi == 98 || texture.dxgi == 99;
    if (!bc1 && !bc7) {
        return 0;
    }
    const std::size_t block = bc1 ? 8 : 16;
    const std::size_t blocks = std::size_t(std::max(1, texture.width / 4)) * std::size_t(std::max(1, texture.height / 4));
    const std::size_t count = std::min<std::size_t>(blocks, texture.size / block);
    const std::size_t step = std::max<std::size_t>(1, count / 1024); // a thousand blocks at most
    double sum[3] = {}, weight = 0;
    for (std::size_t b = 0; b < count; b += step) {
        const std::uint8_t* at = tpf.data() + texture.offset + b * block;
        std::array<std::uint8_t, 64> pixels;
        if (bc1) {
            pixels = BbGameArt::DecodeBc1Block(std::span<const std::uint8_t, 8>(at, 8));
        } else {
            pixels = BbGameArt::DecodeBc7Block(std::span<const std::uint8_t, 16>(at, 16));
        }
        for (std::size_t p = 0; p < 64; p += 4) {
            const double a = pixels[p + 3] / 255.0;
            sum[0] += pixels[p] * a, sum[1] += pixels[p + 1] * a, sum[2] += pixels[p + 2] * a;
            weight += a;
        }
    }
    if (weight <= 0) {
        return 0;
    }
    const auto channel = [&](int c) { return std::uint32_t(std::clamp(sum[c] / weight, 0.0, 255.0)); };
    return 0xFF000000u | channel(0) << 16 | channel(1) << 8 | channel(2);
}

std::optional<Texture> DecodeTexture(std::span<const std::uint8_t> tpf, int max_side) {
    const auto textures = BbGameArt::ParseTpf(tpf);
    if (!textures || textures->empty()) {
        return std::nullopt;
    }
    const BbGameArt::TpfTexture& texture = textures->front();
    const bool bc1 = texture.dxgi == 71 || texture.dxgi == 72, bc7 = texture.dxgi == 98 || texture.dxgi == 99;
    if ((!bc1 && !bc7) || texture.width < 4 || texture.height < 4 || texture.width > 8192 || texture.height > 8192) {
        return std::nullopt;
    }
    const int block = bc1 ? 8 : 16, wb = texture.width / 4, hb = texture.height / 4;
    if (std::size_t(texture.offset) + std::size_t(texture.size) > tpf.size()) {
        return std::nullopt;
    }
    // The first level, in the PS4's tiles (8 x 8 blocks, padded).
    const std::size_t tiled = std::size_t((wb + 7) / 8 * 8) * std::size_t((hb + 7) / 8 * 8) * std::size_t(block);
    const auto blocks = BbGameArt::DeswizzleBlocks(tpf.subspan(texture.offset, std::min<std::size_t>(texture.size, tiled)),
                                                   wb, hb, block);
    std::vector<std::uint32_t> full(std::size_t(texture.width) * std::size_t(texture.height));
    for (int by = 0; by < hb; ++by) {
        for (int bx = 0; bx < wb; ++bx) {
            const std::uint8_t* at = blocks.data() + (std::size_t(by) * std::size_t(wb) + std::size_t(bx)) * std::size_t(block);
            const auto px = bc1 ? BbGameArt::DecodeBc1Block(std::span<const std::uint8_t, 8>(at, 8))
                                : BbGameArt::DecodeBc7Block(std::span<const std::uint8_t, 16>(at, 16));
            for (int i = 0; i < 16; ++i) {
                const std::uint8_t* p = px.data() + i * 4;
                full[std::size_t(by * 4 + i / 4) * std::size_t(texture.width) + std::size_t(bx * 4 + i % 4)] =
                    std::uint32_t(p[3]) << 24 | std::uint32_t(p[0]) << 16 | std::uint32_t(p[1]) << 8 | p[2];
            }
        }
    }
    Texture out{texture.width, texture.height, std::move(full), {}};
    // Halved (2 x 2 averages) down to max_side.
    while (out.width > max_side || out.height > max_side) {
        Texture half{std::max(1, out.width / 2), std::max(1, out.height / 2), {}, {}};
        half.pixels.resize(std::size_t(half.width) * std::size_t(half.height));
        for (int y = 0; y < half.height; ++y) {
            for (int x = 0; x < half.width; ++x) {
                std::uint32_t sum[4] = {};
                for (int k = 0; k < 4; ++k) {
                    const int sx = std::min(out.width - 1, x * 2 + (k & 1)), sy = std::min(out.height - 1, y * 2 + (k >> 1));
                    const std::uint32_t p = out.pixels[std::size_t(sy) * std::size_t(out.width) + std::size_t(sx)];
                    for (int c = 0; c < 4; ++c) {
                        sum[c] += (p >> (c * 8)) & 0xFF;
                    }
                }
                std::uint32_t p = 0;
                for (int c = 0; c < 4; ++c) {
                    p |= (sum[c] / 4) << (c * 8);
                }
                half.pixels[std::size_t(y) * std::size_t(half.width) + std::size_t(x)] = p;
            }
        }
        out = std::move(half);
    }
    // Its smaller levels, down to 4 pixels.
    Texture level = out;
    while (level.width > 4 && level.height > 4) {
        Texture half{level.width / 2, level.height / 2, {}, {}};
        half.pixels.resize(std::size_t(half.width) * std::size_t(half.height));
        for (int y = 0; y < half.height; ++y) {
            for (int x = 0; x < half.width; ++x) {
                std::uint32_t sum[4] = {};
                for (int k = 0; k < 4; ++k) {
                    const std::uint32_t p =
                        level.pixels[std::size_t(y * 2 + (k >> 1)) * std::size_t(level.width) + std::size_t(x * 2 + (k & 1))];
                    for (int c = 0; c < 4; ++c) {
                        sum[c] += (p >> (c * 8)) & 0xFF;
                    }
                }
                std::uint32_t p = 0;
                for (int c = 0; c < 4; ++c) {
                    p |= (sum[c] / 4) << (c * 8);
                }
                half.pixels[std::size_t(y) * std::size_t(half.width) + std::size_t(x)] = p;
            }
        }
        out.mips.push_back(half);
        level = std::move(half);
    }
    return out;
}

/// How bright the baked light is taken: the game's light maps hold a third of it (BB_MAP_LIGHT_GAIN
/// overrides; 0 leaves the light out).
float LightGain() {
    static const float gain = [] {
        const char* text = std::getenv("BB_MAP_LIGHT_GAIN");
        return text != nullptr ? float(std::atof(text)) : 3.0f;
    }();
    return gain;
}

std::uint32_t Texture::Sample(float u, float v, int level) const {
    const Texture& t = level <= 0 || mips.empty() ? *this : mips[std::size_t(std::min<int>(level, int(mips.size())) - 1)];
    if (t.width <= 0 || t.height <= 0) {
        return 0;
    }
    // Kaniel 2026-10-19 ("increase the quality of the render"): the four texels round the spot,
    // blended (wrapping at the edges).
    const float fx = (u - std::floor(u)) * float(t.width) - 0.5f, fy = (v - std::floor(v)) * float(t.height) - 0.5f;
    const int x0 = int(std::floor(fx)), y0 = int(std::floor(fy));
    const float ax = fx - float(x0), ay = fy - float(y0);
    const auto at = [&](int x, int y) {
        x = ((x % t.width) + t.width) % t.width;
        y = ((y % t.height) + t.height) % t.height;
        return t.pixels[std::size_t(y) * std::size_t(t.width) + std::size_t(x)];
    };
    const std::uint32_t p[4] = {at(x0, y0), at(x0 + 1, y0), at(x0, y0 + 1), at(x0 + 1, y0 + 1)};
    const float w[4] = {(1 - ax) * (1 - ay), ax * (1 - ay), (1 - ax) * ay, ax * ay};
    std::uint32_t out = 0;
    for (int c = 0; c < 4; ++c) {
        float sum = 0;
        for (int k = 0; k < 4; ++k) {
            sum += float((p[k] >> (c * 8)) & 0xFF) * w[k];
        }
        out |= std::uint32_t(std::clamp(sum + 0.5f, 0.0f, 255.0f)) << (c * 8);
    }
    return out;
}

int Texture::LevelFor(float texels) const {
    if (!(texels > 1.0f)) {
        return 0;
    }
    return std::min(int(mips.size()), int(std::log2(texels)));
}

std::vector<PointLight> ReadLights(std::span<const std::uint8_t> btl) {
    // The header: version (2), count at +8, the names' length at +0xC, a light's size at +0x14;
    // the names from +0x3C, then the lights.
    std::vector<PointLight> lights;
    std::int32_t version = 0, count = 0, names = 0, size = 0;
    if (!Get(btl, 0, version) || !Get(btl, 8, count) || !Get(btl, 0xC, names) || !Get(btl, 0x14, size) ||
        version != 2 || count < 0 || count > 10000 || names < 0 || size < 0x5C) {
        return lights;
    }
    const std::size_t first = 0x3C + std::size_t(names);
    for (std::int32_t i = 0; i < count; ++i) {
        const std::size_t at = first + std::size_t(i) * std::size_t(size);
        std::uint8_t rgb[3] = {};
        float power = 0, cone = 0, radius = 0;
        PointLight light;
        if (!Get(btl, at + 0x1D, rgb[0]) || !Get(btl, at + 0x1E, rgb[1]) || !Get(btl, at + 0x1F, rgb[2]) ||
            !Get(btl, at + 0x20, power) || !Get(btl, at + 0x2C, cone) || !Get(btl, at + 0x38, light.x) ||
            !Get(btl, at + 0x3C, light.y) || !Get(btl, at + 0x40, light.z) || !Get(btl, at + 0x58, radius)) {
            break;
        }
        if (cone != 0.0f || !(radius > 0.0f) || radius > 60.0f || !std::isfinite(light.x)) {
            continue; // a spotlight, or no reach
        }
        light.color = 0xFF000000u | std::uint32_t(rgb[0]) << 16 | std::uint32_t(rgb[1]) << 8 | rgb[2];
        light.radius = radius;
        light.power = power;
        lights.push_back(light);
    }
    return lights;
}

std::vector<ArchiveEntry> ReadArchiveHeader(std::span<const std::uint8_t> bhd) {
    std::vector<ArchiveEntry> entries;
    std::int32_t count = 0;
    std::int64_t entry_size = 0;
    if (bhd.size() < 0x40 || std::memcmp(bhd.data(), "BHF4", 4) != 0 || !Get(bhd, 0x0C, count) ||
        !Get(bhd, 0x20, entry_size) || entry_size < 0x24 || count < 0 || count > 1'000'000) {
        return entries;
    }
    for (std::int32_t i = 0; i < count; ++i) {
        const std::size_t at = 0x40 + std::size_t(i) * std::size_t(entry_size);
        std::int64_t size = 0;
        std::uint32_t offset = 0, name = 0;
        if (!Get(bhd, at + 8, size) || !Get(bhd, at + 0x18, offset) || !Get(bhd, at + 0x20, name)) {
            break;
        }
        // Names in UTF-16 when the header says so (+0x30), else in single bytes (a repacked
        // archive, as the rig's low-spec texture mod's).
        std::string text;
        if (bhd[0x30] != 0) {
            text = Utf16At(bhd, name);
        } else {
            for (std::size_t c = name; c < bhd.size() && bhd[c] != 0 && text.size() < 256; ++c) {
                text += char(bhd[c]);
            }
        }
        entries.push_back(ArchiveEntry{std::move(text), offset, std::uint64_t(size)});
    }
    return entries;
}

std::optional<MapStudio> ReadMapStudio(std::span<const std::uint8_t> msb) {
    if (msb.size() < 0x20 || std::memcmp(msb.data(), "MSB ", 4) != 0) {
        return std::nullopt;
    }
    const auto lists = ParamLists(msb);
    const auto models_it = lists.find("MODEL_PARAM_ST");
    const auto parts_it = lists.find("PARTS_PARAM_ST");
    if (models_it == lists.end() || parts_it == lists.end()) {
        return std::nullopt;
    }
    std::vector<std::string> models;
    for (const std::size_t record : models_it->second) {
        std::int64_t name = 0;
        Get(msb, record, name);
        models.push_back(Utf16At(msb, record + std::size_t(name)));
    }
    MapStudio studio;
    for (const std::size_t record : parts_it->second) {
        std::int64_t name_at = 0;
        std::int32_t model = -1;
        if (!Get(msb, record + 8, name_at) || !Get(msb, record + 0x1C, model)) {
            continue;
        }
        const std::string name = Utf16At(msb, record + std::size_t(name_at));
        const Vec3 position = Vec3At(msb, record + 0x28);
        if (name.starts_with('c')) {
            studio.spots.push_back(position);
            continue;
        }
        if (name.starts_with('h') && model >= 0 && std::size_t(model) < models.size() &&
            models[std::size_t(model)].starts_with('h')) {
            std::int64_t sub = 0;
            std::int16_t banner = -1;
            if (Get(msb, record + 0xB8, sub) && sub > 0) {
                Get(msb, record + std::size_t(sub) + 8, banner);
            }
            studio.collisions.push_back(MapStudio::Collision{
                Piece{models[std::size_t(model)], position, Vec3At(msb, record + 0x34), Vec3At(msb, record + 0x40)},
                banner == -1 ? 0 : std::abs(banner)});
            continue;
        }
        if (!name.starts_with('m') || model < 0 || std::size_t(model) >= models.size() ||
            !models[std::size_t(model)].starts_with('m')) {
            continue;
        }
        studio.pieces.push_back(Piece{models[std::size_t(model)], position, Vec3At(msb, record + 0x34),
                                      Vec3At(msb, record + 0x40)});
    }
    if (const auto points = lists.find("POINT_PARAM_ST"); points != lists.end()) {
        for (const std::size_t record : points->second) {
            studio.spots.push_back(Vec3At(msb, record + 0x14));
        }
    }
    return studio;
}

bool AppendTriangles(std::span<const std::uint8_t> flver, std::vector<float>& out, std::vector<std::uint32_t>* colors,
                     const MaterialColor& color_of, std::vector<float>* uvs) {
    if (flver.size() < 0x80 || std::memcmp(flver.data(), "FLVER\0L\0", 8) != 0) {
        return false;
    }
    std::uint32_t data = 0;
    std::int32_t dummies = 0, materials = 0, bones = 0, meshes = 0, buffers = 0, facesets = 0, layouts = 0;
    std::uint8_t header_index_size = 0;
    Get(flver, 0x0C, data);
    Get(flver, 0x14, dummies);
    Get(flver, 0x18, materials);
    Get(flver, 0x1C, bones);
    Get(flver, 0x20, meshes);
    Get(flver, 0x24, buffers);
    Get(flver, 0x48, header_index_size);
    Get(flver, 0x50, facesets);
    Get(flver, 0x54, layouts);
    if (dummies < 0 || materials < 0 || bones < 0 || meshes < 0 || buffers < 0 || facesets < 0 || layouts < 0) {
        return false;
    }
    const std::size_t material_base = 0x80 + std::size_t(dummies) * 0x40;
    const std::size_t mesh_base = material_base + std::size_t(materials) * 0x20 + std::size_t(bones) * 0x80;
    const std::size_t faceset_base = mesh_base + std::size_t(meshes) * 0x30;
    const std::size_t buffer_base = faceset_base + std::size_t(facesets) * 0x20;
    const std::size_t layout_base = buffer_base + std::size_t(buffers) * 0x20;
    const std::size_t texture_base = layout_base + std::size_t(layouts) * 0x10;
    // Each layout's position member: its offset in the vertex and whether it is a float3 or float4;
    // its first texture coordinates (semantic 5): offset and type (0x15/0x16 shorts, 1 floats).
    std::vector<std::int32_t> position_of(std::size_t(layouts), -1);
    // Up to two texture coordinate sets: (offset, type, which pair of a 0x16).
    struct UvSource {
        std::int32_t offset = -1;
        std::uint32_t type = 0;
        int pair = 0;
    };
    std::vector<std::array<UvSource, 2>> uv_sets(static_cast<std::size_t>(layouts));
    std::uint32_t version = 0;
    Get(flver, 0x08, version);
    const float uv_factor = version >= 0x2000F ? 2048.0f : 1024.0f;
    for (std::int32_t i = 0; i < layouts; ++i) {
        std::int32_t count = 0, members = 0;
        Get(flver, layout_base + std::size_t(i) * 0x10, count);
        Get(flver, layout_base + std::size_t(i) * 0x10 + 0xC, members);
        for (std::int32_t m = 0; m < count && m < 64; ++m) {
            const std::size_t at = std::size_t(members) + std::size_t(m) * 0x14;
            std::int32_t offset = 0;
            std::uint32_t type = 0, semantic = 0;
            Get(flver, at + 4, offset);
            Get(flver, at + 8, type);
            Get(flver, at + 0xC, semantic);
            if (semantic == 0 && (type == 2 || type == 3) && position_of[std::size_t(i)] < 0) {
                position_of[std::size_t(i)] = offset;
            }
            if (semantic == 5 && (type == 1 || type == 0x15 || type == 0x16)) {
                auto& sets = uv_sets[std::size_t(i)];
                for (int pair = 0; pair < (type == 0x16 ? 2 : 1); ++pair) {
                    for (UvSource& set : sets) {
                        if (set.offset < 0) {
                            set = UvSource{offset, type, pair};
                            break;
                        }
                    }
                }
            }
        }
    }
    std::vector<float> corners, corner_uvs;
    std::vector<std::uint32_t> corner_colors;
    std::map<std::int32_t, std::uint32_t> material_colors;
    // A material's colour: its first diffuse texture's (path "...\m24_01_trims_003_a.tif").
    const auto material_color = [&](std::int32_t material) -> std::uint32_t {
        if (!colors || !color_of || material < 0 || material >= materials) {
            return 0;
        }
        if (const auto known = material_colors.find(material); known != material_colors.end()) {
            return known->second;
        }
        std::int32_t count = 0, first = 0;
        Get(flver, material_base + std::size_t(material) * 0x20 + 8, count);
        Get(flver, material_base + std::size_t(material) * 0x20 + 0xC, first);
        std::uint32_t color = 0;
        std::string diffuse, diffuse2, mask, light;
        for (std::int32_t t = first; t < first + count && t >= 0 && t < first + 32; ++t) {
            std::int32_t path = 0, type = 0;
            Get(flver, texture_base + std::size_t(t) * 0x20, path);
            Get(flver, texture_base + std::size_t(t) * 0x20 + 4, type);
            const std::string kind = Utf16At(flver, std::size_t(type));
            std::string name = Utf16At(flver, std::size_t(path));
            name = name.substr(name.find_last_of("\\/") + 1);
            name = name.substr(0, name.find('.'));
            if (kind == "g_DiffuseTexture" || (diffuse.empty() && kind.find("Diffuse") != std::string::npos &&
                                               kind != "g_DiffuseTexture2")) {
                diffuse = diffuse.empty() ? name : diffuse;
            } else if (kind == "g_DiffuseTexture2") {
                diffuse2 = name;
            } else if (kind == "g_BlendMaskTexture") {
                mask = name;
            } else if (kind == "g_DOLTexture1") {
                light = name;
            }
        }
        if (!diffuse.empty()) {
            color = color_of(diffuse2.empty() && mask.empty() && light.empty()
                                 ? diffuse
                                 : diffuse + ";" + diffuse2 + ";" + mask + ";" + light);
        }
        material_colors[material] = color;
        return color;
    };
    for (std::int32_t mesh = 0; mesh < meshes; ++mesh) {
        const std::size_t at = mesh_base + std::size_t(mesh) * 0x30;
        std::int32_t material = 0, faceset_count = 0, faceset_list = 0, buffer_count = 0, buffer_list = 0;
        Get(flver, at + 4, material);
        Get(flver, at + 0x20, faceset_count);
        Get(flver, at + 0x24, faceset_list);
        Get(flver, at + 0x28, buffer_count);
        Get(flver, at + 0x2C, buffer_list);
        if (material >= 0 && material < materials) {
            std::int32_t mtd = 0;
            Get(flver, material_base + std::size_t(material) * 0x20 + 4, mtd);
            if (SkippedMaterial(Utf16At(flver, std::size_t(mtd)))) {
                continue;
            }
        }
        std::int32_t buffer = -1;
        if (buffer_count < 1 || !Get(flver, std::size_t(buffer_list), buffer) || buffer < 0 || buffer >= buffers) {
            continue;
        }
        std::int32_t layout = 0, vertex_size = 0, vertex_count = 0;
        std::uint32_t buffer_offset = 0;
        const std::size_t vb = buffer_base + std::size_t(buffer) * 0x20;
        Get(flver, vb + 4, layout);
        Get(flver, vb + 8, vertex_size);
        Get(flver, vb + 0xC, vertex_count);
        Get(flver, vb + 0x1C, buffer_offset);
        if (layout < 0 || layout >= layouts || position_of[std::size_t(layout)] < 0 || vertex_size <= 0 ||
            vertex_count <= 0) {
            continue;
        }
        // Kaniel 2026-10-18: water as a calm blue-grey surface (its textures are foam and ripples).
        std::uint32_t mesh_color = material_color(material);
        if (colors && material >= 0 && material < materials) {
            std::int32_t mtd = 0;
            Get(flver, material_base + std::size_t(material) * 0x20 + 4, mtd);
            const std::string name = Utf16At(flver, std::size_t(mtd));
            if (name.find("water") != std::string::npos || name.find("Water") != std::string::npos) {
                mesh_color = WaterColor;
            }
        }
        const std::size_t vertices = std::size_t(data) + buffer_offset + std::size_t(position_of[std::size_t(layout)]);
        if (vertices + std::size_t(vertex_count - 1) * std::size_t(vertex_size) + 12 > flver.size()) {
            continue;
        }
        const auto& sets = uv_sets[std::size_t(layout)];
        const auto corner = [&](std::uint32_t index) {
            const std::uint8_t* p = flver.data() + vertices + std::size_t(index) * std::size_t(vertex_size);
            float xyz[3];
            std::memcpy(xyz, p, 12);
            corners.insert(corners.end(), xyz, xyz + 3);
            if (uvs != nullptr) {
                float uv[4] = {0.0f, 0.0f, 0.0f, 0.0f};
                for (int k = 0; k < 2; ++k) {
                    const UvSource& set = sets[std::size_t(k)];
                    const std::size_t at = std::size_t(data) + buffer_offset + std::size_t(std::max(0, set.offset)) +
                                           std::size_t(index) * std::size_t(vertex_size);
                    if (set.offset < 0 || at + 8 > flver.size()) {
                        continue;
                    }
                    if (set.type == 1) {
                        std::memcpy(uv + k * 2, flver.data() + at, 8);
                    } else {
                        std::int16_t s[2];
                        std::memcpy(s, flver.data() + at + std::size_t(set.pair) * 4, 4);
                        uv[k * 2] = float(s[0]) / uv_factor;
                        uv[k * 2 + 1] = float(s[1]) / uv_factor;
                    }
                }
                corner_uvs.insert(corner_uvs.end(), uv, uv + 4);
            }
        };
        for (std::int32_t f = 0; f < faceset_count; ++f) {
            std::int32_t faceset = -1;
            if (!Get(flver, std::size_t(faceset_list) + std::size_t(f) * 4, faceset) || faceset < 0 ||
                faceset >= facesets) {
                continue;
            }
            const std::size_t fs = faceset_base + std::size_t(faceset) * 0x20;
            std::uint32_t flags = 0, index_offset = 0;
            std::uint8_t strip = 0;
            std::int32_t index_count = 0, index_size = 0;
            Get(flver, fs, flags);
            Get(flver, fs + 4, strip);
            Get(flver, fs + 8, index_count);
            Get(flver, fs + 0xC, index_offset);
            Get(flver, fs + 0x18, index_size);
            if ((flags & 0x03000000u) != 0 || index_count < 3) {
                continue; // the lower levels of detail
            }
            if (index_size == 0) {
                index_size = header_index_size != 0 ? header_index_size : 16;
            }
            const std::size_t width = index_size == 32 ? 4 : 2;
            const std::size_t base = std::size_t(data) + index_offset;
            if (base + std::size_t(index_count) * width > flver.size()) {
                continue;
            }
            const auto index_at = [&](std::int32_t i) -> std::uint32_t {
                if (width == 4) {
                    std::uint32_t v;
                    std::memcpy(&v, flver.data() + base + std::size_t(i) * 4, 4);
                    return v;
                }
                std::uint16_t v;
                std::memcpy(&v, flver.data() + base + std::size_t(i) * 2, 2);
                return v == 0xFFFF ? 0xFFFFFFFFu : v;
            };
            const auto triangle = [&](std::uint32_t a, std::uint32_t b, std::uint32_t c) {
                const auto n = std::uint32_t(vertex_count);
                if (a >= n || b >= n || c >= n || a == b || b == c || a == c) {
                    return;
                }
                corner(a);
                corner(b);
                corner(c);
                corner_colors.push_back(mesh_color);
            };
            if (strip) {
                for (std::int32_t i = 0; i + 2 < index_count; ++i) {
                    triangle(index_at(i), index_at(i + 1), index_at(i + 2));
                }
            } else {
                for (std::int32_t i = 0; i + 2 < index_count; i += 3) {
                    triangle(index_at(i), index_at(i + 1), index_at(i + 2));
                }
            }
        }
    }
    out.insert(out.end(), corners.begin(), corners.end());
    if (colors) {
        colors->insert(colors->end(), corner_colors.begin(), corner_colors.end());
    }
    if (uvs) {
        uvs->insert(uvs->end(), corner_uvs.begin(), corner_uvs.end());
    }
    return true;
}

bool SkippedModel(std::string_view model, std::span<const float> triangles) {
    if (model.size() < 2) {
        return true;
    }
    int number = 0;
    for (const char c : model.substr(1)) {
        if (c < '0' || c > '9') {
            return true;
        }
        number = number * 10 + (c - '0');
    }
    if (number >= 900000 || (number >= 8000 && number < 10000)) {
        return true;
    }
    const std::size_t count = triangles.size() / 9;
    if (count == 0) {
        return true;
    }
    // Kaniel 2026-10-18 ("a lot of noise"): small props (bottles, rubble, bodies, candles) are
    // specks from above; a model within a metre and a half each way is left out.
    {
        float lo[3] = {triangles[0], triangles[1], triangles[2]}, hi[3] = {lo[0], lo[1], lo[2]};
        for (std::size_t i = 0; i + 3 <= triangles.size(); i += 3) {
            for (int c = 0; c < 3; ++c) {
                lo[c] = std::min(lo[c], triangles[i + std::size_t(c)]);
                hi[c] = std::max(hi[c], triangles[i + std::size_t(c)]);
            }
        }
        if (hi[0] - lo[0] < 1.5f && hi[1] - lo[1] < 1.5f && hi[2] - lo[2] < 1.5f) {
            return true;
        }
    }
    if (count > 64) {
        // A backdrop under or round the whole area (the Dream's sea of flowers, the woods' far
        // hills): its triangles spread thin, under one in 20 square metres over 200,000.
        float x0 = triangles[0], x1 = x0, z0 = triangles[2], z1 = z0;
        for (std::size_t i = 0; i + 3 <= triangles.size(); i += 3) {
            x0 = std::min(x0, triangles[i]), x1 = std::max(x1, triangles[i]);
            z0 = std::min(z0, triangles[i + 2]), z1 = std::max(z1, triangles[i + 2]);
        }
        const float spread = (x1 - x0) * (z1 - z0);
        return spread > 200'000.0f && float(count) < spread * 0.05f;
    }
    for (std::size_t i = 0; i < count; ++i) {
        if (TriangleArea(triangles.data() + i * 9) > 100.0f) {
            return true;
        }
    }
    return false;
}

std::optional<PlayArea> PlayAreaOf(std::span<const Vec3> spots) {
    if (spots.empty()) {
        return std::nullopt;
    }
    std::vector<float> xs, ys, zs;
    for (const Vec3& s : spots) {
        xs.push_back(s[0]);
        ys.push_back(s[1]);
        zs.push_back(s[2]);
    }
    constexpr float Margin = 40.0f;
    return PlayArea{Percentile(xs, 0.01) - Margin, Percentile(xs, 0.99) + Margin, Percentile(zs, 0.01) - Margin,
                    Percentile(zs, 0.99) + Margin, Percentile(ys, 0.99) + 25.0f};
}

void Place(const Piece& piece, std::span<const float> model, const PlayArea& area, std::vector<float>& out,
           std::span<const std::uint32_t> colors, std::vector<std::uint32_t>* out_colors, std::span<const float> uvs,
           std::vector<float>* out_uvs) {
    const std::array<float, 9> m = RotationOf(piece.rotation, piece.scale);
    const Vec3& p = piece.position;
    float tri[9];
    for (std::size_t t = 0; t + 9 <= model.size(); t += 9) {
        for (int k = 0; k < 3; ++k) {
            const float* v = model.data() + t + std::size_t(k) * 3;
            tri[k * 3 + 0] = v[0] * m[0] + v[1] * m[3] + v[2] * m[6] + p[0];
            tri[k * 3 + 1] = v[0] * m[1] + v[1] * m[4] + v[2] * m[7] + p[1];
            tri[k * 3 + 2] = v[0] * m[2] + v[1] * m[5] + v[2] * m[8] + p[2];
        }
        const float cx = (tri[0] + tri[3] + tri[6]) / 3.0f, cz = (tri[2] + tri[5] + tri[8]) / 3.0f;
        const float low = std::min({tri[1], tri[4], tri[7]});
        if (cx < area.x0 || cx > area.x1 || cz < area.z0 || cz > area.z1 || low >= area.ceiling) {
            continue;
        }
        out.insert(out.end(), tri, tri + 9);
        if (out_colors) {
            out_colors->push_back(t / 9 < colors.size() ? colors[t / 9] : 0u);
        }
        if (out_uvs) {
            const std::size_t u = t / 9 * 12;
            for (std::size_t k = 0; k < 12; ++k) {
                out_uvs->push_back(u + k < uvs.size() ? uvs[u + k] : 0.0f);
            }
        }
    }
}

Canvas::Canvas(const PlayArea& area, float ppm_, int max_side) {
    const float w = std::max(1.0f, area.x1 - area.x0), h = std::max(1.0f, area.z1 - area.z0);
    ppm = std::min(ppm_, float(max_side) / std::max(w, h));
    width = std::max(1, int(std::ceil(w * ppm)));
    height = std::max(1, int(std::ceil(h * ppm)));
    x0 = area.x0;
    z1 = area.z1;
    top.assign(std::size_t(width) * std::size_t(height), -std::numeric_limits<float>::infinity());
    light.assign(top.size(), 0);
    color.assign(top.size(), 0);
    walk.assign(top.size(), 0);
    cells_w = (width + Chart::FloorCell - 1) / Chart::FloorCell;
    cells_h = (height + Chart::FloorCell - 1) / Chart::FloorCell;
    const std::size_t levels = std::size_t(cells_w) * std::size_t(cells_h) * Chart::Levels;
    level = std::make_shared<std::vector<float>>(levels, -std::numeric_limits<float>::infinity());
    level_top.assign(levels, Chart::NoHeight);
    level_light.assign(levels, 0);
    level_color.assign(levels, 0);
}

void Canvas::AddLevel(std::size_t cell, float h) {
    // A floor within 1.8 m of one known there is the same storey (a ramp, uneven ground); else a
    // storey of its own, kept highest first (the lowest goes when there are too many).
    float* l = level->data() + cell * Chart::Levels;
    for (int k = 0; k < Chart::Levels && std::isfinite(l[k]); ++k) {
        if (std::abs(l[k] - h) < 1.8f) {
            l[k] = std::max(l[k], h);
            return;
        }
    }
    int at = 0;
    while (at < Chart::Levels && std::isfinite(l[at]) && l[at] > h) {
        ++at;
    }
    if (at == Chart::Levels) {
        return;
    }
    for (int k = Chart::Levels - 1; k > at; --k) {
        l[k] = l[k - 1];
    }
    l[at] = h;
}

void Canvas::ShareFloors(const Canvas& other) {
    if (other.level->size() == level->size()) {
        level = other.level;
    }
}

void Canvas::AddWalkable(std::span<const float> floors) {
    has_walk = has_walk || !floors.empty();
    for (std::size_t t = 0; t + 9 <= floors.size(); t += 9) {
        const float* v = floors.data() + t;
        float px[3], py[3];
        for (int k = 0; k < 3; ++k) {
            px[k] = (v[k * 3] - x0) * ppm;
            py[k] = (z1 - v[k * 3 + 2]) * ppm;
        }
        const float high = std::max({v[1], v[4], v[7]});
        const int bx0 = std::max(0, int(std::floor(std::min({px[0], px[1], px[2]}))));
        const int by0 = std::max(0, int(std::floor(std::min({py[0], py[1], py[2]}))));
        const int bx1 = std::min(width - 1, int(std::floor(std::max({px[0], px[1], px[2]}))));
        const int by1 = std::min(height - 1, int(std::floor(std::max({py[0], py[1], py[2]}))));
        const float den = (py[1] - py[2]) * (px[0] - px[2]) + (px[2] - px[1]) * (py[0] - py[2]);
        for (int y = by0; y <= by1; ++y) {
            for (int x = bx0; x <= bx1; ++x) {
                bool inside = bx0 == bx1 || by0 == by1 || std::abs(den) < 1e-9f;
                bool strictly = inside; // the storeys take only what the floor covers
                float h = high;
                if (!inside) {
                    const float cx = float(x) + 0.5f, cy = float(y) + 0.5f;
                    const float l1 = ((py[1] - py[2]) * (cx - px[2]) + (px[2] - px[1]) * (cy - py[2])) / den;
                    const float l2 = ((py[2] - py[0]) * (cx - px[2]) + (px[0] - px[2]) * (cy - py[2])) / den;
                    inside = l1 >= -0.05f && l2 >= -0.05f && 1.0f - l1 - l2 >= -0.05f;
                    strictly = l1 >= -0.001f && l2 >= -0.001f && 1.0f - l1 - l2 >= -0.001f;
                    h = std::clamp(l1 * v[1] + l2 * v[4] + (1.0f - l1 - l2) * v[7], std::min({v[1], v[4], v[7]}), high);
                }
                if (inside) {
                    walk[std::size_t(y) * std::size_t(width) + std::size_t(x)] = 1;
                }
                if (strictly) {
                    AddLevel(std::size_t(y / Chart::FloorCell) * std::size_t(cells_w) + std::size_t(x / Chart::FloorCell),
                             h);
                }
            }
        }
    }
}

void Canvas::Add(std::span<const float> triangles, std::span<const std::uint32_t> colors, std::span<const float> uvs) {
    // The light from the north-west, high, as an old map's relief.
    const float lx = -0.45f, ly = 0.78f, lz = 0.43f;
    for (std::size_t t = 0; t + 9 <= triangles.size(); t += 9) {
        const float* v = triangles.data() + t;
        const float ax = v[3] - v[0], ay = v[4] - v[1], az = v[5] - v[2];
        const float bx = v[6] - v[0], by = v[7] - v[1], bz = v[8] - v[2];
        float nx = ay * bz - az * by, ny = az * bx - ax * bz, nz = ax * by - ay * bx;
        const float length = std::sqrt(nx * nx + ny * ny + nz * nz);
        if (length <= 0.0f) {
            continue;
        }
        if (ny < 0.0f) {
            nx = -nx, ny = -ny, nz = -nz;
        }
        const float lambert = std::clamp((nx * lx + ny * ly + nz * lz) / length, 0.0f, 1.0f);
        const auto shade = std::uint8_t(std::lround(lambert * 255.0f));
        std::uint32_t tint = t / 9 < colors.size() ? colors[t / 9] : 0u;
        // Kaniel 2026-10-19: a textured triangle takes its surface's pixel at each spot.
        std::shared_ptr<const Surface> surface;
        const float* uv = nullptr;
        if (IsTextured(tint)) {
            surface = texture_of ? texture_of(tint & 0xFFFFFFu) : nullptr;
            uv = t / 9 * 12 + 12 <= uvs.size() ? uvs.data() + t / 9 * 12 : nullptr;
            if (!surface || !surface->diffuse || !uv) {
                tint = 0;
                surface = nullptr;
            }
        }
        // Pixels of the corners (z north up).
        float px[3], py[3], h[3];
        for (int k = 0; k < 3; ++k) {
            px[k] = (v[k * 3] - x0) * ppm;
            py[k] = (z1 - v[k * 3 + 2]) * ppm;
            h[k] = v[k * 3 + 1];
        }
        const int bx0 = std::max(0, int(std::floor(std::min({px[0], px[1], px[2]}))));
        const int by0 = std::max(0, int(std::floor(std::min({py[0], py[1], py[2]}))));
        const int bx1 = std::min(width - 1, int(std::floor(std::max({px[0], px[1], px[2]}))));
        const int by1 = std::min(height - 1, int(std::floor(std::max({py[0], py[1], py[2]}))));
        if (bx0 > bx1 || by0 > by1) {
            continue;
        }
        // Each texture's level for this triangle: as many texels a pixel as its coordinates span.
        int diffuse_level = 0, level2 = 0, light_level = 0;
        if (surface) {
            const float pixels = std::max(0.5f, std::abs((px[1] - px[0]) * (py[2] - py[0]) - (px[2] - px[0]) * (py[1] - py[0])));
            const auto span = [&](int k, const std::shared_ptr<const Texture>& tex) {
                if (!tex) {
                    return 0;
                }
                const float* a = uv + 0 * 4 + k * 2;
                const float* b = uv + 1 * 4 + k * 2;
                const float* c = uv + 2 * 4 + k * 2;
                const float area = std::abs((b[0] - a[0]) * (c[1] - a[1]) - (c[0] - a[0]) * (b[1] - a[1])) *
                                   float(tex->width) * float(tex->height);
                return tex->LevelFor(std::sqrt(area / pixels));
            };
            diffuse_level = span(0, surface->diffuse);
            level2 = span(0, surface->diffuse2);
            light_level = span(1, surface->light);
        }
        const auto plot = [&](int x, int y, float z, float l1 = 1.0f / 3, float l2 = 1.0f / 3) {
            const std::size_t at = std::size_t(y) * std::size_t(width) + std::size_t(x);
            std::uint32_t c = tint;
            if (surface) {
                const float l3 = 1.0f - l1 - l2;
                const float u0 = l1 * uv[0] + l2 * uv[4] + l3 * uv[8], v0 = l1 * uv[1] + l2 * uv[5] + l3 * uv[9];
                c = surface->diffuse->Sample(u0, v0, diffuse_level);
                if ((c >> 24) < 96) {
                    return; // cut out (leaves, railings): what lies under shows
                }
                float rgb[3] = {float((c >> 16) & 0xFF), float((c >> 8) & 0xFF), float(c & 0xFF)};
                // The second texture where its mask says (moss, mud, damage).
                if (surface->diffuse2 && surface->mask) {
                    const float m = float((surface->mask->Sample(u0, v0, diffuse_level) >> 16) & 0xFF) / 255.0f;
                    const std::uint32_t d = surface->diffuse2->Sample(u0, v0, level2);
                    const float other[3] = {float((d >> 16) & 0xFF), float((d >> 8) & 0xFF), float(d & 0xFF)};
                    for (int k = 0; k < 3; ++k) {
                        rgb[k] += (other[k] - rgb[k]) * m;
                    }
                }
                // The light the game baked for the model.
                if (surface->light) {
                    const float u1 = l1 * uv[2] + l2 * uv[6] + l3 * uv[10], v1 = l1 * uv[3] + l2 * uv[7] + l3 * uv[11];
                    const std::uint32_t lit = surface->light->Sample(u1, v1, light_level);
                    const float gain = LightGain();
                    rgb[0] *= float((lit >> 16) & 0xFF) / 255.0f * gain;
                    rgb[1] *= float((lit >> 8) & 0xFF) / 255.0f * gain;
                    rgb[2] *= float(lit & 0xFF) / 255.0f * gain;
                }
                c = 0xFF000000u | std::uint32_t(std::clamp(rgb[0], 0.0f, 255.0f)) << 16 |
                    std::uint32_t(std::clamp(rgb[1], 0.0f, 255.0f)) << 8 | std::uint32_t(std::clamp(rgb[2], 0.0f, 255.0f));
            }
            if (z > top[at]) {
                top[at] = z;
                light[at] = shade;
                color[at] = c;
            }
            // The floor's own surface (and what stands on it) under each storey: its colour for
            // the floor view.
            const std::size_t cell =
                (std::size_t(y / Chart::FloorCell) * std::size_t(cells_w) + std::size_t(x / Chart::FloorCell)) *
                Chart::Levels;
            const std::vector<float>& floors = *level;
            for (int k = 0; k < Chart::Levels && std::isfinite(floors[cell + std::size_t(k)]); ++k) {
                const std::size_t l = cell + std::size_t(k);
                const auto dm = std::int16_t(std::clamp(z * 10.0f, -32767.0f, 32767.0f));
                if (z >= floors[l] - 0.4f && z <= floors[l] + 1.2f && dm > level_top[l]) {
                    level_top[l] = dm;
                    level_light[l] = shade;
                    level_color[l] = c;
                }
            }
        };
        if (bx0 == bx1 && by0 == by1) {
            plot(bx0, by0, std::max({h[0], h[1], h[2]})); // under a pixel: its top
            continue;
        }
        const float den = (py[1] - py[2]) * (px[0] - px[2]) + (px[2] - px[1]) * (py[0] - py[2]);
        if (std::abs(den) < 1e-9f) {
            // Edge-on from above (a wall): its line, at its top.
            const float z = std::max({h[0], h[1], h[2]});
            for (int k = 0; k < 3; ++k) {
                const int j = (k + 1) % 3;
                const int steps = int(std::max(std::abs(px[j] - px[k]), std::abs(py[j] - py[k]))) + 1;
                for (int s = 0; s <= steps; ++s) {
                    const float f = float(s) / float(steps);
                    const int x = int(px[k] + (px[j] - px[k]) * f), y = int(py[k] + (py[j] - py[k]) * f);
                    if (x >= 0 && y >= 0 && x < width && y < height) {
                        plot(x, y, z);
                    }
                }
            }
            continue;
        }
        const float inv = 1.0f / den;
        for (int y = by0; y <= by1; ++y) {
            const float cy = float(y) + 0.5f;
            for (int x = bx0; x <= bx1; ++x) {
                const float cx = float(x) + 0.5f;
                const float l1 = ((py[1] - py[2]) * (cx - px[2]) + (px[2] - px[1]) * (cy - py[2])) * inv;
                const float l2 = ((py[2] - py[0]) * (cx - px[2]) + (px[0] - px[2]) * (cy - py[2])) * inv;
                const float l3 = 1.0f - l1 - l2;
                constexpr float Slack = -0.02f;
                if (l1 < Slack || l2 < Slack || l3 < Slack) {
                    continue;
                }
                plot(x, y, l1 * h[0] + l2 * h[1] + l3 * h[2], l1, l2);
            }
        }
    }
}

void Canvas::Merge(const Canvas& other) {
    if (other.width != width || other.height != height) {
        return;
    }
    for (std::size_t i = 0; i < top.size(); ++i) {
        if (other.top[i] > top[i]) {
            top[i] = other.top[i];
            light[i] = other.light[i];
            color[i] = other.color[i];
        }
        walk[i] = walk[i] | other.walk[i];
    }
    has_walk = has_walk || other.has_walk;
    if (level != other.level && level->size() == other.level->size()) {
        for (std::size_t l = 0; l < level->size(); ++l) {
            if (!std::isfinite((*level)[l]) && std::isfinite((*other.level)[l])) {
                (*level)[l] = (*other.level)[l];
            }
        }
    }
    for (std::size_t l = 0; l < level_top.size() && l < other.level_top.size(); ++l) {
        if (other.level_top[l] > level_top[l]) {
            level_top[l] = other.level_top[l];
            level_light[l] = other.level_light[l];
            level_color[l] = other.level_color[l];
        }
    }
}

Chart Canvas::Finish() const {
    Chart chart;
    chart.width = width;
    chart.height = height;
    chart.x0 = x0;
    chart.z1 = z1;
    chart.ppm = ppm;
    chart.rgba.assign(std::size_t(width) * std::size_t(height) * 4, 0);
    std::vector<float> heights;
    heights.reserve(top.size() / 4);
    for (std::size_t i = 0; i < top.size(); i += 4) {
        if (std::isfinite(top[i])) {
            heights.push_back(top[i]);
        }
    }
    if (heights.empty()) {
        return chart;
    }
    const float low = Percentile(heights, 0.02), high = Percentile(heights, 0.98);
    const float span = std::max(1.0f, high - low);
    // Kaniel 2026-10-18 ("a lot of noise"): each drawn pixel takes the median light and the most
    // common colour of the 5x5 pixels round it on its own surface (within 0.75 m of its height), so
    // grime and cobble specks blend while walls and roof edges stay sharp; a pixel with few such
    // neighbours is a speck itself (a bottle, a stray leaf) and takes its whole window's median.
    std::vector<float> smooth_top = top;
    std::vector<std::uint8_t> smooth_light = light;
    std::vector<std::uint32_t> smooth_color = color;
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const std::size_t at = std::size_t(y) * std::size_t(width) + std::size_t(x);
            if (!std::isfinite(top[at])) {
                continue;
            }
            float hs[25];
            std::uint8_t ls[25];
            std::uint32_t cs[25];
            int n = 0;
            const auto gather = [&](bool same_surface) {
                n = 0;
                for (int dy = -2; dy <= 2; ++dy) {
                    for (int dx = -2; dx <= 2; ++dx) {
                        const int nx = x + dx, ny = y + dy;
                        if (nx < 0 || ny < 0 || nx >= width || ny >= height) {
                            continue;
                        }
                        const std::size_t o = std::size_t(ny) * std::size_t(width) + std::size_t(nx);
                        if (std::isfinite(top[o]) && (!same_surface || std::abs(top[o] - top[at]) < 0.75f)) {
                            hs[n] = top[o], ls[n] = light[o], cs[n] = color[o];
                            ++n;
                        }
                    }
                }
            };
            gather(true);
            if (n < 8) {
                gather(false);
            }
            std::nth_element(hs, hs + n / 2, hs + n);
            std::nth_element(ls, ls + n / 2, ls + n);
            smooth_top[at] = hs[n / 2];
            smooth_light[at] = ls[n / 2];
            int best = 0;
            for (int i = 0; i < n && !textured; ++i) {
                const int votes = int(std::count(cs, cs + n, cs[i]));
                if (votes > best) {
                    best = votes;
                    smooth_color[at] = cs[i];
                }
            }
        }
    }
    // Kaniel 2026-10-18 ("a lot of extra stuff outside the map"): with the walkable floors known,
    // what lies more than Reach metres from all of them goes (two passes of a distance sweep).
    std::vector<std::uint8_t> kept(top.size(), 1);
    if (has_walk) {
        const int reach = std::max(1, int(Reach * ppm));
        std::vector<int> far(top.size(), 1 << 20);
        for (std::size_t i = 0; i < top.size(); ++i) {
            if (walk[i]) {
                far[i] = 0;
            }
        }
        for (int y = 0; y < height; ++y) {
            for (int x = 0; x < width; ++x) {
                int& d = far[std::size_t(y) * std::size_t(width) + std::size_t(x)];
                if (x > 0) d = std::min(d, far[std::size_t(y) * std::size_t(width) + std::size_t(x - 1)] + 1);
                if (y > 0) d = std::min(d, far[std::size_t(y - 1) * std::size_t(width) + std::size_t(x)] + 1);
            }
        }
        for (int y = height - 1; y >= 0; --y) {
            for (int x = width - 1; x >= 0; --x) {
                int& d = far[std::size_t(y) * std::size_t(width) + std::size_t(x)];
                if (x + 1 < width) d = std::min(d, far[std::size_t(y) * std::size_t(width) + std::size_t(x + 1)] + 1);
                if (y + 1 < height) d = std::min(d, far[std::size_t(y + 1) * std::size_t(width) + std::size_t(x)] + 1);
            }
        }
        for (std::size_t i = 0; i < top.size(); ++i) {
            kept[i] = far[i] <= reach ? 1 : 0;
        }
    }
    // Kaniel 2026-10-18 (shadows): the light from the north-west, 40 degrees up; a pixel with a
    // surface rising above that line within 15 m toward the light lies in its shadow.
    const float rise = std::tan(40.0f * 3.14159265f / 180.0f) / ppm * 1.41421356f; // metres a diagonal pixel
    const int shadow_steps = int(15.0f * ppm / 1.41421356f);
    const auto in_shadow = [&](int x, int y, float h) {
        for (int k = 1; k <= shadow_steps; ++k) {
            const int sx = x - k, sy = y - k;
            if (sx < 0 || sy < 0) {
                return false;
            }
            const float other = smooth_top[std::size_t(sy) * std::size_t(width) + std::size_t(sx)];
            if (std::isfinite(other) && other - h > float(k) * rise) {
                return true;
            }
        }
        return false;
    };
    const auto height_at = [&](int x, int y) {
        x = std::clamp(x, 0, width - 1);
        y = std::clamp(y, 0, height - 1);
        const float h = smooth_top[std::size_t(y) * std::size_t(width) + std::size_t(x)];
        return std::isfinite(h) ? h : low - 8.0f;
    };
    // Ink on paper: low ground dark umber, high ground pale parchment, the light's relief over it,
    // ink lines where the height drops away (walls, roof edges, the shore of the void).
    const float paper[3] = {214.0f, 198.0f, 162.0f}, umber[3] = {84.0f, 70.0f, 54.0f}, ink[3] = {38.0f, 29.0f, 22.0f};
    // Kaniel 2026-10-19 (torches and lamps glowing): each warm, small light's glow on the
    // surfaces within its reach and not far above it, fading with distance.
    std::vector<float> glow;
    for (const PointLight& light : lights) {
        const float r = ((light.color >> 16) & 0xFF) / 255.0f, g = ((light.color >> 8) & 0xFF) / 255.0f,
                    b = (light.color & 0xFF) / 255.0f;
        if (r < g || g < b || r - b < 0.15f || light.radius > 20.0f) {
            continue; // not a flame's colour, or a wide fill light
        }
        if (glow.empty()) {
            glow.assign(top.size() * 3, 0.0f);
        }
        const float reach = light.radius * 1.2f;
        const float strength = std::clamp(light.power, 1.0f, 4.0f) * 60.0f;
        const int cx = int((light.x - x0) * ppm), cy = int((z1 - light.z) * ppm), rp = int(reach * ppm) + 1;
        for (int y = std::max(0, cy - rp); y <= std::min(height - 1, cy + rp); ++y) {
            for (int x = std::max(0, cx - rp); x <= std::min(width - 1, cx + rp); ++x) {
                const std::size_t at = std::size_t(y) * std::size_t(width) + std::size_t(x);
                if (!std::isfinite(top[at]) || top[at] > light.y + 1.5f) {
                    continue;
                }
                const float dx = (float(x) + 0.5f) / ppm + x0 - light.x, dz = z1 - (float(y) + 0.5f) / ppm - light.z;
                const float dy = (top[at] - light.y) * 0.3f; // a torch on a wall lights the ground below it
                const float d = std::sqrt(dx * dx + dy * dy + dz * dz);
                if (d >= reach) {
                    continue;
                }
                const float f = (1.0f - d / reach) * (1.0f - d / reach) * strength;
                glow[at * 3] += f * r;
                glow[at * 3 + 1] += f * g;
                glow[at * 3 + 2] += f * b;
            }
        }
    }
    // A surface's colour at height h: its light, its material, lit or in shadow, graded.
    const auto colour = [&](float h, std::uint8_t light_at, std::uint32_t tint, float shade, float lit[3]) {
        const float t = std::clamp((h - low) / span, 0.0f, 1.0f);
        const float relief = 0.50f + 0.62f * float(light_at) / 255.0f;
        float base[3];
        for (int c = 0; c < 3; ++c) {
            base[c] = umber[c] + (paper[c] - umber[c]) * t;
        }
        if (tint != 0 && textured) {
            // Kaniel 2026-10-19 ("look almost like in game"): the texture's own pixel, a little
            // brighter (the game lights it), only a touch of the height's tone.
            const float rgb[3] = {float((tint >> 16) & 0xFF), float((tint >> 8) & 0xFF), float(tint & 0xFF)};
            for (int c = 0; c < 3; ++c) {
                base[c] = std::clamp(rgb[c] * (1.15f + 0.25f * t), 0.0f, 255.0f);
            }
        } else if (tint != 0) {
            // Kaniel 2026-10-17 ("can we make it colored?"): the material's own colour,
            // livelier than the dark textures and as bright as the height says, a little of
            // the paper's tone kept.
            float rgb[3] = {float((tint >> 16) & 0xFF), float((tint >> 8) & 0xFF), float(tint & 0xFF)};
            const float lum = std::max(1.0f, 0.299f * rgb[0] + 0.587f * rgb[1] + 0.114f * rgb[2]);
            const float want = 95.0f + 120.0f * t;
            for (int c = 0; c < 3; ++c) {
                const float vivid = lum + (rgb[c] - lum) * 1.25f;
                const float paper_tone = (umber[c] + (paper[c] - umber[c]) * t);
                base[c] = std::clamp(vivid * want / lum, 0.0f, 255.0f) * 0.8f + paper_tone * 0.2f;
            }
        }
        for (int c = 0; c < 3; ++c) {
            lit[c] = std::clamp(base[c] * relief * shade, 0.0f, 255.0f);
        }
        // Kaniel 2026-10-18 ("the location feels in different colours"): the area's own grade.
        if (!grade.empty()) {
            // The table expects the lit, tone-mapped scene, darker than these flat colours.
            const float dim[3] = {lit[0] * 0.8f, lit[1] * 0.8f, lit[2] * 0.8f};
            const auto g = Graded(grade, dim);
            for (int c = 0; c < 3; ++c) {
                lit[c] = lit[c] * 0.35f + g[std::size_t(c)] * 0.65f;
            }
        }
    };
    // ppm-independent: a metre and a half of drop inks fully over 2 m.
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const std::size_t at = std::size_t(y) * std::size_t(width) + std::size_t(x);
            if (!std::isfinite(top[at]) || !kept[at]) {
                continue;
            }
            const float h = smooth_top[at];
            const float drop = std::max({std::abs(height_at(x + 1, y) - height_at(x - 1, y)),
                                         std::abs(height_at(x, y + 1) - height_at(x, y - 1))});
            const float line = std::clamp((drop - 2.0f) / 2.0f, 0.0f, 1.0f) * 0.55f;
            // A faint grain, as paper has (a hash of the pixel: the same picture every time).
            std::uint32_t hash = std::uint32_t(x) * 73856093u ^ std::uint32_t(y) * 19349663u;
            hash = (hash ^ (hash >> 13)) * 0x5bd1e995u;
            const float grain = float((hash >> 24) & 0xFF) / 255.0f * 3.0f - 1.5f;
            std::uint8_t* px = chart.rgba.data() + at * 4;
            float lit[3];
            colour(h, smooth_light[at], smooth_color[at], !oblique && in_shadow(x, y, h) ? 0.72f : 1.0f, lit);
            if (!glow.empty()) {
                const float* g = glow.data() + at * 3;
                for (int c = 0; c < 3; ++c) {
                    lit[c] = std::min(255.0f, lit[c] + g[c]);
                }
            }
            for (int c = 0; c < 3; ++c) {
                float v = lit[c] + grain;
                v = v * (1.0f - line) + ink[c] * line;
                px[c] = std::uint8_t(std::clamp(v, 0.0f, 255.0f));
            }
            px[3] = 255;
        }
    }
    // Kaniel 2026-10-18 (floors): each cell's top and its storeys' floors in their colours.
    const auto decimetres = [](float h) {
        return std::int16_t(std::clamp(std::lround(h * 10.0f), -32767L, 32767L));
    };
    Chart::Storeys& storeys = chart.storeys;
    storeys.width = cells_w;
    storeys.height = cells_h;
    const std::size_t cells = std::size_t(cells_w) * std::size_t(cells_h);
    storeys.roof.assign(cells, Chart::NoHeight);
    storeys.level.assign(cells * Chart::Levels, Chart::NoHeight);
    storeys.rgb.assign(cells * Chart::Levels, 0);
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const std::size_t at = std::size_t(y) * std::size_t(width) + std::size_t(x);
            if (std::isfinite(top[at]) && kept[at]) {
                std::int16_t& roof = storeys.roof[std::size_t(y / Chart::FloorCell) * std::size_t(cells_w) +
                                                  std::size_t(x / Chart::FloorCell)];
                roof = std::max(roof, decimetres(smooth_top[at]));
            }
        }
    }
    for (std::size_t cell = 0; cell < cells; ++cell) {
        if (storeys.roof[cell] == Chart::NoHeight) {
            continue; // nothing drawn there (or cut away)
        }
        for (int k = 0; k < Chart::Levels; ++k) {
            const std::size_t l = cell * Chart::Levels + std::size_t(k);
            const float floor = (*level)[l];
            if (!std::isfinite(floor)) {
                break;
            }
            float lit[3];
            const bool seen = level_top[l] != Chart::NoHeight;
            colour(floor, seen ? level_light[l] : 200, seen ? level_color[l] : 0u, 0.92f, lit);
            storeys.level[l] = decimetres(floor);
            storeys.rgb[l] = 0xFF000000u | (std::uint32_t(lit[0]) << 16) | (std::uint32_t(lit[1]) << 8) |
                             std::uint32_t(lit[2]);
        }
    }
    return Cropped(chart);
}

Chart Cropped(const Chart& chart) {
    int cx0 = chart.width, cy0 = chart.height, cx1 = -1, cy1 = -1;
    for (int y = 0; y < chart.height; ++y) {
        for (int x = 0; x < chart.width; ++x) {
            if (chart.rgba[(std::size_t(y) * std::size_t(chart.width) + std::size_t(x)) * 4 + 3] != 0) {
                cx0 = std::min(cx0, x), cx1 = std::max(cx1, x), cy0 = std::min(cy0, y), cy1 = std::max(cy1, y);
            }
        }
    }
    if (cx1 < 0) {
        return chart;
    }
    constexpr int Margin = 6;
    cx0 = std::max(0, cx0 - Margin), cy0 = std::max(0, cy0 - Margin);
    cx1 = std::min(chart.width - 1, cx1 + Margin), cy1 = std::min(chart.height - 1, cy1 + Margin);
    // On the storeys' cells, so they crop with it.
    cx0 -= cx0 % Chart::FloorCell, cy0 -= cy0 % Chart::FloorCell;
    Chart out;
    out.labels = chart.labels;
    out.width = cx1 - cx0 + 1;
    out.height = cy1 - cy0 + 1;
    out.ppm = chart.ppm;
    out.x0 = chart.x0 + float(cx0) / chart.ppm;
    out.z1 = chart.z1 - float(cy0) / chart.ppm;
    out.rgba.resize(std::size_t(out.width) * std::size_t(out.height) * 4);
    for (int y = 0; y < out.height; ++y) {
        std::memcpy(out.rgba.data() + std::size_t(y) * std::size_t(out.width) * 4,
                    chart.rgba.data() + ((std::size_t(y + cy0) * std::size_t(chart.width)) + std::size_t(cx0)) * 4,
                    std::size_t(out.width) * 4);
    }
    const Chart::Storeys& from = chart.storeys;
    if (from.width > 0) {
        Chart::Storeys& to = out.storeys;
        const int ox = cx0 / Chart::FloorCell, oy = cy0 / Chart::FloorCell;
        to.width = std::min(from.width - ox, (out.width + Chart::FloorCell - 1) / Chart::FloorCell);
        to.height = std::min(from.height - oy, (out.height + Chart::FloorCell - 1) / Chart::FloorCell);
        for (int y = 0; y < to.height; ++y) {
            const std::size_t row = std::size_t(y + oy) * std::size_t(from.width) + std::size_t(ox);
            to.roof.insert(to.roof.end(), from.roof.begin() + std::ptrdiff_t(row),
                           from.roof.begin() + std::ptrdiff_t(row + std::size_t(to.width)));
            to.level.insert(to.level.end(), from.level.begin() + std::ptrdiff_t(row * Chart::Levels),
                            from.level.begin() + std::ptrdiff_t((row + std::size_t(to.width)) * Chart::Levels));
            to.rgb.insert(to.rgb.end(), from.rgb.begin() + std::ptrdiff_t(row * Chart::Levels),
                          from.rgb.begin() + std::ptrdiff_t((row + std::size_t(to.width)) * Chart::Levels));
        }
    }
    return out;
}

namespace {
/// The storeys' cell of the pixel (px, py); -1 outside them.
std::ptrdiff_t CellOf(const Chart& chart, int px, int py) {
    const Chart::Storeys& s = chart.storeys;
    if (px < 0 || py < 0) {
        return -1;
    }
    const int cx = px / Chart::FloorCell, cy = py / Chart::FloorCell;
    return cx < s.width && cy < s.height ? std::ptrdiff_t(cy) * s.width + cx : -1;
}
} // namespace

bool UnderRoof(const Chart& chart, float x, float y, float z) {
    // Most of the 3 x 3 cells round the spot covered: a lamp post or a sign overhead is not a roof.
    const auto [px, py] = chart.PixelOf(x, z);
    const int feet = int(std::lround((y + 3.0f) * 10.0f));
    int covered = 0;
    for (int dy = -1; dy <= 1; ++dy) {
        for (int dx = -1; dx <= 1; ++dx) {
            const std::ptrdiff_t cell =
                CellOf(chart, int(std::floor(px)) + dx * Chart::FloorCell, int(std::floor(py)) + dy * Chart::FloorCell);
            if (cell >= 0 && chart.storeys.roof[std::size_t(cell)] != Chart::NoHeight &&
                chart.storeys.roof[std::size_t(cell)] > feet) {
                ++covered;
            }
        }
    }
    return covered >= 5;
}

std::int16_t ShownHeight(const Chart& chart, std::size_t cell, float y) {
    const Chart::Storeys& s = chart.storeys;
    const std::int16_t roof = s.roof[cell];
    if (std::isnan(y) || roof == Chart::NoHeight) {
        return roof;
    }
    const int at_most = int(std::lround((y + 1.5f) * 10.0f));
    for (int k = 0; k < Chart::Levels; ++k) {
        const std::int16_t l = s.level[cell * Chart::Levels + std::size_t(k)];
        if (l == Chart::NoHeight) {
            break;
        }
        if (l <= at_most) {
            return roof > l + 25 ? l : roof;
        }
    }
    return roof;
}

void FloorView(const Chart& chart, float y, std::uint8_t* rgba) {
    const Chart::Storeys& s = chart.storeys;
    if (s.width <= 0) {
        return;
    }
    const std::size_t cells = std::size_t(s.width) * std::size_t(s.height);
    const int at_most = int(std::lround((y + 1.5f) * 10.0f));
    // Each cell's storey (its highest floor up to 1.5 m above the feet), and whether a roof covers it.
    std::vector<std::int16_t> storey(cells, Chart::NoHeight);
    std::vector<std::int8_t> shown(cells, -1);
    for (std::size_t c = 0; c < cells; ++c) {
        for (int k = 0; k < Chart::Levels; ++k) {
            const std::int16_t l = s.level[c * Chart::Levels + std::size_t(k)];
            if (l == Chart::NoHeight) {
                break;
            }
            if (l <= at_most) {
                storey[c] = l;
                if (s.roof[c] != Chart::NoHeight && s.roof[c] > l + 25) {
                    shown[c] = std::int8_t(k);
                }
                break;
            }
        }
    }
    const auto same = [&](std::size_t c, int cx, int cy) {
        if (cx < 0 || cy < 0 || cx >= s.width || cy >= s.height) {
            return false;
        }
        const std::int16_t other = storey[std::size_t(cy) * std::size_t(s.width) + std::size_t(cx)];
        return other != Chart::NoHeight && std::abs(int(other) - int(storey[c])) <= 12;
    };
    const float ink[3] = {38.0f, 29.0f, 22.0f};
    for (int py = 0; py < chart.height; ++py) {
        for (int px = 0; px < chart.width; ++px) {
            const std::ptrdiff_t cell = CellOf(chart, px, py);
            if (cell < 0 || shown[std::size_t(cell)] < 0) {
                continue;
            }
            const std::size_t c = std::size_t(cell);
            const std::uint32_t rgb = s.rgb[c * Chart::Levels + std::size_t(shown[c])];
            // Where the storey ends (a wall, a drop): the ink line on the cell's side that faces it.
            const int cx = px / Chart::FloorCell, cy = py / Chart::FloorCell;
            const int sx = px % Chart::FloorCell == 0 ? -1 : (px % Chart::FloorCell == Chart::FloorCell - 1 ? 1 : 0);
            const int sy = py % Chart::FloorCell == 0 ? -1 : (py % Chart::FloorCell == Chart::FloorCell - 1 ? 1 : 0);
            const bool edge = (sx != 0 && !same(c, cx + sx, cy)) || (sy != 0 && !same(c, cx, cy + sy));
            std::uint8_t* out = rgba + (std::size_t(py) * std::size_t(chart.width) + std::size_t(px)) * 4;
            const float v[3] = {float((rgb >> 16) & 0xFF), float((rgb >> 8) & 0xFF), float(rgb & 0xFF)};
            for (int k = 0; k < 3; ++k) {
                out[k] = std::uint8_t(edge ? v[k] * 0.3f + ink[k] * 0.7f : v[k]);
            }
            out[3] = 255;
        }
    }
}

std::vector<std::uint8_t> Save(const Chart& chart) {
    std::vector<std::uint8_t> file(32);
    std::memcpy(file.data(), "BBMAP9\0\0", 8);
    std::memcpy(file.data() + 8, &chart.width, 4);
    std::memcpy(file.data() + 12, &chart.height, 4);
    std::memcpy(file.data() + 16, &chart.x0, 4);
    std::memcpy(file.data() + 20, &chart.z1, 4);
    std::memcpy(file.data() + 24, &chart.ppm, 4);
    mz_ulong size = mz_compressBound(mz_ulong(chart.rgba.size()));
    std::vector<std::uint8_t> packed(size);
    if (mz_compress2(packed.data(), &size, chart.rgba.data(), mz_ulong(chart.rgba.size()), 6) != MZ_OK) {
        return {};
    }
    const auto stored = std::uint32_t(size);
    std::memcpy(file.data() + 28, &stored, 4);
    file.insert(file.end(), packed.begin(), packed.begin() + std::ptrdiff_t(size));
    // The labels after the pixels: a count, then (name id, x, z) each.
    const auto count = std::uint32_t(chart.labels.size());
    const auto put = [&](const void* v) {
        const auto* b = static_cast<const std::uint8_t*>(v);
        file.insert(file.end(), b, b + 4);
    };
    put(&count);
    for (const Chart::Label& label : chart.labels) {
        put(&label.name_id);
        put(&label.x);
        put(&label.z);
    }
    // The storeys: their size, then the tops, the floors and their colours, zlib-compressed.
    const Chart::Storeys& s = chart.storeys;
    std::vector<std::uint8_t> raw;
    const auto append = [&](const auto& values) {
        const auto* b = reinterpret_cast<const std::uint8_t*>(values.data());
        raw.insert(raw.end(), b, b + values.size() * sizeof(values[0]));
    };
    append(s.roof);
    append(s.level);
    append(s.rgb);
    mz_ulong storeys_size = mz_compressBound(mz_ulong(raw.size()));
    std::vector<std::uint8_t> storeys_packed(storeys_size);
    if (mz_compress2(storeys_packed.data(), &storeys_size, raw.data(), mz_ulong(raw.size()), 6) != MZ_OK) {
        return {};
    }
    const auto storeys_stored = std::uint32_t(storeys_size);
    put(&s.width);
    put(&s.height);
    put(&storeys_stored);
    file.insert(file.end(), storeys_packed.begin(), storeys_packed.begin() + std::ptrdiff_t(storeys_size));
    return file;
}

std::optional<Chart> Load(std::span<const std::uint8_t> file) {
    if (file.size() < 32 || std::memcmp(file.data(), "BBMAP9\0\0", 8) != 0) {
        return std::nullopt;
    }
    Chart chart;
    std::uint32_t stored = 0;
    std::memcpy(&chart.width, file.data() + 8, 4);
    std::memcpy(&chart.height, file.data() + 12, 4);
    std::memcpy(&chart.x0, file.data() + 16, 4);
    std::memcpy(&chart.z1, file.data() + 20, 4);
    std::memcpy(&chart.ppm, file.data() + 24, 4);
    std::memcpy(&stored, file.data() + 28, 4);
    if (chart.width <= 0 || chart.height <= 0 || chart.width > 8192 || chart.height > 8192 ||
        !(chart.ppm > 0.0f) || stored > file.size() - 32) {
        return std::nullopt;
    }
    chart.rgba.resize(std::size_t(chart.width) * std::size_t(chart.height) * 4);
    mz_ulong size = mz_ulong(chart.rgba.size());
    if (mz_uncompress(chart.rgba.data(), &size, file.data() + 32, stored) != MZ_OK || size != chart.rgba.size()) {
        return std::nullopt;
    }
    std::size_t at = 32 + stored;
    std::uint32_t count = 0;
    if (at + 4 <= file.size()) {
        std::memcpy(&count, file.data() + at, 4);
        at += 4;
    }
    for (std::uint32_t i = 0; i < count && at + 12 <= file.size() && i < 10000; ++i, at += 12) {
        Chart::Label label;
        std::memcpy(&label.name_id, file.data() + at, 4);
        std::memcpy(&label.x, file.data() + at + 4, 4);
        std::memcpy(&label.z, file.data() + at + 8, 4);
        chart.labels.push_back(label);
    }
    Chart::Storeys s;
    std::uint32_t storeys_stored = 0;
    if (at + 12 > file.size()) {
        return std::nullopt;
    }
    std::memcpy(&s.width, file.data() + at, 4);
    std::memcpy(&s.height, file.data() + at + 4, 4);
    std::memcpy(&storeys_stored, file.data() + at + 8, 4);
    at += 12;
    if (s.width < 0 || s.height < 0 || s.width > 8192 || s.height > 8192 || storeys_stored > file.size() - at) {
        return std::nullopt;
    }
    const std::size_t cells = std::size_t(s.width) * std::size_t(s.height);
    s.roof.resize(cells);
    s.level.resize(cells * Chart::Levels);
    s.rgb.resize(cells * Chart::Levels);
    std::vector<std::uint8_t> raw(cells * 2 + cells * Chart::Levels * 2 + cells * Chart::Levels * 4);
    mz_ulong raw_size = mz_ulong(raw.size());
    if (cells > 0 &&
        (mz_uncompress(raw.data(), &raw_size, file.data() + at, storeys_stored) != MZ_OK || raw_size != raw.size())) {
        return std::nullopt;
    }
    std::memcpy(s.roof.data(), raw.data(), cells * 2);
    std::memcpy(s.level.data(), raw.data() + cells * 2, cells * Chart::Levels * 2);
    std::memcpy(s.rgb.data(), raw.data() + cells * 2 + cells * Chart::Levels * 2, cells * Chart::Levels * 4);
    chart.storeys = std::move(s);
    return chart;
}

std::string MapNameOf(std::uint32_t packed_map) {
    char name[16];
    std::snprintf(name, sizeof(name), "m%02u_%02u_00_00", (packed_map >> 24) & 0xFF, (packed_map >> 16) & 0xFF);
    return name;
}

namespace {
/// A surface's colour at a corner (its first and second texture coordinates, `uv`): the diffuse
/// texture's at a middling level (an average over a hand's width), the second one mixed in by
/// its mask, in the baked light. 0xFFRRGGBB.
std::uint32_t SurfaceColorAt(const Surface& surface, const float* uv) {
    constexpr int Level = 3;
    std::uint32_t c = surface.diffuse->Sample(uv[0], uv[1], Level);
    float rgb[3] = {float((c >> 16) & 0xFF), float((c >> 8) & 0xFF), float(c & 0xFF)};
    if (surface.diffuse2 && surface.mask) {
        const float m = float((surface.mask->Sample(uv[0], uv[1], Level) >> 16) & 0xFF) / 255.0f;
        const std::uint32_t d = surface.diffuse2->Sample(uv[0], uv[1], Level);
        const float other[3] = {float((d >> 16) & 0xFF), float((d >> 8) & 0xFF), float(d & 0xFF)};
        for (int k = 0; k < 3; ++k) {
            rgb[k] += (other[k] - rgb[k]) * m;
        }
    }
    if (surface.light) {
        const std::uint32_t lit = surface.light->Sample(uv[2], uv[3], 1);
        const float gain = LightGain();
        rgb[0] *= float((lit >> 16) & 0xFF) / 255.0f * gain;
        rgb[1] *= float((lit >> 8) & 0xFF) / 255.0f * gain;
        rgb[2] *= float(lit & 0xFF) / 255.0f * gain;
    }
    return 0xFF000000u | std::uint32_t(std::clamp(rgb[0], 0.0f, 255.0f)) << 16 |
           std::uint32_t(std::clamp(rgb[1], 0.0f, 255.0f)) << 8 | std::uint32_t(std::clamp(rgb[2], 0.0f, 255.0f));
}
} // namespace

std::uint32_t SurfaceColor(const Surface& surface, const float* uv) {
    return SurfaceColorAt(surface, uv);
}

std::uint32_t MeshBuilder::Corner(const float* at, std::uint32_t color) {
    const auto q = [&](float v) { return std::uint64_t(std::int64_t(std::floor(v / cell)) + (1 << 20)) & 0x1FFFFF; };
    const std::uint64_t key = q(at[0]) | q(at[1]) << 21 | q(at[2]) << 42;
    auto [it, fresh] = corner_of.try_emplace(key, std::uint32_t(counts.size()));
    if (fresh) {
        sums.insert(sums.end(), 6, 0.0);
        counts.push_back(0);
    }
    double* s = sums.data() + std::size_t(it->second) * 6;
    s[0] += at[0], s[1] += at[1], s[2] += at[2];
    s[3] += (color >> 16) & 0xFF, s[4] += (color >> 8) & 0xFF, s[5] += color & 0xFF;
    ++counts[it->second];
    return it->second;
}

void MeshBuilder::Add(const float* triangle, const std::uint32_t corner_colors[3]) {
    const std::uint32_t a = Corner(triangle, corner_colors[0]), b = Corner(triangle + 3, corner_colors[1]),
                        c = Corner(triangle + 6, corner_colors[2]);
    if (a == b || b == c || a == c) {
        return; // folded to a line or a point
    }
    std::uint32_t k[3] = {a, b, c};
    std::sort(k, k + 3);
    const std::uint64_t key = std::uint64_t(k[0]) * 0x9E3779B97F4A7C15ull ^ std::uint64_t(k[1]) * 0xC2B2AE3D27D4EB4Full ^
                              std::uint64_t(k[2]);
    if (!seen.insert(key).second) {
        return; // the same three corners again (its back, a copy)
    }
    tris.insert(tris.end(), {a, b, c});
}

void MeshBuilder::Merge(const MeshBuilder& other) {
    // Each of the other's triangles with its corners at their averages, in their colours.
    for (std::size_t t = 0; t + 3 <= other.tris.size(); t += 3) {
        float at[9];
        std::uint32_t colors[3];
        for (int k = 0; k < 3; ++k) {
            const std::uint32_t v = other.tris[t + std::size_t(k)];
            const double* s = other.sums.data() + std::size_t(v) * 6;
            const double n = std::max(1u, other.counts[v]);
            for (int d = 0; d < 3; ++d) {
                at[k * 3 + d] = float(s[d] / n);
            }
            colors[k] = 0xFF000000u | std::uint32_t(s[3] / n) << 16 | std::uint32_t(s[4] / n) << 8 | std::uint32_t(s[5] / n);
        }
        Add(at, colors);
    }
}

AreaMesh MeshBuilder::Finish() const {
    AreaMesh mesh;
    mesh.positions.reserve(counts.size() * 3);
    mesh.colors.reserve(counts.size());
    for (std::size_t v = 0; v < counts.size(); ++v) {
        const double* s = sums.data() + v * 6;
        const double n = std::max(1u, counts[v]);
        mesh.positions.insert(mesh.positions.end(), {float(s[0] / n), float(s[1] / n), float(s[2] / n)});
        // RGBA bytes in memory: R first.
        mesh.colors.push_back(0xFF000000u | std::uint32_t(s[5] / n) << 16 | std::uint32_t(s[4] / n) << 8 |
                              std::uint32_t(s[3] / n));
    }
    mesh.indices = tris;
    return mesh;
}

std::vector<std::uint8_t> SaveMesh(const AreaMesh& mesh) {
    std::vector<std::uint8_t> raw;
    const auto put32 = [&](std::uint32_t v) {
        const auto* b = reinterpret_cast<const std::uint8_t*>(&v);
        raw.insert(raw.end(), b, b + 4);
    };
    put32(std::uint32_t(mesh.colors.size()));
    put32(std::uint32_t(mesh.indices.size()));
    const auto* p = reinterpret_cast<const std::uint8_t*>(mesh.positions.data());
    raw.insert(raw.end(), p, p + mesh.positions.size() * 4);
    const auto* c = reinterpret_cast<const std::uint8_t*>(mesh.colors.data());
    raw.insert(raw.end(), c, c + mesh.colors.size() * 4);
    const auto* i = reinterpret_cast<const std::uint8_t*>(mesh.indices.data());
    raw.insert(raw.end(), i, i + mesh.indices.size() * 4);
    mz_ulong size = mz_compressBound(mz_ulong(raw.size()));
    std::vector<std::uint8_t> file(16 + size);
    std::memcpy(file.data(), "BBMESH1\0", 8);
    const auto raw_size = std::uint32_t(raw.size());
    std::memcpy(file.data() + 8, &raw_size, 4);
    if (mz_compress2(file.data() + 16, &size, raw.data(), mz_ulong(raw.size()), 6) != MZ_OK) {
        return {};
    }
    const auto stored = std::uint32_t(size);
    std::memcpy(file.data() + 12, &stored, 4);
    file.resize(16 + size);
    return file;
}

std::optional<AreaMesh> LoadMesh(std::span<const std::uint8_t> file) {
    if (file.size() < 16 || std::memcmp(file.data(), "BBMESH1\0", 8) != 0) {
        return std::nullopt;
    }
    std::uint32_t raw_size = 0, stored = 0;
    std::memcpy(&raw_size, file.data() + 8, 4);
    std::memcpy(&stored, file.data() + 12, 4);
    if (stored > file.size() - 16 || raw_size < 8 || raw_size > (1u << 31)) {
        return std::nullopt;
    }
    std::vector<std::uint8_t> raw(raw_size);
    mz_ulong size = raw_size;
    if (mz_uncompress(raw.data(), &size, file.data() + 16, stored) != MZ_OK || size != raw_size) {
        return std::nullopt;
    }
    std::uint32_t vertices = 0, indices = 0;
    std::memcpy(&vertices, raw.data(), 4);
    std::memcpy(&indices, raw.data() + 4, 4);
    if (8 + std::uint64_t(vertices) * 16 + std::uint64_t(indices) * 4 != raw_size) {
        return std::nullopt;
    }
    AreaMesh mesh;
    mesh.positions.resize(std::size_t(vertices) * 3);
    mesh.colors.resize(vertices);
    mesh.indices.resize(indices);
    std::memcpy(mesh.positions.data(), raw.data() + 8, std::size_t(vertices) * 12);
    std::memcpy(mesh.colors.data(), raw.data() + 8 + std::size_t(vertices) * 12, std::size_t(vertices) * 4);
    std::memcpy(mesh.indices.data(), raw.data() + 8 + std::size_t(vertices) * 16, std::size_t(indices) * 4);
    for (const std::uint32_t i : mesh.indices) {
        if (i >= vertices) {
            return std::nullopt;
        }
    }
    return mesh;
}

std::optional<AreaMesh> MeshOf(std::uint32_t packed_map, const ReadFile& read, int threads, const ReadRange& read_range,
                               float cell) {
    threads = std::clamp(threads, 1, 16);
    std::vector<MeshBuilder> parts(static_cast<std::size_t>(threads), MeshBuilder(cell));
    const std::array<std::uint32_t, 1> one{packed_map};
    // A canvas too small to matter: the models go to the meshes instead.
    if (!ChartAreas(one, read, 0.01f, 64, threads, read_range, true, 0, &parts)) {
        return std::nullopt;
    }
    for (std::size_t i = 1; i < parts.size(); ++i) {
        parts[0].Merge(parts[i]);
    }
    if (parts[0].Triangles() == 0) {
        return std::nullopt;
    }
    return parts[0].Finish();
}

std::optional<Chart> ChartArea(std::uint32_t packed_map, const ReadFile& read, float ppm, int max_side, int threads,
                               const ReadRange& read_range, bool textured, int view) {
    const std::array<std::uint32_t, 1> one{packed_map};
    return ChartAreas(one, read, ppm, max_side, threads, read_range, textured, view);
}

Chart Halved(const Chart& chart) {
    Chart out;
    out.width = std::max(1, chart.width / 2);
    out.height = std::max(1, chart.height / 2);
    out.x0 = chart.x0;
    out.z1 = chart.z1;
    out.ppm = chart.ppm / 2.0f;
    out.labels = chart.labels;
    out.rgba.resize(std::size_t(out.width) * std::size_t(out.height) * 4);
    for (int y = 0; y < out.height; ++y) {
        for (int x = 0; x < out.width; ++x) {
            // The colours weighed by their cover, so an edge against the void stays its colour.
            float sum[3] = {}, cover = 0;
            for (int k = 0; k < 4; ++k) {
                const std::uint8_t* p = chart.rgba.data() +
                                        (std::size_t(y * 2 + (k >> 1)) * std::size_t(chart.width) + std::size_t(x * 2 + (k & 1))) * 4;
                const float a = float(p[3]) / 255.0f;
                for (int c = 0; c < 3; ++c) {
                    sum[c] += float(p[c]) * a;
                }
                cover += a;
            }
            std::uint8_t* q = out.rgba.data() + (std::size_t(y) * std::size_t(out.width) + std::size_t(x)) * 4;
            for (int c = 0; c < 3; ++c) {
                q[c] = cover > 0 ? std::uint8_t(std::clamp(sum[c] / cover + 0.5f, 0.0f, 255.0f)) : 0;
            }
            q[3] = std::uint8_t(std::clamp(cover / 4.0f * 255.0f + 0.5f, 0.0f, 255.0f));
        }
    }
    return out;
}

ObliqueView ViewOf(int view) {
    return ObliqueView{float(std::clamp(view, 1, 4) - 1) * 1.5707963f, 0.87f};
}

std::array<float, 3> ToView(const ObliqueView& view, float x, float y, float z) {
    const float c = std::cos(view.yaw), s = std::sin(view.yaw);
    const float u = x * c - z * s, v = x * s + z * c;
    const float sp = std::sin(view.pitch), cp = std::cos(view.pitch);
    return {u, v * sp + y * cp, -v * cp + y * sp};
}

namespace {

/// The material colours of an area's textures (map/mAA/*.tpfbhd, their low-resolution "_l"
/// copies), worked out when first asked; safe from several threads.
class TextureColors {
public:
    TextureColors(const ReadFile& read, const ReadRange& read_range) : read{read}, read_range{read_range} {}

    /// The archives of area folder `folder` ("m24") in the index.
    void AddFolder(const std::string& folder, std::span<const std::string> archives) {
        std::scoped_lock lock{mutex};
        for (const std::string& archive : archives) {
            const std::string bhd = "map/" + folder + "/" + archive + ".tpfbhd";
            for (const ArchiveEntry& entry : ReadArchiveHeader(read(bhd))) {
                index.try_emplace(entry.name, Where{"map/" + folder + "/" + archive + ".tpfbdt", entry.offset, entry.size});
            }
        }
    }

    std::uint32_t Of(std::string_view texture) {
        if (!read_range) {
            return 0;
        }
        const std::string name(texture.substr(0, texture.find(';')));
        Where where;
        {
            std::scoped_lock lock{mutex};
            if (const auto known = colors.find(name); known != colors.end()) {
                return known->second;
            }
            auto found = index.find(name + "_l.tpf.dcx");
            if (found == index.end()) {
                found = index.find(name + ".tpf.dcx");
            }
            if (found == index.end()) {
                colors[name] = 0;
                return 0;
            }
            where = found->second;
        }
        const auto packed = read_range(where.file, where.offset, where.size);
        const auto tpf = BbGameArt::InflateDcx(packed);
        const std::uint32_t color = tpf ? AverageColor(*tpf) : 0;
        std::scoped_lock lock{mutex};
        colors[name] = color;
        return color;
    }

    /// Kaniel 2026-10-19: a texture itself (its full copy, else its low-resolution one) as an id
    /// for TexturedColor; -1 when there is none.
    std::int32_t ImageOf(std::string_view texture) {
        if (!read_range) {
            return -1;
        }
        const std::string name(texture);
        Where where;
        {
            std::scoped_lock lock{mutex};
            if (const auto known = image_ids.find(name); known != image_ids.end()) {
                return known->second;
            }
            auto found = index.find(name + ".tpf.dcx");
            if (found == index.end()) {
                found = index.find(name + "_l.tpf.dcx");
            }
            if (found == index.end()) {
                image_ids[name] = -1;
                return -1;
            }
            where = found->second;
        }
        const auto tpf = BbGameArt::InflateDcx(read_range(where.file, where.offset, where.size));
        auto decoded = tpf ? DecodeTexture(*tpf) : std::nullopt;
        std::scoped_lock lock{mutex};
        if (const auto known = image_ids.find(name); known != image_ids.end()) {
            return known->second; // another thread got there first
        }
        if (!decoded) {
            image_ids[name] = -1;
            return -1;
        }
        images.push_back(std::make_shared<const Texture>(std::move(*decoded)));
        return image_ids[name] = std::int32_t(images.size() - 1);
    }
    std::shared_ptr<const Texture> Image(std::uint32_t id) {
        std::scoped_lock lock{mutex};
        return id < images.size() ? images[id] : nullptr;
    }
    /// A material's surface ("diffuse;diffuse2;mask;light", MaterialColor) as an id for
    /// TexturedColor; -1 when its diffuse texture is not there. The baked light is the DOL
    /// texture's first atlas, its low-resolution copy (light changes slowly).
    std::int32_t SurfaceOf(std::string_view key) {
        {
            std::scoped_lock lock{mutex};
            if (const auto known = surface_ids.find(std::string(key)); known != surface_ids.end()) {
                return known->second;
            }
        }
        std::string parts[4];
        std::size_t at = 0;
        for (int k = 0; k < 4; ++k) {
            const std::size_t end = std::min(key.size(), key.find(';', at));
            parts[k] = std::string(key.substr(std::min(at, key.size()), end - std::min(at, key.size())));
            at = end + 1;
        }
        Surface surface;
        const auto image = [&](const std::string& name) -> std::shared_ptr<const Texture> {
            const std::int32_t id = name.empty() ? -1 : ImageOf(name);
            return id >= 0 ? Image(std::uint32_t(id)) : nullptr;
        };
        surface.diffuse = image(parts[0]);
        surface.diffuse2 = image(parts[1]);
        surface.mask = image(parts[2]);
        if (!parts[3].empty() && LightGain() > 0.0f) {
            surface.light = image(parts[3] + "_atlas000_l");
            if (!surface.light) {
                surface.light = image(parts[3] + "_atlas000");
            }
        }
        std::scoped_lock lock{mutex};
        if (const auto known = surface_ids.find(std::string(key)); known != surface_ids.end()) {
            return known->second;
        }
        if (!surface.diffuse) {
            return surface_ids[std::string(key)] = -1;
        }
        surfaces.push_back(std::make_shared<const Surface>(std::move(surface)));
        return surface_ids[std::string(key)] = std::int32_t(surfaces.size() - 1);
    }
    std::shared_ptr<const Surface> SurfaceAt(std::uint32_t id) {
        std::scoped_lock lock{mutex};
        return id < surfaces.size() ? surfaces[id] : nullptr;
    }

private:
    struct Where {
        std::string file;
        std::uint64_t offset = 0, size = 0;
    };
    const ReadFile& read;
    const ReadRange& read_range;
    std::mutex mutex;
    std::map<std::string, Where> index;
    std::map<std::string, std::uint32_t> colors;
    std::map<std::string, std::int32_t> image_ids;
    std::vector<std::shared_ptr<const Texture>> images;
    std::map<std::string, std::int32_t> surface_ids;
    std::vector<std::shared_ptr<const Surface>> surfaces;
};

/// The collision meshes of an area (map/mAA_BB_00_00/hAA_BB_00_00.hkxbhd/.hkxbdt, the .patch ones
/// over them) by their file names' stems ("h24_01_00_00_000100").
std::map<std::string, std::vector<std::uint8_t>> CollisionFiles(const std::string& map_name, const ReadFile& read) {
    std::map<std::string, std::vector<std::uint8_t>> files;
    const std::string stem = "map/" + map_name + "/h" + map_name.substr(1);
    for (const std::string suffix : {"", ".patch"}) {
        const auto bhd = read(stem + ".hkxbhd" + suffix);
        if (bhd.empty()) {
            continue;
        }
        const auto bdt = read(stem + ".hkxbdt" + suffix);
        for (const ArchiveEntry& entry : ReadArchiveHeader(bhd)) {
            if (entry.offset + entry.size > bdt.size()) {
                continue;
            }
            std::string name = entry.name.substr(entry.name.find_last_of("\\/") + 1);
            name = name.substr(0, name.find('.'));
            files[name].assign(bdt.begin() + std::ptrdiff_t(entry.offset),
                               bdt.begin() + std::ptrdiff_t(entry.offset + entry.size));
        }
    }
    return files;
}

/// The texture archives of area AA's folder: mAA_0000 .. mAA_0009 and mAA_BB_0000 .. (those
/// that are there).
std::vector<std::string> ArchivesOf(const std::string& map_name, const ReadFile& exists) {
    std::vector<std::string> archives;
    const std::string area = map_name.substr(0, 3), block = map_name.substr(0, 6);
    // Kaniel 2026-10-19 (blended ground): a material can take a mask or a texture from another
    // block of its area (m24_01 uses m24_00's masks), so those blocks' archives come last.
    std::vector<std::string> prefixes{area, block};
    for (int b = 0; b < 10; ++b) {
        char other[16];
        std::snprintf(other, sizeof(other), "%s_%02d", area.c_str(), b);
        if (other != block) {
            prefixes.emplace_back(other);
        }
    }
    for (const std::string& prefix : prefixes) {
        for (int i = 0; i < 10; ++i) {
            char name[32];
            std::snprintf(name, sizeof(name), "%s_%04d", prefix.c_str(), i);
            if (std::find(archives.begin(), archives.end(), name) == archives.end() &&
                !exists("map/" + area + "/" + name + ".tpfbhd").empty()) {
                archives.emplace_back(name);
            }
        }
    }
    // Kaniel 2026-10-19: the light the game baked for each model (gi_env_mAA, gi_env_mAA_BB).
    for (const std::string& name : {"gi_env_" + area, "gi_env_" + block}) {
        if (std::find(archives.begin(), archives.end(), name) == archives.end() &&
            !exists("map/" + area + "/" + name + ".tpfbhd").empty()) {
            archives.push_back(name);
        }
    }
    return archives;
}

} // namespace

std::optional<Chart> ChartAreas(std::span<const std::uint32_t> packed_maps, const ReadFile& read, float ppm,
                                int max_side, int threads, const ReadRange& read_range, bool textured, int view,
                                std::vector<MeshBuilder>* meshes) {
    TextureColors textures(read, read_range);
    const auto inflate = [](std::vector<std::uint8_t> file) {
        if (file.size() >= 4 && std::memcmp(file.data(), "DCX\0", 4) == 0) {
            return BbGameArt::InflateDcx(file).value_or(std::vector<std::uint8_t>{});
        }
        return file;
    };
    // Each area's pieces, filtered by its own play area; the picture spans them all.
    struct Source {
        std::string name;
        MapStudio studio;
        PlayArea area;
    };
    std::vector<Source> sources;
    std::optional<PlayArea> all;
    for (const std::uint32_t packed_map : packed_maps) {
        const std::string name = MapNameOf(packed_map);
        auto studio = ReadMapStudio(inflate(read("map/MapStudio/" + name + ".msb.dcx")));
        const auto area = studio && !studio->pieces.empty() ? PlayAreaOf(studio->spots) : std::nullopt;
        if (!area) {
            continue;
        }
        all = all ? PlayArea{std::min(all->x0, area->x0), std::max(all->x1, area->x1), std::min(all->z0, area->z0),
                             std::max(all->z1, area->z1), std::max(all->ceiling, area->ceiling)}
                  : *area;
        sources.push_back(Source{name, std::move(*studio), *area});
        if (read_range) {
            textures.AddFolder(name.substr(0, 3), ArchivesOf(name, read));
        }
    }
    if (sources.empty()) {
        return std::nullopt;
    }
    // A model at a time, every piece of it: tens of millions of triangles never at once. Each
    // thread draws every `threads`-th model on its own canvas; the canvases are merged.
    struct Model {
        const Source* source;
        std::string name;
        std::vector<const Piece*> pieces;
    };
    std::vector<Model> models;
    for (const Source& source : sources) {
        std::map<std::string, std::vector<const Piece*>> by_model;
        for (const Piece& piece : source.studio.pieces) {
            by_model[piece.model].push_back(&piece);
        }
        for (auto& [name, pieces] : by_model) {
            models.push_back(Model{&source, name, std::move(pieces)});
        }
    }
    threads = std::clamp(threads, 1, 16);
    // An oblique picture: every triangle turned into the view's frame (x: u, z: w, y: nearness),
    // the canvas round the play area's box seen that way.
    const std::optional<ObliqueView> oblique = view > 0 ? std::optional<ObliqueView>(ViewOf(view)) : std::nullopt;
    const auto turn = [&](std::vector<float>& triangles) {
        if (!oblique) {
            return;
        }
        for (std::size_t i = 0; i + 3 <= triangles.size(); i += 3) {
            const auto t = ToView(*oblique, triangles[i], triangles[i + 1], triangles[i + 2]);
            triangles[i] = t[0], triangles[i + 1] = t[2], triangles[i + 2] = t[1];
        }
    };
    PlayArea canvas_area = *all;
    if (oblique) {
        float low = all->ceiling;
        for (const Source& source : sources) {
            for (const Vec3& spot : source.studio.spots) {
                low = std::min(low, spot[1]);
            }
        }
        low -= 40.0f;
        canvas_area = PlayArea{FLT_MAX, -FLT_MAX, FLT_MAX, -FLT_MAX, FLT_MAX};
        for (int k = 0; k < 8; ++k) {
            const auto t = ToView(*oblique, k & 1 ? all->x1 : all->x0, k & 4 ? all->ceiling : low, k & 2 ? all->z1 : all->z0);
            canvas_area.x0 = std::min(canvas_area.x0, t[0]), canvas_area.x1 = std::max(canvas_area.x1, t[0]);
            canvas_area.z0 = std::min(canvas_area.z0, t[1]), canvas_area.z1 = std::max(canvas_area.z1, t[1]);
        }
    }
    std::vector<Canvas> canvases(std::size_t(threads), Canvas(canvas_area, ppm, max_side));
    if (oblique) {
        for (Canvas& canvas : canvases) {
            canvas.SetOblique();
        }
    }
    // Kaniel 2026-10-18: the collision meshes' floors: where one can walk (what lies far from it
    // goes) and where each place the game names lies (its label).
    std::map<std::int32_t, std::array<double, 3>> places; // name id -> area-weighted x, z, area
    for (const Source& source : sources) {
        const auto files = CollisionFiles(source.name, read);
        std::map<std::string, std::vector<float>> meshes;
        for (const MapStudio::Collision& collision : source.studio.collisions) {
            const std::string& model = collision.piece.model;
            auto mesh = meshes.find(model);
            if (mesh == meshes.end()) {
                std::vector<float> triangles;
                auto file = files.find(model);
                if (file == files.end()) {
                    file = files.find("h" + source.name.substr(1) + "_" + model.substr(1));
                }
                if (file != files.end()) {
                    AppendCollisionTriangles(inflate(file->second), triangles);
                }
                mesh = meshes.emplace(model, Floors(triangles)).first;
            }
            std::vector<float> world;
            Place(collision.piece, mesh->second, PlayArea{-1e9f, 1e9f, -1e9f, 1e9f, 1e9f}, world);
            std::vector<float> seen = world;
            turn(seen);
            canvases[0].AddWalkable(seen);
            // Area names (ids ending in 0) are the title already.
            if (collision.place_name > 0 && collision.place_name % 10 != 0) {
                auto& place = places[collision.place_name];
                for (std::size_t t = 0; t + 9 <= world.size(); t += 9) {
                    const float a = TriangleArea(world.data() + t);
                    place[0] += a * (world[t] + world[t + 3] + world[t + 6]) / 3.0;
                    place[1] += a * (world[t + 2] + world[t + 5] + world[t + 8]) / 3.0;
                    place[2] += a;
                }
            }
        }
    }
    for (std::size_t i = 1; i < canvases.size(); ++i) {
        canvases[i].ShareFloors(canvases[0]);
    }
    // Kaniel 2026-10-19: the areas' torches and lamps (their light files' point lights).
    for (const Source& source : sources) {
        const auto btl = inflate(read("map/" + source.name + "/" + source.name + "_0000.btl.dcx"));
        for (const PointLight& light : ReadLights(btl)) {
            if (light.x >= source.area.x0 && light.x <= source.area.x1 && light.z >= source.area.z0 &&
                light.z <= source.area.z1) {
                PointLight placed = light;
                if (oblique) {
                    const auto t = ToView(*oblique, light.x, light.y, light.z);
                    placed.x = t[0], placed.y = t[2], placed.z = t[1];
                }
                canvases[0].AddLight(placed);
            }
        }
    }
    if (textured) {
        for (Canvas& canvas : canvases) {
            canvas.SetTextures([&](std::uint32_t id) { return textures.SurfaceAt(id); });
        }
    }
    // The game's colour grade of the (first) area's block.
    if (const auto tpf = BbGameArt::InflateDcx(read("map/" + sources[0].name.substr(0, 3) + "/" +
                                                   sources[0].name.substr(0, 3) + "_cgrading.tpf.dcx"))) {
        if (auto grade = ReadColorGrade(*tpf, sources[0].name.substr(0, 6))) {
            canvases[0].SetGrade(std::move(*grade));
        }
    }
    const auto draw = [&](int part) {
        std::vector<float> model, world, model_uvs, world_uvs;
        std::vector<std::uint32_t> model_colors, world_colors;
        const MaterialColor color_of = [&](std::string_view texture) {
            if (textured) {
                if (const std::int32_t id = textures.SurfaceOf(texture); id >= 0) {
                    return TexturedColor(std::uint32_t(id));
                }
            }
            return textures.Of(texture);
        };
        for (std::size_t m = std::size_t(part); m < models.size(); m += std::size_t(threads)) {
            const Model& it = models[m];
            const std::string& map = it.source->name;
            model.clear();
            model_colors.clear();
            model_uvs.clear();
            const auto flver = inflate(read("map/" + map + "/" + map + "_" + it.name.substr(1) + ".flver.dcx"));
            if (!AppendTriangles(flver, model, &model_colors, color_of, textured ? &model_uvs : nullptr) ||
                SkippedModel(it.name, model)) {
                continue;
            }
            for (const Piece* piece : it.pieces) {
                world.clear();
                world_colors.clear();
                world_uvs.clear();
                Place(*piece, model, it.source->area, world, model_colors, &world_colors, model_uvs,
                      textured ? &world_uvs : nullptr);
                turn(world);
                if (meshes != nullptr) {
                    // The live 3D map's mesh: each corner in its surface's colour there.
                    MeshBuilder& mesh = (*meshes)[std::size_t(part)];
                    for (std::size_t t = 0; t + 9 <= world.size(); t += 9) {
                        std::uint32_t corner[3];
                        const std::uint32_t tint = t / 9 < world_colors.size() ? world_colors[t / 9] : 0u;
                        const auto surface = IsTextured(tint) ? textures.SurfaceAt(tint & 0xFFFFFFu) : nullptr;
                        for (int k = 0; k < 3; ++k) {
                            const float* uv = t / 9 * 12 + 12 <= world_uvs.size() ? world_uvs.data() + t / 9 * 12 + k * 4
                                                                                  : nullptr;
                            corner[k] = surface && uv ? SurfaceColor(*surface, uv) : (tint != 0 ? tint : 0xFF808080u);
                        }
                        mesh.Add(world.data() + t, corner);
                    }
                    continue;
                }
                canvases[std::size_t(part)].Add(world, world_colors, world_uvs);
            }
        }
    };
    std::vector<std::thread> workers;
    for (int part = 1; part < threads; ++part) {
        workers.emplace_back(draw, part);
    }
    draw(0);
    for (std::thread& worker : workers) {
        worker.join();
    }
    for (std::size_t part = 1; part < canvases.size(); ++part) {
        canvases[0].Merge(canvases[part]);
    }
    Chart chart = canvases[0].Finish();
    for (const auto& [id, place] : places) {
        if (place[2] > 0) {
            chart.labels.push_back(Chart::Label{id, float(place[0] / place[2]), float(place[1] / place[2])});
        }
    }
    return chart;
}

} // namespace BbMapArt
