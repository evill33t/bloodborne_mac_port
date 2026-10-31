// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// The rig's autoplay: its script verbs, census and pad. Split from bloodborne_re.cpp; the
// shared declarations are in bloodborne_re_internal.h.

#include "core/bloodborne_compass.h"
#include "core/bloodborne_crash_dump.h"
#include "core/bloodborne_duel.h"
#include "core/bloodborne_party.h"
#include "core/bloodborne_pings.h"
#include "core/bloodborne_re.h"
#include "core/bloodborne_re_internal.h"
#include "core/bloodborne_trade.h"
#include "core/bloodborne_write_watch.h"
#include "core/libraries/network/server_link.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <charconv>
#include "common/float_chars.h"
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <map>
#include <mutex>
#include <numbers>
#include <optional>
#include <random>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>
#ifdef _WIN32
#include <process.h>
#else
#include <unistd.h>
#endif

#include <fmt/ranges.h>
#include "common/logging/log.h"
#include "common/memory_patcher.h"
#include "common/path_util.h"
#include "common/singleton.h"
#include "core/cpu_patches.h"
#include "core/debugger.h"
#include "core/emulator_settings.h"
#include "core/file_format/psf.h"
#include "core/libraries/network/sockets.h"
#include "core/libraries/np/np_handler.h"
#include "core/libraries/np/np_matching2/np_matching2_signaling.h"
#include "core/libraries/np/np_trophy.h"
#include "core/libraries/system/userservice.h"
#include "core/memory.h"

namespace Core::Bloodborne {
namespace {

// Autoplay (SHADPS4_BLOODBORNE_AUTOPLAY=<script>, see ParseAutoplayScript): tests with no one at
// the pad. The pad reader (AutoplayPad) presses Cross on the title screen until the player is in
// the world; the party tick then runs the script's commands one after the other, logging each
// ("Bloodborne autoplay: step 3/9 travel 2102962 ok"), and "Bloodborne autoplay: done (ok)" or
// "done (failed)" at the end, which the rig's test driver waits for.
struct AutoplayRun {
    enum class Phase { Off, Title, World, Done };
    Phase phase = Phase::Off;
    std::vector<AutoplayCommand> script;
    size_t next = 0;
    std::chrono::steady_clock::time_point started{};
    std::chrono::steady_clock::time_point step_started{};
    bool step_begun = false;
    std::optional<std::chrono::steady_clock::time_point> hold_until;
    u32 travel_map = 0;
    std::optional<double> travel_fixed_at;     // seconds into the step it was moved to the lamp
    const LampPoint* travel_planned = nullptr; // the lamp the travel was placed at, if any
    u32 attack_handle = 0;
    std::optional<u32> attack_placed; // the attack cycle the player was last put in place for
    s32 noted_max_hp = 0;             // note-max-hp: the player's max HP then
    std::optional<std::pair<s32,s32>> noted_wallet; // echoes and Insight before a reward
    u32 pickup_handle = 0;            // pickup: the map item it went for
    LampPosition pickup_at{};         // pickup: where the player was put on it
    s32 hit_from_hp = 0;              // hit: the target's HP when it began
    u64 bullets_from = 0;             // wait-bullets: how many this game had made when it began
    std::string script_path;          // sync: the go files are beside it (AutoplaySyncPath)
    std::string chapter;              // the chapter the script is in (a rig suite's section)
    // chapter <name> <seconds>: when the chapter's time budget runs out (the rig's, 2026-10-15).
    std::optional<std::chrono::steady_clock::time_point> chapter_deadline;
    double chapter_budget = 0.0;
    bool keep_going = false;   // keep-going on: a failed step gives up only its chapter
    bool failed = false;       // a step failed and the script went on: "done (failed)"
    LampPosition glide_from{}; // glide: where the player was when it began
    // watch-unseen: the character watched and until when, and how often it was seen drawn.
    u32 watch_handle = 0;
    std::optional<std::chrono::steady_clock::time_point> watch_until;
    std::optional<std::chrono::steady_clock::time_point> watch_started;
    u32 watch_seen = 0;
    // watch-effects: the character whose SpEffects are logged, until when, the last full line's
    // time and what it had at the last look.
    u32 effects_handle = 0;
    std::optional<std::chrono::steady_clock::time_point> effects_until;
    std::optional<std::chrono::steady_clock::time_point> effects_logged;
    std::vector<s32> effects_seen;
    // quit-title: back to the title phase; its steps wait until the title menu is up and the
    // lobby has heard of it (title_visits: the title menu's visits when it quit).
    bool leaving_world = false;
    u64 title_visits = 0;
};
std::mutex autoplay_mutex;
AutoplayRun autoplay_run;
std::atomic<u32> autoplay_buttons{0};
std::atomic<bool> autoplay_godmode{false};
std::atomic<u32> autoplay_sticks{0x80808080}; // lx, ly, rx, ry
std::once_flag autoplay_loaded;

void LoadAutoplay() {
    const char* path = std::getenv("SHADPS4_BLOODBORNE_AUTOPLAY");
    if (path == nullptr || *path == '\0') {
        return;
    }
    std::ifstream file(path);
    const std::string text{std::istreambuf_iterator<char>{file}, {}};
    std::scoped_lock lock{autoplay_mutex};
    autoplay_run.script = ParseAutoplayScript(text);
    autoplay_run.script_path = path;
    autoplay_run.phase = AutoplayRun::Phase::Title;
    autoplay_run.started = std::chrono::steady_clock::now();
    LOG_INFO(Debug, "Bloodborne autoplay: {} steps from {}", autoplay_run.script.size(), path);
}

// The rig's checks and reports read the game's own map (GetCurrentPackedMap), not the place
// tracker: they test the game, the tracker included; only comparisons with a board's maps use
// the tracker's, as the boards do.
bool AutoplayInWorld() {
    const u64 global = GetGlobalState();
    return GetLocalPlayer() >= 0x10000 && IsUsablePackedMap(GetCurrentPackedMap()) &&
           global >= 0x10000 && HasMemoryAccess(global, 0x09, MemoryProt::CpuRead) &&
           ReadValue<u8>(global, 0x08) == 0 && LocalPlayerPosition().has_value();
}

size_t AutoplaySessionMembers() {
    const u64 session = ReadValue<u64>(image_base + SessionManagerPointerOffset, 0);
    if (session < 0x10000 || !HasMemoryAccess(session, 0x128, MemoryProt::CpuRead)) {
        return 1;
    }
    const u64 begin = ReadValue<u64>(session, 0xF8);
    const u64 end = ReadValue<u64>(session, 0x100);
    return begin >= 0x10000 && end > begin && end - begin <= 0x20 * 8 ? (end - begin) / 0x20 : 1;
}

size_t AutoplayMembersOnMyMap() {
    const u32 map = PlayingMap();
    std::scoped_lock lock{map_board_mutex};
    if (std::chrono::steady_clock::now() - map_board_time > std::chrono::seconds{3}) {
        return 0;
    }
    return static_cast<size_t>(
        std::ranges::count_if(map_board, [&](const MemberMap& entry) { return entry.map == map; }));
}

struct AutoplayCharacter {
    u64 character = 0;
    u64 data = 0;
    float distance = 0.0f;
};

// The character a command names: by handle, or the nearest living one within 30 m (0).
std::atomic<u32> autoplay_marked{0};

std::optional<AutoplayCharacter> FindAutoplayCharacter(u32 handle) {
    if (handle == AutoplayMarked) {
        handle = autoplay_marked.load();
        if (handle == 0) {
            return std::nullopt;
        }
    }
    // Other players are not among the world's characters: the nearest one shown, by its body.
    if (handle == AutoplayPlayer || (handle != 0 && handle != AutoplaySelf)) {
        const auto me = LocalPlayerPosition();
        std::optional<AutoplayCharacter> nearest;
        for (const OtherPlayer& other : OtherPlayers()) {
            const u64 modules = ReadValue<u64>(other.character, 0x3B0);
            const u64 data = modules >= 0x10000 ? ReadValue<u64>(modules, 0x20) : 0;
            if (!me || !other.at || data < 0x10000 ||
                (handle != AutoplayPlayer && ReadValue<u32>(other.character, 0x08) != handle) ||
                (handle == AutoplayPlayer && ReadValue<s32>(data, 0xF8) <= 0)) {
                continue;
            }
            const float distance = std::hypot((*other.at)[0] - (*me)[0], (*other.at)[1] - (*me)[1],
                                              (*other.at)[2] - (*me)[2]);
            if (!nearest || distance < nearest->distance) {
                nearest = AutoplayCharacter{other.character, data, distance};
            }
        }
        if (nearest || handle == AutoplayPlayer) {
            return nearest;
        }
    }
    if (handle == AutoplaySelf) {
        const u64 player = GetLocalPlayer();
        const u64 modules = player >= 0x10000 ? ReadValue<u64>(player, 0x3B0) : 0;
        const u64 data = modules >= 0x10000 ? ReadValue<u64>(modules, 0x20) : 0;
        return data >= 0x10000 ? std::optional{AutoplayCharacter{player, data, 0.0f}}
                               : std::nullopt;
    }
    const u64 world = ReadValue<u64>(image_base + CandidateLocalStatePointerOffset, 0);
    const auto me = LocalPlayerPosition();
    if (world < 0x10000 || !me) {
        return std::nullopt;
    }
    std::optional<AutoplayCharacter> found;
    ForEachWorldCharacter(world, [&](u64 character) {
        if (character == GetLocalPlayer()) {
            return;
        }
        const u64 modules = ReadValue<u64>(character, 0x3B0);
        const u64 data = modules >= 0x10000 ? ReadValue<u64>(modules, 0x20) : 0;
        const u64 physics = modules >= 0x10000 ? ReadValue<u64>(modules, 0x68) : 0;
        if (data < 0x10000 || physics < 0x10000) {
            return;
        }
        const float dx = ReadValue<float>(physics, 0x1E0) - (*me)[0];
        const float dy = ReadValue<float>(physics, 0x1E4) - (*me)[1];
        const float dz = ReadValue<float>(physics, 0x1E8) - (*me)[2];
        const float distance = std::sqrt(dx * dx + dy * dy + dz * dz);
        if (handle != 0) {
            if (ReadValue<u32>(character, 0x08) == handle) {
                found = AutoplayCharacter{character, data, distance};
            }
            return;
        }
        if (ReadValue<s32>(data, 0xF8) > 0 && distance < 30.0f &&
            (!found || distance < found->distance)) {
            found = AutoplayCharacter{character, data, distance};
        }
    });
    return found;
}

void LogAutoplaySnapshot() {
    const auto at = LocalPlayerPosition();
    std::string enemies;
    const u64 world = ReadValue<u64>(image_base + CandidateLocalStatePointerOffset, 0);
    std::vector<std::pair<float, std::string>> close_by;
    if (world >= 0x10000 && at) {
        ForEachWorldCharacter(world, [&](u64 character) {
            if (character == GetLocalPlayer()) {
                return;
            }
            const u64 modules = ReadValue<u64>(character, 0x3B0);
            const u64 data = modules >= 0x10000 ? ReadValue<u64>(modules, 0x20) : 0;
            const u64 physics = modules >= 0x10000 ? ReadValue<u64>(modules, 0x68) : 0;
            if (data < 0x10000 || physics < 0x10000) {
                return;
            }
            const float dx = ReadValue<float>(physics, 0x1E0) - (*at)[0];
            const float dy = ReadValue<float>(physics, 0x1E4) - (*at)[1];
            const float dz = ReadValue<float>(physics, 0x1E8) - (*at)[2];
            const float distance = std::sqrt(dx * dx + dy * dy + dz * dz);
            if (distance < 30.0f) {
                close_by.emplace_back(
                    distance,
                    fmt::format("{:#x} hp {}/{} dead {:#x} at {:.1f} m ({:.1f}, {:.1f}, {:.1f})",
                                ReadValue<u32>(character, 0x08), ReadValue<s32>(data, 0xF8),
                                ReadValue<s32>(data, 0xFC), ReadValue<u8>(character, 0x1E3),
                                distance, ReadValue<float>(physics, 0x1E0),
                                ReadValue<float>(physics, 0x1E4),
                                ReadValue<float>(physics, 0x1E8)));
            }
        });
    }
    std::ranges::sort(close_by);
    for (size_t index = 0; index < close_by.size() && index < 6; ++index) {
        enemies += " [" + close_by[index].second + "]";
    }
    const auto self = FindAutoplayCharacter(AutoplaySelf);
    // Raw, as all of the rig's checks (see AutoplayInWorld).
    LOG_INFO(Debug,
             "Bloodborne autoplay snapshot: map {:#x} at ({:.1f}, {:.1f}, {:.1f}) heading {:.2f} "
             "hp {}/{} session {} members on map {} in party session {} nearby:{}",
             GetCurrentPackedMap(), at ? (*at)[0] : 0.0f, at ? (*at)[1] : 0.0f,
             at ? (*at)[2] : 0.0f, LocalPlayerHeading().value_or(0.0f),
             self ? ReadValue<s32>(self->data, 0xF8) : 0,
             self ? ReadValue<s32>(self->data, 0xFC) : 0, AutoplaySessionMembers(),
             AutoplayMembersOnMyMap(), InPartySession(), enemies.empty() ? " none" : enemies);
}

// census [<meters>]: every character that close (default 60 m), nearest first, with its HP, death
// flags (+0x1E3), story hide (+0x28) and explicit disable ([+0x18]+0x20 bit 0): which world
// shows or killed what (the clinic's beast, Iosefka).
// A character's backread level ([+0x18] +0x10 now, +0x14 wanted; 4 full, 0 none: the state
// machine 0x018BD320) and its draw flags ([+0x58] +0x114, bit 0x40 cleared by the enable switch),
// for the census: which copy of a character is drawn in one world and not the other.
s32 BackreadLevel(u64 character, u64 offset) {
    const u64 control = ReadValue<u64>(character, 0x18);
    return control >= 0x10000 ? ReadValue<s32>(control, offset) : -1;
}

u32 DrawFlags(u64 character) {
    const u64 model = ReadValue<u64>(character, 0x58);
    return model >= 0x10000 && HasMemoryAccess(model, 0x116, MemoryProt::CpuRead)
               ? ReadValue<u16>(model, 0x114)
               : 0xFFFFFFFF;
}

void LogAutoplayCensus(float meters) {
    const auto at = LocalPlayerPosition();
    const u64 world = ReadValue<u64>(image_base + CandidateLocalStatePointerOffset, 0);
    if (world < 0x10000 || !at) {
        LOG_INFO(Debug, "Bloodborne autoplay census: no world");
        return;
    }
    std::vector<std::pair<float, std::string>> close_by;
    const std::unordered_map<u64, s32> entities = CharacterEntityIds(world);
    ForEachWorldCharacter(world, [&](u64 character) {
        if (character == GetLocalPlayer()) {
            return;
        }
        const u64 modules = ReadValue<u64>(character, 0x3B0);
        const u64 data = modules >= 0x10000 ? ReadValue<u64>(modules, 0x20) : 0;
        const u64 physics = modules >= 0x10000 ? ReadValue<u64>(modules, 0x68) : 0;
        if (data < 0x10000 || physics < 0x10000) {
            return;
        }
        const float x = ReadValue<float>(physics, 0x1E0);
        const float y = ReadValue<float>(physics, 0x1E4);
        const float z = ReadValue<float>(physics, 0x1E8);
        const float dx = x - (*at)[0];
        const float dy = y - (*at)[1];
        const float dz = z - (*at)[2];
        const float distance = std::sqrt(dx * dx + dy * dy + dz * dz);
        if (distance < meters) {
            close_by.emplace_back(
                distance,
                fmt::format("{:#x} hp {}/{} dead {:#x} hidden {} disabled {} at {:.1f} m "
                            "({:.1f}, {:.1f}, {:.1f}) backread {}/{} draw {:#x} team {} entity {}",
                            ReadValue<u32>(character, 0x08), ReadValue<s32>(data, 0xF8),
                            ReadValue<s32>(data, 0xFC), ReadValue<u8>(character, 0x1E3),
                            ReadValue<u8>(character, 0x28), IsCharacterDisabled(character) ? 1 : 0,
                            distance, x, y, z, BackreadLevel(character, 0x10),
                            BackreadLevel(character, 0x14), DrawFlags(character),
                            ReadValue<s32>(character, CharacterTeamOffset),
                            entities.contains(character) ? entities.at(character) : -1));
        }
    });
    std::ranges::sort(close_by);
    // Raw, as all of the rig's checks (see AutoplayInWorld).
    LOG_INFO(Debug,
             "Bloodborne autoplay census: map {:#x}, {} characters within {:.0f} m of "
             "({:.1f}, {:.1f}, {:.1f})",
             GetCurrentPackedMap(), close_by.size(), meters, (*at)[0], (*at)[1], (*at)[2]);
    for (const auto& [distance, line] : close_by) {
        LOG_INFO(Debug, "Bloodborne autoplay census: {}", line);
    }
}

std::string AutoplayText(const AutoplayCommand& command) {
    std::string text = command.verb;
    for (const std::string& arg : command.args) {
        text += ' ' + arg;
    }
    return text;
}

// One script command's tick, as RunAutoplayStep hands it to the command's verb: its words, whether
// it ran before (begun), the seconds since it began, and where a failure goes.
struct AutoplayStep {
    const AutoplayCommand& command;
    bool begun;
    double elapsed;
    std::string& failure;

    std::string_view Arg(size_t index) const {
        return index < command.args.size() ? std::string_view{command.args[index]}
                                           : std::string_view{};
    }
    double Number(size_t index, double fallback) const {
        const std::string_view text = Arg(index);
        double value = fallback;
        if (!text.empty()) {
            BbSeamless::FloatChars(text.data(), text.data() + text.size(), value);
        }
        return value;
    }
    // Not done yet; failed once `limit` seconds have gone by.
    bool WaitedOut(double limit) const {
        if (elapsed > limit) {
            failure = fmt::format("timed out after {:.0f} s", elapsed);
        }
        return false;
    }
};

bool AutoplayWait(const AutoplayStep& step) {
    return step.elapsed >= step.Number(0, 1.0);
}

bool AutoplayWaitSession(const AutoplayStep& step) {
    // In a team, until its load screen is gone too (Kaniel 2026-10-16: it stays a while longer
    // now, and a member under it is not up on the vitals board yet).
    return (AutoplaySessionMembers() >= static_cast<size_t>(step.Number(0, 2)) &&
            !Coop::TeamLoadCovers()) ||
           step.WaitedOut(step.Number(1, 180));
}

// wait-solo [<timeout>]: until the session is gone (a member quit or crashed).
bool AutoplayWaitSolo(const AutoplayStep& step) {
    return AutoplaySessionMembers() <= 1 || step.WaitedOut(step.Number(0, 180));
}

bool AutoplayWaitMembers(const AutoplayStep& step) {
    return AutoplayMembersOnMyMap() >= static_cast<size_t>(step.Number(0, 2)) ||
           step.WaitedOut(step.Number(1, 180));
}

bool AutoplayWaitMap(const AutoplayStep& step) {
    const auto map = ParseAutoplayTarget(step.Arg(0));
    // Raw, as all of the rig's checks (see AutoplayInWorld).
    return (map && AutoplayInWorld() && GetCurrentPackedMap() == *map) ||
           step.WaitedOut(step.Number(1, 180));
}

// wait-vital <up|down|dead> [<timeout>]: until this player's party death state is that.
bool AutoplayWaitVital(const AutoplayStep& step) {
    const auto vital = ParseVital(step.Arg(0));
    if (!vital) {
        step.failure = "no such vital";
        return false;
    }
    return party_death_run.downed.Current() == *vital || step.WaitedOut(step.Number(1, 60));
}

// Kaniel 2026-10-25: death-mode <down|spectate> [<timeout>]: the team leader picks its team's
// death mode (as its team screen does); done once this game's lobby says so. wait-death-mode
// <down|spectate> [<timeout>]: until it does (a member's game, the leader's pick).
std::optional<DeathMode> ParseDeathMode(std::string_view name) {
    return name == "down"       ? std::optional{DeathMode::GoDown}
           : name == "spectate" ? std::optional{DeathMode::Spectate}
                                : std::nullopt;
}

bool AutoplayDeathMode(const AutoplayStep& step) {
    const auto mode = ParseDeathMode(step.Arg(0));
    if (!mode) {
        step.failure = "no such death mode";
        return false;
    }
    if (Coop::TeamDeathModeNow() == *mode) {
        return true;
    }
    if (step.command.verb == "death-mode" && !step.begun) {
        Coop::TeamLobbyRequest(bbcoop::TeamRequest::DEATH_MODE,
                               *mode == DeathMode::Spectate ? "1" : "0");
    }
    return step.WaitedOut(step.Number(1, 60));
}

// Kaniel 2026-10-25: lamp-use: a lamp use counted, as the lamp's travel counts it (the rig's X
// at a lamp brings no prompt): a spectating teammate wakes on it.
bool AutoplayLampUse(const AutoplayStep&) {
    const u32 uses = Coop::lamp_uses.fetch_add(1) + 1;
    LOG_INFO(Debug, "Bloodborne autoplay: a lamp use counted ({} this run)", uses);
    return true;
}

// free-camera <on|off> [<timeout>]: the free camera's hotkey (as F7), until it is on or off.
bool AutoplayFreeCamera(const AutoplayStep& step) {
    const bool on = step.Arg(0) == "on";
    if (!on && step.Arg(0) != "off") {
        step.failure = "free-camera takes on or off";
        return false;
    }
    if (Coop::FreeCameraOn() == on) {
        return true;
    }
    if (!step.begun) {
        Coop::ToggleFreeCameraNow(false);
    }
    return step.WaitedOut(step.Number(1, 10));
}

// wait-member-vital <vital> [<timeout>]: until another member's is, on the vitals board.
bool AutoplayWaitMemberVital(const AutoplayStep& step) {
    const auto vital = ParseVital(step.Arg(0));
    if (!vital) {
        step.failure = "no such vital";
        return false;
    }
    const VitalsView view = ReadVitalsBoard();
    return (view.Fresh() && std::ranges::any_of(view.board.members,
                                                [&](const MemberVitals& member) {
                                                    return member.member != view.member &&
                                                           member.vital == *vital;
                                                })) ||
           step.WaitedOut(step.Number(1, 60));
}

// travel <warp point> [<timeout>], and ensure-map's travel: the warp, then the player moved to
// the lamp once the map is in, then done once it stands there.
// The travel's start: the warp (as a guest's own lamp trip), and what to wait for.
void BeginAutoplayTravel(const AutoplayStep& step, s32 point) {
    const u64 global = GetGlobalState();
    if (global < 0x10000) {
        step.failure = "no game state";
        return;
    }
    // A guest's own trip (TickGuestLampTrip), at the lamp when it is known.
    const LampPoint* planned = StartTripToLamp(point);
    // As a guest's own lamp trip in a session does; out of one it is the game's own travel,
    // as a player's at a lamp.
    if (IsPartyGuestRole() && InPartySession()) {
        guest_trip_in_flight = true;
    }
    std::scoped_lock lock{autoplay_mutex};
    autoplay_run.travel_map = PackedMapOfWarpPoint(point);
    autoplay_run.travel_planned = planned;
    autoplay_run.travel_fixed_at.reset();
}

// The travel's arrival: the warp alone leaves the player at (0, 0, 0), above the void, so to the
// lamp, as a guest's trip does (guest_trip_fixup_at). The load put the save's last lamp back as
// the respawn point, so the lamp travelled to goes in again first.
void FixAutoplayTravelArrival(const AutoplayStep& step, s32 point) {
    if (const u64 global = GetGlobalState(); global >= 0x10000) {
        WriteValue(global, 0x1528, point);
    }
    const auto at = LocalPlayerPosition();
    const LampPoint* planned = nullptr;
    {
        std::scoped_lock lock{autoplay_mutex};
        planned = autoplay_run.travel_planned;
    }
    LOG_INFO(Debug, "Bloodborne autoplay: travel arrived at ({}, {}, {}) planned at the lamp={}",
             at ? (*at)[0] : 0.0f, at ? (*at)[1] : 0.0f, at ? (*at)[2] : 0.0f, planned != nullptr);
    if (ArrivalNeedsMove(planned, at)) {
        MoveToRespawnPoint(true);
    }
    std::scoped_lock lock{autoplay_mutex};
    autoplay_run.travel_fixed_at = step.elapsed;
}

bool AutoplayTravelTo(const AutoplayStep& step, bool ensure) {
    const s32 point = static_cast<s32>(step.Number(ensure ? 1 : 0, HuntersDreamWarpPoint));
    if (!step.begun) {
        BeginAutoplayTravel(step, point);
        return false;
    }
    // Raw, as all of the rig's checks (see AutoplayInWorld).
    const bool arrived = step.elapsed > 5.0 && AutoplayInWorld() && !PartyTripActive() &&
                         GetCurrentPackedMap() == autoplay_run.travel_map;
    if (arrived && !autoplay_run.travel_fixed_at) {
        FixAutoplayTravelArrival(step, point);
        return false;
    }
    if (arrived && step.elapsed > *autoplay_run.travel_fixed_at + 2.0) {
        const auto at = LocalPlayerPosition();
        if (!at || (*at)[1] < -500.0f) {
            step.failure = "arrived in the void";
            return false;
        }
        return true;
    }
    return step.WaitedOut(step.Number(ensure ? 2 : 1, 180));
}

bool AutoplayTravel(const AutoplayStep& step) {
    return AutoplayTravelTo(step, false);
}

// ensure-map <map> <warp point>: travel there unless already on that map (a test's start).
bool AutoplayEnsureMap(const AutoplayStep& step) {
    if (!step.begun) {
        const auto map = ParseAutoplayTarget(step.Arg(0));
        if (!map) {
            step.failure = "no such map";
            return false;
        }
        // Already there, unless falling through the void (a save made after a bad warp).
        const auto at = LocalPlayerPosition();
        // Raw, as all of the rig's checks (see AutoplayInWorld).
        if (GetCurrentPackedMap() == *map && at && (*at)[1] > -500.0f) {
            return true;
        }
    }
    return AutoplayTravelTo(step, true);
}

// hurt <target> [<hp>], kill <target>.
bool AutoplayHurtOrKill(const AutoplayStep& step) {
    const std::string& verb = step.command.verb;
    const auto target = ParseAutoplayTarget(step.Arg(0));
    const auto found = target ? FindAutoplayCharacter(*target) : std::nullopt;
    if (!found || !HasMemoryAccess(found->data, 0xFC, MemoryProt::CpuWrite)) {
        step.failure = "no such character";
        return false;
    }
    const s32 hp = ReadValue<s32>(found->data, 0xF8);
    const s32 target_hp = verb == "kill" ? 0 : std::min(hp, static_cast<s32>(step.Number(1, 1)));
    // A kill goes through the game's own (writing 0 HP leaves a standing corpse); checked
    // against its prologue first.
    if (verb == "kill" && IsCharacter(found->character, "autoplay kill") &&
        Coop::KillCharacterCallable()) {
        using Kill = void PS4_SYSV_ABI (*)(u64 character);
        reinterpret_cast<Kill>(image_base + KillCharacterOffset)(found->character);
    } else {
        WriteValue(found->data, 0xF8, target_hp);
        // It stands for this player's hits (see own_damage), which the HP board shares.
        std::scoped_lock lock{own_hits_mutex};
        own_damage.Hit(ReadValue<u32>(found->character, 0x08), true, hp - target_hp);
    }
    LOG_INFO(Debug, "Bloodborne autoplay: {} {:#x} hp {} -> {} at {:.1f} m", verb,
             ReadValue<u32>(found->character, 0x08), hp, target_hp, found->distance);
    return true;
}

// mark <target>: the character later commands call "marked" (the nearest one now, say).
bool AutoplayMark(const AutoplayStep& step) {
    const auto target = ParseAutoplayTarget(step.Arg(0));
    const auto found = target ? FindAutoplayCharacter(*target) : std::nullopt;
    if (!found) {
        step.failure = "no such character";
        return false;
    }
    autoplay_marked.store(ReadValue<u32>(found->character, 0x08));
    LOG_INFO(Debug, "Bloodborne autoplay: marked {:#x} hp {} at {:.1f} m", autoplay_marked.load(),
             ReadValue<s32>(found->data, 0xF8), found->distance);
    return true;
}

// approach <target> [<dx> [<dz> [<heading>]]]: stand there, beside it, turned to <heading>.
bool AutoplayApproach(const AutoplayStep& step) {
    const auto target = ParseAutoplayTarget(step.Arg(0));
    const auto found = target ? FindAutoplayCharacter(*target) : std::nullopt;
    const u64 modules = found ? ReadValue<u64>(found->character, 0x3B0) : 0;
    const u64 physics = modules >= 0x10000 ? ReadValue<u64>(modules, 0x68) : 0;
    if (physics < 0x10000) {
        step.failure = "no such character";
        return false;
    }
    TeleportLocalPlayer({ReadValue<float>(physics, 0x1E0) + static_cast<float>(step.Number(1, 1.5)),
                         ReadValue<float>(physics, 0x1E4) + 0.2f,
                         ReadValue<float>(physics, 0x1E8) + static_cast<float>(step.Number(2, 0))},
                        static_cast<float>(step.Number(3, 0)));
    return true;
}

// attack and hit: the player put beside the target for this attack cycle (AttackStanceFor),
// once per cycle, and R1 held in the cycle's swing.
void AutoplayAttackCycle(const AutoplayCharacter& found, double elapsed, bool log_missed) {
    const u32 cycle = AttackCycleAt(elapsed);
    const u64 modules = ReadValue<u64>(found.character, 0x3B0);
    const u64 physics = modules >= 0x10000 ? ReadValue<u64>(modules, 0x68) : 0;
    if (autoplay_run.attack_placed != cycle && physics >= 0x10000) {
        if (log_missed && autoplay_run.attack_placed) {
            LOG_INFO(Debug, "Bloodborne autoplay: attack cycle {} missed (heading {:.2f})",
                     *autoplay_run.attack_placed, LocalPlayerHeading().value_or(0.0f));
        }
        const float target_x = ReadValue<float>(physics, 0x1E0);
        const float target_z = ReadValue<float>(physics, 0x1E8);
        const AttackStance stance = AttackStanceFor(target_x, target_z, cycle);
        TeleportLocalPlayer({stance.x, ReadValue<float>(physics, 0x1E4) + 0.2f, stance.z},
                            HeadingTowards(stance.x, stance.z, target_x, target_z));
        autoplay_run.attack_placed = cycle;
    }
    autoplay_buttons.store(AttackHoldsR1(elapsed) ? AutoplayButton("r1") : 0);
}

// attack <target> [<timeout>]: one HP left, then light attacks from each side in turn until
// it dies (AttackStanceFor): a real hit, so the kill reward and the death run as in play.
bool AutoplayAttack(const AutoplayStep& step) {
    if (!step.begun) {
        const auto target = ParseAutoplayTarget(step.Arg(0));
        const auto found = target ? FindAutoplayCharacter(*target) : std::nullopt;
        if (!found || !HasMemoryAccess(found->data, 0xFC, MemoryProt::CpuWrite)) {
            step.failure = "no such character";
            return false;
        }
        const s32 hp = ReadValue<s32>(found->data, 0xF8);
        WriteValue(found->data, 0xF8, std::min(hp, s32{1}));
        autoplay_run.attack_handle = ReadValue<u32>(found->character, 0x08);
        autoplay_run.attack_placed.reset();
        LOG_INFO(Debug, "Bloodborne autoplay: attacking {:#x} (hp {} -> 1) at {:.1f} m",
                 autoplay_run.attack_handle, hp, found->distance);
    }
    // By its handle from here on: "nearest" would be the next enemy once this one is dead.
    const auto found = FindAutoplayCharacter(autoplay_run.attack_handle);
    const s32 hp = found ? ReadValue<s32>(found->data, 0xF8) : 0;
    if (hp <= 0) {
        autoplay_buttons.store(0);
        LOG_INFO(Debug, "Bloodborne autoplay: {:#x} {} in attack cycle {}",
                 autoplay_run.attack_handle, found ? "died" : "is gone",
                 autoplay_run.attack_placed.value_or(0));
        return true;
    }
    AutoplayAttackCycle(*found, step.elapsed, true);
    if (step.elapsed > step.Number(1, 40)) {
        step.failure = fmt::format("still alive after {:.0f} s: hp {}", step.elapsed, hp);
    }
    return false;
}

// hit <target> [<timeout>]: light attacks from each side in turn (as attack, without taking
// its HP down first) until one lands: a real hit that the game sends to the other players.
bool AutoplayHit(const AutoplayStep& step) {
    if (!step.begun) {
        const auto target = ParseAutoplayTarget(step.Arg(0));
        const auto found = target ? FindAutoplayCharacter(*target) : std::nullopt;
        if (!found) {
            step.failure = "no such character";
            return false;
        }
        autoplay_run.attack_handle = ReadValue<u32>(found->character, 0x08);
        autoplay_run.attack_placed.reset();
        autoplay_run.hit_from_hp = ReadValue<s32>(found->data, 0xF8);
        LOG_INFO(Debug, "Bloodborne autoplay: hitting {:#x} (hp {}) at {:.1f} m",
                 autoplay_run.attack_handle, autoplay_run.hit_from_hp, found->distance);
    }
    const auto found = FindAutoplayCharacter(autoplay_run.attack_handle);
    if (!found) {
        autoplay_buttons.store(0);
        step.failure = "the target is gone";
        return false;
    }
    const s32 hp = ReadValue<s32>(found->data, 0xF8);
    if (hp < autoplay_run.hit_from_hp) {
        autoplay_buttons.store(0);
        LOG_INFO(Debug, "Bloodborne autoplay: hit {:#x}: hp {} -> {} in attack cycle {}",
                 autoplay_run.attack_handle, autoplay_run.hit_from_hp, hp,
                 autoplay_run.attack_placed.value_or(0));
        return true;
    }
    AutoplayAttackCycle(*found, step.elapsed, false);
    if (step.elapsed > step.Number(1, 30)) {
        autoplay_buttons.store(0);
        step.failure = fmt::format("no hit landed in {:.0f} s: hp {}", step.elapsed, hp);
    }
    return false;
}

bool AutoplayTeleport(const AutoplayStep& step) {
    TeleportLocalPlayer({static_cast<float>(step.Number(0, 0)),
                         static_cast<float>(step.Number(1, 0)),
                         static_cast<float>(step.Number(2, 0))},
                        static_cast<float>(step.Number(3, 0)));
    return true;
}

// place <target> <x> <y> <z> [<heading>]: another character put there as the player's teleport
// is (the host's Doll moved away from where a guest's copy of her stands, playtest 2026-10-18).
bool AutoplayPlace(const AutoplayStep& step) {
    const auto target = ParseAutoplayTarget(step.Arg(0));
    const auto found = target ? FindAutoplayCharacter(*target) : std::nullopt;
    if (!found || found->character == GetLocalPlayer()) {
        step.failure = "no such character";
        return false;
    }
    if (!PlaceCharacterAt(
            found->character,
            {static_cast<float>(step.Number(1, 0)), static_cast<float>(step.Number(2, 0)),
             static_cast<float>(step.Number(3, 0)), 1.0f},
            {0.0f, static_cast<float>(step.Number(4, 0)), 0.0f, 0.0f}, "AutoplayPlace")) {
        step.failure = "no teleport";
        return false;
    }
    LOG_INFO(Debug, "Bloodborne autoplay: placed {:#x} at ({:.1f}, {:.1f}, {:.1f})",
             ReadValue<u32>(found->character, 0x08), step.Number(1, 0), step.Number(2, 0),
             step.Number(3, 0));
    return true;
}

// expect-walks <metres> [<timeout>]: the left stick held forward until the player stands that far
// from where it began (a player kept in a talk, kneeling at the Doll for good in
// playtest 2026-10-18, never walks).
bool AutoplayExpectWalks(const AutoplayStep& step) {
    const auto at = LocalPlayerPosition();
    if (!step.begun) {
        if (!at) {
            step.failure = "no player";
            return false;
        }
        std::scoped_lock lock{autoplay_mutex};
        autoplay_run.glide_from = *at;
    }
    LampPosition from{};
    {
        std::scoped_lock lock{autoplay_mutex};
        from = autoplay_run.glide_from;
    }
    const float dx = at ? (*at)[0] - from[0] : 0.0f;
    const float dz = at ? (*at)[2] - from[2] : 0.0f;
    const float walked = std::sqrt(dx * dx + dz * dz);
    if (walked >= static_cast<float>(step.Number(0, 2))) {
        autoplay_sticks.store(0x80808080);
        LOG_INFO(Debug, "Bloodborne autoplay: walked {:.1f} m in {:.1f} s", walked, step.elapsed);
        return true;
    }
    if (step.elapsed > step.Number(1, 10)) {
        autoplay_sticks.store(0x80808080);
        step.failure = fmt::format("walked {:.1f} m in {:.0f} s", walked, step.elapsed);
        return false;
    }
    autoplay_sticks.store(0x80800080); // lx 0x80, ly 0x00: straight ahead
    return false;
}

// glide <x y z>...: from where the player is along those points at walking pace, by small
// teleports facing the way it goes (a far teleport drops it through the floor): a path a "map
// change" log line gave (its trail), to cross a border between two maps.
bool AutoplayGlide(const AutoplayStep& step) {
    const auto& args = step.command.args;
    if (args.empty() || args.size() % 3 != 0) {
        step.failure = "glide takes x y z points";
        return false;
    }
    if (!step.begun) {
        const auto at = LocalPlayerPosition();
        if (!at) {
            step.failure = "no player";
            return false;
        }
        std::scoped_lock lock{autoplay_mutex};
        autoplay_run.glide_from = *at;
    }
    std::vector<LampPosition> path;
    {
        std::scoped_lock lock{autoplay_mutex};
        path.push_back(autoplay_run.glide_from);
    }
    for (size_t at = 0; at < args.size(); at += 3) {
        path.push_back({static_cast<float>(step.Number(at, 0)),
                        static_cast<float>(step.Number(at + 1, 0)),
                        static_cast<float>(step.Number(at + 2, 0))});
    }
    const auto point = GlidePoint(path, step.elapsed);
    const LampPosition to = point.value_or(path.back());
    const LampPosition ahead = GlidePoint(path, step.elapsed + 0.25).value_or(path.back());
    const float dx = ahead[0] - to[0];
    const float dz = ahead[2] - to[2];
    // A heading h faces (-sin h, cos h).
    TeleportLocalPlayer(to, dx == 0.0f && dz == 0.0f ? 0.0f : std::atan2(-dx, dz));
    return !point.has_value();
}

// pickup [<meters>] [<timeout>]: onto the nearest usable map item within reach (30 m), then
// X for 0.3 s of every 1.5 s until the item leaves the usable list: a real pickup through the
// item button (the host could not pick up items in the Forbidden Woods, 2026-10-09). Only an
// item on the player's own floor: one 21 m below the first lamp lies in another map's block,
// whose ground is not loaded, and the player teleported there fell into it (rig 2026-10-25).
constexpr float PickupFloorMetres = 8.0f;
bool AutoplayPickup(const AutoplayStep& step) {
    const auto position = MapItemSearchPlayerPosition();
    const auto items =
        position ? UsableMapItemsNear(*position) : std::vector<std::pair<float, MapItemSpot>>{};
    if (!step.begun) {
        const auto on_floor = std::ranges::find_if(items, [&](const auto& item) {
            return std::abs(item.second.at[1] - (*position)[1]) <= PickupFloorMetres;
        });
        if (on_floor == items.end() || on_floor->first > step.Number(0, 30)) {
            step.failure = "no usable map item near";
            return false;
        }
        const MapItemSpot& spot = on_floor->second;
        autoplay_run.pickup_handle = spot.handle;
        autoplay_run.pickup_at = {spot.at[0], spot.at[1] + 0.2f, spot.at[2]};
        TeleportLocalPlayer(autoplay_run.pickup_at);
        LOG_INFO(Debug, "Bloodborne autoplay: picking up map item {:#x} {:.1f} m away", spot.handle,
                 on_floor->first);
    }
    const bool there = std::ranges::any_of(
        items, [](const auto& item) { return item.second.handle == autoplay_run.pickup_handle; });
    if (!there) {
        autoplay_buttons.store(0);
        LOG_INFO(Debug, "Bloodborne autoplay: map item {:#x} picked up after {:.1f} s",
                 autoplay_run.pickup_handle, step.elapsed);
        return true;
    }
    // The far item's ground may not be loaded yet, or a body by it pushes the player away: a
    // player off the item goes back on it.
    if (step.begun && position && PickupPlayerOffTheItem(*position, autoplay_run.pickup_at)) {
        TeleportLocalPlayer(autoplay_run.pickup_at);
        LOG_INFO(Debug,
                 "Bloodborne autoplay: back on map item {:#x} from ({:.1f}, {:.1f}, {:.1f}) after "
                 "{:.1f} s",
                 autoplay_run.pickup_handle, (*position)[0], (*position)[1], (*position)[2],
                 step.elapsed);
    }
    autoplay_buttons.store(
        step.elapsed > 1.0 && std::fmod(step.elapsed, 1.5) < 0.3 ? AutoplayButton("cross") : 0);
    if (step.elapsed > step.Number(1, 20)) {
        autoplay_buttons.store(0);
        step.failure = fmt::format(
            "map item {:#x} still there after {:.0f} s (item button found {} not busy {} "
            "pressed {})",
            autoplay_run.pickup_handle, step.elapsed, item_button_counts[1].load(),
            item_button_counts[2].load(), item_button_counts[3].load());
    }
    return false;
}

// rise <metres>: straight up from where the player stands, to fall back down.
bool AutoplayRise(const AutoplayStep& step) {
    const auto at = LocalPlayerPosition();
    if (!at) {
        step.failure = "not in the world";
        return false;
    }
    TeleportLocalPlayer({(*at)[0], (*at)[1] + static_cast<float>(step.Number(0, 8)), (*at)[2]},
                        LocalPlayerHeading().value_or(0.0f));
    return true;
}

// approach-downed: a metre from the nearest downed party member on this map, where the
// vitals board has it.
bool AutoplayApproachDowned(const AutoplayStep& step) {
    const VitalsView view = ReadVitalsBoard();
    const auto me = LocalPlayerPosition();
    const auto distance = [&](const LampPosition& at) {
        return std::hypot(at[0] - (*me)[0], at[1] - (*me)[1], at[2] - (*me)[2]);
    };
    std::optional<LampPosition> nearest;
    for (const MemberVitals& member : view.board.members) {
        if (me && view.Fresh() && member.member != view.member && member.vital == Vital::Down &&
            member.map == PlayingMap() && (!nearest || distance(member.at) < distance(*nearest))) {
            nearest = member.at;
        }
    }
    if (!nearest) {
        step.failure = "nobody down on this map";
        return false;
    }
    TeleportLocalPlayer({(*nearest)[0] + 1.0f, (*nearest)[1] + 0.2f, (*nearest)[2]});
    return true;
}

// revive-downed [<timeout>] [r1|r2]: strikes the nearest downed party member on this map until
// nobody is down (how many hits depends on the weapon): from each side in turn, turned to it, as
// `attack` does (AttackStanceFor), since only a swing that lands counts for its revive. With r2
// the swings are heavy ones (Kaniel 2026-10-12: R2 revived no faster than R1).
bool AutoplayReviveDowned(const AutoplayStep& step) {
    const std::string_view swing = step.Arg(1).empty() ? "r1" : step.Arg(1);
    if (swing != "r1" && swing != "r2") {
        step.failure = "revive-downed swings r1 or r2";
        return false;
    }
    if (!step.begun) {
        autoplay_run.attack_placed.reset();
    }
    const VitalsView view = ReadVitalsBoard();
    const auto me = LocalPlayerPosition();
    std::optional<LampPosition> downed;
    for (const MemberVitals& member : view.board.members) {
        if (view.Fresh() && member.member != view.member && member.vital == Vital::Down &&
            member.map == PlayingMap()) {
            downed = member.at;
        }
    }
    if (!downed || !me) {
        autoplay_buttons.store(0);
        if (!step.begun) {
            step.failure = "nobody down on this map";
            return false;
        }
        LOG_INFO(Debug, "Bloodborne autoplay: nobody down after {:.1f} s of revive hits",
                 step.elapsed);
        return true;
    }
    // The body itself (the other player nearest to where the board has it), not the board's
    // position, which lags: swings aimed at it missed (rig 2026-10-12, one hit in 7 s). A body
    // with the downed team (DownedTeams::Hostile) comes first: in a party of three the nearest
    // was the host standing by, and the swings circled it (rig down-revive3 2026-10-14).
    const LampPosition posted = *downed;
    float nearest = 4.0f;
    bool nearest_downed = false;
    for (const OtherPlayer& other : OtherPlayers()) {
        if (!other.at ||
            !HasMemoryAccess(other.character, CharacterTeamOffset + 4, MemoryProt::CpuRead)) {
            continue;
        }
        const bool is_downed =
            ReadValue<s32>(other.character, CharacterTeamOffset) == DownedTeams::Hostile;
        const float d = std::hypot((*other.at)[0] - posted[0], (*other.at)[2] - posted[2]);
        if (d < 4.0f &&
            ((is_downed && !nearest_downed) || (is_downed == nearest_downed && d < nearest))) {
            nearest = d;
            nearest_downed = is_downed;
            downed = *other.at;
        }
    }
    const u32 cycle = AttackCycleAt(step.elapsed);
    if (autoplay_run.attack_placed != cycle) {
        const AttackStance stance = AttackStanceFor((*downed)[0], (*downed)[2], cycle);
        TeleportLocalPlayer({stance.x, (*downed)[1] + 0.2f, stance.z},
                            HeadingTowards(stance.x, stance.z, (*downed)[0], (*downed)[2]));
        autoplay_run.attack_placed = cycle;
    }
    autoplay_buttons.store(AttackHoldsR1(step.elapsed) ? AutoplayButton(swing) : 0);
    if (step.elapsed > step.Number(0, 60)) {
        autoplay_buttons.store(0);
        step.failure = fmt::format("still down after {:.0f} s of revive hits", step.elapsed);
    }
    return false;
}

// press <button> [<ms>], stick/look <x> <y> [<ms>]: left/right stick held (150 ms).
bool AutoplayPressOrStick(const AutoplayStep& step) {
    const bool press = step.command.verb == "press";
    u32 buttons = 0;
    u32 sticks = 0x80808080;
    if (press) {
        buttons = AutoplayButton(step.Arg(0));
        if (buttons == 0) {
            step.failure = "no such button";
            return false;
        }
    } else {
        const u32 x = static_cast<u32>(std::clamp(step.Number(0, 128), 0.0, 255.0));
        const u32 y = static_cast<u32>(std::clamp(step.Number(1, 128), 0.0, 255.0));
        sticks = step.command.verb=="look"?0x8080 | (x<<16) | (y<<24):x | (y << 8) | 0x80800000;
    }
    autoplay_buttons.store(buttons);
    autoplay_sticks.store(sticks);
    std::scoped_lock lock{autoplay_mutex};
    autoplay_run.hold_until =
        std::chrono::steady_clock::now() +
        std::chrono::milliseconds{static_cast<s64>(step.Number(press ? 1 : 2, 150))};
    return false;
}

bool AutoplaySetFlag(const AutoplayStep& step) {
    using SetFlag = void PS4_SYSV_ABI (*)(u64, s32, bool);
    reinterpret_cast<SetFlag>(image_base + PartyGhostSetEventFlagOffset)(
        0, static_cast<s32>(step.Number(0, 0)), step.Number(1, 1) != 0);
    return true;
}

// return-point <point>: the lamp a death respawns this player at (GlobalState +0x1528, what a
// lamp rest stores): members with other lamps without the trips there (a wipe's respawn, Kaniel
// 2026-10-13: everyone wakes where the host does).
bool AutoplayReturnPoint(const AutoplayStep& step) {
    const u64 global = GetGlobalState();
    if (global < 0x10000 || !HasMemoryAccess(global + 0x1528, 4, MemoryProt::CpuWrite)) {
        step.failure = "no global state";
        return false;
    }
    WriteValue(global, 0x1528, static_cast<s32>(step.Number(0, 0)));
    LOG_INFO(Debug, "Bloodborne autoplay: return point {}", ReadValue<s32>(global, 0x1528));
    return true;
}

// expect-flag <flag> [<0|1>], wait-flag <flag> [<0|1>] [<timeout>].
bool AutoplayExpectOrWaitFlag(const AutoplayStep& step) {
    const bool on = EventFlagOn(static_cast<s32>(step.Number(0, 0)));
    if (on == (step.Number(1, 1) != 0)) {
        return true;
    }
    if (step.command.verb == "expect-flag") {
        step.failure = fmt::format("flag is {}", on ? 1 : 0);
        return false;
    }
    return step.WaitedOut(step.Number(2, 180));
}

// flags <flag>...: each flag's state, logged (which world has what, compared in the logs).
bool AutoplayLogFlags(const AutoplayStep& step) {
    std::string states;
    for (size_t index = 0; !step.Arg(index).empty(); ++index) {
        const s32 flag = static_cast<s32>(step.Number(index, 0));
        states += fmt::format(" {}={}", flag, EventFlagOn(flag) ? 1 : 0);
    }
    LOG_INFO(Debug, "Bloodborne autoplay flags (map {:#x}):{}", GetCurrentPackedMap(), states);
    return true;
}

// altar-record <altar 0-6 | view>: a ritual altar's record (94k00000-94k05999, or the current
// altar's view 92900000-92905999), its 750 bytes logged as hex (team altars: the rig compares
// every game's).
bool AutoplayAltarRecord(const AutoplayStep& step) {
    const bool view = step.Arg(0) == "view";
    const s32 altar = view ? -1 : static_cast<s32>(step.Number(0, -1));
    if (!view && (altar < 0 || altar >= 7)) {
        step.failure = "altar-record takes an altar 0-6 or view";
        return false;
    }
    const s32 base = view ? 92900000 : 94000000 + altar * 100000;
    std::string hex;
    for (s32 byte = 0; byte < 750; ++byte) {
        const auto bits = EventFlagBits(base + byte * 8, 8);
        if (!bits) {
            step.failure = "the event flags are not readable";
            return false;
        }
        hex += fmt::format("{:02x}", *bits & 0xFF);
    }
    LOG_INFO(Debug, "Bloodborne autoplay altar-record {} (map {:#x}): {}",
             view ? "view" : std::string{step.Arg(0)}, GetCurrentPackedMap(), hex);
    return true;
}

// flag-list <first> <last>: every event flag on in the range, logged in full (the session's own
// log of flags is cut at 48).
bool AutoplayFlagList(const AutoplayStep& step) {
    const s32 first = static_cast<s32>(step.Number(0, 0));
    const s32 last = static_cast<s32>(step.Number(1, first));
    std::string list;
    size_t count = 0;
    for (s32 flag = first; flag <= last; ++flag) {
        if (EventFlagOn(flag)) {
            list += fmt::format(" {}", flag);
            ++count;
        }
    }
    LOG_INFO(Debug, "Bloodborne autoplay flag-list {}-{} (map {:#x}): {} on:{}", first, last,
             GetCurrentPackedMap(), count, list);
    return true;
}

// altar-state <altar 0-6>: the ritual altar object's status (+0x25C of its state).
bool AutoplayAltarState(const AutoplayStep& step) {
    const s32 altar = static_cast<s32>(step.Number(0, -1));
    const auto status = RitualAltarStatus(altar);
    LOG_INFO(Debug, "Bloodborne autoplay altar-state {} (map {:#x}): {}", altar,
             GetCurrentPackedMap(), status ? fmt::format("status {}", *status) : "no object");
    return true;
}

// godmode [off]: the player's HP back to its max on every party tick (tests that stand
// around near enemies), until switched off.
bool AutoplayGodmode(const AutoplayStep& step) {
    autoplay_godmode.store(step.Arg(0) != "off");
    return true;
}

bool AutoplaySnapshot(const AutoplayStep&) {
    LogAutoplaySnapshot();
    return true;
}

// chapter <name>: a rig suite's next section; the rig judges each one on its own lines.
bool AutoplayChapter(const AutoplayStep& step) {
    std::scoped_lock lock{autoplay_mutex};
    autoplay_run.chapter = std::string{step.Arg(0)};
    autoplay_run.chapter_deadline.reset();
    if (const auto budget = AutoplayChapterBudget(step.command)) {
        autoplay_run.chapter_budget = *budget;
        autoplay_run.chapter_deadline =
            std::chrono::steady_clock::now() +
            std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                std::chrono::duration<double>(*budget));
    }
    LOG_INFO(Debug, "Bloodborne autoplay: chapter {}", step.Arg(0));
    return true;
}

// keep-going <on|off>: on, a failed step gives up its chapter and the script goes on at the next
// one (NextAutoplayChapter); off (a chapter the rest needs, as the join), it ends there.
bool AutoplayKeepGoing(const AutoplayStep& step) {
    std::scoped_lock lock{autoplay_mutex};
    autoplay_run.keep_going = step.Arg(0) == "on";
    return true;
}

// sync <name> [<timeout>]: wait until every game of the suite got here: the rig sees each one's
// "reached" line and writes the go file (AutoplaySyncPath) for all of them at once.
bool AutoplaySync(const AutoplayStep& step) {
    std::string script;
    {
        std::scoped_lock lock{autoplay_mutex};
        script = autoplay_run.script_path;
    }
    const auto path = AutoplaySyncPath(script, step.Arg(0));
    if (!path) {
        step.failure = "no such sync name";
        return false;
    }
    if (!step.begun) {
        LOG_INFO(Debug, "Bloodborne autoplay: sync {} reached (t={:.1f})", step.Arg(0),
                 SteadyMilliseconds() / 1000.0);
    }
    std::error_code error;
    return std::filesystem::exists(*path, error) || step.WaitedOut(step.Number(1, 600));
}

bool AutoplayCensus(const AutoplayStep& step) {
    LogAutoplayCensus(static_cast<float>(step.Number(0, 60)));
    return true;
}

// shot <name> [<timeout>]: a picture of the port's next frame (AutoplayShotPath), once its
// presenter took the request. The emulator has no request file: skipped. A picture not taken
// in time (10 s) is noted and the script goes on: it shows the HUD, it tests nothing.
bool AutoplayShot(const AutoplayStep& step) {
    static const char* request = std::getenv("BB_SHOT_TRIGGER");
    if (request == nullptr) {
        LOG_INFO(Debug, "Bloodborne autoplay: shot {} skipped (nothing takes pictures)",
                 step.Arg(0));
        return true;
    }
    if (!step.begun) {
        const auto path = AutoplayShotPath(request, step.Arg(0));
        if (!path) {
            step.failure = "no such shot name";
            return false;
        }
        std::error_code error;
        std::filesystem::create_directories(std::filesystem::path{*path}.parent_path(), error);
        // Written whole, then moved into place: the presenter never reads half a path.
        const std::string partial = std::string{request} + ".part";
        std::ofstream{partial, std::ios::binary | std::ios::trunc} << *path;
        std::filesystem::rename(partial, request, error);
        if (error) {
            step.failure = fmt::format("no shot request: {}", error.message());
            return false;
        }
        LOG_INFO(Debug, "Bloodborne autoplay: shot {} asked for", *path);
    }
    std::error_code error;
    if (!std::filesystem::exists(request, error)) {
        return true;
    }
    if (step.elapsed > step.Number(1, 10)) {
        LOG_WARNING(Debug, "Bloodborne autoplay: shot {} not taken in {:.0f} s", step.Arg(0),
                    step.elapsed);
        return true;
    }
    return false;
}

// keys <token>... (title-keys on the title screen): the port's menu keys (AutoplayMenuKeys), for
// its overlay to press one a frame (BB_MENU_KEYS_FILE). The emulator has no such file: skipped.
// Written whole, then moved into place: the overlay never reads half of it.
bool WriteAutoplayMenuKeys(const std::vector<std::string>& tokens, std::string& failure) {
    const auto text = AutoplayMenuKeys(tokens);
    if (!text) {
        failure = "no such menu key";
        return false;
    }
    static const char* path = std::getenv("BB_MENU_KEYS_FILE");
    if (path == nullptr || *path == '\0') {
        LOG_INFO(Debug, "Bloodborne autoplay: menu keys skipped (no overlay reads them)");
        return true;
    }
    const std::string partial = std::string{path} + ".part";
    std::ofstream{partial, std::ios::binary | std::ios::trunc} << *text;
    std::error_code error;
    std::filesystem::rename(partial, path, error);
    if (error) {
        failure = fmt::format("no menu keys file: {}", error.message());
        return false;
    }
    LOG_INFO(Debug, "Bloodborne autoplay: menu keys {}", text->substr(0, text->size() - 1));
    return true;
}

bool AutoplayKeys(const AutoplayStep& step) {
    return WriteAutoplayMenuKeys(step.command.args, step.failure);
}

// cut-link <seconds>: this game's connection drops for that long (phase 7: a Wi-Fi blip), then
// comes back on the same socket; the session should ride it out.
bool AutoplayCutLink(const AutoplayStep& step) {
    const double seconds = step.Number(0, 3.0);
    Libraries::Net::CutServerLinkForTest(static_cast<u32>(seconds * 1000.0));
    LOG_INFO(Debug, "Bloodborne autoplay: connection cut for {:.0f} s", seconds);
    return true;
}

// quit-title (Kaniel 2026-10-25): the System menu's Exit Game answered Yes, as a player quits to
// the title: the script's next steps are the title's (team-*, title-*), then the world's again
// after the next load (TickAutoplay moves the script to the title phase).
bool AutoplayQuitTitle(const AutoplayStep& step) {
    // lea rax, [rip + ...]: the function's first instruction in 1.09.
    constexpr std::array<u8, 3> First{0x48, 0x8D, 0x05};
    const u64 at = image_base + QuitGameConfirmedOffset;
    if (!HasMemoryAccess(at, First.size(), MemoryProt::CpuRead) ||
        std::memcmp(reinterpret_cast<const void*>(at), First.data(), First.size()) != 0) {
        step.failure = "the game's quit is not where 1.09 has it";
        return false;
    }
    reinterpret_cast<void PS4_SYSV_ABI (*)(u64)>(at)(0);
    LOG_INFO(Debug, "Bloodborne autoplay: quit to the title (the System menu's Exit Game)");
    return true;
}

// cutscene-options <seconds>: Options pressed (200 ms) once the next cutscene has played that long.
// The game's tick, and so the script, stands still under a cutscene: the press waits in the pad's
// vote (CutsceneVotePad), which still runs.
bool AutoplayCutsceneOptions(const AutoplayStep& step) {
    Coop::ArmCutsceneOptions(static_cast<s64>(step.Number(0, 5) * 1000.0));
    return true;
}

bool AutoplayLog(const AutoplayStep& step) {
    std::string text;
    for (const std::string& word : step.command.args) {
        text += (text.empty() ? "" : " ") + word;
    }
    LOG_INFO(Debug, "Bloodborne autoplay log: {}", text);
    return true;
}

// wait-hp <handle|self> <op> <value> [<timeout>]: expect-hp that waits until it holds (rig
// 2026-10-22, apart-hurt: 25 s beside an enemy sometimes downed the guest, alone, for the rest of
// the suite; it leaves at the first hit that lands).
bool AutoplayWaitHp(const AutoplayStep& step) {
    const auto target = ParseAutoplayTarget(step.Arg(0));
    const auto found = target ? FindAutoplayCharacter(*target) : std::nullopt;
    const s32 hp = found ? ReadValue<s32>(found->data, 0xF8) : 0;
    const auto holds = found ? CompareAutoplay(hp, step.Arg(1), static_cast<s64>(step.Number(2, 0)))
                             : std::nullopt;
    if (holds && *holds) {
        return true;
    }
    if (step.elapsed > step.Number(3, 30)) {
        step.failure = found ? fmt::format("hp is {} after {:.0f} s", hp, step.elapsed)
                             : std::string{"no such character"};
    }
    return false;
}

bool AutoplayExpectHp(const AutoplayStep& step) {
    const auto target = ParseAutoplayTarget(step.Arg(0));
    const auto found = target ? FindAutoplayCharacter(*target) : std::nullopt;
    if (!found) {
        step.failure = "no such character";
        return false;
    }
    const s32 hp = ReadValue<s32>(found->data, 0xF8);
    const auto holds = CompareAutoplay(hp, step.Arg(1), static_cast<s64>(step.Number(2, 0)));
    if (!holds || !*holds) {
        step.failure = fmt::format("hp is {}", hp);
        return false;
    }
    return true;
}

// dump <target>: a character's first 0x800 bytes, its control block (+0x18) and its module
// pointers (+0x3B0) with each module's first 0x100 bytes, logged in hex, to diff one character
// between the host's and a guest's world (Iosefka shown on a guest with the event hide byte
// set, 2026-10-10).
bool AutoplayDump(const AutoplayStep& step) {
    const auto target = ParseAutoplayTarget(step.Arg(0));
    const auto found = target ? FindAutoplayCharacter(*target) : std::nullopt;
    if (!found || !HasMemoryAccess(found->character, 0x400, MemoryProt::CpuRead)) {
        step.failure = "no such character";
        return false;
    }
    const u32 handle = ReadValue<u32>(found->character, 0x08);
    const auto dump = [&](const char* what, u64 base, u64 size) {
        if (base < 0x10000 || !HasMemoryAccess(base, size, MemoryProt::CpuRead)) {
            return;
        }
        for (u64 offset = 0; offset < size; offset += 0x20) {
            std::string row;
            for (u64 byte = 0; byte < 0x20; ++byte) {
                row += fmt::format("{:02x}", ReadValue<u8>(base, offset + byte));
            }
            LOG_INFO(Debug, "Bloodborne autoplay dump {:#x} {} +{:03x}: {}", handle, what, offset,
                     row);
        }
    };
    dump("chr", found->character, 0x800);
    dump("ctrl", ReadValue<u64>(found->character, 0x18), 0x100);
    // The model (draw flags at +0x114): how a phantom is drawn.
    dump("model", ReadValue<u64>(found->character, 0x58), 0x400);
    const u64 modules = ReadValue<u64>(found->character, 0x3B0);
    dump("modules", modules, 0x100);
    // Each module's first bytes: what draws the character is in one of them.
    for (u64 index = 0; modules >= 0x10000 && index < 0x20; ++index) {
        dump(fmt::format("module{:02x}", index).c_str(), ReadValue<u64>(modules, index * 8), 0x100);
    }
    return true;
}

// raise-stat <index> <amount>: one of the player's stats (PlayerGameData +0x40 + 8 * index,
// what channel 8 sends) raised, as a level up does (see OwnDataWatch).
bool AutoplayRaiseStat(const AutoplayStep& step) {
    const u64 mine = OwnGameData();
    const u64 at = 0x40 + 8 * static_cast<u64>(step.Number(0, 0));
    if (mine < 0x10000 || at > 0x90 || !HasMemoryAccess(mine + at, 4, MemoryProt::CpuWrite)) {
        step.failure = "no player data";
        return false;
    }
    const s32 before = ReadValue<s32>(mine, at);
    WriteValue(mine, at, static_cast<s32>(before + step.Number(1, 1)));
    LOG_INFO(Debug, "Bloodborne autoplay raise-stat: +{:#x} {} -> {}", at, before,
             ReadValue<s32>(mine, at));
    return true;
}

// Real inventory evidence, not an injected result: compare a finished invasion with its
// starting wallet. PlayerGameData offsets follow Kaniel's bbport_save_editor.h, 1.09.
bool AutoplayNoteWallet(const AutoplayStep& step) {
    const u64 data=OwnGameData();
    if(data<0x10000 || !HasMemoryAccess(data+0x80,0x18,MemoryProt::CpuRead)) {step.failure="no wallet";return false;}
    // +0x80 is the base value; the HUD's current Insight is +0x84.
    autoplay_run.noted_wallet=std::pair{ReadValue<s32>(data,0x94),ReadValue<s32>(data,0x84)};
    LOG_INFO(Debug,"Bloodborne autoplay wallet: echoes {} insight {} level {}",autoplay_run.noted_wallet->first,autoplay_run.noted_wallet->second,ReadValue<s32>(data,0x90));
    return true;
}
bool AutoplayExpectWallet(const AutoplayStep& step) {
    const u64 data=OwnGameData();
    if(!autoplay_run.noted_wallet || data<0x10000 || !HasMemoryAccess(data+0x80,0x18,MemoryProt::CpuRead)) {step.failure="no noted wallet";return false;}
    const auto [echoes,insight]=*autoplay_run.noted_wallet;
    const s32 delta_echoes=ReadValue<s32>(data,0x94)-echoes,delta_insight=ReadValue<s32>(data,0x84)-insight;
    if((step.Arg(0)!="any" && delta_echoes!=step.Number(0,0)) || delta_insight!=step.Number(1,0)) {step.failure=fmt::format("wallet delta echoes {} insight {}",delta_echoes,delta_insight);return false;}
    LOG_INFO(Debug,"Bloodborne autoplay wallet verified: echoes {:+} insight {:+}",delta_echoes,delta_insight);
    return true;
}

bool AutoplayExpectDregs(const AutoplayStep& step) {
    const auto counts=InvasionDregs();
    if(!counts) {step.failure="no native Dreg inventories";return false;}
    LOG_INFO(Debug,"Bloodborne autoplay Blood Dregs: held {} stored {}",counts->first,counts->second);
    if(counts->first!=step.Number(0,0) || counts->second!=step.Number(1,0)) {
        step.failure=fmt::format("Blood Dregs held {} stored {}",counts->first,counts->second);return false;
    }
    return true;
}

// add-effect <SpEffect>: the effect put on the player through its own add-effect virtual, as the
// game's events do (4680: a point of Insight, common event 9350's; the rig's characters have few).
bool AutoplayAddEffect(const AutoplayStep& step) {
    const u64 player = GetLocalPlayer();
    if (!HasVirtual(player, CharacterAddSpEffectSlot, AddSpEffectOffset, "autoplay AddSpEffect")) {
        step.failure = "no player";
        return false;
    }
    const s32 effect = static_cast<s32>(step.Number(0, 0));
    reinterpret_cast<AddSpEffectFn>(image_base + AddSpEffectOffset)(
        player, effect, player, 0, 1, 0, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 0);
    LOG_INFO(Debug, "Bloodborne autoplay add-effect: {}", effect);
    return true;
}

// The map object of entity `entity`, as event command 2005:3 (bank 2005 case 3, 0x017C480C)
// finds it in 0x013CA580: the registry at [0x0553B168] (+0x10 map count, +0x18 maps of 0x20, each
// +0x10 block count and +0x18 blocks of 0x1F8 with a sorted (entity, code) list at +0xF0 count /
// +0xF8), then the code's block ((code >> 20) & 0xFF of [+0x20], at [+0x28] + block * 0x1F8) and
// item (code & 0xFFFFF of [block +0xC8], at [block +0xD0] + item * 0x2E0).
std::optional<u64> MapObjectOfEntity(s32 entity) {
    const u64 registry = ReadValue<u64>(image_base + EntityObjectRegistryOffset, 0);
    if (registry < 0x10000 || !HasMemoryAccess(registry, 0x30, MemoryProt::CpuRead)) {
        return std::nullopt;
    }
    std::optional<s32> code;
    const s32 maps = ReadValue<s32>(registry, 0x10);
    const u64 map_array = ReadValue<u64>(registry, 0x18);
    for (s32 map = 0; map < std::min(maps, 64) && map_array >= 0x10000 && !code; ++map) {
        const u64 entry = map_array + static_cast<u64>(map) * 0x20;
        const s32 blocks = ReadValue<s32>(entry, 0x10);
        const u64 block_array = ReadValue<u64>(entry, 0x18);
        for (s32 block = 0; block < std::min(blocks, 64) && block_array >= 0x10000 && !code;
             ++block) {
            const u64 at = block_array + static_cast<u64>(block) * 0x1F8;
            const s32 count = ReadValue<s32>(at, 0xF0);
            const u64 pairs = ReadValue<u64>(at, 0xF8);
            if (pairs < 0x10000 || count <= 0 ||
                !HasMemoryAccess(pairs, static_cast<u64>(count) * 8, MemoryProt::CpuRead)) {
                continue;
            }
            for (s32 low = 0, high = count - 1; low <= high;) {
                const s32 middle = (low + high) / 2;
                const s32 id = ReadValue<s32>(pairs, static_cast<u64>(middle) * 8);
                if (id == entity) {
                    const s32 found = ReadValue<s32>(pairs, static_cast<u64>(middle) * 8 + 4);
                    if (found >= 0) {
                        code = found;
                    }
                    break;
                }
                if (id < entity) {
                    low = middle + 1;
                } else {
                    high = middle - 1;
                }
            }
        }
    }
    if (!code) {
        return std::nullopt;
    }
    const u32 block = (static_cast<u32>(*code) >> 20) & 0xFF;
    const u32 item = static_cast<u32>(*code) & 0xFFFFF;
    if (block >= ReadValue<u32>(registry, 0x20)) {
        return std::nullopt;
    }
    const u64 block_at = ReadValue<u64>(registry, 0x28) + block * 0x1F8;
    if (block_at < 0x10000 || item >= ReadValue<u32>(block_at, 0xC8)) {
        return std::nullopt;
    }
    const u64 object = ReadValue<u64>(block_at, 0xD0) + item * 0x2E0;
    return HasMemoryAccess(object, 0x200, MemoryProt::CpuRead) ? std::optional{object}
                                                               : std::nullopt;
}

// object <entity>...: each map object's enable switch (bit 14 of +0x1FC, what 2005:3 writes in
// 0x015A6CA0) and that word, logged (Kaniel's playtest 2026-10-20: a guest saw the Witch's Abode
// lamp, object 2201951, before her kill).
bool AutoplayObject(const AutoplayStep& step) {
    for (size_t index = 0; !step.Arg(index).empty(); ++index) {
        const s32 entity = static_cast<s32>(step.Number(index, 0));
        if (const auto object = MapObjectOfEntity(entity)) {
            const u32 flags = ReadValue<u32>(*object, 0x1FC);
            LOG_INFO(Debug,
                     "Bloodborne autoplay object {}: enabled {} flags {:#x} at {:#x} (map {:#x})",
                     entity, (flags >> 14) & 1, flags, *object, GetCurrentPackedMap());
        } else {
            LOG_INFO(Debug, "Bloodborne autoplay object {}: none", entity);
        }
    }
    return true;
}

// peek <target> <offset>, poke <target> <offset> <value> [1] (hex): a character's u32 (or byte)
// at an offset, logged or written (reverse engineering on the rig: a field's meaning tried in
// play).
bool AutoplayPeekOrPoke(const AutoplayStep& step) {
    const std::string& verb = step.command.verb;
    const auto target = ParseAutoplayTarget(step.Arg(0));
    const auto found = target ? FindAutoplayCharacter(*target) : std::nullopt;
    u32 offset = 0;
    u32 value = 0;
    const std::string_view offset_text = step.Arg(1);
    const std::string_view value_text = step.Arg(2);
    std::from_chars(offset_text.data(), offset_text.data() + offset_text.size(), offset, 16);
    std::from_chars(value_text.data(), value_text.data() + value_text.size(), value, 16);
    if (!found || offset > 0x2000 ||
        !HasMemoryAccess(found->character + offset, 4, MemoryProt::CpuRead)) {
        step.failure = "no such character";
        return false;
    }
    const u32 before = ReadValue<u32>(found->character, offset);
    if (verb == "poke" && step.Arg(3) == "1") {
        WriteValue(found->character, offset, static_cast<u8>(value)); // one byte
    } else if (verb == "poke") {
        WriteValue(found->character, offset, value);
    }
    LOG_INFO(Debug, "Bloodborne autoplay {}: {:#x} +{:#x} = {:#x}{}", verb,
             ReadValue<u32>(found->character, 0x08), offset, before,
             verb == "poke" ? fmt::format(" -> {:#x}", value) : "");
    return true;
}

// expect-hidden <handle> <0|1> [<timeout>]: the character hidden by the story (its event
// disable byte, +0x28) or not, within the time given (default: now).
bool AutoplayExpectHidden(const AutoplayStep& step) {
    const auto target = ParseAutoplayTarget(step.Arg(0));
    const auto found = target ? FindAutoplayCharacter(*target) : std::nullopt;
    if (!found) {
        step.failure = "no such character";
        return false;
    }
    const bool hidden = ReadValue<u8>(found->character, 0x28) != 0;
    if (hidden == (step.Number(1, 1) != 0)) {
        return true;
    }
    if (step.elapsed >= step.Number(2, 0)) {
        step.failure = fmt::format("hidden is {}", hidden ? 1 : 0);
    }
    return false;
}

// expect-disabled <handle> <0|1> [<timeout>]: the character's enable switch off (2004:5,
// IsCharacterDisabled) or on, within the time given (default: now).
bool AutoplayExpectDisabled(const AutoplayStep& step) {
    const auto target = ParseAutoplayTarget(step.Arg(0));
    const auto found = target ? FindAutoplayCharacter(*target) : std::nullopt;
    if (!found) {
        step.failure = "no such character";
        return false;
    }
    const bool disabled = IsCharacterDisabled(found->character);
    if (disabled == (step.Number(1, 1) != 0)) {
        return true;
    }
    if (step.elapsed >= step.Number(2, 0)) {
        step.failure = fmt::format("disabled is {}", disabled ? 1 : 0);
    }
    return false;
}

// set-disabled <handle> <0|1>: switches the character off (1) or on (0) in this game only, as its
// own story's event command 2004:5 would (the red mist's enemy in the Forbidden Woods starts off).
bool AutoplaySetDisabled(const AutoplayStep& step) {
    const auto target = ParseAutoplayTarget(step.Arg(0));
    const auto found = target ? FindAutoplayCharacter(*target) : std::nullopt;
    if (!found || !SetCharacterDisabled(found->character, step.Number(1, 1) != 0)) {
        step.failure = "no such character";
        return false;
    }
    LOG_INFO(Debug, "Bloodborne autoplay set-disabled: {:#x} -> {}",
             ReadValue<u32>(found->character, 0x08), step.Number(1, 1) != 0 ? 1 : 0);
    return true;
}

// watch-unseen <handle> <seconds>: from now on, for that long, every frame the character is
// loaded on this map drawn (neither its hide byte +0x28 nor its enable switch off) is logged as
// "watched <handle> drawn" (a scenario forbids it). The step itself ends at once.
bool AutoplayWatchUnseen(const AutoplayStep& step) {
    const auto target = ParseAutoplayTarget(step.Arg(0));
    if (!target) {
        step.failure = "no such character";
        return false;
    }
    const auto now = std::chrono::steady_clock::now();
    std::scoped_lock lock{autoplay_mutex};
    autoplay_run.watch_handle = static_cast<u32>(*target);
    autoplay_run.watch_started = now;
    autoplay_run.watch_until =
        now + std::chrono::milliseconds{static_cast<s64>(step.Number(1, 30) * 1000.0)};
    autoplay_run.watch_seen = 0;
    return true;
}

// wait-bullets <bullet> <count> [<timeout>]: until this game has made that many bullets of that
// BulletParam id since the step began (default 60 s): a boss's spells made in every game, as each
// makes its own copies' (Logarius's swords: 223200590).
bool AutoplayWaitBullets(const AutoplayStep& step) {
    const s32 bullet = static_cast<s32>(step.Number(0, 0));
    if (!step.begun) {
        autoplay_run.bullets_from = BulletsMade(bullet);
    }
    const u64 made = BulletsMade(bullet) - autoplay_run.bullets_from;
    if (made >= static_cast<u64>(step.Number(1, 1))) {
        LOG_INFO(Debug, "Bloodborne autoplay: {} bullets {} in {:.1f} s", made, bullet,
                 step.elapsed);
        return true;
    }
    if (step.elapsed > step.Number(2, 60)) {
        step.failure = fmt::format("{} bullets {} in {:.0f} s", made, bullet, step.elapsed);
    }
    return false;
}

// watch-effects <target> <seconds>: from now on, for that long, the character's SpEffects are
// logged once a second ("effects of <handle>: ...") and each one it gains or loses as it happens
// ("effects of <handle> changed: +5633"): a boss's spells wait for effects its runner's AI puts
// on it. The step itself ends at once.
bool AutoplayWatchEffects(const AutoplayStep& step) {
    const auto target = ParseAutoplayTarget(step.Arg(0));
    if (!target) {
        step.failure = "no such character";
        return false;
    }
    const auto now = std::chrono::steady_clock::now();
    std::scoped_lock lock{autoplay_mutex};
    autoplay_run.effects_handle = static_cast<u32>(*target);
    autoplay_run.effects_until =
        now + std::chrono::milliseconds{static_cast<s64>(step.Number(1, 30) * 1000.0)};
    autoplay_run.effects_logged.reset();
    autoplay_run.effects_seen.clear();
    return true;
}

// The watch-effects log, every frame after the co-op tick (see CheckAutoplayWatch).
void CheckAutoplayEffectsWatch(std::chrono::steady_clock::time_point now) {
    u32 handle = 0;
    {
        std::scoped_lock lock{autoplay_mutex};
        if (!autoplay_run.effects_until) {
            return;
        }
        if (now > *autoplay_run.effects_until) {
            LOG_INFO(Debug, "Bloodborne autoplay: effects watch on {:#x} over",
                     autoplay_run.effects_handle);
            autoplay_run.effects_until.reset();
            return;
        }
        handle = autoplay_run.effects_handle;
    }
    if (!AutoplayInWorld()) {
        return;
    }
    const auto found = FindAutoplayCharacter(handle);
    if (!found) {
        return;
    }
    std::vector<s32> effects = EffectsOf(found->character);
    std::ranges::sort(effects);
    std::scoped_lock lock{autoplay_mutex};
    const std::string changes = DescribeEffectChanges(autoplay_run.effects_seen, effects);
    if (!changes.empty() && autoplay_run.effects_logged) {
        LOG_INFO(Debug, "Bloodborne autoplay: effects of {:#x} changed: {}", handle, changes);
    }
    if (!autoplay_run.effects_logged ||
        now - *autoplay_run.effects_logged >= std::chrono::seconds{1}) {
        LOG_INFO(Debug, "Bloodborne autoplay: effects of {:#x} hp {}: {}", handle,
                 ReadValue<s32>(found->data, 0xF8), fmt::join(effects, " "));
        autoplay_run.effects_logged = now;
    }
    autoplay_run.effects_seen = std::move(effects);
}

} // namespace

namespace Coop {

// The watch-unseen check, every frame after the co-op tick (TickPartyController). Not while the
// world loads: nothing is drawn then.
void CheckAutoplayWatch() {
    const auto now = std::chrono::steady_clock::now();
    CheckAutoplayEffectsWatch(now);
    u32 handle = 0;
    double into = 0.0;
    {
        std::scoped_lock lock{autoplay_mutex};
        if (!autoplay_run.watch_until) {
            return;
        }
        if (now > *autoplay_run.watch_until) {
            LOG_INFO(Debug, "Bloodborne autoplay: watch on {:#x} over, seen drawn {} times",
                     autoplay_run.watch_handle, autoplay_run.watch_seen);
            autoplay_run.watch_until.reset();
            return;
        }
        handle = autoplay_run.watch_handle;
        into = std::chrono::duration<double>(now - *autoplay_run.watch_started).count();
    }
    if (!AutoplayInWorld()) {
        return;
    }
    const auto found = FindAutoplayCharacter(handle);
    if (!found || ReadValue<u8>(found->character, 0x28) != 0 ||
        IsCharacterDisabled(found->character)) {
        return;
    }
    std::scoped_lock lock{autoplay_mutex};
    if (++autoplay_run.watch_seen <= 5) {
        LOG_INFO(Debug,
                 "Bloodborne autoplay: watched {:#x} drawn on map {:#x} ({:.2f} s into the "
                 "watch)",
                 handle, GetCurrentPackedMap(), into);
    }
}

} // namespace Coop

namespace {

// note-max-hp: the player's max HP now, for a later expect-max-hp self == noted.
bool AutoplayNoteMaxHp(const AutoplayStep& step) {
    const auto self = FindAutoplayCharacter(AutoplaySelf);
    if (!self) {
        step.failure = "no player";
        return false;
    }
    autoplay_run.noted_max_hp = ReadValue<s32>(self->data, 0xFC);
    LOG_INFO(Debug, "Bloodborne autoplay: max hp {}", autoplay_run.noted_max_hp);
    return true;
}

// expect-max-hp <target> <op> <value|noted>: its max HP (a party guest's is not cut to 70%).
bool AutoplayExpectMaxHp(const AutoplayStep& step) {
    const auto target = ParseAutoplayTarget(step.Arg(0));
    const auto found = target ? FindAutoplayCharacter(*target) : std::nullopt;
    if (!found) {
        step.failure = "no such character";
        return false;
    }
    const s32 max = ReadValue<s32>(found->data, 0xFC);
    const s64 limit =
        step.Arg(2) == "noted" ? autoplay_run.noted_max_hp : static_cast<s64>(step.Number(2, 0));
    const auto holds = CompareAutoplay(max, step.Arg(1), limit);
    if (!holds || !*holds) {
        step.failure = fmt::format("max hp is {}", max);
        return false;
    }
    return true;
}

// expect-effect <id> <0|1> [<target>]: whether the player (or that character) has that SpEffect
// (9200: the level sync).
bool AutoplayExpectEffect(const AutoplayStep& step) {
    const auto target = ParseAutoplayTarget(step.Arg(2).empty() ? "self" : step.Arg(2));
    const auto found = target ? FindAutoplayCharacter(*target) : std::nullopt;
    if (!found) {
        step.failure = "no such character";
        return false;
    }
    const auto effects = EffectsOf(found->character);
    const bool has =
        std::ranges::find(effects, static_cast<s32>(step.Number(0, 0))) != effects.end();
    if (has != (step.Number(1, 1) != 0)) {
        step.failure = fmt::format("effect {} is {}", step.Arg(0), has ? "on" : "off");
        return false;
    }
    return true;
}

// Kaniel 2026-10-25, the host rules. use-lamp [<warp point>]: this player's trip from a lamp (the
// Hunter's Dream) or a headstone (the point named), as the lamp's talk asks for it (the rig's X at
// a lamp brings no prompt): the travel together question when the rule takes it, else a trip of
// its own. A lamp counts as touched (the Rot rule's cure).
bool AutoplayUseLamp(const AutoplayStep& step) {
    const s32 point = static_cast<s32>(step.Number(0, HuntersDreamWarpPoint));
    if (point == HuntersDreamWarpPoint) {
        lamp_uses.fetch_add(1);
    }
    if (OfferTeamTravel(point)) {
        LOG_INFO(Debug, "Bloodborne autoplay use-lamp: {} waits for the team question", point);
    } else {
        LOG_INFO(Debug, "Bloodborne autoplay use-lamp: travels alone to {}", point);
        RequestTeamTrip(point);
    }
    return true;
}

// travel-answer <yes|no> [<timeout>]: the travel question's answer, once it shows.
bool AutoplayTeamTravel(const AutoplayStep& step) {
    const auto view = ReadTeamTravel();
    if (view && view->question != TeamTravelView::Question::None) {
        LOG_INFO(Debug, "Bloodborne autoplay travel-answer: {} to the {} question for {}",
                 step.Arg(0),
                 view->question == TeamTravelView::Question::Vote ? "vote" : "with the team",
                 view->point);
        AnswerTeamTravel(step.Arg(0) == "yes");
        return true;
    }
    return step.WaitedOut(step.Number(1, 30));
}

// expect-rot <n> [<timeout>]: this player has n rot penalties, each on it as its effect, within
// the time (the effects come on a moment after a death's respawn).
bool AutoplayExpectRot(const AutoplayStep& step) {
    const RotView view = ReadRotView();
    const std::vector<s32> effects = LocalEffects();
    bool on = true;
    for (const u32 kind : view.kinds) {
        on = on && kind >= 1 && kind <= RotKinds &&
             std::ranges::find(effects, RotRows[kind - 1].row) != effects.end();
    }
    if (view.kinds.size() == static_cast<std::size_t>(step.Number(0, 0)) && on) {
        LOG_INFO(Debug, "Bloodborne autoplay expect-rot: {} ({})", view.kinds.size(),
                 view.text.empty() ? "none" : view.text);
        return true;
    }
    if (step.WaitedOut(step.Number(1, 20))) {
        step.failure = fmt::format("rot {} ({}), effects on={}", view.kinds.size(), view.text, on);
    }
    return false;
}

bool AutoplayExpectSession(const AutoplayStep& step) {
    const size_t members = AutoplaySessionMembers();
    if (members != static_cast<size_t>(step.Number(0, 1))) {
        step.failure = fmt::format("{} in the session", members);
        return false;
    }
    return true;
}

// expect-others-near <n> <meters>: at least n other players stand alive within <meters> of
// the player (a guest that got up invisible to the host: its copy dead, or left where it died).
bool AutoplayExpectOthersNear(const AutoplayStep& step) {
    const auto here = LocalPlayerPosition();
    const double limit = step.Number(1, 10);
    size_t close_by = 0;
    std::string seen;
    for (const OtherPlayer& other : OtherPlayers()) {
        const double distance =
            here && other.at ? std::hypot((*other.at)[0] - (*here)[0], (*other.at)[1] - (*here)[1],
                                          (*other.at)[2] - (*here)[2])
                             : -1.0;
        seen += fmt::format(" [type={} hp={} dead={:#x} at {:.1f} m]", other.type, other.hp,
                            other.dead_flags, distance);
        close_by +=
            other.hp > 0 && (other.dead_flags & 0x05) == 0 && distance >= 0.0 && distance <= limit;
    }
    if (close_by < static_cast<size_t>(step.Number(0, 1))) {
        step.failure =
            fmt::format("{} alive within {} m:{}", close_by, limit, seen.empty() ? " none" : seen);
        return false;
    }
    return true;
}

// expect-compass <bearing> <metres> [<degrees> [<metres>]] [down|dead] (Kaniel 2026-10-17, the
// teammate compass): within 10 s a marker on the compass is at that bearing (degrees from straight
// ahead, positive to the right) and distance, within the tolerances (20 degrees, 1.5 m), and in
// that state when one is named. Logs what the compass shows.
bool AutoplayExpectCompass(const AutoplayStep& step) {
    CompassView view;
    const bool shown = ReadCompass(view);
    std::string seen;
    for (size_t i = 0; i < view.markers.size(); ++i) {
        const CompassMarker& marker = view.markers[i];
        const int vital = view.vitals[i];
        seen += fmt::format(" [{} at {:.1f} deg, {:.1f} m, strip {:+.2f}{}{}]", view.names[i],
                            marker.bearing * 180.0f / std::numbers::pi_v<float>, marker.distance,
                            marker.place.x, marker.place.pinned ? " pinned" : "",
                            vital == static_cast<int>(Vital::Down)   ? " down"
                            : vital == static_cast<int>(Vital::Dead) ? " dead"
                                                                     : "");
    }
    const std::string text =
        shown ? fmt::format("camera heading {:.1f} deg,{}",
                            view.heading * 180.0f / std::numbers::pi_v<float>, seen)
              : view.why;
    const double bearing = step.Number(0, 0.0), metres = step.Number(1, 0.0);
    // Kaniel 2026-10-17: a last word "down" or "dead" wants the marker in that state.
    const std::string_view last = step.command.args.empty() ? std::string_view{} : std::string_view{step.command.args.back()};
    const bool wants_state = last == "down" || last == "dead";
    const int state = static_cast<int>(last == "down" ? Vital::Down : Vital::Dead);
    const auto number = [&](size_t index, double fallback) {
        return wants_state && index + 1 >= step.command.args.size() ? fallback
                                                                    : step.Number(index, fallback);
    };
    const double bearing_tolerance = number(2, 20.0), metres_tolerance = number(3, 1.5);
    for (size_t i = 0; i < view.markers.size(); ++i) {
        const CompassMarker& marker = view.markers[i];
        const double off = std::abs(
            WrapAngle(marker.bearing - static_cast<float>(bearing * std::numbers::pi / 180.0)));
        if (off * 180.0 / std::numbers::pi <= bearing_tolerance &&
            std::abs(marker.distance - metres) <= metres_tolerance &&
            (!wants_state || view.vitals[i] == state)) {
            LOG_INFO(Debug, "Bloodborne autoplay compass: {}", text);
            return true;
        }
    }
    if (step.elapsed > 10.0) {
        step.failure = fmt::format("no marker at {:.0f} deg, {:.1f} m: {}", bearing, metres, text);
        LOG_INFO(Debug, "Bloodborne autoplay compass: {}", text);
    }
    return false;
}

// expect-stains <compass> <map> [<seconds>] (Kaniel 2026-10-20, bloodstains on the compass and
// map): within `seconds` (10) the bloodstains shown number `compass` on the compass and `map` on
// the map screen's data (Kaniel after v1.0.2: only this player's own). Logs what both show.
bool AutoplayExpectStains(const AutoplayStep& step) {
    CompassView view;
    ReadCompass(view);
    PartyPanel panel;
    ReadPartyPanel(panel);
    size_t on_compass = 0, on_map = 0;
    std::string seen;
    if (view.stain) {
        ++on_compass;
        seen += fmt::format(" [compass own at {:.1f} deg, {:.1f} m]",
                            view.stain->bearing * 180.0f / std::numbers::pi_v<float>,
                            view.stain->distance);
    }
    if (const Bloodstain& stain = panel.own_stain; stain.map != 0) {
        ++on_map;
        seen += fmt::format(" [map own on {:#x} at ({:.1f}, {:.1f}, {:.1f}), {} echoes]", stain.map,
                            stain.at[0], stain.at[1], stain.at[2], stain.echoes);
    }
    const std::string text = seen.empty() ? " none" : seen;
    if (on_compass == static_cast<size_t>(step.Number(0, 0)) &&
        on_map == static_cast<size_t>(step.Number(1, 0))) {
        LOG_INFO(Debug, "Bloodborne autoplay stains:{}", text);
        return true;
    }
    if (step.elapsed > step.Number(2, 10.0)) {
        step.failure = fmt::format("{} on the compass, {} on the map:{}", on_compass, on_map, text);
    }
    return false;
}

// pickup-stain [<timeout>]: this player glides onto its own bloodstain at walking pace (from 2.5 m
// short of it, on the way from where it stands) and stays on it, pressing X now and then, until the
// game's record of it clears. A teleport onto the stain never touches it, and a glide past it
// missed it after a 10 s frame stall (rig 2026-10-21).
bool AutoplayPickupStain(const AutoplayStep& step) {
    static LampPosition from{}, way{};
    const auto stain = Coop::ReadOwnBloodstain();
    if (!step.begun) {
        const auto at = LocalPlayerPosition();
        if (!stain || !at) {
            step.failure = stain ? "no player" : "no bloodstain of this player";
            return false;
        }
        float dx = stain->at[0] - (*at)[0], dz = stain->at[2] - (*at)[2];
        const float length = std::sqrt(dx * dx + dz * dz);
        way = length < 0.1f ? LampPosition{0.0f, 0.0f, 1.0f}
                            : LampPosition{dx / length, 0.0f, dz / length};
        from = {stain->at[0] - way[0] * 2.5f, stain->at[1] + 0.3f, stain->at[2] - way[2] * 2.5f};
        LOG_INFO(Debug,
                 "Bloodborne autoplay: through the bloodstain ({} echoes) at ({:.1f}, {:.1f}, "
                 "{:.1f})",
                 stain->echoes, stain->at[0], stain->at[1], stain->at[2]);
    }
    if (!stain) {
        autoplay_buttons.store(0);
        LOG_INFO(Debug, "Bloodborne autoplay: bloodstain picked up after {:.1f} s", step.elapsed);
        return true;
    }
    const float along = std::min(static_cast<float>(step.elapsed) * 1.5f, 2.5f);
    // A heading h faces (-sin h, cos h).
    TeleportLocalPlayer({from[0] + way[0] * along, from[1], from[2] + way[2] * along},
                        std::atan2(-way[0], way[2]));
    autoplay_buttons.store(
        step.elapsed > 1.0 && std::fmod(step.elapsed, 1.5) < 0.3 ? AutoplayButton("cross") : 0);
    if (step.elapsed > step.Number(0, 15)) {
        autoplay_buttons.store(0);
        step.failure = fmt::format("the bloodstain ({} echoes) still there", stain->echoes);
    }
    return false;
}

// expect-others-shown <n>: exactly n other players show in this world (not hidden: Kaniel
// 2026-10-15, members gone to another map still stood here; nor faded out: player report
// 2026-10-23, the copy of a player who fell into the void stayed faded out after its respawn).
bool AutoplayExpectOthersShown(const AutoplayStep& step) {
    size_t shown = 0;
    std::string seen;
    for (const OtherPlayer& other : OtherPlayers()) {
        const bool off = IsCharacterDisabled(other.character);
        const float fade = CharacterFade(other.character);
        const bool faded = fade < 1.0f;
        seen += fmt::format(" [{:#x} {} fade {:.2f}]", ReadValue<u32>(other.character, 0x08),
                            off     ? "hidden"
                            : faded ? "faded out"
                                    : "shown",
                            fade);
        shown += off || faded ? 0 : 1;
    }
    if (shown != static_cast<size_t>(step.Number(0, 0))) {
        step.failure = fmt::format("{} shown:{}", shown, seen.empty() ? " none" : seen);
        return false;
    }
    return true;
}

bool AutoplayExpectMap(const AutoplayStep& step) {
    const auto map = ParseAutoplayTarget(step.Arg(0));
    // Raw, as all of the rig's checks (see AutoplayInWorld).
    if (!map || GetCurrentPackedMap() != *map) {
        step.failure = fmt::format("on map {:#x}", GetCurrentPackedMap());
        return false;
    }
    return true;
}

bool AutoplayDone(const AutoplayStep&) {
    return true;
}

// ping-aim (Kaniel 2026-10-24, team pings): a ping where the camera aims (on the locked-on enemy
// while locked on), as the pad's double tap or the ping key places it.
bool AutoplayPingAim(const AutoplayStep&) {
    RequestAimPing();
    return true;
}

// ping-map <map> <x> <y> <z>: a ping on that map's spot, as the map screen's Square places it.
bool AutoplayPingMap(const AutoplayStep& step) {
    const auto map = ParseAutoplayTarget(step.Arg(0));
    if (!map || *map == 0 || step.Arg(3).empty()) {
        step.failure = "ping-map wants <map> <x> <y> <z>";
        return false;
    }
    RequestMapPing(
        *map, {float(step.Number(1, 0.0)), float(step.Number(2, 0.0)), float(step.Number(3, 0.0))});
    return true;
}

// expect-pings <count> [<seconds> [<map> [<x> <z> [<metres>]]]] (Kaniel 2026-10-24): within
// `seconds` (10) `count` pings are shown; with a map, one of them is on it (within `metres`, 3,
// of x, z when given). Logs every ping shown: its sender, spot, distance and bearing here, and
// where it is on the screen.
bool AutoplayExpectPings(const AutoplayStep& step) {
    PingsView view;
    ReadPings(view);
    std::string seen;
    for (const PingShown& ping : view.pings) {
        seen +=
            fmt::format(" [{} ({}) on map {:#x} at ({:.1f}, {:.1f}, {:.1f}) enemy {:#x}{}",
                        ping.name.empty() ? "?" : ping.name,
                        ping.member.empty() ? "own" : ping.member, ping.map, ping.at[0], ping.at[1],
                        ping.at[2], ping.enemy, ping.from_map ? " from the map" : "");
        if (ping.here) {
            seen += fmt::format(" here {:.1f} m at {:.1f} deg", ping.distance,
                                ping.bearing * 180.0f / std::numbers::pi_v<float>);
        }
        if (ping.screen) {
            seen += fmt::format(" screen ({:.2f}, {:.2f}){}", ping.screen->x, ping.screen->y,
                                ping.screen->pinned ? " pinned" : "");
        }
        seen += fmt::format(" {:.1f} s left]", ping.seconds_left);
    }
    const std::string text = fmt::format("{}:{}", view.pings.size(), seen.empty() ? " none" : seen);
    bool matches = view.pings.size() == static_cast<size_t>(step.Number(0, 0));
    if (matches && !step.Arg(2).empty()) {
        const auto map = ParseAutoplayTarget(step.Arg(2));
        const bool spot = !step.Arg(4).empty();
        const double x = step.Number(3, 0.0), z = step.Number(4, 0.0), metres = step.Number(5, 3.0);
        matches = map && std::ranges::any_of(view.pings, [&](const PingShown& ping) {
                      return ping.map == *map &&
                             (!spot || std::hypot(ping.at[0] - x, ping.at[2] - z) <= metres);
                  });
    }
    if (matches) {
        LOG_INFO(Debug, "Bloodborne autoplay pings: {}", text);
        return true;
    }
    if (step.elapsed > step.Number(1, 10.0)) {
        step.failure = fmt::format("pings shown {}", text);
        LOG_INFO(Debug, "Bloodborne autoplay pings: {}", text);
    }
    return false;
}

// The lobby's verbs (team-*) and the title's (title-press <button> [ms], title-wait <seconds>,
// title-shot <name>: the menus before the world) run on the title screen (TickAutoplayTitle); in
// the world they are over already.
bool AutoplayTeamVerb(const AutoplayStep&) {
    return true;
}

// Kaniel 2026-10-16: the team menu in game, what its buttons do (menu-invite <online id>,
// menu-kick <online id>, menu-join, menu-leave; each [timeout]). Invasions: menu-invade,
// menu-invade-leave, menu-allow-invaders <0|1>, wait-invader <0|1> (RunTeamMenuVerb).
bool AutoplayTeamMenuVerb(const AutoplayStep& step) {
    return Coop::RunTeamMenuVerb(step.command, step.begun, step.elapsed, step.failure);
}

// Trading (bloodborne_trade.cpp). Items: echoes, vials, bullets, goods:<id> and, one each,
// weapon:<id>, armor:<id>, gem:<id> (ids as the game's params have them, decimal).
std::optional<u32> TradeItemId(std::string_view name, std::string_view prefix) {
    u32 id = 0;
    if (!name.starts_with(prefix) ||
        std::from_chars(name.data() + prefix.size(), name.data() + name.size(), id).ec !=
            std::errc{}) {
        return std::nullopt;
    }
    return id;
}

std::optional<TradeItem> ParseTradeItem(std::string_view name, std::string_view count) {
    u32 value = 0;
    if (std::from_chars(count.data(), count.data() + count.size(), value).ec != std::errc{}) {
        return std::nullopt;
    }
    if (const auto goods = TradeItemId(name, "goods:")) {
        return TradeItem{BloodborneCoop::TradeItemKind::Goods, *goods, value};
    }
    static constexpr std::pair<std::string_view, u32> Categories[]{
        {"weapon:", BloodborneCoop::WeaponCategory},
        {"armor:", BloodborneCoop::ArmorCategory},
        {"gem:", BloodborneCoop::GemCategory},
    };
    for (const auto& [prefix, category] : Categories) {
        if (const auto id = TradeItemId(name, prefix)) {
            return TradeItem{BloodborneCoop::TradeItemKind::Item, category | *id, value};
        }
    }
    if (name == "echoes") {
        return TradeItem{BloodborneCoop::TradeItemKind::Echoes, 0, value};
    }
    if (name == "vials") {
        return TradeItem{BloodborneCoop::TradeItemKind::Goods, BloodborneCoop::BloodVialGoods,
                         value};
    }
    if (name == "bullets") {
        return TradeItem{BloodborneCoop::TradeItemKind::Goods,
                         BloodborneCoop::QuicksilverBulletGoods, value};
    }
    return std::nullopt;
}

// trade-give <item> <count>: adds the items as a trade's grant does (the popup and all): a test's
// stock.
bool AutoplayTradeGive(const AutoplayStep& step) {
    const auto item = ParseTradeItem(step.Arg(0), step.Arg(1));
    if (!item) {
        step.failure = "no such item";
        return false;
    }
    AddTradeItems(std::span{&*item, 1});
    return true;
}

// trade-stock <item> <count>: the player holds exactly that many (the rest taken out, or what is
// missing added): a test's start.
bool AutoplayTradeStock(const AutoplayStep& step) {
    const auto item = ParseTradeItem(step.Arg(0), step.Arg(1));
    if (!item) {
        step.failure = "no such item";
        return false;
    }
    const u32 held = TradeHeld(*item);
    TradeItem change = *item;
    if (item->kind == BloodborneCoop::TradeItemKind::Item) {
        change.count = 1; // one each
        for (u32 count = held; count > item->count; --count) {
            TakeTradeItems(std::span{&change, 1});
        }
        for (u32 count = held; count < item->count; ++count) {
            AddTradeItems(std::span{&change, 1});
        }
    } else if (held > item->count) {
        change.count = held - item->count;
        TakeTradeItems(std::span{&change, 1});
    } else if (held < item->count) {
        change.count = item->count - held;
        AddTradeItems(std::span{&change, 1});
    }
    return true;
}

// trade-socket weapon:<id> gem:<id> <effect>: a gem with that first effect in the first slot of
// the player's first such weapon (a test's stock: the gem is what must travel with it).
bool AutoplayTradeSocket(const AutoplayStep& step) {
    const auto weapon = ParseTradeItem(step.Arg(0), "1");
    const auto gem = ParseTradeItem(step.Arg(1), "1");
    u32 effect = 0;
    const std::string_view effect_text = step.Arg(2);
    if (!weapon || !gem ||
        std::from_chars(effect_text.data(), effect_text.data() + effect_text.size(), effect).ec !=
            std::errc{}) {
        step.failure = "no such item";
        return false;
    }
    if (!SocketTradeGem(weapon->id, gem->id, effect)) {
        step.failure = "could not put the gem in";
        return false;
    }
    return true;
}

// expect-socketed weapon:<id> gem:<id> <effect> [<timeout>]: one of the player's such weapons has
// that gem with that first effect.
bool AutoplayExpectSocketed(const AutoplayStep& step) {
    const auto weapon = ParseTradeItem(step.Arg(0), "1");
    const auto gem = ParseTradeItem(step.Arg(1), "1");
    u32 effect = 0;
    const std::string_view effect_text = step.Arg(2);
    if (!weapon || !gem ||
        std::from_chars(effect_text.data(), effect_text.data() + effect_text.size(), effect).ec !=
            std::errc{}) {
        step.failure = "no such item";
        return false;
    }
    if (HasSocketedGem(weapon->id, gem->id, effect)) {
        return true;
    }
    if (step.elapsed > step.Number(3, 0)) {
        step.failure = "no such gem in the weapon";
    }
    return false;
}

// trade-offer <item> <count>... [for <item> <count>...] [<timeout>]: an offer to the first other
// member on this map, once there is one and trades are on.
bool AutoplayTradeOffer(const AutoplayStep& step) {
    TradeItems give;
    TradeItems want;
    bool wanting = false;
    double timeout = 60;
    for (size_t index = 0; index < step.command.args.size();) {
        const std::string_view word = step.Arg(index);
        if (word == "for") {
            wanting = true;
            ++index;
            continue;
        }
        const auto item = ParseTradeItem(word, step.Arg(index + 1));
        if (!item) {
            timeout = step.Number(index, timeout);
            ++index;
            continue;
        }
        (wanting ? want : give).push_back(*item);
        index += 2;
    }
    const std::vector<std::string> partners = TradePartners();
    if (!TradesOn() || partners.empty()) {
        return step.WaitedOut(timeout);
    }
    switch (OfferTrade(partners.front(), give, want)) {
    case TradeOfferResult::Sent:
        return true;
    case TradeOfferResult::NotEnough:
        step.failure = "not enough to offer";
        return false;
    default:
        step.failure = "not an offer";
        return false;
    }
}

// trade-answer <yes|no> [<timeout>]: answers the oldest offer to this member once there is one.
bool AutoplayTradeAnswer(const AutoplayStep& step) {
    const auto ask = FrontTradeAsk();
    if (!ask) {
        return step.WaitedOut(step.Number(1, 60));
    }
    const bool yes = step.Arg(0) == "yes";
    if (AnswerTrade(ask->id(), yes) != yes) {
        step.failure = "could not accept";
        return false;
    }
    return true;
}

// held <item>: logs what the player holds of it.
bool AutoplayHeld(const AutoplayStep& step) {
    const auto item = ParseTradeItem(step.Arg(0), "1");
    if (!item) {
        step.failure = "no such item";
        return false;
    }
    LOG_INFO(Debug, "Bloodborne autoplay held {}: {}", step.Arg(0), TradeHeld(*item));
    return true;
}

// expect-held <item> <count> [<timeout>]: what the player holds of it, within the time given.
bool AutoplayExpectHeld(const AutoplayStep& step) {
    const auto item = ParseTradeItem(step.Arg(0), step.Arg(1));
    if (!item) {
        step.failure = "no such item";
        return false;
    }
    const u32 held = TradeHeld(*item);
    if (held == item->count) {
        return true;
    }
    if (step.elapsed > step.Number(2, 0)) {
        step.failure = fmt::format("holds {}, not {}", held, item->count);
    }
    return false;
}

// wait-trade <accepted|declined|expired|refused|received|returned> [<timeout>]: until this run saw
// such a trade notice.
bool AutoplayWaitTrade(const AutoplayStep& step) {
    using Kind = TradeClient::Notice::Kind;
    static constexpr std::pair<std::string_view, Kind> Kinds[]{
        {"accepted", Kind::Accepted}, {"declined", Kind::Declined}, {"expired", Kind::Expired},
        {"refused", Kind::Refused},   {"received", Kind::Received}, {"returned", Kind::Returned},
    };
    const auto found =
        std::ranges::find(Kinds, step.Arg(0), &std::pair<std::string_view, Kind>::first);
    if (found == std::end(Kinds)) {
        step.failure = "no such notice";
        return false;
    }
    return TradeNoticesSeen(found->second) > 0 || step.WaitedOut(step.Number(1, 60));
}

// The gestures this player has learned, for the log.
std::string LearnedGesturesText() {
    std::vector<s32> learned;
    for (const GestureSlot& gesture : PlayerGestures()) {
        if (gesture.learned) {
            learned.push_back(gesture.gesture);
        }
    }
    return fmt::format("{}", fmt::join(learned, " "));
}

// talk-gesture <gesture> <flag>: an NPC talk's "gesture get" step (talk scripts 01.09, e.g.
// Alfred's t240333 gives Pray, 9, by 6749): nothing while the flag is on, else the gesture learned
// (talk command 66) and the flag set (11), as in this player's own talk.
bool AutoplayTalkGesture(const AutoplayStep& step) {
    const s32 gesture = static_cast<s32>(step.Number(0, 0));
    const s32 flag = static_cast<s32>(step.Number(1, 0));
    if (EventFlagOn(flag)) {
        LOG_INFO(Debug, "Bloodborne autoplay: talk gives no gesture {}: its flag {} is on", gesture,
                 flag);
        return true;
    }
    if (!LearnGesture(gesture)) {
        step.failure = "no slot for the gesture";
        return false;
    }
    reinterpret_cast<void PS4_SYSV_ABI (*)(u64, s32, bool)>(
        image_base + PartyGhostSetEventFlagOffset)(0, flag, true);
    LOG_INFO(Debug, "Bloodborne autoplay: talk gave gesture {} (flag {}); learned: {}", gesture,
             flag, LearnedGesturesText());
    return true;
}

// expect-gesture <gesture> <0|1> [<timeout>]: whether this player has learned the gesture.
bool AutoplayExpectGesture(const AutoplayStep& step) {
    const s32 gesture = static_cast<s32>(step.Number(0, 0));
    const auto gestures = PlayerGestures();
    const bool learned = std::ranges::any_of(
        gestures, [&](const GestureSlot& slot) { return slot.learned && slot.gesture == gesture; });
    if (learned == (step.Number(1, 1) != 0)) {
        return true;
    }
    if (step.elapsed > step.Number(2, 0)) {
        step.failure = fmt::format("gesture {} {}learned ({} slots; learned: {})", gesture,
                                   learned ? "" : "not ", gestures.size(), LearnedGesturesText());
    }
    return false;
}

// Kaniel 2026-10-25, team duels: duel-propose and duel-cancel (the team screen's buttons),
// duel-answer <yes|no> [<timeout>] (the question's box, once it is asked), wait-duel <part>
// [<timeout>] (this player's part: none, asked, answered, countdown, fighting or out),
// duel-strike (below).
bool AutoplayDuelPropose(const AutoplayStep&) {
    ProposeDuel();
    return true;
}

bool AutoplayDuelCancel(const AutoplayStep&) {
    CancelDuel();
    return true;
}

bool AutoplayDuelAnswer(const AutoplayStep& step) {
    if (const auto ask = DuelAsk()) {
        AnswerDuel(ask->first, step.Arg(0) == "yes");
        LOG_INFO(Debug, "Bloodborne autoplay: duel {} from {} answered {}", ask->first, ask->second,
                 step.Arg(0));
        return true;
    }
    return step.WaitedOut(step.Number(1, 30));
}

// duel-strike [<timeout>]: light attacks on the nearest other player from each side in turn
// until this player fights no more (its foe out, or itself).
bool AutoplayDuelStrike(const AutoplayStep& step) {
    if (ReadDuel().part != DuelPart::Fighting) {
        autoplay_buttons.store(0);
        LOG_INFO(Debug, "Bloodborne autoplay: duel strikes done after {:.1f} s", step.elapsed);
        return true;
    }
    if (const auto found = FindAutoplayCharacter(AutoplayPlayer)) {
        AutoplayAttackCycle(*found, step.elapsed, false);
    }
    if (step.elapsed > step.Number(0, 60)) {
        autoplay_buttons.store(0);
        step.failure = fmt::format("still fighting after {:.0f} s", step.elapsed);
    }
    return false;
}

bool AutoplayWaitDuel(const AutoplayStep& step) {
    static constexpr std::array<std::string_view, 6> Parts{"none",      "asked",    "answered",
                                                           "countdown", "fighting", "out"};
    const auto wanted = std::ranges::find(Parts, step.Arg(0));
    if (wanted == Parts.end()) {
        step.failure = "no such duel part";
        return false;
    }
    const DuelPart part = ReadDuel().part;
    if (static_cast<size_t>(part) == static_cast<size_t>(wanted - Parts.begin())) {
        return true;
    }
    if (step.WaitedOut(step.Number(1, 60))) {
        step.failure =
            fmt::format("this player's duel part is {}", Parts[static_cast<size_t>(part)]);
    }
    return false;
}

// The script's verbs (tools/rig/tests/test_rig.py keeps the same list for the scenarios).
using AutoplayVerb = bool (*)(const AutoplayStep& step);
constexpr auto AutoplayVerbs = std::to_array<std::pair<std::string_view, AutoplayVerb>>({
    {"wait", AutoplayWait},
    {"wait-session", AutoplayWaitSession},
    {"wait-solo", AutoplayWaitSolo},
    {"wait-members", AutoplayWaitMembers},
    {"wait-map", AutoplayWaitMap},
    {"wait-vital", AutoplayWaitVital},
    {"wait-member-vital", AutoplayWaitMemberVital},
    {"death-mode", AutoplayDeathMode},
    {"wait-death-mode", AutoplayDeathMode},
    {"free-camera", AutoplayFreeCamera},
    {"lamp-use", AutoplayLampUse},
    {"ensure-map", AutoplayEnsureMap},
    {"travel", AutoplayTravel},
    {"hurt", AutoplayHurtOrKill},
    {"kill", AutoplayHurtOrKill},
    {"mark", AutoplayMark},
    {"approach", AutoplayApproach},
    {"attack", AutoplayAttack},
    {"hit", AutoplayHit},
    {"teleport", AutoplayTeleport},
    {"expect-walks", AutoplayExpectWalks},
    {"place", AutoplayPlace},
    {"glide", AutoplayGlide},
    {"pickup", AutoplayPickup},
    {"rise", AutoplayRise},
    {"approach-downed", AutoplayApproachDowned},
    {"revive-downed", AutoplayReviveDowned},
    {"press", AutoplayPressOrStick},
    {"stick", AutoplayPressOrStick},
    {"look", AutoplayPressOrStick},
    {"set-flag", AutoplaySetFlag},
    {"return-point", AutoplayReturnPoint},
    {"expect-flag", AutoplayExpectOrWaitFlag},
    {"talk-gesture", AutoplayTalkGesture},
    {"expect-gesture", AutoplayExpectGesture},
    {"duel-propose", AutoplayDuelPropose},
    {"duel-cancel", AutoplayDuelCancel},
    {"duel-answer", AutoplayDuelAnswer},
    {"wait-duel", AutoplayWaitDuel},
    {"duel-strike", AutoplayDuelStrike},
    {"wait-flag", AutoplayExpectOrWaitFlag},
    {"flags", AutoplayLogFlags},
    {"altar-record", AutoplayAltarRecord},
    {"altar-state", AutoplayAltarState},
    {"flag-list", AutoplayFlagList},
    {"godmode", AutoplayGodmode},
    {"snapshot", AutoplaySnapshot},
    {"census", AutoplayCensus},
    {"shot", AutoplayShot},
    {"log", AutoplayLog},
    {"cutscene-options", AutoplayCutsceneOptions},
    {"expect-hp", AutoplayExpectHp},
    {"wait-hp", AutoplayWaitHp},
    {"dump", AutoplayDump},
    {"raise-stat", AutoplayRaiseStat},
    {"add-effect", AutoplayAddEffect},
    {"note-wallet", AutoplayNoteWallet},
    {"expect-wallet", AutoplayExpectWallet},
    {"expect-dregs", AutoplayExpectDregs},
    {"object", AutoplayObject},
    {"peek", AutoplayPeekOrPoke},
    {"poke", AutoplayPeekOrPoke},
    {"expect-hidden", AutoplayExpectHidden},
    {"expect-disabled", AutoplayExpectDisabled},
    {"set-disabled", AutoplaySetDisabled},
    {"watch-unseen", AutoplayWatchUnseen},
    {"watch-effects", AutoplayWatchEffects},
    {"wait-bullets", AutoplayWaitBullets},
    {"note-max-hp", AutoplayNoteMaxHp},
    {"expect-max-hp", AutoplayExpectMaxHp},
    {"expect-effect", AutoplayExpectEffect},
    {"expect-session", AutoplayExpectSession},
    {"use-lamp", AutoplayUseLamp},
    {"travel-answer", AutoplayTeamTravel},
    {"expect-rot", AutoplayExpectRot},
    {"expect-others-near", AutoplayExpectOthersNear},
    {"expect-others-shown", AutoplayExpectOthersShown},
    {"expect-compass", AutoplayExpectCompass},
    {"expect-stains", AutoplayExpectStains},
    {"ping-aim", AutoplayPingAim},
    {"ping-map", AutoplayPingMap},
    {"expect-pings", AutoplayExpectPings},
    {"pickup-stain", AutoplayPickupStain},
    {"expect-map", AutoplayExpectMap},
    {"chapter", AutoplayChapter},
    {"keep-going", AutoplayKeepGoing},
    {"sync", AutoplaySync},
    {"team-create", AutoplayTeamVerb},
    {"team-invite", AutoplayTeamVerb},
    {"team-accept", AutoplayTeamVerb},
    {"team-back", AutoplayTeamVerb},
    {"team-wait", AutoplayTeamVerb},
    {"team-leave", AutoplayTeamVerb},
    {"team-screen", AutoplayTeamVerb},
    {"team-ready", AutoplayTeamVerb},
    {"team-start", AutoplayTeamVerb},
    {"team-alone", AutoplayTeamVerb},
    {"team-worlds", AutoplayTeamVerb},
    {"title-press", AutoplayTeamVerb},
    {"title-wait", AutoplayTeamVerb},
    {"title-shot", AutoplayTeamVerb},
    {"title-keys", AutoplayTeamVerb},
    {"menu-invite", AutoplayTeamMenuVerb},
    {"menu-kick", AutoplayTeamMenuVerb},
    {"menu-join", AutoplayTeamMenuVerb},
    {"menu-leave", AutoplayTeamMenuVerb},
    {"menu-invade", AutoplayTeamMenuVerb},
    {"menu-invade-leave", AutoplayTeamMenuVerb},
    {"menu-allow-invaders", AutoplayTeamMenuVerb},
    {"wait-invader", AutoplayTeamMenuVerb},
    {"keys", AutoplayKeys},
    {"done", AutoplayDone},
    {"trade-give", AutoplayTradeGive},
    {"trade-stock", AutoplayTradeStock},
    {"trade-socket", AutoplayTradeSocket},
    {"expect-socketed", AutoplayExpectSocketed},
    {"trade-offer", AutoplayTradeOffer},
    {"trade-answer", AutoplayTradeAnswer},
    {"held", AutoplayHeld},
    {"expect-held", AutoplayExpectHeld},
    {"wait-trade", AutoplayWaitTrade},
    {"cut-link", AutoplayCutLink},
    {"quit-title", AutoplayQuitTitle},
});

// Runs one script command for this tick: true once it is done, `failure` set if it failed.
bool RunAutoplayStep(const AutoplayCommand& command, bool begun, double elapsed,
                     std::string& failure) {
    const AutoplayStep step{command, begun, elapsed, failure};
    for (const auto& [verb, run] : AutoplayVerbs) {
        if (command.verb == verb) {
            return run(step);
        }
    }
    failure = "unknown command";
    return false;
}

// Godmode: the player's HP back to its max each tick while alive.
void KeepAutoplayGodmode() {
    if (!autoplay_godmode.load()) {
        return;
    }
    if (const auto self = FindAutoplayCharacter(AutoplaySelf);
        self && ReadValue<s32>(self->data, 0xF8) > 0 &&
        HasMemoryAccess(self->data, 0x100, MemoryProt::CpuWrite)) {
        WriteValue(self->data, 0xF8, ReadValue<s32>(self->data, 0xFC));
    }
}

// The step to run this tick, or nothing (not in the world, a held press still on or just let go,
// or the script over). autoplay_mutex held.
struct AutoplayTurn {
    AutoplayCommand command;
    size_t index = 0;
    size_t total = 0;
    bool begun = false;
    double elapsed = 0.0;
};
std::optional<AutoplayTurn> NextAutoplayTurn(std::chrono::steady_clock::time_point now) {
    if (autoplay_run.phase != AutoplayRun::Phase::World) {
        return std::nullopt;
    }
    if (autoplay_run.hold_until) {
        if (now < *autoplay_run.hold_until) {
            return std::nullopt;
        }
        autoplay_run.hold_until.reset();
        autoplay_buttons.store(0);
        autoplay_sticks.store(0x80808080);
        LOG_INFO(Debug, "Bloodborne autoplay: step {}/{} {} ok", autoplay_run.next + 1,
                 autoplay_run.script.size(), AutoplayText(autoplay_run.script[autoplay_run.next]));
        ++autoplay_run.next;
        autoplay_run.step_started = now;
        autoplay_run.step_begun = false;
        return std::nullopt;
    }
    if (autoplay_run.next >= autoplay_run.script.size() ||
        autoplay_run.script[autoplay_run.next].verb == "done") {
        autoplay_run.phase = AutoplayRun::Phase::Done;
        LOG_INFO(Debug, "Bloodborne autoplay: done ({})", autoplay_run.failed ? "failed" : "ok");
        return std::nullopt;
    }
    AutoplayTurn turn{autoplay_run.script[autoplay_run.next], autoplay_run.next,
                      autoplay_run.script.size(), autoplay_run.step_begun,
                      std::chrono::duration<double>(now - autoplay_run.step_started).count()};
    autoplay_run.step_begun = true;
    return turn;
}

// A step failed: on to the next chapter with keep-going, else the script is over.
// autoplay_mutex held.
void FailAutoplayStep(const AutoplayTurn& turn, const std::string& failure) {
    LOG_ERROR(Debug, "Bloodborne autoplay: step {}/{} {} FAILED: {}", turn.index + 1, turn.total,
              AutoplayText(turn.command), failure);
    autoplay_buttons.store(0);
    autoplay_sticks.store(0x80808080);
    if (autoplay_run.keep_going) {
        if (const auto next = NextAutoplayChapter(autoplay_run.script, turn.index)) {
            // The rig lets the others past this chapter's syncs (they would wait in vain).
            LOG_INFO(Debug, "Bloodborne autoplay: chapter {} given up", autoplay_run.chapter);
            autoplay_run.failed = true;
            autoplay_run.next = *next;
            autoplay_run.step_started = std::chrono::steady_clock::now();
            autoplay_run.step_begun = false;
            return;
        }
    }
    LOG_INFO(Debug, "Bloodborne autoplay: done (failed)");
    autoplay_run.phase = AutoplayRun::Phase::Done;
}

} // namespace

namespace Coop {

// From the party tick (the game thread), once the player is in the world.
void TickAutoplay() {
    KeepAutoplayGodmode();
    const auto now = std::chrono::steady_clock::now();
    std::optional<AutoplayTurn> turn;
    {
        std::scoped_lock lock{autoplay_mutex};
        turn = NextAutoplayTurn(now);
        // A chapter past its time budget gives up there (keep-going: the suite goes on).
        if (turn && turn->command.verb != "chapter" && autoplay_run.chapter_deadline &&
            now > *autoplay_run.chapter_deadline) {
            autoplay_run.chapter_deadline.reset();
            FailAutoplayStep(*turn, fmt::format("chapter {} over its {:g} s budget",
                                                autoplay_run.chapter, autoplay_run.chapter_budget));
            return;
        }
    }
    if (!turn) {
        return;
    }
    const AutoplayCommand& command = turn->command;
    const size_t index = turn->index;
    const size_t total = turn->total;
    std::string failure;
    const bool finished = RunAutoplayStep(command, turn->begun, turn->elapsed, failure);
    std::scoped_lock lock{autoplay_mutex};
    if (!failure.empty()) {
        FailAutoplayStep(*turn, failure);
        return;
    }
    if (finished && !autoplay_run.hold_until) {
        LOG_INFO(Debug, "Bloodborne autoplay: step {}/{} {} ok (t={:.1f})", index + 1, total,
                 AutoplayText(command), SteadyMilliseconds() / 1000.0);
        ++autoplay_run.next;
        autoplay_run.step_started = std::chrono::steady_clock::now();
        autoplay_run.step_begun = false;
        if (command.verb == "quit-title") {
            autoplay_run.phase = AutoplayRun::Phase::Title;
            autoplay_run.leaving_world = true;
            autoplay_run.title_visits = Coop::TeamTitleVisits();
        }
    }
}

} // namespace Coop

namespace {

// From the pad reader: on the title screen, Cross now and then until the player is in the world.
void TickAutoplayTitle() {
    std::scoped_lock lock{autoplay_mutex};
    if (autoplay_run.phase != AutoplayRun::Phase::Title) {
        return;
    }
    const auto now = std::chrono::steady_clock::now();
    if (autoplay_run.leaving_world) {
        // quit-title: the world goes first, then the title menu comes up and the lobby hears of
        // it (its team steps read the lobby's answer, not its state from the world).
        if (AutoplayInWorld()) {
            autoplay_buttons.store(0);
            return;
        }
        if (Coop::TeamTitleVisits() == autoplay_run.title_visits) {
            // The title's splash: Cross now and then until its menu is up.
            const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                now - autoplay_run.step_started);
            autoplay_buttons.store(
                TitlePressesCross(static_cast<u64>(ms.count())) ? AutoplayButton("cross") : 0);
            return;
        }
        if (Coop::TeamLobbySaysPlaying()) {
            autoplay_buttons.store(0);
            return;
        }
        autoplay_run.leaving_world = false;
        autoplay_run.started = now; // the title's Cross times from here
        LOG_INFO(Debug, "Bloodborne autoplay: back at the title");
    }
    if (AutoplayInWorld()) {
        autoplay_run.phase = AutoplayRun::Phase::World;
        autoplay_run.step_started = now;
        autoplay_run.step_begun = false;
        autoplay_buttons.store(0);
        // Raw, as all of the rig's checks (see AutoplayInWorld).
        LOG_INFO(Debug, "Bloodborne autoplay: in the world on map {:#x}", GetCurrentPackedMap());
        return;
    }
    // The script's first steps may be the lobby's (team-*): they run here, before the title's
    // Cross (twice a second: they wait on the server's answers).
    if (autoplay_run.next < autoplay_run.script.size() &&
        (Coop::IsTeamTitleVerb(autoplay_run.script[autoplay_run.next].verb) ||
         autoplay_run.script[autoplay_run.next].verb.starts_with("title-"))) {
        const AutoplayCommand& command = autoplay_run.script[autoplay_run.next];
        if (!autoplay_run.step_begun) {
            autoplay_run.step_started = now; // the title's steps time from their own start
        }
        const double elapsed =
            std::chrono::duration<double>(now - autoplay_run.step_started).count();
        std::string failure;
        bool finished = false;
        if (command.verb == "title-press") {
            // The button held for its time (150 ms), then let go for as long.
            const u32 button = AutoplayButton(command.args.empty() ? "" : command.args[0]);
            const double held =
                (command.args.size() > 1 ? std::atof(command.args[1].c_str()) : 150.0) / 1000.0;
            if (button == 0) {
                failure = "no such button";
            }
            autoplay_buttons.store(elapsed < held ? button : 0);
            finished = elapsed >= 2 * held;
        } else if (command.verb == "title-wait") {
            autoplay_buttons.store(0);
            finished = elapsed >= (command.args.empty() ? 1.0 : std::atof(command.args[0].c_str()));
        } else if (command.verb == "title-keys") {
            autoplay_buttons.store(0);
            finished = WriteAutoplayMenuKeys(command.args, failure);
        } else if (command.verb == "title-shot") {
            autoplay_buttons.store(0);
            const AutoplayCommand shot{"shot", command.args};
            finished = AutoplayShot(AutoplayStep{shot, autoplay_run.step_begun, elapsed, failure});
        } else if (Coop::IsTeamScreenVerb(command.verb) && !Coop::TeamScreenUp()) {
            // The title's Cross until the save's load is held and the team screen is up.
            const auto ms =
                std::chrono::duration_cast<std::chrono::milliseconds>(now - autoplay_run.started);
            autoplay_buttons.store(
                TitlePressesCross(static_cast<u64>(ms.count())) ? AutoplayButton("cross") : 0);
            finished = false;
            if (elapsed > 180.0) {
                failure = "the team screen never came up";
            }
        } else {
            autoplay_buttons.store(0);
            static std::chrono::steady_clock::time_point last_team_step{};
            if (now - last_team_step < std::chrono::milliseconds{500}) {
                return;
            }
            last_team_step = now;
            finished = Coop::RunTeamTitleVerb(command, autoplay_run.step_begun, elapsed, failure);
        }
        autoplay_run.step_begun = true;
        if (!failure.empty()) {
            LOG_ERROR(Debug, "Bloodborne autoplay: step {}/{} {} FAILED: {}", autoplay_run.next + 1,
                      autoplay_run.script.size(), AutoplayText(command), failure);
            LOG_INFO(Debug, "Bloodborne autoplay: done (failed)");
            autoplay_run.phase = AutoplayRun::Phase::Done;
        } else if (finished) {
            LOG_INFO(Debug, "Bloodborne autoplay: step {}/{} {} ok (t={:.1f})",
                     autoplay_run.next + 1, autoplay_run.script.size(), AutoplayText(command),
                     SteadyMilliseconds() / 1000.0);
            ++autoplay_run.next;
            autoplay_run.step_started = now;
            autoplay_run.step_begun = false;
        }
        return;
    }
    const auto ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(now - autoplay_run.started);
    autoplay_buttons.store(TitlePressesCross(static_cast<u64>(ms.count())) ? AutoplayButton("cross")
                                                                           : 0);
}

} // namespace

bool AutoplayPad(std::uint32_t& buttons, std::array<std::uint8_t, 4>& sticks) {
    MakeEnemiesHarder();
    std::call_once(autoplay_loaded, LoadAutoplay);
    if (autoplay_run.phase == AutoplayRun::Phase::Off) {
        return false;
    }
    TickAutoplayTitle();
    buttons = autoplay_buttons.load();
    const u32 packed = autoplay_sticks.load();
    sticks = {static_cast<u8>(packed), static_cast<u8>(packed >> 8), static_cast<u8>(packed >> 16),
              static_cast<u8>(packed >> 24)};
    return true;
}

} // namespace Core::Bloodborne
