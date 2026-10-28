// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "map_document.h"
#include <stop_token>
namespace BbMapEditor {
// Static collision surfaces only. This does not determine navigation connectivity or
// include animated object collision. Queries use the original, unthinned triangles.
class CollisionScene {
public:
    static CollisionScene Read(const BbMapArt::MapStudio& studio,const std::string& base_map,
                               const BbMapArt::ReadFile& read,const BbMapArt::ReadRange& range,
                               std::stop_token stop={});
    static CollisionScene FromTriangles(std::span<const float> world);
    std::optional<float> Below(BbMapArt::Vec3 position,float distance,float offset=0) const;
    std::size_t Triangles() const{return triangles.size();}
    std::size_t missing_models=0;
    std::shared_ptr<const BbMapArt::AreaMesh> preview;
private:
    struct Triangle {std::array<BbMapArt::Vec3,3> v;};
    struct Node {BbMapArt::Vec3 lo,hi;std::uint32_t begin=0,end=0,left=0,right=0;};
    std::vector<Triangle> triangles;
    std::vector<std::uint32_t> order;
    std::vector<Node> nodes;
    std::uint32_t Build(std::uint32_t begin,std::uint32_t end);
};
}
