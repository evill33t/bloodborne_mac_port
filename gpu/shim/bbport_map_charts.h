// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "bbport_map_art.h"
#include <map>

// Player-owned terrain, generated off the render thread and cached beside GPU caches.
namespace BbMapCharts {
std::shared_ptr<const BbMapArt::Chart> Get(std::uint32_t map, int view = 0);
std::shared_ptr<const BbMapArt::AreaMesh> GetMesh(std::uint32_t map);
std::map<std::uint32_t,std::shared_ptr<const BbMapArt::Chart>> Atlas(int view = 0);
void Prepare(std::uint32_t map,int view = 0);
std::map<std::uint32_t,std::shared_ptr<const BbMapArt::AreaMesh>> MeshAtlas();
void PrepareMesh(std::uint32_t map);
std::string Status(std::uint32_t map, bool mesh = false);
}
