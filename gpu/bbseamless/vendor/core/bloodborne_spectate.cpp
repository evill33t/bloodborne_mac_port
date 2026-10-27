// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// Spectating teammates and the free camera (Kaniel 2026-10-25), the pure part: see
// bloodborne_spectate.h.

#include "core/bloodborne_spectate.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace Core::Bloodborne {

namespace {

using Vec3 = std::array<float, 3>;

float Dot(const Vec3& a, const Vec3& b) {
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

Vec3 Cross(const Vec3& a, const Vec3& b) {
    return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
}

Vec3 Axis(const std::array<float, 4>& row) {
    return {row[0], row[1], row[2]};
}

// The way a camera at `heading` tilted `pitch` down looks (x east, y up, z north).
Vec3 Looks(float heading, float pitch) {
    return {std::sin(heading) * std::cos(pitch), -std::sin(pitch),
            std::cos(heading) * std::cos(pitch)};
}

// Stick bytes within this much of the middle turn and move nothing.
constexpr float StickDeadZone = 0.15f;

} // namespace

std::vector<std::string> SpectateTargets(std::span<const MemberVitals> board, std::string_view self,
                                         u32 map) {
    std::vector<std::string> targets;
    if (map == 0) {
        return targets;
    }
    for (const MemberVitals& member : board) {
        if (member.member != self && member.map == map && member.vital == Vital::Up) {
            targets.push_back(member.member);
        }
    }
    return targets;
}

std::optional<std::string> NextSpectateTarget(std::span<const std::string> targets,
                                              const std::optional<std::string>& current, int step) {
    if (targets.empty()) {
        return std::nullopt;
    }
    const auto at = current ? std::ranges::find(targets, *current) : targets.end();
    if (at == targets.end()) {
        return targets.front();
    }
    const auto count = static_cast<std::ptrdiff_t>(targets.size());
    const std::ptrdiff_t index = ((at - targets.begin()) + step % count + count) % count;
    return targets[static_cast<size_t>(index)];
}

SpectateEnd EndOfSpectate(const SpectateCheck& check) {
    if (check.wiped || (check.someone_dead && !check.someone_up && !check.someone_down)) {
        return SpectateEnd::Wipe;
    }
    if (check.gave_up) {
        return SpectateEnd::GiveUp;
    }
    if (check.teammate_used_a_lamp) {
        return SpectateEnd::Lamp;
    }
    if (check.boss_died) {
        return SpectateEnd::Boss;
    }
    if (!check.someone_up && !check.someone_down && !check.someone_dead &&
        check.alone_ms >= SpectateAloneMs) {
        return SpectateEnd::PartyLeft;
    }
    if (check.party_lost) {
        return SpectateEnd::PartyLost;
    }
    return SpectateEnd::None;
}

void SpectateCheckOthers(SpectateCheck& check, std::span<const MemberVitals> board,
                         std::string_view self, u32 map) {
    for (const MemberVitals& member : board) {
        if (member.member == self || map == 0 || member.map != map) {
            continue;
        }
        check.someone_up = check.someone_up || member.vital == Vital::Up;
        check.someone_down = check.someone_down || member.vital == Vital::Down;
        check.someone_dead = check.someone_dead || member.vital == Vital::Dead;
    }
}

std::string_view SpectateEndName(SpectateEnd end) {
    switch (end) {
    case SpectateEnd::Wipe:
        return "the party's wipe";
    case SpectateEnd::GiveUp:
        return "giving up";
    case SpectateEnd::Lamp:
        return "a teammate's lamp";
    case SpectateEnd::Boss:
        return "the boss down";
    case SpectateEnd::PartyLeft:
        return "nobody left on the map";
    case SpectateEnd::PartyLost:
        return "the party gone";
    case SpectateEnd::None:
        break;
    }
    return "nothing";
}

bool LampUseWatch::Tick(std::span<const MemberVitals> board, std::string_view self) {
    bool used = false;
    for (const MemberVitals& member : board) {
        if (member.member == self) {
            continue;
        }
        const auto known =
            std::ranges::find(seen, member.member, &std::pair<std::string, u32>::first);
        if (known == seen.end()) {
            seen.emplace_back(member.member, member.lamp_uses);
            continue;
        }
        used = used || member.lamp_uses > known->second;
        known->second = member.lamp_uses;
    }
    return used;
}

bool BossDownWatch::Tick(const std::vector<bool>& dead) {
    bool went = false;
    if (last.size() == dead.size()) {
        for (size_t i = 0; i < dead.size(); ++i) {
            went = went || (dead[i] && !last[i]);
        }
    }
    last.assign(dead.begin(), dead.end());
    return went;
}

float StickAxis(u8 value) {
    const float axis = std::clamp((static_cast<float>(value) - 128.0f) / 127.0f, -1.0f, 1.0f);
    return std::abs(axis) < StickDeadZone ? 0.0f : axis;
}

SpectatorPad SpectatorPadOf(const PadInput& pad, u32 buttons_before) {
    SpectatorPad out;
    const u32 pressed = pad.buttons & ~buttons_before;
    out.step = ((pressed & PadR1) != 0 ? 1 : 0) - ((pressed & PadL1) != 0 ? 1 : 0);
    out.turn_x = StickAxis(pad.sticks[2]);
    out.turn_y = StickAxis(pad.sticks[3]);
    return out;
}

float CameraHeadingOf(const PingCamera& camera) {
    const Vec3 looks = Axis(camera.rows[2]);
    return std::atan2(looks[0], looks[2]);
}

float CameraPitchOf(const PingCamera& camera) {
    return std::asin(std::clamp(-camera.rows[2][1], -1.0f, 1.0f));
}

PingCamera LookingCamera(const PingCamera& game, const PingPoint& eye, float heading, float pitch) {
    // The x axis on the side of the y and z axes the game's is on (+1: x = y cross z).
    const float hand = Dot(Axis(game.rows[0]), Cross(Axis(game.rows[1]), Axis(game.rows[2]))) < 0.0f
                           ? -1.0f
                           : 1.0f;
    const Vec3 looks = Looks(heading, pitch);
    const float sh = std::sin(heading), ch = std::cos(heading);
    const float sp = std::sin(pitch), cp = std::cos(pitch);
    const Vec3 up{sh * sp, cp, ch * sp}; // at right angles to `looks`, towards the heading
    const Vec3 cross = Cross(up, looks);
    PingCamera camera = game;
    camera.rows[0] = {hand * cross[0], hand * cross[1], hand * cross[2], 0.0f};
    camera.rows[1] = {up[0], up[1], up[2], 0.0f};
    camera.rows[2] = {looks[0], looks[1], looks[2], 0.0f};
    camera.rows[3] = {eye[0], eye[1], eye[2], 1.0f};
    return camera;
}

PingCamera SpectateCamera(const PingCamera& game, const PingPoint& target, float heading,
                          float pitch) {
    const Vec3 looks = Looks(heading, pitch);
    const PingPoint spot{target[0], target[1] + SpectateHeight, target[2]};
    const PingPoint eye{spot[0] - looks[0] * SpectateDistance,
                        spot[1] - looks[1] * SpectateDistance,
                        spot[2] - looks[2] * SpectateDistance};
    return LookingCamera(game, eye, heading, pitch);
}

CameraTurn TurnCamera(CameraTurn now, float turn_x, float turn_y, float mouse_x, float mouse_y,
                      float seconds) {
    constexpr float Pi = 3.14159265358979f;
    now.heading += turn_x * CameraTurnRate * seconds + mouse_x * CameraMouseRadians;
    now.pitch += turn_y * CameraTurnRate * seconds + mouse_y * CameraMouseRadians;
    now.heading = std::remainder(now.heading, 2.0f * Pi);
    now.pitch = std::clamp(now.pitch, -CameraPitchLimit, CameraPitchLimit);
    return now;
}

FreeCamera FreeCameraFrom(const PingCamera& camera) {
    return FreeCamera{.at = {camera.rows[3][0], camera.rows[3][1], camera.rows[3][2]},
                      .turn = {CameraHeadingOf(camera), CameraPitchOf(camera)}};
}

FreeCamera StepFreeCamera(const FreeCamera& camera, const PadInput& pad, float mouse_x,
                          float mouse_y, float seconds) {
    FreeCamera next = camera;
    next.turn = TurnCamera(camera.turn, StickAxis(pad.sticks[2]), StickAxis(pad.sticks[3]), mouse_x,
                           mouse_y, seconds);
    const float speed = FreeCameraSpeed * seconds *
                        ((pad.buttons & PadR2) != 0   ? FreeCameraFast
                         : (pad.buttons & PadL2) != 0 ? FreeCameraSlow
                                                      : 1.0f);
    const float ahead = -StickAxis(pad.sticks[1]), aside = StickAxis(pad.sticks[0]);
    const float rise =
        ((pad.buttons & PadR1) != 0 ? 1.0f : 0.0f) - ((pad.buttons & PadL1) != 0 ? 1.0f : 0.0f);
    const float sh = std::sin(camera.turn.heading), ch = std::cos(camera.turn.heading);
    // Ahead along the heading (level); to its right is (cos h, -sin h) in (x, z).
    next.at[0] += (ahead * sh + aside * ch) * speed;
    next.at[1] += rise * speed;
    next.at[2] += (ahead * ch - aside * sh) * speed;
    return next;
}

bool FreeCameraHotkey(u32 before, u32 now) {
    return (now & PadL3) != 0 && (now & PadSquare) != 0 && (before & PadSquare) == 0;
}

bool BossBarsUp(std::span<const u8> gauges) {
    for (size_t slot = 0; (slot + 1) * BossGaugeSize <= gauges.size(); ++slot) {
        s32 handle = 0;
        std::memcpy(&handle, gauges.data() + slot * BossGaugeSize + 4, sizeof(handle));
        if (handle != -1 && handle != 0) {
            return true;
        }
    }
    return false;
}

bool BossFightCatchesDeath(bool bars_up, std::span<const MemberVitals> board, std::string_view self,
                           u32 map, const std::optional<LampPosition>& me,
                           const std::optional<LampPosition>& boss) {
    const std::optional<LampPosition> centre = boss ? boss : me;
    if (!bars_up || !centre || map == 0) {
        return false;
    }
    return std::ranges::any_of(board, [&](const MemberVitals& member) {
        return member.member != self && member.map == map && member.vital == Vital::Up &&
               std::hypot(member.at[0] - (*centre)[0], member.at[1] - (*centre)[1],
                          member.at[2] - (*centre)[2]) <= BossLeaveMetres;
    });
}

std::optional<u32> BossBarHandle(std::span<const u8> gauges) {
    for (size_t slot = 0; (slot + 1) * BossGaugeSize <= gauges.size(); ++slot) {
        s32 handle = 0;
        std::memcpy(&handle, gauges.data() + slot * BossGaugeSize + 4, sizeof(handle));
        if (handle != -1 && handle != 0) {
            return static_cast<u32>(handle);
        }
    }
    return std::nullopt;
}

DeathCatch CatchOf(NoDeadCheck check, bool armed, bool down, bool answer, bool boss_fight,
                   bool spectating) {
    if (answer) {
        return DeathCatch::None;
    }
    if (KeepsPlayerAlive(check, armed, down, false)) {
        return DeathCatch::Down;
    }
    if ((check == NoDeadCheck::Lethal || check == NoDeadCheck::Instant) &&
        (boss_fight || spectating)) {
        return DeathCatch::BossFight;
    }
    return DeathCatch::None;
}

} // namespace Core::Bloodborne
