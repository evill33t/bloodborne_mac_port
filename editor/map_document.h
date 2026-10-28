// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "bbport_map_art.h"
#include "game_params.h"
#include "event_document.h"
#include <filesystem>
#include <atomic>
#include <optional>
#include <map>
#include <memory>
#include <set>
#include <span>
#include <string>
#include <vector>
#include <variant>

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
    bool operator==(const Reference&) const = default;
};
struct EnemyData {
    bool dummy=false;
    std::int32_t entity_id=0, instance_id=0, npc_param=0, think_param=0, talk_id=0, chara_init=0;
    std::int32_t init_anim=0, damage_anim=0;
    std::int16_t patrol_type=0, fallback_platoon=0;
    Reference collision, patrol;
    std::array<Reference,8> move_points;
    bool operator==(const EnemyData&) const = default;
};
struct Part {
    std::string name, model;
    std::size_t record = 0;
    std::int32_t entity_id=-1, instance_id=0, type_id=0;
    std::uint32_t type=1;
    bool added=false;
    Transform original, transform;
    PartKind kind = PartKind::Object;
    std::optional<EnemyData> enemy;
    std::optional<EnemyData> original_enemy;
};
struct Region {
    std::string name;
    std::size_t record=0;
    int key=-1;
    std::int32_t entity_id=-1;
    std::uint32_t shape=0;
    bool added=false;
    Transform original,transform;
};
struct RouteData {
    std::int32_t type=0;
    std::array<Reference,32> points;
    bool operator==(const RouteData&) const = default;
};
struct Route {
    std::string name;
    std::size_t record=0;
    int key=-1;
    std::int32_t entity_id=-1,event_id=0;
    bool added=false;
    RouteData original,data;
};
struct TreasureData {
    Reference part;
    std::array<std::int32_t,3> lots{-1,-1,-1};
    std::int32_t action_button=-1;
    bool in_chest=false,disabled=false;
    bool operator==(const TreasureData&) const = default;
};
struct Treasure {
    std::string name;
    std::size_t record=0;
    std::int32_t event_id=0,entity_id=-1;
    bool added=false;
    TreasureData original,data;
};
// Copies retain uninterpreted record data, with fresh identifiers. Structural exports
// keep part type groups contiguous and remap references to preserve their targets.
class Project;
class Document {
public:
    static Document Read(std::span<const std::uint8_t> file);
    static Document Open(const std::filesystem::path& path);
    const std::vector<Part>& Parts() const { return parts; }
    const std::vector<Region>& Regions() const { return regions; }
    const std::vector<Route>& Routes() const { return routes; }
    const std::vector<Treasure>& Treasures() const {return treasures;}
    const std::vector<ItemLot>& ItemLots() const {return item_lots;}
    void AttachParameters(std::shared_ptr<const GameParams> value){parameters=std::move(value);}
    void AttachEvents(const std::string& name,std::shared_ptr<const EventFile> file);
    const std::map<std::string,std::shared_ptr<const EventFile>>& Scripts() const {return scripts;}
    const std::vector<ScriptOperand>& EventEdits() const {return event_edits;}
    const std::vector<PickupQuest>& Quests() const {return quests;}
    ScriptOperand EventArgumentValue(const std::string& script,std::size_t event,std::size_t instruction,std::size_t argument) const;
    bool SetEventArgument(const std::string& script,std::size_t event,std::size_t instruction,std::size_t argument,double value);
    std::size_t AddPickupQuest(std::size_t treasure,const ItemContents& reward,const std::string& script);
    bool RemoveQuest(std::size_t index);
    std::size_t DuplicateTreasure(std::size_t index);
    bool RemoveTreasure(std::size_t index);
    bool SetTreasure(std::size_t index,const TreasureData& data);
    bool SetItemContents(std::size_t index,const ItemContents& contents);
    bool Changed() const;
    std::uint64_t Version() const {return cursor?history[cursor-1].version:0;}
    bool Set(std::size_t index, const Transform& transform);
    bool SetEnemy(std::size_t index, const EnemyData& data);
    bool SetRegion(std::size_t index, const Transform& transform);
    bool SetRoute(std::size_t index, const RouteData& data);
    std::size_t AddWaypoint(const BbMapArt::Vec3& position);
    std::size_t AddRoute();
    bool RemoveRegion(std::size_t index);
    bool RemoveRoute(std::size_t index);
    std::size_t Duplicate(std::size_t index,bool activate_object=false);
    std::size_t DuplicateAt(std::size_t index,const Transform& transform);
    // Only editor-created copies can be removed; existing reference targets stay valid.
    bool Remove(std::size_t index);
    bool Undo();
    bool Redo();
    std::vector<std::uint8_t> Inflated() const;
    std::vector<std::uint8_t> Save() const;
    // Exclusive creation: exports cannot overwrite a source map or an existing mod.
    std::filesystem::path Export(const std::filesystem::path& folder, const std::string& map) const;
private:
    friend class Project;
    std::vector<std::uint8_t> source, raw;
    std::filesystem::path source_game;
    std::filesystem::path source_map;
    std::vector<Part> parts;
    std::vector<Region> regions;
    std::vector<Route> routes;
    std::vector<Treasure> treasures;
    std::vector<ItemLot> item_lots;
    std::shared_ptr<const GameParams> parameters;
    std::map<std::string,std::shared_ptr<const EventFile>> scripts;
    std::vector<ScriptOperand> event_edits;
    std::vector<PickupQuest> quests;
    std::set<std::int32_t> reserved_script_ids;
    int next_region_key=0,next_route_key=0;
    struct Section { std::string name; std::size_t at, name_at, next; std::vector<std::size_t> entries; };
    std::vector<Section> sections;
    std::map<std::size_t,std::size_t> record_ends;
    std::set<std::string> reserved_names;
    std::set<std::string> reserved_region_names,reserved_route_names;
    std::set<std::string> reserved_treasure_names;
    std::set<std::int32_t> reserved_lot_ids;
    std::map<std::int32_t,std::set<std::int32_t>> reserved_pickup_flags;
    std::set<std::int32_t> reserved_entities;
    std::map<std::string,std::set<std::int32_t>> reserved_instances;
    std::map<std::uint32_t,std::set<std::int32_t>> reserved_types;
    bool reserved_siblings=false;
    void ReserveIdentifiers();
    void ReserveSiblings();
    std::int32_t NewEntityID(std::int32_t source_id=-1);
    std::int32_t NewEventID() const;
    ItemLot NewLot(std::int32_t source,bool independent);
    std::int32_t NewPickupFlag(std::int32_t source);
    void Group(std::size_t first);
    using Entry=std::variant<Part,Region,Route,Treasure,ItemLot,ScriptOperand,PickupQuest>;
    struct Edit {
        std::size_t index;std::optional<Entry> before,after;std::size_t group=0;
        static std::uint64_t NextVersion(){static std::atomic<std::uint64_t> value{0};return ++value;}
        std::uint64_t version=NextVersion();
    };
    void Apply(const Edit& edit, bool forward);
    std::vector<Edit> history;
    std::size_t cursor = 0;
    std::size_t next_group=0;
    bool dcx = false;
};
std::vector<std::uint8_t> ReadFile(const std::filesystem::path& path);
bool ValidMapName(const std::string& name);
BbMapArt::Vec3 WorldPoint(const Transform& transform, const float* point);
} // namespace BbMapEditor
