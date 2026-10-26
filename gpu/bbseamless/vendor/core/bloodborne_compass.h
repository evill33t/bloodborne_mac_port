// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// Kaniel 2026-10-17: the teammate compass. A strip at the top of the screen turned with the camera;
// each party member on this map is a marker on it at its bearing, with its distance. Positions
// are the game's world metres (x east-west, y up, z north-south; positions on two maps are not
// compared). Angles are radians.

#pragma once

#include <array>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "common/types.h"

namespace Core::Bloodborne {

using CompassPoint = std::array<float, 3>;

// The camera's heading: the way from its drawn position (camera object +0x100) to its pivot
// (+0xD0) on the ground, as the camera object's yaw: 0 looks along +z, growing turns right
// (runtime_camhook.c: the mouse moving right adds to the yaw), toward +x. Nothing while the camera
// looks straight down or up (closer than a millimetre on the ground).
std::optional<float> CameraHeading(const CompassPoint& pivot, const CompassPoint& drawn);

// The heading of the way from `from` to `to` (the camera's yaw that would look at it).
std::optional<float> HeadingTo(const CompassPoint& from, const CompassPoint& to);

// `angle` in (-pi, pi].
float WrapAngle(float angle);

// Where a member stands seen from the camera: its bearing (0 straight ahead, positive to the
// right, in (-pi, pi]) and distance in metres (3D).
struct CompassBearing {
    float bearing = 0.0f;
    float distance = 0.0f;
    bool operator==(const CompassBearing&) const = default;
};
std::optional<CompassBearing> BearingOf(const CompassPoint& me, float camera_heading,
                                        const CompassPoint& member);

// The strip shows CompassHalfSpan either side of straight ahead; a member past it is pinned at the
// strip's edge (with an arrow): `x` from -1 (left end) to 1 (right end).
constexpr float CompassHalfSpan = 1.5707964f; // 90 degrees
struct CompassPlace {
    float x = 0.0f;
    bool pinned = false;
    bool operator==(const CompassPlace&) const = default;
};
CompassPlace PlaceOnStrip(float bearing);

// The markers for the members (by index) whose positions are known here.
struct CompassMarker {
    s32 member = 0;
    float bearing = 0.0f;
    float distance = 0.0f;
    CompassPlace place;
    bool down = false;
    bool operator==(const CompassMarker&) const = default;
};
// Kaniel 2026-10-17: `down` (by index, may be empty) marks the downed members, whose markers come
// last (drawn over the others).
std::vector<CompassMarker> CompassMarkers(std::span<const std::optional<CompassPoint>> members,
                                          const CompassPoint& me, float camera_heading,
                                          const std::vector<bool>& down = {});

// Kaniel 2026-10-20: this player's own bloodstain on the compass (only its own: Kaniel after
// v1.0.2), on this map only (the caller leaves another map's out).
struct CompassStain {
    float bearing = 0.0f;
    float distance = 0.0f;
    CompassPlace place;
    bool operator==(const CompassStain&) const = default;
};
std::optional<CompassStain> CompassStainOf(const std::optional<CompassPoint>& own,
                                           const CompassPoint& me, float camera_heading);

// What the compass shows now (ReadCompass): the camera's heading and the markers, with each
// marker's name (the party panel's), or why it shows nothing.
struct CompassView {
    float heading = 0.0f;
    std::vector<CompassMarker> markers;
    std::vector<std::string> names;
    // Kaniel 2026-10-17, by marker too: its board name (its portrait's key), whether it is down or
    // dead (Vital's numbers: 2 down, 3 dead) and its revive's progress (0 to 1).
    std::vector<std::string> members;
    std::vector<int> vitals;
    std::vector<float> revives;
    // Kaniel 2026-10-20: this player's own bloodstain here.
    std::optional<CompassStain> stain;
    std::string why;
};

// The live characters' positions for the members' board ones: the board's come a few times a
// second, the game's characters move every frame. Each member takes the nearest character within
// `reach` of its board position (each character once, the nearest pairs first); nothing where none
// is that close.
std::vector<std::optional<CompassPoint>> LivePositions(
    std::span<const std::optional<CompassPoint>> members,
    std::span<const std::optional<CompassPoint>> characters, float reach);

} // namespace Core::Bloodborne
