// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "bbport_map_art.h"
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace BbMapEditor {
struct Transform {
    BbMapArt::Vec3 position{}, rotation{}, scale{1, 1, 1};
    bool operator==(const Transform&) const = default;
};
enum class PartKind { Object, Enemy };
struct Reference {
    int index = -1;
    std::string name;
    bool Missing() const { return index != -1 && name.empty(); }
};
struct EnemyData {
    bool dummy=false;
    std::int32_t entity_id=0, instance_id=0, npc_param=0, think_param=0, talk_id=0, chara_init=0;
    std::int32_t init_anim=0, damage_anim=0;
    Reference collision, patrol;
    std::array<Reference,8> move_points;
};
struct Part {
    std::string name, model;
    std::size_t record = 0;
    Transform original, transform;
    PartKind kind = PartKind::Object;
    std::optional<EnemyData> enemy;
};
// A deliberately narrow MSBB editor. Uninterpreted sections, entity IDs, groups, scripts,
// collisions and padding survive byte-for-byte; only existing placement transforms are writable.
class Document {
public:
    static Document Read(std::span<const std::uint8_t> file);
    static Document Open(const std::filesystem::path& path);
    const std::vector<Part>& Parts() const { return parts; }
    bool Changed() const;
    bool Set(std::size_t index, const Transform& transform);
    bool Undo();
    bool Redo();
    std::vector<std::uint8_t> Inflated() const;
    std::vector<std::uint8_t> Save() const;
    // Exclusive creation: exports cannot overwrite a source map or an existing mod.
    std::filesystem::path Export(const std::filesystem::path& folder, const std::string& map) const;
private:
    std::vector<std::uint8_t> source, raw;
    std::filesystem::path source_game;
    std::vector<Part> parts;
    struct Edit { std::size_t index; Transform before, after; };
    std::vector<Edit> history;
    std::size_t cursor = 0;
    bool dcx = false;
};
std::vector<std::uint8_t> ReadFile(const std::filesystem::path& path);
bool ValidMapName(const std::string& name);
BbMapArt::Vec3 WorldPoint(const Transform& transform, const float* point);
} // namespace BbMapEditor
