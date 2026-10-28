// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "map_document.h"
namespace BbMapEditor {
// A project retains editable history and the original serialization baselines.
// Geometry/textures continue to come from the user's game folder.
class Project {
public:
    std::filesystem::path game;
    std::string map,selected_name;
    int selected_kind=0,route_key=-1;
    std::array<float,6> camera{0,0,0,4,.45f,.85f};
    bool exported=false;
    std::vector<std::uint8_t> exported_msb;
    std::vector<ItemLot> exported_lots;
    std::vector<ScriptOperand> exported_events;
    std::vector<PickupQuest> exported_quests;
    static Project Capture(const std::filesystem::path& game,const std::string& map,const Document& document);
    Document OriginalDocument() const;
    void Restore(Document& document) const;
    std::span<const std::uint8_t> Source(const std::string& relative) const;
    std::vector<std::string> EventNames() const;
    std::vector<std::uint8_t> Bytes() const;
    static Project Read(std::span<const std::uint8_t> bytes);
    static Project Open(const std::filesystem::path& path);
    // Atomic replacement only for an existing, valid editor project. Other files and
    // any location under the game folder are refused.
    void Save(const std::filesystem::path& path) const;
private:
    std::map<std::string,std::vector<std::uint8_t>> sources;
    std::vector<std::uint8_t> session;
};
}
