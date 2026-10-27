// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// Travel and place: lamps and rests, talks, trips, the guest's resume. Split from
// bloodborne_re.cpp; the shared declarations are in bloodborne_re_internal.h.

#include "core/bloodborne_crash_dump.h"
#include "core/bloodborne_flags.h"
#include "core/bloodborne_lamp_points.h"
#include "core/bloodborne_party.h"
#include "core/bloodborne_re.h"
#include "core/bloodborne_re_internal.h"
#include "core/bloodborne_trade.h"
#include "core/bloodborne_write_watch.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <charconv>
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
namespace Coop {

uintptr_t image_base{};

} // namespace Coop

namespace {

// Lamps as campfires (see IsLampTravelFlag). Two register hooks in the talk scripts' native code
// let a party in a session use its lamps, and turn the travel dialog's flag into a rest; the
// party's rest board on shadNet tells every member when the whole party rests at one lamp.
//
// ESD function 52 ("in multiplayer"): `cmp [count+0x14], 1; setg al; test ecx, ecx; setg cl; or
// cl, al` (more than one player, or a pending matching). Only the warp chairs (lamps, the
// Hunter's Dream headstones, chalice lamps) ask it; their multiplayer state has no rest.
constexpr u64 EsdIsMultiplayerResultOffset = 0x0203C55F;
constexpr std::array<u8, 7> EsdIsMultiplayerResult{0x85, 0xC9, 0x0F, 0x9F, 0xC1, 0x08, 0xC1};
// Talk command 11 (SetEventFlag), with the flag in r15d: `mov ecx, [rax+0xA0]; cmp ecx, -1`
// loads the flag range the talk may set ([rax+0xA0, rax+0xA4], -1 for any). A range that leaves
// the flag out makes the command skip it.
constexpr u64 TalkSetEventFlagRangeOffset = 0x0204232E;
constexpr std::array<u8, 9> TalkSetEventFlagRange{0x8B, 0x88, 0xA0, 0x00, 0x00,
                                                  0x00, 0x83, 0xF9, 0xFF};
constexpr s32 LampTouchAnimation = 101160;
// WorldChrMan +0xA6C: the bonfire respawn left over from Dark Souls. 1 asks for it; WorldChrMan
// then respawns every loaded block's characters (3), SprjEmkSystem restarts the event scripts
// (4) and the characters are released again (5, then 0). The healing fountain update requests it
// 1.33 s after OnEvent_BonfireRespawn, which no Bloodborne talk script sends.
constexpr u64 WorldRespawnStateOffset = 0xA6C;
// The two restock steps of the respawn load (0x01939470): 0x014DBB70 sorts out one goods entry
// (0x40000385) first, 0x014DACE0 then moves every goods with the restock flag from the storage
// box into the inventory, up to its limit. Neither takes arguments.
// The bonfire regene (Util_RequestRegene's work, 0x018FAC80(player, refill)): with refill 0 it
// skips the Dark Souls flask refill and only applies the rest special effects 100-109 (HP and
// the rest). RequestFullRecover alone did not heal the player at a lamp.
constexpr u64 BonfireRegeneOffset = 0x018FAC80;
constexpr u64 RestockItemClampOffset = 0x014DBB70;
constexpr u64 RestockFromStorageOffset = 0x014DACE0;
constexpr std::array<u8, 7> RestockItemClampPrologue{0x55, 0x48, 0x89, 0xE5, 0x48, 0x8D, 0x05};

// SprjEmkSystem's update (0x012F0F90): `mov ecx, [rax+0xA6C]; cmp ecx, 4` with rax = WorldChrMan.
// In state 3 it restarts every event script and resets the temporary event flags, then moves
// to 4. In a session that restart spawns things a reload never does (a quest monster behind the
// window near the Central Yharnam lamp), so a party rest goes straight to 4: the characters still
// respawn (WorldChrMan did that before state 3) and the scripts keep running as they were.
constexpr u64 EventRestartStateOffset = 0x012F0FCB;
constexpr std::array<u8, 9> EventRestartState{0x8B, 0x88, 0x6C, 0x0A, 0x00, 0x00, 0x83, 0xF9, 0x04};
std::atomic<bool> party_rest_respawn_pending{};
// The bonfire respawn's two walks over a block's characters (0x0190E3B0) skip a character whose
// NPC param has bit 0x2000000 at +0x145 (`mov rax, [rax+8]; test rax, rax` loads the param). For
// a living character during a party rest the hook points rax at a stand-in with that bit set.
constexpr std::array<u64, 2> RespawnParamChecks{0x0190E445, 0x0190E572};
constexpr std::array<u8, 7> RespawnParamCheck{0x48, 0x8B, 0x40, 0x08, 0x48, 0x85, 0xC0};
alignas(16) std::array<u8, 0x150> no_respawn_param{};
alignas(16) std::array<u64, 2> no_respawn_param_holder{};
std::atomic<bool> party_rest_respawn_running{};
// Which respawn runs (see RespawnsInWorldRespawn): a party rest, or the one after a wipe's trip.
std::atomic<WorldRespawnKind> world_respawn_kind{WorldRespawnKind::PartyRest};
// What a BroughtBack respawn brings back (see BringBackWhatTheHostBroughtBack). Each one is a
// short freeze of the map's characters, so they come in batches: the bell-ringing woman's
// skeletons came back every few seconds while she rang (Kaniel's chalice playtest 2026-10-25).
std::unordered_set<u32> brought_back_handles;
constexpr auto BringBackEvery = std::chrono::seconds{2};

// A wipe's trip loads the map again, which brings back what died since this world was loaded,
// as a solo death does: the host's and the other guests' copies of an enemy the party killed came
// back at full HP. Not what this member's own save left dead when its world came from that save:
// rig smoke 2026-10-12, a guest whose game restarted mid-session loaded its save with 0x1001400b
// dead, joined as a phantom in the host's world, and the wipe's trip (WarpNextStageBonfire, the
// same stage request 0x013CDE30 as the solo death's reload in SoloPlayDeath_2 0x01381DE0) took it
// home: its world came back as the save had it, 0x1001400b at 0 HP with no death mark (+0x1E3 0).
// So a few seconds after a wipe's trip arrives (bosses beaten in the save are put down by then,
// 0x26), the bonfire respawn brings back exactly those (LeftDeadByItsSave), with the event
// restart skipped as for a party rest. Nothing to bring back: nothing happens.
constexpr auto WipeRespawnAfterArrival = std::chrono::seconds{5};
std::optional<std::chrono::steady_clock::time_point> wipe_respawn_at;

std::mutex lamp_rest_mutex;
LampRestState lamp_rest;
LampRestCoordinator lamp_rest_coordinator;

} // namespace

namespace Coop {

bool lamp_rest_hooks_installed{};

} // namespace Coop

namespace {

bool lamp_rests_enabled = LampRestsEnabled(std::getenv("SHADPS4_BLOODBORNE_LAMP_REST"));
std::optional<std::chrono::steady_clock::time_point> lamp_touch_animation_at;
alignas(16) std::array<u8, 0x100> no_talk_flag_range{}; // +0xA0 = +0xA4 = 0

// The character talking in the talk whose functions were evaluated last on this thread (r15 is the
// talk at the dispatcher, +0x18 its talker, as ESD function 14 reads it): its commands run right
// after, so RestInsteadOfLampTravel knows whose talk sets a flag.
thread_local u64 esd_talker = 0;

} // namespace

namespace Coop {

// Every lamp the local player used (party death forgets its old downs at a lamp).
std::atomic<u32> lamp_uses{};

// What the game thread tells the rest board client, and what the client last heard back.
std::atomic<bool> rest_board_active{};
std::atomic<s32> rest_board_lamp{NotResting};
std::mutex rest_board_position_mutex;
std::optional<LampPosition> rest_board_position;
std::mutex rest_board_mutex;
std::vector<LampRest> rest_board;
std::chrono::steady_clock::time_point rest_board_time{};

// Seamless travel: the map this member is on while in a party session (0 otherwise), posted to
// the party's map board by the same client thread, and the board it read back.
std::atomic<u32> party_map_now{};

} // namespace Coop

namespace {

// Characters seen alive during this visit to the map (sampled once a second while in a party
// session; see RespawnsAtPartyRest).
std::mutex seen_alive_mutex;
std::unordered_set<u64> seen_alive;
u64 seen_alive_visit = 0; // see Place::visit: a reload of the same map makes new characters

bool WasAliveThisVisit(u64 character) {
    std::scoped_lock lock{seen_alive_mutex};
    return seen_alive.contains(character);
}

// The character's HP (+0xF8) and max HP (+0xFC) of its data module ([[+0x3B0]+0x20]), death flags
// (+0x1E3), explicit disable and story hide byte (+0x28).
RespawnCandidate ReadRespawnCandidate(u64 character) {
    const u64 modules = ReadValue<u64>(character, 0x3B0);
    const u64 data = modules >= 0x10000 ? ReadValue<u64>(modules, 0x20) : 0;
    return RespawnCandidate{
        .hp = data >= 0x10000 ? ReadValue<s32>(data, 0xF8) : 0,
        .max_hp = data >= 0x10000 ? ReadValue<s32>(data, 0xFC) : 0,
        .death_flags = ReadValue<u8>(character, 0x1E3),
        .disabled = IsCharacterDisabled(character),
        .hidden = ReadValue<u8>(character, 0x28) != 0,
        .changed_by_party = ChangedLikeTheHost(ReadValue<u32>(character, 0x08)),
        .brought_back = brought_back_handles.contains(ReadValue<u32>(character, 0x08)),
    };
}

void PS4_SYSV_ABI RespawnOnlyTheDead(u64, GuestRegisterSnapshot* registers) {
    if (!party_rest_respawn_running.load() || registers->rax < 0x10000 ||
        RespawnsInWorldRespawn(world_respawn_kind.load(), ReadRespawnCandidate(registers->rbx),
                               WasAliveThisVisit(registers->rbx))) {
        return;
    }
    registers->rax = reinterpret_cast<u64>(no_respawn_param_holder.data());
}

void PS4_SYSV_ABI SkipEventRestartForPartyRest(u64, GuestRegisterSnapshot* registers) {
    const u64 world = registers->rax;
    if (!party_rest_respawn_pending.load() || world < 0x10000 ||
        ReadValue<s32>(world, WorldRespawnStateOffset) != 3) {
        return;
    }
    party_rest_respawn_pending.store(false);
    party_rest_respawn_running.store(false);
    WriteValue(world, WorldRespawnStateOffset, s32{4});
    LOG_INFO(Debug, "Bloodborne party rest: kept the event scripts running (no restart)");
}

void PS4_SYSV_ABI LetPartyUseLamps(u64, GuestRegisterSnapshot* registers) {
    // Diagnostics: whether this player's lamp talk scripts ask at all (once, then every 3000).
    static std::atomic<u64> asked{};
    const u64 count = asked.fetch_add(1) + 1;
    if (count == 1 || count % 3000 == 0) {
        LOG_INFO(Debug, "Bloodborne lamp talk asked 'in multiplayer' {} times (party session={})",
                 count, InPartySession());
    }
    if (lamp_rest_hooks_installed && InPartySession()) {
        registers->rax &= ~u64{0xFF};
        registers->rcx = 0;
    }
}

} // namespace

namespace Coop {

// A party guest's lamp trip (see GuestMakesItsOwnLampTrip): the touch animation, then the warp,
// both from the party tick.
s32 guest_trip_point = HuntersDreamWarpPoint;
// A guest's own warp into the host's map ended in a grey void (no position at arrival); a second
// after arrival the guest is moved to the lamp the way a revive is.
bool guest_trip_in_flight{};

} // namespace Coop

namespace {

std::optional<std::chrono::steady_clock::time_point> guest_trip_fixup_at;
std::optional<std::chrono::steady_clock::time_point> guest_lamp_touch_at;
// The lamp the trip in flight was placed at (see ArrivalNeedsMove), if any.
const LampPoint* guest_trip_planned{};

} // namespace

namespace Coop {

std::optional<std::chrono::steady_clock::time_point> guest_lamp_trip_at;

} // namespace Coop

namespace {

void TickGuestLampTrip() {
    const auto now = std::chrono::steady_clock::now();
    if (guest_trip_fixup_at && now >= *guest_trip_fixup_at) {
        guest_trip_fixup_at.reset();
        const auto at = LocalPlayerPosition();
        LOG_INFO(Debug,
                 "Bloodborne party guest trip: position after arrival known={} ({}, {}, {}) "
                 "planned at the lamp={}",
                 at.has_value(), at ? (*at)[0] : 0.0f, at ? (*at)[1] : 0.0f, at ? (*at)[2] : 0.0f,
                 guest_trip_planned != nullptr);
        if (ArrivalNeedsMove(guest_trip_planned, at)) {
            MoveToRespawnPoint(true);
        }
        // The summon warp byte the lamp's placement set is spent (the pre-match warp waits on it).
        if (const u64 global = GetGlobalState();
            guest_trip_planned && global >= 0x10000 && ReadValue<u8>(global, 0x1520) != 0) {
            LOG_INFO(Debug, "Bloodborne party guest trip: summon warp byte still set; cleared");
            WriteValue(global, 0x1520, u8{0});
        }
        guest_trip_planned = nullptr;
    }
    if (guest_lamp_touch_at && now >= *guest_lamp_touch_at) {
        guest_lamp_touch_at.reset();
        using ForcePlayAnimation = bool PS4_SYSV_ABI (*)(u64 unused, s32 entity, s32 animation);
        reinterpret_cast<ForcePlayAnimation>(image_base + ForcePlayAnimationOffset)(
            0, LocalPlayerEntity, LampTouchAnimation);
    }
    if (guest_lamp_trip_at && now >= *guest_lamp_trip_at) {
        guest_lamp_trip_at.reset();
        guest_trip_planned = StartTripToLamp(guest_trip_point);
        guest_trip_in_flight = true;
    }
}

// Whether the character with entity id `entity` is switched off in this game (none: not loaded).
std::optional<bool> LampCharacterOff(s32 entity) {
    const u64 world = ReadValue<u64>(image_base + CandidateLocalStatePointerOffset, 0);
    if (world < 0x10000) {
        return std::nullopt;
    }
    for (const auto& [character, id] : CharacterEntityIds(world)) {
        if (id == entity) {
            return IsCharacterDisabled(character);
        }
    }
    return std::nullopt;
}

// Kaniel 2026-10-25 (travel together): a lamp's or headstone's trip the team's question takes over
// (the game's own trip is not made then).
bool TeamQuestionTakesTrip(s32 flag) {
    const bool lamp = IsLampTravelFlag(flag);
    if (!lamp_rest_hooks_installed || (!lamp && !IsHeadstoneTravelFlag(flag)) ||
        !OfferTeamTravel(lamp ? HuntersDreamWarpPoint : HeadstoneWarpPoint(flag))) {
        return false;
    }
    if (lamp) {
        lamp_uses.fetch_add(1); // a lamp touched (the Rot rule's cure)
    }
    LOG_INFO(Debug, "Bloodborne party: trip flag {} waits for the team question", flag);
    return true;
}

void PS4_SYSV_ABI RestInsteadOfLampTravel(u64, GuestRegisterSnapshot* registers) {
    const s32 flag = static_cast<s32>(registers->r15);
    // A ritual altar's talk runs in every game: only the talker's game sets its warp.
    if (party_controller && esd_talker >= 0x10000 && esd_talker != GetLocalPlayer() &&
        SkipsOthersAltarWarp(flag,
                             std::ranges::any_of(OtherPlayers(), [](const OtherPlayer& other) {
                                 return other.character == esd_talker;
                             }))) {
        registers->rax = reinterpret_cast<u64>(no_talk_flag_range.data());
        static std::atomic<u32> logged{};
        if (logged.fetch_add(1) < 20) {
            LOG_INFO(Debug, "Bloodborne party: another player's altar talk sets flag {}; not here",
                     flag);
        }
        return;
    }
    // A lamp whose boss lives (its character still off here) is not lit: event 7100 would run on
    // the request, light nothing and end (Kaniel's playtest 2026-10-20, the Witch's Abode).
    if (party_controller && InPartySession()) {
        if (const auto lamp = LampOfLightRequest(flag);
            lamp && RefusesLampLight(flag, LampCharacterOff(lamp->character))) {
            registers->rax = reinterpret_cast<u64>(no_talk_flag_range.data());
            LOG_INFO(Debug,
                     "Bloodborne party: lamp {} is not lit yet (its boss lives); request {} "
                     "refused",
                     lamp->character, flag);
            return;
        }
    }
    const bool guest =
        party_controller && party_controller->SearchEffectId() == SmallResonantBellEffect;
    if (flag >= 70000000 && flag < 80000000 && guest) {
        LOG_INFO(Debug, "Bloodborne party guest talk sets flag {} (party session={})", flag,
                 InPartySession()); // diagnostics
    }
    // An NPC's gift (common event 9100 ends for a phantom): the guest's own talk earns it.
    if (GivesGuestNpcGift(InPartySession(), guest, flag)) {
        QueueGuestNpcGift(*NpcGiftFor(flag));
    }
    // Event 12107000 ends for a client: a party guest's headstone trip is ours to make. Not gated
    // on the white-phantom type: in the host's Dream that check stopped the trip (the type is
    // logged to find out why).
    if (TeamQuestionTakesTrip(flag)) {
        registers->rax = reinterpret_cast<u64>(no_talk_flag_range.data());
        return;
    }
    if (lamp_rest_hooks_installed && IsHeadstoneTravelFlag(flag) && InPartySession() &&
        party_controller->SearchEffectId() == SmallResonantBellEffect) {
        const auto now = std::chrono::steady_clock::now();
        guest_trip_point = HeadstoneWarpPoint(flag);
        guest_lamp_touch_at = now + std::chrono::milliseconds{500};
        guest_lamp_trip_at = now + std::chrono::seconds{3};
        const u64 player = GetLocalPlayer();
        LOG_INFO(Debug, "Bloodborne party guest travels by headstone {} to {} (type {})", flag,
                 guest_trip_point, player >= 0x10000 ? ReadValue<s32>(player, 0x78) : -1);
        return;
    }
    if (!lamp_rest_hooks_installed || !IsLampTravelFlag(flag)) {
        return;
    }
    lamp_uses.fetch_add(1);
    if (!InPartySession()) {
        LOG_INFO(Debug, "Bloodborne lamp travel {} outside a party session", flag);
        return;
    }
    if (!lamp_rests_enabled) {
        LOG_INFO(Debug, "Bloodborne party member travels from lamp {} (party rests off)", flag);
        if (GuestMakesItsOwnLampTrip(true,
                                     party_controller->SearchEffectId() == SmallResonantBellEffect
                                         ? PartyRole::Guest
                                         : PartyRole::Host)) {
            const auto now = std::chrono::steady_clock::now();
            guest_lamp_touch_at = now + std::chrono::milliseconds{500};
            guest_lamp_trip_at = now + std::chrono::seconds{3};
            guest_trip_point = HuntersDreamWarpPoint;
        }
        return;
    }
    const auto position = LocalPlayerPosition();
    LampUse use;
    {
        std::scoped_lock lock{lamp_rest_mutex};
        use = lamp_rest.Use(flag, position.value_or(LampPosition{}));
    }
    if (use == LampUse::Travel) {
        // The game's own trip (common event 7200), for this player only.
        LOG_INFO(Debug, "Bloodborne party member travels from lamp {}", flag);
        return;
    }
    registers->rax = reinterpret_cast<u64>(no_talk_flag_range.data());
    // Played from TickLampRest once the talk has closed; started here, the closing talk cancels
    // it.
    lamp_touch_animation_at = std::chrono::steady_clock::now() + std::chrono::milliseconds{500};
    LOG_INFO(Debug, "Bloodborne party member rests at lamp {} (position known={})", flag,
             position.has_value());
}

// ESD function 14 (`mov rax, [r15+0x18]; test rax, rax`, then `mov eax, [rax+0x78]`): the talking
// player's character type, which the lamp scripts (and others) require to be 0 for their prompt.
// For a party guest the hook points r15 at a stand-in talk whose player has type 0; r15 is the
// dispatcher's callee-saved register, restored on its way out.
constexpr u64 EsdTalkerTypeOffset = 0x0203B7FB;
constexpr std::array<u8, 7> EsdTalkerType{0x49, 0x8B, 0x47, 0x18, 0x48, 0x85, 0xC0};
alignas(16) std::array<u8, 0x100> host_like_talker{};
alignas(16) std::array<u64, 8> host_like_talk{};

// A talk object marks itself "phantom talker" (+0x25, what ESD function 19 answers) when it starts
// and the local player's type is neither live (0) nor grey (8): `mov eax, [rax+0x78]; or eax, 8`
// with rax = the player. For a party guest the hook points rax at a stand-in of type 0.
constexpr u64 TalkPhantomCheckOffset = 0x0203AC04;
constexpr std::array<u8, 6> TalkPhantomCheck{0x8B, 0x40, 0x78, 0x83, 0xC8, 0x08};

void PS4_SYSV_ABI LetPartyGuestsStartTalksLikeTheHost(u64, GuestRegisterSnapshot* registers) {
    const u64 player = registers->rax;
    const s32 type = player >= 0x10000 && HasMemoryAccess(player, 0x7C, MemoryProt::CpuRead)
                         ? ReadValue<s32>(player, 0x78)
                         : -1;
    if (lamp_rest_hooks_installed && TalksAsHost(InPartySession(), type)) {
        registers->rax = reinterpret_cast<u64>(host_like_talker.data());
    }
}

void PS4_SYSV_ABI LetPartyGuestsTalkLikeTheHost(u64, GuestRegisterSnapshot* registers) {
    const u64 player = GetLocalPlayer();
    const s32 type = player >= 0x10000 && HasMemoryAccess(player, 0x7C, MemoryProt::CpuRead)
                         ? ReadValue<s32>(player, 0x78)
                         : -1;
    if (lamp_rest_hooks_installed && TalksAsHost(InPartySession(), type)) {
        registers->r15 = reinterpret_cast<u64>(host_like_talk.data());
    }
}

// Diagnostics for a guest's missing lamp prompt: which talk functions the scripts ask and what
// they answer (function id from the dispatcher's switch at 0x0203AD95, result at its common
// exit 0x0203EECF), logged every 5 s while a party guest is in a session.
constexpr u64 EsdFunctionIdOffset = 0x0203AD95;
constexpr std::array<u8, 5> EsdFunctionId{0x89, 0xC1, 0x83, 0xF8, 0x3E};
constexpr u64 EsdFunctionResultOffset = 0x0203EECF;
constexpr std::array<u8, 9> EsdFunctionResult{0x41, 0xC7, 0x44, 0x24, 0x08, 0x02, 0x00, 0x00, 0x00};
// Talk commands (dispatcher 0x0203FFD0, command id in eax at 0x02040002) and the dialog answers
// (ESD functions 21/22 leave through 0x0203EBCB with the result in ecx) go to the same log,
// commands as "c<id>".
constexpr u64 TalkCommandIdOffset = 0x02040002;
constexpr std::array<u8, 5> TalkCommandId{0x89, 0xC1, 0x83, 0xF8, 0x57};
constexpr u64 EsdDialogResultOffset = 0x0203EBCB;
thread_local s32 esd_function_asked = -1;
std::mutex esd_answers_mutex;
std::map<std::pair<s32, s32>, u64> esd_answers;
std::atomic<bool> esd_answers_wanted{};

void PS4_SYSV_ABI NoteEsdFunction(u64, GuestRegisterSnapshot* registers) {
    esd_function_asked = static_cast<s32>(registers->rax);
    const u64 talk = registers->r15;
    esd_talker = talk >= 0x10000 && HasMemoryAccess(talk, 0x20, MemoryProt::CpuRead)
                     ? ReadValue<u64>(talk, 0x18)
                     : 0;
}

// ESD function 26 (`movzx eax, byte [r15 + 0x30]`, a flag of the talk) answered 1 for a party
// guest, and the lamp scripts take their prompt only on 0 (with f14, f19, f27 and f31): a guest
// could use no lamp (playtest 2026-10-13, rig guest-lamp-rest). A party guest in its session gets
// the host's 0.
constexpr s32 EsdTalkFlag30 = 26;

void PS4_SYSV_ABI NoteEsdAnswer(u64, GuestRegisterSnapshot* registers) {
    if (esd_function_asked == EsdTalkFlag30 && (registers->rax & 0xFF) != 0 &&
        lamp_rest_hooks_installed && IsPartyGuestRole() && InPartySession()) {
        static std::atomic<bool> logged{};
        if (!logged.exchange(true)) {
            LOG_INFO(Debug, "Bloodborne party guest talk: ESD function 26 answered 1; 0 as the "
                            "host's");
        }
        registers->rax &= ~u64{0xFF};
    }
    if (!esd_answers_wanted.load(std::memory_order_relaxed)) {
        return;
    }
    std::scoped_lock lock{esd_answers_mutex};
    ++esd_answers[{esd_function_asked, static_cast<s32>(registers->rax)}];
}

void PS4_SYSV_ABI NoteTalkCommand(u64, GuestRegisterSnapshot* registers) {
    if (!esd_answers_wanted.load(std::memory_order_relaxed)) {
        return;
    }
    std::scoped_lock lock{esd_answers_mutex};
    ++esd_answers[{1000 + static_cast<s32>(registers->rax), 0}];
}

void PS4_SYSV_ABI NoteEsdDialogAnswer(u64, GuestRegisterSnapshot* registers) {
    if (!esd_answers_wanted.load(std::memory_order_relaxed)) {
        return;
    }
    std::scoped_lock lock{esd_answers_mutex};
    ++esd_answers[{esd_function_asked, static_cast<s32>(registers->rcx)}];
}

// Where a talk is: a map's talk update (0x0204C0B0) weighs each enabled talk every frame; at
// 0x0204C63B (`mov rax, [rcx+r13*8+0x28]`, before the reach check) rcx + r13 * 8 is its entry
// (+0x10 the talk, its ESD id at +0x10; +0x20 the character's entity) and r14 the character.
// A hook there found Gilbert's window (talk 241319 at (-199.2, -28.6, 63.3), 3 m from lamp
// 241001) but runs for every talk every frame, and joins failed far more often while it was on
// (rig 2026-10-10: 4 of 19, against 1 in 60): install one only to look a talk up.

void LogEsdAnswers(bool party_guest) {
    static std::chrono::steady_clock::time_point last{};
    esd_answers_wanted.store(party_guest, std::memory_order_relaxed);
    const auto now = std::chrono::steady_clock::now();
    if (!party_guest || now - last < std::chrono::seconds{5}) {
        return;
    }
    last = now;
    std::string text;
    {
        std::scoped_lock lock{esd_answers_mutex};
        for (const auto& [key, count] : esd_answers) {
            text += key.first >= 1000 ? fmt::format("c{}x{} ", key.first - 1000, count)
                                      : fmt::format("f{}={}x{} ", key.first, key.second, count);
        }
        esd_answers.clear();
    }
    if (!text.empty()) {
        LOG_INFO(Debug, "Bloodborne guest talk answers: {}", text);
    }
}

} // namespace

namespace Coop {

// Both or neither: lamps that work in a session but still travel would take one player away.
void InstallLampsAsCampfires(u64 base, u64 image_size) {
    if (!party_controller) {
        return;
    }
    const auto starts_with = [base](u64 offset, std::span<const u8> bytes) {
        return std::ranges::equal(
            std::span<const u8>{reinterpret_cast<const u8*>(base + offset), bytes.size()}, bytes);
    };
    if (!starts_with(RestockItemClampOffset, RestockItemClampPrologue) ||
        !starts_with(RestockFromStorageOffset,
                     std::span<const u8>{StandardR15Prologue.data(), 6}) ||
        !starts_with(BonfireRegeneOffset, std::span<const u8>{StandardR15Prologue.data(), 6})) {
        LOG_ERROR(Debug, "Bloodborne rest functions not found; lamps stay locked in a session");
        coop_features.Skipped("lamps", FeatureMismatch(base, image_size, "lamps"));
        return;
    }
    if (EsdIsMultiplayerResultOffset + EsdIsMultiplayerResult.size() > image_size ||
        TalkSetEventFlagRangeOffset + TalkSetEventFlagRange.size() > image_size ||
        !InstallCoopHook(reinterpret_cast<void*>(base + TalkSetEventFlagRangeOffset),
                         TalkSetEventFlagRange, 0, RestInsteadOfLampTravel)) {
        LOG_ERROR(Debug, "Bloodborne lamp rest hook unavailable; lamps stay locked in a session");
        coop_features.Skipped("lamps", SiteFailure(base, image_size, TalkSetEventFlagRangeOffset));
        return;
    }
    if (!InstallCoopHook(reinterpret_cast<void*>(base + EsdIsMultiplayerResultOffset),
                         EsdIsMultiplayerResult, 0, LetPartyUseLamps)) {
        LOG_ERROR(Debug, "Bloodborne lamp multiplayer hook unavailable; lamps stay locked in a "
                         "session");
        coop_features.Skipped("lamps", SiteFailure(base, image_size, EsdIsMultiplayerResultOffset));
        return;
    }
    if (!InstallCoopHook(reinterpret_cast<void*>(base + EventRestartStateOffset), EventRestartState,
                         0, SkipEventRestartForPartyRest)) {
        LOG_ERROR(Debug, "Bloodborne party rest would restart the event scripts; lamps stay "
                         "locked in a session");
        coop_features.Skipped("lamps", SiteFailure(base, image_size, EventRestartStateOffset));
        return;
    }
    const u32 no_respawn = 0x2000000;
    std::memcpy(no_respawn_param.data() + 0x145, &no_respawn, sizeof(no_respawn));
    no_respawn_param_holder[1] = reinterpret_cast<u64>(no_respawn_param.data());
    for (const u64 offset : RespawnParamChecks) {
        if (!InstallCoopHook(reinterpret_cast<void*>(base + offset), RespawnParamCheck, 0,
                             RespawnOnlyTheDead)) {
            LOG_ERROR(Debug, "Bloodborne party rest would respawn the living; lamps stay locked "
                             "in a session");
            coop_features.Skipped("lamps", SiteFailure(base, image_size, offset));
            return;
        }
    }
    coop_features.On("lamps");
    struct TalkProbe {
        u64 offset;
        std::span<const u8> bytes;
        GuestRegisterEditHook hook;
    };
    const std::array<TalkProbe, 4> probes{{
        {EsdFunctionIdOffset, EsdFunctionId, NoteEsdFunction},
        {EsdFunctionResultOffset, EsdFunctionResult, NoteEsdAnswer},
        {EsdDialogResultOffset, EsdFunctionResult, NoteEsdDialogAnswer},
        {TalkCommandIdOffset, TalkCommandId, NoteTalkCommand},
    }};
    const auto probe_failed = std::ranges::find_if(probes, [base](const TalkProbe& probe) {
        return !InstallCoopHook(reinterpret_cast<void*>(base + probe.offset), probe.bytes, 0,
                                probe.hook);
    });
    if (probe_failed != probes.end()) {
        LOG_ERROR(Debug, "Bloodborne talk diagnostics unavailable");
        coop_features.Skipped("talk diagnostics",
                              SiteFailure(base, image_size, probe_failed->offset));
    } else {
        coop_features.On("talk diagnostics");
    }
    if (!InstallCoopHook(reinterpret_cast<void*>(base + TalkPhantomCheckOffset), TalkPhantomCheck,
                         0, LetPartyGuestsStartTalksLikeTheHost)) {
        LOG_ERROR(Debug, "Bloodborne talk phantom hook unavailable; party guests get no lamps");
        coop_features.Skipped("talk phantom",
                              SiteFailure(base, image_size, TalkPhantomCheckOffset));
    } else {
        coop_features.On("talk phantom");
    }
    host_like_talk[3] = reinterpret_cast<u64>(host_like_talker.data()); // +0x18
    if (EsdTalkerTypeOffset + EsdTalkerType.size() > image_size ||
        !InstallCoopHook(reinterpret_cast<void*>(base + EsdTalkerTypeOffset), EsdTalkerType, 0,
                         LetPartyGuestsTalkLikeTheHost)) {
        LOG_ERROR(Debug, "Bloodborne talk hook unavailable; party guests get no lamp prompt");
        coop_features.Skipped("talk host", SiteFailure(base, image_size, EsdTalkerTypeOffset));
    } else {
        coop_features.On("talk host");
    }
    lamp_rest_hooks_installed = true;
    LOG_INFO(Debug, "Bloodborne lamps as campfires: a party rests at its lamps together");
}

} // namespace Coop

namespace {

std::vector<LampRest> FreshRestBoard() {
    std::scoped_lock lock{rest_board_mutex};
    if (std::chrono::steady_clock::now() - rest_board_time > std::chrono::seconds{3}) {
        return {};
    }
    return rest_board;
}
std::vector<u64> hidden_characters;

// Diagnostics: what the respawn changes on each character besides its HP, to find how the event
// scripts hide characters that the disable bit does not cover.
struct CharacterState {
    u64 character;
    u32 handle;        // +0x08
    u8 control_flags;  // [+0x18]+0x20
    s32 control_state; // [+0x18]+0x10
    u64 state_flags;   // +0x1E0
    s32 hp;
    bool operator==(const CharacterState&) const = default;
};
std::vector<CharacterState> characters_before_rest;
// A copy for the delayed comparison (LogCharactersActivatedByRest), 5 s after the respawn.
std::vector<CharacterState> characters_at_rest;
CharacterState ReadCharacterState(u64 character);
std::optional<std::chrono::steady_clock::time_point> compare_activation_at;

bool IsCharacterActive(const CharacterState& state) {
    return (state.control_flags & 1) == 0 && state.control_state == 4 && state.hp > 0;
}

// Diagnostics: characters that were inactive or hidden but alive before the rest and are active
// a few seconds after it, with their position and the words around +0x28..+0x60 to name them.
void LogCharactersActivatedByRest() {
    s32 logged = 0;
    for (const auto& before : characters_at_rest) {
        if (IsCharacterActive(before) || before.hp <= 0 ||
            !HasMemoryAccess(before.character, 0x3B8, MemoryProt::CpuRead) ||
            ReadValue<u32>(before.character, 0x08) != before.handle) {
            continue;
        }
        const auto now = ReadCharacterState(before.character);
        if (!IsCharacterActive(now) || ++logged > 30) {
            continue;
        }
        const u64 modules = ReadValue<u64>(before.character, 0x3B0);
        const u64 physics = modules >= 0x10000 ? ReadValue<u64>(modules, 0x68) : 0;
        std::string words;
        for (u64 offset = 0x20; offset < 0x80; offset += 4) {
            words += fmt::format(" {:x}", ReadValue<u32>(before.character, offset));
        }
        LOG_INFO(Debug,
                 "Bloodborne rest activated character {:#x} handle {:#x}: control {:#x}/{} "
                 "flags {:#x} hp {} at ({}, {}, {}) words{}",
                 before.character, before.handle, before.control_flags, before.control_state,
                 before.state_flags, before.hp,
                 physics >= 0x10000 ? ReadValue<float>(physics, 0x1E0) : 0.0f,
                 physics >= 0x10000 ? ReadValue<float>(physics, 0x1E4) : 0.0f,
                 physics >= 0x10000 ? ReadValue<float>(physics, 0x1E8) : 0.0f, words);
    }
    LOG_INFO(Debug, "Bloodborne rest: {} inactive characters became active", logged);
    characters_at_rest.clear();
}

CharacterState ReadCharacterState(u64 character) {
    const u64 control = ReadValue<u64>(character, 0x18);
    return CharacterState{
        .character = character,
        .handle = ReadValue<u32>(character, 0x08),
        .control_flags = control >= 0x10000 ? ReadValue<u8>(control, 0x20) : u8{0xFF},
        .control_state = control >= 0x10000 ? ReadValue<s32>(control, 0x10) : -1,
        .state_flags = ReadValue<u64>(character, 0x1E0),
        .hp = CharacterHp(character),
    };
}

// Puts back the script-set state flags the respawn cleared (see RestoreScriptFlagsAfterRespawn).
void RestoreScriptFlags() {
    s32 restored = 0;
    for (const auto& before : characters_before_rest) {
        if (!HasMemoryAccess(before.character, 0x3B8, MemoryProt::CpuWrite) ||
            ReadValue<u32>(before.character, 0x08) != before.handle) {
            continue;
        }
        const u64 now = ReadValue<u64>(before.character, 0x1E0);
        const u64 flags = RestoreScriptFlagsAfterRespawn(before.state_flags, now, before.hp);
        if (flags != now) {
            WriteValue(before.character, 0x1E0, flags);
            ++restored;
        }
    }
    LOG_INFO(Debug, "Bloodborne party rest: restored script flags on {} characters", restored);
}

void LogCharacterChanges() {
    s32 logged = 0;
    for (const auto& before : characters_before_rest) {
        if (!HasMemoryAccess(before.character, 0x3B8, MemoryProt::CpuRead)) {
            continue;
        }
        auto after = ReadCharacterState(before.character);
        if (after.handle == before.handle && after.control_flags == before.control_flags &&
            after.control_state == before.control_state &&
            after.state_flags == before.state_flags && (after.hp > 0) == (before.hp > 0)) {
            continue;
        }
        if (++logged > 60) {
            break;
        }
        LOG_INFO(Debug,
                 "Bloodborne rest changed character {:#x} handle {:#x}: control {:#x}/{} -> "
                 "{:#x}/{} flags {:#x} -> {:#x} hp {} -> {}",
                 before.character, before.handle, before.control_flags, before.control_state,
                 after.control_flags, after.control_state, before.state_flags, after.state_flags,
                 before.hp, after.hp);
    }
    // Characters that only exist after the respawn (a generator spawning, for instance).
    const u64 world = ReadValue<u64>(image_base + CandidateLocalStatePointerOffset, 0);
    s32 added = 0;
    if (world >= 0x10000) {
        ForEachWorldCharacter(world, [&](u64 character) {
            if (std::ranges::any_of(characters_before_rest, [&](const CharacterState& before) {
                    return before.character == character;
                })) {
                return;
            }
            const auto state = ReadCharacterState(character);
            ++added;
            LOG_INFO(Debug,
                     "Bloodborne rest added character {:#x} handle {:#x} control {:#x}/{} "
                     "flags {:#x} hp {}",
                     character, state.handle, state.control_flags, state.control_state,
                     state.state_flags, state.hp);
        });
    }
    // WorldChrMan +0xA40: the other list the respawn walks (0x018EB900 on each entry).
    const u64 others = world >= 0x10000 ? ReadValue<u64>(world, 0xA40) : 0;
    LOG_INFO(Debug, "Bloodborne rest: {} of {} characters changed, {} added, {} in +0xA40", logged,
             characters_before_rest.size(), added,
             others >= 0x10000 ? ReadValue<s32>(others, 8) : -1);
    characters_before_rest.clear();
}

// The hidden characters stay remembered while the player stays in the same map: one that showed
// up after an earlier rest and was killed is dead now, not hidden, and the next respawn would
// bring it back (Kaniel saw exactly that with the window monster).
u32 hidden_characters_map = 0;

void RememberHiddenCharacters(u64 world) {
    if (const u32 map = CurrentPlace().map; map != hidden_characters_map) {
        hidden_characters.clear();
        hidden_characters_map = map;
    }
    characters_before_rest.clear();
    ForEachWorldCharacter(world, [](u64 character) {
        characters_before_rest.push_back(ReadCharacterState(character));
    });
    s32 disabled = 0;
    ForEachWorldCharacter(world, [&](u64 character) {
        const bool off = IsCharacterDisabled(character);
        disabled += off ? 1 : 0;
        if (KeepDisabledAfterRespawn(off, CharacterHp(character)) &&
            !std::ranges::contains(hidden_characters, character)) {
            hidden_characters.push_back(character);
        }
    });
    LOG_INFO(Debug, "Bloodborne party rest: {} characters disabled, {} of them hidden alive",
             disabled, hidden_characters.size());
}

void HideCharactersAgain() {
    characters_at_rest = characters_before_rest;
    compare_activation_at = std::chrono::steady_clock::now() + std::chrono::seconds{5};
    RestoreScriptFlags();
    LogCharacterChanges();
    s32 hidden = 0;
    // Only characters still in the world's blocks: the list outlives a rest, and a stale pointer
    // handed to the game's disable would crash it.
    std::unordered_set<u64> current;
    if (const u64 world = ReadValue<u64>(image_base + CandidateLocalStatePointerOffset, 0);
        world >= 0x10000) {
        ForEachWorldCharacter(world, [&](u64 character) { current.insert(character); });
    }
    for (const u64 character : hidden_characters) {
        if (current.contains(character) && IsCharacter(character, "HideCharactersAgain") &&
            !IsCharacterDisabled(character)) {
            reinterpret_cast<void PS4_SYSV_ABI (*)(u64, bool)>(
                image_base + SetCharacterDisabledOffset)(character, true);
            ++hidden;
        }
    }
    if (!hidden_characters.empty()) {
        LOG_INFO(Debug, "Bloodborne party rest: hid {} of {} characters the respawn re-enabled",
                 hidden, hidden_characters.size());
    }
}

// Does what a reload at the lamp would (see RefreshAtLamp) and asks WorldChrMan for the bonfire
// respawn (see WorldRespawnStateOffset).
void RefreshWorldAtLamp(s32 lamp, s32 players) {
    static s32 respawn_before;
    respawn_before = -1;
    const LampRestCalls calls{
        .request_full_recover =
            [] {
                reinterpret_cast<bool PS4_SYSV_ABI (*)()>(image_base + RequestFullRecoverOffset)();
                if (const u64 player = GetLocalPlayer(); IsCharacter(player, "BonfireRegene")) {
                    reinterpret_cast<void PS4_SYSV_ABI (*)(u64, s32)>(
                        image_base + BonfireRegeneOffset)(player, 0);
                }
            },
        .clamp_restock_item =
            [] {
                reinterpret_cast<void PS4_SYSV_ABI (*)()>(image_base + RestockItemClampOffset)();
            },
        .restock_from_storage =
            [] {
                reinterpret_cast<void PS4_SYSV_ABI (*)()>(image_base + RestockFromStorageOffset)();
            },
        .request_world_respawn =
            [] {
                const u64 world = ReadValue<u64>(image_base + CandidateLocalStatePointerOffset, 0);
                respawn_before =
                    world >= 0x10000 ? ReadValue<s32>(world, WorldRespawnStateOffset) : -1;
                if (respawn_before == 0) {
                    world_respawn_kind.store(WorldRespawnKind::PartyRest);
                    RememberHiddenCharacters(world);
                    party_rest_respawn_pending.store(true);
                    party_rest_respawn_running.store(true);
                    WriteValue(world, WorldRespawnStateOffset, s32{1});
                }
            },
    };
    const bool refreshed = RefreshAtLamp(calls);
    LOG_INFO(Debug,
             "Bloodborne party of {} rested at lamp {}: refreshed={} (healed, restocked) world "
             "respawn state {} -> {}",
             players, lamp, refreshed, respawn_before, respawn_before == 0 ? 1 : respawn_before);
}

void LogWorldRespawnChanges() {
    static s32 last = 0;
    const u64 world = ReadValue<u64>(image_base + CandidateLocalStatePointerOffset, 0);
    const s32 state = world >= 0x10000 ? ReadValue<s32>(world, WorldRespawnStateOffset) : 0;
    if (state != last) {
        LOG_INFO(Debug, "Bloodborne world respawn state {} -> {}", last, state);
        if (state == 0) {
            HideCharactersAgain();
            brought_back_handles.clear();
        }
        last = state;
    }
}

void SampleLivingCharacters() {
    static std::chrono::steady_clock::time_point next{};
    const auto now = std::chrono::steady_clock::now();
    const u64 world = ReadValue<u64>(image_base + CandidateLocalStatePointerOffset, 0);
    if (now < next || world < 0x10000) {
        return;
    }
    next = now + std::chrono::seconds{1};
    std::scoped_lock lock{seen_alive_mutex};
    if (const u64 visit = CurrentPlace().visit; visit != seen_alive_visit) {
        seen_alive.clear();
        seen_alive_visit = visit;
    }
    ForEachWorldCharacter(world, [](u64 character) {
        if (CharacterHp(character) > 0) {
            seen_alive.insert(character);
        }
    });
}

// See WipeRespawnAfterArrival. The log line names what the save left dead on every member after
// a wipe's trip (none on a member whose own reload brought everything back).
void RespawnWhatTheSaveLeftDead() {
    const u64 world = ReadValue<u64>(image_base + CandidateLocalStatePointerOffset, 0);
    if (!InPartySession() || world < 0x10000) {
        LOG_INFO(Debug, "Bloodborne party wipe: no respawn check (party session={}, world={})",
                 InPartySession(), world >= 0x10000);
        return;
    }
    std::string which;
    s32 count = 0;
    ForEachWorldCharacter(world, [&](u64 character) {
        if (LeftDeadByItsSave(ReadRespawnCandidate(character))) {
            ++count;
            which += fmt::format(" {:#x}", ReadValue<u32>(character, 0x08));
        }
    });
    const s32 state = ReadValue<s32>(world, WorldRespawnStateOffset);
    const bool respawn = count > 0 && state == 0;
    LOG_INFO(Debug,
             "Bloodborne party wipe: {} characters left dead by this member's save on map {:#x}:{} "
             "(world respawn state {}, respawning={})",
             count, CurrentPlace().map, which, state, respawn);
    if (!respawn) {
        return;
    }
    world_respawn_kind.store(WorldRespawnKind::AfterWipe);
    RememberHiddenCharacters(world);
    party_rest_respawn_pending.store(true);
    party_rest_respawn_running.store(true);
    WriteValue(world, WorldRespawnStateOffset, s32{1});
}

// The bonfire respawn's own steps, with party rests on or off (the respawn hooks are installed
// either way): a wipe's respawn when due, and putting back what a respawn re-enabled once it ends.
void TickWorldRespawn() {
    const auto now = std::chrono::steady_clock::now();
    if (wipe_respawn_at && now >= *wipe_respawn_at) {
        wipe_respawn_at.reset();
        RespawnWhatTheSaveLeftDead();
    }
    LogWorldRespawnChanges();
    if (compare_activation_at && now >= *compare_activation_at) {
        compare_activation_at.reset();
        LogCharactersActivatedByRest();
    }
}

// On a trip's arrival: a wipe's trip checks for what the save left dead a little later.
void ScheduleWipeRespawn() {
    if (wipe_trip_respawn_pending.exchange(false)) {
        wipe_respawn_at = std::chrono::steady_clock::now() + WipeRespawnAfterArrival;
    }
}

} // namespace

namespace Coop {

std::atomic<bool> wipe_trip_respawn_pending{};

void BringBackWhatTheHostBroughtBack(std::span<const u32> handles) {
    static std::chrono::steady_clock::time_point last{};
    const auto now = std::chrono::steady_clock::now();
    if (handles.empty() || !lamp_rest_hooks_installed || now - last < BringBackEvery) {
        return;
    }
    const u64 world = ReadValue<u64>(image_base + CandidateLocalStatePointerOffset, 0);
    if (world < 0x10000 || ReadValue<s32>(world, WorldRespawnStateOffset) != 0 ||
        party_rest_respawn_pending.load()) {
        return;
    }
    last = now;
    brought_back_handles = {handles.begin(), handles.end()};
    std::string which;
    for (const u32 handle : handles) {
        which += fmt::format(" {:#x}", handle);
    }
    static u32 logged = 0;
    if (++logged <= 200) {
        LOG_INFO(Debug,
                 "Bloodborne party: {} enemies the host's map generator brought back are dead "
                 "here on map {:#x}; bringing them back:{}",
                 handles.size(), CurrentPlace().map, which);
    }
    world_respawn_kind.store(WorldRespawnKind::BroughtBack);
    RememberHiddenCharacters(world);
    party_rest_respawn_pending.store(true);
    party_rest_respawn_running.store(true);
    WriteValue(world, WorldRespawnStateOffset, s32{1});
}

void TickLampRest() {
    if (!lamp_rest_hooks_installed) {
        return;
    }
    TickWorldRespawn();
    if (!lamp_rests_enabled) {
        return;
    }
    SampleLivingCharacters();
    if (lamp_touch_animation_at && std::chrono::steady_clock::now() >= *lamp_touch_animation_at) {
        lamp_touch_animation_at.reset();
        using ForcePlayAnimation = bool PS4_SYSV_ABI (*)(u64 unused, s32 entity, s32 animation);
        const bool played = reinterpret_cast<ForcePlayAnimation>(
            image_base + ForcePlayAnimationOffset)(0, LocalPlayerEntity, LampTouchAnimation);
        LOG_INFO(Debug, "Bloodborne lamp touch animation {} played={}", LampTouchAnimation, played);
    }
    const bool in_party_session = InPartySession();
    const auto board = FreshRestBoard();
    const auto position = LocalPlayerPosition();
    std::optional<s32> resting;
    std::optional<LampPosition> rested_at;
    {
        std::scoped_lock lock{lamp_rest_mutex};
        if (in_party_session && lamp_rest.JoinNearby(board, position)) {
            LOG_INFO(Debug, "Bloodborne party member joined a rest nearby");
        }
        resting = lamp_rest.Tick(position, in_party_session);
        rested_at = lamp_rest.JoinablePosition();
    }
    static std::optional<s32> last_resting;
    if (resting != last_resting) {
        LOG_INFO(Debug, "Bloodborne party member {} lamp {}", resting ? "rests at" : "left the",
                 resting.value_or(last_resting.value_or(NotResting)));
        last_resting = resting;
    }
    {
        std::scoped_lock lock{rest_board_position_mutex};
        rest_board_position = rested_at;
    }
    rest_board_lamp.store(resting.value_or(NotResting));
    rest_board_active.store(in_party_session);

    const s32 players = SessionPlayers();
    if (lamp_rest_coordinator.Tick(resting, players, board)) {
        RefreshWorldAtLamp(*resting, players);
    }
}

} // namespace Coop

namespace {

constexpr u64 LeaveSessionOffset = 0x015BED10;
std::mutex party_trip_mutex;
PartyTrip party_trip;

} // namespace

namespace Coop {

bool PartyTripActive() {
    std::scoped_lock lock{party_trip_mutex};
    return party_trip.Active(PartyTrip::Clock::now());
}
std::vector<s32> effects_before_trip; // multiplayer effects to put back after the trip

std::atomic<u64> party_trips_started{0}; // ends the visit of KeepEnemyDeathsInStep

} // namespace Coop

namespace {

[[maybe_unused]] const bool party_trips_registered =
    RegisterLifecycleReset("kill visit", {Lifecycle::Trip}, [] { ++party_trips_started; });

// `world_reload`: the world load's StopRequest, a trip only once its load shows (see
// PartyTrip::StartWorldReload); its lifecycle trip waits for that (a border's flips with no load
// must not end the kill visit).
void StartPartyTrip(const GuestRegisterSnapshot* registers, bool world_reload) {
    if (InPartySession()) {
        if (!world_reload) {
            OnLifecycle(Lifecycle::Trip);
        }
        {
            std::scoped_lock lock{party_trip_mutex};
            // Raw: the trip machinery compares the game's own stage (PartyTrip::ObserveStage).
            if (world_reload) {
                party_trip.StartWorldReload(PartyTrip::Clock::now(), GetCurrentPackedMap(),
                                            GetLocalPlayer());
            } else {
                party_trip.Start(PartyTrip::Clock::now(), GetCurrentPackedMap(), GetLocalPlayer());
            }
        }
        SaveRemoteJoinTasks();
        effects_before_trip = LocalEffects();
        ParkEnemyWatch();
        LOG_INFO(Debug, "Bloodborne party trip started map={:#x}", CurrentPlace().map);
        // Who moves the player (a lamp, a death, a time-of-day cutscene, or something else: the
        // rig's host left for the title when its guest's game died apart from it).
        if (registers != nullptr) {
            LogGameCallChainAt("party trip", registers->rsp);
        }
    } else if (!world_reload) {
        std::scoped_lock lock{party_trip_mutex};
        party_trip.StartOutOfSession(PartyTrip::Clock::now(), GetCurrentPackedMap(),
                                     GetLocalPlayer());
    }
}

// A join that lands while a trip begun outside the session is still loading (a lamp's warp as
// the session formed, rig 2026-10-21): that trip is the party's, so its world teardown and leave
// keep the session; what StartPartyTrip keeps for the arrival is taken now.
[[maybe_unused]] const bool trip_on_join_registered =
    RegisterLifecycleReset("trip under way", {Lifecycle::Join}, [] {
        {
            std::scoped_lock lock{party_trip_mutex};
            if (!party_trip.JoinSession(PartyTrip::Clock::now())) {
                return;
            }
        }
        SaveRemoteJoinTasks();
        effects_before_trip = LocalEffects();
        ParkEnemyWatch();
        LOG_INFO(Debug, "Bloodborne party trip: the session formed during a trip, map={:#x}",
                 CurrentPlace().map);
    });

bool PS4_SYSV_ABI RecordPartyTrip(u64, const GuestRegisterSnapshot* registers) {
    StartPartyTrip(registers, false);
    return false;
}

} // namespace

namespace Coop {

// Every LeaveSession while a party trip is under way is skipped (the stage change's own call at
// 0x0193C689 is only one of them), and logged with its caller.
void LogGameCallChainAt(const char* what, u64 rsp) {
    if (image_base == 0) {
        return;
    }
    std::vector<u64> words;
    for (u64 address = rsp; address < rsp + 0x2000; address += 8) {
        if (!HasMemoryAccess(address, 8, MemoryProt::CpuRead)) {
            break;
        }
        words.push_back(*reinterpret_cast<const u64*>(address));
    }
    LOG_INFO(Debug, "Bloodborne {} from the game:{}", what,
             FormatGameAddresses(words, image_base, MemoryPatcher::g_eboot_image_size, 24));
}

} // namespace Coop

namespace {

bool PS4_SYSV_ABI KeepSessionAfterTrip(u64, const GuestRegisterSnapshot* registers) {
    const u64 return_address = ReadValue<u64>(registers->rsp, 0);
    if (!PartyTripActive() && InPartySession() && return_address >= image_base &&
        IsWorldReloadLeaveSession(return_address - image_base)) {
        // The world load's leave with no StopRequest before it (a guest at the Cathedral Ward
        // border, rig 2026-10-13): the same reload as KeepMatchingDuringTrip's.
        StartPartyTrip(registers, true);
        LOG_INFO(Debug, "Bloodborne party: a world reload's leave is a trip once it loads");
    }
    if (!PartyTripActive()) {
        // Diagnostics: a session that broke right after a boss kill (host to the title screen,
        // a lone guest dropped after lighting the lamp): who leaves. Every world load calls it,
        // so only while in a party session.
        if (party_controller && InPartySession()) {
            LogGameCallChainAt("LeaveSession", registers->rsp);
        }
        return false;
    }
    const u64 caller = ReadValue<u64>(registers->rsp, 0);
    LOG_INFO(Debug, "Bloodborne party trip: skipped LeaveSession from {:#x} map={:#x}",
             caller >= image_base ? caller - image_base : caller, CurrentPlace().map);
    return true;
}

// After this member's trip the other players are re-created as they come to this map (creating a
// player into a map it is not on makes its game leave the session), each once (see PlanRecreate).
bool recreate_players_pending{};
// Pending for RecreateAfterJoin, not a trip: members with no task are left to the game's join.
bool recreate_pending_after_join{};
std::set<std::string> recreated_since_trip;

void LogEsdAnswers(bool party_guest);

void TickGuestLampTrip();

std::filesystem::path GuestResumeFile() {
    return Common::FS::GetUserPath(Common::FS::PathType::UserDir) /
           SlotFileName("resume", character_slot.load());
}

} // namespace

namespace Coop {

// `heading` (radians about the vertical axis) goes in as the rotation's y; whether the player
// turns to it shows in the snapshot's heading.
void TeleportLocalPlayer(const LampPosition& at, float heading) {
    PlaceLocalPlayer({at[0], at[1], at[2], 1.0f}, {0.0f, heading, 0.0f, 0.0f},
                     "TeleportLocalPlayer");
}

} // namespace Coop

namespace {

// See PlacementCalls.
void SetCharacterRotation(u64 character, const float* rotation) {
    const u64 modules = ReadValue<u64>(character, 0x3B0);
    const u64 transform = modules >= 0x10000 ? ReadValue<u64>(modules, 0x68) : 0;
    if (transform >= 0x10000 && HasMemoryAccess(transform, 0x1E0, MemoryProt::CpuWrite)) {
        for (size_t index = 0; index < 4; ++index) {
            WriteValue(transform, 0x1D0 + index * sizeof(float), rotation[index]);
        }
    }
}

} // namespace

namespace Coop {

void SnapCamera() {
    const u64 holder = ReadValue<u64>(image_base + CameraManagerPointerOffset, 0);
    const u64 cameras = holder >= 0x10000 ? ReadValue<u64>(holder, CameraManagerField) : 0;
    if (cameras >= 0x10000 && HasMemoryAccess(cameras, CameraSnapByte + 1, MemoryProt::CpuWrite)) {
        WriteValue(cameras, CameraSnapByte, u8{1});
    }
}

bool PlaceCharacterAt(u64 character, const std::array<float, 4>& position,
                      const std::array<float, 4>& rotation, const char* what) {
    const u64 vtable = IsCharacter(character, what) ? ReadValue<u64>(character, 0) : 0;
    const u64 teleport = vtable >= 0x10000 ? ReadValue<u64>(vtable, 0x5F8) : 0;
    if (teleport < 0x10000) {
        return false;
    }
    using Teleport = void PS4_SYSV_ABI (*)(u64, const float*, const float*, s32);
    using SnapPosition = void PS4_SYSV_ABI (*)(u64, const float*);
    using ResetFollow = void PS4_SYSV_ABI (*)(u64, bool);
    const PlacementCalls calls{
        .teleport = reinterpret_cast<Teleport>(teleport),
        .snap_position = reinterpret_cast<SnapPosition>(image_base + SnapCharacterPositionOffset),
        .set_rotation = SetCharacterRotation,
        .reset_follow = reinterpret_cast<ResetFollow>(image_base + ResetCharacterFollowOffset),
        .snap_camera = SnapCamera,
    };
    alignas(16) const std::array<float, 4> at = position;
    alignas(16) const std::array<float, 4> turn = rotation;
    return PlaceCharacter(calls, character, at.data(), turn.data());
}

bool PlaceLocalPlayer(const std::array<float, 4>& position, const std::array<float, 4>& rotation,
                      const char* what) {
    const u64 player = GetLocalPlayer();
    if (!PlaceCharacterAt(player, position, rotation, what)) {
        return false;
    }
    WriteValue(player, 0x1E1, static_cast<u8>(ReadValue<u8>(player, 0x1E1) | 2));
    return true;
}

// The local player's heading (transform +0x1D4, as the summon placement reads it), radians. A
// teleport turns the player to the rotation it is given, a moment later (a heading written right
// after it does not stay).
std::optional<float> LocalPlayerHeading() {
    const u64 player = GetLocalPlayer();
    const u64 modules = player >= 0x10000 ? ReadValue<u64>(player, 0x3B0) : 0;
    const u64 transform = modules >= 0x10000 ? ReadValue<u64>(modules, 0x68) : 0;
    return transform >= 0x10000 && HasMemoryAccess(transform, 0x1D8, MemoryProt::CpuRead)
               ? std::optional{ReadValue<float>(transform, 0x1D4)}
               : std::nullopt;
}

} // namespace Coop

namespace {

void ResumeWhereTheGuestLeft(bool in_session) {
    enum class Phase { WaitForLoad, Travelling, Recording };
    static Phase phase = Phase::WaitForLoad;
    static std::optional<std::chrono::steady_clock::time_point> stable_since;
    static ResumePoint target;
    static std::chrono::steady_clock::time_point last_saved{};
    static ResumePoint saved;
    static s32 own_progress = 0;
    static u32 loaded_map = 0;                    // where the load the resume is for put the guest
    static std::optional<LampPosition> loaded_at; // and the spot it stood on then
    static std::optional<std::chrono::steady_clock::time_point> loaded_since; // its first ready
    static std::chrono::steady_clock::time_point travel_started{};
    static int for_slot = -1;
    if (!IsPartyGuestRole()) {
        return;
    }
    // Another character (back to the title, another slot): its own load, its own point.
    if (const int slot = character_slot.load(); slot != for_slot) {
        for_slot = slot;
        phase = Phase::WaitForLoad;
        stable_since.reset();
        saved = {};
        loaded_map = 0;
        loaded_at.reset();
        loaded_since.reset();
    }
    const u64 global = GetGlobalState();
    const bool loading = global < 0x10000 || ReadValue<u8>(global, 0x08) != 0;
    // Raw: the point saved pairs it with the position read now (the committed map lags a border
    // crossing, and a lamp trip to the wrong map would end in the void).
    const u32 map = GetCurrentPackedMap();
    const auto at = LocalPlayerPosition();
    const auto now = std::chrono::steady_clock::now();
    // A session that ended under the guest (a kick, a lost connection) sends it home: that load
    // goes back to the last point too, not to its save's position (Yosefka's clinic).
    static bool was_in_session = false;
    if (in_session) {
        was_in_session = true;
    } else if (loading && was_in_session && !PartyTripActive()) {
        was_in_session = false;
        phase = Phase::WaitForLoad;
        loaded_map = 0;
        loaded_at.reset();
        loaded_since.reset();
        LOG_INFO(Debug, "Bloodborne party: guest sent home; it will resume at its last point");
        TraceLife("sent-home");
    }
    const bool ready = !loading && IsUsablePackedMap(map) && at && character_slot.load() >= 0;
    if (!ready) {
        stable_since.reset();
        // A load before the resume (a team join's reload): the spot it puts the guest on counts.
        if (loading && phase == Phase::WaitForLoad && TeamResumeDue()) {
            loaded_map = 0;
            loaded_at.reset();
        }
        return;
    }
    if (!stable_since) {
        stable_since = now;
    }
    if (phase == Phase::WaitForLoad && loaded_map == 0) {
        loaded_map = map;
        loaded_at = *at;
        if (!loaded_since) {
            loaded_since = now;
        }
    }
    const bool settled = now - *stable_since >= std::chrono::seconds{3};
    switch (phase) {
    case Phase::WaitForLoad: {
        if (!settled) {
            return;
        }
        // Kaniel 2026-10-18: a team's load puts the guest beside the team's host (its world may
        // still be loading: its place is waited for a while).
        const bool team_load = TeamResumeDue();
        const s64 since_load_ms =
            loaded_since
                ? std::chrono::duration_cast<std::chrono::milliseconds>(now - *loaded_since).count()
                : 0;
        if (WaitsForTeamSession(team_load, in_session, since_load_ms)) {
            return;
        }
        std::optional<ResumePoint> host_place;
        if (team_load) {
            std::scoped_lock lock{vitals_mutex};
            host_place = HostPlaceOf(vitals_board.members, TeamStartHost());
        }
        const s64 waited_ms =
            std::chrono::duration_cast<std::chrono::milliseconds>(now - *stable_since).count() -
            3'000;
        if (WaitsForHostPlace(team_load, host_place.has_value(), waited_ms)) {
            return;
        }
        if (team_load) {
            LOG_INFO(Debug, "Bloodborne party: team load: the host's place is {}",
                     host_place ? fmt::format("map {:#x} ({}, {}, {})", host_place->map,
                                              host_place->position[0], host_place->position[1],
                                              host_place->position[2])
                                : "not known; the guest's own last place");
        }
        std::ifstream file{GuestResumeFile()};
        const std::string text{std::istreambuf_iterator<char>{file}, {}};
        // Phase 7: the server's point first (a team join puts the guest in a session at once).
        // A team load's file is the guest's own from before this session: it counts.
        const auto point = PickResumePoint(in_session && !team_load, ParseResumePoint(text),
                                           TakeServerResumePoint(), host_place);
        // An unknown count (0) is no other character (see IsOtherCharacter).
        own_progress = in_session ? 0 : CountSetFlags().value_or(0);
        const ResumeStep step =
            point ? ChooseResumeStep(*point, map, *at, own_progress, loaded_map, loaded_at)
                  : ResumeStep{};
        phase = Phase::Recording;
        if (point && map != loaded_map) {
            LOG_INFO(Debug, "Bloodborne party: guest travelled on from map {:#x}; no resume",
                     loaded_map);
        }
        if (step.kind == ResumeStep::Kind::Forget) {
            std::error_code error;
            std::filesystem::remove(GuestResumeFile(), error);
            LOG_INFO(Debug, "Bloodborne party: another character in save slot {}; no resume",
                     character_slot.load());
        } else if (step.kind == ResumeStep::Kind::Move) {
            TeleportLocalPlayer(point->position);
            LOG_INFO(Debug, "Bloodborne party: guest resumed at ({}, {}, {}) on map {:#x}",
                     point->position[0], point->position[1], point->position[2], map);
        } else if (step.kind == ResumeStep::Kind::Travel) {
            target = *point;
            guest_trip_point = step.warp_point;
            guest_lamp_trip_at = now;
            travel_started = now;
            phase = Phase::Travelling;
            LOG_INFO(Debug, "Bloodborne party: guest resumes on map {:#x}; travelling to {}",
                     point->map, step.warp_point);
        }
        if (phase == Phase::Recording) {
            EndTeamResume();
        }
        return;
    }
    case Phase::Travelling:
        if (map == target.map && settled) {
            TeleportLocalPlayer(target.position);
            phase = Phase::Recording;
            LOG_INFO(Debug, "Bloodborne party: guest resumed at ({}, {}, {}) on map {:#x}",
                     target.position[0], target.position[1], target.position[2], map);
            EndTeamResume();
        } else if (now - travel_started > std::chrono::seconds{120}) {
            phase = Phase::Recording;
            EndTeamResume();
            LOG_WARNING(Debug, "Bloodborne party: the resume's trip to map {:#x} never arrived",
                        target.map);
        }
        return;
    case Phase::Recording:
        break;
    }
    if (!settled || now - last_saved < std::chrono::seconds{5}) {
        return;
    }
    last_saved = now;
    // The count of its own world: in a session the flag store is the host's.
    if (!in_session) {
        own_progress = CountSetFlags().value_or(own_progress);
    }
    const ResumePoint point{map, *at, own_progress};
    if (ChooseResumeStep(saved, map, *at, point.progress).kind == ResumeStep::Kind::Stay &&
        saved.map == map) {
        return; // within 5 m of the last one written
    }
    saved = point;
    std::ofstream{GuestResumeFile(), std::ios::trunc} << EncodeResumePoint(point) << '\n';
}

// Insight +1 (common event 9350 applies it once per point, 10 frames apart).
constexpr s32 InsightPointEffect = 4680;

// A guest's insight for finding a boss it saw die in the session (see EncounterInsightWatch):
// its found flag set here (each player keeps its own, KeepsLocalFlag) and one point at a time
// through the player's own add-effect virtual, as 9350 does.
void GiveEncounterInsight(bool in_session) {
    static EncounterInsightWatch watch;
    static s32 points_due = 0;
    static std::chrono::steady_clock::time_point last_point{};
    for (const EncounterInsight& boss :
         watch.Tick(IsPartyGuestRole(), in_session, [](s32 flag) { return EventFlagOn(flag); })) {
        reinterpret_cast<bool PS4_SYSV_ABI (*)(u64, s32, bool)>(
            image_base + PartyGhostSetEventFlagOffset)(0, boss.once_flag, true);
        points_due += boss.amount;
        LOG_INFO(Debug, "Bloodborne party: boss of {} down; {} insight for finding it (flag {})",
                 boss.kill_event, boss.amount, boss.once_flag);
    }
    const auto now = std::chrono::steady_clock::now();
    if (points_due <= 0 || now - last_point < std::chrono::milliseconds{300}) {
        return;
    }
    const u64 player = GetLocalPlayer();
    if (!HasVirtual(player, CharacterAddSpEffectSlot, AddSpEffectOffset, "insight AddSpEffect")) {
        return;
    }
    reinterpret_cast<AddSpEffectFn>(image_base + AddSpEffectOffset)(
        player, InsightPointEffect, player, 0, 1, 0, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 0);
    last_point = now;
    --points_due;
}

// See RechecksShopLines: once per save slot, the messengers' weapon lines a broken event patch
// opened are cleared; the next Dream visit opens them again for the player's own badges.
void RecheckShopLinesOnce(u32 map) {
    const int slot = character_slot.load();
    const auto marker = Common::FS::GetUserPath(Common::FS::PathType::UserDir) /
                        fmt::format("bloodborne_party_shoplines_slot{}.txt", slot);
    static int checked_slot = -2;
    static bool done = false;
    if (slot != checked_slot) {
        checked_slot = slot;
        std::error_code error;
        done = slot < 0 || std::filesystem::exists(marker, error);
    }
    if (!RechecksShopLines(party_controller.has_value(), slot, done, map) ||
        GetLocalPlayer() < 0x10000) {
        return;
    }
    done = true;
    int cleared = 0;
    for (const s32 flag : ShopLineFlags()) {
        if (EventFlagOn(flag)) {
            reinterpret_cast<bool PS4_SYSV_ABI (*)(u64, s32, bool)>(
                image_base + PartyGhostSetEventFlagOffset)(0, flag, false);
            ++cleared;
        }
    }
    std::ofstream{marker} << "rechecked" << std::endl;
    LOG_INFO(Debug,
             "Bloodborne party: cleared {} messenger weapon lines in save slot {}; the next Dream "
             "visit opens them for this character's badges",
             cleared, slot);
}

// See HeldItemFlagFix: every second in a loaded world, each held-item flag follows this player's
// own items, so a save that took the host's value from a team world gets its own badges from the
// bosses, NPCs and maps, and a player who holds one never gets it twice.
// See PhantomKeyItemsToGiveBack: while this guest is a phantom (character type 1) its key item
// list is looked at every 250 ms; once its own list is back (another list, not a phantom), what
// it gained in the phantom's comes back. Its own list is looked at as often out of the join's
// state, and a new phantom's list gets its own key items (OwnKeyItemsForPhantom).
void GiveBackPhantomKeyItems() {
    constexpr s32 PhantomType = 1;
    static u64 phantom_list = 0;
    static std::vector<HeldGoods> first_look;
    static std::vector<HeldGoods> last_look;
    static std::vector<HeldGoods> own_look;
    static s64 looked_ms = 0;
    const u64 player = GetLocalPlayer();
    if (!party_controller || player < 0x10000 || !PlayerItemsReadable()) {
        return;
    }
    const u64 list = KeyItemList();
    if (ReadValue<s32>(player, 0x78) == PhantomType) {
        const s64 now_ms = SteadyMilliseconds();
        if (now_ms - looked_ms < 250) {
            return;
        }
        looked_ms = now_ms;
        last_look = HeldKeyGoods();
        if (list != phantom_list) {
            phantom_list = list;
            first_look = last_look;
            for (const HeldGoods& held : OwnKeyItemsForPhantom(own_look, last_look)) {
                const TradeItem item{BloodborneCoop::TradeItemKind::Goods, held.goods,
                                     static_cast<u32>(held.count)};
                AddTradeItems(std::span{&item, 1}, ItemPopup::Silent);
                LOG_INFO(Debug,
                         "Bloodborne party: {} x goods {} of the guest's own in the phantom's key "
                         "items",
                         held.count, held.goods);
            }
        }
        return;
    }
    if (phantom_list == 0 || list == phantom_list) {
        if (const s64 now_ms = SteadyMilliseconds(); now_ms - looked_ms >= 250) {
            looked_ms = now_ms;
            own_look = HeldKeyGoods();
        }
        return;
    }
    phantom_list = 0;
    for (const HeldGoods& held : PhantomKeyItemsToGiveBack(first_look, last_look, HeldKeyGoods())) {
        const TradeItem item{BloodborneCoop::TradeItemKind::Goods, held.goods,
                             static_cast<u32>(held.count)};
        AddTradeItems(std::span{&item, 1}, ItemPopup::Silent);
        LOG_INFO(Debug,
                 "Bloodborne party: gave back {} x goods {}: gained as a phantom, dropped by "
                 "the game with its key item list",
                 held.count, held.goods);
    }
}

// See LampsToRelight: the lamps this character has lit (cleared for a new character or another
// save slot), and in a session a guest's lamps, its own and the team's world's, lit again when
// they read off (its join's reload gives its game a new flag store with none lit).
std::atomic<bool> kept_lamps_reset{false};
[[maybe_unused]] const bool kept_lamps_registered = RegisterLifecycleReset(
    "kept lamps", {Lifecycle::NewCharacter}, [] { kept_lamps_reset.store(true); });

void KeepGuestLampsLit() {
    static std::set<s32> kept;
    static int kept_slot = -1;
    const int slot = character_slot.load();
    if (kept_lamps_reset.exchange(false) || slot != kept_slot) {
        kept.clear();
        kept_slot = slot;
    }
    if (!IsPartyGuestRole() || !InPartySession()) {
        for (const s32 lamp : LitHeadstoneLamps(EventFlagOn)) {
            kept.insert(lamp);
        }
        return;
    }
    const std::vector<s32> team = TeamWorldFlagsOnForGuest();
    const std::vector<s32> relight = LampsToRelight(kept, team, EventFlagOn);
    if (!relight.empty()) {
        using SetFlag = void PS4_SYSV_ABI (*)(u64, s32, bool);
        std::vector<s32> lamps;
        std::string named;
        for (const s32 flag : relight) {
            reinterpret_cast<SetFlag>(image_base + PartyGhostSetEventFlagOffset)(0, flag, true);
            lamps.push_back(*LampOfActiveFlag(flag));
            named += fmt::format(" {}", flag);
        }
        ListLamps(lamps);
        LOG_INFO(Debug,
                 "Bloodborne party: lamps lit again for this guest (its own and the team's):{}",
                 named);
    }
    for (const s32 lamp : LitHeadstoneLamps(EventFlagOn)) {
        kept.insert(lamp);
    }
}

// See HeadstoneLampsLit: every second in a party, each Dream headstone's flag follows this
// player's own lamps (in a session its own event's set reaches it only over a flag message).
void KeepHeadstoneFlagsOwn() {
    static s64 checked_ms = 0;
    const s64 now_ms = SteadyMilliseconds();
    if (!party_controller || now_ms - checked_ms < 1'000 || GetLocalPlayer() < 0x10000) {
        return;
    }
    checked_ms = now_ms;
    KeepGuestLampsLit();
    for (const HeadstoneLamps& headstone : DreamHeadstones) {
        const bool lit = *HeadstoneLampsLit(headstone.flag, EventFlagOn);
        if (EventFlagOn(headstone.flag) == lit) {
            continue;
        }
        reinterpret_cast<bool PS4_SYSV_ABI (*)(u64, s32, bool)>(
            image_base + PartyGhostSetEventFlagOffset)(0, headstone.flag, lit);
        LOG_INFO(Debug, "Bloodborne party: headstone flag {} {}: this player's lamps",
                 headstone.flag, lit ? "set" : "cleared");
    }
}

// See ImpurityBonusChange: every second, the Impurity rune's HP bonus put on (through the
// player's own add-effect virtual, as event 9035's 2004:8) or taken off.
void KeepImpurityBonus(bool in_session) {
    static s64 checked_ms = 0;
    static bool added = false;
    const s64 now_ms = SteadyMilliseconds();
    const u64 player = GetLocalPlayer();
    if (!party_controller || now_ms - checked_ms < 1'000 || player < 0x10000) {
        return;
    }
    checked_ms = now_ms;
    const std::vector<s32> effects = LocalEffects();
    if (std::ranges::find(effects, ImpurityHpEffect) == effects.end()) {
        added = false; // gone with a load, or never there
    }
    const bool gets_bonus = GetsImpurityBonus(in_session, IsPartyGuestRole());
    switch (ImpurityBonusChange(gets_bonus, effects, added)) {
    case EffectChange::Add:
        if (!HasVirtual(player, CharacterAddSpEffectSlot, AddSpEffectOffset, "Impurity bonus")) {
            return;
        }
        reinterpret_cast<AddSpEffectFn>(image_base + AddSpEffectOffset)(
            player, ImpurityHpEffect, player, 0, 1, 0, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 0);
        added = true;
        LOG_INFO(Debug, "Bloodborne party: Impurity rune worn in the party; HP bonus {} on",
                 ImpurityHpEffect);
        break;
    case EffectChange::Remove:
        added = !EndPlayerEffect(player, ImpurityHpEffect);
        LOG_INFO(Debug, "Bloodborne party: Impurity HP bonus {} off (ended={})", ImpurityHpEffect,
                 !added);
        break;
    case EffectChange::None:
        break;
    }
}

void KeepHeldItemFlagsOwn() {
    static s64 checked_ms = 0;
    const s64 now_ms = SteadyMilliseconds();
    if (!party_controller || now_ms - checked_ms < 1'000 || GetLocalPlayer() < 0x10000 ||
        !PlayerItemsReadable()) {
        return;
    }
    checked_ms = now_ms;
    for (const HeldItemFlag& held : HeldItemFlags) {
        const bool holds = TradeHeld({BloodborneCoop::TradeItemKind::Goods, held.goods, 0}) > 0;
        const auto fix = HeldItemFlagFix(true, EventFlagOn(held.flag), holds);
        if (!fix) {
            continue;
        }
        reinterpret_cast<bool PS4_SYSV_ABI (*)(u64, s32, bool)>(
            image_base + PartyGhostSetEventFlagOffset)(0, held.flag, *fix);
        LOG_INFO(Debug, "Bloodborne party: held-item flag {} {}: this player {} goods {}",
                 held.flag, *fix ? "set" : "cleared", holds ? "holds" : "has no", held.goods);
    }
}

// See GestureFlagFix: every second, a gesture's flag goes off while this player has not learned
// the gesture, so the NPC's talk gives it (a save that took the host's flag from a team world).
void KeepGestureFlagsOwn() {
    static s64 checked_ms = 0;
    const s64 now_ms = SteadyMilliseconds();
    if (!party_controller || now_ms - checked_ms < 1'000 || GetLocalPlayer() < 0x10000 ||
        !IsUsablePackedMap(PlayingMap())) {
        return;
    }
    checked_ms = now_ms;
    const std::vector<GestureSlot> gestures = PlayerGestures();
    if (std::ranges::none_of(gestures, &GestureSlot::learned)) {
        return; // no list yet (every character starts with gestures learned)
    }
    for (const GestureFlag& given : GestureFlags) {
        const bool learned = std::ranges::any_of(gestures, [&](const GestureSlot& slot) {
            return slot.learned && slot.gesture == given.gesture;
        });
        const auto fix = GestureFlagFix(true, EventFlagOn(given.flag), learned);
        if (!fix) {
            continue;
        }
        reinterpret_cast<bool PS4_SYSV_ABI (*)(u64, s32, bool)>(
            image_base + PartyGhostSetEventFlagOffset)(0, given.flag, *fix);
        LOG_INFO(Debug,
                 "Bloodborne party: gesture flag {} cleared: this player has not learned gesture "
                 "{}",
                 given.flag, given.gesture);
    }
}

// See GiftTalkFlagFix: every second, a Tonsil Stone giver's first-talk flag goes off while this
// player holds no Tonsil Stone, so the giver's talk gives this player its own (a save that took
// the host's flag from a team world).
void KeepGiftTalkFlagsOwn() {
    static s64 checked_ms = 0;
    const s64 now_ms = SteadyMilliseconds();
    if (!party_controller || now_ms - checked_ms < 1'000 || GetLocalPlayer() < 0x10000 ||
        !PlayerItemsReadable()) {
        return;
    }
    checked_ms = now_ms;
    for (const GiftTalkFlag& talk : GiftTalkFlags) {
        const bool holds = TradeHeld({BloodborneCoop::TradeItemKind::Goods, talk.goods, 0}) > 0;
        const auto fix = GiftTalkFlagFix(true, EventFlagOn(talk.flag), holds);
        if (!fix) {
            continue;
        }
        reinterpret_cast<bool PS4_SYSV_ABI (*)(u64, s32, bool)>(
            image_base + PartyGhostSetEventFlagOffset)(0, talk.flag, *fix);
        LOG_INFO(Debug, "Bloodborne party: gift talk flag {} cleared: this player has no goods {}",
                 talk.flag, talk.goods);
    }
}

// See OwnedWeaponFlagFix: every second in a loaded world, an owned-weapon flag comes on once this
// player holds the weapon; once per save slot (a file marks it done), those of weapons it does not
// hold go off, so the Bath shop sells this character's own weapons only. 5500-5521 go off at any
// time (RechecksOwnedWeaponFlagAlways: a phantom's game sets them on each load).
void KeepOwnedWeaponFlagsOwn() {
    static s64 checked_ms = 0;
    static int checked_slot = -2;
    static bool recheck = false;
    const s64 now_ms = SteadyMilliseconds();
    if (!party_controller || now_ms - checked_ms < 1'000 || GetLocalPlayer() < 0x10000 ||
        !PlayerItemsReadable() || !IsUsablePackedMap(PlayingMap())) {
        return;
    }
    checked_ms = now_ms;
    const int slot = character_slot.load();
    const auto marker = Common::FS::GetUserPath(Common::FS::PathType::UserDir) /
                        fmt::format("bloodborne_party_owned_weapons_slot{}.txt", slot);
    if (slot != checked_slot) {
        checked_slot = slot;
        std::error_code error;
        recheck = slot >= 0 && !std::filesystem::exists(marker, error);
    }
    const std::vector<u32> held = HeldWeaponIds();
    int cleared = 0;
    for (const OwnedWeaponFlag& owned : OwnedWeaponFlags) {
        const bool holds = HoldsWeapon(owned.weapon, held);
        const auto fix = OwnedWeaponFlagFix(true, EventFlagOn(owned.flag), holds,
                                            recheck || RechecksOwnedWeaponFlagAlways(owned.flag));
        if (!fix) {
            continue;
        }
        reinterpret_cast<bool PS4_SYSV_ABI (*)(u64, s32, bool)>(
            image_base + PartyGhostSetEventFlagOffset)(0, owned.flag, *fix);
        if (*fix || !recheck) {
            LOG_INFO(Debug, "Bloodborne party: owned-weapon flag {} {}: this player {} {}",
                     owned.flag, *fix ? "set" : "cleared", *fix ? "holds" : "has no", owned.weapon);
        } else {
            ++cleared;
        }
    }
    if (recheck) {
        recheck = false;
        std::ofstream{marker} << "rechecked" << std::endl;
        LOG_INFO(Debug,
                 "Bloodborne party: cleared {} owned-weapon flags in save slot {} (weapons this "
                 "character does not hold)",
                 cleared, slot);
    }
}

} // namespace

namespace Coop {

void TickPlayersOnThisMap() {
    LogItemModuleChecks();
    LogItemButtonAndMapItems();
    KeepGuestFullHp();
    PolishMultiplayerUi();
    const bool in_session = InPartySession();
    // The boards go both ways in and out of a session (a guest on its own trip is out of one).
    const bool sharing = SharesWithParty(party_controller.has_value(), GetLocalPlayer() >= 0x10000);
    UnlockPartyTrophies(sharing);
    KeepRitualAltarsWhole();
    SetPartyWorldChanges(sharing);
    KeepGuestProgress(in_session);
    LogFlagChangesInSession(sharing);
    {
        const auto started = std::chrono::steady_clock::now();
        KeepEnemyDeathsInStep(sharing);
        coop_time.Add("enemy step",
                      std::chrono::duration_cast<std::chrono::nanoseconds>(
                          std::chrono::steady_clock::now() - started)
                          .count(),
                      SteadyMilliseconds());
    }
    SnapshotHostMapState(sharing);
    ResumeWhereTheGuestLeft(in_session);
    LogWatchedWrites();
    GiveDeferredBossItems();
    TurnOffMultiplayerWalls(in_session);
    PlayGuestAsHost(in_session);
    TickGuestLampTrip();
    // SHADPS4_BLOODBORNE_ESD_LOG=1: every member's talk answers, not only a guest's.
    static const bool esd_log_all = std::getenv("SHADPS4_BLOODBORNE_ESD_LOG") != nullptr;
    LogEsdAnswers(in_session && party_controller &&
                  (esd_log_all || party_controller->SearchEffectId() == SmallResonantBellEffect));
    RestoreSearchAfterTrip(in_session);
    const u32 map = PlayingMap();
    party_map_now.store(party_controller && IsUsablePackedMap(map) ? map : 0);
    if (EnemyOwnerOn()) {
        TickEnemyOwner(in_session, IsUsablePackedMap(map) ? map : 0);
    } else {
        ClaimEnemiesWhenAlone(in_session, IsUsablePackedMap(map) ? map : 0);
    }
    GrantPartyEchoes();
    TickTrades();
    FinishWaitingJoins(in_session);
    ResendOwnDataOnChange(in_session);
    LogMemberDataChanges(in_session);
    GiveEncounterInsight(in_session);
    KeepImpurityBonus(in_session);
    // Whether this character has been to the Hunter's Dream: its own world has the first visit's
    // flag, or it is there (the first visit's event runs for a guest too).
    static int dream_slot = -2;
    static bool dream_visited = false;
    if (const int slot = character_slot.load(); slot != dream_slot) {
        dream_slot = slot;
        dream_visited = false;
    }
    constexpr u32 HuntersDream = 0x15000000;
    if (!dream_visited && ((!in_session && EventFlagOn(9401)) || map == HuntersDream)) {
        dream_visited = true;
    }
    for (const s32 flag :
         GuestProgressFlags(IsPartyGuestRole(), in_session, IsUsablePackedMap(map) ? map : 0,
                            dream_visited, in_session && EventFlagOn(12410900))) {
        if (!EventFlagOn(flag)) {
            // Set while in the session, so the guest's world keeps it (KeepWorldChanges).
            reinterpret_cast<bool PS4_SYSV_ABI (*)(u64, s32, bool)>(
                image_base + PartyGhostSetEventFlagOffset)(0, flag, true);
            LOG_INFO(Debug, "Bloodborne party: guest out in the world; set Dream flag {}", flag);
        }
    }
    RecheckShopLinesOnce(IsUsablePackedMap(map) ? map : 0);
    GiveBackPhantomKeyItems();
    KeepHeldItemFlagsOwn();
    KeepGestureFlagsOwn();
    KeepGiftTalkFlagsOwn();
    KeepOwnedWeaponFlagsOwn();
    KeepHeadstoneFlagsOwn();
    static RecreateAfterJoin recreate_after_join;
    if (recreate_after_join.Tick(IsPartyGuestRole(), in_session,
                                 in_session ? OtherSessionMembers().size() : 0,
                                 in_session && InSessionWorld(), SteadyMilliseconds())) {
        LOG_INFO(Debug, "Bloodborne party: joined where another guest is; re-creating the others");
        recreate_pending_after_join =
            RecreateIsForAJoin(recreate_players_pending, recreate_pending_after_join);
        recreate_players_pending = true;
        recreated_since_trip.clear();
    }
    if (!in_session) {
        recreate_players_pending = false;
        recreate_pending_after_join = false;
        recreated_since_trip.clear();
        saved_join_tasks.clear();
        return;
    }
    if (!recreate_players_pending) {
        return;
    }
    std::vector<MemberMap> board;
    std::string self;
    {
        std::scoped_lock lock{map_board_mutex};
        if (std::chrono::steady_clock::now() - map_board_time > std::chrono::seconds{3}) {
            return;
        }
        board = map_board;
        self = map_board_member;
    }
    // Not while the player is still being placed: a guest's own warp has no position for a moment
    // and gets moved to its lamp (guest_trip_fixup_at); re-created then, the host never appeared.
    if (guest_trip_fixup_at || !LocalPlayerPosition()) {
        return;
    }
    std::vector<std::string> online_ids;
    for (const MemberMap& entry : board) {
        if (entry.member != self) {
            if (std::string online_id = OnlineIdOfBoardMember(entry.member); !online_id.empty()) {
                online_ids.push_back(std::move(online_id));
            }
        }
    }
    const std::vector<u64> members = OtherSessionMembers();
    std::vector<std::optional<std::string>> names;
    for (const u64 member : members) {
        names.push_back(SessionMemberBoardName(member, online_ids));
    }
    const RecreatePlan plan = PlanRecreate(names, board, self, map, recreated_since_trip);
    if (!plan.now.empty()) {
        std::vector<u64> now;
        std::string who;
        for (const size_t index : plan.now) {
            now.push_back(members[index]);
            if (names[index]) {
                recreated_since_trip.insert(*names[index]);
                who += ' ' + OnlineIdOfBoardMember(*names[index]);
            } else {
                who += " ?";
            }
        }
        LOG_INFO(Debug, "Bloodborne party trip: re-creating{} on map {:#x} ({} of {} members)", who,
                 map, now.size(), members.size());
        RecreateRemotePlayers(now, !recreate_pending_after_join);
    }
    if (plan.done) {
        recreate_players_pending = false;
    }
}

// From the party tick: ends the trip once the player is on the new stage (GlobalState +0x08 is
// the loading flag).
void TickPartyTrip() {
    const u64 state = GetGlobalState();
    if (state < 0x10000 || !HasMemoryAccess(state, 0x09, MemoryProt::CpuRead)) {
        return;
    }
    std::unique_lock lock{party_trip_mutex};
    const auto now = PartyTrip::Clock::now();
    const bool was_active = party_trip.Active(now);
    bool guest_arrived = false;
    // Raw, here and in the arrival line: the trip machinery is one of the tracker's inputs.
    party_trip.ObserveStage(GetCurrentPackedMap(), ReadValue<u8>(state, 0x08) != 0,
                            GetLocalPlayer());
    if (party_trip.DropIfNoLoad(now)) {
        LOG_INFO(Debug,
                 "Bloodborne party: the world reload loaded nothing (map {:#x}): no trip, the "
                 "session kept",
                 GetCurrentPackedMap());
        return;
    }
    if (was_active && !party_trip.Active(now)) {
        if (party_trip.WorldReload()) {
            OnLifecycle(Lifecycle::Trip); // held back until it loaded (StartPartyTrip)
        }
        const auto at = LocalPlayerPosition();
        LOG_INFO(Debug, "Bloodborne party trip: arrived map={:#x} at=({}, {}, {}) lamp={}",
                 GetCurrentPackedMap(), at ? (*at)[0] : 0.0f, at ? (*at)[1] : 0.0f,
                 at ? (*at)[2] : 0.0f, ReadValue<s32>(state, 0x1528));
        // Kaniel saw the camera fly in after every trip, the host's too: the snap the map's start
        // asked for can come before the player stands where the trip put it.
        SnapCamera();
        recreate_players_pending = true;
        recreate_pending_after_join = false;
        recreated_since_trip.clear();
        restore_search_pending = true;
        // Again on arrival (below), in case the load created no map items (see
        // SetGuestPickupsBeforeMapItems): the headstone reads the lamps when it is used.
        guest_arrived = IsPartyGuestRole();
        if (guest_trip_in_flight) {
            guest_trip_in_flight = false;
            guest_trip_fixup_at = std::chrono::steady_clock::now() + std::chrono::seconds{1};
        }
        ScheduleWipeRespawn();
    }
    lock.unlock();
    if (guest_arrived) {
        // The game put the guest's own saved world back as the trip loaded, also after the load's
        // pass: the map's team world again, then the session's changes.
        ApplyTeamWorldOnLoad(true);
        LOG_INFO(Debug, "Bloodborne party trip: set {} of the session's world changes on arrival",
                 SetGuestWorldChanges());
        ApplyTeamTimeOfDayOnLoad();
    }
}

} // namespace Coop

namespace {

// CSMultiPlayMan's member-leave handler (mgr, member id): marks the member's join task as
// exiting (reason 1 for anyone but the local player). Logged with its caller while in a party
// session, to find who drops a member after a trip.
constexpr u64 MemberLeaveOffset = 0x01E54DB0;

bool PS4_SYSV_ABI LogMemberLeave(u64, const GuestRegisterSnapshot* registers) {
    if (!party_controller) {
        return false;
    }
    const u64 caller = ReadValue<u64>(registers->rsp, 0);
    const u64 session = ReadValue<u64>(image_base + SessionManagerPointerOffset, 0);
    const bool self = session >= 0x10000 && registers->rsi >= 0x10000 &&
                      ReadValue<u64>(registers->rsi, 0) == ReadValue<u64>(session, 0xD0);
    LOG_INFO(Debug, "Bloodborne member leave from {:#x} self={} trip={} map={:#x}",
             caller >= image_base ? caller - image_base : caller, self, PartyTripActive(),
             CurrentPlace().map);
    return false;
}

// CSMultiPlayMan StopRequest (0x01ED07A0): a phantom's stage transition calls it (0x013CDEF6)
// before LeaveSession, which takes the guest out of the room. Skipped during a party trip.
bool PS4_SYSV_ABI KeepMatchingDuringTrip(u64, const GuestRegisterSnapshot* registers) {
    const u64 return_address = ReadValue<u64>(registers->rsp, 0);
    if (return_address >= image_base &&
        StartsTripOnWorldReload(return_address - image_base, PartyTripActive(), InPartySession())) {
        // A time-of-day cutscene's reload: a trip, so the session and its LeaveSession are kept;
        // one with no load (a border, Kaniel 2026-10-13) only keeps the session.
        StartPartyTrip(registers, true);
        LOG_INFO(Debug, "Bloodborne party: a world reload (time of day) is a trip once it loads");
    }
    if (!PartyTripActive()) {
        if (party_controller && InPartySession()) {
            LogGameCallChainAt("CSMultiPlayMan StopRequest", registers->rsp);
        }
        return false;
    }
    const u64 caller = ReadValue<u64>(registers->rsp, 0);
    LOG_INFO(Debug, "Bloodborne party trip: skipped StopRequest from {:#x}",
             caller >= image_base ? caller - image_base : caller);
    return true;
}

} // namespace

namespace Coop {

void InstallSeamlessTravel(u64 base, u64 image_size) {
    if (!party_controller || LeaveSessionOffset + StandardR15Prologue.size() > image_size) {
        return;
    }
    if (!world_teardown_hook_installed) {
        coop_features.Skipped("seamless travel", "needs world teardown");
        return;
    }
    const std::span<const u8> prologue{StandardR15Prologue.data(), 6};
    if (!InstallCoopHook(reinterpret_cast<void*>(base + LeaveSessionOffset), prologue, 0,
                         KeepSessionAfterTrip)) {
        LOG_ERROR(Debug, "Bloodborne seamless travel hooks unavailable; a trip ends the session");
        coop_features.Skipped("seamless travel", SiteFailure(base, image_size, LeaveSessionOffset));
        return;
    }
    if (!InstallCoopHook(reinterpret_cast<void*>(base + StageTransitionEntryOffset), prologue, 0,
                         RecordPartyTrip)) {
        LOG_ERROR(Debug, "Bloodborne seamless travel hooks unavailable; a trip ends the session");
        coop_features.Skipped("seamless travel",
                              SiteFailure(base, image_size, StageTransitionEntryOffset));
        return;
    }
    seamless_travel_hooks_installed = true;
    coop_features.On("seamless travel");
    if (!InstallCoopHook(reinterpret_cast<void*>(base + MultiPlayStopRequestOffset), prologue, 0,
                         KeepMatchingDuringTrip)) {
        LOG_ERROR(Debug, "Bloodborne StopRequest hook unavailable; a guest's trip leaves the room");
        coop_features.Skipped("stop request",
                              SiteFailure(base, image_size, MultiPlayStopRequestOffset));
    } else {
        stop_request_hook_installed = true;
        coop_features.On("stop request");
    }
    if (!InstallCoopHook(reinterpret_cast<void*>(base + MemberLeaveOffset), prologue, 0,
                         LogMemberLeave)) {
        LOG_ERROR(Debug, "Bloodborne member-leave log unavailable");
        coop_features.Skipped("member leave log", SiteFailure(base, image_size, MemberLeaveOffset));
    } else {
        coop_features.On("member leave log");
    }
    LOG_INFO(Debug, "Bloodborne seamless travel: a party member's trip keeps the session");
}

// A summoned guest leaving the host's world, or a host leaving its own (quit to title, death
// respawn), ends the session like the unpatched game would; the keep-matching patch otherwise
// leaves a ghost member in the room.
void EndSessionOnWorldTeardown() {
    const u64 multi_play = GetMatchingState();
    const u64 rules = ReadValue<u64>(image_base + SummonSessionRulesPointerOffset, 0);
    if (multi_play < 0x10000 || !HasMemoryAccess(multi_play, 0x128, MemoryProt::CpuRead) ||
        rules < 0x10000 || !HasMemoryAccess(rules, 0x88, MemoryProt::CpuRead)) {
        return;
    }
    const s32 matching_state = ReadValue<s32>(multi_play, 0x124);
    const s32 reload_state = ReadValue<s32>(rules, 0x84);
    const bool reload_pending =
        pending_summon_reload_map.load(std::memory_order_acquire) != 0 ||
        pending_summon_reload_state_map.load(std::memory_order_acquire) != 0;
    // Diagnostics for the guest crashes in a world teardown outside a session (a lamp or
    // headstone trip while its bell searched, 2026-10-08): the matching state the keep-matching
    // patch carries over each teardown.
    LOG_INFO(Debug,
             "Bloodborne world teardown: matching state {} reload state {} reload pending {} "
             "trip {} effects{}",
             matching_state, reload_state, reload_pending, PartyTripActive(), [] {
                 std::string text;
                 for (const s32 effect : LocalEffects()) {
                     text += fmt::format(" {}", effect);
                 }
                 return text;
             }());
    if (!ShouldEndSessionOnWorldTeardown(matching_state, reload_state, reload_pending)) {
        return;
    }
    if (PartyTripActive()) {
        LOG_INFO(Debug, "Bloodborne party trip: kept the session through the world teardown");
        return;
    }
    using StopRequest = bool PS4_SYSV_ABI (*)(u64 multi_play);
    const bool result =
        reinterpret_cast<StopRequest>(image_base + MultiPlayStopRequestOffset)(multi_play);
    LOG_INFO(Debug, "Bloodborne {} left its world; ended the session result={}",
             matching_state == MatchingStateHosting ? "host" : "guest", result);
}

} // namespace Coop

} // namespace Core::Bloodborne
