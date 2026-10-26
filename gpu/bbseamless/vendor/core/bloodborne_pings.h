// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// Kaniel 2026-10-24: team pings. A player marks a spot (where the camera aims, the enemy it is
// locked on, or a spot on the world map) and every member of its team sees it for a while: on the
// screen where it lies, on the compass, and on the world map. Positions are the game's world
// metres on a packed map (x east-west, y up, z north-south). This file is the pure part: the
// pings kept, where one goes, and where it shows on the screen and the map.

#pragma once

#include <array>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "common/types.h"

namespace Core::Bloodborne {

using PingPoint = std::array<float, 3>;

// Kaniel 2026-10-24: a ping lasts 20 s, a player has one at a time (a new one replaces it) and
// may place one a second at most.
constexpr s64 PingLifetimeMs = 20'000;
constexpr s64 PingCooldownMs = 1'000;

struct TeamPingMark {
    std::string member; // the sender's board name; "" for this player's own
    u32 map = 0;        // packed map
    PingPoint at{};
    u32 enemy = 0; // the enemy it follows (its handle), 0 none
    u32 seq = 0;   // counts up with each ping of the sender's run
    bool from_map = false;
    s64 placed_ms = 0; // this game's clock, when it came (or was placed)
    bool operator==(const TeamPingMark&) const = default;
};

// The pings shown: one per member, the newest replacing the one before; a ping heard twice (the
// same sequence number again) is not taken twice.
class PingBoard {
public:
    // Whether it was taken (false: the same ping again).
    bool Take(TeamPingMark ping);
    // The pings still alive at `now_ms`, oldest first; the expired ones go.
    std::vector<TeamPingMark> Live(s64 now_ms);
    // An enemy ping's spot moves with its enemy.
    void Follow(const std::string& member, const PingPoint& at);
    void Clear() {
        pings_.clear();
    }

private:
    std::vector<TeamPingMark> pings_;
};

// One ping a second: whether one may be placed at `now_ms` (and it is counted then).
class PingCooldown {
public:
    bool Allow(s64 now_ms);

private:
    std::optional<s64> last_ms_;
};

// The camera the frame is drawn with (the camera manager's from +0x10, see GameCameraAt in the
// map code): its world matrix rows (the x axis to the picture's right, the y axis up, the z axis
// it looks along, its position; w 0, 0, 0, 1), the vertical field of view (radians), width /
// height, near and far planes.
struct PingCamera {
    std::array<std::array<float, 4>, 4> rows{};
    float fov = 0, aspect = 0, near_plane = 0, far_plane = 0;
    bool operator==(const PingCamera&) const = default;
};
// A camera as the game's are: finite, unit axes at right angles, a sane view.
bool LooksLikePingCamera(const PingCamera& camera);
PingPoint CameraPosition(const PingCamera& camera);
PingPoint CameraLooks(const PingCamera& camera);

// Where a point lies in the picture: x from -1 (left) to 1 (right), y from -1 (bottom) to 1
// (top); nothing behind the camera (or closer than its near plane).
std::optional<std::array<float, 2>> ProjectPing(const PingCamera& camera, const PingPoint& point);

// Where a ping's marker goes on the screen, in the picture's fractions (0, 0 its top left, 1, 1
// its bottom right). A ping out of the picture (or behind the camera) is pinned to the edge,
// `margin` (a fraction of the picture) inside it, in its direction; `arrow` is then the way it
// lies, in radians on the screen (0 right, pi / 2 down).
struct PingOnScreen {
    float x = 0, y = 0;
    bool pinned = false;
    float arrow = 0;
};
std::optional<PingOnScreen> PingScreenSpot(const PingCamera& camera, const PingPoint& point,
                                           float margin);

// A character a ping may go on: its handle and where it stands (its feet).
struct PingCharacter {
    u32 handle = 0;
    PingPoint at{};
};

// Kaniel 2026-10-24: where an aimed ping goes. The camera's line, from the camera along where it
// looks: the first character standing close to it (within PingCharacterReach of the line, at its
// middle height) within PingReach; else where the line meets the ground the player stands on
// (its feet's height) within PingReach; else PingAimFallback metres along it. A spot further
// than PingReach from the player is brought back to that distance.
constexpr float PingReach = 60.0f;
constexpr float PingCharacterReach = 1.2f;
constexpr float PingAimFallback = 25.0f;
struct PingAim {
    PingPoint at{};
    u32 enemy = 0; // the character's handle when it went on one
};
PingAim AimPing(const PingCamera& camera, const PingPoint& feet,
                std::span<const PingCharacter> characters);

// Kaniel 2026-10-24: a ping while locked on goes on the target. The HUD's lock cursor (FeData
// +0x14FC, +0x1500: the target's spot on a 1920 x 1080 screen) names it: the character whose
// middle shows nearest to it, within PingLockReach of the picture's height.
constexpr float PingLockReach = 0.12f;
std::optional<PingCharacter> LockedCharacter(const PingCamera& camera, float cursor_x,
                                             float cursor_y,
                                             std::span<const PingCharacter> characters);

// Kaniel 2026-10-24: a map ping. The map's areas as they lie on its atlas (an area's metres plus
// its world's place there): the spot under the map's cursor is on the area drawn there (the one
// shown when two overlap), in that area's own metres.
struct PingArea {
    u32 map = 0;
    float x0 = 0, x1 = 0, z0 = 0, z1 = 0; // its picture, in atlas metres
    float dx = 0, dz = 0;                 // its world's place on the atlas
};
struct PingAreaSpot {
    u32 map = 0;
    float x = 0, z = 0;
    bool operator==(const PingAreaSpot&) const = default;
};
std::optional<PingAreaSpot> PingAreaAt(std::span<const PingArea> areas, u32 shown, float atlas_x,
                                       float atlas_z);

// What the pings show now (ReadPings, as of the last party tick): each live ping with its
// sender's character name, and on this player's map its distance, its place on the compass
// (bearing from the camera's heading) and on the screen.
struct PingShown {
    std::string member; // the sender's board name, "" this player's own
    std::string name;   // the sender's character name, "" when not known
    u32 map = 0;
    PingPoint at{};
    u32 enemy = 0;
    bool from_map = false;
    bool here = false; // on this player's map
    float distance = 0.0f;
    float bearing = 0.0f; // 0 ahead, positive to the right (the compass's)
    std::optional<PingOnScreen> screen;
    float seconds_left = 0.0f;
};
struct PingsView {
    std::optional<float> heading; // the camera's (the compass's), for the strip
    std::vector<PingShown> pings;
};

// Kaniel 2026-10-24 (after the first pictures): where a map ping's words go, in screen pixels.
// Under its mark (radius `r`, `gap` away), else over it, else to its right, else to its left: the
// first of these that crosses none of the place names drawn there (`taken`); when each crosses
// one, the one crossing the least.
struct PingBox {
    float x0 = 0, y0 = 0, x1 = 0, y1 = 0;
};
PingBox PlacePingLabel(float x, float y, float r, float width, float height,
                       std::span<const PingBox> taken, float gap);

// How a ping reads on the screen beside its sender's name: its distance on this map, else the
// area it is in (the map's words for it, e.g. "Cathedral Ward").
std::string PingLabel(const std::string& name, std::optional<float> distance,
                      const std::string& area);

} // namespace Core::Bloodborne
