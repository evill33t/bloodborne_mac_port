// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "core/bloodborne_compass.h"
#include "core/bloodborne_crash_dump.h"
#include "core/bloodborne_party.h"
#include "core/bloodborne_re.h"
#include "core/bloodborne_re_internal.h"
#include "core/bloodborne_write_watch.h"
#include "../../invasion_visual.h"

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
#include "core/libraries/network/server_link.h"
#include "core/libraries/network/sockets.h"
#include "core/libraries/np/np_handler.h"
#include "core/libraries/np/np_matching2/np_matching2_signaling.h"
#include "core/libraries/np/np_trophy.h"
#include "core/libraries/system/userservice.h"
#include "core/memory.h"

#include <httplib.h>

namespace Core::Bloodborne {
namespace {

constexpr u64 CurrentMapListPointerOffset = 0x0553B148;

} // namespace

namespace Coop {

bool HasMemoryAccess(u64 address, size_t size, MemoryProt required) {
    if (size == 0 || address > std::numeric_limits<u64>::max() - size) {
        return false;
    }

    auto* memory = Memory::Instance();
    if (memory == nullptr || !memory->IsValidMapping(address, size)) {
        return false;
    }

    void* mapping_start{};
    void* mapping_end{};
    u32 protection{};
    const u32 required_bits = static_cast<u32>(required);
    return memory->QueryProtection(address, &mapping_start, &mapping_end, &protection) == 0 &&
           (protection & required_bits) == required_bits &&
           address >= reinterpret_cast<u64>(mapping_start) &&
           address + size <= reinterpret_cast<u64>(mapping_end);
}

} // namespace Coop

namespace {

// A guarded call (`site`) refused an object that is there (see RefusedCallLog): logged once per
// site and vtable seen, so a wrong pointer or a vtable list missing a class shows in the log.
void LogRefusedCall(std::string_view site, u64 object) {
    if (object < 0x10000) {
        return; // no object: nothing to say
    }
    const u64 vtable = TryRead<u64>(object, 0).value_or(0);
    static std::mutex mutex;
    static RefusedCallLog logged;
    {
        std::scoped_lock lock{mutex};
        if (!logged.FirstTime(site, vtable)) {
            return;
        }
    }
    LOG_WARNING(Debug, "Bloodborne refused a game call: {} got an object ({:#x}) with vtable {}",
                site, object,
                DescribeVtable(vtable, image_base, MemoryPatcher::g_eboot_image_size));
}

} // namespace

namespace Coop {

// Whether `object` is one of the game objects `vtables` names (see CharacterVtables): checked
// before the game is called with it (`site`, for the log when it is not).
bool HasVtable(u64 object, std::span<const u64> vtables, std::string_view site) {
    const auto vtable = object >= 0x10000 ? TryRead<u64>(object, 0) : std::nullopt;
    if (vtable.has_value() && IsGameVtable(*vtable, image_base, vtables)) {
        return true;
    }
    LogRefusedCall(site, object);
    return false;
}

bool HasVtable(u64 object, u64 vtable, std::string_view site) {
    return HasVtable(object, std::span<const u64>{&vtable, 1}, site);
}

bool IsCharacter(u64 object, std::string_view site) {
    return HasVtable(object, CharacterVtables, site);
}

// Whether the object's virtual at `slot` is the 01.09 function at `function` (e.g. the
// character's AddSpEffect): before a call through it, or a direct call that stands for it.
bool HasVirtual(u64 object, u64 slot, u64 function, std::string_view site) {
    const u64 vtable = object >= 0x10000 ? TryRead<u64>(object, 0).value_or(0) : 0;
    if (image_base != 0 && vtable >= 0x10000 &&
        TryRead<u64>(vtable, slot) == std::optional<u64>{image_base + function}) {
        return true;
    }
    LogRefusedCall(site, object);
    return false;
}

void SetInvasionTint(u64 character,bool on) {
    constexpr s32 red=BbInvasionVisual::Effect,blue=9006;
    if(!on) {
        if(HasPlayerEffect(character,red) && EndPlayerEffect(character,red))
            LOG_INFO(Debug,"Co-op invasion appearance: body {:#x} red effect off",ReadValue<u32>(character,0x08));
        return;
    }
    // Native 1.09: 9026 is normal-invasion guest play, stateInfo 276 -> VFX SFX 212.
    // Paramdex's original row names/definitions and NightwatchCoop's effect/VFX research
    // identify this path. Keep the port's existing full-HP policy when adding its visual.
    const u64 param=FindLoadedParam("SP_EFFECT_PARAM_ST");
    const auto table=param?WritableSpEffects(param):std::nullopt;
    const auto row=table?FindParamRow(*table,red):std::nullopt;
    const auto blank=table?FindParamRow(*table,0):std::nullopt;
    const auto source=table?FindParamRow(*table,BbInvasionVisual::SourceEffect):std::nullopt;
    if(!row || !blank || !source || *row+0x210>table->size() || *blank+0x210>table->size() || *source+0x210>table->size()) return;
    // Refuse a parameter alias: modifying it must never alter another effect's row.
    const u16 count=ReadValue<u16>(param,0xA);
    for(u16 i=0;i<count;++i) {
        const u64 entry=0x40+u64(i)*24;
        if(ReadValue<u64>(param,entry)!=red && ReadValue<u64>(param,entry+8)==*row) return;
    }
    if(!BbInvasionVisual::Prepare(table->subspan(*row,0x210),table->subspan(*blank,0x210),table->subspan(*source,0x210))) return;
    // The copied visual persists independently of the native session-kind controller.
    if(HasPlayerEffect(character,BbInvasionVisual::SourceEffect)) EndPlayerEffect(character,BbInvasionVisual::SourceEffect);
    if(HasPlayerEffect(character,blue)) EndPlayerEffect(character,blue);
    if(!HasPlayerEffect(character,red) && HasVirtual(character,CharacterAddSpEffectSlot,AddSpEffectOffset,"invasion red effect")) {
        reinterpret_cast<AddSpEffectFn>(image_base+AddSpEffectOffset)(character,red,character,0,1,0,1.f,1.f,1.f,1.f,1.f,0);
        LOG_INFO(Debug,"Co-op invasion appearance: body {:#x} native red effect {} on",ReadValue<u32>(character,0x08),red);
    }
}

bool WriteTransportedSummonPlacement(u64 state, const SummonPlacementDescriptor& placement) {
    const std::array<float, 4> position{placement.x, placement.y, placement.z, 1.0F};
    const std::array<float, 4> orientation{0.0F, placement.heading, 0.0F, 0.0F};
    return WriteValue(state, 0x14C4, placement.packed_region) &&
           WriteValue(state, 0x14D0, position) && WriteValue(state, 0x14E0, orientation);
}

bool IsUsablePackedMap(u32 packed_region) {
    return packed_region != 0 && packed_region != std::numeric_limits<u32>::max();
}

bool IsSameSummonPlacement(const SummonPlacementDescriptor& left,
                           const SummonPlacementDescriptor& right) {
    return left.packed_region == right.packed_region &&
           std::bit_cast<u32>(left.x) == std::bit_cast<u32>(right.x) &&
           std::bit_cast<u32>(left.y) == std::bit_cast<u32>(right.y) &&
           std::bit_cast<u32>(left.z) == std::bit_cast<u32>(right.z) &&
           std::bit_cast<u32>(left.heading) == std::bit_cast<u32>(right.heading) &&
           left.area == right.area;
}

u64 GetLocalPlayer() {
    const u64 local_state =
        image_base != 0 ? ReadValue<u64>(image_base + CandidateLocalStatePointerOffset, 0) : 0;
    return local_state >= 0x10000 ? ReadValue<u64>(local_state, 0x60) : 0;
}

} // namespace Coop

namespace {

// The effect manager's node removal (player +0x1C8 manager, node, 1): what the bell's own
// code calls to take off 9003/9004 (0x015069BA). It unlinks the node (+0x58 next, +0x60 prev).
constexpr u64 RemoveEffectNodeOffset = 0x017E1D90;
constexpr std::array<u8, 8> RemoveEffectNodePrologue{0x55, 0x48, 0x89, 0xE5,
                                                     0x41, 0x57, 0x41, 0x56};

} // namespace

namespace Coop {

// Takes `effect_id` off the player (the game's thread). False when it is not on.
bool EndPlayerEffect(u64 player, s32 effect_id) {
    const u64 manager = player >= 0x10000 ? ReadValue<u64>(player, 0x1C8) : 0;
    if (manager < 0x10000 || image_base == 0 ||
        !std::ranges::equal(
            std::span<const u8>{reinterpret_cast<const u8*>(image_base + RemoveEffectNodeOffset),
                                RemoveEffectNodePrologue.size()},
            RemoveEffectNodePrologue)) {
        return false;
    }
    u64 node = ReadValue<u64>(manager, 0x08);
    for (size_t index = 0; node >= 0x10000 && index < 512; ++index) {
        if (ReadValue<s32>(node, 0x40) == effect_id) {
            using Remove = void PS4_SYSV_ABI (*)(u64 manager, u64 node, u32 now);
            reinterpret_cast<Remove>(image_base + RemoveEffectNodeOffset)(manager, node, 1);
            return true;
        }
        const u64 next = ReadValue<u64>(node, 0x58);
        if (next == node) {
            break;
        }
        node = next;
    }
    return false;
}

bool HasPlayerEffect(u64 player, s32 effect_id) {
    const u64 manager = player >= 0x10000 ? ReadValue<u64>(player, 0x1C8) : 0;
    if (manager < 0x10000) {
        return false;
    }

    u64 node = ReadValue<u64>(manager, 0x08);
    for (size_t index = 0; node >= 0x10000 && index < 512; ++index) {
        const u64 next = ReadValue<u64>(node, 0x58);
        const u32 flags = ReadValue<u32>(node, 0x1C);
        const u64 param = ReadValue<u64>(node, 0x48);
        if ((flags & 0x800C0003U) == 0 && param >= 0x10000 &&
            ReadValue<s32>(node, 0x40) == effect_id) {
            return true;
        }
        if (next == node) {
            break;
        }
        node = next;
    }
    return false;
}

u32 GetCurrentPackedMap() {
    const u64 map_list =
        image_base != 0 ? ReadValue<u64>(image_base + CurrentMapListPointerOffset, 0) : 0;
    if (map_list < 0x10000 || !HasMemoryAccess(map_list, 0x24, MemoryProt::CpuRead)) {
        return 0;
    }

    const s32 index = ReadValue<s32>(map_list, 0x20);
    const u64 map_array = ReadValue<u64>(map_list, 0x10);
    if (index < 0 || map_array < 0x10000 ||
        !HasMemoryAccess(map_array, 0x28, MemoryProt::CpuRead)) {
        return 0;
    }

    const s32 count = ReadValue<s32>(map_array, 0x18);
    const u64 entries = ReadValue<u64>(map_array, 0x20);
    if (index >= count || entries < 0x10000) {
        return 0;
    }

    constexpr u64 MapEntryStride = 0xA0;
    const u64 entry_offset = static_cast<u64>(index) * MapEntryStride;
    if (entry_offset > std::numeric_limits<u64>::max() - entries) {
        return 0;
    }
    const u64 entry = entries + entry_offset;
    if (entry > std::numeric_limits<u64>::max() - sizeof(u64)) {
        return 0;
    }
    return ReadValue<u32>(entry, sizeof(u64));
}

} // namespace Coop

namespace {

// Where this player is (see PlaceTracker): fed by TickPlace once per party tick, read by every
// feature instead of GetCurrentPackedMap. Only the load-time hooks, the trip machinery and the
// rig read the game's map themselves.
std::mutex place_mutex;
PlaceTracker place_tracker;
std::vector<std::function<void(const PlaceChange&)>> place_listeners;

} // namespace

namespace Coop {

Place CurrentPlace() {
    std::scoped_lock lock{place_mutex};
    return place_tracker.Current();
}

// The committed map while the player stands in the world, else 0 (in a load, before the first
// map read, in a cutscene): what the features that pause in loads read.
u32 PlayingMap() {
    const Place place = CurrentPlace();
    return place.state == PlaceState::Playing ? place.map : 0;
}

} // namespace Coop

namespace {

// `listener` runs on the game's thread after each change of place, outside the tracker's lock.
bool OnPlaceChange(std::function<void(const PlaceChange&)> listener) {
    std::scoped_lock lock{place_mutex};
    place_listeners.push_back(std::move(listener));
    return true;
}

// One line per change of place.
[[maybe_unused]] const bool place_log_listening = OnPlaceChange(
    [](const PlaceChange& change) { LOG_INFO(Debug, "{}", FormatPlaceChange(change)); });

// A load's end changes the world: its characters settle (deaths now are no kills, the shown board
// waits for its story, see PostsShownState).
[[maybe_unused]] const bool place_world_listening = OnPlaceChange([](const PlaceChange& change) {
    if (StartsNewWorld(change)) {
        world_changed_ms.store(SteadyMilliseconds());
        world_loaded_ms.store(SteadyMilliseconds());
    }
});

// Each change of place into the co-op trace.
[[maybe_unused]] const bool place_trace_listening =
    OnPlaceChange([](const PlaceChange& change) { Trace("place", TracePlaceFields(change)); });

// The resets of what lives for a visit or a session (see LifecycleResets): each container
// registers its own next to it with RegisterLifecycleReset.
std::mutex lifecycle_mutex;
LifecycleResets lifecycle_resets;

} // namespace

namespace Coop {

bool RegisterLifecycleReset(std::string name, std::initializer_list<Lifecycle> events,
                            std::function<void()> reset) {
    std::scoped_lock lock{lifecycle_mutex};
    lifecycle_resets.Register(std::move(name), events, std::move(reset));
    return true;
}

void OnLifecycle(Lifecycle event) {
    std::vector<std::string> ran;
    u64 epoch = 0;
    {
        std::scoped_lock lock{lifecycle_mutex};
        ran = lifecycle_resets.Dispatch(event);
        epoch = lifecycle_resets.Epoch();
    }
    TraceLife(std::string{LifecycleName(event)}.c_str(), {{"epoch", std::to_string(epoch)}});
    LOG_INFO(Debug, "Co-op lifecycle: {}, session epoch {} (reset: {})", LifecycleName(event),
             epoch, ran.empty() ? "nothing" : fmt::format("{}", fmt::join(ran, ", ")));
}

void InitPartyController() {
    if (TeamModeOn()) {
        // Phase 5: the team's Start says the party and the role (HoldTitleLoadForTeam).
        party_controller.emplace(PartyRole::Host, true);
        LOG_INFO(Debug, "Bloodborne party: the team lobby decides the party (team mode)");
        StartTeamLobby();
        return;
    }
    const char* party = std::getenv("SHADPS4_BLOODBORNE_PARTY");
    const char* role = std::getenv("SHADPS4_BLOODBORNE_PARTY_ROLE");
    if (party == nullptr || !ParsePartySetting(party)) {
        return;
    }
    const std::string_view role_name = role != nullptr ? role : "";
    if (role_name != "host" && role_name != "guest") {
        LOG_WARNING(Debug, "Bloodborne party set but SHADPS4_BLOODBORNE_PARTY_ROLE is not "
                           "host or guest; auto-rejoin disabled");
        return;
    }
    party_controller.emplace(role_name == "host" ? PartyRole::Host : PartyRole::Guest);
    LOG_INFO(Debug, "Bloodborne party auto-rejoin enabled as {}", role_name);
}

} // namespace Coop

namespace {

PartyObservation ObserveParty(u64 player, s32 search_effect) {
    PartyObservation observation{};
    observation.player_valid =
        player >= 0x10000 && HasMemoryAccess(player, sizeof(u64), MemoryProt::CpuRead);
    observation.map_usable = PlayingMap() != 0;

    const u64 state = GetGlobalState();
    observation.stage_transition =
        state < 0x10000 || !HasMemoryAccess(state, 0x1521, MemoryProt::CpuRead) ||
        ReadValue<u8>(state, 0x08) != 0 || ReadValue<u8>(state, 0x1520) != 0 ||
        pending_summon_reload_map.load(std::memory_order_acquire) != 0 ||
        pending_summon_reload_state_map.load(std::memory_order_acquire) != 0;

    // The cross-map responder resume owns the bell while it is armed; stay out of its way.
    const u64 multi_play = GetMatchingState();
    observation.matching_idle =
        !IsUsablePackedMap(pending_responder_resume.target_map) && multi_play >= 0x10000 &&
        HasMemoryAccess(multi_play, 0x128, MemoryProt::CpuRead) &&
        ReadValue<s32>(multi_play, 0x124) == 0 && ReadValue<u64>(multi_play, 0x18) == 0;

    observation.search_effect_active =
        observation.player_valid && HasPlayerEffect(player, search_effect);
    observation.boss_sequence = EventFlagOn(4000) || EventFlagOn(4047);
    observation.room_for_more = party_controller &&
                                party_controller->SearchEffectId() == BeckoningBellEffect &&
                                HostHasRoomForMore();
    observation.white_phantom = observation.player_valid &&
                                HasMemoryAccess(player, 0x7C, MemoryProt::CpuRead) &&
                                ReadValue<s32>(player, 0x78) == WhiteGhostCharacterType;
    observation.last_search_post = LastSummonSearchPost();
    return observation;
}

} // namespace

namespace Coop {

std::vector<s32> LocalEffects() {
    return EffectsOf(GetLocalPlayer());
}

std::vector<s32> EffectsOf(u64 character) {
    std::vector<s32> effects;
    const u64 manager = character >= 0x10000 ? ReadValue<u64>(character, 0x1C8) : 0;
    u64 node = manager >= 0x10000 ? ReadValue<u64>(manager, 0x08) : 0;
    for (size_t index = 0; node >= 0x10000 && index < 128; ++index) {
        const u64 next = ReadValue<u64>(node, 0x58);
        if ((ReadValue<u32>(node, 0x1C) & 0x800C0003U) == 0) {
            effects.push_back(ReadValue<s32>(node, 0x40));
        }
        if (next == node) {
            break;
        }
        node = next;
    }
    return effects;
}

} // namespace Coop

namespace {

// GlobalState +0x16F8 -> +0x14: players in the session (the count ESD function 52 checks).
constexpr u64 SessionPlayersOffset = 0x16F8;

} // namespace

namespace Coop {

std::mutex flag_board_mutex;
std::vector<std::pair<s32, bool>> flags_to_post;

void PostWorldChanges(const FlagChanges& changes) {
    // A ritual altar's record goes whole on its own (ShareRitualAltarRecord), never here.
    auto shared = FlagsToShare(changes);
    // A guest running its map without the host (two guests there both do, each in its own copy):
    // its game ran the map's story, so the team takes the map's steps it made, all but its loads'
    // own (LoneGuestShares).
    // Every guest's loads teach the map's load flags, alone or not yet known to be.
    static LoneGuestShares lone_shares;
    const auto map_steps =
        IsPartyGuestRole()
            ? lone_shares.Take(changes, PlayingMap(), SteadyMilliseconds() - world_loaded_ms.load())
            : std::vector<std::pair<s32, bool>>{};
    if (CurrentEnemyRole() == EnemyRole::Owner || HostAwayFromThisMap()) {
        if (!map_steps.empty()) {
            std::string which;
            for (const auto& [flag, on] : map_steps) {
                which += fmt::format(" {}{}", on ? "" : "-", flag);
            }
            LOG_INFO(Debug,
                     "Bloodborne party: this lone guest's map steps go to the team (-off):{}",
                     which);
            shared.insert(shared.end(), map_steps.begin(), map_steps.end());
        }
    }
    if (shared.empty() || TakeFlagsForTheLog(shared)) {
        return;
    }
    std::scoped_lock lock{flag_board_mutex};
    flags_to_post.insert(flags_to_post.end(), shared.begin(), shared.end());
}

bool InPartySession() {
    const u64 multi_play = GetMatchingState();
    return party_controller.has_value() && multi_play >= 0x10000 &&
           HasMemoryAccess(multi_play, 0x128, MemoryProt::CpuRead) &&
           IsPartySession(true, ReadValue<s32>(multi_play, 0x124));
}

size_t SessionMembers() {
    const u64 session =
        image_base != 0 ? ReadValue<u64>(image_base + SessionManagerPointerOffset, 0) : 0;
    if (session < 0x10000 || !HasMemoryAccess(session, 0x108, MemoryProt::CpuRead)) {
        return 1;
    }
    const u64 begin = ReadValue<u64>(session, 0xF8);
    const u64 end = ReadValue<u64>(session, 0x100);
    return begin >= 0x10000 && end > begin && end - begin <= 0x20 * 8 ? (end - begin) / 0x20 : 1;
}

s32 SessionPlayers() {
    const u64 global = GetGlobalState();
    const u64 session = global >= 0x10000 ? ReadValue<u64>(global, SessionPlayersOffset) : 0;
    return session >= 0x10000 ? ReadValue<s32>(session, 0x14) : 0;
}

// A character's position, where the healing fountain update reads the player's ([+0x3B0]+0x68,
// then +0x1E0).
std::optional<LampPosition> CharacterPosition(u64 character) {
    const u64 modules = character >= 0x10000 ? ReadValue<u64>(character, 0x3B0) : 0;
    const u64 physics = modules >= 0x10000 ? ReadValue<u64>(modules, 0x68) : 0;
    if (physics < 0x10000 || !HasMemoryAccess(physics, 0x1EC, MemoryProt::CpuRead)) {
        return std::nullopt;
    }
    return LampPosition{ReadValue<float>(physics, 0x1E0), ReadValue<float>(physics, 0x1E4),
                        ReadValue<float>(physics, 0x1E8)};
}

// The local player's position.
std::optional<LampPosition> LocalPlayerPosition() {
    return CharacterPosition(GetLocalPlayer());
}

// Kaniel 2026-10-17 (the compass): the game camera's heading. The camera object (the one bbport's
// runtime_camhook.c turns: pivot +0xD0, drawn position +0x100, floats) hangs off the camera
// manager [[CameraManagerPointerOffset] +CameraManagerField], at a path that is found once: a
// pointer in the manager (CameraSearchSize bytes), or one in the object it points at (the first
// CameraSearchInner bytes), to an object whose pivot is by the player and whose drawn position
// is a camera's distance from it. The path found is logged; it is checked every read and looked
// for again (at most once a second) when it stops looking like a camera.
namespace {

constexpr u64 CameraSearchSize = 0x1150;
constexpr u64 CameraSearchInner = 0x400;
constexpr u64 CameraPivotAt = 0xD0;
constexpr u64 CameraDrawnAt = 0x100;

struct CameraPath {
    u64 outer = 0;            // the pointer's place in the manager
    std::optional<u64> inner; // and in the object it points at, for a second step
    bool operator==(const CameraPath&) const = default;
};

bool ReadCameraPoints(u64 camera, LampPosition& pivot, LampPosition& drawn) {
    if (camera < 0x10000 || (camera & 7) != 0 ||
        !HasMemoryAccess(camera + CameraPivotAt, CameraDrawnAt - CameraPivotAt + 12,
                         MemoryProt::CpuRead)) {
        return false;
    }
    std::memcpy(pivot.data(), reinterpret_cast<const void*>(camera + CameraPivotAt), 12);
    std::memcpy(drawn.data(), reinterpret_cast<const void*>(camera + CameraDrawnAt), 12);
    for (const float value : {pivot[0], pivot[1], pivot[2], drawn[0], drawn[1], drawn[2]}) {
        if (!std::isfinite(value) || std::abs(value) > 1.0e5f) {
            return false;
        }
    }
    return true;
}

float Distance(const LampPosition& a, const LampPosition& b) {
    return std::hypot(a[0] - b[0], a[1] - b[1], a[2] - b[2]);
}

// A camera: its drawn position 0.3 to 30 m from its pivot (the follow camera; a cutscene's may be
// farther, and is not the compass's).
bool LooksLikeCamera(const LampPosition& pivot, const LampPosition& drawn) {
    const float reach = Distance(pivot, drawn);
    return reach >= 0.3f && reach <= 30.0f;
}

u64 CameraAt(u64 manager, const CameraPath& path) {
    const u64 outer = ReadValue<u64>(manager, path.outer);
    return path.inner ? ReadValue<u64>(outer, *path.inner) : outer;
}

std::optional<CameraPath> FindCameraPath(u64 manager, const LampPosition& player) {
    if (!HasMemoryAccess(manager, CameraSearchSize, MemoryProt::CpuRead)) {
        return std::nullopt;
    }
    const auto is_camera = [&](u64 camera) {
        LampPosition pivot{}, drawn{};
        return ReadCameraPoints(camera, pivot, drawn) && LooksLikeCamera(pivot, drawn) &&
               Distance(pivot, player) <= 3.0f;
    };
    for (u64 outer = 0; outer < CameraSearchSize; outer += 8) {
        if (is_camera(ReadValue<u64>(manager, outer))) {
            return CameraPath{outer};
        }
    }
    for (u64 outer = 0; outer < CameraSearchSize; outer += 8) {
        const u64 object = ReadValue<u64>(manager, outer);
        if (object < 0x10000 || (object & 7) != 0 ||
            !HasMemoryAccess(object, CameraSearchInner, MemoryProt::CpuRead)) {
            continue;
        }
        for (u64 inner = 0; inner < CameraSearchInner; inner += 8) {
            if (is_camera(ReadValue<u64>(object, inner))) {
                return CameraPath{outer, inner};
            }
        }
    }
    return std::nullopt;
}

} // namespace

std::optional<float> CameraHeadingNow() {
    static std::optional<CameraPath> path;
    static s64 searched_at = std::numeric_limits<s64>::min();
    static bool told_missing = false;
    const u64 holder = ReadValue<u64>(image_base + CameraManagerPointerOffset, 0);
    const u64 manager = holder >= 0x10000 ? ReadValue<u64>(holder, CameraManagerField) : 0;
    if (manager < 0x10000) {
        return std::nullopt;
    }
    LampPosition pivot{}, drawn{};
    if (!path || !ReadCameraPoints(CameraAt(manager, *path), pivot, drawn) ||
        !LooksLikeCamera(pivot, drawn)) {
        const s64 now = SteadyMilliseconds();
        const auto player = LocalPlayerPosition();
        if (!player ||
            (searched_at != std::numeric_limits<s64>::min() && now - searched_at < 1'000)) {
            return std::nullopt;
        }
        searched_at = now;
        const auto found = FindCameraPath(manager, *player);
        if (!found) {
            if (!told_missing) {
                told_missing = true;
                LOG_INFO(Debug,
                         "Co-op compass: no camera found by the player in the camera "
                         "manager {:#x} yet",
                         manager);
            }
            return std::nullopt;
        }
        if (found != path) {
            LOG_INFO(Debug, "Co-op compass: the camera is [[manager {:#x}]+{:#x}]{} = {:#x}",
                     manager, found->outer,
                     found->inner ? fmt::format("+{:#x}", *found->inner) : std::string{},
                     CameraAt(manager, *found));
        }
        path = found;
        if (!ReadCameraPoints(CameraAt(manager, *path), pivot, drawn)) {
            return std::nullopt;
        }
    }
    return CameraHeading(pivot, drawn);
}

// Kaniel 2026-10-12: the hardest bugs come where two maps meet (a fog border crossed back and
// forth, teleports, cutscenes). Each map change is logged with where the player was and the
// path it came by (PositionTrail): the rig replays such a path with glide to cross it itself.
void LogMapChanges() {
    static u32 last_map = 0;
    static PositionTrail trail;
    const u32 map = GetCurrentPackedMap();
    const auto at = LocalPlayerPosition();
    if (at) {
        trail.Note(static_cast<u64>(SteadyMilliseconds()), *at);
    }
    if (map == last_map) {
        return;
    }
    if (last_map != 0 && map != 0 && at) {
        LOG_INFO(Debug,
                 "Bloodborne map change {:#x} -> {:#x} at ({:.1f}, {:.1f}, {:.1f}) trip={}; "
                 "trail: {}",
                 last_map, map, (*at)[0], (*at)[1], (*at)[2], PartyTripActive(),
                 FormatTrail(trail.Points()));
    }
    last_map = map;
    trail.Clear();
}

std::vector<OtherPlayer> OtherPlayers() {
    std::vector<OtherPlayer> players;
    const u64 world = ReadValue<u64>(image_base + CandidateLocalStatePointerOffset, 0);
    const s32 count = world >= 0x10000 ? ReadValue<s32>(world, 0x30) : 0;
    const u64 entries = world >= 0x10000 ? ReadValue<u64>(world, 0x38) : 0;
    const u64 local = GetLocalPlayer();
    for (s32 index = 0; entries >= 0x10000 && index < std::min(count, 8); ++index) {
        const u64 player = ReadValue<u64>(entries + static_cast<u64>(index) * 0x38, 0);
        if (player < 0x10000 || player == local ||
            !HasMemoryAccess(player, 0x3B8, MemoryProt::CpuRead)) {
            continue;
        }
        OtherPlayer other{player, ReadValue<s32>(player, 0x78)};
        const u64 modules = ReadValue<u64>(player, 0x3B0);
        const u64 data = modules >= 0x10000 ? ReadValue<u64>(modules, 0x20) : 0;
        other.hp = data >= 0x10000 ? ReadValue<s32>(data, 0x128) : 0;
        other.dead_flags = ReadValue<u8>(player, 0x1E3);
        const u64 physics = modules >= 0x10000 ? ReadValue<u64>(modules, 0x68) : 0;
        if (physics >= 0x10000 && HasMemoryAccess(physics, 0x1EC, MemoryProt::CpuRead)) {
            other.at =
                LampPosition{ReadValue<float>(physics, 0x1E0), ReadValue<float>(physics, 0x1E4),
                             ReadValue<float>(physics, 0x1E8)};
        }
        players.push_back(other);
    }
    return players;
}

// Kaniel 2026-10-15: the other players' characters show by the server's map board alone
// (ShownOtherPlayers): one whose member is not on this map is hidden (the game's disable: no
// model, no collision) and shown again once a member stands there.
void ReconcileMemberPresence() {
    static std::unordered_map<u64, s64> unmatched_since;
    static std::unordered_set<u64> hidden;
    static std::unordered_map<u64, s64> first_seen;
    static std::unordered_set<u64> stale_hidden;
    static std::unordered_map<u64, FadeBack> fade_back;
    using SetDisabled = void PS4_SYSV_ABI (*)(u64, bool);
    const auto set_disabled =
        reinterpret_cast<SetDisabled>(image_base + SetCharacterDisabledOffset);
    const std::vector<OtherPlayer> others = OtherPlayers();
    std::unordered_set<u64> present;
    for (const OtherPlayer& other : others) {
        present.insert(other.character);
    }
    // Characters gone from the world are forgotten (never touched again).
    std::erase_if(hidden, [&](u64 character) { return !present.contains(character); });
    std::erase_if(unmatched_since,
                  [&](const auto& entry) { return !present.contains(entry.first); });
    std::erase_if(first_seen, [&](const auto& entry) { return !present.contains(entry.first); });
    std::erase_if(stale_hidden, [&](u64 character) { return !present.contains(character); });
    std::erase_if(fade_back, [&](const auto& entry) { return !present.contains(entry.first); });
    // Kaniel's playtest 2026-10-19: two copies of one member take his packets off one queue on two
    // threads (a double free in everyone's game when he joined back). All but his newest copy
    // stay hidden (a hidden copy's update, the one that takes them, does not run).
    const s64 now_ms = SteadyMilliseconds();
    std::vector<u64> identities;
    std::vector<s64> seen;
    for (const OtherPlayer& other : others) {
        identities.push_back(MemberIdentityOf(other.character));
        seen.push_back(first_seen.try_emplace(other.character, now_ms).first->second);
    }
    const std::vector<bool> stale = StaleDuplicatePlayers(identities, seen);
    for (size_t i = 0; i < others.size(); ++i) {
        const u64 character = others[i].character;
        if (!stale[i] && stale_hidden.erase(character) > 0) {
            // The other copy is gone: this one is the member's again, shown by the board as any.
            hidden.insert(character);
            LOG_INFO(Debug, "Bloodborne presence: player {:#x} is member {:#x}'s only copy again",
                     ReadValue<u32>(character, 0x08), identities[i]);
        }
        if (!stale[i] || stale_hidden.contains(character)) {
            continue;
        }
        if (!IsCharacterDisabled(character)) {
            set_disabled(character, true);
            ForgetGroundOfHidden(character);
        }
        stale_hidden.insert(character);
        hidden.erase(character);
        unmatched_since.erase(character);
        LOG_WARNING(Debug,
                    "Bloodborne presence: player {:#x} is a second copy of member {:#x}: hidden "
                    "(the newest copy stays)",
                    ReadValue<u32>(character, 0x08), identities[i]);
    }
    std::vector<MemberVitals> board;
    std::vector<MemberVitals> hostile;
    std::string self;
    bool fresh = false;
    {
        std::scoped_lock lock{vitals_mutex};
        board = vitals_board.members;
        hostile = hostile_vitals;
        self = vitals_member;
        fresh = vitals_heard_at &&
                std::chrono::steady_clock::now() - *vitals_heard_at < std::chrono::seconds{5};
    }
    const u32 map = CurrentPlace().map;
    if (!InPartySession() || !fresh || map == 0) {
        SetInvaderHandles({});
        // Out of a session (or nothing known): everyone as the game has them (a stale second copy
        // of a member stays hidden).
        for (const u64 character : hidden) {
            if (IsCharacterDisabled(character)) {
                set_disabled(character, false);
            }
        }
        hidden.clear();
        unmatched_since.clear();
        return;
    }
    std::vector<std::optional<LampPosition>> positions;
    for (const OtherPlayer& other : others) {
        positions.push_back(other.at);
    }
    // A party member's death (not an invader's: its line joins the board below).
    const bool someone_died = std::ranges::any_of(board, [&](const MemberVitals& member) {
        return member.member != self && member.map == map && member.vital == Vital::Dead;
    });
    // Invasions: the invader's line keeps its body shown too; which body is its (the hit hook and
    // party death read them).
    std::vector<u32> invaders;
    if (!hostile.empty()) {
        const std::vector<bool> invader = InvaderBodies(positions, board, hostile, self, map);
        for (size_t i = 0; i < others.size(); ++i) {
            if (invader[i]) {
                invaders.push_back(ReadValue<u32>(others[i].character, 0x08));
            }
        }
        board.insert(board.end(), hostile.begin(), hostile.end());
    }
    SetInvaderHandles(std::move(invaders));
    const std::vector<std::optional<size_t>> vouching =
        VouchingMembers(positions, board, self, map);
    for (size_t i = 0; i < others.size(); ++i) {
        const u64 character = others[i].character;
        if (stale_hidden.contains(character)) {
            continue;
        }
        const std::optional<size_t> member = vouching[i];
        if (fade_back[character].Tick(CharacterFade(character), someone_died,
                                      member ? std::optional{board[*member].vital}
                                             : std::nullopt) &&
            HasMemoryAccess(character + CharacterFadeOffset, sizeof(float), MemoryProt::CpuWrite)) {
            WriteValue(character, CharacterFadeOffset, 1.0f);
            LOG_INFO(Debug,
                     "Bloodborne presence: player {:#x} faded out (a fall into the void) and its "
                     "member {} is up again: faded back in",
                     ReadValue<u32>(character, 0x08), board[*member].member);
        }
        const bool shown = !others[i].at || member.has_value();
        if (shown) {
            unmatched_since.erase(character);
            if (hidden.erase(character) > 0 && IsCharacterDisabled(character)) {
                set_disabled(character, false);
                std::string board_text;
                for (const MemberVitals& member : board) {
                    board_text +=
                        fmt::format(" [{} map {:#x} at ({:.0f}, {:.0f}, {:.0f})]", member.member,
                                    member.map, member.at[0], member.at[1], member.at[2]);
                }
                const auto& at = others[i].at;
                LOG_INFO(Debug,
                         "Bloodborne presence: a member stands by player {:#x} at ({:.0f}, {:.0f}, "
                         "{:.0f}) again: shown; board (self {}):{}",
                         ReadValue<u32>(character, 0x08), at ? (*at)[0] : 0.0f,
                         at ? (*at)[1] : 0.0f, at ? (*at)[2] : 0.0f, self, board_text);
            }
            continue;
        }
        const s64 since = unmatched_since.try_emplace(character, now_ms).first->second;
        if (PresenceHides(now_ms, first_seen.at(character), since) && !hidden.contains(character) &&
            !IsCharacterDisabled(character)) {
            set_disabled(character, true);
            ForgetGroundOfHidden(character);
            hidden.insert(character);
            const auto& at = others[i].at;
            LOG_INFO(Debug,
                     "Bloodborne presence: no member on map {:#x} by player {:#x} at ({:.0f}, "
                     "{:.0f}, {:.0f}): hidden",
                     map, ReadValue<u32>(character, 0x08), at ? (*at)[0] : 0.0f,
                     at ? (*at)[1] : 0.0f, at ? (*at)[2] : 0.0f);
        }
    }
}

// Kaniel's crash 2026-10-17: hidden, its floor update no longer runs, and the floor of a map block
// this game then drops stayed (0x0191B7C0). No floor (as in mid-air) until it is shown again and
// finds one. Kaniel's crash 2026-10-19 (0x01524B24): the same for the object its physics holds as
// the one it stands on: Lisara's hidden copy kept one of the map block his game had just dropped.
void ForgetGroundOfHidden(u64 character) {
    if (HasMemoryAccess(character + 0x288, sizeof(u64), MemoryProt::CpuWrite)) {
        WriteValue<u64>(character, 0x288, 0);
    }
    const u64 modules = ReadValue<u64>(character, 0x3B0);
    const u64 physics = modules >= 0x10000 ? ReadValue<u64>(modules, 0x68) : 0;
    if (physics >= 0x10000 &&
        HasMemoryAccess(physics + PhysicsContactObjectOffset, sizeof(u64), MemoryProt::CpuWrite)) {
        WriteValue<u64>(physics, PhysicsContactObjectOffset, 0);
    }
}

// The member a remote player's character stands for: its network state ([[+0x60]+0xD0], what the
// game's per-character receive 0x01A6D390 looks the member's connection up by) starts with the
// member's identity. 0 when unknown.
u64 MemberIdentityOf(u64 character) {
    const u64 control = ReadValue<u64>(character, 0x60);
    if (control < 0x10000 || !HasMemoryAccess(control + 0xD0, sizeof(u64), MemoryProt::CpuRead)) {
        return 0;
    }
    const u64 network = ReadValue<u64>(control, 0xD0);
    if (network < 0x10000 || !HasMemoryAccess(network, sizeof(u64), MemoryProt::CpuRead)) {
        return 0;
    }
    return ReadValue<u64>(network, 0);
}

bool IsCharacterDisabled(u64 character) {
    const u64 control = ReadValue<u64>(character, 0x18);
    return control >= 0x10000 && (ReadValue<u8>(control, 0x20) & 1) != 0;
}

float CharacterFade(u64 character) {
    return character >= 0x10000 && HasMemoryAccess(character + CharacterFadeOffset, sizeof(float),
                                                   MemoryProt::CpuRead)
               ? ReadValue<float>(character, CharacterFadeOffset)
               : 1.0f;
}

bool CharacterHitsOff(u64 character) {
    const u64 model = ReadValue<u64>(character, 0x58);
    return model >= 0x10000 && (ReadValue<u16>(model, 0x114) & 2) != 0;
}

bool SetCharacterHitsOff(u64 character, bool off) {
    const u64 model = ReadValue<u64>(character, 0x58);
    if (model < 0x10000 || !HasMemoryAccess(model + 0x114, sizeof(u16), MemoryProt::CpuWrite)) {
        return false;
    }
    const u16 flags = ReadValue<u16>(model, 0x114);
    return WriteValue(model, 0x114, static_cast<u16>(off ? flags | 2 : flags & ~2));
}

s32 CharacterHp(u64 character) {
    const u64 modules = ReadValue<u64>(character, 0x3B0);
    const u64 data = modules >= 0x10000 ? ReadValue<u64>(modules, 0x20) : 0;
    return data >= 0x10000 ? ReadValue<s32>(data, 0xF8) : 0;
}

} // namespace Coop

namespace {

// See InSessionWorld.
std::atomic<bool> in_session_world{false};

// After the session's Join and Leave (this member's own), from the same tick: whether this member
// is in the session's world yet (SessionArrival, kept through trips), and the other members
// leaving the session while it stays (MemberDepartures), a MemberLeft each.
void TickSessionMembers(bool in_session, bool trip, bool loading, s64 now_ms) {
    if (!trip) {
        static SessionArrival arrival;
        const Place place = CurrentPlace();
        const u64 multi_play = GetMatchingState();
        const bool summoned = multi_play >= 0x10000 &&
                              HasMemoryAccess(multi_play, 0x128, MemoryProt::CpuRead) &&
                              ReadValue<s32>(multi_play, 0x124) == MatchingStateSummonedGuest;
        const bool arrived = arrival.Tick(in_session, summoned, place.visit,
                                          place.state == PlaceState::Playing, now_ms);
        if (arrived != in_session_world.exchange(arrived, std::memory_order_relaxed)) {
            LOG_INFO(Debug, "Bloodborne party: {} the session's world (map {:#x} visit {})",
                     arrived ? "in" : "out of", place.map, place.visit);
        }
    }
    static MemberDepartures departures;
    const size_t left = departures.Tick(in_session, !trip && !loading,
                                        in_session ? OtherSessionMembers().size() : 0, now_ms);
    for (size_t index = 0; index < left; ++index) {
        OnLifecycle(Lifecycle::MemberLeft);
    }
}

// The tracker's reading for this tick (the only raw map read besides the load-time hooks, the
// trip machinery and the rig), first in the tick so every feature after it sees this tick's
// place. Before TickPartyTrip: the tick that ends a trip still reads it as one (a lamp trip).
void TickPlace() {
    const u64 global = GetGlobalState();
    const u64 player = GetLocalPlayer();
    const u64 modules = player >= 0x10000 ? ReadValue<u64>(player, 0x3B0) : 0;
    const u64 data = modules >= 0x10000 ? ReadValue<u64>(modules, 0x20) : 0;
    const bool trip = PartyTripActive();
    const PlaceReading reading{
        .now_ms = SteadyMilliseconds(),
        .map = GetCurrentPackedMap(),
        .player = player >= 0x10000 ? player : 0,
        .at = LocalPlayerPosition(),
        .loading = global < 0x10000 || TryRead<u8>(global, 0x08).value_or(1) != 0,
        .trip = trip,
        .dead = data >= 0x10000 && ReadValue<s32>(data, 0xF8) <= 0,
    };
    std::optional<PlaceChange> change;
    std::vector<std::function<void(const PlaceChange&)>> listeners;
    {
        std::scoped_lock lock{place_mutex};
        change = place_tracker.Update(reading);
        if (change) {
            listeners = place_listeners;
        }
    }
    if (change) {
        for (const auto& listener : listeners) {
            listener(*change);
        }
    }
    // Into and out of a party session (not during a trip: seamless travel keeps the session
    // through the world's teardown).
    static bool was_in_session = false;
    if (!trip) {
        if (const bool in_session = InPartySession(); in_session != was_in_session) {
            was_in_session = in_session;
            OnLifecycle(in_session ? Lifecycle::Join : Lifecycle::Leave);
        }
    }
    TickSessionMembers(was_in_session, trip, reading.loading, reading.now_ms);
}

} // namespace

namespace Coop {

bool InSessionWorld() {
    return in_session_world.load(std::memory_order_relaxed);
}

} // namespace Coop

namespace Coop {

CoopTimeMeter coop_time{30'000};
CoopTimeMeter coop_tick_parts{30'000};

TickPartTimer::~TickPartTimer() {
    coop_tick_parts.Add(name,
                        std::chrono::duration_cast<std::chrono::nanoseconds>(
                            std::chrono::steady_clock::now() - started)
                            .count(),
                        SteadyMilliseconds());
}

void ReportCoopTime(s64 now_ms) {
    // A warning, so the port's console shows it beside its Perf lines.
    if (const auto line = coop_time.Report(now_ms)) {
        LOG_WARNING(Debug, "Co-op time: {}", *line);
    }
    if (const auto line = coop_tick_parts.Report(now_ms, 4)) {
        LOG_WARNING(Debug, "Co-op tick slowest parts: {}", *line);
    }
}

namespace {

template <typename Part>
void TimeTickPart(std::string_view name, Part&& part) {
    const TickPartTimer timer{name};
    part();
}

} // namespace

void TickPartyController() {
    TimeTickPart("NoteGameThreadTick", NoteGameThreadTick);
    TimeTickPart("TickPlace", TickPlace);
    TimeTickPart("TickAutoplay", TickAutoplay);
    TimeTickPart("TickPartyTrip", TickPartyTrip);
    TimeTickPart("TickPlayersOnThisMap", TickPlayersOnThisMap);
    TimeTickPart("TickHostRules", TickHostRules);
    TimeTickPart("TickMapScreen", TickMapScreen);
    TimeTickPart("CheckAutoplayWatch", CheckAutoplayWatch);
    TimeTickPart("LogBossFightBullets", LogBossFightBullets);
    TimeTickPart("LogJoinTaskChanges", LogJoinTaskChanges);
    TimeTickPart("WatchSummonJoin", WatchSummonJoin);
    TimeTickPart("KeepGuestBloodstainMark", KeepGuestBloodstainMark);
    TimeTickPart("ShowGuestBloodstain", ShowGuestBloodstain);
    TimeTickPart("LogBloodstainStateChanges", LogBloodstainStateChanges);
    TimeTickPart("LogMapChanges", LogMapChanges);
    TimeTickPart("LogLocalEffectChanges", LogLocalEffectChanges);
    if (party_controller) {
        TimeTickPart("LogActionBlockingState", LogActionBlockingState);
        TimeTickPart("KeepBossHpInStep", KeepBossHpInStep);
        TimeTickPart("LogNearbyEnemyControl", LogNearbyEnemyControl);
        TimeTickPart("LogEnemyTargets", LogEnemyTargets);
        TimeTickPart("ClearStaleDeathMarks", ClearStaleDeathMarks);
        TimeTickPart("LogJoinTasksOnChange", LogJoinTasksOnChange);
        TimeTickPart("ReconcileMemberPresence", ReconcileMemberPresence);
        TimeTickPart("LogUnsharedNearbyCharacters", LogUnsharedNearbyCharacters);
    }
    TimeTickPart("LogOtherPlayersChanges", LogOtherPlayersChanges);
    TimeTickPart("ObserveLocalAliveState", ObserveLocalAliveState);
    {
        const auto started = std::chrono::steady_clock::now();
        TickPartyDeath();
        coop_time.Add("party death",
                      std::chrono::duration_cast<std::chrono::nanoseconds>(
                          std::chrono::steady_clock::now() - started)
                          .count(),
                      SteadyMilliseconds());
    }
    TimeTickPart("TickDuel", TickDuel);
    {
        const auto started = std::chrono::steady_clock::now();
        TimeTickPart("TickPings", TickPings);
        coop_time.Add("pings",
                      std::chrono::duration_cast<std::chrono::nanoseconds>(
                          std::chrono::steady_clock::now() - started)
                          .count(),
                      SteadyMilliseconds());
    }
    TimeTickPart("TickLampRest", TickLampRest);
    if (!party_controller) {
        return;
    }
    const TickPartTimer party_controller_part{"party controller"};
    ReopenClosedPartySession();
    const u64 player = GetLocalPlayer();
    const PartyObservation observation = ObserveParty(player, party_controller->SearchEffectId());
    const PartyAction action = party_controller->Tick(observation, PartyController::Clock::now());
    // Phase 6: a team opens and joins its host's session itself, no bell and no signs.
    if (TeamModeOn() && !InPartySession()) {
        // Why a team's game out of its session does not open or join one (every 10 s).
        static s64 said_ms = 0;
        if (const s64 now_ms = SteadyMilliseconds(); now_ms - said_ms >= 10'000) {
            said_ms = now_ms;
            LOG_INFO(Debug,
                     "Co-op team join: out of a session: action {} player {} map {} transition {} "
                     "boss {} idle {} effect {} phantom {} waiting {}",
                     static_cast<int>(action), observation.player_valid, observation.map_usable,
                     observation.stage_transition, observation.boss_sequence,
                     observation.matching_idle, observation.search_effect_active,
                     observation.white_phantom, party_controller->Waiting());
        }
    }
    // Invasions: the invader's trip (a solo player's: TickTeamJoin returns before it).
    TickInvasion();
    if (TickTeamJoin(action == PartyAction::StartSearch)) {
        return;
    }
    if (action == PartyAction::EndSearch) {
        // A bell that outlasted its search (see PartyController::RingAgain): the game starts a
        // search only when the effect goes on, so the old one comes off first; the controller
        // rings again on a later tick, once it is off.
        const bool ended = EndPlayerEffect(player, party_controller->SearchEffectId());
        LOG_INFO(Debug,
                 "Bloodborne party: ended the host's stale bell effect {} to ring again (ended={}, "
                 "last search posted {} ms ago)",
                 party_controller->SearchEffectId(), ended,
                 observation.last_search_post
                     ? std::chrono::duration_cast<std::chrono::milliseconds>(
                           PartyController::Clock::now() - *observation.last_search_post)
                           .count()
                     : -1);
        return;
    }
    if (action != PartyAction::StartSearch) {
        return;
    }
    if (EndsStaleSessionEffect(IsPartyGuestRole() ? PartyRole::Guest : PartyRole::Host,
                               InPartySession(), LocalEffects())) {
        LOG_INFO(Debug,
                 "Bloodborne party: a guest out of its session ended the session effect {} "
                 "to ring (ended={})",
                 SessionEffect, EndPlayerEffect(player, SessionEffect));
        return; // the bell on a later tick, as RingAgain does
    }
    {
        // Rig 2026-10-13 (crashes, host-crash): a guest whose host's game died rang with no
        // sign ever posted, while every fresh player's ring posts one. The state the sign may
        // wait on, per ring, to tell the two apart.
        const u64 global = GetGlobalState();
        const u64 multi_play = GetMatchingState();
        const u64 session = ReadValue<u64>(image_base + SessionManagerPointerOffset, 0);
        const bool global_ok =
            global >= 0x10000 && HasMemoryAccess(global, 0x1580, MemoryProt::CpuRead);
        const bool multi_ok =
            multi_play >= 0x10000 && HasMemoryAccess(multi_play, 0x128, MemoryProt::CpuRead);
        const bool session_ok =
            session >= 0x10000 && HasMemoryAccess(session, 0x128, MemoryProt::CpuRead);
        LOG_INFO(Debug,
                 "Bloodborne party ring state: type {} player +0x52C {:#x} global +0x08 {:#x} "
                 "+0x1520 {:#x} +0x1547 {:#x} +0x1578 {:#x} matching +0x18 {:#x} +0x124 {} "
                 "session +0x124 {} closing {}",
                 player >= 0x10000 ? ReadValue<s32>(player, 0x78) : -1,
                 player >= 0x10000 ? ReadValue<u8>(player, 0x52C) : 0,
                 global_ok ? ReadValue<u8>(global, 0x08) : 0xFF,
                 global_ok ? ReadValue<u32>(global, 0x1520) : 0,
                 global_ok ? ReadValue<u8>(global, 0x1547) : 0xFF,
                 global_ok ? ReadValue<u32>(global, 0x1578) : 0,
                 multi_ok ? ReadValue<u64>(multi_play, 0x18) : 0,
                 multi_ok ? ReadValue<s32>(multi_play, 0x124) : -1,
                 session_ok ? ReadValue<s32>(session, 0x124) : -1,
                 session_ok ? ReadValue<u8>(session, SessionClosingOffset) : 0xFF);
    }
    const bool result = StartPlayerSummonSearch(player, party_controller->SearchEffectId());
    LOG_INFO(Debug, "Bloodborne party auto-rejoin started search effect={} map={:#x} result={}",
             party_controller->SearchEffectId(), CurrentPlace().map, result);
}

} // namespace Coop

void SetBuildIdentity(std::string_view revision, u64 event_files, u64 eboot, u64 patches_bin) {
    const std::string build = BloodborneCoop::EncodeBuildFingerprint(
        CoopBuildFingerprint(BuildIdentity{.revision = std::string{revision},
                                           .eboot = eboot,
                                           .event_files = event_files,
                                           .patches_bin = patches_bin},
                             [](const char* name) -> const char* { return std::getenv(name); }));
    LOG_INFO(Debug, "Co-op: build fingerprint {}", build);
    {
        std::scoped_lock lock{peer_hello_mutex};
        own_hello = PeerHello{.revision = std::string{revision},
                              .event_files = event_files,
                              .features = WorldLogSwitchOn() ? WorldLogFeature : u8{0},
                              .build = build};
    }
    // The server link's hello: shadNet takes one build only, its own release's (build check on
    // join), and refuses another naming what differs.
    Libraries::Net::SetServerLinkBuild(build);
    Libraries::Net::SetServerLinkObserver([](std::string_view event, std::string_view detail) {
        Trace("link", {{"event", std::string{event}}, {"detail", std::string{detail}}});
    });
}

void NoteSaveFileOpened(std::string_view guest_path) {
    if (const auto slot = CharacterSlotOfSavePath(guest_path)) {
        NoteSaveOpenedForName();
        MakeEnemiesHarder();
        if (character_slot.exchange(*slot) != *slot) {
            LOG_INFO(Debug, "Bloodborne party: character save slot {}", *slot);
            OnLifecycle(Lifecycle::NewCharacter);
        }
    }
}

} // namespace Core::Bloodborne
