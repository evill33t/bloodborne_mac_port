// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "map_document.h"
namespace BbMapEditor {
struct SourcePlacement {
    std::string name,model;
    // 0 terrain, 1 prop/asset, 2 character, 3 nonvisual MSB part.
    std::uint32_t kind=3;
    std::int32_t source_id=-1;
    Transform transform;
};
struct SourceScene {
    std::string game,map,notice;
    std::map<std::string,std::shared_ptr<const BbMapArt::AreaMesh>> models;
    std::vector<SourcePlacement> placements;
    static SourceScene Read(std::span<const std::uint8_t> bytes);
    static SourceScene Open(const std::filesystem::path& file);
    // Copies a native template, retaining its model/AI/collision. Foreign identifiers
    // never become native references. One undo removes the complete transferred copy.
    std::size_t Transfer(Document& target,std::size_t native_template,std::size_t source,
                         BbMapArt::Vec3 source_anchor,BbMapArt::Vec3 target_anchor,float yaw) const;
};
}
