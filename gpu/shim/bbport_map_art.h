// SPDX-License-Identifier: GPL-2.0-or-later
// bbport co-op: the map screen's pictures of the areas (Kaniel 2026-10-17: an Elden Ring style map).
// Bloodborne has no map, so each area's is drawn from the player's own game files the first time
// it is wanted and kept in the user folder; nothing of the game's is shipped. An area's MSB
// (map/MapStudio/mAA_BB_00_00.msb.dcx) places its map pieces, each a FLVER model
// (map/mAA_BB_00_00/mAA_BB_00_00_NNNNNN.flver.dcx); their triangles seen from above, the highest
// one at each pixel, shaded by its slope and height, make a picture of the area in ink on paper.
// Positions are the game's metres on that map: x east-west, y up, z north-south; the picture has
// +z up and +x right, as the camera's heading turns (bloodborne_compass.h: 0 looks along +z,
// turning right goes toward +x).
#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace BbMapArt {

using Vec3 = std::array<float, 3>;

/// A map piece placed by the MSB: its model ("m000100"), where it stands, its rotation in degrees
/// about x, y and z, and its scale.
struct Piece {
    std::string model;
    Vec3 position{}, rotation{}, scale{1.0f, 1.0f, 1.0f};
};
/// What an area's MSB says for its picture: the map pieces, and the spots players reach (its
/// characters and its points: where the picture is wanted).
struct MapStudio {
    std::vector<Piece> pieces;
    std::vector<Vec3> spots;
    /// Kaniel 2026-10-18: the collision parts ("h" parts of "h" models: the floors and walls one
    /// walks on), each with the place name the game shows on entering it (item FMG 19, its
    /// subtype data's +8; 0 none).
    struct Collision {
        Piece piece;
        std::int32_t place_name = 0;
    };
    std::vector<Collision> collisions;
};
/// The MSB (inflated); nothing if it is not one.
std::optional<MapStudio> ReadMapStudio(std::span<const std::uint8_t> msb);

/// A material's colour by its textures' names: the diffuse texture's ("m24_01_trims_003_a"), and
/// when the material has them, after ';' its second diffuse texture, its blend mask and its baked
/// light (the DOL texture, "24010000_m000100_gi_0000_00_dol_00"), each possibly empty. 0xFFRRGGBB,
/// or 0 when it has none; or TexturedColor(id), a surface of the canvas's (Canvas::SetTextures).
using MaterialColor = std::function<std::uint32_t(std::string_view textures)>;
/// Kaniel 2026-10-19 ("look almost like in game but view from above"): a triangle drawn with its
/// texture `id` instead of one colour.
constexpr std::uint32_t TexturedColor(std::uint32_t id) {
    return 0x01000000u | (id & 0xFFFFFFu);
}
constexpr bool IsTextured(std::uint32_t color) {
    return (color >> 24) == 0x01;
}
/// A texture's pixels (a .tpf inflated, its first texture: BC1 or BC7, halved down to at most
/// `max_side` pixels a side), 0xAARRGGBB rows; Sample wraps (u, v) as the game's samplers do.
struct Texture {
    int width = 0, height = 0;
    std::vector<std::uint32_t> pixels;
    /// The smaller levels, each half the one before (a far surface takes a smaller one, so its
    /// detail does not flicker into noise).
    std::vector<Texture> mips;
    std::uint32_t Sample(float u, float v, int level = 0) const;
    /// The level for `texels` texels a picture pixel.
    int LevelFor(float texels) const;
};
/// Kaniel 2026-10-19: what a textured triangle is drawn with: its diffuse texture, a second one
/// mixed in by its blend mask (moss, mud, damage over stone), and the light the game baked for
/// its model (sampled at the second texture coordinates).
struct Surface {
    std::shared_ptr<const Texture> diffuse, diffuse2, mask, light;
};
/// A surface's colour at a corner (its first and second texture coordinates, four floats).
std::uint32_t SurfaceColor(const Surface& surface, const float* uv);
std::optional<Texture> DecodeTexture(std::span<const std::uint8_t> tpf, int max_side = 256);

/// The highest-detail triangles of a FLVER2 model (inflated), three corners of x, y, z each,
/// appended to `out` in the model's space; with `colors`, each triangle's material colour
/// (`color_of` its diffuse texture) appended to it. Left out: the lower levels of detail and the
/// meshes whose material is the sky, the moon, the sun or water (a flat disc over a river). False
/// if it is not a FLVER.
/// With `uvs`, each triangle's texture coordinates appended to it: for each corner its first
/// (u, v) and its second (the baked light's), twelve a triangle.
bool AppendTriangles(std::span<const std::uint8_t> flver, std::vector<float>& out,
                     std::vector<std::uint32_t>* colors = nullptr, const MaterialColor& color_of = {},
                     std::vector<float>* uvs = nullptr);

/// A collision mesh's triangles (a Havok 2014 packfile of compressed meshes, inflated: one file of
/// map/mAA_BB_00_00/hAA_BB_00_00.hkxbdt), three corners of x, y, z each, appended to `out` in the
/// part's space. False if it is not one.
bool AppendCollisionTriangles(std::span<const std::uint8_t> hkx, std::vector<float>& out);

/// The upward faces of triangles (floors and ramps one can stand on: the normal within about 63
/// degrees of up), the rest left out.
std::vector<float> Floors(std::span<const float> triangles);

/// The colour grade the game draws an area with (map/mAA/mAA_cgrading.tpf.dcx, its
/// "mAA_BB_CGrading0001": a 16x16x16 table, blue slices of green rows of red, in the PS4's tiled
/// order), as 16x16x16 RGB in plain order; nothing when the area has none.
std::optional<std::vector<std::uint8_t>> ReadColorGrade(std::span<const std::uint8_t> tpf, const std::string& block);
/// `rgb` through a grade (trilinear).
std::array<float, 3> Graded(std::span<const std::uint8_t> grade, const float rgb[3]);

/// A texture's average colour (a .tpf inflated, its first texture: BC1 or BC7, opaque pixels
/// only) as 0xFFRRGGBB; 0 when it is not one of those.
std::uint32_t AverageColor(std::span<const std::uint8_t> tpf);

/// Kaniel 2026-10-19 (torches and lamps glowing): a point light of an area's light file
/// (map/mAA_BB_00_00/mAA_BB_00_00_0000.btl.dcx, inflated): where it is, its colour (0xFFRRGGBB),
/// its reach in metres and its power. Spotlights and the sky's lights are left out.
struct PointLight {
    float x = 0, y = 0, z = 0;
    std::uint32_t color = 0;
    float radius = 0, power = 0;
    bool operator==(const PointLight&) const = default;
};
std::vector<PointLight> ReadLights(std::span<const std::uint8_t> btl);

/// A texture archive's header (BHF4, a .tpfbhd): each file's name and where it is in the .tpfbdt.
struct ArchiveEntry {
    std::string name;
    std::uint64_t offset = 0, size = 0;
};
std::vector<ArchiveEntry> ReadArchiveHeader(std::span<const std::uint8_t> bhd);

/// Models left out of the picture: the sky domes (m9xxxxx), the distant views (m008xxx, m009xxx)
/// and loose effect quads (a few huge flat triangles: fog, light shafts).
bool SkippedModel(std::string_view model, std::span<const float> triangles);

/// Where the picture is wanted: the middle of the spots (the outer 1% left out each way) and 40 m
/// round them, and a ceiling 25 m over the highest spots (a roof over the play area stays, the
/// cloud ring and the far towers go).
struct PlayArea {
    float x0 = 0, x1 = 0, z0 = 0, z1 = 0, ceiling = 0;
};
std::optional<PlayArea> PlayAreaOf(std::span<const Vec3> spots);

/// The piece's model triangles (`model`, its space) put where the piece stands, appended to `out`;
/// the ones whose middle is outside the play area or wholly above its ceiling are left out. With
/// `colors` (one a triangle) the kept ones' go to `out_colors`.
void Place(const Piece& piece, std::span<const float> model, const PlayArea& area, std::vector<float>& out,
           std::span<const std::uint32_t> colors = {}, std::vector<std::uint32_t>* out_colors = nullptr,
           std::span<const float> uvs = {}, std::vector<float>* out_uvs = nullptr);

/// An area's picture: RGBA rows, `ppm` pixels a metre; the pixel (px, py) shows x = x0 + px / ppm,
/// z = z1 - py / ppm (north up). Transparent where nothing is.
struct Chart {
    int width = 0, height = 0;
    float x0 = 0, z1 = 0, ppm = 1;
    std::vector<std::uint8_t> rgba;
    /// Kaniel 2026-10-18: the places named in it (item FMG 19 ids) where their floors lie.
    struct Label {
        std::int32_t name_id = 0;
        float x = 0, z = 0;
        bool operator==(const Label&) const = default;
    };
    std::vector<Label> labels;
    /// Kaniel 2026-10-18 ("sometimes roofs, sometimes floors"): the storeys under each cell of
    /// FloorCell x FloorCell pixels: the picture's top there and up to Levels walkable floors
    /// (the collision meshes'), highest first, each with its finished colour, so the map can show
    /// the floor one stands on where a roof covers it (FloorView). Empty when not drawn.
    static constexpr int FloorCell = 2, Levels = 3;
    static constexpr std::int16_t NoHeight = INT16_MIN;
    struct Storeys {
        int width = 0, height = 0;         // in cells
        std::vector<std::int16_t> roof;    // decimetres, NoHeight none
        std::vector<std::int16_t> level;   // Levels a cell, decimetres, NoHeight none
        std::vector<std::uint32_t> rgb;    // Levels a cell, 0xFFRRGGBB
        bool operator==(const Storeys&) const = default;
    };
    Storeys storeys;
    /// Where the world spot (x, z) is in the picture, in pixels.
    std::array<float, 2> PixelOf(float x, float z) const { return {(x - x0) * ppm, (z1 - z) * ppm}; }
};

/// The chart cut to what is drawn in it (a few pixels round it); its frame follows.
Chart Cropped(const Chart& chart);
/// Kaniel 2026-10-19 ("increase the quality of the render"): the picture at half its size, each
/// pixel the four it covers blended (drawn at twice the detail, its edges come out smooth). The
/// storeys are left out.
Chart Halved(const Chart& chart);

/// Kaniel 2026-10-18: a roof (the picture's top) more than 3 m over feet at height `y` at (x, z):
/// the player is indoors, under a bridge or under trees, and the map shows floors.
bool UnderRoof(const Chart& chart, float x, float y, float z);
/// The picture as one standing at height `y` sees it: each cell's storey is its highest floor up
/// to 1.5 m above `y`; where the picture's top lies more than 2.5 m above that floor (a roof, a
/// bridge, a tree), the floor shows instead, inked where its storey ends (walls). `rgba` is the
/// chart's own picture to change (its size).
void FloorView(const Chart& chart, float y, std::uint8_t* rgba);
/// The height FloorView shows at a storeys' cell for one standing at `y` (NaN: the roofs): the
/// storey's floor where a roof covers it, else the top; Chart::NoHeight where nothing is.
std::int16_t ShownHeight(const Chart& chart, std::size_t cell, float y);

/// The picture drawn up: world triangles added a model at a time (an area has tens of millions),
/// the highest at each pixel kept with its slope's light; Finish shades it in ink on paper. `ppm`
/// pixels a metre, fewer when the play area would be wider or taller than `max_side`; the picture
/// cropped to what was drawn.
class Canvas {
public:
    Canvas(const PlayArea& area, float ppm, int max_side);
    /// `colors`: each triangle's material colour (0xFFRRGGBB, 0 none; TexturedColor: its surface,
    /// sampled at `uvs`, twelve a triangle); empty: none.
    void Add(std::span<const float> triangles, std::span<const std::uint32_t> colors = {},
             std::span<const float> uvs = {});
    /// The surfaces TexturedColor ids name (looked up once a triangle; safe from several threads).
    void SetTextures(std::function<std::shared_ptr<const Surface>(std::uint32_t id)> texture_of) {
        this->texture_of = std::move(texture_of);
        textured = true;
    }
    /// Kaniel 2026-10-18 ("extra stuff outside the map"): walkable floors (Floors of the collision
    /// meshes). Once any are added, Finish leaves out what lies more than `reach` metres from all
    /// of them (distant views, floating rooftops beyond the walls).
    void AddWalkable(std::span<const float> floors);
    /// A light whose warm glow Finish lays on the surfaces round it (the warm, small ones: torches,
    /// lamps, candles).
    void AddLight(const PointLight& light) { lights.push_back(light); }
    /// An oblique picture (ChartAreas' view): no shadows from above.
    void SetOblique() { oblique = true; }
    /// The game's colour grade for the picture (ReadColorGrade); none: the materials' own colours.
    void SetGrade(std::vector<std::uint8_t> grade) { this->grade = std::move(grade); }
    static constexpr float Reach = 12.0f;
    /// Another canvas of the same play area and size drawn into this one (the higher surface wins):
    /// an area's models drawn by several threads at once.
    void Merge(const Canvas& other);
    /// The storeys AddWalkable found on `other` (canvas 0) for this one too: the threads' canvases
    /// colour them as they draw.
    void ShareFloors(const Canvas& other);
    Chart Finish() const;

private:
    // The storeys (Chart::Storeys) under each cell as drawn: floor heights (-inf none; one table
    // for the canvases of all threads, copies share it), and the drawn surface found at each (its
    // height in decimetres, its light and colour).
    void AddLevel(std::size_t cell, float h);
    int cells_w = 0, cells_h = 0;
    std::shared_ptr<std::vector<float>> level;
    std::vector<std::int16_t> level_top;
    std::vector<std::uint8_t> level_light;
    std::vector<std::uint32_t> level_color;
    int width = 0, height = 0;
    float x0 = 0, z1 = 0, ppm = 1;
    std::vector<float> top;          // the highest surface's height at each pixel (-inf: none)
    std::vector<std::uint8_t> light; // its slope's light, 0 to 255
    std::vector<std::uint32_t> color; // its material's colour (0 none)
    std::vector<std::uint8_t> walk;   // a walkable floor there (AddWalkable)
    bool has_walk = false;
    std::function<std::shared_ptr<const Surface>(std::uint32_t)> texture_of;
    std::vector<PointLight> lights;
    bool oblique = false;
    bool textured = false; // colours are the textures' own pixels: kept as they are
    std::vector<std::uint8_t> grade;
};

/// A chart as kept in the user folder ("BBMAP9": the number goes up whenever the drawing changes,
/// so pictures drawn the old way are drawn again; the frame, the pixels zlib-compressed, the
/// labels, then the storeys zlib-compressed), and
/// read back (nothing when it is not one).
std::vector<std::uint8_t> Save(const Chart& chart);
std::optional<Chart> Load(std::span<const std::uint8_t> file);

/// The folder name of a packed map (0x18010000 -> "m24_01_00_00").
std::string MapNameOf(std::uint32_t packed_map);

/// An area's picture from the game's files: `read` gives a file of dvdroot_ps4 by its relative
/// path ("map/MapStudio/m24_01_00_00.msb.dcx"), inflated or not as the file is, empty when it is
/// missing (called from `threads` threads at once); `read_range` the bytes [offset, offset + size)
/// of one (the texture archives are hundreds of megabytes). Nothing when the MSB is missing or
/// places nothing.
using ReadFile = std::function<std::vector<std::uint8_t>(const std::string& relative)>;
using ReadRange =
    std::function<std::vector<std::uint8_t>(const std::string& relative, std::uint64_t offset, std::uint64_t size)>;
/// Kaniel 2026-10-19 ("can we prerender it?"): the 3D view's pictures, the place seen from an
/// angle from four sides. View 1 looks north, 2 west, 3 south, 4 east (`yaw` turns as the map
/// screen's does: u = x cos - z sin to the right, v = x sin + z cos ahead), from `pitch` above the
/// ground. Such a picture's frame is in (u, w), w = v sin(pitch) + y cos(pitch) upward: Chart x0
/// is the u of its left edge, z1 the w of its top.
struct ObliqueView {
    float yaw = 0, pitch = 0;
};
ObliqueView ViewOf(int view);
/// A world spot's (u, w) and its nearness to the viewer in an oblique view.
std::array<float, 3> ToView(const ObliqueView& view, float x, float y, float z);

/// Kaniel 2026-10-19 ("an opt in option to view rendered in real time 3d map"): an area's models
/// thinned for drawing live: their corners merged on a grid of `cell` metres (each merged corner
/// at the average of its own, in their average colour: the texture's, blended, in the baked
/// light), the triangles that fold to nothing or repeat left out. Positions in the area's own
/// coordinates (x, y, z), colours 0xAABBGGRR (as a GPU reads RGBA bytes), three indices a triangle.
struct AreaMesh {
    std::vector<float> positions;
    std::vector<std::uint32_t> colors;
    std::vector<std::uint32_t> indices;
    bool operator==(const AreaMesh&) const = default;
};
/// Kept in the user folder ("BBMESH1", zlib-compressed) and read back.
std::vector<std::uint8_t> SaveMesh(const AreaMesh& mesh);
std::optional<AreaMesh> LoadMesh(std::span<const std::uint8_t> file);
/// Corners merged into `cell`-metre grid cells (MeshOf's thinning), for tests.
class MeshBuilder {
public:
    explicit MeshBuilder(float cell) : cell{cell} {}
    void Add(const float* triangle, const std::uint32_t corner_colors[3]);
    void Merge(const MeshBuilder& other);
    AreaMesh Finish() const;
    std::size_t Triangles() const { return tris.size() / 3; }

private:
    std::uint32_t Corner(const float* at, std::uint32_t color);
    float cell;
    std::unordered_map<std::uint64_t, std::uint32_t> corner_of;
    std::vector<double> sums;    // x, y, z, r, g, b each
    std::vector<std::uint32_t> counts;
    std::vector<std::uint32_t> tris;
    std::unordered_set<std::uint64_t> seen; // the triangles already in (by their corners)
};
std::optional<AreaMesh> MeshOf(std::uint32_t packed_map, const ReadFile& read, int threads = 1,
                               const ReadRange& read_range = {}, float cell = 0.4f);

/// `textured`: the pictures in the game's own textures (else one colour a material); `view`
/// 1 to 4: the oblique picture (ViewOf) instead of the one from above.
std::optional<Chart> ChartArea(std::uint32_t packed_map, const ReadFile& read, float ppm = 3.0f,
                               int max_side = 3072, int threads = 1, const ReadRange& read_range = {},
                               bool textured = false, int view = 0);
/// Several areas in one picture, each placed by its own MSB's coordinates (the blocks of one
/// area, m24_00 to m24_02, share them).
std::optional<Chart> ChartAreas(std::span<const std::uint32_t> packed_maps, const ReadFile& read, float ppm = 3.0f,
                                int max_side = 3072, int threads = 1, const ReadRange& read_range = {},
                                bool textured = false, int view = 0, std::vector<MeshBuilder>* meshes = nullptr);

} // namespace BbMapArt
