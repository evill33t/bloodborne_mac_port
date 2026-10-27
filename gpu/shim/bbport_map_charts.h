// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "bbport_map_art.h"

// Player-owned terrain, generated off the render thread and cached beside GPU caches.
namespace BbMapCharts {
std::shared_ptr<const BbMapArt::Chart> Get(std::uint32_t map);
std::shared_ptr<const BbMapArt::AreaMesh> GetMesh(std::uint32_t map);
std::string Status(std::uint32_t map, bool mesh = false);
}
