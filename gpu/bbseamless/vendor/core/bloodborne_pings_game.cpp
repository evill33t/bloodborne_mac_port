// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// Kaniel 2026-10-24: team pings in the game (bloodborne_pings.h is the pure part). The game
// thread places this player's pings (asked for by the pad, the keyboard or the map screen), takes
// the team's as the server sends them on, follows the enemies pinged, and keeps what the overlay
// and the rig read (ReadPings).

#include "core/bloodborne_re_internal.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include <fmt/format.h>
#include "common/logging/log.h"
#include "coop/coop_messages.h"
#include "core/bloodborne_compass.h"
#include "core/bloodborne_pings.h"

namespace Core::Bloodborne {
namespace {

std::atomic<bool> pings_on{true};

// Asked for from other threads (the pad, the overlay, the rig), placed by the game thread.
std::atomic<u32> aim_asks{0};
std::mutex asks_mutex;
std::optional<std::pair<u32, PingPoint>> map_ask;

// The game thread's own.
PingBoard board;
PingCooldown cooldown;
u32 own_seq = 0;
std::vector<std::pair<std::string, u32>> alive_before; // (member, seq) as the last tick had them

std::mutex view_mutex;
PingsView view_now;

// Where a ping's marker shows over its spot: an enemy's over its head.
constexpr float EnemyMarkerHeight = 1.8f;
// The screen's edge a marker keeps off, a share of the picture.
constexpr float ScreenMargin = 0.04f;
// The characters an aimed ping may go on, at most this far from the player.
constexpr float CharacterSearchMetres = 70.0f;

std::optional<PingCamera> ReadPingCamera() {
    static_assert(sizeof(PingCamera) == 0x50);
    const u64 holder = ReadValue<u64>(image_base + CameraManagerPointerOffset, 0);
    const u64 manager = holder >= 0x10000 ? ReadValue<u64>(holder, CameraManagerField) : 0;
    if (manager < 0x10000 ||
        !HasMemoryAccess(manager + PingCameraAt, sizeof(PingCamera), MemoryProt::CpuRead)) {
        return std::nullopt;
    }
    PingCamera camera;
    std::memcpy(&camera, reinterpret_cast<const void*>(manager + PingCameraAt), sizeof(camera));
    return LooksLikePingCamera(camera) ? std::optional{camera} : std::nullopt;
}

// The living enemies near the player (not the players), with their handles.
std::vector<PingCharacter> EnemiesNear(const PingPoint& me) {
    std::vector<PingCharacter> enemies;
    const u64 world = ReadValue<u64>(image_base + CandidateLocalStatePointerOffset, 0);
    if (world < 0x10000) {
        return enemies;
    }
    std::vector<u64> players{GetLocalPlayer()};
    for (const OtherPlayer& other : OtherPlayers()) {
        players.push_back(other.character);
    }
    ForEachWorldCharacter(world, [&](u64 character) {
        if (std::ranges::find(players, character) != players.end() ||
            IsCharacterDisabled(character)) {
            return;
        }
        const u64 modules = ReadValue<u64>(character, 0x3B0);
        const u64 data = modules >= 0x10000 ? ReadValue<u64>(modules, 0x20) : 0;
        const u64 physics = modules >= 0x10000 ? ReadValue<u64>(modules, 0x68) : 0;
        if (data < 0x10000 || physics < 0x10000 || ReadValue<s32>(data, 0xF8) <= 0) {
            return;
        }
        const PingPoint at{ReadValue<float>(physics, 0x1E0), ReadValue<float>(physics, 0x1E4),
                           ReadValue<float>(physics, 0x1E8)};
        const float dx = at[0] - me[0], dy = at[1] - me[1], dz = at[2] - me[2];
        if (dx * dx + dy * dy + dz * dz <= CharacterSearchMetres * CharacterSearchMetres) {
            enemies.push_back({ReadValue<u32>(character, 0x08), at});
        }
    });
    return enemies;
}

// The enemy with this handle, where it stands, while it lives.
std::optional<PingPoint> EnemyAt(u32 handle) {
    const u64 world = ReadValue<u64>(image_base + CandidateLocalStatePointerOffset, 0);
    std::optional<PingPoint> found;
    if (world < 0x10000) {
        return found;
    }
    ForEachWorldCharacter(world, [&](u64 character) {
        if (found || ReadValue<u32>(character, 0x08) != handle) {
            return;
        }
        const u64 modules = ReadValue<u64>(character, 0x3B0);
        const u64 data = modules >= 0x10000 ? ReadValue<u64>(modules, 0x20) : 0;
        const u64 physics = modules >= 0x10000 ? ReadValue<u64>(modules, 0x68) : 0;
        if (data >= 0x10000 && physics >= 0x10000 && ReadValue<s32>(data, 0xF8) > 0) {
            found = PingPoint{ReadValue<float>(physics, 0x1E0), ReadValue<float>(physics, 0x1E4),
                              ReadValue<float>(physics, 0x1E8)};
        }
    });
    return found;
}

// The menus' quiet tick (SystemSoundCursor), on the game thread.
void PlayPingSound() {
    static const bool known =
        std::memcmp(reinterpret_cast<const void*>(image_base + SystemSoundOffset),
                    SystemSoundPrologue.data(), SystemSoundPrologue.size()) == 0;
    const u64 sounds = ReadValue<u64>(image_base + SystemSoundPointerOffset, 0);
    if (!known || sounds < 0x10000) {
        return;
    }
    using PlaySystemSound = void PS4_SYSV_ABI (*)(u64 sounds, s32 sound);
    reinterpret_cast<PlaySystemSound>(image_base + SystemSoundOffset)(sounds, SystemSoundCursor);
}

std::string Where(const PingPoint& at) {
    return fmt::format("({:.2f}, {:.2f}, {:.2f})", at[0], at[1], at[2]);
}

// This player's ping: kept here and sent to the team.
void PlacePing(u32 map, const PingPoint& at, u32 enemy, bool from_map, std::string_view how,
               s64 now_ms) {
    if (!cooldown.Allow(now_ms)) {
        LOG_INFO(Debug, "Co-op ping: too soon after the last one ({}), not placed", how);
        return;
    }
    TeamPingMark ping{.member = "",
                      .map = map,
                      .at = at,
                      .enemy = enemy,
                      .seq = ++own_seq,
                      .from_map = from_map,
                      .placed_ms = now_ms};
    board.Take(ping);
    SendTeamPing(BloodborneCoop::TeamPing{
        .map = map, .at = at, .enemy = enemy, .seq = ping.seq, .from_map = from_map});
    LOG_INFO(Debug, "Co-op ping: placed seq {} on map {:#x} at {} enemy {:#x} ({})", ping.seq, map,
             Where(at), enemy, how);
}

void PlaceAimed(s64 now_ms) {
    const auto me = LocalPlayerPosition();
    const u32 map = PlayingMap();
    const auto camera = ReadPingCamera();
    if (!me || map == 0 || !camera || !GameHudUp()) {
        LOG_INFO(Debug, "Co-op ping: not in the world (camera {}, map {:#x}), not placed",
                 camera.has_value(), map);
        return;
    }
    const std::vector<PingCharacter> enemies = EnemiesNear(*me);
    // Locked on: the target, followed.
    if (ReadValue<s32>(image_base + HudFeDataOffset, HudLockCursorAt) == 0) {
        const float x = float(ReadValue<s32>(image_base + HudFeDataOffset, HudLockCursorXAt));
        const float y = float(ReadValue<s32>(image_base + HudFeDataOffset, HudLockCursorYAt));
        if (const auto locked = LockedCharacter(*camera, x, y, enemies)) {
            PlacePing(map, locked->at, locked->handle, false, "locked on", now_ms);
            return;
        }
        LOG_INFO(Debug, "Co-op ping: locked on, but no enemy under the lock cursor ({}, {})", x, y);
    }
    const PingAim aim = AimPing(*camera, *me, enemies);
    PlacePing(map, aim.at, aim.enemy, false, aim.enemy != 0 ? "aimed at an enemy" : "aimed",
              now_ms);
}

std::string NameOf(const std::string& member) {
    return member.empty() ? OwnCharacterName() : CharacterNameOf(member);
}

} // namespace

void RequestAimPing() {
    aim_asks.fetch_add(1);
}

void RequestMapPing(u32 map, const PingPoint& at) {
    std::scoped_lock lock{asks_mutex};
    map_ask = std::pair{map, at};
}

void SetTeamPingsOn(bool on) {
    pings_on.store(on, std::memory_order_relaxed);
}

bool ReadPings(PingsView& view) {
    std::scoped_lock lock{view_mutex};
    view = view_now;
    return !view.pings.empty();
}

namespace Coop {

void TickPings() {
    const s64 now_ms = SteadyMilliseconds();
    const bool on = pings_on.load(std::memory_order_relaxed);
    // The team's, as they came.
    for (const BloodborneCoop::TeamPing& heard : TakeTeamPings()) {
        if (!on || heard.member.empty()) {
            continue;
        }
        const bool taken = board.Take(TeamPingMark{.member = heard.member,
                                                   .map = heard.map,
                                                   .at = heard.at,
                                                   .enemy = heard.enemy,
                                                   .seq = heard.seq,
                                                   .from_map = heard.from_map,
                                                   .placed_ms = now_ms});
        LOG_INFO(Debug, "Co-op ping: from {} ({}) seq {} on map {:#x} at {} enemy {:#x}{}{}",
                 heard.member, CharacterNameOf(heard.member), heard.seq, heard.map, Where(heard.at),
                 heard.enemy, heard.from_map ? " (from the map)" : "",
                 taken ? "" : ", heard before");
        if (taken) {
            PlayPingSound();
        }
    }
    // This player's own.
    const u32 aims = aim_asks.exchange(0);
    std::optional<std::pair<u32, PingPoint>> on_map;
    {
        std::scoped_lock lock{asks_mutex};
        on_map = std::exchange(map_ask, std::nullopt);
    }
    if (on && on_map) {
        PlacePing(on_map->first, on_map->second, 0, true, "on the map", now_ms);
    } else if (on && aims > 0) {
        PlaceAimed(now_ms);
    }
    if (!on) {
        board.Clear();
    }

    std::vector<TeamPingMark> live = board.Live(now_ms);
    const u32 map = PlayingMap();
    // Enemy pings on this map follow their enemies.
    for (TeamPingMark& ping : live) {
        if (ping.enemy != 0 && ping.map == map) {
            if (const auto at = EnemyAt(ping.enemy)) {
                ping.at = *at;
                board.Follow(ping.member, *at);
            }
        }
    }
    // Gone ones, for the log.
    std::vector<std::pair<std::string, u32>> alive;
    for (const TeamPingMark& ping : live) {
        alive.emplace_back(ping.member, ping.seq);
    }
    for (const auto& [member, seq] : alive_before) {
        if (std::ranges::find(alive, std::pair{member, seq}) == alive.end()) {
            LOG_INFO(Debug, "Co-op ping: {}'s ping seq {} is gone",
                     member.empty() ? "this player" : member, seq);
        }
    }
    alive_before = std::move(alive);

    PingsView view;
    const auto me = LocalPlayerPosition();
    const auto camera = live.empty() ? std::nullopt : ReadPingCamera();
    view.heading = CameraHeadingNow();
    const bool hud = GameHudUp();
    for (const TeamPingMark& ping : live) {
        PingShown shown{.member = ping.member,
                        .name = NameOf(ping.member),
                        .map = ping.map,
                        .at = ping.at,
                        .enemy = ping.enemy,
                        .from_map = ping.from_map,
                        .seconds_left =
                            float(PingLifetimeMs - (now_ms - ping.placed_ms)) / 1000.0f};
        shown.here = me && map != 0 && ping.map == map;
        if (shown.here) {
            if (view.heading) {
                if (const auto seen = BearingOf(*me, *view.heading, ping.at)) {
                    shown.bearing = seen->bearing;
                    shown.distance = seen->distance;
                }
            }
            if (shown.distance == 0.0f) {
                const float dx = ping.at[0] - (*me)[0], dy = ping.at[1] - (*me)[1],
                            dz = ping.at[2] - (*me)[2];
                shown.distance = std::sqrt(dx * dx + dy * dy + dz * dz);
            }
            if (camera && hud) {
                const PingPoint marker{ping.at[0],
                                       ping.at[1] + (ping.enemy != 0 ? EnemyMarkerHeight : 0.0f),
                                       ping.at[2]};
                shown.screen = PingScreenSpot(*camera, marker, ScreenMargin);
            }
        }
        view.pings.push_back(std::move(shown));
    }
    std::scoped_lock lock{view_mutex};
    view_now = std::move(view);
}

} // namespace Coop
} // namespace Core::Bloodborne
