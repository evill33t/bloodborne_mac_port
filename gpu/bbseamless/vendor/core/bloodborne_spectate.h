// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// Kaniel 2026-10-25: two ways of looking at the game that are not the player's own camera. A team
// whose leader picked "Spectate teammates" (TeamDeathMode) does not go down: a lethal hit while a
// teammate stands on the map leaves the player unseen, unheard and unhurt where it fell, its
// camera behind a living teammate (L1 / R1 the next one), until a teammate uses a lamp or the
// area's boss dies; then it wakes at its own last lamp, without a reload. Holding Circle wakes it
// there at once. Everyone else on the map dead is the party's wipe, as it is today. And the free
// camera (an opt-in setting, a hotkey): the view flies apart from the player, whose input is held
// while it does. This file is the pure part: who is watched, what ends it, the pads, the cameras.

#pragma once

#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "common/types.h"
#include "core/bloodborne_party.h"
#include "core/bloodborne_pings.h"

namespace Core::Bloodborne {

// The teammates a spectator may watch: the other members standing on `map`, in the board's order.
std::vector<std::string> SpectateTargets(std::span<const MemberVitals> board, std::string_view self,
                                         u32 map);
// The one watched once `step` is taken (-1 L1, the one before; +1 R1, the next; 0 stays): the one
// watched now while it is still a target, else the first; round the list. None without targets.
std::optional<std::string> NextSpectateTarget(std::span<const std::string> targets,
                                              const std::optional<std::string>& current, int step);

// What ends spectating, checked in this order: a wipe (the board's for this member, or everyone
// else on its map dead with nobody up or down there), holding Circle, a teammate's lamp, the
// area's boss down, the map's party gone (nobody else up, down or dead there for SpectateAloneMs:
// they left), and the party itself gone (out of the session or the board unheard of). A wipe is
// the party's real death; every other end wakes the spectator at its own last lamp.
enum class SpectateEnd { None, Wipe, GiveUp, Lamp, Boss, PartyLeft, PartyLost };
constexpr s64 SpectateAloneMs = 2'000;
struct SpectateCheck {
    bool wiped = false;
    bool gave_up = false;
    bool teammate_used_a_lamp = false;
    bool boss_died = false;
    // The other members on this map, by their vitals.
    bool someone_up = false;
    bool someone_down = false;
    bool someone_dead = false;
    s64 alone_ms = 0; // how long nobody else was up, down or dead on this map
    bool party_lost = false;
};
SpectateEnd EndOfSpectate(const SpectateCheck& check);
// The board's others on `map` as a SpectateCheck has them (up, down, dead).
void SpectateCheckOthers(SpectateCheck& check, std::span<const MemberVitals> board,
                         std::string_view self, u32 map);
std::string_view SpectateEndName(SpectateEnd end);

// The lamps each other member used (MemberVitals::lamp_uses, counting up through its run): true
// when one went up since the last tick. A member seen the first time only sets where it starts.
class LampUseWatch {
public:
    bool Tick(std::span<const MemberVitals> board, std::string_view self);

private:
    std::vector<std::pair<std::string, u32>> seen;
};

// The bosses' defeat flags (EncounterInsights' kill events), read while spectating: true when one
// went on since the last reading. Restart forgets the readings (a new spectate).
class BossDownWatch {
public:
    bool Tick(const std::vector<bool>& dead);
    void Restart() {
        last.clear();
    }

private:
    std::vector<bool> last;
};

// The PS4 pad's L1 and Square (the same bits as AutoplayButton's), and L3.
constexpr u32 PadL1 = 0x400;
constexpr u32 PadL2 = 0x100;
constexpr u32 PadL3 = 0x2;
constexpr u32 PadSquare = 0x8000;
// A stick byte (0 to 255, 128 centred) as -1 to 1, nothing within the dead zone.
float StickAxis(u8 value);
// A spectator's pad: nothing of it reaches the game (Circle's hold to give up is read before);
// L1 / R1 going down step the teammate watched, the right stick turns the camera around them.
struct SpectatorPad {
    PadInput game; // what the game gets: nothing
    int step = 0;
    float turn_x = 0.0f; // -1 to 1, the right stick (x to the right, y down)
    float turn_y = 0.0f;
};
SpectatorPad SpectatorPadOf(const PadInput& pad, u32 buttons_before);

// The camera's heading as the compass has it (0 north, +z; growing to the right, toward +x) and
// its pitch (down from level), from the way it looks.
float CameraHeadingOf(const PingCamera& camera);
float CameraPitchOf(const PingCamera& camera);
// `game` moved to `eye`, looking along `heading` tilted `pitch` down: its axes keep their
// handedness (an x axis on the same side of the others as the game's) and its view (field of
// view, planes) stays.
PingCamera LookingCamera(const PingCamera& game, const PingPoint& eye, float heading, float pitch);
// Kaniel 2026-10-25: the spectator's camera, over the watched teammate's shoulder: the spot it
// looks at SpectateHeight over the teammate's feet, SpectateDistance behind it along the heading.
constexpr float SpectateDistance = 4.5f;
constexpr float SpectateHeight = 1.4f;
// How fast a full stick tilt turns a co-op camera (radians a second), how far a mouse count does,
// and the pitch it keeps within (up and down).
constexpr float CameraTurnRate = 2.4f;
constexpr float CameraMouseRadians = 0.0025f;
constexpr float CameraPitchLimit = 1.45f;
PingCamera SpectateCamera(const PingCamera& game, const PingPoint& target, float heading,
                          float pitch);
// The heading and pitch after a turn by the stick (`turn_x`, `turn_y`, -1 to 1) for `seconds` and
// by the mouse (`mouse_x`, `mouse_y`, counts); the pitch kept within CameraPitchLimit.
struct CameraTurn {
    float heading = 0.0f;
    float pitch = 0.0f;
};
CameraTurn TurnCamera(CameraTurn now, float turn_x, float turn_y, float mouse_x, float mouse_y,
                      float seconds);

// The free camera: where it is and how it looks. Moved by the left stick (along its heading,
// level), R1 / L1 up / down, turned by the right stick and the mouse; R2 held flies fast, L2 slow.
struct FreeCamera {
    PingPoint at{};
    CameraTurn turn;
};
constexpr float FreeCameraSpeed = 6.0f; // metres a second at full tilt
constexpr float FreeCameraFast = 4.0f;
constexpr float FreeCameraSlow = 0.25f;
FreeCamera FreeCameraFrom(const PingCamera& camera);
FreeCamera StepFreeCamera(const FreeCamera& camera, const PadInput& pad, float mouse_x,
                          float mouse_y, float seconds);
// Whether the free camera's pad hotkey went down: Square pressed while L3 is held.
bool FreeCameraHotkey(u32 before, u32 now);

// Kaniel 2026-10-25 ("if you are in the boss fight and you are died like that you should be in the
// new spectator mode instead of respawning", "and then trigger overall death"; guests fell into
// Logarius' pit one by one and woke at lamps mid-fight): in a boss fight, a death the down does not
// catch (the game's kills: an event's kill plane, a fall into a pit's void) spectates while a
// teammate still fights there, in either death mode. The last fighter's death is then the party's
// wipe for everyone (EndOfSpectate). Outside boss fights nothing changes.
// Whether the game's boss bars (BossGaugesOffset, BossGaugeSlots of BossGaugeSize) show a boss: a
// slot with a boss's handle (-1 none; 0 never one either).
bool BossBarsUp(std::span<const u8> gauges);
// A boss fight (its bars up in this game) with a teammate standing in it: another member up on
// `map` within BossLeaveMetres of the boss (`boss`, its bar's character), or of this player (`me`)
// while the boss's place is not known; nothing while neither is.
bool BossFightCatchesDeath(bool bars_up, std::span<const MemberVitals> board, std::string_view self,
                           u32 map, const std::optional<LampPosition>& me,
                           const std::optional<LampPosition>& boss = std::nullopt);
// The first boss the bars show (its handle), if any.
std::optional<u32> BossBarHandle(std::span<const u8> gauges);
// What the IsNoDead hook makes of a check that would kill the player: nothing (the game's own
// answer), a down's catch (party death's rule, KeepsPlayerAlive: the team's death mode follows),
// or a boss fight's: its lethal checks and the game's kills (Instant) while `boss_fight`
// (BossFightCatchesDeath) or `spectating` (a spectator stays out of reach of every kill).
enum class DeathCatch { None, Down, BossFight };
DeathCatch CatchOf(NoDeadCheck check, bool armed, bool down, bool answer, bool boss_fight,
                   bool spectating);

} // namespace Core::Bloodborne
