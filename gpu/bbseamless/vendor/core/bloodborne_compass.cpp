// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <cmath>
#include <numbers>

#include "core/bloodborne_compass.h"

namespace Core::Bloodborne {

namespace {

constexpr float Pi = std::numbers::pi_v<float>;

float DistanceSquared(const CompassPoint& a, const CompassPoint& b) {
    const float dx = a[0] - b[0], dy = a[1] - b[1], dz = a[2] - b[2];
    return dx * dx + dy * dy + dz * dz;
}

} // namespace

std::optional<float> HeadingTo(const CompassPoint& from, const CompassPoint& to) {
    const float dx = to[0] - from[0], dz = to[2] - from[2];
    if (dx * dx + dz * dz < 1e-6f) {
        return std::nullopt;
    }
    // The camera object's direction on the ground is (sin y, cos y).
    return std::atan2(dx, dz);
}

std::optional<float> CameraHeading(const CompassPoint& pivot, const CompassPoint& drawn) {
    return HeadingTo(drawn, pivot);
}

float WrapAngle(float angle) {
    angle = std::remainder(angle, 2 * Pi);
    return angle <= -Pi ? angle + 2 * Pi : angle;
}

std::optional<CompassBearing> BearingOf(const CompassPoint& me, float camera_heading,
                                        const CompassPoint& member) {
    const auto heading = HeadingTo(me, member);
    if (!heading) {
        return std::nullopt;
    }
    return CompassBearing{WrapAngle(*heading - camera_heading),
                          std::sqrt(DistanceSquared(me, member))};
}

CompassPlace PlaceOnStrip(float bearing) {
    bearing = WrapAngle(bearing);
    if (std::abs(bearing) > CompassHalfSpan) {
        return {bearing < 0.0f ? -1.0f : 1.0f, true};
    }
    return {bearing / CompassHalfSpan, false};
}

std::vector<CompassMarker> CompassMarkers(std::span<const std::optional<CompassPoint>> members,
                                          const CompassPoint& me, float camera_heading,
                                          const std::vector<bool>& down) {
    std::vector<CompassMarker> markers;
    for (size_t index = 0; index < members.size(); ++index) {
        const auto seen =
            members[index] ? BearingOf(me, camera_heading, *members[index]) : std::nullopt;
        if (seen) {
            markers.push_back({static_cast<s32>(index), seen->bearing, seen->distance,
                               PlaceOnStrip(seen->bearing), index < down.size() && down[index]});
        }
    }
    std::ranges::stable_partition(markers,
                                  [](const CompassMarker& marker) { return !marker.down; });
    return markers;
}

std::optional<CompassStain> CompassStainOf(const std::optional<CompassPoint>& own,
                                           const CompassPoint& me, float camera_heading) {
    const auto seen = own ? BearingOf(me, camera_heading, *own) : std::nullopt;
    if (!seen) {
        return std::nullopt;
    }
    return CompassStain{seen->bearing, seen->distance, PlaceOnStrip(seen->bearing)};
}

std::vector<std::optional<CompassPoint>> LivePositions(
    std::span<const std::optional<CompassPoint>> members,
    std::span<const std::optional<CompassPoint>> characters, float reach) {
    struct Pair {
        float distance;
        size_t member;
        size_t character;
    };
    std::vector<Pair> pairs;
    for (size_t m = 0; m < members.size(); ++m) {
        for (size_t c = 0; c < characters.size(); ++c) {
            if (!members[m] || !characters[c]) {
                continue;
            }
            if (const float distance = DistanceSquared(*members[m], *characters[c]);
                distance <= reach * reach) {
                pairs.push_back({distance, m, c});
            }
        }
    }
    std::ranges::sort(pairs, {}, &Pair::distance);
    std::vector<std::optional<CompassPoint>> live(members.size());
    std::vector<bool> taken(characters.size(), false);
    for (const Pair& pair : pairs) {
        if (!live[pair.member] && !taken[pair.character]) {
            live[pair.member] = characters[pair.character];
            taken[pair.character] = true;
        }
    }
    return live;
}

} // namespace Core::Bloodborne
