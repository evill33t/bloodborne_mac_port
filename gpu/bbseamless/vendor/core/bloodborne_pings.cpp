// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "core/bloodborne_pings.h"

namespace Core::Bloodborne {

namespace {

using Vec3 = std::array<float, 3>;

Vec3 Axis(const std::array<float, 4>& row) {
    return {row[0], row[1], row[2]};
}
float Dot(const Vec3& a, const Vec3& b) {
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}
Vec3 Minus(const Vec3& a, const Vec3& b) {
    return {a[0] - b[0], a[1] - b[1], a[2] - b[2]};
}
Vec3 Along(const Vec3& from, const Vec3& way, float t) {
    return {from[0] + way[0] * t, from[1] + way[1] * t, from[2] + way[2] * t};
}
float Length(const Vec3& a) {
    return std::sqrt(Dot(a, a));
}

// A character's middle (above its feet): where the camera's line and the lock cursor see it.
constexpr float CharacterMiddle = 0.9f;

} // namespace

bool PingBoard::Take(TeamPingMark ping) {
    const auto old = std::ranges::find(pings_, ping.member, &TeamPingMark::member);
    if (old != pings_.end()) {
        if (old->seq == ping.seq) {
            return false;
        }
        pings_.erase(old);
    }
    pings_.push_back(std::move(ping));
    return true;
}

std::vector<TeamPingMark> PingBoard::Live(s64 now_ms) {
    std::erase_if(pings_, [&](const TeamPingMark& ping) {
        return now_ms - ping.placed_ms >= PingLifetimeMs;
    });
    return pings_;
}

void PingBoard::Follow(const std::string& member, const PingPoint& at) {
    if (const auto ping = std::ranges::find(pings_, member, &TeamPingMark::member);
        ping != pings_.end()) {
        ping->at = at;
    }
}

bool PingCooldown::Allow(s64 now_ms) {
    if (last_ms_ && now_ms - *last_ms_ < PingCooldownMs) {
        return false;
    }
    last_ms_ = now_ms;
    return true;
}

bool LooksLikePingCamera(const PingCamera& camera) {
    for (const auto& row : camera.rows) {
        for (const float value : row) {
            if (!std::isfinite(value) || std::abs(value) > 1.0e6f) {
                return false;
            }
        }
    }
    for (const float value : {camera.fov, camera.aspect, camera.near_plane, camera.far_plane}) {
        if (!std::isfinite(value)) {
            return false;
        }
    }
    constexpr float Slack = 0.01f;
    for (size_t i = 0; i < 3; ++i) {
        const Vec3 axis = Axis(camera.rows[i]);
        if (std::abs(Dot(axis, axis) - 1.0f) > Slack || camera.rows[i][3] != 0.0f) {
            return false;
        }
        for (size_t j = i + 1; j < 3; ++j) {
            if (std::abs(Dot(axis, Axis(camera.rows[j]))) > Slack) {
                return false;
            }
        }
    }
    return camera.rows[3][3] == 1.0f && camera.fov > 0.05f && camera.fov < 3.0f &&
           camera.aspect > 0.1f && camera.aspect < 10.0f && camera.near_plane > 0.0f &&
           camera.far_plane > camera.near_plane;
}

PingPoint CameraPosition(const PingCamera& camera) {
    return Axis(camera.rows[3]);
}

PingPoint CameraLooks(const PingCamera& camera) {
    return Axis(camera.rows[2]);
}

std::optional<std::array<float, 2>> ProjectPing(const PingCamera& camera, const PingPoint& point) {
    const Vec3 d = Minus(point, CameraPosition(camera));
    const float ahead = Dot(d, CameraLooks(camera));
    if (ahead <= camera.near_plane) {
        return std::nullopt;
    }
    const float t = std::tan(camera.fov * 0.5f);
    return std::array<float, 2>{Dot(d, Axis(camera.rows[0])) / (ahead * t * camera.aspect),
                                Dot(d, Axis(camera.rows[1])) / (ahead * t)};
}

std::optional<PingOnScreen> PingScreenSpot(const PingCamera& camera, const PingPoint& point,
                                           float margin) {
    if (!LooksLikePingCamera(camera)) {
        return std::nullopt;
    }
    const float edge = 1.0f - 2.0f * margin; // the margin, in the picture's -1..1
    if (const auto seen = ProjectPing(camera, point);
        seen && std::abs((*seen)[0]) <= edge && std::abs((*seen)[1]) <= edge) {
        return PingOnScreen{((*seen)[0] + 1.0f) * 0.5f, (1.0f - (*seen)[1]) * 0.5f, false, 0.0f};
    }
    // Out of the picture: its way from the middle, by where it lies across the camera (to the
    // right, up) in the picture's own scale; straight behind, and behind above, read as below.
    const Vec3 d = Minus(point, CameraPosition(camera));
    const float t = std::tan(camera.fov * 0.5f);
    float right = Dot(d, Axis(camera.rows[0])) / (t * camera.aspect);
    float up = Dot(d, Axis(camera.rows[1])) / t;
    const bool behind = Dot(d, CameraLooks(camera)) <= camera.near_plane;
    if (std::abs(right) < 1e-4f && std::abs(up) < 1e-4f) {
        up = -1.0f;
    } else if (behind && up > 0.0f && up > std::abs(right)) {
        up = -up;
    }
    const float scale = edge / std::max(std::abs(right), std::abs(up));
    right *= scale;
    up *= scale;
    return PingOnScreen{(right + 1.0f) * 0.5f, (1.0f - up) * 0.5f, true,
                        std::atan2(-up / camera.aspect, right)};
}

PingAim AimPing(const PingCamera& camera, const PingPoint& feet,
                std::span<const PingCharacter> characters) {
    const Vec3 from = CameraPosition(camera), way = CameraLooks(camera);
    const float reach_from_camera = PingReach + Length(Minus(feet, from));
    // The first character standing close to the line.
    const PingCharacter* nearest = nullptr;
    float nearest_t = 0.0f;
    for (const PingCharacter& character : characters) {
        const Vec3 middle{character.at[0], character.at[1] + CharacterMiddle, character.at[2]};
        const float t = Dot(Minus(middle, from), way);
        if (t <= 0.5f || t > reach_from_camera ||
            Length(Minus(middle, Along(from, way, t))) > PingCharacterReach ||
            Length(Minus(character.at, feet)) > PingReach) {
            continue;
        }
        if (nearest == nullptr || t < nearest_t) {
            nearest = &character;
            nearest_t = t;
        }
    }
    PingAim aim;
    if (nearest != nullptr) {
        aim.at = nearest->at;
        aim.enemy = nearest->handle;
        return aim;
    }
    // Where the line meets the ground the player stands on, else a fixed way along it.
    bool on_ground = false;
    if (way[1] < -1e-3f) {
        const float t = (feet[1] - from[1]) / way[1];
        if (const Vec3 hit = Along(from, way, t);
            t > 0.0f && Length(Minus(hit, feet)) <= PingReach) {
            aim.at = hit;
            on_ground = true;
        }
    }
    if (!on_ground) {
        aim.at = Along(from, way, PingAimFallback);
    }
    const Vec3 off = Minus(aim.at, feet);
    if (const float far = Length(off); far > PingReach) {
        aim.at = Along(feet, off, PingReach / far);
    }
    return aim;
}

std::optional<PingCharacter> LockedCharacter(const PingCamera& camera, float cursor_x,
                                             float cursor_y,
                                             std::span<const PingCharacter> characters) {
    if (!LooksLikePingCamera(camera)) {
        return std::nullopt;
    }
    const float x = cursor_x / 1920.0f * 2.0f - 1.0f, y = 1.0f - cursor_y / 1080.0f * 2.0f;
    const PingCharacter* best = nullptr;
    float best_off = 0.0f;
    for (const PingCharacter& character : characters) {
        const auto seen = ProjectPing(
            camera, {character.at[0], character.at[1] + CharacterMiddle, character.at[2]});
        if (!seen) {
            continue;
        }
        // In the picture's heights (its -1..1 is 2 of them): x stretched by its shape.
        const float off = std::hypot(((*seen)[0] - x) * camera.aspect, (*seen)[1] - y) * 0.5f;
        if (off <= PingLockReach && (best == nullptr || off < best_off)) {
            best = &character;
            best_off = off;
        }
    }
    if (best == nullptr) {
        return std::nullopt;
    }
    return *best;
}

std::optional<PingAreaSpot> PingAreaAt(std::span<const PingArea> areas, u32 shown, float atlas_x,
                                       float atlas_z) {
    const PingArea* found = nullptr;
    for (const PingArea& area : areas) {
        if (atlas_x < area.x0 || atlas_x > area.x1 || atlas_z < area.z0 || atlas_z > area.z1) {
            continue;
        }
        if (found == nullptr || area.map == shown) {
            found = &area;
        }
    }
    if (found == nullptr) {
        return std::nullopt;
    }
    return PingAreaSpot{found->map, atlas_x - found->dx, atlas_z - found->dz};
}

std::string PingLabel(const std::string& name, std::optional<float> distance,
                      const std::string& area) {
    if (distance) {
        char line[32];
        std::snprintf(line, sizeof(line), *distance < 10.0f ? "  %.1f m" : "  %.0f m", *distance);
        return name + line;
    }
    return area.empty() ? name : name + "  " + area;
}

PingBox PlacePingLabel(float x, float y, float r, float width, float height,
                       std::span<const PingBox> taken, float gap) {
    const float off = r + gap;
    const PingBox places[] = {
        {x - width / 2, y + off, x + width / 2, y + off + height},  // under
        {x - width / 2, y - off - height, x + width / 2, y - off},  // over
        {x + off, y - height / 2, x + off + width, y + height / 2}, // right
        {x - off - width, y - height / 2, x - off, y + height / 2}, // left
    };
    const auto crowding = [&](const PingBox& place) {
        float area = 0.0f;
        for (const PingBox& name : taken) {
            const float w = std::min(place.x1, name.x1) - std::max(place.x0, name.x0);
            const float h = std::min(place.y1, name.y1) - std::max(place.y0, name.y0);
            if (w > 0.0f && h > 0.0f) {
                area += w * h;
            }
        }
        return area;
    };
    const PingBox* best = &places[0];
    float least = crowding(places[0]);
    for (const PingBox& place : places) {
        if (const float area = crowding(place); area < least) {
            best = &place;
            least = area;
        }
    }
    return *best;
}

} // namespace Core::Bloodborne
