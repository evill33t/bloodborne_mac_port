// SPDX-License-Identifier: GPL-2.0-or-later
// bbport co-op: the map's walkable floors from the game's collision meshes (bbport_map_art.h).
// The Havok 2014 packfile and compressed-mesh reading follows Hunter's Atlas
// (MikeTat1988/HuntersAtlas, generate_navigation_maps.py, GPL-2.0).
#include "bbport_map_art.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <optional>
#include <string>

namespace BbMapArt {

namespace {

template <typename T>
bool At(std::span<const std::uint8_t> bytes, std::size_t at, T& out) {
    if (at > bytes.size() || bytes.size() - at < sizeof(T)) {
        return false;
    }
    std::memcpy(&out, bytes.data() + at, sizeof(T));
    return true;
}

template <typename T>
T Value(std::span<const std::uint8_t> bytes, std::size_t at) {
    T out{};
    At(bytes, at, out);
    return out;
}

} // namespace

bool AppendCollisionTriangles(std::span<const std::uint8_t> hkx, std::vector<float>& out) {
    static constexpr std::uint8_t Magic[8] = {0x57, 0xE0, 0xE0, 0x57, 0x10, 0xC0, 0xC0, 0x10};
    if (hkx.size() < 0x100 || std::memcmp(hkx.data(), Magic, 8) != 0) {
        return false;
    }
    const std::size_t start = std::size_t(Value<std::int16_t>(hkx, 0x3E)) + 0x40;
    // Sections: class names (0), types (1), data (2); each header's 7 words from +20.
    std::uint32_t names = Value<std::uint32_t>(hkx, start + 20);
    std::uint32_t section[7];
    for (int i = 0; i < 7; ++i) {
        section[i] = Value<std::uint32_t>(hkx, start + 2 * 64 + 20 + std::size_t(i) * 4);
    }
    const std::size_t base = section[0], local = section[1], global = section[2], virt = section[3],
                      exports = section[4];
    if (base + exports > hkx.size() || local > global || global > virt || virt > exports) {
        return false;
    }
    std::map<std::uint32_t, std::uint32_t> fix;
    for (std::size_t o = base + local; o + 8 <= base + global; o += 8) {
        const auto from = Value<std::uint32_t>(hkx, o), to = Value<std::uint32_t>(hkx, o + 4);
        if (from != 0xFFFFFFFFu) {
            fix[from] = to;
        }
    }
    for (std::size_t o = base + global; o + 12 <= base + virt; o += 12) {
        const auto from = Value<std::uint32_t>(hkx, o), sid = Value<std::uint32_t>(hkx, o + 4);
        if (from != 0xFFFFFFFFu && sid == 2) {
            fix[from] = Value<std::uint32_t>(hkx, o + 8);
        }
    }
    std::vector<std::uint32_t> trees;
    std::vector<std::uint32_t> systems;
    for (std::size_t o = base + virt; o + 12 <= base + exports; o += 12) {
        const auto object = Value<std::uint32_t>(hkx, o), name = Value<std::uint32_t>(hkx, o + 8);
        if (object == 0xFFFFFFFFu) {
            continue;
        }
        const char* text = reinterpret_cast<const char*>(hkx.data()) + names + name;
        if (names + name >= hkx.size()) {
            continue;
        }
        const std::string kind(text, strnlen(text, hkx.size() - names - name));
        if (kind == "hknpCompressedMeshShapeData") {
            trees.push_back(object + 16);
        } else if (kind == "hknpPhysicsSystemData") {
            systems.push_back(object);
        }
    }
    if (trees.empty()) {
        return false;
    }
    const auto fixed = [&](std::uint32_t at) -> std::optional<std::uint32_t> {
        const auto found = fix.find(at);
        return found == fix.end() ? std::nullopt : std::optional{found->second};
    };
    // The bodies placing each tree: position, then the rotation's quaternion (x, y, z, w).
    struct Body {
        std::uint32_t tree;
        float p[3], m[9];
    };
    std::vector<Body> bodies;
    for (const std::uint32_t system : systems) {
        const auto list = fixed(system + 64);
        if (!list) {
            continue;
        }
        const auto count = Value<std::uint32_t>(hkx, base + system + 72);
        for (std::uint32_t i = 0; i < count && i < 100000; ++i) {
            const std::uint32_t body = *list + i * 96;
            const auto shape = fixed(body);
            if (!shape) {
                continue;
            }
            // Bloodborne's Havok 2014 keeps the mesh data pointer at +0x58 (later ones +0x60).
            auto data = fixed(*shape + 88);
            if (!data || std::find(trees.begin(), trees.end(), *data + 16) == trees.end()) {
                data = fixed(*shape + 96);
            }
            if (!data) {
                continue;
            }
            Body b{*data + 16, {}, {}};
            for (int k = 0; k < 3; ++k) {
                b.p[k] = Value<float>(hkx, base + body + 48 + std::size_t(k) * 4);
            }
            float q[4];
            for (int k = 0; k < 4; ++k) {
                q[k] = Value<float>(hkx, base + body + 64 + std::size_t(k) * 4);
            }
            const float n = std::max(1e-9f, std::sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]));
            const float x = q[0] / n, y = q[1] / n, z = q[2] / n, w = q[3] / n;
            const float m[9] = {1 - 2 * (y * y + z * z), 2 * (x * y - z * w),     2 * (x * z + y * w),
                                2 * (x * y + z * w),     1 - 2 * (x * x + z * z), 2 * (y * z - x * w),
                                2 * (x * z - y * w),     2 * (y * z + x * w),     1 - 2 * (x * x + y * y)};
            std::memcpy(b.m, m, sizeof(m));
            bodies.push_back(b);
        }
    }
    for (const std::uint32_t tree : trees) {
        // An array member at `offset`: its data (fixed up) and count (+8).
        const auto array = [&](std::uint32_t offset) -> std::pair<std::size_t, std::uint32_t> {
            const auto count = Value<std::uint32_t>(hkx, base + tree + offset + 8);
            const auto data = fixed(tree + offset);
            if (!count || !data || count > 10'000'000u) {
                return {0, 0};
            }
            return {base + *data, count};
        };
        float domain[8];
        for (int k = 0; k < 8; ++k) {
            domain[k] = Value<float>(hkx, base + tree + 16 + std::size_t(k) * 4);
        }
        const auto [prims_at, prims] = array(80);
        const auto [indices_at, indices] = array(96);
        const auto [packed_at, packed] = array(112);
        const auto [shared_at, shared] = array(128);
        const auto shared_vertex = [&](std::uint32_t i, float* xyz) {
            const auto v = Value<std::uint64_t>(hkx, shared_at + std::size_t(i) * 8);
            const double q[3] = {double(v & 0x1FFFFF), double((v >> 21) & 0x1FFFFF), double(v >> 42)};
            const double range[3] = {double(0x1FFFFF), double(0x1FFFFF), double(0x3FFFFF)};
            for (int k = 0; k < 3; ++k) {
                xyz[k] = float(domain[k] + q[k] * (domain[4 + k] - domain[k]) / range[k]);
            }
        };
        const auto sections = fixed(tree + 64);
        const auto count = Value<std::uint32_t>(hkx, base + tree + 72);
        if (!sections) {
            continue;
        }
        std::vector<float> geometry;
        for (std::uint32_t s = 0; s < count && s < 1'000'000u; ++s) {
            const std::size_t o = base + *sections + std::size_t(s) * 96;
            float codec[6];
            for (int k = 0; k < 6; ++k) {
                codec[k] = Value<float>(hkx, o + 48 + std::size_t(k) * 4);
            }
            const auto first = Value<std::uint32_t>(hkx, o + 72), sh = Value<std::uint32_t>(hkx, o + 76),
                       pr = Value<std::uint32_t>(hkx, o + 80);
            const auto npacked = Value<std::uint8_t>(hkx, o + 88), nshared = Value<std::uint8_t>(hkx, o + 89);
            std::vector<float> xyz;
            for (std::uint32_t i = 0; i < npacked && first + i < packed; ++i) {
                const auto v = Value<std::uint32_t>(hkx, packed_at + std::size_t(first + i) * 4);
                xyz.push_back(float(v & 0x7FF) * codec[3] + codec[0]);
                xyz.push_back(float((v >> 11) & 0x7FF) * codec[4] + codec[1]);
                xyz.push_back(float(v >> 22) * codec[5] + codec[2]);
            }
            for (std::uint32_t i = 0; i < nshared && (sh >> 8) + i < indices; ++i) {
                const auto index = Value<std::uint16_t>(hkx, indices_at + std::size_t((sh >> 8) + i) * 2);
                if (index >= shared) {
                    continue;
                }
                float v[3];
                shared_vertex(index, v);
                xyz.insert(xyz.end(), v, v + 3);
            }
            const std::size_t vertices = xyz.size() / 3;
            for (std::uint32_t p = 0; p < (pr & 0xFF) && (pr >> 8) + p < prims; ++p) {
                std::uint8_t c[4];
                std::memcpy(c, hkx.data() + prims_at + std::size_t((pr >> 8) + p) * 4, 4);
                if (c[0] == 0xDE && c[1] == 0xAD && c[2] == 0xDE && c[3] == 0xAD) {
                    continue; // Havok's poison in padding slots
                }
                const auto add = [&](int a, int b, int d) {
                    if (c[a] >= vertices || c[b] >= vertices || c[d] >= vertices) {
                        return;
                    }
                    for (const int k : {a, b, d}) {
                        geometry.insert(geometry.end(), xyz.begin() + std::ptrdiff_t(c[k]) * 3,
                                        xyz.begin() + std::ptrdiff_t(c[k]) * 3 + 3);
                    }
                };
                add(0, 1, 2);
                if (c[2] != c[3]) {
                    add(0, 2, 3);
                }
            }
        }
        bool placed = false;
        for (const Body& b : bodies) {
            if (b.tree != tree) {
                continue;
            }
            placed = true;
            for (std::size_t i = 0; i + 3 <= geometry.size(); i += 3) {
                const float* v = geometry.data() + i;
                for (int r = 0; r < 3; ++r) {
                    out.push_back(b.m[r * 3] * v[0] + b.m[r * 3 + 1] * v[1] + b.m[r * 3 + 2] * v[2] + b.p[r]);
                }
            }
        }
        if (!placed) {
            out.insert(out.end(), geometry.begin(), geometry.end());
        }
    }
    return true;
}

} // namespace BbMapArt
