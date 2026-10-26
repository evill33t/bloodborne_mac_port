// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// Kaniel 2026-10-25: the co-op cameras, the game's camera put elsewhere for a frame: over a
// watched teammate's shoulder while this player spectates (bloodborne_party_death.cpp), or flying
// free (the free camera, an opt-in setting and a hotkey). The frame step reads its camera manager
// at CoopCameraHookOffset once it has updated, before the frame's cameras are copied from it; the
// camera written there is the one the frame is drawn with (the overhead map's way, PR #320). The
// pure part (where the cameras go) is bloodborne_spectate.cpp.

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstring>
#include <mutex>
#include <optional>
#include <string>
#include <utility>

#include "common/logging/log.h"
#include "core/bloodborne_re.h"
#include "core/bloodborne_re_internal.h"
#include "core/bloodborne_spectate.h"
#include "core/cpu_patches.h"

namespace Core::Bloodborne {

namespace {

using Clock = std::chrono::steady_clock;

// The frame step's camera manager (r13 the step, its camera manager at +0x128) and the camera the
// frame is drawn with in it.
constexpr u64 StepCameraManagerField = 0x128;
constexpr u64 FrameCameraAt = 0x10;
static_assert(sizeof(PingCamera) == 0x50);

std::mutex camera_mutex;
// The spectator's camera: the watched teammate's character (its handle, 0 none) and its name.
u32 spectate_handle = 0;
std::string spectate_name;
// The free camera: allowed by the setting, on, and its pad (the latest read).
bool free_allowed = false;
bool free_on = false;
PadInput free_pad;
u32 free_pad_before = 0;
// Both cameras' turn: the right stick's tilt (the pad's latest) and the mouse's counts since the
// last frame.
float turn_x = 0.0f, turn_y = 0.0f;
float mouse_x = 0.0f, mouse_y = 0.0f;

// The frame step's own state (the game's thread only).
struct StepState {
    Clock::time_point last{};
    // The spectator's camera, once it follows someone.
    std::optional<CameraTurn> spectate_turn;
    u32 spectate_logged = 0;
    // The free camera, once it flies; where it and the player began.
    std::optional<FreeCamera> free;
    PingPoint free_from{};
    std::optional<LampPosition> player_from;
};
StepState step_state;

std::optional<LampPosition> HandlePosition(u32 handle) {
    for (const OtherPlayer& other : OtherPlayers()) {
        if (ReadValue<u32>(other.character, 0x08) == handle) {
            return other.at;
        }
    }
    return std::nullopt;
}

float Distance(const LampPosition& a, const LampPosition& b) {
    return std::hypot(a[0] - b[0], a[1] - b[1], a[2] - b[2]);
}

void FreeCameraEnded(StepState& state) {
    if (!state.free) {
        return;
    }
    const auto player = LocalPlayerPosition();
    LOG_INFO(Debug,
             "Bloodborne camera: the free camera is off: it flew {:.1f} m from where it began; the "
             "player moved {:.1f} m meanwhile",
             Distance(state.free->at, state.free_from),
             player && state.player_from ? Distance(*player, *state.player_from) : -1.0f);
    state.free.reset();
    state.player_from.reset();
}

// The camera this frame is drawn with, if a co-op camera has it; `game` the game's own.
std::optional<PingCamera> CoopCameraFor(const PingCamera& game, StepState& state, float seconds) {
    u32 handle = 0;
    std::string name;
    bool flying = false;
    PadInput pad;
    float tx = 0.0f, ty = 0.0f, mx = 0.0f, my = 0.0f;
    {
        std::scoped_lock lock{camera_mutex};
        handle = spectate_handle;
        name = spectate_name;
        flying = free_on;
        pad = free_pad;
        tx = turn_x;
        ty = turn_y;
        mx = std::exchange(mouse_x, 0.0f);
        my = std::exchange(mouse_y, 0.0f);
    }
    if (!flying) {
        FreeCameraEnded(state);
    }
    if (flying) {
        if (!state.free) {
            state.free = FreeCameraFrom(game);
            state.free_from = state.free->at;
            state.player_from = LocalPlayerPosition();
            LOG_INFO(Debug, "Bloodborne camera: the free camera is on at ({:.1f}, {:.1f}, {:.1f})",
                     state.free->at[0], state.free->at[1], state.free->at[2]);
        }
        state.free = StepFreeCamera(*state.free, pad, mx, my, seconds);
        return LookingCamera(game, state.free->at, state.free->turn.heading,
                             state.free->turn.pitch);
    }
    const std::optional<LampPosition> target = handle != 0 ? HandlePosition(handle) : std::nullopt;
    if (!target) {
        if (state.spectate_turn) {
            LOG_INFO(Debug, "Bloodborne camera: the game's camera is its own again");
        }
        state.spectate_turn.reset();
        state.spectate_logged = 0;
        return std::nullopt;
    }
    if (!state.spectate_turn) {
        // From where the game's camera looked, a little from above.
        state.spectate_turn = CameraTurn{CameraHeadingOf(game), 0.3f};
    }
    state.spectate_turn = TurnCamera(*state.spectate_turn, tx, ty, mx, my, seconds);
    if (state.spectate_logged != handle) {
        state.spectate_logged = handle;
        LOG_INFO(Debug, "Bloodborne camera: over {}'s shoulder ({:#x} at ({:.1f}, {:.1f}, {:.1f}))",
                 name, handle, (*target)[0], (*target)[1], (*target)[2]);
    }
    return SpectateCamera(game, *target, state.spectate_turn->heading, state.spectate_turn->pitch);
}

} // namespace

namespace Coop {

void PS4_SYSV_ABI CoopCameraStep(u64, const GuestRegisterSnapshot* registers) {
    const u64 manager = ReadValue<u64>(registers->r13, StepCameraManagerField);
    if (manager < 0x10000 ||
        !HasMemoryAccess(manager + FrameCameraAt, sizeof(PingCamera), MemoryProt::CpuReadWrite)) {
        return;
    }
    PingCamera game;
    std::memcpy(&game, reinterpret_cast<const void*>(manager + FrameCameraAt), sizeof(game));
    if (!LooksLikePingCamera(game)) {
        return;
    }
    StepState& state = step_state;
    const Clock::time_point now = Clock::now();
    const float seconds =
        state.last == Clock::time_point{}
            ? 0.0f
            : std::clamp(std::chrono::duration<float>(now - state.last).count(), 0.0f, 0.1f);
    state.last = now;
    if (const auto camera = CoopCameraFor(game, state, seconds)) {
        std::memcpy(reinterpret_cast<void*>(manager + FrameCameraAt), &*camera, sizeof(*camera));
    }
}

void SetSpectateCamera(u32 handle, std::string_view name) {
    std::scoped_lock lock{camera_mutex};
    spectate_handle = handle;
    spectate_name = name;
    if (handle == 0) {
        turn_x = turn_y = 0.0f;
    }
}

void CoopCameraTurn(float x, float y) {
    std::scoped_lock lock{camera_mutex};
    turn_x = x;
    turn_y = y;
}

bool ToggleFreeCameraNow(bool forced) {
    std::scoped_lock lock{camera_mutex};
    if (!free_allowed && !forced && !free_on) {
        LOG_INFO(Debug, "Bloodborne camera: the free camera's hotkey: it is off in the settings");
        return false;
    }
    free_on = !free_on;
    free_pad = {};
    return free_on;
}

bool FreeCameraOn() {
    std::scoped_lock lock{camera_mutex};
    return free_on;
}

} // namespace Coop

bool CoopCameraMouse(float dx, float dy) {
    std::scoped_lock lock{camera_mutex};
    if (!free_on && spectate_handle == 0) {
        return false;
    }
    mouse_x += dx;
    mouse_y += dy;
    return true;
}

void SetFreeCameraAllowed(bool allowed) {
    std::scoped_lock lock{camera_mutex};
    if (free_allowed && !allowed && free_on) {
        free_on = false; // switched off in the menu: the game's camera at once
    }
    free_allowed = allowed;
}

void ToggleFreeCamera() {
    Coop::ToggleFreeCameraNow(false);
}

bool FreeCameraPad(u32& buttons, std::array<u8, 4>& sticks, u8& l2, u8& r2) {
    bool toggle = false;
    {
        std::scoped_lock lock{camera_mutex};
        toggle = free_allowed && FreeCameraHotkey(free_pad_before, buttons);
        free_pad_before = buttons;
    }
    if (toggle) {
        Coop::ToggleFreeCameraNow(false);
        buttons &= ~(PadSquare | PadL3); // the hotkey's press is not the game's
    }
    std::scoped_lock lock{camera_mutex};
    if (!free_on) {
        return false;
    }
    free_pad = PadInput{.buttons = buttons, .sticks = sticks, .l2 = l2, .r2 = r2};
    buttons = 0;
    sticks = {128, 128, 128, 128};
    l2 = r2 = 0;
    return true;
}

} // namespace Core::Bloodborne
