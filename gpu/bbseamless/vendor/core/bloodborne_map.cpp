// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>

#include "core/bloodborne_lamp_points.h"
#include "core/bloodborne_map.h"
#include "core/bloodborne_party.h"

namespace Core::Bloodborne {

namespace {

constexpr s32 LampWarpBase = 2950; // a lamp object's entity id + 1000 (LampOfActiveFlag's)
constexpr s32 LampsPerMap = 8;     // 2958 and 2959: a boss arena's points
constexpr s32 ActiveFlagBase = 10007810;

// The map (e.g. 241) and the lamp's index on it of a lamp's warp point; nothing for any other.
std::optional<std::pair<s32, s32>> MapAndIndex(s32 warp_point) {
    const s32 within = warp_point % 10000 - LampWarpBase;
    if (warp_point <= 0 || within < 0 || within >= LampsPerMap) {
        return std::nullopt;
    }
    return std::pair{warp_point / 10000, within};
}

} // namespace

const std::vector<MapLamp>& MapLamps() {
    static const std::vector<MapLamp> lamps = [] {
        std::vector<MapLamp> list;
        for (const LampPoint& point : LampPoints) {
            const bool dream = point.packed_map == PackedMapOfWarpPoint(HuntersDreamWarpPoint);
            if (dream ? point.warp_point != HuntersDreamWarpPoint
                      : !MapAndIndex(point.warp_point)) {
                continue;
            }
            list.push_back(MapLamp{point.warp_point, point.packed_map, point.x, point.y, point.z});
        }
        std::ranges::sort(list, {}, &MapLamp::warp_point);
        return list;
    }();
    return lamps;
}

const MapLamp* FindMapLamp(s32 warp_point) {
    const auto& lamps = MapLamps();
    const auto found = std::ranges::lower_bound(lamps, warp_point, {}, &MapLamp::warp_point);
    return found != lamps.end() && found->warp_point == warp_point ? &*found : nullptr;
}

std::optional<s32> LampActiveFlag(s32 warp_point) {
    const auto lamp = MapAndIndex(warp_point);
    if (!lamp || warp_point == HuntersDreamWarpPoint) {
        return std::nullopt;
    }
    return ActiveFlagBase + lamp->first * 10000 + lamp->second * 20;
}

s32 LampNameId(s32 warp_point) {
    if (warp_point == HuntersDreamWarpPoint) {
        return 70002100;
    }
    return 70000000 + warp_point / 10000 * 10 + (warp_point % 10000 - LampWarpBase);
}

s32 AreaNameId(u32 packed_map) {
    return static_cast<s32>((packed_map >> 24) * 100 + ((packed_map >> 16) & 0xFF) * 10);
}

MapTravelBlock MapTravelCheck(const MapTravelState& state) {
    if (!state.in_world) {
        return MapTravelBlock::NotInWorld;
    }
    if (state.travelling) {
        return MapTravelBlock::Travelling;
    }
    if (state.down) {
        return MapTravelBlock::Down;
    }
    if (state.boss_fight) {
        return MapTravelBlock::BossFight;
    }
    if (!state.lit) {
        return MapTravelBlock::NotLit;
    }
    return MapTravelBlock::None;
}

std::span<const s32> AreaBossFlags(u32 packed_map) {
    struct Area {
        u32 map;
        std::array<s32, 2> flags;
        std::size_t count;
    };
    static constexpr std::array<Area, 15> Areas{{
        {0x16000000, {12201800}, 1},
        {0x17000000, {12301800}, 1},
        {0x18000000, {12401800}, 1},
        {0x18010000, {12411800}, 1},
        {0x18020000, {12421700, 12421800}, 2},
        {0x19000000, {12501800}, 1},
        {0x1a000000, {12601800}, 1},
        {0x1b000000, {12701800}, 1},
        {0x1c000000, {12801800}, 1},
        {0x20000000, {13201800}, 1},
        {0x21000000, {13301800}, 1},
        {0x22000000, {13401800}, 1},
        {0x23000000, {13501800, 13501850}, 2},
        {0x24000000, {13601800}, 1},
        {0, {}, 0},
    }};
    for (const Area& area : Areas) {
        if (area.map == (packed_map & 0xFFFF0000u) && area.count != 0) {
            return std::span<const s32>{area.flags.data(), area.count};
        }
    }
    return {};
}

ExploredCell ExploredCellAt(u32 packed_map, float x, float z) {
    const auto cell = [](float v) {
        return static_cast<s16>(
            std::clamp(std::floor(v / ExploredCellMetres), -32768.0f, 32767.0f));
    };
    return {packed_map & 0xFFFF0000u, cell(x), cell(z)};
}

std::vector<u8> SaveExplored(const std::vector<ExploredCell>& cells) {
    std::vector<u8> file{'B', 'B', 'F', 'O', 'G', '1', 0, 0};
    const auto put = [&](const void* v, std::size_t n) {
        const auto* b = static_cast<const u8*>(v);
        file.insert(file.end(), b, b + n);
    };
    const auto count = static_cast<u32>(cells.size());
    put(&count, 4);
    for (const ExploredCell& c : cells) {
        put(&c.packed_map, 4);
        put(&c.x, 2);
        put(&c.z, 2);
    }
    return file;
}

std::optional<std::vector<ExploredCell>> LoadExplored(std::span<const u8> file) {
    static constexpr u8 Magic[8] = {'B', 'B', 'F', 'O', 'G', '1', 0, 0};
    if (file.size() < 12 || !std::equal(Magic, Magic + 8, file.begin())) {
        return std::nullopt;
    }
    u32 count = 0;
    std::memcpy(&count, file.data() + 8, 4);
    if (count > 1'000'000 || file.size() < 12 + std::size_t(count) * 8) {
        return std::nullopt;
    }
    std::vector<ExploredCell> cells(count);
    for (u32 i = 0; i < count; ++i) {
        const u8* at = file.data() + 12 + std::size_t(i) * 8;
        std::memcpy(&cells[i].packed_map, at, 4);
        std::memcpy(&cells[i].x, at + 4, 2);
        std::memcpy(&cells[i].z, at + 6, 2);
    }
    return cells;
}

bool MapTripArrived(const MapTripProgress& trip, u32 map_now, bool loading_now) {
    constexpr double NoLoadSeconds = 8.0;
    if (loading_now || map_now != trip.target_map) {
        return false;
    }
    return trip.load_seen || trip.seconds >= NoLoadSeconds;
}

} // namespace Core::Bloodborne
