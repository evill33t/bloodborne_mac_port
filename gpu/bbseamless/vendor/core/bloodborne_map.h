// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// Kaniel 2026-10-17: the port's map screen (an Elden Ring style map): where this player and the
// party stand, the lamps lit, and a trip to one of them from the map. The lamps are the respawn
// points of bloodborne_lamp_points.h whose warp point is a lamp's (map * 10000 + 2950 + k), the
// Hunter's Dream once; their names and the areas' are the game's own text.

#pragma once

#include <optional>
#include <span>
#include <vector>

#include "common/types.h"

namespace Core::Bloodborne {

struct MapLamp {
    s32 warp_point = 0;
    u32 packed_map = 0;
    float x = 0, y = 0, z = 0;
    bool operator==(const MapLamp&) const = default;
};

// The lamps a map screen lists, by warp point: each map's lamps (k 0 to 7; 8 and 9 are a boss
// arena's points, not lamps) and the Hunter's Dream once (HuntersDreamWarpPoint).
const std::vector<MapLamp>& MapLamps();
const MapLamp* FindMapLamp(s32 warp_point);

// The event flag that is on while the lamp is lit (LampOfActiveFlag's other way round); nothing
// for the Hunter's Dream, which is always there.
std::optional<s32> LampActiveFlag(s32 warp_point);

// The lamp's name in the game's menu text (menu.msgbnd FMG 30, 70002411 "Central Yharnam"), and an
// area's (item.msgbnd FMG 19, 2410 "Central Yharnam" for 0x18010000).
constexpr s32 LampNameFmg = 30;
constexpr s32 AreaNameFmg = 19;
s32 LampNameId(s32 warp_point);
s32 AreaNameId(u32 packed_map);

// Why a trip from the map would not start now (None: it would).
enum class MapTravelBlock {
    None,
    NotLit,     // the lamp is not lit for this character
    NotInWorld, // a load, a cutscene, the title
    Down,       // this player is down or dead
    BossFight,  // a boss fight is on (flag 4000)
    Travelling, // a trip is already under way
};
struct MapTravelState {
    bool lit = false;
    bool in_world = false;
    bool down = false;
    bool boss_fight = false;
    bool travelling = false;
};
MapTravelBlock MapTravelCheck(const MapTravelState& state);

// A trip from the map ends once its map is in and a load was seen on the way (a lamp on the same
// map loads it again), or a few seconds after it started when no load was seen at all.
struct MapTripProgress {
    u32 target_map = 0;
    bool load_seen = false;
    double seconds = 0.0; // since the trip started
};
bool MapTripArrived(const MapTripProgress& trip, u32 map_now, bool loading_now);

// Kaniel 2026-10-18 (the map's fog of war): an area's main bosses' defeat flags (Hunter's Atlas's
// list, MikeTat1988/HuntersAtlas); all of them on, the whole area shows on the map. None for the
// Dream and the chalice dungeons.
std::span<const s32> AreaBossFlags(u32 packed_map);

// The fog of war's cells: where the player has walked, in ExploredCellMetres squares of a map.
constexpr float ExploredCellMetres = 8.0f;
struct ExploredCell {
    u32 packed_map = 0;
    s16 x = 0, z = 0;
    auto operator<=>(const ExploredCell&) const = default;
};
ExploredCell ExploredCellAt(u32 packed_map, float x, float z);
// The explored cells as kept in the user folder ("BBFOG1", a count, then map / x / z each), and
// read back (nothing when it is not one).
std::vector<u8> SaveExplored(const std::vector<ExploredCell>& cells);
std::optional<std::vector<ExploredCell>> LoadExplored(std::span<const u8> file);

} // namespace Core::Bloodborne
