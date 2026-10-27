// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// The co-op runtime's internals that its files share (bloodborne_re.cpp and
// the bloodborne_*.cpp split from it): types, constants and templates, and the
// functions and variables one file defines for the others, in namespace Coop.
// Only those files include it.

#pragma once

#include "coop.pb.h"
#include "core/bloodborne_altar.h"
#include "core/bloodborne_crash_dump.h"
#include "core/bloodborne_host_rules.h"
#include "core/bloodborne_invasion.h"
#include "core/bloodborne_party.h"
#include "core/bloodborne_re.h"
#include "core/bloodborne_spectate.h"
#include "core/bloodborne_team.h"
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

#include <httplib.h>
#include "core/bloodborne_re_sites.h"

namespace BloodborneCoop {
struct TeamPing; // coop/coop_messages.h
}

namespace Core::Bloodborne {
namespace Coop {
extern std::atomic<u64> event_sequence;
extern std::mutex capture_mutex;
extern std::ofstream capture_file;
extern uintptr_t image_base;
extern bool negative_area_patch_installed;
extern bool area_flag_patch_installed;
extern bool responder_bell_area_patch_installed;
extern bool responder_bell_common_patch_installed;
extern bool active_bell_negative_area_patch_installed;
extern bool active_bell_area_flag_patch_installed;
extern bool responder_search_negative_area_patch_installed;
extern bool sos_status_area_patch_installed;
extern bool summon_candidate_area_patch_installed;
extern bool summon_build_area_patch_installed;
extern bool summon_build_world_state_patch_installed;
extern bool summon_build_negative_event_patch_installed;
extern bool stage_transition_keep_matching_patch_installed;
extern bool world_teardown_hook_installed;
extern bool ghost_death_revive_hook_installed;
extern bool seamless_travel_hooks_installed;
extern bool stop_request_hook_installed;
extern bool host_death_respawn_hook_installed;
extern bool summon_build_host_placement_hook_installed;
extern bool cross_map_guest_handoff_hook_installed;
extern bool summon_reload_state_hook_installed;
extern bool deferred_summon_reload_hook_installed;
extern bool healing_fountain_host_availability_hook_installed;

struct SummonPlacementDescriptor {
    u32 packed_region{};
    float x{};
    float y{};
    float z{};
    float heading{};
    s32 area{};
};
static_assert(sizeof(SummonPlacementDescriptor) == 0x18);

struct HostPlacementRewriteRecord {
    std::string_view result{"not_attempted"};
    u64 prepared{};
    SummonPlacementDescriptor candidate{};
    SummonPlacementDescriptor host{};
};
extern thread_local HostPlacementRewriteRecord host_placement_rewrite;
extern std::atomic<u32> pending_summon_reload_map;
extern std::atomic<u32> pending_summon_reload_state_map;

struct GuestPlacementHandoffRecord {
    std::string_view result{"not_attempted"};
    u64 object{};
    u64 state{};
    u32 current_map{};
    u32 received_map{};
    u32 selected_map{};
    bool transported_host{};
    bool placement_refreshed{};
    bool select_result{};
    bool summon_reload_armed{};
    s32 transition_result{};
};
extern thread_local GuestPlacementHandoffRecord guest_placement_handoff;

struct SummonReloadStateRecord {
    std::string_view result{"not_pending"};
    u32 target_map{};
    u32 descriptor_map{};
    u64 reload_state{};
    s32 state_before{};
    s32 state_after{};
    bool summon_reload_started{};
    bool state_setter_used{};
    bool placement_refreshed{};
};
extern thread_local SummonReloadStateRecord summon_reload_state;

struct DeferredSummonReloadRecord {
    std::string_view result{"not_pending"};
    u32 target_map{};
    u32 current_map{};
    bool summon_reload_started{};
};
extern thread_local DeferredSummonReloadRecord deferred_summon_reload;

struct PreMatchGuestWarpRecord {
    std::string_view result{"not_pending"};
    u64 state{};
    u32 current_map{};
    u32 target_map{};
    u32 selected_map{};
    s32 multi_play_state{-1};
    bool placement_written{};
    bool select_result{};
    bool responder_resume_armed{};
    s32 transition_result{};
};
extern thread_local PreMatchGuestWarpRecord pre_match_guest_warp;

struct PendingResponderResume {
    u32 target_map{};
    s32 target_area{};
    u32 ready_observations{};
    u32 attempts{};
    bool dispatched{};
    std::chrono::steady_clock::time_point dispatched_at{};
};
extern thread_local PendingResponderResume pending_responder_resume;

struct ResponderResumeRecord {
    std::string_view result{"not_pending"};
    u32 target_map{};
    u32 current_map{};
    s32 target_area{};
    s32 current_area{};
    u64 state{};
    u64 multi_play{};
    s32 multi_play_state{-1};
    u64 matching_controller{};
    u64 player{};
    u32 ready_observations{};
    u32 attempts{};
    bool effect_active{};
    bool native_result{};
};
extern thread_local ResponderResumeRecord responder_resume;

struct HealingFountainAvailabilityRecord {
    std::string_view result{"not_attempted"};
    u64 object{};
    u64 player{};
    bool blocked_before{};
    bool host_effect{};
    bool blocked_after{};
};
extern thread_local HealingFountainAvailabilityRecord healing_fountain_availability;
bool EnvFlagEnabled(const char* name);
std::string BytesToHex(std::span<const u8> bytes);
extern CoopFeatureReport coop_features;
// The channel table is on (see ChannelPolicy): it drops the map item packets itself.
extern bool channel_table_installed;
// Phase 6: the team join's game functions are where the table has them (InstallTeamLobby).
extern bool team_join_ready;
std::string FeatureMismatch(u64 base, u64 image_size, std::string_view feature);
std::string SiteFailure(u64 base, u64 image_size, u64 offset);
bool InstallCoopHook(void* address, std::span<const u8> bytes, u64 tag, GuestCodeHook hook);

bool InstallCoopHook(void* address, std::span<const u8> bytes, u64 tag, GuestFunctionOverride hook);

bool InstallCoopHook(void* address, std::span<const u8> bytes, u64 tag, GuestFunctionResult hook);

bool InstallCoopHook(void* address, std::span<const u8> bytes, u64 tag, GuestRegisterEditHook hook);
bool HasMemoryAccess(u64 address, size_t size, MemoryProt required);

// The value at address + offset, or nothing when that memory is not readable (for reads where 0
// means something, which ReadValue would give then).
template <typename T>
std::optional<T> TryRead(u64 address, size_t offset) {
    T value{};
    if (address > std::numeric_limits<u64>::max() - offset) {
        return std::nullopt;
    }
    const u64 source = address + offset;
    if (source > std::numeric_limits<u64>::max() - sizeof(value)) {
        return std::nullopt;
    }

    if (!HasMemoryAccess(source, sizeof(value), MemoryProt::CpuRead)) {
        return std::nullopt;
    }

    std::memcpy(&value, reinterpret_cast<const void*>(source), sizeof(value));
    return value;
}

template <typename T>
T ReadValue(u64 address, size_t offset) {
    return TryRead<T>(address, offset).value_or(T{});
}
bool HasVtable(u64 object, std::span<const u64> vtables, std::string_view site);
bool HasVtable(u64 object, u64 vtable, std::string_view site);
bool IsCharacter(u64 object, std::string_view site);
bool HasVirtual(u64 object, u64 slot, u64 function, std::string_view site);

template <typename T>
bool WriteValue(u64 address, size_t offset, const T& value) {
    if (address > std::numeric_limits<u64>::max() - offset) {
        return false;
    }
    const u64 destination = address + offset;
    if (!HasMemoryAccess(destination, sizeof(value), MemoryProt::CpuWrite)) {
        return false;
    }

    std::memcpy(reinterpret_cast<void*>(destination), &value, sizeof(value));
    return true;
}
bool WriteTransportedSummonPlacement(u64 state, const SummonPlacementDescriptor& placement);
bool IsUsablePackedMap(u32 packed_region);

bool IsSameSummonPlacement(const SummonPlacementDescriptor& left,
                           const SummonPlacementDescriptor& right);

u64 GetSummonManagerRoot();
u64 GetGlobalState();
u64 GetMatchingState();
u64 GetLocalPlayer();
bool HasPlayerEffect(u64 player, s32 effect_id);
// Takes `effect_id` off the player (the game's thread). False when it is not on.
bool EndPlayerEffect(u64 player, s32 effect_id);
u32 GetCurrentPackedMap();
Place CurrentPlace();
u32 PlayingMap();

// The co-op trace's records (see CoopTraceVersion).
void TraceLife(const char* event, TraceSampler::Fields more = {});
void Trace(std::string kind, TraceSampler::Fields fields);

bool RegisterLifecycleReset(std::string name, std::initializer_list<Lifecycle> events,
                            std::function<void()> reset);
void OnLifecycle(Lifecycle event);
void RefreshSeamlessLocalPlacement();
void ApplyCrossMapSummonHostPlacement(const GuestRegisterSnapshot& registers);

bool IsPartyGuestRole();
void ApplyCrossMapSummonGuestPlacement(const GuestRegisterSnapshot& registers);
void ApplySummonReloadStateAtDescriptor(const GuestRegisterSnapshot& registers);
void ApplyDeferredSummonReload();
void ApplyPreMatchCrossMapGuestWarp();
bool WarpGuestToLampPoint(const LampPoint& point);
// A trip to a warp point (a guest's own, the rig's): at the lamp's own point when it is known
// (returned), else the lamp lookup's trip.
const LampPoint* StartTripToLamp(s32 warp_point);
bool StartPlayerSummonSearch(u64 player, s32 effect_id);
void ResumePreMatchCrossMapResponder();
extern std::optional<PartyController> party_controller;
void InitPartyController();

bool HostHasRoomForMore();
// When the game last posted a summon search (NoteSummonSearchPosted), if ever.
std::optional<std::chrono::steady_clock::time_point> LastSummonSearchPost();

bool EventFlagOn(s32 flag);
// `bits` bits of event flags from `flag` (the game's own read), or nothing without the manager.
std::optional<u32> EventFlagBits(s32 flag, u32 bits);
// The ritual altar the player talks to (its record the current altar's view), if it is there.
std::optional<s32> CurrentRitualAltar();
// Ritual altar `altar` (0-6) loads its dungeon again from its flags.
void ReloadRitualAltar(s32 altar);
// Team altars (bloodborne_altar.h): the current altar's view changed (`changed` flags); a ritual's
// or a join's record goes to the team whole (SharesAltarRecord), anything else stays here.
void ShareRitualAltarRecord(size_t changed);
// As a save or a map loads: every altar record (and the view) that names no dungeon is repaired
// (RepairAltarRecord), before the map's events and items read them.
void RepairRitualAltars();
// Every second: RepairRitualAltars as soon as any record's first words name no dungeon.
void KeepRitualAltarsWhole();
void LogBloodstainStateChanges();
// Kaniel 2026-10-20 (bloodstains on the compass and map): this player's bloodstain as the game
// keeps it (BloodstainOf), nothing in a load.
std::optional<Bloodstain> ReadOwnBloodstain();
bool InPartySession();
// The members of this game's session, itself counted (SprjSessionManager +0xF8..+0x100); 1
// out of one (phase 7).
size_t SessionMembers();
// In the session and in its world (see SessionArrival): a summoned guest only once it arrived.
// Party death counts and posts this member as in the party only then.
bool InSessionWorld();
void ShowGuestBloodstain();
void KeepGuestBloodstainMark();

void ObserveLocalAliveState();
void LogOtherPlayersChanges();
void LogLocalEffectChanges();
void LogActionBlockingState();

std::vector<s32> EffectsOf(u64 character);
std::vector<s32> LocalEffects();

// ForcePlayAnimation(_, entity, animation) event binding, and the lamp touch common event 7200
// plays before it warps (Lua_Warp's 7725 does nothing on the player).
constexpr u64 ForcePlayAnimationOffset = 0x0132E290;
extern bool lamp_rest_hooks_installed;
extern std::atomic<u32> lamp_uses;
extern std::atomic<bool> rest_board_active;
extern std::atomic<s32> rest_board_lamp;
extern std::mutex rest_board_position_mutex;
extern std::optional<LampPosition> rest_board_position;
extern std::mutex rest_board_mutex;
extern std::vector<LampRest> rest_board;
extern std::chrono::steady_clock::time_point rest_board_time;
extern std::atomic<u32> party_map_now;
extern std::atomic<bool> trophy_board_active;
extern std::mutex trophy_mutex;
extern std::vector<s32> trophies_earned_in_party;
void NoteGameTrophyUnlock(s32 trophy);
void ReadTrophyBoard(std::span<const s32> board);
void UnlockPartyTrophies(bool sharing);
extern std::mutex flag_board_mutex;
extern std::vector<std::pair<s32, bool>> flags_to_post;
extern std::mutex echo_board_mutex;
extern std::vector<KillEchoes> echoes_to_post;
extern KillEchoLedger echo_ledger;
extern std::mutex enemy_hp_mutex;
extern MemberMapState local_enemy_hp;
extern std::mutex map_state_mutex;
extern MemberMapState local_map_state;
extern std::optional<MemberMapState> host_map_state;
extern std::mutex shown_board_mutex;
extern MemberMapState local_shown;
extern std::vector<std::pair<s32, bool>> flags_to_set;
void PostWorldChanges(const FlagChanges& changes);
extern std::mutex map_board_mutex;
extern std::vector<MemberMap> map_board;
extern std::string map_board_member;
extern std::chrono::steady_clock::time_point map_board_time;
s32 SessionPlayers();
std::optional<LampPosition> CharacterPosition(u64 character);
std::optional<LampPosition> LocalPlayerPosition();
// The game camera's heading (CameraHeading), nothing while its camera is not found.
std::optional<float> CameraHeadingNow();

s32 CharacterHp(u64 character);

// The other players' characters in this world (WorldChrMan +0x30 count, +0x38 entries of 0x38
// bytes; the local player is one of them), with HP (+0x128 of their data) and position.
struct OtherPlayer {
    u64 character = 0;
    s32 type = 0;
    s32 hp = 0;
    u8 dead_flags = 0;
    std::optional<LampPosition> at;
};
std::vector<OtherPlayer> OtherPlayers();
extern s32 guest_trip_point;
extern bool guest_trip_in_flight;
// A party wipe's trip is under way: its arrival checks for what the save left dead (see
// WipeTripRespawnsOnArrival).
extern std::atomic<bool> wipe_trip_respawn_pending;
// A guest's dead copies of what the host's map generator brought back (BroughtBackByTheHost):
// the bonfire respawn brings exactly those back, at most once every BringBackEvery. The game's
// thread only.
void BringBackWhatTheHostBroughtBack(std::span<const u32> handles);
void MoveToRespawnPoint(bool arriving_from_trip);
extern std::optional<std::chrono::steady_clock::time_point> guest_lamp_trip_at;
constexpr u64 WarpNextStageBonfireOffset = 0x0132E050;
void InstallLampsAsCampfires(u64 base, u64 image_size);
extern std::atomic<u32> coop_trace_file;
bool CoopTraceOn();

void TraceFrame(const char* kind, std::span<const u8> frame, u32 addr, u16 port,
                std::optional<bool> from_peer = std::nullopt);

void TraceBoard(const char* kind, const std::string& endpoint, std::optional<int> status,
                std::string_view body);
std::chrono::sys_seconds LocalTimeNow();
void StartCoopTrace();
extern std::condition_variable_any enemy_hp_wake;
extern bool enemy_hp_changed;
void StartRestBoardClient();

// Characters hidden before a party rest (see KeepDisabledAfterRespawn). WorldChrMan's blocks are
// +0x20 (count) / +0x28 (0x160 bytes each); a block's characters +0xB8 / +0xC0 (0x38 bytes each,
// the character first), the same walk as the respawn (0x01915330). A character's explicit
// disable is [+0x18]+0x20 bit 0, set by 0x018C6390 (the SetDisable binding's call).
constexpr u64 SetCharacterDisabledOffset = 0x018C6390;

template <typename Visit>
void ForEachWorldCharacter(u64 world, Visit&& visit) {
    const s32 blocks = ReadValue<s32>(world, 0x20);
    const u64 block_array = ReadValue<u64>(world, 0x28);
    for (s32 block = 0; block_array >= 0x10000 && block < std::min(blocks, 64); ++block) {
        const u64 entry = block_array + static_cast<u64>(block) * 0x160;
        const s32 count = ReadValue<s32>(entry, 0xB8);
        const u64 characters = ReadValue<u64>(entry, 0xC0);
        for (s32 index = 0; characters >= 0x10000 && index < std::min(count, 1024); ++index) {
            const u64 character = ReadValue<u64>(characters + static_cast<u64>(index) * 0x38, 0);
            if (character >= 0x10000 && HasMemoryAccess(character, 0x3B8, MemoryProt::CpuRead)) {
                visit(character);
            }
        }
    }
}
bool IsCharacterDisabled(u64 character);
// A character's fade (float at +0x20C, 1.0 while shown): a fall into the void fades it out to 0.0,
// which only a map load puts back (rig 2026-10-22). 1.0 if it is no character.
constexpr u64 CharacterFadeOffset = 0x20C;
float CharacterFade(u64 character);
// The character's enable switch as event command 2004:5 sets it; false if it is no character.
bool SetCharacterDisabled(u64 character, bool disabled);
// A character's hits switch: bit 1 (2) of its model's flags ([+0x58]+0x114), set = no hits.
// Event command 2004:39 (bank 2004 handler case 39, 0x017BC152) calls 0x013CABD0 with the entity
// id, which for a character sets or clears that bit (at 0x013CAD09) and nothing else.
constexpr u64 SetEntityHitsOffset = 0x013CABD0;
constexpr u64 SetCharacterHitsBitOffset = 0x013CAD09;
bool CharacterHitsOff(u64 character);
// As 2004:39 does for a character; false if it is no character. The game's thread only.
bool SetCharacterHitsOff(u64 character, bool off);
// Event command 2005:3 (an object's enable switch; bank 2005 handler 0x017C46E0, case 3 at
// 0x017C480C) calls 0x013CABD0 and then 0x013CA580 with the entity id; the latter finds a map
// object in the registry at [0x0553B168] and calls 0x015A6CA0, which sets bit 14 of its +0x1FC.
constexpr u64 EntityObjectRegistryOffset = 0x0553B168;
constexpr u64 SetEntityEnabledOffset = 0x013CA580;
constexpr u64 SetMapObjectEnabledOffset = 0x015A6CA0;
void TickLampRest();

bool EnemiesAreHarder();
void KeepBossHpInStep();

// Diagnostics for a guest alone on a map (the host elsewhere): its enemies lagged and took no
// damage, the host runs them. For the 4 living characters nearest to the local player within
// 40 m: handle, type +0x78, network update authority +0x1EC, state flags +0x1E0, and the
// control block [+0x18] words +0x00..+0x3C, logged every 2 s on change to compare who runs an
// enemy on the host, on a guest beside it and on a guest alone.
void WatchEnemyHp(u64 hp_address, u32 handle);
// Phase 8 (one HP path): this game's own hit, at once to the others through the server, and the
// others' hits as they came (taken once).
struct EnemyHitNews {
    u32 map = 0;
    u32 handle = 0;
    s32 hp = 0;
    s32 max = 0;
    std::string member;
};
void SendEnemyHit(u32 map, u32 handle, s32 hp, s32 max);
std::vector<EnemyHitNews> TakeEnemyHits();
// Team altars: a ritual's record to the others through the server (relayed as it is), and the
// others' as they came (taken once).
struct AltarRecordNews {
    s32 altar = 0;
    AltarRecord record{};
    std::string member;
};
void SendAltarRecord(s32 altar, const AltarRecord& record);
std::vector<AltarRecordNews> TakeAltarRecords();
// Kaniel 2026-10-24: this player's team ping to the server (it sends it on to the team), and the
// team's as they came.
void SendTeamPing(const BloodborneCoop::TeamPing& ping);
std::vector<BloodborneCoop::TeamPing> TakeTeamPings();
// Kaniel 2026-10-25: this player's duel requests to the server, and the duel's states as they
// came (bloodborne_duel_game.cpp).
void SendDuelRequest(bbcoop::DuelRequest::Action action, u32 duel, u32 map);
std::vector<bbcoop::DuelState> TakeDuelStates();
// Phase 8: the owner's activation pass wakes the groups around the other players too (its entry
// adds their groups to its block's record, its end puts the record back; see WithOthersGroups).
void PS4_SYSV_ABI WakeAroundOthers(u64, const GuestRegisterSnapshot* registers);
void PS4_SYSV_ABI PutBackWokenRecords(u64, const GuestRegisterSnapshot* registers);
// After the game's ownership step: the owner runs every awake enemy (see AwakeEnemyWord).
void PS4_SYSV_ABI KeepAwakeEnemies(u64, const GuestRegisterSnapshot* registers);
// Enemy positions (see SendsFullPositionPacket): a follower counts the entries it takes.
void PS4_SYSV_ABI CountPositionTaken(u64, const GuestRegisterSnapshot* registers);
// A position packet (ch 4) went into the game's queues.
void CountPositionPacketIn();
// The game's hand-out of enemies (see HandOutSeconds): the owner's 0x17 to `member` as it sends
// it, and a follower's as its game takes it (the hook at HandOutReceivedOffset). The owner's goes
// out settled (HandOutSettle) where it can be written: its entries are rewritten in place.
void NoteHandOutSent(const std::string& member, std::span<u8> payload, bool writable);
void PS4_SYSV_ABI TakeHandOut(u64, const GuestRegisterSnapshot* registers);
// How many party ticks (frames) have run: the owner's packets a frame.
u64 EnemyFrameSerial();
void LogNearbyEnemyControl();
// Which other players the enemies' AI can take as targets, and whom the enemies near hold.
void LogEnemyTargets();
// The death mark a teammate's body here kept after it got up again on its own game, cleared so
// the enemies take it as a target again (StaleDeathMarks).
void ClearStaleDeathMarks();
void LogJoinTasksOnChange();
void ReconcileMemberPresence();
// The object a character's physics ([[+0x3B0]+0x68]) holds as the one it stands on (read at
// 0x01524B0C; null: none).
constexpr u64 PhysicsContactObjectOffset = 0xA8;
void ForgetGroundOfHidden(u64 character);
u64 MemberIdentityOf(u64 character);

void TickPartyTrip();
void LogJoinTaskChanges();
void TickPlayersOnThisMap();
// Kaniel 2026-10-17: the port's map screen's reads and trips (bloodborne_map_screen.cpp).
void TickMapScreen();
// Kaniel 2026-10-25: the co-op cameras (bloodborne_camera.cpp): the frame step's hook after its
// camera manager's update, installed by InstallCoopCamera. The spectator's camera follows the
// character with `handle` (0: the game's own camera), `name` the teammate's, for the log; the
// pad's right stick turns it (CoopCameraTurn). The free camera's toggle (`forced`: the rig's, the
// setting aside) says whether it is on now.
constexpr u64 CoopCameraHookOffset = 0x0193BE5A;
void PS4_SYSV_ABI CoopCameraStep(u64 tag, const GuestRegisterSnapshot* registers);
void SetSpectateCamera(u32 handle, std::string_view name);
void CoopCameraTurn(float x, float y);
bool ToggleFreeCameraNow(bool forced);
bool FreeCameraOn();

void TickAutoplay();
// The rig's watch-unseen check (bloodborne_autoplay.cpp), after the co-op tick.
void CheckAutoplayWatch();
void LogBossFightBullets();
// How many bullets of that BulletParam id this game made so far.
u64 BulletsMade(s32 bullet);
void TickPartyDeath();
// Kaniel 2026-10-24: team pings (bloodborne_pings_game.cpp), each game tick.
void TickPings();
// Kaniel 2026-10-25: team duels (bloodborne_duel_game.cpp), each game tick after party death.
void TickDuel();

void ReopenClosedPartySession();
void WatchSummonJoin();
void LogUnsharedNearbyCharacters();
bool PartyTripActive();
s64 SteadyMilliseconds();
void TickPartyController();

void PS4_SYSV_ABI TraceEntry(u64 tag, const GuestRegisterSnapshot* registers);
bool PS4_SYSV_ABI OverrideHostDeathRespawn(u64 tag, const GuestRegisterSnapshot* registers);
bool PS4_SYSV_ABI StayWhenTheHostLeaves(u64 tag, const GuestRegisterSnapshot* registers);

void LogGameCallChainAt(const char* what, u64 rsp);

// Seamless travel (see PartyTrip): the stage transition every trip goes through (lamps, the
// Dream's headstones, the event warps), and LeaveSession, which 01.09 calls from several places
// on the way (one is the stage change at 0x0193C689).
constexpr u64 StageTransitionEntryOffset = 0x013CDE30;
void SaveRemoteJoinTasks();
extern std::vector<s32> effects_before_trip;

void ParkEnemyWatch();
extern std::atomic<u64> party_trips_started;

// CSMultiPlayMan (0x05540230) keeps the join tasks in a vector at +0x10..+0x18 (see
// ShouldRecreateAfterTrip). The task's SetStep virtual (vtable +0x28) requests the next step.
constexpr u64 MultiPlayManPointerOffset = 0x05540230;

template <typename Visit>
void ForEachJoinTask(Visit&& visit) {
    const u64 manager = ReadValue<u64>(image_base + MultiPlayManPointerOffset, 0);
    if (manager < 0x10000 || !HasMemoryAccess(manager, 0x20, MemoryProt::CpuRead)) {
        return;
    }
    const u64 begin = ReadValue<u64>(manager, 0x10);
    const u64 end = ReadValue<u64>(manager, 0x18);
    if (begin < 0x10000 || end <= begin || end - begin > 8 * 16 ||
        !HasMemoryAccess(begin, end - begin, MemoryProt::CpuRead)) {
        return;
    }
    for (u64 entry = begin; entry < end; entry += 8) {
        const u64 task = ReadValue<u64>(entry, 0);
        if (task >= 0x10000 && HasMemoryAccess(task, 0xF0, MemoryProt::CpuRead)) {
            visit(task);
        }
    }
}

// What the game does when a member joins (0x0178C20C): allocate 0x100 bytes from the allocator
// at 0x05540438 (vtable +0x58), construct the task (0x01E4F9F0: task, member id, kind) and add it
// to CSMultiPlayMan +0x08 (0x01E55A70). Member ids are 0x20 bytes: SprjSessionManager
// (0x05540290) lists the session's at +0xF8..+0x100 and holds the local one at +0xD0; the game
// compares them with 0x00CA1C70.
constexpr u64 SessionManagerPointerOffset = 0x05540290;
constexpr u64 MemberIdEqualOffset = 0x00CA1C70;
extern std::vector<SavedJoinTask> saved_join_tasks;
extern std::map<u64, MemberDataWait> joins_waiting_for_data;
// `build_missing`: build a task for a member that has none (after a trip, whose load emptied the
// list). Not after a join: a member without a task there is one whose join the game is still
// making, and the game's own join (0x0178C1E3) makes no task for a member that already has one,
// so one built there first (as kind 0, rig smoke 2026-10-12) kept that guest invisible.
void RecreateRemotePlayers(std::span<const u64> only, bool build_missing);
std::optional<u32> HostMapFromItsPost();
std::vector<u64> OtherSessionMembers();

std::optional<std::string> SessionMemberBoardName(u64 member,
                                                  const std::vector<std::string>& online_ids);
extern bool restore_search_pending;
void RestoreSearchAfterTrip(bool in_session);
void PlayGuestAsHost(bool in_session);
void TurnOffMultiplayerWalls(bool in_session);
u64 FindLoadedParam(std::string_view type);
void KeepGuestFullHp();
void MakeEnemiesHarder();
// Kaniel 2026-10-25, the host rules (bloodborne_host_rules.h): this launcher's own, and the ones
// this game plays by (its team's while in one, else its own).
const HostRules& OwnHostRules();
HostRules ActiveHostRules();
// The hunters on this player's map (1 alone, or when the party's map board is old).
std::size_t HuntersOnMyMap();
// The server's rot for this player once its lobby said it (taken once), and this player's rot
// to the server (in a team only).
std::optional<std::vector<u32>> TakeKeptRot();
void ReportRot(std::span<const u32> rot);
// The game thread's host rules step (bloodborne_host_rules_game.cpp): the scaling, the rot.
void TickHostRules();
// From the hit hooks: an enemy's hit on this player (its handle), and this player's kill.
void NoteHitOnMe(u32 attacker);
void NoteOwnKill(u32 enemy);
// For the party HUD: the rot this player has (RotKind numbers, the oldest first).
struct RotView {
    bool on = false;
    std::vector<u32> kinds;
    std::string text;
};
RotView ReadRotView();
// Kaniel 2026-10-25, the travel together rule: this player's lamp or headstone trip to `point`
// (the trip hook, the game thread): true when the question takes it over (the game's own trip is
// not made then). The server's news, the overlay's answer and view, the game thread's step.
bool OfferTeamTravel(s32 point);
void TakeTeamTravelNews(const bbcoop::TeamTravel& travel);
void SendTeamTravel(const bbcoop::TeamTravel& travel);
void AnswerTeamTravel(bool yes);
std::optional<TeamTravelView> ReadTeamTravel();
void TickTeamTravel();
// A team trip to `point` for this player, made by the map screen's step when nothing stops it (a
// boss fight, a down, a load).
void RequestTeamTrip(s32 point);
// The loaded SpEffectParam, all of it writable, or none.
std::optional<std::span<u8>> WritableSpEffects(u64 table);
void PolishMultiplayerUi();
void InstallNoLevelSyncInParty(u64 base, u64 image_size);
void InstallSolidNpcsInParty(u64 base, u64 image_size);
void InstallFloorGuard(u64 base, u64 image_size);
// A flag value read over a flag block that is missing reads as zeros (bloodborne_party_rules.cpp).
void InstallFlagBlockGuard(u64 base, u64 image_size);
void InstallMemberPacketGuard(u64 base, u64 image_size);

// Diagnostics for shared loot (RE trace only): the map item module (MapItemMan, 0x017D-0x017E)
// asks "am I a summoned guest" (matching state 6) or "am I the host" (3) at these spots. Counting
// which run while a guest walks up to and picks up an item shows where the guest is held back.
struct ItemModuleCheck {
    u64 offset;
    std::array<u8, 7> bytes;
};
constexpr std::array<u8, 7> GuestStateCheck{0x83, 0xB8, 0x24, 0x01, 0x00, 0x00, 0x06};
constexpr std::array<u8, 7> GuestStateCheckRcx{0x83, 0xB9, 0x24, 0x01, 0x00, 0x00, 0x06};
constexpr std::array<u8, 7> HostStateCheck{0x83, 0xB8, 0x24, 0x01, 0x00, 0x00, 0x03};

constexpr auto ItemModuleChecks = std::to_array<ItemModuleCheck>({
    {0x017D3759, GuestStateCheck},
    {0x017D4F54, GuestStateCheck},
    {0x017D6818, GuestStateCheck},
    {0x017D6AD2, GuestStateCheck},
    {0x017D83CB, GuestStateCheck},
    {0x017D853B, GuestStateCheck},
    {0x017D88BB, GuestStateCheck},
    {0x017D9AA8, GuestStateCheckRcx},
    {0x017D9E1C, GuestStateCheck},
    {0x017D9EE0, GuestStateCheckRcx},
    {0x017DDCD9, GuestStateCheck},
    {0x017DE5EC, GuestStateCheck},
    {0x017DE738, GuestStateCheck},
    {0x017DE79D, HostStateCheck},
    {0x017DE8CB, GuestStateCheck},
    {0x017DE925, HostStateCheck},
});
extern std::array<std::atomic<u64>, ItemModuleChecks.size()> item_module_check_hits;
void PS4_SYSV_ABI CountItemModuleCheck(u64 tag, const GuestRegisterSnapshot*);

void SetGuestPickupsBeforeMapItems();

// Diagnostics: who adds echoes (AddSouls 0x018FC600(player, amount, _, apply rates)).
constexpr u64 AddSoulsOffset = 0x018FC600;

bool PostsKillEchoesInSession();
void InstallSharedLoot(u64 base, u64 image_size);
void InstallItemModuleDiagnostics(u64 base, u64 image_size);
void LogItemModuleChecks();
constexpr u64 MapItemManPointerOffset = 0x0553D6E0;
extern std::array<std::atomic<u64>, 6> item_button_counts;
void InstallItemButtonDiagnostics(u64 base, u64 image_size);
std::optional<std::array<float, 3>> MapItemSearchPlayerPosition();

struct MapItemSpot {
    u32 handle;
    std::array<float, 3> at;
};
std::vector<std::pair<float, MapItemSpot>> UsableMapItemsNear(const std::array<float, 3>& from);
void LogItemButtonAndMapItems();
extern std::atomic<int> character_slot;

// Sets the kept world changes in the guest's world: how many took. The ones that took are done
// unless `keep` (a trip's load: the game puts its own saved world back after it, so the trip's
// arrival sets them again).
size_t SetGuestWorldChanges(bool keep = false);

std::optional<s32> CountSetFlags();

// From the map item creation hook: before a load creates the guest's map items.
size_t ApplyHostMapState();

// A copy of the whole event flag store (see ChangedFlags): the manager's block map at +0x38 is a
// tree (left +0x00, right +0x10, nil marker +0x19, block id +0x20); a block's bits are at
// [node+0x30] (kind 2 at +0x28) or in the pool +0x28 at index [node+0x30] (kind 1), +0x20 bytes
// each.
struct FlagBlock {
    u32 id = 0;
    std::vector<u8> bytes;
    u64 address = 0;
};

struct FlagStore {
    u32 flags_per_block = 0;
    std::vector<FlagBlock> blocks;
    // Every node and block was read (none unmapped, the walk not cut short).
    bool whole = false;
};
FlagStore ReadFlagStore();
void LogWatchedWrites();
void InstallLampsStayLitAtRegistration(u64 base, u64 image_size);

// A party guest's boss item (see DefersItemLotAward): held at the award's entry and given
// from the tick once the boss-clear sequence is over.
constexpr u64 AwardItemLotOffset = 0x017DDC50;
extern thread_local bool giving_deferred_item_lot;
void GiveDeferredBossItems();
// A party guest's own talk set an NPC gift's trigger (GivesGuestNpcGift): its lot is given and the
// trigger cleared NpcGiftDelay later, on the game's thread (GiveDeferredBossItems).
void QueueGuestNpcGift(const NpcGift& gift);
void InstallKeepLocalFlags(u64 base, u64 image_size);
void InstallDeferredBossItems(u64 base, u64 image_size);
extern std::atomic<s64> world_changed_ms;
// The last load that started a new world (StartsNewWorld) alone, for StoryRemovals: no owner
// change or wipe moves it.
extern std::atomic<s64> world_loaded_ms;
void LogFlagChangesInSession(bool sharing);
void KeepGuestProgress(bool in_session);
void SetPartyWorldChanges(bool sharing);
void GrantPartyEchoes();
// Whether this guest hid or switched off the character to match the host (game thread).
bool ChangedLikeTheHost(u32 handle);
void ClaimEnemiesWhenAlone(bool in_session, u32 map);
// Entity ids by character object (WorldChrMan's tables, as the lookup 0x0191A440 finds them).
std::unordered_map<u64, s32> CharacterEntityIds(u64 world);
// Phase 4 (see EnemyRole), the port: the server's owner of this
// member's area decides who runs its enemies, instead of ClaimEnemiesWhenAlone.
bool EnemyOwnerOn();
// With the owners on: whether the server sent the kill step something since the last call (the
// owner's HP, kills), so it runs at once.
bool TakeServerWorldNews();
// The server's owners, or none when not heard of for EnemyOwnersFreshMs.
std::vector<EnemyOwnerEntry> ServerEnemyOwners(s64 now_ms);
// Once per party tick: this member's role on its map, the ownership words of its characters and
// the owner's frame and send rates in the log.
void TickEnemyOwner(bool in_session, u32 map);
// This member's role and the owner it follows (its board name), as the last tick found them: for
// the hooks (any thread).
EnemyRole CurrentEnemyRole();
// In a party session with the host away from this game's map (the map board fresh).
bool HostAwayFromThisMap();
std::string CurrentEnemyOwner();
// The owner got another member's hit (0x15): the enemy step runs on the next frames, so the HP
// it takes goes to the log at once.
void ForceEnemySteps();
// A game packet this member sent on `channel` (the send's entry): counted for the owner's rates.
void CountEnemyPacketSent(u32 channel);
extern std::mutex own_hits_mutex;
extern OwnDamage own_damage;
void InstallOwnHitCount(u64 base, u64 image_size);
extern DirectDamageInbox direct_damage_inbox;
extern std::mutex direct_boards_mutex;
extern DirectInbox<DirectVitals> direct_vitals_inbox;
extern DirectInbox<DirectKills> direct_kills_inbox;
extern std::mutex peer_hello_mutex;
extern std::optional<PeerHello> own_hello;
bool TakeDirectDamage();
// A co-op frame to every room peer, or to one (both traced).
void SendToRoomPeers(const std::vector<u8>& frame);
void SendToPeer(const std::vector<u8>& frame, u32 addr, u16 port);
// The signed-in user's online id, or "" before shadNet's login.
std::string LocalOnlineId();
// Phase 11: this game's character's name (PlayerGameData +0x5D0, what the game sends the others),
// "" before a save is loaded or when it does not read. Players see only characters' names.
std::string OwnCharacterName();
void SendDirectKills(std::span<const u32> kills, bool changed);
// The world log's runtime (bloodborne_world_log.cpp, docs/WORLD_SYNC.md Stage C).
// SHADPS4_BLOODBORNE_WORLD_LOG (this game's hello says so: WorldLogFeature).
bool WorldLogSwitchOn();
// A world log frame from a room peer (game thread).
void TakeWorldLogFrame(u8 type, std::span<const u8> payload, u32 addr, u16 port);
// The kill step's view of the party's kills: `members` the other members' (the log's or the kill
// board's), `all` with this member's own and held ones and, on the board path, the peers' direct
// frames. Decides the path, runs the log (or posts `changes` to the board) on the way.
struct PartyKillsNow {
    std::unordered_set<u32> all;
    std::unordered_set<u32> members;
};
// `forgot`: this kill step took back this member's kills for its own wipe; `not_mine`: kills of
// other members that died here by the game's kill, not proposed as this member's.
PartyKillsNow GatherPartyKills(const VisitKills& visit_kills, u32 map,
                               std::span<const std::pair<s32, bool>> changes, bool forgot,
                               const std::unordered_set<u32>& not_mine);
// The party's world on the server (bloodborne_server_world.cpp, docs/SERVER_COOP_PLAN.md phase
// 3): always in the port, never in shadPS4.
bool ServerWorldOn();
// The kill step (game thread): this member's standing kills to shadNet (`propose`: in the session
// and not held by a wipe), and the party's kills as the server's log has them.
struct ServerWorldKills {
    std::unordered_set<u32> kills;
    bool host_wiped = false; // the party's log started over after a wipe (see WorldLogWipeHold)
    bool fresh = false;      // in step with the server's log
    std::vector<WorldLogEntry> applied; // what came in this step (the flag events among them)
    WorldLogReturnPoint return_point;   // the host's, as the log has it
};
ServerWorldKills TickServerWorld(const WorldLogKills& mine, bool propose, s64 now_ms);
// The maps the party's log reset on their own since the last call (a Reset of one map: everyone
// on it died while another member stood elsewhere); the game thread.
std::vector<u32> TakeLogMapResets();
// This member's real death reset its map while another member stood elsewhere (DeathReset::
// OwnMap): shadNet starts that map over unless someone stands on it.
void TellServerMapWipe(u32 map);
// Step 3b: the host's characters on its map, its map's state and its return point, whole (sent on
// a change); any member's shared flag events and HP report.
void ShowToServer(u32 map, std::span<const CharacterShown> characters);
void MapStateToServer(u32 map, std::span<const u8> bits);
void ReturnPointToServer(s32 point, u32 map);
void FlagsToServer(std::span<const std::pair<s32, bool>> shared);
void HpToServer(u32 map, const EnemyHpReport& report, bool changed);
// The party's log as this member has it: the host's characters and map state (none until known),
// every member's HP reports.
std::optional<WorldLogShown> ServerShown();
std::optional<WorldLogMapState> ServerMapState();
WorldLogHp ServerHp();
// Step 3c, what the HTTP boards carried: the stream (its thread) runs without a kill step too.
void StartServerWorld();
// Phase 5, the team lobby (bloodborne_team.h): always in the port, with the server world.
// The party is the team's: its key and this game's role come with the team's Start.
bool TeamModeOn();
// Phase 6 (bloodborne_join.cpp, see TeamJoin): the host opens its game session as a summon
// does; its join data (the summon packet's, without the sign's two words); a guest joins from it
// in place. Game thread.
bool OpenTeamSession();
std::optional<std::vector<u8>> TeamSessionJoinData();
bool JoinTeamSession(std::span<const u8> join, u32 kind, u32 sign);
// Each party controller tick in team mode (bloodborne_server_world.cpp): opens, announces or
// joins the team's session (`ready`: the controller's go, where it would ring the bell).
// False when not in team mode (the bell and signs then).
bool TickTeamJoin(bool ready);
// The placement a guest's join keeps (its own spot), as a claim's header hands it.
void ReceiveSeamlessPlacement(const SummonPlacementDescriptor& placement);
// At the game's start: signs in to the server's lobby once the game has its online id.
void StartTeamLobby();
// The title loads the save in `slot` (TitleStep's load state, the game thread): true holds the load
// a frame more (waiting for the team's Start); false lets it go, the team's party set if any.
// `fresh`: the game starts that character at the beginning (a new one: Kaniel 2026-10-16, its
// team's world is a new one unless the leader picks another).
bool HoldTitleLoadForTeam(u32 slot, bool fresh = false);
// A save file opened (NoteSaveFileOpened): the game's character name is the save's soon after.
void NoteSaveOpenedForName();
// The title menu is up again: the next load asks the team again.
void TeamBackAtTitle();
// Kaniel 2026-10-16: a save loads from the title: what the last one's story removed is forgotten
// (HoldStoryRemovals).
void ForgetStoryRemovals();
// The team screen (bbport_overlay.cpp) is up: the title's load is held for it. Its buttons.
bool TeamScreenUp();
// A team's guest is loading or joining its host's session: the port holds a loading screen.
TeamLoad::View TeamLoadView();
// The team's loading screen is up: it holds the game's input and its sound (the port's audio).
bool TeamLoadCovers();
// Options held down for the game to skip a cutscene under that screen (the port's pad).
bool CutsceneSkipPressed();
// Kaniel 2026-10-17: Options while a cutscene plays in a party session is a vote (CutsceneVote):
// what the pad does with it (`options`: the player holds it), and the vote as the screen shows it
// (false: nothing to show).
CutsceneVote::Pad CutsceneVotePad(bool options);
bool CutsceneVoteView(u32& want, u32& watchers);
// The rig's cutscene-options: Options pressed once the next cutscene has played `after_ms`.
void ArmCutsceneOptions(s64 after_ms);
// SprjRemoMan's step methods (tag: the step), hooked at their entries.
void PS4_SYSV_ABI NoteRemoStep(u64 step, const GuestRegisterSnapshot* registers);
void TeamChoose(TeamGate::Choice choice);
// Kaniel 2026-10-16: the team screen over the game, opened from its System menu (the port's
// "Co-op Team" page) and closed by its own buttons; Circle for the game's page under it a moment
// after it closed (the port's pad); whether a save plays (and in its team's world); the menu's
// "Join the hunt" (an invitation's team, or "" in a team already).
void TeamMenu(bool open, bool from_page);
bool TeamMenuOpen();
bool TeamMenuBackPressed();
// The rig's team-worlds verb asked the team screen to open its world list (once).
bool TeamWorldListAsked();
bool TeamPlaying(bool* in_hunt);
// Kaniel 2026-10-25: the game's KillCharacter (KillCharacterOffset) may be called: its entry as
// the game has it (KillCharacterStart), or party death's hook over it (boss fight deaths).
constexpr std::array<u8, 6> KillCharacterStart{0x55, 0x48, 0x89, 0xE5, 0x41, 0x57};
bool KillCharacterCallable();
// Kaniel 2026-10-25: the team's death mode as this game's lobby has it (TeamDeathMode).
DeathMode TeamDeathModeNow();
void TeamJoinHunt(const std::string& invite_team);
// The rig's in-game team menu verbs (menu-invite, menu-kick, menu-join, menu-leave).
bool RunTeamMenuVerb(const AutoplayCommand& command, bool begun, double elapsed,
                     std::string& failure);
// The rig's screen verbs (team-ready, team-start, team-alone) want the title's Cross until the
// screen is up.
bool IsTeamScreenVerb(std::string_view verb);
// The rig's lobby verbs (team-*), run on the title screen: true once done, `failure` if failed.
bool RunTeamTitleVerb(const AutoplayCommand& command, bool begun, double elapsed,
                      std::string& failure);
bool IsTeamTitleVerb(std::string_view verb);
// Phase 5b: the team's world goes into the save as it loads (before its map items), or the save's
// own world comes back when it plays alone; the team's host reports its world now and then.
// `trip`: a guest's party trip, whose load gives its game its own saved world back: every block
// takes the team's world again (TakesTeamWorldBlock).
void ApplyTeamWorldOnLoad(bool trip = false);
// A team guest's copy of the team's world: its flags on (none for the team's host, or with no
// world yet).
std::vector<s32> TeamWorldFlagsOnForGuest();
// The blocks the team's world covers, for a team guest (none otherwise).
std::vector<u32> TeamWorldBlocksForGuest();
// Puts lamps (entities, LampOfActiveFlag) on this character's lamp list, so they stay lit for it.
void ListLamps(std::span<const s32> lamps);
void TickTeamWorld(s64 now_ms);
// A team guest takes the team world's time of day (TimeOfDayToFollow) when it is not loading.
void FollowTeamTimeOfDay(s64 now_ms);
// At a team guest's load, after the session's changes: the team world's time of day exactly
// (TeamTimeOfDayAtLoad).
void ApplyTeamTimeOfDayOnLoad();
// A team guest takes the story steps the host's world reports change (TeamStoryToFollow).
void FollowTeamStory(s64 now_ms);
// The lobby as the server last sent it, for the game's menu (none before its answer), and this
// player's lobby name; a request from the menu.
std::optional<bbcoop::LobbyState> TeamLobbyState();
// Phase 7: where the server says this member last stood in its team's world (once per run).
std::optional<ResumePoint> TakeServerResumePoint();
// A new character in the slot: the server's point (the old character's) does not count this run.
void RefuseServerResumePoint();
// Kaniel 2026-10-18: a guest's team load (not a new character's) ends beside the team's host: the
// resume takes the host's place, and the team's screen stays up until it is done (EndTeamResume).
bool TeamResumeDue();
void EndTeamResume();
// The team's host as its Start named it ("" playing alone).
std::string TeamStartHost();
std::string TeamLobbyMember();
// Phase 11: a member's character name (its board name), as the server's lobby last said it; ""
// when the lobby does not know it.
std::string CharacterNameOf(std::string_view member);
void TeamLobbyRequest(bbcoop::TeamRequest::Action action, const std::string& id);
// Invasions (bloodborne_invasion.h; bloodborne_server_world.cpp). The menu's "Invade a hunt" and
// "Leave the invasion" (InvadeRequest, `cancel`), and the leader's "Allow invaders"; what the
// notice shows; whether the team allows invaders now.
void InvasionRequest(bool cancel);
void TeamAllowInvaders(bool allow);
InvasionView InvasionViewNow();
bool TeamAllowsInvaders();
// This game is in a world it invades, or on its way in or home: that world is not its own (its
// flags, lamps, echoes and save stay its own).
bool Invading();
// The invader's board name in this team's world now, "" without one.
std::string InvaderInWorld();
// Each party controller tick, before the team's join (game thread): the invader's trip.
void TickInvasion();
// A save's load: an invasion's own world kept aside and not put back (a crash mid-visit) comes
// back first.
void RestoreInvasionHomeOnLoad();
// Party death's side (bloodborne_party_death.cpp): the invader's bodies here (the presence tick
// finds them, InvaderBodies), its hits on the local player (the hit hook), whether a lethal check
// lets the death through, and the invader's own death in the target's world (revived in place,
// InvaderGoHome takes it home).
void SetInvaderHandles(std::vector<u32> handles);
void SetInvasionTint(u64 character, bool on);
void QueueInvasionRewardAck(bbcoop::Envelope envelope);
std::vector<u32> InvaderHandles();
bool IsInvaderHandle(u32 handle);
void NoteInvaderHit();
bool InvaderHitFresh();
bool TakeInvaderDeath();
void ReviveInvaderInPlace();
// This member's part of each board (bloodborne_boards.cpp's rounds send them).
void SendServerMap(u32 map);
void SendServerRest(s32 lamp, const std::optional<LampPosition>& at);
void SendServerTrophies(std::span<const s32> trophies);
void SendServerEchoes(std::span<const KillEchoes> echoes);
void SendServerVitals(const MemberVitals& mine, std::span<const ReviveHitTotal> hits);
// The portrait's pixels as its parts carry them (SplitPortrait), or none.
void SendServerPortrait(u32 id, std::span<const u8> pixels);
// The boards as the server sends them, taken on the stream's thread (bloodborne_boards.cpp).
void TakeServerMapBoard(std::vector<MemberMap> maps);
void TakeServerRestBoard(std::vector<LampRest> rests);
void TakeServerEchoes(std::vector<KillEchoes> echoes);
void TakeServerVitals(VitalsBoard board);
void TakeServerPortrait(const std::string& member, u32 id, std::vector<u8> pixels);
// The host's characters on its map (step 2): to the log while the shown characters take it (the
// shown board gets them as before, the fallback copy).
void ShowOnTheLog(u32 map, std::span<const CharacterShown> characters);
// What a guest follows of the host's characters: the log's copy while the shown characters take
// it and the copy is fresh and whole, else the shown board's.
std::optional<MemberMapState> HostShownNow();
// Step 3. A change's shared flags (FlagsToShare) to the log: true when the log takes them (else
// they go to the flag board as before).
bool TakeFlagsForTheLog(std::span<const std::pair<s32, bool>> shared);
// The flag board's changes, under flag_board_mutex: while the log carries the flags, the ones the
// log brought already (or brings later) count once (FlagEventOnce).
void FilterBoardFlags(std::vector<std::pair<s32, bool>>& changes);
// The host's map state (see SnapshotHostMapState) to the log while it carries the flags.
void MapStateOnTheLog(u32 map, std::span<const u8> bits);
// What a guest's load takes of the host's map flags: the log's copy while it is whole and fresh,
// else the map state board's.
std::optional<MemberMapState> HostMapStateNow();
// A guest's copy of the host's return point (GlobalState +0x1528, see PartyRespawnPoint): the last
// one the log brought in this session, kept through the host's new logs (a wipe starts one just as
// the guest's death respawn wants it). None out of a session or before the log brought one.
std::optional<s32> HostReturnPointNow();
// Step 4. The other members' HP reports for SharedDamage (`own_source`: this member's): the log's
// while it carries them (the host's own, a member's copy while fresh), else the peers' direct
// frames and the HP board, as before.
std::vector<MemberMapState> OthersEnemyHp(u64 own_source);
// This member's HP report: the direct frame (0x30) to every peer, and to the host's log while it
// runs.
void SendEnemyHp(const MemberMapState& mine, bool changed);
// The world snapshot (WorldSnapshotPostMs), for the board client's thread: whether the log carries
// the kill, shown, flag, map state and HP boards' jobs now (they are not polled then); the host's
// next blob to post; whether this member wants the server's copy (its own is not fresh); the one
// read.
bool WorldLogCarriesBoards();
std::optional<std::vector<u8>> TakeWorldSnapshotToPost();
bool WorldSnapshotWanted();
void NoteWorldSnapshot(s64 age_ms, std::vector<u8> blob);
void SendDirectDamage(const MemberMapState& mine, bool changed);
void SendPeerHello();
// Party portraits (bloodborne_portrait.h): this character's own (the status page's capture, else
// the one kept for its face) to the room's peers; the list slot `slot` shows `member` (its board
// name, "" nobody): whether that is a portrait.
void TickPortraits();
bool ShowPartyPortrait(int slot, std::string_view member);
void KeepEnemyDeathsInStep(bool sharing);
void SnapshotHostMapState(bool sharing);
void TeleportLocalPlayer(const LampPosition& at, float heading = 0.0f);
// Has every camera jump to where it should be on its next update (see PlacementCalls).
void SnapCamera();
// Any character put there as the player's teleport does (vtable +0x5F8, see PlaceCharacter).
bool PlaceCharacterAt(u64 character, const std::array<float, 4>& position,
                      const std::array<float, 4>& rotation, const char* what);
// Moves the local player the way the game places it at a lamp (see PlaceCharacter); `what` names
// the caller in the character check's log.
bool PlaceLocalPlayer(const std::array<float, 4>& position, const std::array<float, 4>& rotation,
                      const char* what);
std::optional<float> LocalPlayerHeading();

void FinishWaitingJoins(bool in_session);
void ResendOwnDataOnChange(bool in_session);
void LogMemberDataChanges(bool in_session);
u64 OwnGameData();
u32 OwnCovenantEffect();
std::optional<std::pair<u32,u32>> InvasionDregs();
void InstallSeamlessTravel(u64 base, u64 image_size);
void EndSessionOnWorldTeardown();
void ReviveGuestAfterGhostDeath();
extern std::mutex vitals_mutex;
void NoteCutsceneVote(u32 state);
extern VitalsBoard vitals_board;
// The invader's lines of the server's vitals board (TakeHostileLines): only presence reads them.
extern std::vector<MemberVitals> hostile_vitals;
extern std::string vitals_member;
extern std::optional<std::chrono::steady_clock::time_point> vitals_heard_at;

struct VitalsView {
    VitalsBoard board;
    std::string member;
    s64 age_ms = std::numeric_limits<s64>::max();

    bool Fresh() const {
        return age_ms <= 3'000;
    }
};
VitalsView ReadVitalsBoard();
// The bosses this member's game runs now (BossClaims), posted with its vitals.
std::vector<u32> OwnBossClaims();
void PostVitals(const MemberVitals& mine, std::vector<ReviveHitTotal> hits);
void StartVitalsBoardClient();

// The local player's side of party death, on the game thread.
struct PartyDeathRun {
    // Down ids start somewhere new each run, so the party's hits on an old down never count.
    DownedState downed{static_cast<u32>(std::random_device{}() % 0x40000000U) + 1};
    HoldToConfirm give_up{GiveUpHoldMs};
    ReviveHits revive_hits;
    LethalCatches lethal_catches;
    u32 seen_boss_catches = 0; // Kaniel 2026-10-25: a boss fight's catches (spectate)
    u32 seen_attacks = 0;
    u32 seen_lamp_uses = 0;
    u32 handled_wipe = 0;
    u8 flags_added = 0;
    u8 hide_before = 0;
    u8 silence_before = 0;
    u32 targets_forgotten = 0;          // since the last log line
    DownedTeams downed_teams;           // the downed members' bodies here, struck back up
    DownedBodyPicker downed_bodies;     // which body here is each downed member's
    RevivePressFallback press_fallback; // swings at a downed body whose hit never landed
    // After a revive the player takes no damage for ReviveGraceMs: the party's last swings, still
    // landing on its body as the invaders' team on their screens, would put it straight down again.
    std::optional<s64> grace_until;
    u8 grace_flags = 0;
    u32 real_hits_logged = 0;
    ReviveHitScale revive_scale; // see ReviveHitScale
    // The kneel not asked for yet in this down (TakesAnimationRequest), and when the down began.
    bool kneel_waiting = false;
    s64 down_since = 0;
    // Since when a down has been out of the party session (a few seconds of it end the down).
    std::optional<s64> down_out_of_session_since;
    // Kaniel 2026-10-25: spectating teammates (DeathMode::Spectate, bloodborne_spectate.h): the
    // one watched, the teammates' lamps and the bosses' flags that wake this player, since when
    // nobody else is on its map, and its body's fade from before (it lies unseen meanwhile).
    std::optional<std::string> watched;
    u32 watched_handle = 0;
    LampUseWatch lamp_watch;
    BossDownWatch boss_watch;
    s64 boss_read_at = 0;
    std::optional<s64> alone_since;
    float fade_before = 1.0f;
};
extern PartyDeathRun party_death_run;
// Co-op work on the game's threads, logged next to the port's Perf lines (see CoopTimeMeter).
extern CoopTimeMeter coop_time;
// The co-op tick's parts, the slowest few logged after the Co-op time line.
extern CoopTimeMeter coop_tick_parts;
// One part of the co-op tick, into coop_tick_parts (AtiniirSpectre 2026-10-23: the tick grew
// after a death and the log could not say which part), until the end of the scope.
class TickPartTimer {
public:
    explicit TickPartTimer(std::string_view name) : name{name} {}
    TickPartTimer(const TickPartTimer&) = delete;
    TickPartTimer& operator=(const TickPartTimer&) = delete;
    ~TickPartTimer();

private:
    std::string_view name;
    std::chrono::steady_clock::time_point started = std::chrono::steady_clock::now();
};
void ReportCoopTime(s64 now_ms);
void InstallPartyDeath(u64 base, u64 image_size);
void ApplyHealingFountainHostAvailability(const GuestRegisterSnapshot& registers);
u64 GetSummonManager();
void WriteHex(std::ostream& out, u64 value);
bool VerifySites();

} // namespace Coop

using namespace Coop;

} // namespace Core::Bloodborne
