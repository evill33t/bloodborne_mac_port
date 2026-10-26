// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// Party death: going down, revives, the bloodstain, the party panel. Split from bloodborne_re.cpp;
// the shared declarations are in bloodborne_re_internal.h.

#include "core/bloodborne_compass.h"
#include "core/bloodborne_crash_dump.h"
#include "core/bloodborne_duel.h"
#include "core/bloodborne_party.h"
#include "core/bloodborne_portrait.h"
#include "core/bloodborne_re.h"
#include "core/bloodborne_re_internal.h"
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
#include <new>
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

// Invasions (bloodborne_invasion.h): the invader's bodies here as of the last presence tick (the
// hit hook reads them), its last hit on the local player, and whether this death followed one
// (the vitals say killed_by_invader until the player is up again).
std::mutex invader_handles_mutex;
std::vector<u32> invader_handles;
std::atomic<s64> invader_hit_ms{-1};
std::atomic<bool> died_to_invader{false};
// The invader's own death, from the death scripts' hooks (InvaderTrip's `dead`).
std::atomic<bool> invader_death_noted{false};

// The character's level (PlayerGameData +0x90, as the port's save editor reads it), 0 unknown.
u32 OwnCharacterLevel() {
    const u64 data = OwnGameData();
    if (data < 0x10000 || !HasMemoryAccess(data + 0x90, 4, MemoryProt::CpuRead)) {
        return 0;
    }
    const s32 level = ReadValue<s32>(data, 0x90);
    return level > 0 && level < 1000 ? static_cast<u32>(level) : 0;
}

// A boss fight on (event flag 4000; the death processing sets it too, so only while alive).
constexpr s32 BossFightFlag = 4000;

} // namespace

namespace {

// Event script bindings ReviveDeadPartyGuest calls.
constexpr u64 IsWhiteGhostOffset = 0x01331D80;
constexpr u64 UpDateBloodMarkOffset = 0x013308B0;
constexpr u64 RevivePlayerOffset = 0x01332B20;
// 01.09's local-player revive: refills HP and clears the death's controller locks ([+0x58]
// +0x114 bit 0 and [+0x38]+0x48 bits 0-1), then makes the player a live one (type 0).
constexpr u64 ReviveLocalCharacterOffset = 0x013179B0;
// Resolves a respawn point id to a position and rotation in the loaded blocks.
constexpr u64 ResolveRespawnPointOffset = 0x0154AFC0;
constexpr u64 LoadedBlocksPointerOffset = 0x0553B148;
// ChrResetAnimation(_, entity): sends the character's behaviour back to idle.
constexpr u64 ChrResetAnimationOffset = 0x013370F0;

// Diagnostics for guest bloodstains in the host's world (Phase 2): logs the local bloodstain
// record, the spawned stain object and the world flags that gate them whenever they change.
// The local player's own bloodstain: [[SprjEventMan +0x80] +8], the object the game's show/hide
// (0x01868A50) takes when a load shows it (0x017C23EB-0x017C25DC) and on a join (0x0193A75A).
// (Not FrpgNetMan +0xC68's: passed that, the show crashed Brom's game, Kaniel 2026-10-11.)
u64 OwnBloodstain() {
    const u64 events = ReadValue<u64>(image_base + EventManPointerOffset, 0);
    const u64 list = events >= 0x10000 ? ReadValue<u64>(events, 0x80) : 0;
    return list >= 0x10000 ? ReadValue<u64>(list, 8) : 0;
}

// A stain object the show can take: the stain (its vtable), readable, and its marker (+0x88) and
// effect (+0x98) either absent or readable (the show frees them first).
bool LooksLikeBloodstain(u64 stain) {
    if (!HasVtable(stain, BloodstainVtable, "ShowGuestBloodstain") ||
        !HasMemoryAccess(stain, 0xC8, MemoryProt::CpuRead)) {
        return false;
    }
    for (const u64 offset : {u64{0x88}, u64{0x98}}) {
        const u64 part = ReadValue<u64>(stain, offset);
        if (part != 0 && (part < 0x10000 || !HasMemoryAccess(part, 0x98, MemoryProt::CpuRead))) {
            return false;
        }
    }
    return true;
}

} // namespace

namespace Coop {

// The stain's marker (+0x88) and effect (+0x98) are what 0x01868A50 makes when it shows the
// stain (and frees when it hides it): a stain recorded but never shown has neither (Kaniel
// 2026-10-11: no bloodstains in sight after the party wipe at the Tomb of Oedon).
void LogBloodstainStateChanges() {
    static std::string last;
    const u64 data = ReadValue<u64>(image_base + GameDataManPointerOffset, 0);
    const u64 record = data >= 0x10000 ? ReadValue<u64>(data, 0x30) : 0;
    const u64 stain = OwnBloodstain();
    const u64 global = GetGlobalState();
    const u64 player = GetLocalPlayer();
    const auto state =
        fmt::format("has_mark={} record_valid={} record_30={} record_38={} record_block={:#x} "
                    "record_pos=({}, {}, {}) death_pos=({}, {}, {}) stain={:#x} "
                    "stain_hidden={} shown_marker={:#x} shown_effect={:#x} flag1592={} "
                    "flag1770={} chr_type={}",
                    data >= 0x10000 ? ReadValue<u8>(data, 0x28) : 0xFF,
                    record >= 0x10000 ? ReadValue<u8>(record, 0x44) : 0xFF,
                    record >= 0x10000 ? ReadValue<s32>(record, 0x30) : 0,
                    record >= 0x10000 ? ReadValue<s32>(record, 0x38) : 0,
                    record >= 0x10000 ? ReadValue<u32>(record, 0x3C) : 0,
                    record >= 0x10000 ? ReadValue<float>(record, 0x00) : 0.0f,
                    record >= 0x10000 ? ReadValue<float>(record, 0x04) : 0.0f,
                    record >= 0x10000 ? ReadValue<float>(record, 0x08) : 0.0f,
                    record >= 0x10000 ? ReadValue<float>(record, 0x18) : 0.0f,
                    record >= 0x10000 ? ReadValue<float>(record, 0x1C) : 0.0f,
                    record >= 0x10000 ? ReadValue<float>(record, 0x20) : 0.0f, stain,
                    stain >= 0x10000 ? ReadValue<u8>(stain, 0xC0) : 0xFF,
                    stain >= 0x10000 ? ReadValue<u64>(stain, 0x88) : 0,
                    stain >= 0x10000 ? ReadValue<u64>(stain, 0x98) : 0,
                    global >= 0x10000 ? ReadValue<u8>(global, 0x1592) : 0xFF,
                    global >= 0x10000 ? ReadValue<u8>(global, 0x1770) : 0xFF,
                    player >= 0x10000 ? ReadValue<s32>(player, 0x78) : -1);
    if (state != last) {
        last = state;
        LOG_INFO(Debug, "Bloodborne bloodstain state map={:#x} {}", CurrentPlace().map, state);
    }
}

std::optional<Bloodstain> ReadOwnBloodstain() {
    const u64 data = ReadValue<u64>(image_base + GameDataManPointerOffset, 0);
    const u64 record = data >= 0x10000 ? ReadValue<u64>(data, 0x30) : 0;
    const auto bytes =
        record >= 0x10000 ? TryRead<std::array<u8, BloodstainRecordSize>>(record, 0) : std::nullopt;
    if (!bytes) {
        return std::nullopt;
    }
    return BloodstainOfRecord(ReadValue<u8>(data, 0x28) != 0, *bytes);
}

} // namespace Coop

namespace {

constexpr u64 ShowBloodstainOffset = 0x01868A50;

} // namespace

namespace Coop {

// See ShowsGuestBloodstain: a guest's own stain in the host's world.
void ShowGuestBloodstain() {
    static int tries = 0;
    static u64 tried_visit = 0;
    const u64 data = ReadValue<u64>(image_base + GameDataManPointerOffset, 0);
    const u64 record = data >= 0x10000 ? ReadValue<u64>(data, 0x30) : 0;
    const u64 global = GetGlobalState();
    const u64 player = GetLocalPlayer();
    const Place place = CurrentPlace();
    const u32 map = place.state == PlaceState::Playing ? place.map : 0;
    if (data < 0x10000 || record < 0x10000 || player < 0x10000) {
        return;
    }
    if (place.visit != tried_visit) {
        tries = 0; // a load: a new visit
        tried_visit = place.visit;
    }
    const u64 stain = OwnBloodstain();
    const bool party_guest =
        party_controller && party_controller->SearchEffectId() == SmallResonantBellEffect;
    const bool loading = global < 0x10000 || TryRead<u8>(global, 0x08).value_or(1) != 0;
    if (!LooksLikeBloodstain(stain) ||
        !ShowsGuestBloodstain(party_guest, InPartySession(), loading,
                              ReadValue<u8>(data, 0x28) != 0, ReadValue<s32>(record, 0x38),
                              ReadValue<u32>(record, 0x3C), IsUsablePackedMap(map) ? map : 0,
                              ReadValue<u64>(stain, 0x88) != 0, tries)) {
        return;
    }
    ++tries;
    reinterpret_cast<void PS4_SYSV_ABI (*)(u64, bool, bool)>(image_base + ShowBloodstainOffset)(
        stain, true, false);
    LOG_INFO(Debug,
             "Bloodborne party: showed the guest's own bloodstain ({} echoes) in the host's "
             "world on map {:#x}: marker {:#x}",
             ReadValue<s32>(record, 0x38), map, ReadValue<u64>(stain, 0x88));
}

// See RestoresBloodstainMark: a party guest keeps its lost echoes through a join.
void KeepGuestBloodstainMark() {
    static bool mark_before = false;
    const u64 data = ReadValue<u64>(image_base + GameDataManPointerOffset, 0);
    const u64 record = data >= 0x10000 ? ReadValue<u64>(data, 0x30) : 0;
    if (data < 0x10000 || record < 0x10000) {
        return;
    }
    const bool mark_now = ReadValue<u8>(data, 0x28) != 0;
    const s32 echoes = ReadValue<s32>(record, 0x38);
    const u32 block = ReadValue<u32>(record, 0x3C);
    const bool party_guest =
        party_controller && party_controller->SearchEffectId() == SmallResonantBellEffect;
    if (RestoresBloodstainMark(party_guest, mark_before, mark_now, echoes, block)) {
        WriteValue(data, 0x28, u8{1});
        LOG_INFO(Debug, "Bloodborne party: kept the guest's lost echoes ({}) through the join",
                 echoes);
        return;
    }
    mark_before = mark_now;
}

} // namespace Coop

namespace {

// The event system's "load wait" byte: SetLoadWait (0x013358F0) sets it, the leave handlers
// (0x013840E0 and others) set it with 4047, and only its constructor (0x01339669) clears it.
// Every event update copies it to SprjActionButtonMan +0x52 (0x0133A114): no action button.
constexpr u64 LoadWaitOffset = 0x8D;
constexpr u64 ActionButtonManPointerOffset = 0x05540048;

} // namespace

namespace Coop {

DirectInbox<DirectVitals> direct_vitals_inbox;

} // namespace Coop

namespace {

// UpDateBloodMark (native 0x013D2910) takes the stain position from the player only while
// GlobalState +0x1592 is set, which it is in the player's own world. In the host's world it
// copies a stored position instead (+0x1500, where the guest was summoned from), so the stain
// went somewhere unseen. Set it for the call.
bool PS4_SYSV_ABI UpdateGuestBloodMark() {
    const u64 global = GetGlobalState();
    const bool can_flag =
        global >= 0x10000 && HasMemoryAccess(global, 0x1593, MemoryProt::CpuWrite);
    const u8 own_world = can_flag ? ReadValue<u8>(global, 0x1592) : 0;
    if (can_flag) {
        WriteValue(global, 0x1592, u8{1});
    }
    const bool result =
        reinterpret_cast<bool PS4_SYSV_ABI (*)()>(image_base + UpDateBloodMarkOffset)();
    if (can_flag) {
        WriteValue(global, 0x1592, own_world);
    }
    return result;
}

// Party death (see PartyDeathEnabled). The IsNoDead exit hook (KeepPartyMemberUp) keeps the
// local player at 1 HP where a lethal hit would kill it while it is armed (another member stands
// on its map) or down; the party tick (TickPartyDeath) puts it down, gets it up, lets it give up
// and wipes it; the vitals board client (RunVitalsBoardClient) tells the party.
#ifdef BLOODBORNE_PARTY_DEATH_ALWAYS
// The port: always on, no setting (Kaniel 2026-10-13); its HUD's party list needs it.
const bool party_death_enabled = true;
#else
const bool party_death_enabled =
    PartyDeathEnabled(std::getenv("SHADPS4_BLOODBORNE_PARTY_DEATH"), false);
#endif
bool party_death_hook_installed{};
// What the game thread tells the hook: the local player's data module while party death is live
// (0 otherwise: the hook leaves everyone alone), and whether a lethal hit puts it down.
std::atomic<u64> party_death_data{};
std::atomic<bool> party_death_armed{};
std::atomic<bool> party_death_down{};
// Kaniel 2026-10-25: the local player spectates its teammates (the team's death mode; down above
// is set too: the hook keeps it alive): the pad reader gives the game nothing, counts L1 / R1 for
// the party tick (the teammate watched) and turns the spectator's camera with the right stick.
std::atomic<bool> party_spectating{};
std::atomic<int> spectate_steps{};
// Kaniel 2026-10-25: a boss fight with a teammate in it (BossFightCatchesDeath, from the party
// tick): the game's kills spectate (CatchOf). Each such catch is counted for the party tick, and
// whether it was a fall into the void (the body is put back on its last ground). Our own kills
// (giving up, a wipe's death) go through while party_death_own_kill is set.
std::atomic<bool> party_death_boss_fight{};
std::atomic<u32> party_death_boss_catches{};
std::atomic<bool> party_death_boss_void{};
std::atomic<bool> party_death_own_kill{};
// Lethal hits the hook kept from killing the armed player: each change puts it down.
std::atomic<u32> party_death_catches{};
// The local player fell into the void (a lethal check while FallsIntoTheVoid, or a real death in
// the void, see TickStanding): its respawn makes a trip (see ChooseDeathRespawn). Taken by
// MoveToRespawnPoint.
std::atomic<bool> party_death_void_fall{};
// Kaniel 2026-10-16: another member stood (anywhere) at this member's last tick: its death's reset
// is its own map's, not the party's wipe.
std::atomic<bool> party_death_others_stand{};
// What the next death respawn resets (see DeathResetsMap), called when the death came.
DeathResetCall party_death_reset;
// The death this tick: its respawn resets the map if `reset`, the party's wipe unless another
// member stands now.
bool DiesWithReset(bool reset) {
    party_death_reset.Died(reset, party_death_others_stand.load());
    return reset;
}
// This player's GlobalState +0x1528 from before a death (party death tick, while it is up), to
// tell a special death by (see PartyRespawnPoint). Another character starts over.
std::mutex own_point_mutex;
ReturnPointBeforeDeath own_point_before_death;
[[maybe_unused]] const bool own_point_registered =
    RegisterLifecycleReset("return point before death", {Lifecycle::NewCharacter}, [] {
        std::scoped_lock lock{own_point_mutex};
        own_point_before_death.Reset();
    });
// The point a guest's party death travels to when it is not the guest's own (+0x1528): that
// trip's arrival (MoveToRespawnPoint(true)) resolves it too, as the load put the save's lamp back
// in +0x1528 (see FixAutoplayTravelArrival).
std::optional<s32> party_death_trip_point;

// The point this member's real death wakes it at: a guest in a party session wakes at the host's
// point unless its death set a special one (PartyRespawnPoint). Not written to +0x1528: the
// respawn below resolves (or travels to) the point it is given, and a trip's load puts the save's
// lamp back there anyway.
s32 PartyDeathPoint(s32 own_point) {
    if (!IsPartyGuestRole() || !InPartySession()) {
        return own_point;
    }
    const std::optional<s32> host_point = HostReturnPointNow();
    std::optional<s32> before;
    {
        std::scoped_lock lock{own_point_mutex};
        before = own_point_before_death.Point();
    }
    const s32 point = PartyRespawnPoint(true, own_point, before, host_point);
    if (host_point && point == *host_point) {
        LOG_INFO(Debug, "Bloodborne revive: respawn at the host's point {} (own {})", point,
                 own_point);
    } else if (host_point) {
        LOG_INFO(Debug,
                 "Bloodborne revive: respawn at the own special point {} (the host's {}, before "
                 "the death {})",
                 point, *host_point, before.value_or(-1));
    } else {
        LOG_INFO(Debug, "Bloodborne revive: the host's point is not known; respawn at own {}",
                 point);
    }
    return point;
}

} // namespace

namespace {

// See ClearBossGauges: a party death's own Failed_BossAreaMission for the boss bars.
void ClearBossBarsAfterDeath() {
    const u64 holder = ReadValue<u64>(image_base + BossGaugesPointerOffset, 0);
    constexpr std::size_t Size = BossGaugeSlots * BossGaugeSize;
    if (holder < 0x10000 ||
        !HasMemoryAccess(holder + BossGaugesOffset, Size, MemoryProt::CpuWrite)) {
        return;
    }
    const std::vector<u32> up =
        ClearBossGauges({reinterpret_cast<u8*>(holder + BossGaugesOffset), Size});
    if (!up.empty()) {
        std::string which;
        for (const u32 handle : up) {
            which += fmt::format(" {:#x}", handle);
        }
        LOG_INFO(Debug, "Bloodborne party death: boss bars cleared:{}", which);
    }
}

} // namespace

namespace Coop {

// The lamp a death respawns the player at: resting at a lamp stores its respawn point id in
// GlobalState +0x1528 (SetReturnPointEntityId), and a death respawn resolves that id with
// 0x0154AFC0 against the loaded blocks ([0x0553B148], like Warp.RespawnTransform.Resolve). The
// same resolver finds the lamp while it is in a block the host's world has loaded; the guest is
// then moved there the way the Warp binding moves a character (vtable +0x5F8, then +0x1E1 |= 2).
// Otherwise the guest stays where it fell.
// A point on another map (see ChooseDeathRespawn: the first death's Hunter's Dream, a
// kidnapping's Gaol, a lamp elsewhere) is a trip there, as a guest's lamp trip is. So is a death
// that resets the map (party death's wipe), wherever the lamp is.
// A guest in a party session wakes at the host's point instead, unless its death was special (see
// PartyDeathPoint): the party wakes together.
void MoveToRespawnPoint(bool arriving_from_trip) {
    const bool void_fall = party_death_void_fall.exchange(false) && !arriving_from_trip;
    const DeathReset call =
        arriving_from_trip ? DeathReset::None : party_death_reset.TakeAtRespawn();
    const bool reset = call != DeathReset::None;
    if (!arriving_from_trip) {
        ClearBossBarsAfterDeath();
    }
    if (call == DeathReset::OwnMap) {
        LOG_INFO(Debug, "Bloodborne party death: another member stood when this member died: this "
                        "death is no party wipe (the party's kills stay, but on its own map "
                        "if nobody stands there)");
        // Kaniel's play test 2026-10-19: both on the Ward died while krolebord was in the Dream,
        // and the Ward came back half respawned, different for each of them. shadNet starts the
        // map over for everyone unless someone stands on it (the Reset comes back in the log).
        if (const u32 map = PlayingMap(); IsUsablePackedMap(map)) {
            TellServerMapWipe(map);
        }
    } else if (call == DeathReset::PartyWipe) {
        OnLifecycle(Lifecycle::Wipe); // kills and enemy HP, see their RegisterLifecycleReset
    }
    const u64 global = GetGlobalState();
    const s32 own_point = global >= 0x10000 ? ReadValue<s32>(global, 0x1528) : -1;
    s32 point = own_point;
    if (!arriving_from_trip) {
        party_death_trip_point.reset();
        point = PartyDeathPoint(own_point);
    } else if (party_death_trip_point && *party_death_trip_point == guest_trip_point) {
        point = *std::exchange(party_death_trip_point, std::nullopt); // that death's trip
    }
    const u64 blocks = ReadValue<u64>(image_base + LoadedBlocksPointerOffset, 0);
    const u64 player = GetLocalPlayer();
    if (blocks < 0x10000 || player < 0x10000) {
        return;
    }
    alignas(16) std::array<float, 4> position{};
    alignas(16) std::array<float, 4> rotation{};
    using Resolve = bool PS4_SYSV_ABI (*)(u64 blocks, float* position, float* rotation, s32 point);
    const bool in_loaded_block =
        point > 0 && reinterpret_cast<Resolve>(image_base + ResolveRespawnPointOffset)(
                         blocks, position.data(), rotation.data(), point);
    const DeathRespawn respawn =
        ChooseDeathRespawn(point, in_loaded_block, arriving_from_trip, reset, void_fall);
    if (WipeTripRespawnsOnArrival(reset, respawn)) {
        wipe_trip_respawn_pending.store(true); // see RespawnWhatTheSaveLeftDead
    }
    switch (respawn) {
    case DeathRespawn::StayPut:
        LOG_INFO(Debug, "Bloodborne revive: respawn point {} not reachable; staying put", point);
        return;
    case DeathRespawn::Travel:
        guest_trip_point = point;
        if (point != own_point) {
            party_death_trip_point = point;
        }
        // Right away: getting up where the player fell first looked like a second respawn.
        guest_lamp_trip_at = std::chrono::steady_clock::now() + std::chrono::milliseconds{200};
        LOG_INFO(Debug,
                 "Bloodborne revive: respawn point {} is on map {:#x}; travelling there "
                 "(reset={}, void fall={})",
                 point, PackedMapOfWarpPoint(point), reset, void_fall);
        return;
    case DeathRespawn::MoveToPoint:
        break;
    }
    if (!PlaceLocalPlayer(position, rotation, "revive teleport")) {
        return;
    }
    LOG_INFO(Debug,
             "Bloodborne revive: moved to last lamp {} at ({}, {}, {}) rotation ({}, {}, {})",
             point, position[0], position[1], position[2], rotation[0], rotation[1], rotation[2]);
}

} // namespace Coop

namespace {

void WarpGuestToLastLamp() {
    MoveToRespawnPoint(false);
}

AliveStateSnapshot local_alive_state;

// The local player's state flags (+0x1E0, 8 bytes) and action state ([+0x3B0]+8: +0x1B4/+0x1B8).
u64 LocalActionModule(u64 player) {
    const u64 model = ReadValue<u64>(player, 0x3B0);
    return model >= 0x10000 ? ReadValue<u64>(model, 0x08) : 0;
}

std::optional<AliveState> ReadLocalAliveState(u64 player, bool& alive) {
    alive = false;
    const u64 module = LocalActionModule(player);
    const u64 model = ReadValue<u64>(player, 0x3B0);
    const u64 data = model >= 0x10000 ? ReadValue<u64>(model, 0x20) : 0;
    if (player < 0x10000 || module < 0x10000 || data < 0x10000) {
        return std::nullopt;
    }
    const AliveState state{
        .flags = ReadValue<u64>(player, 0x1E0),
        .action = ReadValue<s32>(module, 0x1B4),
        .action_sub = ReadValue<s32>(module, 0x1B8),
    };
    alive = ReadValue<s32>(data, 0xF8) > 0 && (ReadValue<u8>(player, 0x1E3) & 1) == 0;
    return state;
}

} // namespace

namespace Coop {

void ObserveLocalAliveState() {
    bool alive = false;
    if (const auto state = ReadLocalAliveState(GetLocalPlayer(), alive)) {
        local_alive_state.Observe(alive, *state);
    }
}

} // namespace Coop

namespace {

void RestoreLocalAliveState() {
    const u64 player = GetLocalPlayer();
    bool alive = false;
    const auto before = ReadLocalAliveState(player, alive);
    const auto& saved = local_alive_state.Saved();
    if (!before || !saved) {
        LOG_ERROR(Debug, "Bloodborne revive: no alive state to restore");
        return;
    }
    WriteValue(player, 0x1E0, saved->flags);
    const u64 module = LocalActionModule(player);
    WriteValue(module, 0x1B4, saved->action);
    WriteValue(module, 0x1B8, saved->action_sub);
    LOG_INFO(Debug,
             "Bloodborne revive restored alive state flags {:#x}->{:#x} action {}->{} "
             "sub {}->{}",
             before->flags, saved->flags, before->action, saved->action, before->action_sub,
             saved->action_sub);
}

GhostDeathCalls MakeReviveCalls() {
    return GhostDeathCalls{
        .is_white_ghost =
            reinterpret_cast<bool PS4_SYSV_ABI (*)()>(image_base + IsWhiteGhostOffset),
        .set_event_flag = reinterpret_cast<bool PS4_SYSV_ABI (*)(u64, s32, bool)>(
            image_base + PartyGhostSetEventFlagOffset),
        .clear_return_state =
            [] {
                // OnEvent_4000 (0x013813D0) sets it to 4 and skips deaths while it is above 0.
                const u64 events = ReadValue<u64>(image_base + EventManPointerOffset, 0);
                const u64 state = events >= 0x10000 ? ReadValue<u64>(events, 0x10) : 0;
                if (state >= 0x10000) {
                    WriteValue(state, 0x08, s32{0});
                }
            },
        .request_full_recover =
            reinterpret_cast<bool PS4_SYSV_ABI (*)()>(image_base + RequestFullRecoverOffset),
        .update_blood_mark = UpdateGuestBloodMark,
        .revive_player = reinterpret_cast<bool PS4_SYSV_ABI (*)()>(image_base + RevivePlayerOffset),
        .revive_local_character =
            reinterpret_cast<void PS4_SYSV_ABI (*)()>(image_base + ReviveLocalCharacterOffset),
        .restore_alive_state = RestoreLocalAliveState,
        .set_chr_type =
            reinterpret_cast<void PS4_SYSV_ABI (*)(u64, s32, s32)>(image_base + SetChrTypeOffset),
        .chr_reset_animation =
            reinterpret_cast<void PS4_SYSV_ABI (*)(u64, s32)>(image_base + ChrResetAnimationOffset),
        .warp_to_last_lamp = WarpGuestToLastLamp,
    };
}

} // namespace

namespace Coop {

// A party guest that died gets up again where the game would show the "returned to your world"
// menu (the warp home behind it is patched out, see GhostDeathKeepSessionPatches).
void ReviveGuestAfterGhostDeath() {
    if (Invading()) {
        ReviveInvaderInPlace();
        return;
    }
    const bool revived = ReviveDeadPartyGuest(MakeReviveCalls());
    const u64 player = GetLocalPlayer();
    LOG_INFO(Debug, "Bloodborne party guest died; revived in place={} type={} flags1e3={:#x}",
             revived, player >= 0x10000 ? ReadValue<s32>(player, 0x78) : -1,
             player >= 0x10000 ? ReadValue<u8>(player, 0x1E3) : 0xFF);
}

// Invasions: the invader dead in the target's world gets up where it fell, with no lamp of the
// target's (ReviveInPlace without its warp): InvaderGoHome takes it to its own lamp.
void ReviveInvaderInPlace() {
    GhostDeathCalls calls = MakeReviveCalls();
    calls.warp_to_last_lamp = [] {};
    ReviveDeadPartyHost(calls);
    invader_death_noted.store(true);
    LOG_INFO(Debug, "Co-op invasion: the invader died in the target's world; revived to go home");
}

// Replaces native SoloPlayDeath_2 (bloodstain, then the reload at the lamp) for a host with a
// session: it revives like a guest and the world is not reloaded.
// OnLeave_Limit (event 4046), 5 s after a white phantom lost its host (the host left, travelled,
// kicked it or the room went away): the reset and warp into the phantom's own world. A party
// member skips it (SkipsLeaveLimit): a phantom while the session goes on (with nobody left, the
// host crashed, it goes home, as a phantom never rings again), and a live player always: the
// game ran it on a host whose guest died apart from it, and the warp took the host to the title.
bool PS4_SYSV_ABI StayWhenTheHostLeaves(u64, const GuestRegisterSnapshot* registers) {
    const u64 player = GetLocalPlayer();
    if (!party_controller || player < 0x10000 ||
        !HasMemoryAccess(player, 0x7C, MemoryProt::CpuRead)) {
        return false;
    }
    const s32 type = ReadValue<s32>(player, 0x78);
    if (!SkipsLeaveLimit(type, OtherSessionMembers().size())) {
        LOG_INFO(Debug, "Bloodborne party guest lost its host and the session; going home");
        return false;
    }
    if (type != WhiteGhostCharacterType && registers != nullptr) {
        LogGameCallChainAt("leave limit", registers->rsp);
    }
    // The leave set 4047 ("on the way out"); left set, it swallows the next leave and death.
    reinterpret_cast<bool PS4_SYSV_ABI (*)(u64, s32, bool)>(
        image_base + PartyGhostSetEventFlagOffset)(0, 4047, false);
    // It also set the event system's load wait (+0x8D, the SetLoadWait binding 0x013358F0; the
    // timer passes the event system in rsi) for the reload skipped here, and only a new event
    // system clears it: Kaniel 2026-10-11, after a friend's game went away neither the host nor
    // the other guest could pick up items or talk, only fight.
    const u64 events = registers != nullptr ? registers->rsi : 0;
    const u64 buttons = ReadValue<u64>(image_base + ActionButtonManPointerOffset, 0);
    s32 load_wait = -1;
    if (events >= 0x10000 && HasMemoryAccess(events, LoadWaitOffset + 1, MemoryProt::CpuWrite)) {
        load_wait = ReadValue<u8>(events, LoadWaitOffset);
        if (load_wait == 1) {
            WriteValue(events, LoadWaitOffset, u8{0});
        }
    }
    LOG_INFO(Debug,
             "Bloodborne party member (type {}) lost a member; staying where it is map={:#x} "
             "load wait {} (action buttons {}) cleared",
             type, CurrentPlace().map, load_wait,
             buttons >= 0x10000 ? ReadValue<u8>(buttons, 0x52) : -1);
    return true;
}

bool PS4_SYSV_ABI OverrideHostDeathRespawn(u64, const GuestRegisterSnapshot*) {
    if (Invading()) {
        ReviveInvaderInPlace();
        return true;
    }
    const u64 multi_play = GetMatchingState();
    const s32 matching_state =
        multi_play >= 0x10000 && HasMemoryAccess(multi_play, 0x128, MemoryProt::CpuRead)
            ? ReadValue<s32>(multi_play, 0x124)
            : -1;
    if (!ShouldReviveHostInPlace(matching_state)) {
        return false;
    }
    ReviveDeadPartyHost(MakeReviveCalls());
    LOG_INFO(Debug,
             "Bloodborne live player died in a party session (state {}); revived without a "
             "world reload",
             matching_state);
    return true;
}

} // namespace Coop

namespace {

// The local player's data module ([[player +0x3B0] +0x20]: HP +0xF8, max HP +0xFC, flags +0x200).
u64 PlayerDataModule(u64 player) {
    const u64 modules = player >= 0x10000 ? ReadValue<u64>(player, 0x3B0) : 0;
    return modules >= 0x10000 ? ReadValue<u64>(modules, 0x20) : 0;
}

// The local player's attack rating for the weapon in its right hand, as the equipment menu shows
// it: UpdateWeaponAtk (0x018FCCB0, the character) works out one per weapon slot (physical + arcane
// + fire + bolt with scaling, upgrades and gems) into the character's PlayerGameData (its vtable
// +0x1C8), right hand 1 at +0x5E4 and right hand 2 at +0x5EC; ChrAsm ([player +0x4B8]) +0x10 says
// which right-hand weapon is held. On the game thread.
constexpr u64 UpdateWeaponAtkOffset = 0x018FCCB0;
struct AttackRatings {
    std::array<s32, 4> slots{-1, -1, -1, -1}; // left 1, right 1, left 2, right 2
    s32 right_hand = -1;
    std::array<s32, 4> weapons{-1, -1, -1, -1}; // ChrAsm +0x64: the weapon ids in the same order

    s32 Held() const {
        return right_hand == 1 ? slots[3] : right_hand == 0 ? slots[1] : 0;
    }
};
AttackRatings ReadAttackRatings(u64 player) {
    AttackRatings ratings;
    const u64 vtable = IsCharacter(player, "ReadAttackRatings") ? ReadValue<u64>(player, 0) : 0;
    const u64 game_data_of = vtable >= 0x10000 ? ReadValue<u64>(vtable, 0x1C8) : 0;
    const u64 asm_data = player >= 0x10000 ? ReadValue<u64>(player, 0x4B8) : 0;
    if (game_data_of < 0x10000 || asm_data < 0x10000) {
        return ratings;
    }
    static constexpr std::array<u8, 6> UpdatePrologue{0x55, 0x48, 0x89, 0xE5, 0x41, 0x57};
    if (std::ranges::equal(
            std::span<const u8>{reinterpret_cast<const u8*>(image_base + UpdateWeaponAtkOffset),
                                UpdatePrologue.size()},
            UpdatePrologue)) {
        reinterpret_cast<void PS4_SYSV_ABI (*)(u64)>(image_base + UpdateWeaponAtkOffset)(player);
    }
    const u64 player_data = reinterpret_cast<u64 PS4_SYSV_ABI (*)(u64)>(game_data_of)(player);
    if (player_data < 0x10000 || !HasMemoryAccess(player_data, 0x5F0, MemoryProt::CpuRead)) {
        return ratings;
    }
    for (u64 slot = 0; slot < 4; ++slot) {
        ratings.slots[slot] = ReadValue<s32>(player_data, 0x5E0 + slot * 4);
        ratings.weapons[slot] = ReadValue<s32>(asm_data, 0x64 + slot * 4);
    }
    ratings.right_hand = ReadValue<s32>(asm_data, 0x10);
    return ratings;
}

// Data module +0x200 flags a downed player wears: bit 1 skips any HP loss, bit 3 a hit's damage.
constexpr u8 DownedNoDamageFlags = 0x2 | 0x8;
// WorldChrManDbg's static "Player Hide" and "Player Silence" (debug menu flags): the AI's view of
// the local player (0x01A55390, 0x01A55230) takes it as unseen and unheard, so enemies leave a
// downed player alone.
constexpr u64 PlayerHideFlagOffset = 0x0553E88E;
constexpr u64 PlayerSilenceFlagOffset = 0x0553E88F;

// The enemies' targeting systems (see ForgetsTargetNode): the enemy one's vtable and its base's.
constexpr u64 EnemyTargetingVtableOffset = 0x0531B9F0;
constexpr u64 BaseTargetingVtableOffset = 0x05366280;

// Every enemy in this world forgets the characters with these handles (see ForgetsTargetNode);
// returns how many target nodes it changed.
u32 ForgetDownedTargets(std::span<const u32> handles) {
    const u64 world = ReadValue<u64>(image_base + CandidateLocalStatePointerOffset, 0);
    if (handles.empty() || world < 0x10000) {
        return 0;
    }
    u32 forgotten = 0;
    ForEachWorldCharacter(world, [&](u64 character) {
        const u64 manager = ReadValue<u64>(character, 0x3B8);
        const u64 ai = manager >= 0x10000 && HasMemoryAccess(manager, 0xB8, MemoryProt::CpuRead)
                           ? ReadValue<u64>(manager, 0xB0)
                           : 0;
        const u64 targeting = ai >= 0x10000 && HasMemoryAccess(ai, 0x568, MemoryProt::CpuRead)
                                  ? ReadValue<u64>(ai, 0x560)
                                  : 0;
        if (targeting < 0x10000 || !HasMemoryAccess(targeting, 0x20, MemoryProt::CpuRead)) {
            return;
        }
        const u64 vtable = ReadValue<u64>(targeting, 0);
        if (vtable != image_base + EnemyTargetingVtableOffset &&
            vtable != image_base + BaseTargetingVtableOffset) {
            return;
        }
        u64 node = ReadValue<u64>(targeting, 0x18);
        for (int count = 0;
             count < 64 && node >= 0x10000 && HasMemoryAccess(node, 0x54, MemoryProt::CpuWrite);
             ++count, node = ReadValue<u64>(node, 0x08)) {
            if (ForgetsTargetNode(ReadValue<u8>(node, 0x02), ReadValue<s32>(node, 0x28),
                                  ReadValue<u32>(node, 0x50), handles) &&
                ReadValue<float>(node, 0x2C) != ForgottenTargetTimer) {
                WriteValue(node, 0x2C, ForgottenTargetTimer);
                ++forgotten;
            }
        }
    });
    return forgotten;
}

// A downed member's body in this world (DownedBodyPicker: the one nearest where it posted, kept
// for the down while the reviver stands over it).
struct DownedBody {
    DownedMember member;
    u64 character = 0;
    u32 handle = 0;
};
std::vector<DownedBody> DownedBodies(DownedBodyPicker& picker, std::span<const MemberVitals> board,
                                     std::string_view self, u32 map,
                                     std::span<const OtherPlayer> players) {
    // The invader's body is never a downed member's (the nearest body to a downed member's spot
    // can be the invader standing over its kill).
    const std::vector<u32> invaders = InvaderHandles();
    std::vector<BodyPosition> positions;
    for (const OtherPlayer& other : players) {
        const u32 handle = ReadValue<u32>(other.character, 0x08);
        if (other.at && std::ranges::find(invaders, handle) == invaders.end()) {
            positions.push_back(BodyPosition{handle, *other.at});
        }
    }
    std::vector<DownedBody> bodies;
    for (const DownedBodyPick& pick : picker.Pick(board, self, map, positions)) {
        const auto other = std::ranges::find_if(players, [&](const OtherPlayer& player) {
            return ReadValue<u32>(player.character, 0x08) == pick.handle;
        });
        if (other != players.end()) {
            bodies.push_back(DownedBody{pick.member, other->character, pick.handle});
        }
    }
    return bodies;
}

// The handles of the downed players in this world: the local player while `down`, and the other
// downed members' bodies.
std::vector<u32> DownedPlayerHandles(u64 player, bool down, std::span<const DownedBody> others) {
    std::vector<u32> handles;
    if (down && player >= 0x10000) {
        handles.push_back(ReadValue<u32>(player, 0x08));
    }
    for (const DownedBody& body : others) {
        handles.push_back(body.handle);
    }
    return handles;
}

// What the pad reader saw (FilterPartyDeathPad): the buttons held before the downed filter, the
// attack presses so far and whether the last one was heavy (R2).
std::atomic<u32> party_pad_buttons{};
std::atomic<s64> party_lock_release_ms{-1}; // our last R3 letting go of a lock-on while down
std::atomic<u32> party_down_dropped{0};     // buttons a down took from the pad (logged)
std::atomic<u32> party_attack_presses{};
std::atomic<bool> party_attack_heavy{};

// Real revive hits (SHADPS4_BLOODBORNE_REVIVE_HITS=presses keeps counting attack presses instead):
// the hit hook queues what the local player's hits dealt to a downed body; the party tick counts
// it.
std::mutex real_hits_mutex;
std::vector<u32> real_hit_bodies;           // the downed bodies' handles, as of the last tick
std::vector<std::pair<u32, u32>> real_hits; // (body handle, damage)
std::atomic<bool> real_hits_installed{};

void PS4_SYSV_ABI RecordReviveHit(u64, const GuestRegisterSnapshot* registers) {
    const s32 damage = static_cast<s32>(registers->rax & 0xFFFFFFFF);
    const u64 module = registers->r14;
    if (damage <= 0 || module < 0x10000) {
        return;
    }
    const u64 attacker = ReadValue<u64>(registers->rbp + HitAttackerFrameOffset, 0);
    const u64 victim = ReadValue<u64>(module, 0x08);
    if (attacker < 0x10000 || victim < 0x10000) {
        return;
    }
    // Invasions: the invader's hit on the local player (its lethal check lets the death through,
    // KeepPartyMemberUp).
    if (victim == GetLocalPlayer() && attacker != victim &&
        IsInvaderHandle(ReadValue<u32>(attacker, 0x08))) {
        NoteInvaderHit();
        return;
    }
    if (attacker != GetLocalPlayer()) {
        return;
    }
    const u32 handle = ReadValue<u32>(victim, 0x08);
    std::scoped_lock lock{real_hits_mutex};
    if (std::ranges::find(real_hit_bodies, handle) != real_hit_bodies.end() &&
        real_hits.size() < 64) {
        real_hits.emplace_back(handle, static_cast<u32>(damage));
    }
}

// The downed bodies' handles for the enemy bar hook (SkipsEnemyBarDamage), as of the last tick.
std::mutex bar_bodies_mutex;
std::vector<u32> bar_bodies;

bool PS4_SYSV_ABI SkipDownedBodyBarDamage(u64, const GuestRegisterSnapshot* registers) {
    std::scoped_lock lock{bar_bodies_mutex};
    return SkipsEnemyBarDamage(static_cast<u32>(registers->rsi), bar_bodies);
}

// The downed bodies here take the invaders' team (DownedTeams) and give it back once up.
void SetDownedTeams(PartyDeathRun& run, std::span<const DownedBody> bodies,
                    std::span<const OtherPlayer> players) {
    {
        std::scoped_lock lock{bar_bodies_mutex};
        bar_bodies.clear();
        for (const DownedBody& body : bodies) {
            bar_bodies.push_back(body.handle);
        }
    }
    std::vector<CopyTeam> downed, present;
    for (const DownedBody& body : bodies) {
        downed.push_back(
            CopyTeam{body.handle, ReadValue<s32>(body.character, CharacterTeamOffset)});
    }
    for (const OtherPlayer& other : players) {
        present.push_back(CopyTeam{ReadValue<u32>(other.character, 0x08),
                                   ReadValue<s32>(other.character, CharacterTeamOffset)});
    }
    const std::vector<u32> invaders = InvaderHandles();
    for (const CopyTeam& write :
         run.downed_teams.Tick(downed, present, invaders, invasion_fallback_switch)) {
        for (const OtherPlayer& other : players) {
            if (ReadValue<u32>(other.character, 0x08) == write.handle &&
                HasMemoryAccess(other.character + CharacterTeamOffset, 4, MemoryProt::CpuWrite)) {
                WriteValue(other.character, CharacterTeamOffset, write.team);
                LOG_INFO(Debug, "Bloodborne party death: body {:#x} takes team {}", write.handle,
                         write.team);
            }
        }
    }
}

// The hits the local player landed on the downed bodies (RecordReviveHit) count for their revive.
void CountRealReviveHits(PartyDeathRun& run, std::span<const DownedBody> bodies) {
    std::vector<std::pair<u32, u32>> hits;
    {
        std::scoped_lock lock{real_hits_mutex};
        real_hit_bodies.clear();
        for (const DownedBody& body : bodies) {
            real_hit_bodies.push_back(body.handle);
        }
        hits.swap(real_hits);
    }
    for (const auto& [handle, damage] : hits) {
        const auto body = std::ranges::find(bodies, handle, &DownedBody::handle);
        if (body == bodies.end() || run.downed.Current() != Vital::Up) {
            continue;
        }
        const s32 rating = ReadAttackRatings(GetLocalPlayer()).Held();
        const bool heavy = party_attack_heavy.load();
        const u32 counted = run.revive_scale.Count(damage, rating, heavy);
        if (run.press_fallback.Landed(body->member.member, body->member.down,
                                      SteadyMilliseconds())) {
            continue; // its swing already counted (RevivePressFallback)
        }
        run.revive_hits.Hit(body->member.member, body->member.down, counted);
        if (run.real_hits_logged < 64) {
            ++run.real_hits_logged;
            LOG_INFO(Debug,
                     "Bloodborne party death: real revive hit on {} (down {}) for {} damage "
                     "(dealt {}, attack rating {}, {})",
                     body->member.member, body->member.down, counted, damage, rating,
                     heavy ? "heavy" : "light");
        }
    }
}

// Each tick: the downed bodies here take the invaders' team (DownedTeams) and give it back once up;
// the hits the local player landed on them count for their revive.
void StrikeDownedBodies(PartyDeathRun& run, std::span<const MemberVitals> board,
                        std::string_view self, u32 map, bool in_session) {
    if (!real_hits_installed.load(std::memory_order_relaxed)) {
        return;
    }
    const std::vector<OtherPlayer> players = OtherPlayers();
    const std::vector<DownedBody> bodies =
        in_session ? DownedBodies(run.downed_bodies, board, self, map, players)
                   : std::vector<DownedBody>{};
    SetDownedTeams(run, bodies, players);
    CountRealReviveHits(run, bodies);
}

// The hit's push and reaction (HitReactionOffset): none for the local player while it is down.
bool PS4_SYSV_ABI SkipDownedHitReaction(u64, const GuestRegisterSnapshot* registers) {
    if (!party_death_down.load(std::memory_order_relaxed) || registers->rdi < 0x10000) {
        return false;
    }
    return SkipsHitReaction(true, ReadValue<u64>(registers->rdi, 0x08), GetLocalPlayer());
}

// The fall damage step (0x01A35C33) takes a fall into the void as a fixed lethal height while
// [[character +0x58] +0x38] +0x48 bit 1 is set. Such a fall stays a real death: nobody could
// reach the player down there.
bool CharacterInTheVoid(u64 character) {
    const u64 control = character >= 0x10000 ? ReadValue<u64>(character, 0x58) : 0;
    const u64 state = control >= 0x10000 ? ReadValue<u64>(control, 0x38) : 0;
    return state >= 0x10000 && (ReadValue<u8>(state, 0x48) & 2) != 0;
}

bool FallsIntoTheVoid(u64 data) {
    return CharacterInTheVoid(ReadValue<u64>(data, 0x08));
}

// Diagnostics: IsNoDead's other callers that ask about the local player, once each.
void NoteOtherNoDeadCheck(u64 return_offset) {
    static std::mutex mutex;
    static std::unordered_set<u64> seen;
    std::scoped_lock lock{mutex};
    if (seen.size() < 64 && seen.insert(return_offset).second) {
        LOG_INFO(Debug, "Bloodborne party death: IsNoDead asked from {:#x} (left alone)",
                 return_offset);
    }
}

// IsNoDead's exit (see IsNoDeadExit): for the local player a lethal check answers "no dead"
// while it is armed or down, and each catch that puts it down is counted for the party tick.
void PS4_SYSV_ABI KeepPartyMemberUp(u64, GuestRegisterSnapshot* registers) {
    const u64 data = party_death_data.load(std::memory_order_relaxed);
    if (data == 0 || registers->rbx != data) {
        return;
    }
    u64 return_address = 0;
    std::memcpy(&return_address, reinterpret_cast<const void*>(registers->rbp + 8),
                sizeof(return_address));
    const u64 return_offset = return_address - image_base;
    NoDeadCheck check = ClassifyNoDeadCheck(return_offset);
    if (check == NoDeadCheck::Other) {
        NoteOtherNoDeadCheck(return_offset);
        return;
    }
    check = NoDeadCheckAt(check, ReadValue<s32>(data, 0xF8));
    if (check == NoDeadCheck::Other) {
        return; // a hit that left HP
    }
    bool void_fall = false;
    if (check == NoDeadCheck::Lethal && FallsIntoTheVoid(data)) {
        check = NoDeadCheck::Instant;
        void_fall = true;
    }
    const bool answer = (registers->r14 & 0xFF) != 0;
    // Kaniel 2026-10-25: in a duel a lethal hit puts the player out of it (TickDuel), never down
    // or dead: no party death, wipe or map reset.
    if (check == NoDeadCheck::Lethal && Coop::DuelCatchesLethal()) {
        if (!answer) {
            registers->r14 = (registers->r14 & ~u64{0xFF}) | 1;
            Coop::NoteDuelCatch();
        }
        return;
    }
    const bool down = party_death_down.load(std::memory_order_relaxed);
    // Invasions: the invader's kill is a real death, not a down (its vitals then say so).
    if (!answer && InvaderKillGoesThrough(check, InvaderHitFresh(), down)) {
        died_to_invader.store(true, std::memory_order_relaxed);
        return;
    }
    // A boss fight's catch takes the game's kills, not HP set outright (the revive's and the event
    // bindings' own writes ask too, 0x018BCD9C and 0x018BC5C5), nor our own kills.
    const bool hp_set = return_offset == 0x018BCD9C || return_offset == 0x018BC5C5;
    const bool own_kill = party_death_own_kill.load(std::memory_order_relaxed);
    const DeathCatch caught =
        CatchOf(check, party_death_armed.load(std::memory_order_relaxed), down, answer,
                !own_kill && !hp_set && party_death_boss_fight.load(std::memory_order_relaxed),
                !own_kill && party_spectating.load(std::memory_order_relaxed));
    if (caught == DeathCatch::None) {
        if (void_fall) {
            party_death_void_fall.store(true, std::memory_order_relaxed);
        }
        return;
    }
    registers->r14 = (registers->r14 & ~u64{0xFF}) | 1;
    if (down) {
        return;
    }
    if (caught == DeathCatch::BossFight) {
        party_death_boss_void.store(void_fall, std::memory_order_relaxed);
        party_death_boss_catches.fetch_add(1, std::memory_order_relaxed);
    } else {
        party_death_catches.fetch_add(1, std::memory_order_relaxed);
    }
}

// Kaniel 2026-10-25: the game's KillCharacter (KillCharacterOffset) asks IsNoDead only of a
// character linked at its data module +0xF0, never of the player itself: it writes 0 HP, sets the
// dead bit (+0x1E3) and goes on to the death step (0x018BD9D0). So an event's kill (an arena's kill
// plane, a pit) is caught at its entry: in a boss fight with a teammate in it, or while
// spectating, the local player's kill is skipped and spectates (our own kills go through).
bool kill_hook_installed = false;
bool PS4_SYSV_ABI SkipBossFightKill(u64, const GuestRegisterSnapshot* registers) {
    const u64 player = registers->rdi;
    if (party_death_data.load(std::memory_order_relaxed) == 0 ||
        party_death_own_kill.load(std::memory_order_relaxed) || player < 0x10000 ||
        player != GetLocalPlayer()) {
        return false;
    }
    const bool spectating = party_spectating.load(std::memory_order_relaxed);
    if (!spectating && !party_death_boss_fight.load(std::memory_order_relaxed)) {
        return false;
    }
    if (!spectating && !party_death_down.load(std::memory_order_relaxed)) {
        // A fall into a pit fades the body out as it goes (character +0x20C, DeathWasAVoidFall).
        party_death_boss_void.store(
            DeathWasAVoidFall(CharacterInTheVoid(player),
                              ReadValue<float>(player, CharacterFadeOffset)),
            std::memory_order_relaxed);
        party_death_boss_catches.fetch_add(1, std::memory_order_relaxed);
    }
    return true;
}

// The animation a downed player plays (DownedAnimation): none. Getting up from the kneel plays its
// stand-up (DownedStandAnimation), from another pose resets.
const s32 downed_animation = DownedAnimation(std::getenv("SHADPS4_BLOODBORNE_DOWNED_ANIMATION"));

} // namespace

namespace Coop {

bool KillCharacterCallable() {
    return kill_hook_installed ||
           std::ranges::equal(
               std::span<const u8>{reinterpret_cast<const u8*>(image_base + KillCharacterOffset),
                                   KillCharacterStart.size()},
               KillCharacterStart);
}

VitalsBoard vitals_board;
std::optional<std::chrono::steady_clock::time_point> vitals_heard_at;
std::vector<MemberVitals> hostile_vitals;

} // namespace Coop

namespace Coop {

void SetInvaderHandles(std::vector<u32> handles) {
    std::scoped_lock lock{invader_handles_mutex};
    invader_handles = std::move(handles);
}

std::vector<u32> InvaderHandles() {
    std::scoped_lock lock{invader_handles_mutex};
    return invader_handles;
}

bool IsInvaderHandle(u32 handle) {
    std::scoped_lock lock{invader_handles_mutex};
    return std::ranges::find(invader_handles, handle) != invader_handles.end();
}

void NoteInvaderHit() {
    invader_hit_ms.store(SteadyMilliseconds(), std::memory_order_relaxed);
}

bool InvaderHitFresh() {
    InvaderHitWindow window;
    if (const s64 hit = invader_hit_ms.load(std::memory_order_relaxed); hit >= 0) {
        window.Hit(hit);
    }
    return window.Fresh(SteadyMilliseconds());
}

bool TakeInvaderDeath() {
    return invader_death_noted.exchange(false);
}

} // namespace Coop

namespace {

// What the port's overlay draws (ReadPartyPanel), as of the last party tick.
std::mutex panel_mutex;
PartyPanel panel_now;

// Whether a member on the party panel is down: the nameplates' hook (ten tags a frame) does nothing
// else while none is.
std::atomic<bool> panel_has_downed{};

void SetPartyPanel(PartyPanel panel) {
    std::scoped_lock lock{panel_mutex};
    panel_has_downed.store(PanelHasDowned(panel), std::memory_order_relaxed);
    panel_now = std::move(panel);
}

// Party death's "in the session": only in the session's world. A guest still on its way (its
// summon load) posts "off" and dies as a solo player there (SessionArrival).
bool InPartyHere() {
    return InPartySession() && InSessionWorld();
}

// The other session members' board names (SessionBoardNames), read again every second, nothing
// out of a session or while one of them is not found: the lines of members out of this session
// are dropped from the board (OnlySessionMembers).
// Invasions: the invader is in the session too; its line (`hostile`) is a candidate, and its name
// is left out of the answer.
std::optional<std::vector<std::string>> SessionNames(std::span<const MemberVitals> board,
                                                     std::string_view self,
                                                     std::span<const MemberVitals> hostile) {
    static s64 read_at = std::numeric_limits<s64>::min();
    static std::optional<std::vector<std::string>> names;
    const s64 now = SteadyMilliseconds();
    if (read_at != std::numeric_limits<s64>::min() && now - read_at < 1'000) {
        return names;
    }
    read_at = now;
    names.reset();
    if (!InPartySession()) {
        return names;
    }
    std::vector<std::string> online_ids;
    for (const auto* lines : {&board, &hostile}) {
        for (const MemberVitals& member : *lines) {
            if (std::string id = OnlineIdOfBoardMember(member.member);
                member.member != self && !id.empty()) {
                online_ids.push_back(std::move(id));
            }
        }
    }
    std::vector<std::optional<std::string>> found;
    for (const u64 member : OtherSessionMembers()) {
        found.push_back(SessionMemberBoardName(member, online_ids));
    }
    names = SessionBoardNames(found);
    if (names) {
        std::erase_if(*names, [&](const std::string& name) {
            return std::ranges::find(hostile, name, &MemberVitals::member) != hostile.end();
        });
    }
    return names;
}

} // namespace

namespace Coop {

VitalsView ReadVitalsBoard() {
    VitalsView view;
    std::vector<MemberVitals> hostile;
    {
        std::scoped_lock lock{vitals_mutex};
        view = VitalsView{vitals_board, vitals_member};
        hostile = hostile_vitals;
        if (vitals_heard_at) {
            view.age_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                              std::chrono::steady_clock::now() - *vitals_heard_at)
                              .count();
        }
    }
    // The peers' own lines straight over the P2P link, fresher than the board's copies.
    std::vector<DirectVitals> direct;
    {
        std::scoped_lock lock{direct_boards_mutex};
        direct = direct_vitals_inbox.Fresh(SteadyMilliseconds());
    }
    if (!direct.empty()) {
        view.board = MergeDirectVitals(view.board, direct);
        view.age_ms = std::min<s64>(view.age_ms, 0);
    }
    // An invader's line never counts, even straight from its game (the server keeps those apart).
    if (const std::string invader = InvaderInWorld(); !invader.empty()) {
        std::erase_if(view.board.members,
                      [&](const MemberVitals& member) { return member.member == invader; });
    }
    view.board.members = OnlySessionMembers(view.board.members, view.member,
                                            SessionNames(view.board.members, view.member, hostile));
    return view;
}
PartyDeathRun party_death_run;

} // namespace Coop

namespace {

void SetPlayerFlag(u64 offset, u8 value) {
    WriteValue(image_base + offset, 0, value);
}

// The yellow YOU ARE DOWN banner (the party HUD's, in place of the game's PvP Draw banner): queued
// by the game's own SetTextEffect, as YOU DIED is. Without the party HUD it would show the Draw
// banner's own words, so it is left out where the HUD is not the port's (the emulator) or is off
// (BB_PARTY_HUD=0).
void ShowDownedBanner() {
#ifdef BLOODBORNE_PARTY_HUD
    static const bool party_hud = [] {
        const char* setting = std::getenv("BB_PARTY_HUD"); // run.py's switch for the patched HUD
        return setting == nullptr || std::string_view{setting} != "0";
    }();
    if (!party_hud) {
        return;
    }
    const auto* start = reinterpret_cast<const u8*>(image_base + SetTextEffectOffset);
    if (!std::ranges::equal(std::span<const u8>{start, SetTextEffectStart.size()},
                            SetTextEffectStart)) {
        LOG_ERROR(Debug, "Bloodborne party death: SetTextEffect is not where it should be; no "
                         "downed banner");
        return;
    }
    reinterpret_cast<void PS4_SYSV_ABI (*)(u64, s32)>(image_base + SetTextEffectOffset)(
        0, DownedBannerTextEffect);
#endif
}

void GoDown(PartyDeathRun& run, u64 data, s64 now_ms) {
    const u32 down = run.downed.GoDown(now_ms, ReadValue<s32>(data, 0xFC));
    run.give_up.Restart((party_pad_buttons.load() & PadCircle) != 0);
    party_death_down.store(true);
    const u8 flags = ReadValue<u8>(data, 0x200);
    run.flags_added = DownedNoDamageFlags & static_cast<u8>(~flags);
    WriteValue(data, 0x200, static_cast<u8>(flags | DownedNoDamageFlags));
    WriteValue(data, 0xF8, s32{1});
    run.hide_before = ReadValue<u8>(image_base + PlayerHideFlagOffset, 0);
    run.silence_before = ReadValue<u8>(image_base + PlayerSilenceFlagOffset, 0);
    SetPlayerFlag(PlayerHideFlagOffset, 1);
    SetPlayerFlag(PlayerSilenceFlagOffset, 1);
    ShowDownedBanner();
    // The pose waits until the player takes it (KeepDown).
    run.kneel_waiting = downed_animation > 0;
    run.down_since = now_ms;
    LOG_INFO(Debug,
             "Bloodborne party death: down {} on map {:#x}, {} damage to get up (max HP {}, "
             "pose {})",
             down, CurrentPlace().map, run.downed.DamageNeeded(), ReadValue<s32>(data, 0xFC),
             downed_animation);
}

// Guest memory for TakesAnimationRequest: 0 where it can't be read.
u64 ReadGuest(u64 address, size_t size) {
    if (address < 0x10000 || !HasMemoryAccess(address, size, MemoryProt::CpuRead)) {
        return 0;
    }
    return size == 1 ? ReadValue<u8>(address, 0) : ReadValue<u64>(address, 0);
}

// The downed pose, once the player takes an animation request (not in the lethal hit's middle).
void PoseWhenReady(PartyDeathRun& run, s64 now_ms) {
    const s64 down_for = now_ms - run.down_since;
    if (!run.kneel_waiting ||
        !KneelsNow(true, down_for, TakesAnimationRequest(GetLocalPlayer(), ReadGuest))) {
        return;
    }
    run.kneel_waiting = false;
    using ForcePlayAnimation = void PS4_SYSV_ABI (*)(u64 unused, s32 entity, s32 animation);
    reinterpret_cast<ForcePlayAnimation>(image_base + ForcePlayAnimationOffset)(
        0, LocalPlayerEntity, downed_animation);
    LOG_INFO(Debug, "Bloodborne party death: pose {} asked for {} ms into down {}",
             downed_animation, down_for, run.downed.DownId());
}

// Each tick while down: no damage, the revive's progress as HP (DownedHp), unseen and unheard
// (the game may undo any of them).
void KeepDown(PartyDeathRun& run, u64 data, s64 now_ms) {
    PoseWhenReady(run, now_ms);
    const u8 flags = ReadValue<u8>(data, 0x200);
    if ((flags & DownedNoDamageFlags) != DownedNoDamageFlags) {
        run.flags_added |= DownedNoDamageFlags & static_cast<u8>(~flags);
        WriteValue(data, 0x200, static_cast<u8>(flags | DownedNoDamageFlags));
    }
    const s32 hp =
        DownedHp(ReadValue<s32>(data, 0xFC), run.downed.DamageLanded(), run.downed.DamageNeeded());
    if (ReadValue<s32>(data, 0xF8) != hp) {
        WriteValue(data, 0xF8, hp);
    }
    SetPlayerFlag(PlayerHideFlagOffset, 1);
    SetPlayerFlag(PlayerSilenceFlagOffset, 1);
}

void EndDown(PartyDeathRun& run, u64 data) {
    party_death_down.store(false);
    WriteValue(data, 0x200, static_cast<u8>(ReadValue<u8>(data, 0x200) & ~run.flags_added));
    run.flags_added = 0;
    SetPlayerFlag(PlayerHideFlagOffset, run.hide_before);
    SetPlayerFlag(PlayerSilenceFlagOffset, run.silence_before);
}

void GetUp(PartyDeathRun& run, u64 data, s64 now_ms) {
    EndDown(run, data);
    const u8 flags = ReadValue<u8>(data, 0x200);
    run.grace_flags = DownedNoDamageFlags & static_cast<u8>(~flags);
    WriteValue(data, 0x200, static_cast<u8>(flags | DownedNoDamageFlags));
    run.grace_until = now_ms + ReviveGraceMs;
    const s32 hp = RevivedHp(ReadValue<s32>(data, 0xFC));
    WriteValue(data, 0xF8, hp);
    run.downed.GetUp(now_ms);
    // The stand-up only from the kneel, and only when the player takes it (else it is dropped).
    const bool knelt = downed_animation == DownedKneelAnimation && !run.kneel_waiting;
    const bool stood = knelt && TakesAnimationRequest(GetLocalPlayer(), ReadGuest);
    run.kneel_waiting = false;
    if (stood) {
        using ForcePlayAnimation = void PS4_SYSV_ABI (*)(u64 unused, s32 entity, s32 animation);
        reinterpret_cast<ForcePlayAnimation>(image_base + ForcePlayAnimationOffset)(
            0, LocalPlayerEntity, DownedStandAnimation);
    }
    if (downed_animation > 0 && !stood) {
        reinterpret_cast<void PS4_SYSV_ABI (*)(u64, s32)>(image_base + ChrResetAnimationOffset)(
            0, LocalPlayerEntity);
    }
    LOG_INFO(Debug, "Bloodborne party death: revived from down {} with {} HP (stood up {})",
             run.downed.DownId(), hp, stood);
}

// A real death from the down (a wipe, giving up, shadNet gone): the game's own kill, then the
// party death respawn (see MoveToRespawnPoint), which reloads the map if `reset`.
// The game's own kill of the local player (its death and respawn as any), else its HP to 0.
// Whether the game's kill ran.
bool KillLocalPlayer(u64 player, u64 data) {
    const bool killed = IsCharacter(player, "DieFromDown kill") && Coop::KillCharacterCallable();
    party_death_data.store(0); // a real death from here: the body is the game's (PartyDeathWatches)
    party_death_own_kill.store(true);
    if (killed) {
        reinterpret_cast<void PS4_SYSV_ABI (*)(u64 character)>(image_base +
                                                               KillCharacterOffset)(player);
    } else {
        WriteValue(data, 0xF8, s32{0});
    }
    party_death_own_kill.store(false);
    return killed;
}

void DieFromDown(PartyDeathRun& run, u64 player, u64 data, bool reset, std::string_view why) {
    EndDown(run, data);
    run.downed.Died();
    DiesWithReset(reset);
    const bool killed = KillLocalPlayer(player, data);
    LOG_INFO(Debug, "Bloodborne party death: {} ends down {}; dies (reset={}, game kill={})", why,
             run.downed.DownId(), reset, killed);
}

// One party death tick's view of the world (see TickPartyDeath).
struct DeathTick {
    u64 player = 0;
    u64 data = 0;
    s64 now = 0;
    bool in_session = false;
    u32 map = 0;
    VitalsView view;
    std::span<const MemberVitals> board;
    bool armed = false;
    bool alive = false;
    bool caught = false; // a lethal hit caught while standing (LethalCatches)
};
void StartSpectating(PartyDeathRun& run, const DeathTick& tick, bool boss_fight);

// Without party death the vitals board still says who stands on which map: a host back from a trip
// asks a member that was still joining for its data only once it stands here (FinishWaitingJoins;
// rig 2026-10-11, party death off: the guest stayed unseen).
void PostVitalsWithoutPartyDeath() {
    const u64 data = PlayerDataModule(GetLocalPlayer());
    if (data >= 0x10000 && HasMemoryAccess(data, 0x100, MemoryProt::CpuRead)) {
        const bool in_session = InPartyHere();
        PostVitals(MemberVitals{.map = in_session ? PlayingMap() : 0,
                                .vital = in_session ? Vital::Up : Vital::Off,
                                .hp = ReadValue<s32>(data, 0xF8),
                                .max = ReadValue<s32>(data, 0xFC),
                                .at = LocalPlayerPosition().value_or(LampPosition{})},
                   {});
    }
}

// The downed member whose body (as this game shows it, see DownedBodyPicker) stands within reach
// of this player, for a swing the board's positions put out of reach.
std::optional<DownedMember> SwingTargetByBody(PartyDeathRun& run, const DeathTick& tick,
                                              const LampPosition& me) {
    for (const DownedBody& body :
         DownedBodies(run.downed_bodies, tick.board, tick.view.member, tick.map, OtherPlayers())) {
        const auto pos = CharacterPosition(body.character);
        if (pos && std::hypot((*pos)[0] - me[0], (*pos)[1] - me[1], (*pos)[2] - me[2]) <=
                       ReviveHits::Reach) {
            return body.member;
        }
    }
    return std::nullopt;
}

// Standing (or not in party death): a caught lethal hit puts the player down, a death is real, and
// an attack near a downed member is a revive hit.
void TickStanding(PartyDeathRun& run, const DeathTick& tick) {
    const u32 boss_catches = party_death_boss_catches.load();
    const bool boss_caught = boss_catches != run.seen_boss_catches;
    run.seen_boss_catches = boss_catches;
    if ((tick.caught || boss_caught) && tick.alive) {
        // Kaniel 2026-10-25: the team's death mode (its leader's pick, down by default); a boss
        // fight's death the down does not catch spectates in either mode.
        if (boss_caught) {
            StartSpectating(run, tick, true);
        } else if (Coop::TeamDeathModeNow() == DeathMode::Spectate) {
            StartSpectating(run, tick, false);
        } else {
            GoDown(run, tick.data, tick.now);
        }
        return;
    }
    if (!tick.alive) {
        // A real death: one of the game's kills, or a lethal hit with nobody else standing.
        run.downed.Died();
        const bool reset = DiesWithReset(tick.in_session && DeathResetsMap(false, tick.armed));
        LOG_INFO(Debug, "Bloodborne party death: died for real on map {:#x} (reset={})", tick.map,
                 reset);
        // A fall into the void kills the player without the lethal check (rig 2026-10-22: the
        // Forbidden Woods' cliff west of the lamp; the void bit is clear by then) and fades it out
        // for good unless the map reloads: its respawn makes the trip (ChooseDeathRespawn).
        const float fade = ReadValue<float>(tick.player, CharacterFadeOffset);
        if (DeathWasAVoidFall(CharacterInTheVoid(tick.player), fade)) {
            party_death_void_fall.store(true, std::memory_order_relaxed);
            LOG_INFO(Debug, "Bloodborne party death: the death was a fall into the void (fade {})",
                     fade);
        }
        return;
    }
    const u32 attacks = party_attack_presses.load();
    const bool attacked = attacks != run.seen_attacks;
    run.seen_attacks = attacks;
    const bool real_hits = real_hits_installed.load(std::memory_order_relaxed);
    // Real hits: a swing near a downed member counts as a press only when its hit never lands on
    // the body (RevivePressFallback: a kneeling body took none, playtest 2026-10-13).
    for (const RevivePressFallback::Swing& swing : run.press_fallback.Due(tick.now)) {
        if (run.revive_hits.Press(swing.target.member, swing.target.down, swing.damage, tick.now)) {
            LOG_INFO(Debug,
                     "Bloodborne party death: revive swing at {} (down {}) counted for {} damage: "
                     "its hit did not land on the body",
                     swing.target.member, swing.target.down, swing.damage);
        }
    }
    const auto at = LocalPlayerPosition();
    auto target = attacked && tick.in_session && at
                      ? ReviveTarget(tick.board, tick.view.member, tick.map, *at)
                      : std::nullopt;
    if (!target && real_hits && attacked && tick.in_session && at) {
        // The body as this game shows it can stand away from where its player posted (a remote
        // player's position lags; rig down-revive3 2026-10-14: one swing of many counted).
        target = SwingTargetByBody(run, tick, *at);
        static u32 missed_logged = 0;
        if (!target && missed_logged < 20 &&
            OthersDownOnMap(tick.board, tick.view.member, tick.map).size()) {
            ++missed_logged;
            LOG_INFO(Debug,
                     "Bloodborne party death: a swing at ({:.1f}, {:.1f}, {:.1f}) reached no "
                     "downed body",
                     (*at)[0], (*at)[1], (*at)[2]);
        }
    }
    const AttackRatings ratings = target ? ReadAttackRatings(tick.player) : AttackRatings{};
    const s32 rating = ratings.Held();
    const bool heavy = party_attack_heavy.load();
    const u32 damage = ReviveHitDamage(rating, heavy);
    if (target && real_hits) {
        run.press_fallback.Swung(*target, damage, tick.now);
    } else if (target && run.revive_hits.Press(target->member, target->down, damage, tick.now)) {
        LOG_INFO(Debug,
                 "Bloodborne party death: revive hit on {} (down {}) for {} damage (attack "
                 "rating {}, heavy={}; ratings {} weapons {} right hand {})",
                 target->member, target->down, damage, rating, heavy, fmt::join(ratings.slots, " "),
                 fmt::join(ratings.weapons, " "), ratings.right_hand);
    }
}

// Down: killed anyway, or the end of the down (revived, a wipe, giving up, the board gone quiet).
void TickDown(PartyDeathRun& run, const DeathTick& tick) {
    if (!tick.alive) {
        // Killed anyway (a fall into the void, an event's kill).
        EndDown(run, tick.data);
        run.downed.Died();
        const bool reset = DiesWithReset(tick.in_session && DeathResetsMap(false, tick.armed));
        LOG_INFO(Debug, "Bloodborne party death: killed while down {} (reset={})",
                 run.downed.DownId(), reset);
        return;
    }
    const auto wipe = tick.view.Fresh() ? WipeFor(tick.view.board, tick.view.member) : std::nullopt;
    const bool wiped = wipe && *wipe != run.handled_wipe;
    const bool gave_up = run.give_up.Tick((party_pad_buttons.load() & PadCircle) != 0, tick.now);
    u32 damage = 0;
    for (const MemberVitals& entry : tick.board) {
        if (entry.member == tick.view.member && entry.down == run.downed.DownId()) {
            damage = entry.revive_damage;
        }
    }
    const bool revived = run.downed.Damage(damage, tick.now);
    run.downed.Tick(tick.now);
    KeepDown(run, tick.data, tick.now);
    // Out of the session for a moment is a load or a matching hiccup; for longer, nobody is left to
    // revive this player (the host quit or crashed, a kick).
    if (DownHasThePartyAround(tick.in_session, SessionMembers())) {
        run.down_out_of_session_since.reset();
    } else if (!run.down_out_of_session_since) {
        run.down_out_of_session_since = tick.now;
    }
    const bool session_gone = run.down_out_of_session_since &&
                              tick.now - *run.down_out_of_session_since >= DownSessionLostMs;
    switch (EndOfDown(wiped, gave_up, revived, tick.view.age_ms, !session_gone)) {
    case DownEnd::None:
        break;
    case DownEnd::Revive:
        GetUp(run, tick.data, tick.now);
        break;
    case DownEnd::Wipe:
        run.handled_wipe = *wipe;
        DieFromDown(run, tick.player, tick.data, true, fmt::format("wipe {}", *wipe));
        break;
    case DownEnd::GiveUp:
        DieFromDown(run, tick.player, tick.data,
                    tick.in_session && DeathResetsMap(false, tick.armed), "giving up");
        break;
    case DownEnd::BoardLost:
        DieFromDown(run, tick.player, tick.data, tick.in_session, "the vitals board going quiet");
        break;
    case DownEnd::SessionLost:
        run.down_out_of_session_since.reset();
        DieFromDown(run, tick.player, tick.data, false, "the party session ending");
        break;
    }
}

// Kaniel 2026-10-25: spectating, its body lies unseen, unheard and unhurt where it fell (Hide and
// Silence as a downed one's, the downed no-damage flags) and faded out (character +0x20C, as a
// fall into the void fades it): this screen shows the teammate watched, the others' show nothing
// of it (HideSpectatorCopies). The game may undo any of it.
void KeepSpectating(PartyDeathRun& run, u64 player, u64 data) {
    const u8 flags = ReadValue<u8>(data, 0x200);
    if ((flags & DownedNoDamageFlags) != DownedNoDamageFlags) {
        run.flags_added |= DownedNoDamageFlags & static_cast<u8>(~flags);
        WriteValue(data, 0x200, static_cast<u8>(flags | DownedNoDamageFlags));
    }
    if (ReadValue<s32>(data, 0xF8) < 1) {
        WriteValue(data, 0xF8, s32{1});
    }
    SetPlayerFlag(PlayerHideFlagOffset, 1);
    SetPlayerFlag(PlayerSilenceFlagOffset, 1);
    if (HasMemoryAccess(player + CharacterFadeOffset, sizeof(float), MemoryProt::CpuWrite) &&
        ReadValue<float>(player, CharacterFadeOffset) != 0.0f) {
        WriteValue(player, CharacterFadeOffset, 0.0f);
    }
}

// A member's character in this world (its handle): the other player nearest where it posted,
// within PresenceMetres; 0 for none.
u32 MemberCharacterHandle(std::span<const MemberVitals> board, std::string_view member,
                          std::span<const OtherPlayer> players, float within = PresenceMetres) {
    const auto entry = std::ranges::find(board, member, &MemberVitals::member);
    if (entry == board.end()) {
        return 0;
    }
    u32 handle = 0;
    float best = within;
    for (const OtherPlayer& other : players) {
        if (!other.at) {
            continue;
        }
        const float distance =
            std::hypot((*other.at)[0] - entry->at[0], (*other.at)[1] - entry->at[1],
                       (*other.at)[2] - entry->at[2]);
        if (distance <= best) {
            best = distance;
            handle = ReadValue<u32>(other.character, 0x08);
        }
    }
    return handle;
}

// The teammate watched after L1 / R1's `step` (SpectateTargets, NextSpectateTarget), and the
// camera over its shoulder.
void WatchTeammate(PartyDeathRun& run, const DeathTick& tick, int step) {
    const std::vector<std::string> targets =
        SpectateTargets(tick.board, tick.view.member, tick.map);
    const std::optional<std::string> next = NextSpectateTarget(targets, run.watched, step);
    const u32 handle = next ? MemberCharacterHandle(tick.board, *next, OtherPlayers()) : 0;
    if (next != run.watched) {
        LOG_INFO(Debug, "Bloodborne spectate: watching {} ({} of {} teammates standing here)",
                 next ? Coop::CharacterNameOf(*next) : std::string{"nobody"},
                 next ? std::ranges::find(targets, *next) - targets.begin() + 1 : 0,
                 targets.size());
    }
    if (next != run.watched || handle != run.watched_handle) {
        run.watched = next;
        run.watched_handle = handle;
        Coop::SetSpectateCamera(handle, next ? Coop::CharacterNameOf(*next) : std::string{});
    }
}

void StartSpectating(PartyDeathRun& run, const DeathTick& tick, bool boss_fight) {
    // Kaniel 2026-10-25: a fall into an arena's pit spectates from the ground it fell from (the
    // player's last ground, [player +0x400] +0x30, where its bloodstain would lie), not in the
    // void.
    party_death_void_fall.store(false);
    if (boss_fight && party_death_boss_void.exchange(false)) {
        const u64 ground = ReadValue<u64>(tick.player, 0x400);
        if (ground >= 0x10000 && HasMemoryAccess(ground + 0x30, 12, MemoryProt::CpuRead)) {
            const LampPosition at{ReadValue<float>(ground, 0x30), ReadValue<float>(ground, 0x34),
                                  ReadValue<float>(ground, 0x38)};
            TeleportLocalPlayer(at, LocalPlayerHeading().value_or(0.0f));
            LOG_INFO(Debug,
                     "Bloodborne spectate: fell into the void in a boss fight; the body waits on "
                     "its last ground ({:.1f}, {:.1f}, {:.1f})",
                     at[0], at[1], at[2]);
        }
    }
    run.downed.Spectate();
    run.give_up.Restart((party_pad_buttons.load() & PadCircle) != 0);
    party_death_down.store(true); // the hook keeps it alive; no hit's push or reaction
    spectate_steps.store(0);
    party_spectating.store(true);
    run.flags_added = 0;
    run.hide_before = ReadValue<u8>(image_base + PlayerHideFlagOffset, 0);
    run.silence_before = ReadValue<u8>(image_base + PlayerSilenceFlagOffset, 0);
    const float fade = ReadValue<float>(tick.player, CharacterFadeOffset);
    run.fade_before = fade > 0.0f ? fade : 1.0f;
    run.watched.reset();
    run.watched_handle = 0;
    run.alone_since.reset();
    run.down_out_of_session_since.reset();
    run.boss_watch.Restart();
    run.boss_read_at = 0;
    run.down_since = tick.now;
    KeepSpectating(run, tick.player, tick.data);
    WatchTeammate(run, tick, 0);
    LOG_INFO(Debug,
             "Bloodborne spectate: out of the fight on map {:#x} ({}): unseen where it fell, "
             "watching its teammates",
             tick.map, boss_fight ? "a death in a boss fight" : "the team's death mode");
}

void EndSpectating(PartyDeathRun& run, u64 player, u64 data) {
    party_spectating.store(false);
    party_death_down.store(false);
    WriteValue(data, 0x200, static_cast<u8>(ReadValue<u8>(data, 0x200) & ~run.flags_added));
    run.flags_added = 0;
    SetPlayerFlag(PlayerHideFlagOffset, run.hide_before);
    SetPlayerFlag(PlayerSilenceFlagOffset, run.silence_before);
    if (HasMemoryAccess(player + CharacterFadeOffset, sizeof(float), MemoryProt::CpuWrite)) {
        WriteValue(player, CharacterFadeOffset, run.fade_before);
    }
    run.watched.reset();
    run.watched_handle = 0;
    Coop::SetSpectateCamera(0, {});
}

// Kaniel 2026-10-25: a spectator wakes at its own last lamp (the one it last rested at, GlobalState
// +0x1528, not the host's), with its full HP and without a reload: moved there while the lamp is
// in a loaded block, else by the lamp's trip (ChooseDeathRespawn, as a death's respawn).
void WakeAtOwnLamp(PartyDeathRun& run, const DeathTick& tick, SpectateEnd why) {
    EndSpectating(run, tick.player, tick.data);
    const u8 flags = ReadValue<u8>(tick.data, 0x200);
    run.grace_flags = DownedNoDamageFlags & static_cast<u8>(~flags);
    WriteValue(tick.data, 0x200, static_cast<u8>(flags | DownedNoDamageFlags));
    run.grace_until = tick.now + ReviveGraceMs;
    const s32 hp = std::max(ReadValue<s32>(tick.data, 0xFC), 1);
    WriteValue(tick.data, 0xF8, hp);
    run.downed.GetUp(tick.now);
    const u64 global = GetGlobalState();
    const s32 point = global >= 0x10000 ? ReadValue<s32>(global, 0x1528) : -1;
    const u64 blocks = ReadValue<u64>(image_base + LoadedBlocksPointerOffset, 0);
    alignas(16) std::array<float, 4> position{};
    alignas(16) std::array<float, 4> rotation{};
    using Resolve = bool PS4_SYSV_ABI (*)(u64 blocks, float* position, float* rotation, s32 point);
    const bool in_loaded_block = point > 0 && blocks >= 0x10000 &&
                                 reinterpret_cast<Resolve>(image_base + ResolveRespawnPointOffset)(
                                     blocks, position.data(), rotation.data(), point);
    std::string_view how = "staying put";
    switch (ChooseDeathRespawn(point, in_loaded_block, false, false, false)) {
    case DeathRespawn::StayPut:
        break;
    case DeathRespawn::Travel:
        guest_trip_point = point;
        guest_lamp_trip_at = std::chrono::steady_clock::now() + std::chrono::milliseconds{200};
        how = "its trip";
        break;
    case DeathRespawn::MoveToPoint:
        how = PlaceLocalPlayer(position, rotation, "spectator wakes") ? "moved there" : "not moved";
        break;
    }
    LOG_INFO(Debug,
             "Bloodborne spectate: {} wakes this player at its own lamp {} ({}) with {} HP, no "
             "reload",
             SpectateEndName(why), point, how, hp);
}

// Spectating: killed anyway (a fall into the void, an event's kill), the party's wipe, or woken
// at its own lamp (a teammate's lamp, the boss down, giving up, nobody left).
void TickSpectating(PartyDeathRun& run, const DeathTick& tick, bool lamp_used) {
    if (!tick.alive) {
        EndSpectating(run, tick.player, tick.data);
        run.downed.Died();
        const bool reset = DiesWithReset(tick.in_session && DeathResetsMap(false, tick.armed));
        LOG_INFO(Debug, "Bloodborne spectate: killed while spectating (reset={})", reset);
        return;
    }
    KeepSpectating(run, tick.player, tick.data);
    WatchTeammate(run, tick, spectate_steps.exchange(0));
    SpectateCheck check;
    const auto wipe = tick.view.Fresh() ? WipeFor(tick.view.board, tick.view.member) : std::nullopt;
    check.wiped = wipe && *wipe != run.handled_wipe;
    check.gave_up = run.give_up.Tick((party_pad_buttons.load() & PadCircle) != 0, tick.now);
    check.teammate_used_a_lamp = lamp_used;
    // The bosses' defeat flags, a second apart (the flag store is read, not every frame).
    if (tick.now - run.boss_read_at >= 1'000) {
        run.boss_read_at = tick.now;
        std::vector<bool> dead;
        for (const EncounterInsight& boss : EncounterInsights()) {
            dead.push_back(EventFlagOn(boss.kill_event));
        }
        check.boss_died = run.boss_watch.Tick(dead);
    }
    if (tick.view.Fresh()) {
        SpectateCheckOthers(check, tick.board, tick.view.member, tick.map);
        if (check.someone_up || check.someone_down || check.someone_dead) {
            run.alone_since.reset();
        } else if (!run.alone_since) {
            run.alone_since = tick.now;
        }
        check.alone_ms = run.alone_since ? tick.now - *run.alone_since : 0;
    }
    if (DownHasThePartyAround(tick.in_session, SessionMembers()) &&
        tick.view.age_ms < BoardLostMs) {
        run.down_out_of_session_since.reset();
    } else if (!run.down_out_of_session_since) {
        run.down_out_of_session_since = tick.now;
    }
    check.party_lost = run.down_out_of_session_since &&
                       tick.now - *run.down_out_of_session_since >= DownSessionLostMs;
    const SpectateEnd end = EndOfSpectate(check);
    if (end == SpectateEnd::None) {
        return;
    }
    if (end == SpectateEnd::Wipe) {
        if (wipe) {
            run.handled_wipe = *wipe;
        }
        EndSpectating(run, tick.player, tick.data);
        run.downed.Died();
        DiesWithReset(true);
        const bool killed = KillLocalPlayer(tick.player, tick.data);
        LOG_INFO(Debug,
                 "Bloodborne spectate: the party's wipe ends spectating; dies (reset=true, "
                 "game kill={})",
                 killed);
        return;
    }
    WakeAtOwnLamp(run, tick, end);
}

// Kaniel 2026-10-25: the other members spectating on this map lie unseen in this world too: their
// characters here (the other player nearest where each posted, within DownedBodyPicker::Reach)
// faded out while they spectate, and faded back in once they do not. Their handles, for the
// enemies to forget.
std::vector<u32> HideSpectatorCopies(const DeathTick& tick) {
    static std::unordered_map<u64, float> faded; // character -> its fade before
    const bool someone_spectates = std::ranges::any_of(tick.board, [&](const MemberVitals& member) {
        return member.member != tick.view.member && member.vital == Vital::Spectate;
    });
    if (faded.empty() && !someone_spectates) {
        return {}; // nothing to hide or show again (every party tick: no world walk)
    }
    const std::vector<OtherPlayer> players = OtherPlayers();
    std::unordered_map<u64, float> now;
    std::vector<u32> handles;
    for (const MemberVitals& member : tick.board) {
        if (!tick.in_session || member.member == tick.view.member ||
            member.vital != Vital::Spectate || member.map != tick.map) {
            continue;
        }
        const u32 handle =
            MemberCharacterHandle(tick.board, member.member, players, DownedBodyPicker::Reach);
        const auto other = std::ranges::find_if(players, [&](const OtherPlayer& player) {
            return handle != 0 && ReadValue<u32>(player.character, 0x08) == handle;
        });
        if (other == players.end() || !HasMemoryAccess(other->character + CharacterFadeOffset,
                                                       sizeof(float), MemoryProt::CpuWrite)) {
            continue;
        }
        const auto before = faded.find(other->character);
        const float fade = ReadValue<float>(other->character, CharacterFadeOffset);
        now[other->character] = before != faded.end() ? before->second : fade > 0.0f ? fade : 1.0f;
        if (before == faded.end()) {
            LOG_INFO(Debug, "Bloodborne spectate: {} spectates: its character {:#x} here is unseen",
                     Coop::CharacterNameOf(member.member), handle);
        }
        if (fade != 0.0f) {
            WriteValue(other->character, CharacterFadeOffset, 0.0f);
        }
        handles.push_back(handle);
    }
    for (const auto& [character, fade] : faded) {
        const bool present = std::ranges::any_of(
            players, [&](const OtherPlayer& player) { return player.character == character; });
        if (!now.contains(character) && present &&
            HasMemoryAccess(character + CharacterFadeOffset, sizeof(float), MemoryProt::CpuWrite)) {
            WriteValue(character, CharacterFadeOffset, fade);
            LOG_INFO(Debug, "Bloodborne spectate: player {:#x} is back in the fight: shown again",
                     ReadValue<u32>(character, 0x08));
        }
    }
    faded = std::move(now);
    return handles;
}

// Enemies forget the downed players here (this one and the others), every tick while any is down:
// an enemy that sees a downed guest again (Hide is the local player's only) would take it as a
// target again.
void ForgetTargetsOnDowned(PartyDeathRun& run, const DeathTick& tick) {
    const std::vector<DownedBody> others_down =
        tick.in_session ? DownedBodies(run.downed_bodies, tick.board, tick.view.member, tick.map,
                                       OtherPlayers())
                        : std::vector<DownedBody>{};
    std::vector<u32> downed_handles = DownedPlayerHandles(
        tick.player, run.downed.Current() == Vital::Down || run.downed.Current() == Vital::Spectate,
        others_down);
    // Kaniel 2026-10-25: and the members spectating here.
    for (const u32 handle : HideSpectatorCopies(tick)) {
        downed_handles.push_back(handle);
    }
    if (const u32 forgotten = ForgetDownedTargets(downed_handles); forgotten > 0) {
        if (run.targets_forgotten == 0) {
            LOG_INFO(Debug,
                     "Bloodborne party death: enemies forgot {} target(s) on downed players "
                     "(self down={}, others down={})",
                     forgotten, run.downed.Current() == Vital::Down, others_down.size());
        }
        run.targets_forgotten += forgotten;
    } else if (downed_handles.empty()) {
        run.targets_forgotten = 0;
    }
}

// This member's vitals to the board and the party panel.
void PostVitalsAndPanel(PartyDeathRun& run, const DeathTick& tick) {
    const auto at = LocalPlayerPosition();
    const auto own_stain = Coop::ReadOwnBloodstain();
    PostVitals(
        MemberVitals{
            .map = tick.in_session ? tick.map : 0,
            .vital = tick.in_session ? run.downed.Current() : Vital::Off,
            .hp = ReadValue<s32>(tick.data, 0xF8),
            .max = ReadValue<s32>(tick.data, 0xFC),
            .down = run.downed.DownId(),
            .landed = run.downed.DamageLanded(),
            .needed = run.downed.DamageNeeded(),
            .at = at.value_or(LampPosition{}),
            .level = OwnCharacterLevel(),
            .boss = tick.alive && EventFlagOn(BossFightFlag),
            .killed_by_invader = run.downed.Current() == Vital::Dead && died_to_invader.load(),
            .bosses = tick.in_session ? OwnBossClaims() : std::vector<u32>{},
        },
        run.revive_hits.Totals(tick.board, tick.view.Fresh()));

    const Vital vital = run.downed.Current();
    PartyPanel panel = tick.in_session ? BuildPartyPanel(tick.board, tick.view.member, tick.map,
                                                         vital == Vital::Up ? at : std::nullopt)
                                       : PartyPanel{};
    // Phase 11: what the player sees is each one's character name.
    for (PanelMember& member : panel.members) {
        member.shown = Coop::CharacterNameOf(member.member);
    }
    panel.vital = tick.in_session ? vital : Vital::Off;
    panel.landed = run.downed.DamageLanded();
    panel.needed = run.downed.DamageNeeded();
    panel.give_up =
        vital == Vital::Down || vital == Vital::Spectate ? run.give_up.Progress(tick.now) : 0.0f;
    // Kaniel 2026-10-25: the teammate a spectator watches.
    panel.watching = vital == Vital::Spectate && run.watched
                         ? std::optional{Coop::CharacterNameOf(*run.watched)}
                         : std::nullopt;
    // Kaniel 2026-10-20: this player's own bloodstain (the map shows it out of a party too).
    panel.own_stain = own_stain.value_or(Bloodstain{});
    panel.map = tick.map;
    // Kaniel 2026-10-17, the compass: the members here where their characters stand now (the
    // board's positions are a moment old), from this player's spot and the camera's heading.
    if (tick.in_session && at) {
        std::vector<std::optional<LampPosition>> members, characters;
        for (const PanelMember& member : panel.members) {
            members.push_back(member.at);
        }
        for (const OtherPlayer& other : OtherPlayers()) {
            characters.push_back(other.at);
        }
        const auto live = LivePositions(members, characters, PresenceMetres);
        for (size_t i = 0; i < panel.members.size(); ++i) {
            if (live[i]) {
                panel.members[i].at = live[i];
            }
        }
        panel.me = at;
        panel.camera_heading = CameraHeadingNow();
    }
    SetPartyPanel(std::move(panel));
}

} // namespace

namespace Coop {

void TickPartyDeath() {
    if (!party_death_hook_installed) {
        PostVitalsWithoutPartyDeath();
        return;
    }
    auto& run = party_death_run;
    DeathTick tick;
    tick.player = GetLocalPlayer();
    tick.data = PlayerDataModule(tick.player);
    if (tick.data < 0x10000 || !HasMemoryAccess(tick.data, 0x204, MemoryProt::CpuWrite)) {
        party_death_data.store(0);
        SetPartyPanel({});
        return; // loading
    }
    tick.now = SteadyMilliseconds();
    tick.in_session = InPartyHere();
    tick.map = PlayingMap();
    tick.view = ReadVitalsBoard();
    party_death_others_stand.store(
        tick.view.Fresh() && SomeoneElseStandsAnywhere(tick.view.board.members, tick.view.member));
    tick.board = tick.view.Fresh() ? std::span<const MemberVitals>{tick.view.board.members}
                                   : std::span<const MemberVitals>{};
    tick.armed = tick.in_session && !tick.view.member.empty() &&
                 SomeoneElseStands(tick.board, tick.view.member, tick.map);
    party_death_armed.store(tick.armed);
    party_death_data.store(PartyDeathWatches(tick.in_session, run.downed.Current()) ? tick.data
                                                                                    : 0);
    // Kaniel 2026-10-25: a boss fight with a teammate in it (its deaths spectate).
    {
        const u64 holder = ReadValue<u64>(image_base + BossGaugesPointerOffset, 0);
        constexpr std::size_t Size = BossGaugeSlots * BossGaugeSize;
        const bool bars_up =
            holder >= 0x10000 &&
            HasMemoryAccess(holder + BossGaugesOffset, Size, MemoryProt::CpuRead) &&
            BossBarsUp({reinterpret_cast<const u8*>(holder + BossGaugesOffset), Size});
        static bool bars_were_up = false;
        if (bars_up != bars_were_up) {
            bars_were_up = bars_up;
            LOG_INFO(Debug, "Bloodborne party death: the boss bars are {}",
                     bars_up ? "up" : "down");
        }
        // Where the bar's boss stands: the fighters are those near it (a big arena's are apart).
        std::optional<LampPosition> boss;
        if (const auto handle =
                bars_up
                    ? BossBarHandle({reinterpret_cast<const u8*>(holder + BossGaugesOffset), Size})
                    : std::nullopt) {
            const u64 world = ReadValue<u64>(image_base + CandidateLocalStatePointerOffset, 0);
            if (world >= 0x10000) {
                ForEachWorldCharacter(world, [&](u64 character) {
                    if (!boss && ReadValue<u32>(character, 0x08) == *handle) {
                        boss = CharacterPosition(character);
                    }
                });
            }
        }
        const bool boss_fight =
            tick.in_session && BossFightCatchesDeath(bars_up, tick.board, tick.view.member,
                                                     tick.map, LocalPlayerPosition(), boss);
        if (party_death_boss_fight.exchange(boss_fight) != boss_fight) {
            LOG_INFO(Debug, "Bloodborne party death: boss fight with a teammate in it {} (bars {})",
                     boss_fight ? "on: a death the down does not catch spectates" : "off",
                     bars_up ? "up" : "down");
        }
    }
    // Kaniel 2026-10-25: a teammate's lamp wakes a spectator; counted from every tick's board.
    const bool lamp_used = tick.view.Fresh() && run.lamp_watch.Tick(tick.board, tick.view.member);
    ReadLocalAliveState(tick.player, tick.alive);
    // Every tick, before the switch below wakes a dead player: catches while down or dead go.
    tick.caught = run.lethal_catches.Take(party_death_catches.load(), run.downed.Current());
    if (const u32 uses = lamp_uses.load(); uses != run.seen_lamp_uses) {
        run.seen_lamp_uses = uses;
        run.downed.ClearPenalty();
    }

    if (run.grace_until && (tick.now >= *run.grace_until || run.downed.Current() != Vital::Up)) {
        WriteValue(tick.data, 0x200,
                   static_cast<u8>(ReadValue<u8>(tick.data, 0x200) & ~run.grace_flags));
        run.grace_until.reset();
        run.grace_flags = 0;
    }

    // An invader's hit that killed nobody counts for no later death.
    if (tick.alive && run.downed.Current() != Vital::Dead && !InvaderHitFresh()) {
        died_to_invader.store(false, std::memory_order_relaxed);
    }
    switch (run.downed.Current()) {
    case Vital::Off:
    case Vital::Up:
        TickStanding(run, tick);
        break;
    case Vital::Down:
        TickDown(run, tick);
        break;
    case Vital::Spectate:
        TickSpectating(run, tick, lamp_used);
        break;
    case Vital::Dead:
        if (!tick.alive && tick.in_session && tick.map != 0 && tick.view.Fresh() &&
            !tick.view.member.empty() &&
            party_death_reset.StillDead(SomeoneElseStands(tick.board, tick.view.member, tick.map),
                                        party_death_others_stand.load())) {
            LOG_INFO(Debug,
                     "Bloodborne party death: nobody else stands on map {:#x} before this "
                     "member's respawn: its respawn resets the map too",
                     tick.map);
        }
        if (tick.alive) {
            run.downed.Alive(tick.now);
            died_to_invader.store(false, std::memory_order_relaxed);
            party_death_reset.Cancel();
            LOG_INFO(Debug, "Bloodborne party death: up again after a real death");
        }
        break;
    }
    // Up (not down, not dead): its point now is the one from before any next death.
    if (const u64 global = GetGlobalState();
        tick.alive && global >= 0x10000 &&
        (run.downed.Current() == Vital::Up || run.downed.Current() == Vital::Off)) {
        std::scoped_lock lock{own_point_mutex};
        own_point_before_death.Sample(ReadValue<s32>(global, 0x1528), tick.now);
    }

    ForgetTargetsOnDowned(run, tick);
    StrikeDownedBodies(run, tick.board, tick.view.member, tick.map, tick.in_session);
    PostVitalsAndPanel(run, tick);
}

} // namespace Coop

namespace {

#ifdef BLOODBORNE_PARTY_HUD
// The name on a remote player's nameplate (its TagData's), "" when it cannot be read.
std::string NameplateName(u64 tag_data) {
    std::array<u8, GameHudStringSize> raw{};
    if (!HasMemoryAccess(tag_data + HudTagNameAt, raw.size(), MemoryProt::CpuRead)) {
        return {};
    }
    std::memcpy(raw.data(), reinterpret_cast<const void*>(tag_data + HudTagNameAt), raw.size());
    return GameHudString(raw, [](u64 at, size_t count) {
        std::vector<char16_t> chars(count);
        if (at < 0x10000 || !HasMemoryAccess(at, count * 2, MemoryProt::CpuRead)) {
            return std::vector<char16_t>{};
        }
        std::memcpy(chars.data(), reinterpret_cast<const void*>(at), count * 2);
        return chars;
    });
}

// Each change of a nameplate's name, in the log (the rig checks that the names are the members'):
// a name back after a blank (a member hidden off the map, then shown) is logged again.
void NoteNameplate(int index, const std::string& name) {
    // The HUD's thread calls this for each tag every frame: the same name as last time is no news.
    static std::array<std::string, 16> last;
    if (index < 0 || static_cast<size_t>(index) >= last.size() || last[index] == name) {
        return;
    }
    last[index] = name;
    static std::atomic<u32> logged{0};
    if (logged.fetch_add(1) < 400) {
        LOG_INFO(Debug, "Bloodborne party HUD: nameplate {} is \"{}\"", index, name);
    }
}

// An element's text as html, with its own SetText (the object interface's UTF-8 one).
void SetElementText(u64 element, const std::string& html) {
    if (!HasMemoryAccess(element, 0x50, MemoryProt::CpuRead) ||
        (ReadValue<u32>(element, HudElementTypeAt) & 0x8F) == 0) {
        return;
    }
    const u64 object = ReadValue<u64>(element, HudElementInterfaceAt);
    const u64 vtable = object >= 0x10000 ? ReadValue<u64>(object, 0) : 0;
    const u64 set_text = vtable >= 0x10000 ? ReadValue<u64>(vtable, HudSetTextUtf8Slot) : 0;
    if (set_text >= 0x10000) {
        reinterpret_cast<void PS4_SYSV_ABI (*)(u64, u64, const char*, bool)>(set_text)(
            object, ReadValue<u64>(element, HudElementDataAt), html.c_str(), true);
    }
}

// The HUD tag update's exit (HudTagUpdateExit): over a downed member's nameplate the role line says
// its revive, in the tag's own text field (the game writes it again every frame).
void PS4_SYSV_ABI WriteNameplateRoleLine(u64, const GuestRegisterSnapshot* registers) {
    const u64 tag = registers->r14, data = registers->r13;
    const auto index = RemoteTagIndex(data, image_base + HudFeDataOffset);
    if (!index) {
        return;
    }
    const std::string name = NameplateName(data);
    PartyPanel panel;
    const bool have_panel = ReadPartyPanel(panel);
    // Phase 11: the name over the member is its character's (the game writes its online id).
    const auto line = have_panel ? NameplateNameLine(panel, name) : std::nullopt;
    // One note per tag and frame: two (the id, then the character's name) alternated in the log
    // every frame and used up its lines (rig 2026-10-18: smoke's trip found no names).
    NoteNameplate(*index, line ? name + " shown as " + *line : name);
    if (!have_panel) {
        return;
    }
    if (line) {
        SetElementText(tag + HudTagNameLineAt, *line);
    }
    if (!panel_has_downed.load(std::memory_order_relaxed)) {
        return;
    }
    if (const auto line = NameplateRoleLine(panel, name)) {
        SetElementText(tag + HudTagRoleLineAt, *line);
    }
}
#endif

#ifdef BLOODBORNE_PARTY_HUD
// The native party list (the party HUD's CoopParty0-2), kept by the HUD's own thread.
struct NativePartyList {
    u64 hud = 0;        // the HUD the tags were made in
    u64 generation = 0; // which HUD made (hud_generation) that was
    bool ready = false;
    std::array<u8*, PartyListSize> tags{};
    std::array<std::array<u8, HudTagDataSize>, PartyListSize> data{};
    std::array<std::array<char16_t, 64>, PartyListSize> roles{}, names{};
    // CoopPartyIcon0-2's values (object interface, data), kept for the HUD's life; none without
    // them (a party HUD made without the icon strip). The same for the portraits (CoopPartyFace0-2)
    // and their badges (CoopPartyBadge0-2), none in a HUD made before them.
    using Sprites = std::array<std::pair<u64, u64>, PartyListSize>;
    Sprites icons{}, faces{}, badges{};
    std::array<u32, PartyListSize> icon_frames{}, face_frames{}, badge_frames{};
    // Kaniel 2026-10-18: CoopSupply0-2 (the HUD's own vial and bullet beside each name) and their
    // count fields' values (SupplyVials, SupplyBullets: PathRefs kept for the HUD's life), with
    // what each says now; none in a HUD without them (a mod's).
    Sprites supplies{};
    std::array<u32, PartyListSize> supply_frames{};
    std::array<std::array<u8*, 2>, PartyListSize> supply_texts{};
    std::array<std::array<std::string, 2>, PartyListSize> supply_said{};
    // CoopPrompt: the game's interact prompt widget made for it; null without it.
    u8* prompt = nullptr;
    std::array<u8, HudPromptDataSize> prompt_data{};
    std::array<char16_t, 64> prompt_text{};
};
std::atomic<s64> native_prompt_shown_ms{-1'000'000};
NativePartyList native_list;
std::atomic<u64> hud_generation{};

// The HUD's constructor (HudConstructOffset): one more HUD made.
void PS4_SYSV_ABI CountHudMade(u64, const GuestRegisterSnapshot*) {
    hud_generation.fetch_add(1, std::memory_order_relaxed);
}
std::atomic<s64> native_list_shown_ms{-1'000'000};
// The game's HUD ticked its tags (UpdatePartyList): it is up (not in a load or a cutscene).
std::atomic<s64> game_hud_ticked_ms{-1'000'000};

void PutUtf16(std::array<char16_t, 64>& out, const std::string& text) {
    const size_t length = std::min<size_t>(text.size(), out.size() - 1);
    for (size_t i = 0; i < length; ++i) {
        out[i] = static_cast<unsigned char>(text[i]);
    }
    out[length] = 0;
}

using HudLookup = void PS4_SYSV_ABI (*)(void* out, u64 parent, const char* format, ...);
using HudConstruct = void PS4_SYSV_ABI (*)(void* tag, void* path, s32, s32);
using HudRelease = void PS4_SYSV_ABI (*)(u64 object, void* value, u64 data);

// A looked-up value the made element holds its own copy of: released before the stack goes, as the
// game's own init does (see HudValueFate).
void ReleaseLookedUpValue(u8* path, u32 type) {
    if (HudValueFate(type, false) != HudValueKeep::Release) {
        return;
    }
    u64 object = 0, data = 0;
    std::memcpy(&object, path + HudPathRefInterfaceAt, sizeof(object));
    std::memcpy(&data, path + HudPathRefObjectAt, sizeof(data));
    reinterpret_cast<HudRelease>(ReadValue<u64>(ReadValue<u64>(object, 0), 0x18))(
        object, path + HudPathRefValueAt, data);
}

// The list's tags (CoopParty0-2), as the HUD's own init makes RemotePCTag0-6; false when the HUD
// has none.
bool MakeCoopPartyTags(u64 hud) {
    for (int i = 0; i < PartyListSize; ++i) {
        alignas(16) std::array<u8, HudPathRefSize> path{};
        reinterpret_cast<HudLookup>(image_base + HudChildLookupOffset)(path.data(), hud,
                                                                       "CoopParty%d", i);
        u32 type = 0;
        std::memcpy(&type, path.data() + HudPathRefTypeAt, sizeof(type));
        if ((type & 0x8F) == 0) {
            LOG_INFO(Debug,
                     "Bloodborne party HUD: no CoopParty{} in the HUD; the overlay keeps "
                     "the party list",
                     i);
            return false;
        }
        auto* tag = new (std::align_val_t{16}) u8[HudTagSize]{};
        reinterpret_cast<HudConstruct>(image_base + HudTagConstructorOffset)(tag, path.data(), 0,
                                                                             0);
        native_list.tags[i] = tag;
        // The tag holds its own value.
        ReleaseLookedUpValue(path.data(), type);
    }
    return true;
}

// The list's sprites (CoopPartyIcon0-2, CoopPartyFace0-2, CoopPartyBadge0-2, by `format`): their
// values stay referenced while the HUD lasts, so they live on the heap and are never freed: the
// HUD's destructor releases them there (a stack PathRef left a dead value in the HUD's list, rig
// wipe3 2026-10-12). One per sprite per HUD, as the tags. None without them all.
void FindListSprites(u64 hud, const char* format, NativePartyList::Sprites& sprites) {
    for (int i = 0; i < PartyListSize; ++i) {
        auto* path = new (std::align_val_t{16}) u8[HudPathRefSize]{};
        reinterpret_cast<HudLookup>(image_base + HudChildLookupOffset)(path, hud, format, i);
        u32 type = 0;
        std::memcpy(&type, path + HudPathRefTypeAt, sizeof(type));
        const bool found = (type & 0x8F) != 0;
        if (found) {
            std::memcpy(&sprites[i].first, path + HudPathRefInterfaceAt, 8);
            std::memcpy(&sprites[i].second, path + HudPathRefObjectAt, 8);
        }
        if (HudValueFate(type, found) == HudValueKeep::Drop) {
            ::operator delete[](path, std::align_val_t{16});
        }
        if (!found) {
            sprites = {};
            return;
        }
    }
}

// The text fields under `format` (a slash path from the HUD, one per list slot): their PathRefs,
// kept for the HUD's life (a value they hold is linked into the movie until it goes); all or none.
void FindListTexts(u64 hud, const char* format,
                   std::array<std::array<u8*, 2>, PartyListSize>& texts, int which) {
    for (int i = 0; i < PartyListSize; ++i) {
        auto* path = new (std::align_val_t{16}) u8[HudPathRefSize]{};
        reinterpret_cast<HudLookup>(image_base + HudChildLookupOffset)(path, hud, format, i);
        u32 type = 0;
        std::memcpy(&type, path + HudPathRefTypeAt, sizeof(type));
        if ((type & 0x8F) == 0) {
            ::operator delete[](path, std::align_val_t{16});
            for (auto& slot : texts) {
                slot[which] = nullptr;
            }
            return;
        }
        texts[i][which] = path;
    }
}

// A PathRef's text field set to `text` (plain), through its object interface's SetText.
void SetPathText(const u8* path, const std::string& text) {
    u64 object = 0, data = 0;
    std::memcpy(&object, path + HudPathRefInterfaceAt, 8);
    std::memcpy(&data, path + HudPathRefObjectAt, 8);
    const u64 vtable = object >= 0x10000 ? ReadValue<u64>(object, 0) : 0;
    const u64 set_text = vtable >= 0x10000 ? ReadValue<u64>(vtable, HudSetTextUtf8Slot) : 0;
    if (set_text >= 0x10000) {
        reinterpret_cast<void PS4_SYSV_ABI (*)(u64, u64, const char*, bool)>(set_text)(
            object, data, text.c_str(), false);
    }
}

void FindCoopPartyIcons(u64 hud) {
    FindListSprites(hud, "CoopPartyIcon%d", native_list.icons);
    FindListSprites(hud, "CoopPartyFace%d", native_list.faces);
    FindListSprites(hud, "CoopPartyBadge%d", native_list.badges);
    if (native_list.faces[0].first == 0 || native_list.badges[0].first == 0) {
        native_list.faces = {};
        native_list.badges = {};
    }
    FindListSprites(hud, "CoopSupply%d", native_list.supplies);
    if (native_list.supplies[0].first != 0) {
        FindListTexts(hud, "CoopSupply%d/SupplyVials", native_list.supply_texts, 0);
        FindListTexts(hud, "CoopSupply%d/SupplyBullets", native_list.supply_texts, 1);
    }
    if (native_list.supply_texts[0][0] == nullptr || native_list.supply_texts[0][1] == nullptr) {
        native_list.supplies = {};
    }
    LOG_INFO(Debug, "Bloodborne party HUD: vials and bullets in the HUD's own fields: {}",
             native_list.supplies[0].first != 0 ? "yes" : "no (the overlay draws them)");
}

// Kaniel 2026-10-18: the list shows its members' vials and bullets in the HUD's own fields.
std::atomic<s64> native_supplies_shown_ms{-1'000'000};

// The interact prompt (CoopPrompt), when the HUD has one.
void MakeCoopPrompt(u64 hud) {
    alignas(16) std::array<u8, HudPathRefSize> path{};
    reinterpret_cast<HudLookup>(image_base + HudChildLookupOffset)(path.data(), hud, "CoopPrompt");
    u32 type = 0;
    std::memcpy(&type, path.data() + HudPathRefTypeAt, sizeof(type));
    if ((type & 0x8F) != 0) {
        auto* prompt = new (std::align_val_t{16}) u8[HudPromptSize]{};
        reinterpret_cast<HudConstruct>(image_base + HudPromptConstructorOffset)(prompt, path.data(),
                                                                                0, 0);
        native_list.prompt = prompt;
    }
    LOG_INFO(Debug, "Bloodborne party HUD: the party prompt is the HUD's own: {}",
             native_list.prompt != nullptr ? "yes" : "no (no CoopPrompt)");
    // The prompt holds its own value.
    ReleaseLookedUpValue(path.data(), type);
}

// Makes the list's tags in this HUD as its own init makes RemotePCTag0-6: false when the HUD has no
// CoopParty tags (not the party HUD's), and the overlay keeps drawing the list.
bool MakePartyListTags(u64 hud) {
    if (!MakeCoopPartyTags(hud)) {
        return false;
    }
    FindCoopPartyIcons(hud);
    MakeCoopPrompt(hud);
    LOG_INFO(Debug,
             "Bloodborne party HUD: the party list is the HUD's own ({} tags, icons {}, portraits "
             "{}; their values kept on the heap)",
             PartyListSize, native_list.icons[0].first != 0 ? "too" : "none",
             native_list.faces[0].first != 0 ? "too" : "none");
    return true;
}

// The party prompt (CoopPrompt), as the game updates its own interact prompt, its words ours.
void UpdatePartyPrompt(const PartyPanel& panel, float frame_time) {
    if (native_list.prompt == nullptr) {
        return;
    }
    // The allocator of a nameplate's role line (RemotePCTag0's): the game's own prompt data has
    // none until the game shows a prompt itself (Kaniel 2026-10-12: the prompt never showed).
    std::array<u8, GameHudStringFullSize> string{};
    std::memcpy(
        string.data(),
        reinterpret_cast<const void*>(image_base + HudFeDataOffset + HudRemoteTagsAt + 0x28),
        string.size());
    u64 allocator = 0;
    std::memcpy(&allocator, string.data() + 0x28, 8);
    if (allocator < 0x10000) {
        return;
    }
    const PartyPrompt prompt = PartyPromptFor(panel);
    // Each new prompt in the log (the rig checks its words: website shots 2026-10-19 found an
    // online id in it).
    static std::string last_prompt;
    static u32 prompts_logged = 0;
    if (prompt.text != last_prompt && prompts_logged < 200) {
        last_prompt = prompt.text;
        ++prompts_logged;
        LOG_INFO(Debug, "Bloodborne party HUD: prompt \"{}\"", prompt.text);
    }
    PutUtf16(native_list.prompt_text, prompt.text);
    WritePromptData(native_list.prompt_data, string, prompt,
                    reinterpret_cast<u64>(native_list.prompt_text.data()), prompt.text.size());
    reinterpret_cast<void PS4_SYSV_ABI (*)(u8*, u8*, float)>(image_base + HudPromptUpdateOffset)(
        native_list.prompt, native_list.prompt_data.data(), frame_time);
    if (prompt.shown) {
        native_prompt_shown_ms.store(SteadyMilliseconds(), std::memory_order_relaxed);
    }
}

// A list sprite stops at `frame` (the sprite would play through its frames otherwise).
void ShowListSprite(const NativePartyList::Sprites& sprites, std::array<u32, PartyListSize>& frames,
                    int index, u32 frame) {
    const auto [object, value] = sprites[index];
    if (object < 0x10000 || frame == frames[index]) {
        return;
    }
    frames[index] = frame;
    const u64 goto_frame = ReadValue<u64>(ReadValue<u64>(object, 0), HudGotoFrameSlot);
    reinterpret_cast<void PS4_SYSV_ABI (*)(u64, u64, u32, bool)>(goto_frame)(object, value, frame,
                                                                             true);
}

// The list slot's icon, portrait and badge (PartyListArtFor).
void ShowListArt(int index, const PartyListArt& art) {
    ShowListSprite(native_list.icons, native_list.icon_frames, index, art.icon);
    ShowListSprite(native_list.faces, native_list.face_frames, index, art.face);
    ShowListSprite(native_list.badges, native_list.badge_frames, index, art.badge);
}

// Where the HUD tick has updated its own tags (HudTicksTagsDone): the list's tags, from the panel.
// A new HUD (after a load) gets new tags; the old HUD's are left as they are.
void PS4_SYSV_ABI UpdatePartyList(u64, const GuestRegisterSnapshot* registers) {
    const u64 hud = registers->r15;
    if (hud < 0x10000) {
        return;
    }
    game_hud_ticked_ms.store(SteadyMilliseconds(), std::memory_order_relaxed);
    const u64 generation = hud_generation.load(std::memory_order_relaxed);
    if (PartyListOutdated(hud, generation, native_list.hud, native_list.generation)) {
        native_list = NativePartyList{.hud = hud, .generation = generation};
        native_list.ready = MakePartyListTags(hud);
    }
    if (!native_list.ready) {
        return;
    }
    const float frame_time = ReadValue<float>(registers->rbp + HudFrameTimeFrameOffset, 0);
    // The game's own TagData (RemotePCTag0's): its strings' allocators for ours.
    const u64 game_data = image_base + HudFeDataOffset + HudRemoteTagsAt;
    std::array<u8, HudTagDataSize> game{};
    std::memcpy(game.data(), reinterpret_cast<const void*>(game_data), game.size());
    u64 role_allocator = 0, name_allocator = 0;
    std::memcpy(&role_allocator, game.data() + 0x28 + 0x28, 8);
    std::memcpy(&name_allocator, game.data() + HudTagNameAt + 0x28, 8);
    if (role_allocator < 0x10000 || name_allocator < 0x10000) {
        // The game's strings are not made yet: the tags stay hidden (they start shown, with the
        // sprite's own placeholder words).
        for (int i = 0; i < PartyListSize; ++i) {
            reinterpret_cast<void PS4_SYSV_ABI (*)(u8*, bool)>(image_base + HudSetVisibleOffset)(
                native_list.tags[i], false);
            ShowListArt(i, PartyListArt{});
        }
        return;
    }
    PartyPanel panel;
    ReadPartyPanel(panel);
    const auto entries = PartyListEntries(panel);
    using Update = void PS4_SYSV_ABI (*)(u8* tag, u8* data, float frame_time);
    bool shown = false;
    for (int i = 0; i < PartyListSize; ++i) {
        PutUtf16(native_list.roles[i], entries[i].role);
        PutUtf16(native_list.names[i], entries[i].name);
        const auto [x, y] = PartyListPosition(i);
        WriteTagData(native_list.data[i], game, entries[i], x, y,
                     reinterpret_cast<u64>(native_list.roles[i].data()),
                     reinterpret_cast<u64>(native_list.names[i].data()));
        reinterpret_cast<Update>(image_base + HudTagUpdateOffset)(
            native_list.tags[i], native_list.data[i].data(), frame_time);
        if (entries[i].visible) {
            SetElementText(reinterpret_cast<u64>(native_list.tags[i]) + HudTagNameLineAt,
                           entries[i].name_line);
        }
        // A member's portrait, when it came and the HUD has the slots for it
        // (bloodborne_portrait.h).
        const bool has_faces = native_list.faces[0].first != 0;
        const bool portrait = Coop::ShowPartyPortrait(
            i, has_faces && entries[i].visible ? std::string_view{entries[i].member} : "");
        ShowListArt(i, PartyListArtFor(entries[i], has_faces && portrait));
        if (native_list.supplies[0].first != 0) {
            ShowListSprite(native_list.supplies, native_list.supply_frames, i,
                           SupplyFrame(entries[i]));
            const auto texts = SupplyTexts(entries[i]);
            for (int which = 0; which < 2; ++which) {
                if (native_list.supply_said[i][which] != texts[which] ||
                    native_list.supply_said[i][which].empty()) {
                    SetPathText(native_list.supply_texts[i][which], texts[which]);
                    native_list.supply_said[i][which] = texts[which];
                }
            }
            native_supplies_shown_ms.store(SteadyMilliseconds(), std::memory_order_relaxed);
        }
        shown = shown || entries[i].visible;
    }
    if (shown) {
        native_list_shown_ms.store(SteadyMilliseconds(), std::memory_order_relaxed);
    }
    UpdatePartyPrompt(panel, frame_time);
}

#endif

// The party HUD's nameplates (the port's, BB_PARTY_HUD not 0): a downed member's says its revive.
void InstallPartyNameplates(u64 base, u64 image_size) {
#ifdef BLOODBORNE_PARTY_HUD
    const char* setting = std::getenv("BB_PARTY_HUD");
    if (setting != nullptr && std::string_view{setting} == "0") {
        return;
    }
    if (HudTagUpdateExitOffset + HudTagUpdateExit.size() > image_size ||
        !InstallCoopHook(reinterpret_cast<void*>(base + HudTagUpdateExitOffset), HudTagUpdateExit,
                         0, WriteNameplateRoleLine)) {
        coop_features.Skipped("party nameplates",
                              SiteFailure(base, image_size, HudTagUpdateExitOffset));
        return;
    }
    coop_features.On("party nameplates");
    // Without it a new HUD is told by its address only.
    if (HudConstructOffset + HudConstructPrologue.size() > image_size ||
        !InstallCoopHook(reinterpret_cast<void*>(base + HudConstructOffset), HudConstructPrologue,
                         0, CountHudMade)) {
        LOG_WARNING(Debug, "Bloodborne party HUD: no hook on the HUD's constructor ({})",
                    SiteFailure(base, image_size, HudConstructOffset));
    }
    if (HudTicksTagsDoneOffset + HudTicksTagsDone.size() > image_size ||
        !InstallCoopHook(reinterpret_cast<void*>(base + HudTicksTagsDoneOffset), HudTicksTagsDone,
                         0, UpdatePartyList)) {
        coop_features.Skipped("party list in the HUD",
                              SiteFailure(base, image_size, HudTicksTagsDoneOffset));
        return;
    }
    coop_features.On("party list in the HUD");
#else
    (void)base;
    (void)image_size;
#endif
}

} // namespace

namespace Coop {

// Party death needs a party (SHADPS4_BLOODBORNE_PARTY) and the setting.
void InstallPartyDeath(u64 base, u64 image_size) {
    if (!party_death_enabled || !party_controller) {
        return;
    }
    if (IsNoDeadExitOffset + IsNoDeadExit.size() > image_size ||
        !InstallCoopHook(reinterpret_cast<void*>(base + IsNoDeadExitOffset), IsNoDeadExit, 0,
                         KeepPartyMemberUp)) {
        LOG_ERROR(Debug, "Bloodborne party death hook unavailable; deaths stay as they are");
        coop_features.Skipped("party death", SiteFailure(base, image_size, IsNoDeadExitOffset));
        return;
    }
    party_death_hook_installed = true;
    coop_features.On("party death");
    InstallPartyNameplates(base, image_size);
    if (HitReactionOffset + HitReactionPrologue.size() <= image_size &&
        InstallCoopHook(reinterpret_cast<void*>(base + HitReactionOffset), HitReactionPrologue, 0,
                        SkipDownedHitReaction)) {
        coop_features.On("no hit reaction while down");
    } else {
        coop_features.Skipped("no hit reaction while down",
                              SiteFailure(base, image_size, HitReactionOffset));
    }
    if (KillCharacterOffset + KillCharacterStart.size() <= image_size &&
        InstallCoopHook(reinterpret_cast<void*>(base + KillCharacterOffset), KillCharacterStart, 0,
                        GuestFunctionOverride{SkipBossFightKill})) {
        kill_hook_installed = true;
        coop_features.On("boss fight deaths");
    } else {
        coop_features.Skipped("boss fight deaths",
                              SiteFailure(base, image_size, KillCharacterOffset));
    }
    if (EnemyBarDamageOffset + EnemyBarDamageStart.size() <= image_size &&
        InstallCoopHook(reinterpret_cast<void*>(base + EnemyBarDamageOffset), EnemyBarDamageStart,
                        0, GuestFunctionOverride{SkipDownedBodyBarDamage})) {
        coop_features.On("no damage number on downed bodies");
    } else {
        coop_features.Skipped("no damage number on downed bodies",
                              SiteFailure(base, image_size, EnemyBarDamageOffset));
    }
    // The hit hook also notes the invader's hits (an invasion's kill is a real death), so it goes
    // in with the presses too; only the revive counting follows the setting.
    const char* hits = std::getenv("SHADPS4_BLOODBORNE_REVIVE_HITS");
    const bool hit_hook = HitDamageStoredOffset + HitDamageStored.size() <= image_size &&
                          InstallCoopHook(reinterpret_cast<void*>(base + HitDamageStoredOffset),
                                          HitDamageStored, 0, RecordReviveHit);
    if (hits != nullptr && std::string_view{hits} == "presses") {
        coop_features.Skipped("real revive hits", "SHADPS4_BLOODBORNE_REVIVE_HITS=presses");
    } else if (hit_hook) {
        real_hits_installed.store(true);
        coop_features.On("real revive hits");
    } else {
        coop_features.Skipped("real revive hits",
                              SiteFailure(base, image_size, HitDamageStoredOffset));
    }
    LOG_INFO(Debug, "Bloodborne party death: a lethal hit puts a party member down while another "
                    "stands on its map (damage revives it, holding Circle gives up)");
}

} // namespace Coop

void FilterPartyDeathPad(std::uint32_t& buttons, std::array<std::uint8_t, 4>& sticks,
                         std::uint8_t& l2, std::uint8_t& r2) {
    if (!party_death_hook_installed) {
        return;
    }
    const u32 before = party_pad_buttons.exchange(buttons, std::memory_order_relaxed);
    if (AttackPressed(before, buttons)) {
        party_attack_heavy.store(HeavyAttackPressed(before, buttons), std::memory_order_relaxed);
        party_attack_presses.fetch_add(1, std::memory_order_relaxed);
    }
    if (!party_death_down.load(std::memory_order_relaxed)) {
        party_lock_release_ms.store(-1, std::memory_order_relaxed);
        party_down_dropped.store(0, std::memory_order_relaxed);
        return;
    }
    const PadInput pad{.buttons = buttons, .sticks = sticks, .l2 = l2, .r2 = r2};
    PadInput kept;
    if (party_spectating.load(std::memory_order_relaxed)) {
        // Kaniel 2026-10-25: a spectator's pad drives only its camera (SpectatorPadOf).
        const SpectatorPad spectator = SpectatorPadOf(pad, before);
        if (spectator.step != 0) {
            spectate_steps.fetch_add(spectator.step, std::memory_order_relaxed);
        }
        Coop::CoopCameraTurn(spectator.turn_x, spectator.turn_y);
        kept = spectator.game;
    } else {
        kept = DownedPad(pad);
    }
    // Each button a down takes, once (the rig checks Options and the rest do nothing).
    const u32 dropped = buttons & ~kept.buttons;
    if ((dropped & ~party_down_dropped.fetch_or(dropped, std::memory_order_relaxed)) != 0) {
        LOG_INFO(Debug, "Bloodborne party death: down, the pad dropped buttons {:#x}", dropped);
    }
    buttons = kept.buttons;
    // A lock-on held while down is let go (the HUD's cursor says it is held).
    const s64 now = SteadyMilliseconds();
    const s64 last = party_lock_release_ms.load(std::memory_order_relaxed);
    if (image_base != 0 &&
        ReleasesLockOn(ReadValue<s32>(image_base + HudFeDataOffset, HudLockCursorAt), now,
                       last >= 0 ? std::optional<s64>{last} : std::nullopt)) {
        buttons |= PadR3;
        if (last < 0) {
            LOG_INFO(Debug, "Bloodborne party death: down, letting go of a lock-on (R3)");
        }
        party_lock_release_ms.store(now, std::memory_order_relaxed);
    }
    sticks = kept.sticks;
    l2 = kept.l2;
    r2 = kept.r2;
}

bool NativePartyListShown() {
#ifdef BLOODBORNE_PARTY_HUD
    return SteadyMilliseconds() - native_list_shown_ms.load(std::memory_order_relaxed) < 500;
#else
    return false;
#endif
}

bool NativePartySuppliesShown() {
#ifdef BLOODBORNE_PARTY_HUD
    return SteadyMilliseconds() - native_supplies_shown_ms.load(std::memory_order_relaxed) < 500;
#else
    return false;
#endif
}

bool GameHudUp() {
#ifdef BLOODBORNE_PARTY_HUD
    return SteadyMilliseconds() - game_hud_ticked_ms.load(std::memory_order_relaxed) < 500;
#else
    return true;
#endif
}

bool NativePartyPromptShown() {
#ifdef BLOODBORNE_PARTY_HUD
    return SteadyMilliseconds() - native_prompt_shown_ms.load(std::memory_order_relaxed) < 500;
#else
    return false;
#endif
}

bool ReadCompass(CompassView& view) {
    view = {};
    PartyPanel panel;
    ReadPartyPanel(panel);
    if (!panel.me) {
        view.why = "no party session here";
        return false;
    }
    if (!panel.camera_heading) {
        view.why = "the camera is not found";
        return false;
    }
    if (!GameHudUp()) {
        view.why = "the game's HUD is not up";
        return false;
    }
    std::vector<std::optional<LampPosition>> members;
    std::vector<bool> down;
    for (const PanelMember& member : panel.members) {
        members.push_back(member.at);
        down.push_back(member.vital == Vital::Down);
    }
    view.heading = *panel.camera_heading;
    view.markers = CompassMarkers(members, *panel.me, view.heading, down);
    for (const CompassMarker& marker : view.markers) {
        const PanelMember& member = panel.members[marker.member];
        view.names.push_back(member.shown); // "" before it is known: the overlay says Hunter
        view.members.push_back(member.member);
        view.vitals.push_back(static_cast<int>(member.vital));
        view.revives.push_back(ReviveShare(member.landed, member.needed));
    }
    // Kaniel 2026-10-20: this player's own bloodstain on this map (only its own: Kaniel after
    // v1.0.2).
    const Bloodstain& own = panel.own_stain;
    view.stain = CompassStainOf(panel.map != 0 && own.map == panel.map ? std::optional{own.at}
                                                                       : std::nullopt,
                                *panel.me, view.heading);
    if (view.markers.empty() && !view.stain) {
        view.why = "nobody else on this map";
    }
    return !view.markers.empty() || view.stain.has_value();
}

bool ReadPartyPanel(PartyPanel& panel) {
    std::scoped_lock lock{panel_mutex};
    panel = panel_now;
    return !panel.members.empty() || panel.vital == Vital::Down || panel.vital == Vital::Spectate;
}

namespace Coop {

// Kaniel's play test 2026-10-19: the Blood-starved Beast never went for Lisara. On change, each
// other player here with the first AI rule that leaves it out of the enemies' targets
// (TargetCandidateBlock), and the live characters each enemy within 30 m holds as targets.
void LogEnemyTargets() {
    static std::string last;
    static std::chrono::steady_clock::time_point checked{};
    const auto now = std::chrono::steady_clock::now();
    if (now - checked < std::chrono::seconds{1}) {
        return;
    }
    checked = now;
    const u64 world = ReadValue<u64>(image_base + CandidateLocalStatePointerOffset, 0);
    const auto me = LocalPlayerPosition();
    if (world < 0x10000 || !me) {
        return;
    }
    std::vector<u64> listed;
    if (const u64 search = ReadValue<u64>(image_base + AiTargetListPointerOffset, 0);
        search >= 0x10000 && HasMemoryAccess(search, 0x18, MemoryProt::CpuRead)) {
        const u64 begin = ReadValue<u64>(search, 0x08);
        const u64 end = ReadValue<u64>(search, 0x10);
        const bool readable = begin >= 0x10000 && end > begin && end - begin <= 256 * 8 &&
                              HasMemoryAccess(begin, end - begin, MemoryProt::CpuRead);
        for (u64 at = begin; readable && at < end; at += 8) {
            listed.push_back(ReadValue<u64>(at, 0));
        }
    }
    std::string text;
    // This player too: the one the enemies here do go for, to hold the others against.
    std::vector<std::pair<u64, s32>> players;
    if (const u64 local = GetLocalPlayer(); local >= 0x10000) {
        players.emplace_back(local, ReadValue<s32>(local, 0x78));
    }
    for (const OtherPlayer& other : OtherPlayers()) {
        players.emplace_back(other.character, other.type);
    }
    for (const auto& [character, type] : players) {
        const u64 entry = ReadValue<u64>(character, 0x30);
        const u64 control = ReadValue<u64>(character, 0x18);
        const u64 info = control >= 0x10000 ? ReadValue<u64>(control, 0x30) : 0;
        const TargetCandidate candidate{
            .state = entry >= 0x10000 ? ReadValue<s32>(entry, 0x18) : -1,
            .built = ReadValue<u8>(character, 0x4B5) != 0,
            .listed = std::ranges::contains(listed, character),
            .control = control >= 0x10000 ? ReadValue<u8>(control, 0x20) : u8{0xFF},
            .wake = control >= 0x10000 ? ReadValue<s32>(control, 0x10) : -1,
            .has_info = info >= 0x10000,
            .info_d = info >= 0x10000 ? ReadValue<u8>(info, 0x0D) : u8{0},
            .info_18 = info >= 0x10000 ? ReadValue<u8>(info, 0x18) : u8{0},
        };
        const std::string block = TargetCandidateBlock(candidate);
        text += fmt::format(" [player {:#x} type {} team {} info18 {:#x}: {}]",
                            ReadValue<u32>(character, 0x08), type,
                            ReadValue<s32>(character, CharacterTeamOffset), candidate.info_18,
                            block.empty() ? "a target" : block);
    }
    std::vector<std::pair<float, std::string>> enemies;
    ForEachWorldCharacter(world, [&](u64 character) {
        if (character == GetLocalPlayer() || CharacterHp(character) <= 0) {
            return;
        }
        const u64 modules = ReadValue<u64>(character, 0x3B0);
        const u64 physics = modules >= 0x10000 ? ReadValue<u64>(modules, 0x68) : 0;
        const u64 manager = ReadValue<u64>(character, 0x3B8);
        const u64 ai = manager >= 0x10000 && HasMemoryAccess(manager, 0xB8, MemoryProt::CpuRead)
                           ? ReadValue<u64>(manager, 0xB0)
                           : 0;
        const u64 targeting = ai >= 0x10000 && HasMemoryAccess(ai, 0x568, MemoryProt::CpuRead)
                                  ? ReadValue<u64>(ai, 0x560)
                                  : 0;
        if (physics < 0x10000 || targeting < 0x10000 ||
            !HasMemoryAccess(targeting, 0x20, MemoryProt::CpuRead)) {
            return;
        }
        const u64 vtable = ReadValue<u64>(targeting, 0);
        if (vtable != image_base + EnemyTargetingVtableOffset &&
            vtable != image_base + BaseTargetingVtableOffset) {
            return;
        }
        const float dx = ReadValue<float>(physics, 0x1E0) - (*me)[0];
        const float dy = ReadValue<float>(physics, 0x1E4) - (*me)[1];
        const float dz = ReadValue<float>(physics, 0x1E8) - (*me)[2];
        const float distance = std::sqrt(dx * dx + dy * dy + dz * dz);
        if (distance >= 30.0f) {
            return;
        }
        std::vector<u32> targets;
        u64 node = ReadValue<u64>(targeting, 0x18);
        for (int count = 0;
             count < 64 && node >= 0x10000 && HasMemoryAccess(node, 0x54, MemoryProt::CpuRead);
             ++count, node = ReadValue<u64>(node, 0x08)) {
            if ((ReadValue<u8>(node, 0x02) & 0x7F) == 1 && ReadValue<s32>(node, 0x28) == 1) {
                targets.push_back(ReadValue<u32>(node, 0x50));
            }
        }
        if (targets.empty()) {
            return;
        }
        std::ranges::sort(targets);
        std::string list;
        for (const u32 target : targets) {
            list += fmt::format(" {:#x}", target);
        }
        enemies.emplace_back(
            distance, fmt::format(" [enemy {:#x}:{}]", ReadValue<u32>(character, 0x08), list));
    });
    std::ranges::sort(enemies);
    for (size_t index = 0; index < std::min<size_t>(enemies.size(), 6); ++index) {
        text += enemies[index].second;
    }
    if (text != last) {
        last = text;
        LOG_INFO(Debug, "Bloodborne enemy targets (map {:#x}, {} in the AI list):{}",
                 CurrentPlace().map, listed.size(), text.empty() ? " none" : text);
    }
}

void ClearStaleDeathMarks() {
    static StaleDeathMarks marks;
    if (!InPartyHere()) {
        marks.Tick({}, SteadyMilliseconds());
        return;
    }
    const std::vector<OtherPlayer> players = OtherPlayers();
    const std::vector<u32> invaders = InvaderHandles();
    std::vector<StaleDeathMarks::Body> bodies;
    std::vector<u64> infos;
    for (const OtherPlayer& other : players) {
        const u64 control = ReadValue<u64>(other.character, 0x18);
        const u64 info = control >= 0x10000 ? ReadValue<u64>(control, 0x30) : 0;
        if (info < 0x10000 || !HasMemoryAccess(info + 0x18, 1, MemoryProt::CpuWrite)) {
            continue;
        }
        const u32 handle = ReadValue<u32>(other.character, 0x08);
        bodies.push_back(StaleDeathMarks::Body{
            .handle = handle,
            .marked = (ReadValue<u8>(info, 0x18) & 1) != 0,
            .dead = (other.dead_flags & 0x05) != 0,
            .downed = ReadValue<s32>(other.character, CharacterTeamOffset) == DownedTeams::Hostile,
            .invader = std::ranges::contains(invaders, handle),
        });
        infos.push_back(info);
    }
    for (const u32 handle : marks.Tick(bodies, SteadyMilliseconds())) {
        const auto body = std::ranges::find(bodies, handle, &StaleDeathMarks::Body::handle);
        const u64 info = infos[static_cast<size_t>(body - bodies.begin())];
        WriteValue(info, 0x18, static_cast<u8>(ReadValue<u8>(info, 0x18) & ~u8{1}));
        LOG_INFO(Debug,
                 "Bloodborne party: body {:#x} is up but kept its death mark; cleared, the enemies "
                 "take it as a target again (map {:#x})",
                 handle, CurrentPlace().map);
    }
}

} // namespace Coop

} // namespace Core::Bloodborne
