// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "bbport_map_art.h"
#include <filesystem>
#include <optional>
#include <map>
#include <set>
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
    std::int32_t entity_id=-1, instance_id=0, type_id=0;
    bool added=false;
    Transform original, transform;
    PartKind kind = PartKind::Object;
    std::optional<EnemyData> enemy;
};
// Copies retain uninterpreted record data, with fresh identifiers. Structural exports
// keep part type groups contiguous and remap references to preserve their targets.
class Document {
public:
    static Document Read(std::span<const std::uint8_t> file);
    static Document Open(const std::filesystem::path& path);
    const std::vector<Part>& Parts() const { return parts; }
    bool Changed() const;
    bool Set(std::size_t index, const Transform& transform);
    std::size_t Duplicate(std::size_t index);
    // Only copies created in this session can be removed; existing reference targets stay valid.
    bool Remove(std::size_t index);
    bool Undo();
    bool Redo();
    std::vector<std::uint8_t> Inflated() const;
    std::vector<std::uint8_t> Save() const;
    // Exclusive creation: exports cannot overwrite a source map or an existing mod.
    std::filesystem::path Export(const std::filesystem::path& folder, const std::string& map) const;
private:
    std::vector<std::uint8_t> source, raw;
    std::filesystem::path source_game;
    std::filesystem::path source_map;
    std::vector<Part> parts;
    struct Section { std::string name; std::size_t at, name_at, next; std::vector<std::size_t> entries; };
    std::vector<Section> sections;
    std::map<std::size_t,std::size_t> record_ends;
    std::set<std::string> reserved_names;
    std::set<std::int32_t> reserved_entities;
    std::map<std::string,std::set<std::int32_t>> reserved_instances;
    std::map<std::uint32_t,std::set<std::int32_t>> reserved_types;
    bool reserved_siblings=false;
    void ReserveIdentifiers();
    void ReserveSiblings();
    struct Edit { std::size_t index; std::optional<Part> before, after; };
    void Apply(const Edit& edit, bool forward);
    std::vector<Edit> history;
    std::size_t cursor = 0;
    bool dcx = false;
};
std::vector<std::uint8_t> ReadFile(const std::filesystem::path& path);
bool ValidMapName(const std::string& name);
BbMapArt::Vec3 WorldPoint(const Transform& transform, const float* point);
} // namespace BbMapEditor
