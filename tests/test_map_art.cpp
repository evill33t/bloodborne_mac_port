// SPDX-License-Identifier: GPL-2.0-or-later
// bbport co-op: the map screen's pictures of the areas (shim/bbport_map_art.h), on made-up data: an
// MSB's pieces and spots, a FLVER's highest-detail triangles, the pieces placed, the picture from
// above north up, the file kept in the user folder. With BLOODBORNE_EBOOT naming the game's
// eboot.bin, Central Yharnam is drawn from the game's own files and its lamps land on the picture.
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "gpu/shim/bbport_game_art.h"
#include "gpu/shim/bbport_map_art.h"

using namespace BbMapArt;

namespace {

struct Blob {
    std::vector<std::uint8_t> bytes;
    std::size_t Size() const { return bytes.size(); }
    template <typename T>
    void Put(std::size_t at, T value) {
        if (bytes.size() < at + sizeof(T)) {
            bytes.resize(at + sizeof(T));
        }
        std::memcpy(bytes.data() + at, &value, sizeof(T));
    }
    template <typename T>
    std::size_t Add(T value) {
        const std::size_t at = bytes.size();
        Put(at, value);
        return at;
    }
    std::size_t AddUtf16(const std::string& text) {
        const std::size_t at = bytes.size();
        for (const char c : text) {
            Add<std::uint16_t>(std::uint16_t(c));
        }
        Add<std::uint16_t>(0);
        return at;
    }
    void Align(std::size_t n) {
        while (bytes.size() % n) {
            bytes.push_back(0);
        }
    }
};

struct MsbPart {
    std::string name;
    int model;
    Vec3 position, rotation, scale;
};

/// An MSB of three lists: models, points and parts (as the game's, records after each list).
std::vector<std::uint8_t> MakeMsb(const std::vector<std::string>& models, const std::vector<Vec3>& points,
                                  const std::vector<MsbPart>& parts) {
    Blob msb;
    msb.bytes = {'M', 'S', 'B', ' ', 1, 0, 0, 0, 0x10, 0, 0, 0, 0, 0, 0, 0};
    std::size_t link = 0; // where the previous list's next offset goes
    const auto list = [&](const std::string& name, std::size_t count, auto&& write_record) {
        msb.Align(8);
        const std::size_t at = msb.Size();
        if (link != 0) {
            msb.Put<std::int64_t>(link, std::int64_t(at));
        }
        msb.Add<std::int32_t>(3);
        msb.Add<std::int32_t>(std::int32_t(count + 1));
        const std::size_t name_at = msb.Add<std::int64_t>(0);
        const std::size_t table = msb.Size();
        for (std::size_t i = 0; i <= count; ++i) {
            msb.Add<std::int64_t>(0);
        }
        link = table + count * 8;
        msb.Put<std::int64_t>(name_at, std::int64_t(msb.AddUtf16(name)));
        for (std::size_t i = 0; i < count; ++i) {
            msb.Align(8);
            const std::size_t record = msb.Size();
            msb.Put<std::int64_t>(table + i * 8, std::int64_t(record));
            write_record(i, record);
        }
    };
    list("MODEL_PARAM_ST", models.size(), [&](std::size_t i, std::size_t record) {
        msb.Add<std::int64_t>(0x10); // its name follows
        msb.Add<std::int32_t>(0);
        msb.Add<std::int32_t>(std::int32_t(i));
        msb.Put<std::int64_t>(record, std::int64_t(msb.AddUtf16(models[i]) - record));
    });
    list("POINT_PARAM_ST", points.size(), [&](std::size_t i, std::size_t record) {
        msb.Put<std::int64_t>(record, 0);
        msb.Put<Vec3>(record + 0x14, points[i]);
    });
    list("PARTS_PARAM_ST", parts.size(), [&](std::size_t i, std::size_t record) {
        const MsbPart& part = parts[i];
        msb.Put<std::int64_t>(record + 0x00, 0);
        msb.Put<std::int64_t>(record + 0x08, 0); // patched below
        msb.Put<std::int32_t>(record + 0x1C, part.model);
        msb.Put<Vec3>(record + 0x28, part.position);
        msb.Put<Vec3>(record + 0x34, part.rotation);
        msb.Put<Vec3>(record + 0x40, part.scale);
        msb.Put<std::uint8_t>(record + 0x5F, 0);
        msb.Put<std::int64_t>(record + 0x08, std::int64_t(msb.AddUtf16(part.name) - record));
    });
    return msb.bytes;
}

struct Faceset {
    std::uint32_t flags;
    bool strip;
    std::vector<std::uint16_t> indices;
};
struct Mesh {
    std::string mtd;
    std::vector<Vec3> vertices;
    std::vector<Faceset> facesets;
    std::string diffuse = {}; // its material's diffuse texture, "" none
};

/// A FLVER2 with each mesh its own material, vertex buffer (float3 position then 8 bytes of
/// something else) and facesets.
std::vector<std::uint8_t> MakeFlver(const std::vector<Mesh>& meshes) {
    std::size_t facesets = 0;
    for (const Mesh& m : meshes) {
        facesets += m.facesets.size();
    }
    Blob f;
    f.bytes.resize(0x80);
    std::memcpy(f.bytes.data(), "FLVER\0L\0", 8);
    f.Put<std::uint32_t>(0x08, 0x20014);
    f.Put<std::int32_t>(0x14, 0);                          // dummies
    f.Put<std::int32_t>(0x18, std::int32_t(meshes.size())); // materials
    f.Put<std::int32_t>(0x1C, 0);                          // bones
    f.Put<std::int32_t>(0x20, std::int32_t(meshes.size()));
    f.Put<std::int32_t>(0x24, std::int32_t(meshes.size())); // vertex buffers
    f.Put<std::int32_t>(0x50, std::int32_t(facesets));
    f.Put<std::int32_t>(0x54, 1); // one layout
    const std::size_t materials = 0x80;
    const std::size_t mesh_base = materials + meshes.size() * 0x20;
    const std::size_t faceset_base = mesh_base + meshes.size() * 0x30;
    const std::size_t buffer_base = faceset_base + facesets * 0x20;
    const std::size_t layout_base = buffer_base + meshes.size() * 0x20;
    // The layout, then a texture entry for each mesh's material (a normal map, then its diffuse).
    const std::size_t texture_base = layout_base + 0x10;
    f.bytes.resize(texture_base + meshes.size() * 2 * 0x20);
    for (std::size_t m = 0; m < meshes.size(); ++m) {
        if (meshes[m].diffuse.empty()) {
            continue;
        }
        f.Put<std::int32_t>(materials + m * 0x20 + 8, 2);
        f.Put<std::int32_t>(materials + m * 0x20 + 0xC, std::int32_t(m * 2));
        const std::size_t normal = texture_base + m * 2 * 0x20, diffuse = normal + 0x20;
        f.Put<std::int32_t>(normal, std::int32_t(f.AddUtf16("N:\\tex\\" + meshes[m].diffuse + "_n.tif")));
        f.Put<std::int32_t>(normal + 4, std::int32_t(f.AddUtf16("g_BumpmapTexture")));
        f.Put<std::int32_t>(diffuse, std::int32_t(f.AddUtf16("N:\\tex\\" + meshes[m].diffuse + ".tif")));
        f.Put<std::int32_t>(diffuse + 4, std::int32_t(f.AddUtf16("g_DiffuseTexture")));
    }
    f.Align(4);
    // The layout: position (float3, semantic 0) at 0, then a normal (byte4, semantic 2) at 12.
    const std::size_t members = f.Size();
    f.Put<std::int32_t>(layout_base, 2);
    f.Put<std::int32_t>(layout_base + 0xC, std::int32_t(members));
    f.Add<std::int32_t>(0), f.Add<std::int32_t>(0), f.Add<std::uint32_t>(2), f.Add<std::uint32_t>(0), f.Add<std::int32_t>(0);
    f.Add<std::int32_t>(0), f.Add<std::int32_t>(12), f.Add<std::uint32_t>(0x11), f.Add<std::uint32_t>(2), f.Add<std::int32_t>(0);
    // Lists, names, then the data (offsets from the data's start).
    std::vector<std::size_t> faceset_lists, buffer_lists;
    std::size_t faceset_index = 0;
    for (std::size_t m = 0; m < meshes.size(); ++m) {
        faceset_lists.push_back(f.Size());
        for (std::size_t k = 0; k < meshes[m].facesets.size(); ++k) {
            f.Add<std::int32_t>(std::int32_t(faceset_index++));
        }
        buffer_lists.push_back(f.Add<std::int32_t>(std::int32_t(m)));
        f.Put<std::int32_t>(materials + m * 0x20 + 4, std::int32_t(f.AddUtf16("N:\\mtd\\" + meshes[m].mtd)));
    }
    f.Align(16);
    const std::size_t data = f.Size();
    f.Put<std::uint32_t>(0x0C, std::uint32_t(data));
    faceset_index = 0;
    for (std::size_t m = 0; m < meshes.size(); ++m) {
        const Mesh& mesh = meshes[m];
        const std::size_t at = mesh_base + m * 0x30;
        f.Put<std::int32_t>(at + 4, std::int32_t(m));
        f.Put<std::int32_t>(at + 0x20, std::int32_t(mesh.facesets.size()));
        f.Put<std::int32_t>(at + 0x24, std::int32_t(faceset_lists[m]));
        f.Put<std::int32_t>(at + 0x28, 1);
        f.Put<std::int32_t>(at + 0x2C, std::int32_t(buffer_lists[m]));
        for (const Faceset& fs : mesh.facesets) {
            const std::size_t entry = faceset_base + faceset_index++ * 0x20;
            const std::size_t indices = f.Size() - data;
            for (const std::uint16_t i : fs.indices) {
                f.Add<std::uint16_t>(i);
            }
            f.Align(16);
            f.Put<std::uint32_t>(entry, fs.flags);
            f.Put<std::uint8_t>(entry + 4, fs.strip ? 1 : 0);
            f.Put<std::int32_t>(entry + 8, std::int32_t(fs.indices.size()));
            f.Put<std::uint32_t>(entry + 0xC, std::uint32_t(indices));
            f.Put<std::int32_t>(entry + 0x18, 16);
        }
        const std::size_t vertices = f.Size() - data;
        for (const Vec3& v : mesh.vertices) {
            f.Add<Vec3>(v);
            f.Add<std::uint64_t>(0x7F7F7F7F7F7F7F7Full);
        }
        const std::size_t vb = buffer_base + m * 0x20;
        f.Put<std::int32_t>(vb + 4, 0);
        f.Put<std::int32_t>(vb + 8, 20);
        f.Put<std::int32_t>(vb + 0xC, std::int32_t(mesh.vertices.size()));
        f.Put<std::int32_t>(vb + 0x18, std::int32_t(mesh.vertices.size() * 20));
        f.Put<std::uint32_t>(vb + 0x1C, std::uint32_t(vertices));
    }
    return f.bytes;
}

bool Near(float a, float b, float within = 1e-3f) {
    return std::abs(a - b) <= within;
}

std::vector<std::uint8_t> ReadAll(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(file), {}};
}

const std::uint8_t* PixelAt(const Chart& chart, float x, float z) {
    const auto [px, py] = chart.PixelOf(x, z);
    const int ix = int(px), iy = int(py);
    if (ix < 0 || iy < 0 || ix >= chart.width || iy >= chart.height) {
        return nullptr;
    }
    return chart.rgba.data() + (std::size_t(iy) * std::size_t(chart.width) + std::size_t(ix)) * 4;
}

} // namespace

int main() {
    // The MSB: map pieces (an "m" part of an "m" model), characters ("c" parts) and points are
    // spots; anything else (an object's "o" part) is neither.
    {
        const auto msb = MakeMsb({"m000100", "c1000", "o0001"}, {{5, 6, 7}},
                                 {{"m000100_0000", 0, {1, 2, 3}, {0, 90, 0}, {1, 1, 2}},
                                  {"c1000_0000", 1, {10, 0, -10}, {}, {1, 1, 1}},
                                  {"o0001_0000", 2, {99, 99, 99}, {}, {1, 1, 1}}});
        const auto studio = ReadMapStudio(msb);
        assert(studio);
        assert(studio->pieces.size() == 1);
        const Piece& p = studio->pieces[0];
        assert(p.model == "m000100");
        assert(p.position == (Vec3{1, 2, 3}) && p.rotation == (Vec3{0, 90, 0}) && p.scale == (Vec3{1, 1, 2}));
        assert(studio->spots.size() == 2);
        assert(studio->spots[0] == (Vec3{10, 0, -10}) && studio->spots[1] == (Vec3{5, 6, 7}));
        assert(!ReadMapStudio(std::vector<std::uint8_t>(64, 0)));
    }

    // The FLVER: the highest detail's triangles (a list, and a strip with its restart index); the
    // lower levels and the water's mesh left out.
    {
        Mesh ground{"M[ARSN]_l.mtd",
                    {{0, 0, 0}, {1, 0, 0}, {0, 0, 1}, {1, 0, 1}},
                    {{0, false, {0, 1, 2, 1, 3, 2}}, {0x01000000, false, {0, 1, 2}}, {0x02000000, false, {0, 1, 2}}}};
        Mesh stairs{"M[ARSN].mtd", {{0, 1, 0}, {1, 1, 0}, {0, 2, 1}, {1, 2, 1}}, {{0, true, {0, 1, 2, 3, 0xFFFF, 0, 1, 2}}}};
        Mesh water{"M[ARSN]_water.mtd", {{0, 5, 0}, {9, 5, 0}, {0, 5, 9}}, {{0, false, {0, 1, 2}}}};
        // Kaniel 2026-10-18: alpha-cut decals (litter, grime) and grass cards are left out too.
        Mesh litter{"M[ARSN]_l_al.mtd", {{0, 6, 0}, {9, 6, 0}, {0, 6, 9}}, {{0, false, {0, 1, 2}}}};
        Mesh grass{"M[A]_l_e_grass.mtd", {{0, 7, 0}, {9, 7, 0}, {0, 7, 9}}, {{0, false, {0, 1, 2}}}};
        std::vector<float> out;
        std::vector<std::uint32_t> colors;
        assert(AppendTriangles(MakeFlver({ground, stairs, water, litter, grass}), out, &colors,
                               [](std::string_view) { return 0xFF808080u; }));
        // ground 2, stairs: (0 1 2) (1 2 3) then the restart drops the windows over it, then (0 1 2);
        // Kaniel 2026-10-18: the water's too, in its blue-grey; nothing of the litter or the grass.
        assert(out.size() == 9 * 6 && colors.size() == 6);
        assert(out[0] == 0 && out[3] == 1 && out[8] == 1); // the ground's first corner set
        for (std::size_t i = 1; i < out.size(); i += 3) {
            assert(out[i] < 6.0f); // nothing of the litter's or the grass's
        }
        assert(out[9 * 5 + 1] == 5.0f && colors[5] != 0xFF808080u); // the water, not its texture's colour
        const std::uint32_t blue_grey = colors[5];
        assert((blue_grey & 0xFF) > ((blue_grey >> 16) & 0xFF)); // bluer than red
        std::vector<float> none;
        assert(!AppendTriangles(std::vector<std::uint8_t>(0x100, 0), none) && none.empty());
    }

    // Kaniel 2026-10-17 (colour): each triangle's colour is its material's diffuse texture's; none
    // without one.
    {
        Mesh stone{"M[ARSN]_l.mtd", {{0, 0, 0}, {1, 0, 0}, {0, 0, 1}}, {{0, false, {0, 1, 2}}}, "m24_01_stone_a"};
        Mesh bare{"M[ARSN]_l.mtd", {{0, 0, 0}, {1, 0, 0}, {0, 0, 1}}, {{0, false, {0, 1, 2}}}};
        std::vector<float> out;
        std::vector<std::uint32_t> colors;
        std::vector<std::string> asked;
        assert(AppendTriangles(MakeFlver({stone, bare}), out, &colors, [&](std::string_view texture) {
            asked.emplace_back(texture);
            return texture == "m24_01_stone_a" ? 0xFF804020u : 0u;
        }));
        assert(colors == (std::vector<std::uint32_t>{0xFF804020u, 0u}));
        assert(asked == (std::vector<std::string>{"m24_01_stone_a"})); // once a material
        // Placing keeps each kept triangle's colour.
        std::vector<float> world;
        std::vector<std::uint32_t> world_colors;
        Place(Piece{"m000100", {}, {}, {1, 1, 1}}, out, PlayArea{-10, 10, -10, 10, 10}, world, colors, &world_colors);
        assert(world_colors == colors);
    }

    // A texture's average colour: a TPF of one 8x4 BC1 texture, a red block and a blue one.
    {
        Blob tpf;
        tpf.bytes = {'T', 'P', 'F', 0};
        tpf.Put<std::uint32_t>(8, 1);
        tpf.Put<std::uint32_t>(16 + 0x00, 0x60);                     // data offset
        tpf.Put<std::uint32_t>(16 + 0x04, 16);                       // data size
        tpf.Put<std::uint16_t>(16 + 12, 8);                          // width
        tpf.Put<std::uint16_t>(16 + 14, 4);                          // height
        tpf.Put<std::uint32_t>(16 + 0x18, 0x50);                     // its name
        tpf.Put<std::uint32_t>(16 + 0x20, 72);                       // BC1 sRGB
        tpf.Put<std::uint16_t>(0x50, 0);
        tpf.Put<std::uint16_t>(0x60, 0xF800), tpf.Put<std::uint16_t>(0x62, 0xF800); // red, indices 0
        tpf.Put<std::uint32_t>(0x64, 0);
        tpf.Put<std::uint16_t>(0x68, 0x001F), tpf.Put<std::uint16_t>(0x6A, 0x001F); // blue
        tpf.Put<std::uint32_t>(0x6C, 0);
        const std::uint32_t average = AverageColor(tpf.bytes);
        assert((average >> 24) == 0xFF);
        assert(((average >> 16) & 0xFF) > 120 && ((average >> 16) & 0xFF) < 135); // half red
        assert(((average >> 8) & 0xFF) == 0);
        assert((average & 0xFF) > 120 && (average & 0xFF) < 135); // half blue
        tpf.Put<std::uint32_t>(16 + 0x20, 77); // BC3: not read
        assert(AverageColor(tpf.bytes) == 0 && AverageColor(std::vector<std::uint8_t>(64, 0)) == 0);
    }

    // Kaniel 2026-10-18 ("different colours than the location"): the area's colour grade, a
    // 16x16x16 table in the PS4's tiled order; an identity table gives the colour back, a table
    // turning everything red gives red.
    {
        Blob tpf;
        tpf.bytes = {'T', 'P', 'F', 0};
        tpf.Put<std::uint32_t>(8, 1);
        tpf.Put<std::uint32_t>(16 + 0x00, 0x80);
        tpf.Put<std::uint32_t>(16 + 0x04, 16 * 256 * 4);
        tpf.Put<std::uint16_t>(16 + 12, 16);
        tpf.Put<std::uint16_t>(16 + 14, 256);
        tpf.Put<std::uint32_t>(16 + 0x18, 0x40);
        tpf.Put<std::uint32_t>(16 + 0x20, 28);
        std::size_t name = 0x40;
        for (const char c : std::string("m24_01_CGrading0001")) {
            tpf.Put<std::uint16_t>(name, std::uint16_t(c));
            name += 2;
        }
        tpf.Put<std::uint16_t>(name, 0);
        // Texel (column r, row b * 16 + g) at its tiled place: 8x8 tiles, Morton inside.
        for (int row = 0; row < 256; ++row) {
            for (int column = 0; column < 16; ++column) {
                int i = 0;
                for (int bit = 0; bit < 3; ++bit) {
                    i |= ((column >> bit) & 1) << (2 * bit);
                    i |= ((row % 8 >> bit) & 1) << (2 * bit + 1);
                }
                const std::size_t tile = std::size_t(row / 8) * 2 + std::size_t(column / 8);
                const std::size_t at = 0x80 + (tile * 64 + std::size_t(i)) * 4;
                tpf.Put<std::uint8_t>(at, std::uint8_t(column * 17));
                tpf.Put<std::uint8_t>(at + 1, std::uint8_t(row % 16 * 17));
                tpf.Put<std::uint8_t>(at + 2, std::uint8_t(row / 16 * 17));
                tpf.Put<std::uint8_t>(at + 3, 255);
            }
        }
        const auto grade = ReadColorGrade(tpf.bytes, "m24_01");
        assert(grade && grade->size() == 16 * 16 * 16 * 3);
        const float colour[3] = {200.0f, 100.0f, 30.0f};
        const auto same = Graded(*grade, colour);
        assert(std::abs(same[0] - 200.0f) < 1.0f && std::abs(same[1] - 100.0f) < 1.0f && std::abs(same[2] - 30.0f) < 1.0f);
        std::vector<std::uint8_t> red(grade->size(), 0);
        for (std::size_t i = 0; i < red.size(); i += 3) {
            red[i] = 255;
        }
        const auto all_red = Graded(red, colour);
        assert(all_red[0] > 254.0f && all_red[1] < 1.0f);
        assert(!ReadColorGrade(std::vector<std::uint8_t>(16, 0), "m24_01"));
    }

    // Kaniel 2026-10-19 ("look almost like in game but view from above"): a texture decoded (an
    // 8x4 BC1, red block then blue, in the PS4's tile order) and drawn on a triangle by its
    // texture coordinates: the left half of the ground red, the right half blue.
    {
        Blob tpf;
        tpf.bytes = {'T', 'P', 'F', 0};
        tpf.Put<std::uint32_t>(8, 1);
        tpf.Put<std::uint32_t>(16 + 0x00, 0x80);
        tpf.Put<std::uint32_t>(16 + 0x04, 64 * 8);
        tpf.Put<std::uint16_t>(16 + 12, 8);
        tpf.Put<std::uint16_t>(16 + 14, 4);
        tpf.Put<std::uint32_t>(16 + 0x18, 0x40);
        tpf.Put<std::uint32_t>(16 + 0x20, 71);
        tpf.Put<std::uint16_t>(0x40, 't');
        // Block (0, 0) first, block (1, 0) second in Morton order; all indices 0 (colour 0).
        tpf.Put<std::uint16_t>(0x80, 0xF800);
        tpf.Put<std::uint16_t>(0x82, 0x0000);
        tpf.Put<std::uint16_t>(0x88, 0x001F);
        tpf.Put<std::uint16_t>(0x8A, 0x0000);
        tpf.Put<std::uint8_t>(0x80 + 64 * 8 - 1, 0);
        const auto texture = DecodeTexture(tpf.bytes);
        assert(texture && texture->width == 8 && texture->height == 4);
        const std::uint32_t left = texture->Sample(0.1f, 0.5f), right = texture->Sample(0.9f, 0.5f);
        assert(((left >> 16) & 0xFF) > 200 && (left & 0xFF) < 50);
        assert(((right >> 16) & 0xFF) < 50 && (right & 0xFF) > 200);
        assert(texture->Sample(1.1f, 0.5f) == left); // wraps
        const auto shared = std::make_shared<const Surface>(Surface{std::make_shared<const Texture>(*texture), nullptr, nullptr, nullptr});
        Canvas painted(PlayArea{0, 20, 0, 10, 100}, 2.0f, 4096);
        painted.SetTextures([&](std::uint32_t id) { return id == 7 ? shared : nullptr; });
        const std::vector<float> ground{0, 0, 0, 20, 0, 0, 0, 0, 10, 20, 0, 0, 20, 0, 10, 0, 0, 10};
        // Each corner: its (u, v), then its light's (none here).
        const std::vector<float> uvs{0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 1, 0, 0, 0, 1, 1, 0, 0, 0, 1, 0, 0};
        painted.Add(ground, std::vector<std::uint32_t>(2, TexturedColor(7)), uvs);
        const Chart chart = painted.Finish();
        const std::uint8_t* west = PixelAt(chart, 3, 5);
        const std::uint8_t* east = PixelAt(chart, 17, 5);
        assert(west && east && west[0] > west[2] + 40 && east[2] > east[0] + 40);
    }

    // Kaniel 2026-10-19 ("can we prerender it?"): the 3D view's sides. Side 1 looks north: east
    // is to the right, what lies further north or higher is further up the picture, and what is
    // higher or further south is nearer. Side 3 looks south: east is to the left.
    {
        const ObliqueView north = ViewOf(1), south = ViewOf(3);
        const auto a = ToView(north, 10.0f, 0.0f, 0.0f), b = ToView(north, 0.0f, 0.0f, 10.0f),
                   c = ToView(north, 0.0f, 10.0f, 0.0f);
        assert(std::abs(a[0] - 10.0f) < 1e-3f && std::abs(a[1]) < 1e-3f);
        assert(b[1] > 1.0f && b[2] < -1.0f); // north: up the picture, further away
        assert(c[1] > 1.0f && c[2] > 1.0f);  // up: up the picture, nearer
        assert(ToView(south, 10.0f, 0.0f, 0.0f)[0] < -9.9f);
        assert(ViewOf(2).yaw > 1.5f && ViewOf(4).yaw > 4.7f && ViewOf(9).yaw == ViewOf(4).yaw);
        // A tall block drawn from side 1: its top shows above its foot, and the wall facing south
        // (toward the viewer) shows between them.
        Canvas side(PlayArea{-20, 20, -20, 30, 100}, 2.0f, 4096);
        side.SetOblique();
        std::vector<float> box{-5, 0, -5, 5, 0, -5, -5, 10, -5, 5, 0, -5, 5, 10, -5, -5, 10, -5,     // south wall
                               -5, 10, -5, 5, 10, -5, -5, 10, 5, 5, 10, -5, 5, 10, 5, -5, 10, 5}; // roof
        for (std::size_t i = 0; i + 3 <= box.size(); i += 3) {
            const auto t = ToView(north, box[i], box[i + 1], box[i + 2]);
            box[i] = t[0], box[i + 1] = t[2], box[i + 2] = t[1];
        }
        side.Add(box);
        const Chart drawn = side.Finish();
        const auto roof_at = ToView(north, 0.0f, 10.0f, 0.0f), wall_at = ToView(north, 0.0f, 5.0f, -5.0f);
        assert(PixelAt(drawn, roof_at[0], roof_at[1]) && PixelAt(drawn, roof_at[0], roof_at[1])[3] == 255);
        assert(PixelAt(drawn, wall_at[0], wall_at[1]) && PixelAt(drawn, wall_at[0], wall_at[1])[3] == 255);
    }

    // Kaniel 2026-10-19 (torches glowing): a light file's point lights (version 2, the lights after
    // the names, 200 bytes each), the spotlights left out.
    {
        Blob btl;
        btl.Put<std::int32_t>(0, 2);
        btl.Put<std::int32_t>(8, 2);
        btl.Put<std::int32_t>(0xC, 4); // the names' length
        btl.Put<std::int32_t>(0x14, 200);
        const std::size_t first = 0x3C + 4;
        for (int i = 0; i < 2; ++i) {
            const std::size_t at = first + std::size_t(i) * 200;
            btl.Put<std::uint8_t>(at + 0x1D, 0xFF);
            btl.Put<std::uint8_t>(at + 0x1E, 0xD2);
            btl.Put<std::uint8_t>(at + 0x1F, 0xB4);
            btl.Put<float>(at + 0x20, 2.0f);
            btl.Put<float>(at + 0x2C, i == 0 ? 0.0f : 22.0f); // the second is a spotlight
            btl.Put<float>(at + 0x38, -146.5f);
            btl.Put<float>(at + 0x3C, -57.25f);
            btl.Put<float>(at + 0x40, -129.5f);
            btl.Put<float>(at + 0x58, 6.0f);
        }
        btl.Put<std::uint8_t>(first + 2 * 200 - 1, 0);
        const auto lights = ReadLights(btl.bytes);
        assert(lights.size() == 1 && lights[0].color == 0xFFFFD2B4u && lights[0].x == -146.5f &&
               lights[0].y == -57.25f && lights[0].z == -129.5f && lights[0].radius == 6.0f && lights[0].power == 2.0f);
        assert(ReadLights(std::vector<std::uint8_t>(8, 0)).empty());
        // Its warm glow on the ground under it, none far away.
        Canvas lit(PlayArea{-160, -130, -140, -120, 100}, 2.0f, 4096);
        Canvas dark(PlayArea{-160, -130, -140, -120, 100}, 2.0f, 4096);
        const std::vector<float> ground{-160, -59, -140, -130, -59, -140, -160, -59, -120,
                                        -130, -59, -140, -130, -59, -120, -160, -59, -120};
        lit.Add(ground, std::vector<std::uint32_t>(2, 0xFF606060u));
        dark.Add(ground, std::vector<std::uint32_t>(2, 0xFF606060u));
        lit.AddLight(lights[0]);
        const Chart with = lit.Finish(), without = dark.Finish();
        const std::uint8_t* under = PixelAt(with, -146.5f, -129.5f);
        const std::uint8_t* plain = PixelAt(without, -146.5f, -129.5f);
        assert(under && plain && under[0] > plain[0] + 20 && under[0] - plain[0] > under[2] - plain[2]);
        assert(std::memcmp(PixelAt(with, -158.0f, -122.0f), PixelAt(without, -158.0f, -122.0f), 3) == 0);
    }

    // A texture archive's header: each file's name, offset and size in the .tpfbdt.
    {
        Blob bhd;
        bhd.bytes = {'B', 'H', 'F', '4'};
        bhd.Put<std::uint8_t>(0x30, 1); // UTF-16 names
        bhd.Put<std::int32_t>(0x0C, 2);
        bhd.Put<std::int64_t>(0x20, 0x24);
        bhd.bytes.resize(0x40 + 2 * 0x24);
        for (int i = 0; i < 2; ++i) {
            const std::size_t at = 0x40 + std::size_t(i) * 0x24;
            bhd.Put<std::int64_t>(at + 8, 100 + i);
            bhd.Put<std::uint32_t>(at + 0x18, 1000u * std::uint32_t(i + 1));
            bhd.Put<std::uint32_t>(at + 0x20, std::uint32_t(bhd.AddUtf16(i ? "b_a_l.tpf.dcx" : "a_a.tpf.dcx")));
        }
        const auto entries = ReadArchiveHeader(bhd.bytes);
        assert(entries.size() == 2 && entries[1].name == "b_a_l.tpf.dcx" && entries[1].offset == 2000 &&
               entries[1].size == 101);
        assert(ReadArchiveHeader(std::vector<std::uint8_t>(16, 0)).empty());
        // A repacked archive's names in single bytes (+0x30 0).
        Blob narrow = bhd;
        narrow.Put<std::uint8_t>(0x30, 0);
        const std::size_t name = narrow.Size();
        for (const char c : std::string("c_a.tpf.dcx")) {
            narrow.Add<char>(c);
        }
        narrow.Add<char>(0);
        narrow.Put<std::uint32_t>(0x40 + 0x20, std::uint32_t(name));
        assert(ReadArchiveHeader(narrow.bytes).at(0).name == "c_a.tpf.dcx");
    }

    // Models left out: the sky (m9xxxxx), the distant views (m008xxx), a quad of huge triangles.
    {
        const std::vector<float> small{0, 0, 0, 3, 0, 0, 0, 0, 3};
        const std::vector<float> huge{0, 0, 0, 30, 0, 0, 0, 0, 30};
        assert(SkippedModel("m900000", small) && SkippedModel("m008020", small));
        assert(!SkippedModel("m000100", small) && SkippedModel("m000100", huge));
        // Kaniel 2026-10-18 ("a lot of noise"): a prop within 1.5 m each way (a bottle, rubble).
        assert(SkippedModel("m000100", std::vector<float>{0, 0, 0, 1, 0, 0, 0, 1.4f, 1}));
        assert(!SkippedModel("m000100", std::vector<float>{0, 0, 0, 1, 0, 0, 0, 2, 1})); // a tall post stays
        assert(SkippedModel("m000100", {}));
        std::vector<float> many;
        for (int i = 0; i < 70; ++i) {
            many.insert(many.end(), huge.begin(), huge.end());
        }
        assert(!SkippedModel("m000100", many)); // a big model's big floor stays
        // A backdrop: 70 triangles spread over a kilometre square (the Dream's sea of flowers).
        std::vector<float> backdrop = many;
        backdrop.insert(backdrop.end(), {0, 0, 0, 1000, 0, 0, 0, 0, 1000});
        assert(SkippedModel("m002000", backdrop));
    }

    // The play area: the middle of the spots, 40 m round, the ceiling 25 m over the highest.
    {
        std::vector<Vec3> spots;
        for (int i = 0; i <= 100; ++i) {
            spots.push_back({float(i), float(i % 10), float(-i)});
        }
        spots.push_back({5000, 0, 0}); // a stray point far out
        const auto area = PlayAreaOf(spots);
        assert(area && Near(area->x0, -39.0f) && area->x1 < 200.0f && Near(area->z1, 40.0f));
        assert(Near(area->ceiling, 34.0f));
        assert(!PlayAreaOf({}));
    }

    // Placing: scale, rotation (y by 90 degrees turns +x to -z), then the position; triangles out of
    // the play area or above its ceiling stay out.
    {
        const PlayArea area{-100, 100, -100, 100, 50};
        const std::vector<float> model{1, 0, 0, 2, 0, 0, 1, 0, 1};
        std::vector<float> out;
        Place(Piece{"m000100", {10, 1, 20}, {0, 90, 0}, {2, 1, 1}}, model, area, out);
        assert(out.size() == 9);
        assert(Near(out[0], 10.0f) && Near(out[1], 1.0f) && Near(out[2], 18.0f)); // (2, 0, 0) -> (0, 0, -2)
        assert(Near(out[3], 10.0f) && Near(out[5], 16.0f));
        assert(Near(out[6], 11.0f) && Near(out[8], 18.0f)); // (2, 0, 1) -> (1, 0, -2)
        out.clear();
        Place(Piece{"m000100", {500, 0, 0}, {}, {1, 1, 1}}, model, area, out);
        Place(Piece{"m000100", {0, 60, 0}, {}, {1, 1, 1}}, model, area, out);
        assert(out.empty());
        // About x by 90 degrees: +y goes to +z (the order x, z, y as the map tools turn).
        Place(Piece{"m000100", {}, {90, 0, 0}, {1, 1, 1}}, std::vector<float>{0, 1, 0, 0, 1, 0, 0, 1, 0}, area, out);
        assert(Near(out[0], 0.0f) && Near(out[1], 0.0f) && Near(out[2], 1.0f));
    }

    // The picture: north up, the highest surface wins, nothing is transparent, the shading lighter
    // on high ground than low.
    {
        const PlayArea area{0, 30, 0, 10, 100};
        Canvas canvas(area, 2.0f, 4096);
        // Low ground over the west half, a roof 6 m up over the east half's north.
        canvas.Add(std::vector<float>{0, 0, 0, 10, 0, 0, 0, 0, 10, 10, 0, 0, 10, 0, 10, 0, 0, 10});
        canvas.Add(std::vector<float>{12, 6, 6, 18, 6, 6, 12, 6, 9, 18, 6, 6, 18, 6, 9, 12, 6, 9});
        canvas.Add(std::vector<float>{12, -3, 6, 18, -3, 6, 12, -3, 9}); // under the roof: hidden
        const Chart chart = canvas.Finish();
        // Cropped to what is drawn: the east edge's empty strip goes.
        assert(chart.width < 50 && chart.height == 20 && Near(chart.ppm, 2.0f));
        assert(Near(chart.PixelOf(0, 10)[0], 0.0f) && Near(chart.PixelOf(0, 10)[1], 0.0f)); // north-west corner
        const std::uint8_t* low = PixelAt(chart, 5, 5);
        const std::uint8_t* roof = PixelAt(chart, 15, 8);
        const std::uint8_t* south_east = PixelAt(chart, 15, 2);
        assert(low && low[3] == 255 && roof && roof[3] == 255);
        assert(south_east && south_east[3] == 0);
        assert(roof[0] > low[0]); // pale high ground, darker low ground
        // Kaniel 2026-10-18 ("a lot of noise"): a lone speck of another colour on a surface takes its
        // surroundings' colour, and a lone pixel standing up from it (a bottle) is flattened.
        Canvas specks(area, 2.0f, 4096);
        specks.Add(std::vector<float>{0, 0, 0, 10, 0, 0, 0, 0, 10, 10, 0, 0, 10, 0, 10, 0, 0, 10},
                   std::vector<std::uint32_t>{0xFF208020u, 0xFF208020u});
        specks.Add(std::vector<float>{5, 0.2f, 5, 5.3f, 0.2f, 5, 5, 0.2f, 5.3f}, std::vector<std::uint32_t>{0xFFC02020u});
        specks.Add(std::vector<float>{2.1f, 1.2f, 2.1f, 2.3f, 1.2f, 2.1f, 2.1f, 1.2f, 2.3f},
                   std::vector<std::uint32_t>{0xFF208020u});
        const Chart calm = specks.Finish();
        const std::uint8_t* speck = PixelAt(calm, 5.1f, 5.1f);
        assert(speck && speck[1] > speck[0]); // green, not red
        const std::uint8_t* bottle = PixelAt(calm, 2.2f, 2.2f);
        const std::uint8_t* ground = PixelAt(calm, 3.0f, 3.0f);
        assert(bottle && ground && std::abs(int(bottle[0]) - int(ground[0])) < 12); // no bright dot
        // Kaniel 2026-10-18 ("sometimes roofs, sometimes floors"): a red-roofed house (6 m up, 5 to
        // 15 m, walkable) on green ground, walls 2 m thick round a floor of grey stone; the roof
        // shows from outside and from on it, the floor from under it, inked where it ends (walls).
        {
            const PlayArea yard{0, 20, 0, 20, 100};
            Canvas house(yard, 2.0f, 4096);
            const auto square = [](float a, float b, float y) {
                return std::vector<float>{a, y, a, b, y, a, a, y, b, b, y, a, b, y, b, a, y, b};
            };
            std::vector<float> ground;
            for (const auto& part : {std::array{0.0f, 20.0f, 0.0f, 5.0f}, std::array{0.0f, 20.0f, 15.0f, 20.0f},
                                     std::array{0.0f, 5.0f, 5.0f, 15.0f}, std::array{15.0f, 20.0f, 5.0f, 15.0f}}) {
                const std::vector<float> quad{part[0], 0, part[2], part[1], 0, part[2], part[0], 0, part[3],
                                              part[1], 0, part[2], part[1], 0, part[3], part[0], 0, part[3]};
                ground.insert(ground.end(), quad.begin(), quad.end());
            }
            house.AddWalkable(ground);
            house.AddWalkable(square(7, 13, 0.2f)); // the floor inside the walls
            house.AddWalkable(square(5, 15, 6.0f)); // the roof
            house.Add(ground, std::vector<std::uint32_t>(8, 0xFF20A020u));
            house.Add(square(7, 13, 0.2f), std::vector<std::uint32_t>(2, 0xFF808080u));
            house.Add(square(5, 15, 6.0f), std::vector<std::uint32_t>(2, 0xFFC02020u));
            const Chart chart = house.Finish();
            const std::uint8_t* roof = PixelAt(chart, 10, 10);
            assert(roof && roof[0] > roof[1] + 20); // red from above
            assert(UnderRoof(chart, 10, 0.2f, 10) && !UnderRoof(chart, 2, 0, 2) && !UnderRoof(chart, 10, 6, 10));
            std::vector<std::uint8_t> inside = chart.rgba;
            FloorView(chart, 0.2f, inside.data());
            const auto at = [&](float x, float z) {
                const auto [px, py] = chart.PixelOf(x, z);
                return inside.data() + (std::size_t(py) * std::size_t(chart.width) + std::size_t(px)) * 4;
            };
            const std::uint8_t* floor = at(10.1f, 10.1f);
            assert(std::abs(int(floor[0]) - int(floor[1])) < 20 && std::abs(int(floor[1]) - int(floor[2])) < 25); // grey
            assert(std::memcmp(at(2, 2), PixelAt(chart, 2, 2), 4) == 0); // outside: as it was
            const std::uint8_t* wall = at(7.1f, 10.1f);                    // the floor's edge by the wall
            assert(wall[0] * 4 < floor[0] * 3);                            // inked
            // Standing on the roof: the roof.
            std::vector<std::uint8_t> above = chart.rgba;
            FloorView(chart, 6.0f, above.data());
            assert(above == chart.rgba);
            // Kaniel 2026-10-19 (the 3D view): the heights as shown: inside, the floor; on the
            // roof or with the roofs shown, the roof.
            const auto [hx, hy] = chart.PixelOf(10.1f, 10.1f);
            const std::size_t cell = std::size_t(int(hy) / Chart::FloorCell) * std::size_t(chart.storeys.width) +
                                     std::size_t(int(hx) / Chart::FloorCell);
            assert(ShownHeight(chart, cell, 0.2f) == 2 && ShownHeight(chart, cell, 6.0f) == 60 &&
                   ShownHeight(chart, cell, NAN) == 60);
        }
        // Kaniel 2026-10-18 ("extra stuff outside the map"): with walkable floors given, what lies
        // more than Canvas::Reach metres from them goes; what is near them stays.
        {
            const PlayArea wide{0, 60, 0, 10, 100};
            Canvas outline(wide, 2.0f, 4096);
            outline.Add(std::vector<float>{0, 0, 0, 60, 0, 0, 0, 0, 10, 60, 0, 0, 60, 0, 10, 0, 0, 10});
            outline.AddWalkable(std::vector<float>{0, 0, 0, 10, 0, 0, 0, 0, 10, 10, 0, 0, 10, 0, 10, 0, 0, 10});
            const Chart cut = outline.Finish();
            assert(PixelAt(cut, 5, 5) && PixelAt(cut, 5, 5)[3] == 255);
            assert(PixelAt(cut, 20, 5) && PixelAt(cut, 20, 5)[3] == 255);  // 10 m off: near enough
            assert(!PixelAt(cut, 40, 5) || PixelAt(cut, 40, 5)[3] == 0);   // 30 m off: gone
        }
        // Shadows: the ground south-east of a tall block is darker than the ground north-west of it.
        {
            const PlayArea yard{0, 40, 0, 40, 100};
            Canvas shade(yard, 2.0f, 4096);
            shade.Add(std::vector<float>{0, 0, 0, 40, 0, 0, 0, 0, 40, 40, 0, 0, 40, 0, 40, 0, 0, 40});
            shade.Add(std::vector<float>{18, 8, 18, 22, 8, 18, 18, 8, 22, 22, 8, 18, 22, 8, 22, 18, 8, 22});
            const Chart lit = shade.Finish();
            const std::uint8_t* behind = PixelAt(lit, 24, 16); // south-east of the block, its shadow
            const std::uint8_t* before = PixelAt(lit, 16, 24); // north-west, in the light
            assert(behind && before && behind[0] + 10 < before[0]);
        }
        // A green material's ground comes out green.
        Canvas green(area, 2.0f, 4096);
        green.Add(std::vector<float>{0, 0, 0, 10, 0, 0, 0, 0, 10}, std::vector<std::uint32_t>{0xFF208020u});
        const Chart painted = green.Finish();
        const std::uint8_t* leaf = PixelAt(painted, 2, 2);
        assert(leaf && leaf[1] > leaf[0] + 20 && leaf[1] > leaf[2] + 20);
        // Two canvases of one area (two threads' models) merge: the higher surface wins.
        Canvas first(area, 2.0f, 4096), second(area, 2.0f, 4096);
        first.Add(std::vector<float>{0, 0, 0, 10, 0, 0, 0, 0, 10, 10, 0, 0, 10, 0, 10, 0, 0, 10});
        second.Add(std::vector<float>{12, 6, 6, 18, 6, 6, 12, 6, 9, 18, 6, 6, 18, 6, 9, 12, 6, 9});
        second.Add(std::vector<float>{0, -3, 0, 10, -3, 0, 0, -3, 10}); // under the first's ground
        first.Merge(second);
        const Chart merged = first.Finish();
        assert(merged.rgba == chart.rgba);
        // A picture wider than max_side has fewer pixels a metre.
        Canvas wide(PlayArea{0, 1000, 0, 100, 10}, 3.0f, 500);
        assert(wide.Finish().width == 500);
    }

    // The file in the user folder round trips; anything else is no chart.
    {
        Chart chart;
        chart.width = 3;
        chart.height = 2;
        chart.x0 = -5.5f;
        chart.z1 = 7.25f;
        chart.ppm = 2.5f;
        for (int i = 0; i < 24; ++i) {
            chart.rgba.push_back(std::uint8_t(i * 10));
        }
        chart.labels = {{2411, -219.5f, 182.25f}, {2421, 54.0f, 150.75f}};
        chart.storeys.width = 2;
        chart.storeys.height = 1;
        chart.storeys.roof = {60, Chart::NoHeight};
        chart.storeys.level = {50, 0, Chart::NoHeight, Chart::NoHeight, Chart::NoHeight, Chart::NoHeight};
        chart.storeys.rgb = {0xFF102030u, 0xFF405060u, 0, 0, 0, 0};
        const auto file = Save(chart);
        const auto back = Load(file);
        assert(back && back->labels == chart.labels && back->storeys == chart.storeys);
        assert(back && back->width == 3 && back->height == 2 && back->x0 == -5.5f && back->z1 == 7.25f &&
               back->ppm == 2.5f && back->rgba == chart.rgba);
        auto broken = file;
        broken[0] = 'X';
        assert(!Load(broken));
        assert(!Load(std::vector<std::uint8_t>(file.begin(), file.begin() + 40)));
    }

    assert(MapNameOf(0x18010000) == "m24_01_00_00" && MapNameOf(0x15000000) == "m21_00_00_00");

    // The game's own Central Yharnam: its lamps stand on the picture.
    if (const char* eboot = std::getenv("BLOODBORNE_EBOOT")) {
        const auto dvdroot = std::filesystem::path(eboot).parent_path() / "dvdroot_ps4";
        if (std::filesystem::exists(dvdroot / "map" / "MapStudio" / "m24_01_00_00.msb.dcx")) {
            const auto chart = ChartArea(
                0x18010000, [&](const std::string& relative) { return ReadAll(dvdroot / relative); }, 2.0f, 2048,
                4, [&](const std::string& relative, std::uint64_t offset, std::uint64_t size) {
                    std::ifstream file(dvdroot / relative, std::ios::binary);
                    std::vector<std::uint8_t> bytes(size);
                    file.seekg(std::streamoff(offset));
                    file.read(reinterpret_cast<char*>(bytes.data()), std::streamsize(size));
                    return bytes;
                });
            assert(chart && chart->width > 500 && chart->height > 500);
            // 1st Floor Sickroom, Central Yharnam, Great Bridge (bloodborne_lamp_points.h).
            for (const auto& [x, z] : {std::pair{-199.74f, 179.42f}, {-193.4f, 68.5f}, {-124.8f, 64.3f}}) {
                const std::uint8_t* px = PixelAt(*chart, x, z);
                assert(px && px[3] == 255);
            }
            // Iosefka's Clinic (item FMG 19, 2411) named where its floors lie, near its lamp.
            bool clinic = false;
            for (const Chart::Label& label : chart->labels) {
                clinic = clinic || (label.name_id == 2411 && std::abs(label.x + 199.7f) < 40.0f &&
                                    std::abs(label.z - 179.4f) < 40.0f);
            }
            assert(clinic);
            // In colour: the copper roofs and the moss are far from the paper's brown.
            std::size_t coloured = 0;
            for (std::size_t i = 0; i < chart->rgba.size(); i += 4) {
                const int r = chart->rgba[i], g = chart->rgba[i + 1], b = chart->rgba[i + 2];
                coloured += chart->rgba[i + 3] != 0 && (b > r + 10 || g > r + 10);
            }
            assert(coloured > 1000);
            std::printf("map-art: Central Yharnam drawn %dx%d, %zu pixels blue or green\n", chart->width,
                        chart->height, coloured);
        }
    }
    // The game's collision floors: one under the Grand Cathedral's lamp, at its height.
    if (const char* eboot = std::getenv("BLOODBORNE_EBOOT")) {
        const auto dir = std::filesystem::path(eboot).parent_path() / "dvdroot_ps4" / "map" / "m24_00_00_00";
        if (std::filesystem::exists(dir / "h24_00_00_00.hkxbhd")) {
            const auto bhd = ReadAll(dir / "h24_00_00_00.hkxbhd"), bdt = ReadAll(dir / "h24_00_00_00.hkxbdt");
            std::size_t meshes = 0, floors = 0;
            bool under_lamp = false;
            for (const ArchiveEntry& entry : ReadArchiveHeader(bhd)) {
                const std::vector<std::uint8_t> packed(bdt.begin() + std::ptrdiff_t(entry.offset),
                                                       bdt.begin() + std::ptrdiff_t(entry.offset + entry.size));
                const auto hkx = BbGameArt::InflateDcx(packed);
                std::vector<float> triangles;
                if (hkx && AppendCollisionTriangles(*hkx, triangles)) {
                    ++meshes;
                    const auto flat = Floors(triangles);
                    floors += flat.size() / 9;
                    for (std::size_t t = 0; t + 9 <= flat.size(); t += 9) {
                        const float cx = (flat[t] + flat[t + 3] + flat[t + 6]) / 3,
                                    cz = (flat[t + 2] + flat[t + 5] + flat[t + 8]) / 3;
                        under_lamp = under_lamp || (std::abs(cx - 67.85f) < 3 && std::abs(cz - 339.75f) < 3 &&
                                                    std::abs(flat[t + 1] - 35.71f) < 2);
                    }
                }
            }
            std::printf("map-art: Cathedral Ward collision: %zu meshes, %zu floor triangles\n", meshes, floors);
            assert(meshes > 10 && floors > 10000 && under_lamp);
        }
    }
    std::printf("map-art: all passed\n");
    return 0;
}
