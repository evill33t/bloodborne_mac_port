// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <functional>
#include <limits>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <unordered_set>
#include <vector>

#include "common/types.h"
#include "coop/build_fingerprint.h"
#include "coop/packet_routes.h"
#include "coop/world_log.h"
#include "core/bloodborne_patch_sites.h"

namespace Core::Bloodborne {

// The world log's types and logic are shared with shadNet (server/coop/world_log.h).
using namespace ::BloodborneCoop;
// "seq 3 kill 0x10014066 on 0x18000000" (here: it names the map flags with MapFlagBase)
std::string FormatWorldLogEntry(const WorldLogEntry& entry);

// Validates a "<name>:<secret>" party setting and returns the header value to send.
std::optional<std::string> ParsePartySetting(std::string_view setting);

enum class PartyRole { Host, Guest };
// EndSearch: end the role's search effect (a stale bell, see PartyController::Tick); the search
// starts again on a later tick, once the effect is seen off.
enum class PartyAction { Wait, StartSearch, EndSearch };
// The session effect (9001) a party member carries while in a session. Rig 2026-10-13: a guest
// that had travelled (a live player) kept it after its host's game died, as no leave limit ran,
// and with it on, its bell never posted a sign; a guest out of its session takes it off first.
constexpr s32 SessionEffect = 9001;
bool EndsStaleSessionEffect(PartyRole role, bool in_session, std::span<const s32> effects);

// The special effects a rung bell leaves on the player. While one is active the game's SOS
// status (0x0186FE40) reports the player as searching, so adding the effect is the whole bell.
// The Beckoning Bell's is 9000 (special state 189); the game adds 9003 next to it by itself.
constexpr s32 BeckoningBellEffect = 9000;
constexpr s32 SmallResonantBellEffect = 9005;

// 01.09's add-special-effect virtual (character vtable slot 0x3F0, 0x018C6DB0 for players).
// Only the effect id, the source and the 1.0 scales are understood; the rest are passed exactly
// as the game passes them.
using AddSpEffectFn = void PS4_SYSV_ABI (*)(u64 character, s32 effect_id, u64 source, s32 arg3,
                                            s32 arg4, s32 arg5, float scale0, float scale1,
                                            float scale2, float scale3, float scale4,
                                            s32 stack_arg);

// Starts the summon search without the bell item: adds the search effect to the player the same
// way 01.09 adds 9005 itself (0x01C0D3B0). No use-item path, so no animation, item or Insight.
bool StartSummonSearch(u64 player, s32 effect_id, AddSpEffectFn add_sp_effect);

// What the game looks like this tick, read from guest memory by bloodborne_re.cpp.
struct PartyObservation {
    bool player_valid = false;
    bool map_usable = false;
    bool stage_transition = false;
    bool matching_idle = false; // no multiplayer session, not even a pending one
    bool search_effect_active = false;
    bool white_phantom = false; // still in a host's world (the host may have left)
    // Hosting, with a party member online that is not in the session yet (a party of three:
    // the Beckoning Bell's effect ends when the first cooperator arrives).
    bool room_for_more = false;
    // A boss fight or its clear sequence runs (event flags 4000 / 4047): a player joining then is
    // kicked by the game, which ends the session.
    bool boss_sequence = false;
    // When the game last posted a summon search (/summon_messenger/get, every
    // PartyController::SummonSearchInterval while it searches), if ever.
    std::optional<std::chrono::steady_clock::time_point> last_search_post;
};

// CSMultiPlayMan session state (+0x124) as seen in the rig, and the summon-reload marker the
// game sets (+0x84 of the session rules) while it carries a guest into the host's world.
constexpr s32 MatchingStateHosting = 3;
constexpr s32 MatchingStateSummonedGuest = 6;
constexpr s32 SummonReloadInProgress = 3;

// Whether a world teardown must end the session even though the seamless patch keeps it: a
// summoned guest or a host leaving its world (quit to title), but never during a
// summon reload (see the tests for why). `reload_pending` is our own deferred summon reload.
bool ShouldEndSessionOnWorldTeardown(s32 matching_state, s32 reload_state, bool reload_pending);

// Deaths that keep the party. 01.09 runs the event scripts' death flow natively (the Lua source,
// global_event.lua in script/eventcommon.luabnd.dcx, is never loaded), so the change is a few
// byte patches in PartyGhostDeath / PartyGhostDeath_1 plus a revive where the menu used to be.
constexpr u64 PartyGhostSetEventFlagOffset = 0x0132E5A0;       // SetEventFlag(_, flag, on)
constexpr u64 PartyGhostCustomLuaCallStartOffset = 0x0132ED40; // CustomLuaCallStart(proxy, id, arg)
constexpr u64 PartyGhostDeathMenuBranchOffset = 0x01382359;    // jne past the menu (ClearBoss)

struct CodePatch {
    u64 offset;
    std::vector<u8> original;
    std::vector<u8> patched;
};

// The co-op site table (bloodborne_patch_sites.h) at work.
// "0F 87 ??" -> {0x0F, 0x87, 0x00}.
std::vector<u8> HexBytes(std::string_view hex);
// Same length, and equal but where the pattern says "??".
bool MatchesHexPattern(std::span<const u8> bytes, std::string_view pattern);
std::vector<std::string_view> AcceptedVariants(const CoopSite& site);
// The site's original bytes, or a variant it accepts.
bool MatchesCoopSite(std::span<const u8> bytes, const CoopSite& site);
const CoopFeature* FindCoopFeature(std::string_view name);
// The byte patches of a feature, in table order.
std::vector<CodePatch> CoopPatchesOf(std::string_view feature);
// The first site of `feature` that `image` (the eboot image, from offset 0) does not hold as the
// table has it; null when every site matches.
const CoopSite* FirstMismatchedCoopSite(std::span<const u8> image, std::string_view feature);
// Whether a hook over `bytes` at `offset` is in the table.
bool IsRegisteredCoopHook(u64 offset, std::span<const u8> bytes);
// The table as bbport/patches/coop-sites.json holds it.
std::string CoopSitesJson();

// What the co-op install did with each feature, for one summary line in the log
// (tools/re/check_party_log.py fails a log that skipped a required one).
// Time spent in co-op work on the game's threads (Ada's FPS drop, 2026-10-12): each section's
// calls, summed and the longest, reported once `period_ms` after the first one measured, then
// counted again from nothing. Thread-safe (the HUD's hooks run on another thread).
class CoopTimeMeter {
public:
    explicit CoopTimeMeter(s64 period_ms) : period_ms{period_ms} {}
    void Add(std::string_view section, s64 ns, s64 now_ms);
    // "<section> avg <ms> ms max <ms> ms (<n> calls), ... over <s> s", in the order the sections
    // came first; nothing before the period is over. With `slowest`, only that many sections, the
    // ones with the most time in all, most first.
    std::optional<std::string> Report(s64 now_ms,
                                      size_t slowest = std::numeric_limits<size_t>::max());

private:
    struct Section {
        std::string name;
        s64 total_ns = 0;
        s64 max_ns = 0;
        u64 calls = 0;
    };
    s64 period_ms;
    std::mutex mutex;
    std::optional<s64> since_ms;
    std::vector<Section> sections;
};

class CoopFeatureReport {
public:
    void On(std::string_view feature);
    void Skipped(std::string_view feature, std::string reason);
    // "Co-op features: N/M on (skipped: <feature> [<reason>], ...)", in install order.
    std::string Summary() const;
    std::vector<std::string> SkippedRequired() const;

private:
    struct Result {
        std::string feature;
        std::optional<std::string> skipped;
    };
    void Set(std::string_view feature, std::optional<std::string> skipped);
    std::vector<Result> results;
};

const std::vector<CodePatch>& GhostDeathKeepSessionPatches();

// Target of a `call rel32` at `offset`, or 0 if the bytes are not one.
u64 DecodeNearCallTarget(u64 offset, std::span<const u8> bytes);

// The event script bindings the revive uses (PS4 SysV; the unused first argument is the script
// state the native code passes along).
struct GhostDeathCalls {
    bool PS4_SYSV_ABI (*is_white_ghost)() = nullptr;
    bool PS4_SYSV_ABI (*set_event_flag)(u64 unused, s32 flag, bool on) = nullptr;
    void (*clear_return_state)() = nullptr; // ours: [SprjEventMan+0x10]+8, set by OnEvent_4000
    bool PS4_SYSV_ABI (*request_full_recover)() = nullptr;
    bool PS4_SYSV_ABI (*update_blood_mark)() = nullptr;
    bool PS4_SYSV_ABI (*revive_player)() = nullptr;
    void PS4_SYSV_ABI (*revive_local_character)() = nullptr;
    void (*restore_alive_state)() = nullptr; // ours: see AliveStateSnapshot
    void PS4_SYSV_ABI (*set_chr_type)(u64 unused, s32 entity, s32 type) = nullptr;
    void PS4_SYSV_ABI (*chr_reset_animation)(u64 unused, s32 entity) = nullptr;
    void (*warp_to_last_lamp)() = nullptr; // ours: GlobalState +0x1528, see the emulator
};
constexpr s32 DeathEventFlag = 4000;
constexpr s32 WhiteGhostCharacterType = 1;
constexpr s32 LocalPlayerEntity = 10000;

// What a character's death overwrites besides HP: the state flags at +0x1E0 (the kill sets the
// dead bit, +0x1E3 bit 0) and the action state the death processing writes ([+0x3B0]+8: +0x1B4
// and +0x1B8). Saved from the local player while it is alive, put back by the revive.
struct AliveState {
    u64 flags = 0;
    s32 action = 0;
    s32 action_sub = 0;
};
class AliveStateSnapshot {
public:
    void Observe(bool alive, const AliveState& state) {
        if (alive) {
            saved = state;
        }
    }
    const std::optional<AliveState>& Saved() const {
        return saved;
    }

private:
    std::optional<AliveState> saved;
};

// OnSelfBloodMark(4032, "SelfBloodMark", attributes) in the native g_Initialize: `mov ecx, 0x30`.
const std::vector<CodePatch>& GuestBloodstainPatches();
// No echo theft (Kaniel, 2026-10-10: everywhere, solo too): every 3 s the game lets an enemy within
// 10 m of the local bloodstain steal it while the player is 15 m or more away (the blue-eyed
// enemy, 0x018686D0); the stain is gone until that enemy is killed, so a party member back from
// its lamp never found it. The steal (EnemyIns vtable +0x3E0, 0x018E75A0) returns at once instead:
// its `je` past the return becomes two nops.
const std::vector<CodePatch>& NoEchoTheftPatches();

// HostDead's white phantom branch and the epilogue the seamless patch jumps to.
constexpr u64 HostDeadEpilogueOffset = 0x01381A50;
const std::vector<CodePatch>& HostDeathKeepGuestsPatches();
// A boss kill that keeps the party: BlockClear2_2 no longer sends the guest's leave broadcast
// (4063) or schedules BlockClear2_3Leave, and BlockClear2_3 clears a white phantom's leaving
// flags instead of warping it home. BlockClear2 no longer makes a white phantom wait for that
// load (SetLoadWait turned its action buttons off).
constexpr u64 OnKeyTime2Offset = 0x0132B1C0; // OnKeyTime2(proxy, event, state, delay, ...)
const std::vector<CodePatch>& BossClearKeepSessionPatches();
// Co-op areas (0x0191A750): a block's co-op is off while its flag in a table per area/block/part
// is set (Cathedral Ward: 2405/2406, set at load until the player's own world opened the gate,
// 12400160). A party joins anywhere: the flag check (0x01399BF0) reads as off.
constexpr u64 EventFlagIsSetOffset = 0x01399BF0;
const std::vector<CodePatch>& JoinAnywherePatches();
// A world load that is not a trip (0x0193C5xx: a cutscene that changes the time of day and
// warps the player, e.g. Gascoigne's 24000020) calls StopRequest (returning to 0x0193C65C) and
// LeaveSession for a member of a session. In a party it is a trip like a lamp's.
bool IsWorldReloadStopRequest(u64 caller_offset);
// The same load's LeaveSession (0x0193C689, returning to 0x0193C68E): a guest's game calls only
// this one at the Cathedral Ward border (rig 2026-10-13).
bool IsWorldReloadLeaveSession(u64 caller_offset);
// A lamp object's availability bytes (+0x49 blocked in multiplayer, +0x4A its entity missing)
// that a party member's lamp has cleared. After the host's time-of-day reload (Cathedral Ward
// door, 2026-10-10) a guest's lamp read "entity missing" until it travelled, so it stayed unusable.
struct LampUnblock {
    bool blocked = false;
    bool missing = false;
    bool operator==(const LampUnblock&) const = default;
};
LampUnblock LampBytesToClear(bool blocked, bool missing, bool host_effect, bool in_party_session);
// A summon the server handed to this game (its claim) that has not become a join. Rig 2026-10-10:
// the guest got the summon, its signaling to the host came up, and it never joined the host's
// room (1 of 25 joins). Says once per summon when to log the game's state, so the next stall
// shows what held it.
class SummonJoinWatch {
public:
    using Clock = std::chrono::steady_clock;
    static constexpr auto Patience = std::chrono::seconds{8};
    // The server repeats a claim on every sign refresh (about 1 s); a gap of this means its next.
    static constexpr auto NewSummonGap = std::chrono::seconds{5};
    void Delivered(Clock::time_point now);
    // `joining`: a join task or a session. True once, Patience after the summon arrived.
    bool Stalled(bool joining, Clock::time_point now);

private:
    std::optional<Clock::time_point> since_;
    Clock::time_point last_{};
    bool reported_ = false;
};
// Whether that StopRequest starts a party trip: from the world load, in a party session, with no
// trip under way.
bool StartsTripOnWorldReload(u64 caller_offset, bool trip_active, bool in_party_session);

// Target of a `jmp rel32` at `offset`, or 0 if the bytes are not one.
u64 DecodeNearJumpTarget(u64 offset, std::span<const u8> bytes);

// Revives the local player in place if it is a white phantom (a party guest). Returns whether it
// did; anyone else (and a missing binding) is left to the game.
bool ReviveDeadPartyGuest(const GhostDeathCalls& calls);

// The live player's version: SoloPlayDeath_2 replaced while in a session (no world reload): the
// host, and a guest that travelled (it is a live player afterwards).
bool ShouldReviveHostInPlace(s32 matching_state);
void ReviveDeadPartyHost(const GhostDeathCalls& calls);

// How the game puts the player at its lamp when a map loads (0x0154B2F2..0x0154B3BD): the
// teleport (vtable +0x5F8, mode 1), then the position snap (0x018C16A0: the position and the one
// before it, so nothing eases in from the old spot), the heading (transform +0x1D0) and the
// follow reset (0x018BCF30). The CamReset script binding (0x0132F5A0) is those last calls too.
// Our moves made only the teleport: the camera stayed where the player had been and flew to it
// (a guest's trip arrives at the map's origin and is moved to the lamp a second later).
// Those calls alone still left it flying in (Kaniel watching the rig): the camera's drawn
// position follows its ideal one with a spring (see bbport's runtime_camhook.c) unless the
// camera manager's snap byte is set, which the game sets as a map starts (0x01939C0E) and the
// next camera update hands to each camera (0x01436964); last, the move sets it too.
struct PlacementCalls {
    void PS4_SYSV_ABI (*teleport)(u64 character, const float* position, const float* rotation,
                                  s32 mode) = nullptr;
    void PS4_SYSV_ABI (*snap_position)(u64 character, const float* position) = nullptr;
    void (*set_rotation)(u64 character, const float* rotation) = nullptr; // ours: +0x1D0
    void PS4_SYSV_ABI (*reset_follow)(u64 character, bool placed) = nullptr;
    void (*snap_camera)() = nullptr; // ours: see CameraManagerPointerOffset
};
constexpr u64 SnapCharacterPositionOffset = 0x018C16A0;
constexpr u64 ResetCharacterFollowOffset = 0x018BCF30;
// The camera manager (built by 0x01435FC0, 0x1150 bytes) is at [[0x0553E860] +0x2830]; its byte
// +0x80 snaps every camera to where it should be on the next update.
constexpr u64 CameraManagerPointerOffset = 0x0553E860;
constexpr u64 CameraManagerField = 0x2830;
constexpr u64 CameraSnapByte = 0x80;
// Kaniel 2026-10-24 (team pings): the camera the frame is drawn with, in the camera manager from
// +0x10 (bloodborne_pings.h's PingCamera: its world matrix, field of view, width / height, near
// and far; rebuilt every frame at 0x01436C31 from the active camera, as PR #320 found).
constexpr u64 PingCameraAt = 0x10;
// Returns whether it moved the character (every call is needed).
bool PlaceCharacter(const PlacementCalls& calls, u64 character, const float* position,
                    const float* rotation);

// The sign poster's timers (seconds, a float at +0x28 of the sign object): 01.09 waits 5 s
// before a new sign's first post and 30 s between re-posts, ignoring ss.info's interval.
constexpr float FirstSignPostDelaySeconds = 0.0f;
constexpr float SignRepostSeconds = 1.0f;

// `mov dword ptr [<reg> + 0x28], <seconds>` with the ModRM byte of the game's own store.
std::array<u8, 7> EncodeSignTimerStore(u8 modrm, float seconds);
// The game's sign timer stores.
struct SignTimerSite {
    u64 offset;
    u8 modrm;
    float original_seconds;
    float patched_seconds;
};
constexpr std::array<SignTimerSite, 4> SignTimerSites{{
    {0x018A9264, 0x47, 5.0f, FirstSignPostDelaySeconds},
    {0x018A9324, 0x47, 5.0f, FirstSignPostDelaySeconds},
    {0x018A9757, 0x43, 30.0f, SignRepostSeconds},
    {0x018A988F, 0x43, 30.0f, SignRepostSeconds},
}};
// The game's store at a site, whatever time it holds: game patches rewrite the re-post time (the
// bundled "Uncap FPS++" writes 60 s), and the store is ours to rewrite all the same.
bool IsSignTimerStore(std::span<const u8> bytes, const SignTimerSite& site);

// Lamps as campfires (a party session only; solo and co-op with strangers stay vanilla). A lamp
// is used through its talk script (t<map><nnn>.esd), which 01.09 hides while the game is in
// multiplayer (ESD function 52) and which asks "travel to the Hunter's Dream?" by setting
// 70000100 + map * 10000 + lamp for common event 7200 to warp. In a party session that flag
// means "rest here" instead, and the world refreshes once the whole party rests at one lamp.
constexpr s32 NotResting = 0;
bool IsLampTravelFlag(s32 flag);
// Party rests are work in progress and off by default: lamps travel as usual. The value of
// SHADPS4_BLOODBORNE_LAMP_REST; "1" turns them on.
bool LampRestsEnabled(const char* setting);
bool IsPartySession(bool party_configured, s32 matching_state);
// Whether the talk scripts should see this player as the host (ESD function 14 = 0).
bool TalksAsHost(bool in_party_session, s32 character_type);
// The Hunter's Dream lamp the lamps' trip warps to (common event 7200: WarpPlayerToRespawnPoint).
constexpr s32 HuntersDreamWarpPoint = 2102962;
// The packed map WarpNextStage_Bonfire (0x0132E050) derives from a warp point: area AA and block B
// of AABxxxx.
u32 PackedMapOfWarpPoint(s32 warp_point);
// The Dream's headstones: "travel to <lamp>" sets 7210MMML for event 12107000.
bool IsHeadstoneTravelFlag(s32 flag);
s32 HeadstoneWarpPoint(s32 flag);
// A party guest's lamp trip, which the game's event would end for a client.
bool GuestMakesItsOwnLampTrip(bool in_party_session, PartyRole role);
// Whether an environment setting is "1".
bool SettingIsOn(const char* setting);
// The guest-as-host experiment: turn a white phantom in a party session into a live player.
bool ShouldPlayGuestAsHost(bool switched_on, bool in_party_session, s32 character_type);

using LampPosition = std::array<float, 3>;

// A warp point's respawn point from the map files (bloodborne_lamp_points.h), or none.
struct LampPoint;
const LampPoint* FindLampPoint(s32 warp_point);
// A party guest's trip lands where the game's lamp lookup puts a client: the map's origin, or the
// map's default spot. Planned at a known lamp (the trip's summon placement), the area around the
// lamp loads behind the loading screen; otherwise, and when it still lands elsewhere, the guest
// is moved to the lamp after arrival (MoveToRespawnPoint), and the textures there load late.
bool ArrivalNeedsMove(const LampPoint* planned, const std::optional<LampPosition>& at);
constexpr float ArrivedAtLampMeters = 3.0f;

struct LampRest;

enum class LampUse { Rest, Travel };

// Where this player rests: from using a lamp until walking away from it (or the session ends).
class LampRestState {
public:
    static constexpr float LeaveDistance = 4.0f;
    static constexpr float JoinDistance = 2.5f;

    void Rest(s32 lamp, const LampPosition& at);
    // The lamp's dialog was confirmed: a rest, or the trip if this player already rests there.
    LampUse Use(s32 lamp, const LampPosition& at);
    // Joins a party member's rest when standing near where it rests; true if it did.
    bool JoinNearby(std::span<const LampRest> board, const std::optional<LampPosition>& player);
    std::optional<s32> Tick(const std::optional<LampPosition>& player, bool in_party_session);
    std::optional<LampPosition> RestedAt() const {
        return lamp ? std::optional{rested_at} : std::nullopt;
    }
    // Where others can join this rest: only a rest started at the lamp itself.
    std::optional<LampPosition> JoinablePosition() const;

private:
    std::optional<s32> lamp;
    LampPosition rested_at{};
    bool joined = false;
};

// The party's rest board on shadNet (a message on the server link, once POST
// /bloodborne/party/rest): each member posts
// "<member> <lamp>" and gets back one "<member> <lamp>" line per member resting right now.
struct LampRest {
    std::string member;
    s32 lamp = NotResting;
    std::optional<LampPosition> at; // where the member rests: "<member> <lamp> <x> <y> <z>"
};
std::string EncodeLampRestUpdate(std::string_view member, s32 lamp,
                                 const std::optional<LampPosition>& at = std::nullopt);
std::optional<std::vector<LampRest>> ParseLampRestBoard(std::string_view body);

// Fires once each time every member of the session comes to rest at the lamp this player rests
// at. Each member decides for itself; they all see the same board, so they refresh together.
class LampRestCoordinator {
public:
    bool Tick(std::optional<s32> resting_lamp, s32 session_members,
              std::span<const LampRest> board);

private:
    bool everyone_was_resting = false;
};

// What a lamp reload would do, in place: heal, refill vials and bullets from storage the way the
// respawn load does (0x01939470 calls 0x014DBB70 then 0x014DACE0), and respawn the world's
// enemies (WorldChrMan +0xA6C, see the emulator). The session is untouched.
struct LampRestCalls {
    void (*request_full_recover)() = nullptr;
    void (*clamp_restock_item)() = nullptr;
    void (*restock_from_storage)() = nullptr;
    void (*request_world_respawn)() = nullptr;
};
bool RefreshAtLamp(const LampRestCalls& calls);

// The bonfire respawn re-enables every character of the loaded blocks; the event scripts that
// hide some again do not all run in a session. A character to switch off again afterwards.
bool KeepDisabledAfterRespawn(bool disabled_before, s32 hp_before);
// A party rest respawns only what was killed during this visit to the map: living characters,
// and characters the scripts keep dead from the start, are left out of the bonfire respawn.
bool RespawnsAtPartyRest(s32 hp, bool alive_this_visit);
// A character as the bonfire respawn's walk finds it (see RespawnsInWorldRespawn).
struct RespawnCandidate {
    s32 hp = 0;
    s32 max_hp = 0;
    u8 death_flags = 0;    // +0x1E3: see IsRealDeath (bits 0, 2) and IsDeadInItsWorld (0x26)
    bool disabled = false; // [+0x18]+0x20 bit 0 (see KeepDisabledAfterRespawn)
    bool hidden = false;   // +0x28, hidden by the story (see CharacterShown)
    // Hidden or switched off by the party, not its own story: as the host has it
    // (ShowCharactersLikeTheHost, SwitchCharactersLikeTheHost).
    bool changed_by_party = false;
    // Dead here while the host has it alive again (see BroughtBackByTheHost).
    bool brought_back = false;
};
// Dead in this member's own save from before the session: at 0 HP with no death mark at all
// (bits 0-2 and 5 of +0x1E3 clear), not switched off or hidden by the story. A game that loads
// its save (or a phantom's trip back home, which restores its world as it was) brings such an
// enemy in at 0 HP, neither dead nor alive; only a death or rest reload brings it back.
// Bosses beaten before the session and story deaths are put down by their world (0x26).
bool LeftDeadByItsSave(const RespawnCandidate& character);
// Which bonfire respawn (WorldChrMan +0xA6C) runs: a party rest, or the one after a party wipe's
// trip, which brings back what the member's save left dead (a party wipe resets the map for
// every member, as a solo death does; Kaniel). The wipe's own reload brought this visit's kills
// back already.
// Or, mid-play, the one that brings back on a guest what the host's map generator brought back
// (BroughtBackByTheHost).
enum class WorldRespawnKind : u8 { PartyRest, AfterWipe, BroughtBack };
bool RespawnsInWorldRespawn(WorldRespawnKind kind, const RespawnCandidate& character,
                            bool alive_this_visit);
// A map generator brings the same pre-placed enemies back (MSB events of type 5): the chalice
// bell-ringing woman's red skeletons, the Witch of Hemwick's Mad Ones. It runs in the host's game;
// a guest near one when it died there took the game's kill (DiesInsteadOfHiding), and its copy
// stayed dead after the host's came back, so the guests never saw those again (Kaniel's chalice
// playtest 2026-10-25, v1.2.2). Of this guest's copies dead here (`dead_here`: since when), the
// ones the host's board has alive near the host, no member's kill standing (`party_kills`), dead
// here for BringBackAfterMs at least (a kill made here reaches the host first). Sorted.
struct DeadCopy {
    u32 handle = 0;
    s64 dead_since_ms = 0;
};
constexpr s64 BringBackAfterMs = 3'000;
std::vector<u32> BroughtBackByTheHost(std::span<const CharacterShown> host,
                                      std::span<const DeadCopy> dead_here,
                                      const std::unordered_set<u32>& party_kills, s64 now_ms);
// The same for the character state flags (+0x1E0) the scripts set and the respawn clears.
constexpr u64 ScriptCharacterFlags = 0x4000 | 0x20000 | 0x40000000;
u64 RestoreScriptFlagsAfterRespawn(u64 flags_before, u64 flags_after, s32 hp_before);

// A party member's trip to another stage keeps the session: 01.09 would end it twice, once when
// the old world is torn down and once when the new stage has loaded (stage uid change). Active
// from the stage transition until that second point, or until it has clearly gone wrong.
class PartyTrip {
public:
    using Clock = std::chrono::steady_clock;
    static constexpr Clock::duration Timeout = std::chrono::seconds{90};
    // How long a world reload's trip waits for its load to show (see StartWorldReload).
    static constexpr Clock::duration NoLoadGrace = std::chrono::seconds{5};

    void Start(Clock::time_point now, u32 from_stage, u64 from_player) {
        started = now;
        from = from_stage;
        player = from_player;
        needs_load = false;
        loaded = false;
        out_of_session = false;
    }
    // A trip begun outside a session (a lamp's warp before the join): no party trip, the leave
    // its load does is the game's own, until a session forms while it is under way (JoinSession).
    void StartOutOfSession(Clock::time_point now, u32 from_stage, u64 from_player) {
        Start(now, from_stage, from_player);
        out_of_session = true;
    }
    // In a session now: a trip begun outside one and not arrived yet is the party's (the join
    // landed during its load, rig 2026-10-21: its world teardown ended the session). True once.
    bool JoinSession(Clock::time_point now) {
        if (!started || !out_of_session || now - *started >= Timeout) {
            return false;
        }
        out_of_session = false;
        return true;
    }
    // The world load's own StopRequest (0x0193C65C, a time-of-day reload): a trip only once a load
    // shows (loading, no player, or a new one). At a border it came with no load, the map read
    // flipping between the two (Kaniel 2026-10-13): no arrival then, and DropIfNoLoad ends it.
    void StartWorldReload(Clock::time_point now, u32 from_stage, u64 from_player) {
        Start(now, from_stage, from_player);
        needs_load = true;
    }
    void Finish() {
        started.reset();
    }
    // The current stage (packed map), whether the game is loading and the local player's
    // character, from the party tick. Over on a new stage, or after a reload of the same one (a
    // new character).
    void ObserveStage(u32 stage, bool loading, u64 current_player) {
        if (!started) {
            return;
        }
        if (loading || current_player == 0 || current_player != player) {
            loaded = true;
        }
        if (needs_load && !loaded) {
            return;
        }
        if (!loading && current_player != 0 && (stage != from || current_player != player)) {
            Finish();
        }
    }
    // Whether the last trip started was a world reload's.
    bool WorldReload() const {
        return needs_load;
    }
    // A world reload's trip with no load NoLoadGrace after it began: ended, no arrival. True once.
    bool DropIfNoLoad(Clock::time_point now) {
        if (!started || !needs_load || loaded || now - *started < NoLoadGrace) {
            return false;
        }
        Finish();
        return true;
    }
    bool Active(Clock::time_point now) const {
        return started && !out_of_session && now - *started < Timeout;
    }

private:
    std::optional<Clock::time_point> started;
    u32 from = 0;
    u64 player = 0;
    bool needs_load = false;
    bool loaded = false;
    bool out_of_session = false;
};

// Join tasks (CSMultiPlayerIns, one per session player, in CSMultiPlayMan's list): the kind at
// +0xE8 (0 the remote host, 1 a remote guest; 2 and 3 are the local player as guest or host)
// and the step at +0x50. Step 4 creates the player's character from its synced data (once +0xED
// bit 0 says the data is there), step 5 plays its arrival, step 6 is the steady in-session step.
constexpr s32 RemoteHostTask = 0;
constexpr s32 RemoteGuestTask = 1;
constexpr s32 CreateCharacterStep = 4;
// A remote player's join state from before a trip: kind, the member id's identity pointer, slot
// (+0xEC), flags (+0xED, bit 0 = the member's data is synced) and step (+0x50).
struct SavedJoinTask {
    s32 kind = 0;
    u64 identity = 0;
    u8 slot = 0;
    u8 flags = 0;
    s32 step = 6; // InSessionStep
    bool operator==(const SavedJoinTask&) const = default;
};
// Whether a member's task is rebuilt after a trip from its save, straight into the character
// step (its data was there before the trip), or runs the game's own join from the start (it was
// still joining: its data never came, and a character made without it is a body-less phantom).
bool RebuildsFromSave(const SavedJoinTask& saved);
// The saved task a rebuilt one of `kind` takes: its member's own, else the only one of that kind.
std::optional<size_t> PickSavedJoinTask(std::span<const SavedJoinTask> saved, s32 kind,
                                        u64 identity);
// What a trip's start keeps: the tasks found now, and the saved ones of members not on this map
// (their slots are still theirs when they are met again).
std::vector<SavedJoinTask> MergeSavedJoinTasks(std::span<const SavedJoinTask> before,
                                               std::span<const SavedJoinTask> found);

// A member's character data (2026-10-10 RE). Each game sends its own to the others in parts, on
// the session's packet channels, through the net sync object at 0x0553D6D0 (send 0x017894E0:
// object, member id, channel, buffer, size; receive 0x01789200, polled every frame at 0x01783160):
// channel 8 (0x168 bytes: attributes PlayerGameData +0x40..+0x90, face +0x4AC, name), 0x0B, 0x0C
// (0x80 bytes: the gear) and 0x0D, plus 0x13, 0x3B and 0x3C. The receiver copies them into its
// remote player slot (GameDataMan [0x0553B130] +0x10, 4 slots of 0x6A0; the member's slot is the
// net sync list +0x68, node id +0x10, slot +0x30) and sets a bit per part in the slot's byte at
// GameDataMan [+0x18][slot]: 0x02 channel 8, 0x04 channel 0x0C, 0x08 channel 0x0D, 0x10 channel
// 0x0B (0x01 is the slot's own). A join task waits at step 3 for the gear (task +0xED bit 3, set
// by channel 0x0C), and at step 4 for +0xED bit 0, which the world update (0x01947310) sets once
// the member leaves the net sync's pending list (+0x48, 0x0178B4D0) with all five bits (0x1F).
// A game sends all of its parts after each load (0x0193A979 calls 0x01781AC0), its gear again on
// every change (0x018FA610) and channels 0x0D/0x13 from the player update (0x018F4FF0); stats and
// face only after a load. A one-byte packet on channel 0x0A asks a member for everything: its game
// answers with all parts (0x01783EDA), and entering a session asks every pending member
// (0x0178D9C0).
constexpr u64 NetSyncSendOffset = 0x017894E0;
constexpr u64 NetSyncReceiveOffset = 0x01789200;
constexpr u64 BroadcastMemberDataOffset = 0x01781AC0;
constexpr u32 MemberDataRequestChannel = 0x0A;
constexpr u8 MemberDataParts = 0x1E;
bool HasAllMemberData(u8 parts);
// A member rebuilt after a trip while its data had not come (see RebuildsFromSave) waits for it:
// once the member has stood on this map a moment, its data is asked for (channel 0x0A) and asked
// again every few seconds until all its parts are in, then its character is created. The parts
// seen before a request are stale (from before the trip, or none): each request clears them.
class MemberDataWait {
public:
    enum class Action { None, Request, Create };
    static constexpr s64 SettleMs = 1000;
    static constexpr s64 RepeatMs = 5000;
    Action Tick(s64 now_ms, bool member_here, u8 parts);
    s32 Requests() const {
        return requests;
    }

private:
    std::optional<s64> here_since;
    std::optional<s64> requested_at;
    s32 requests = 0;
};
// The player's own stats and face (what channel 8 carries) change mid-session on a level up, a
// stat reset or the mirror, and the game sends them only after a load, so the others keep the old
// ones. A change that has held for a moment is sent to everyone again (BroadcastMemberDataOffset).
// The first value seen in a session is the one the game sent itself.
class OwnDataWatch {
public:
    static constexpr s64 SettleMs = 2000;
    // `fingerprint` of the player's data, or nullopt while it cannot be sent (no session, loading).
    bool Observe(std::optional<u64> fingerprint, s64 now_ms);

private:
    std::optional<u64> sent;
    std::optional<u64> pending;
    s64 pending_since = 0;
};
constexpr s32 InSessionStep = 6;
bool ShouldRecreateAfterTrip(s32 kind, s32 step);
// Kaniel's playtest 2026-10-16: krolebord joined in place and never saw Lisara: her task in his
// game stayed at step 1 (waiting for her character data) and nothing asked for it. A remote
// player's task still before CreateCharacterStep is one to ask the data for.
bool WaitsForMemberData(s32 kind, s32 step);
s32 RemoteTaskKind(bool hosting);
// SessionManager +0x118: set by the game's kick-all (0x0178D7F8, the ForceSummonSuccess handler);
// while it is set the host kicks every joiner on arrival (0x01ED121E, reason 0xff000019) until the
// room is made again. A party host's session is reopened instead.
constexpr u64 SessionClosingOffset = 0x118;
bool ReopensClosedSession(bool party, s32 matching_state, u8 closing);
// The kind of a join task rebuilt for the member with `identity` (after a trip, or a guest's
// re-create when another guest joins): on the host every other member is a remote guest; on a
// guest the kind the live tasks (`live`, the join list now) or else the saved ones give it, else
// a guest when they name another member as the host, else the host. A live host task wins: a
// guest whose game made no task for a new guest built it as a second host (kind 0), and step 4
// never creates a kind 0 character on a guest (its own kind 2 task creates the host there), so
// the new guest stayed invisible to it (rig smoke 2026-10-12).
s32 RebuiltTaskKind(bool hosting, std::span<const SavedJoinTask> saved,
                    std::span<const SavedJoinTask> live, u64 identity);
// The +0xED flags of a rebuilt task: the saved ones (with the synced bit unless the member is
// still `waiting` for its data), else what the constructor left. Bits 6-7 are not the task's: the
// constructor (0x01E4F9F0, `and 0xc0`) keeps them from the allocation and no join code uses them.
constexpr u8 JoinTaskFlagBits = 0x3F;
u8 RebuiltTaskFlags(u8 constructed, const SavedJoinTask* saved, bool waiting);
s32 LocalTaskKind(bool hosting);
bool ShouldRestoreSearchAfterTrip(bool in_party_session, bool search_effect_active);
// The bell effects (9000/9003, 9005/9006) in `before` that are missing from `after`, in order.
std::vector<s32> EffectsToRestoreAfterTrip(std::span<const s32> before, std::span<const s32> after);

// The "Impurity" rune (the League oath, GemeffectParam 2127001) puts 6142 on its wearer. Common
// event 9035 (started by event 0 with 6142, 180040) adds 180040, max HP x1.02, while the wearer is
// a white phantom (4:3 10000 type 1) and takes it off once it is not. A party guest is one only
// until its first trip (then a live player, type 0), so the bonus went with its first trip (player
// report 2026-10-22), and a host (the one cooperated with) never had it. In a party everyone in
// the session cooperates (Kaniel 2026-10-22): the bonus follows the rune for the host too.
constexpr s32 ImpurityRuneEffect = 6142;
constexpr s32 ImpurityHpEffect = 180040;
constexpr bool ImpurityBonusForHost = true;
// Whether this player gets the bonus while it wears the rune.
bool GetsImpurityBonus(bool in_party_session, bool party_guest);
enum class EffectChange { None, Add, Remove };
// What a player's Impurity bonus needs: on while it `gets_bonus` (GetsImpurityBonus) and wears
// the rune; off once not, if `added` (this code put it on; the game's own stays the game's).
EffectChange ImpurityBonusChange(bool gets_bonus, std::span<const s32> effects, bool added);

// The party's map board on shadNet (a message on the server link, once POST /bloodborne/party/map):
// each member posts
// "<member> <map>" (the packed map in hex) and reads back one such line per member.
struct MemberMap {
    std::string member;
    u32 map = 0;
};
std::string EncodeMapUpdate(std::string_view member, u32 map);
std::optional<std::vector<MemberMap>> ParseMapBoard(std::string_view body);
// Whether there is someone else in the party and all of them are on `map`.
bool EveryoneElseOnMap(std::span<const MemberMap> board, std::string_view self, u32 map);
// Whether no other member is on `map` (a usable map: 0 while loading counts as not alone).
bool NoOneElseOnMap(std::span<const MemberMap> board, std::string_view self, u32 map);

// A party of three: the boards know a member by its online id, hex-encoded (board member names
// are letters and digits only), and a session member's identity object holds its online id.
std::string BoardMemberOfOnlineId(std::string_view online_id);
std::string OnlineIdOfBoardMember(std::string_view member); // "" if it is not one
// The one candidate found in `bytes` as a whole NUL-terminated name, or nothing.
std::optional<std::string> FindOnlineIdIn(std::span<const u8> bytes,
                                          std::span<const std::string> candidates);
// After this member's trip: which session members (their board names, nullopt if unknown) to
// re-create now, the ones on this map not handled yet, and whether every one is then handled.
// With a member that cannot be told apart, all of them once everyone else is here.
struct RecreatePlan {
    std::vector<size_t> now;
    bool done = false;
};
// Two session members found under one name are not told apart: one of them is not known.
RecreatePlan PlanRecreate(std::span<const std::optional<std::string>> members,
                          std::span<const MemberMap> board, std::string_view self, u32 map,
                          const std::set<std::string>& handled);
// Whether the host (its map from its map state post) is on `map` and still on the map board.
bool HostIsHere(std::optional<u32> host_map, std::span<const MemberMap> board,
                std::string_view self, u32 map);
// A guest runs its map's enemies while this holds: the host is not on its map (two guests there
// both do). Without the host's post yet, no other member on its map.
bool HostAway(std::optional<u32> host_map, std::span<const MemberMap> board, std::string_view self,
              u32 map);
// Another member is on another map (kill echoes go to the board then).
bool SomeoneElsewhere(std::span<const MemberMap> board, std::string_view self, u32 map);
// Whether this member posts the kill echoes it gets on `map` to the echo board: someone is
// elsewhere, and no other member on `map` comes before it by board name. The game gives a kill's
// echoes to every member on the killer's map, so each of them would post the same kill (rig
// smoke 2026-10-12: the second guest in the Dream got the host's +995 twice).
bool PostsKillEchoes(std::span<const MemberMap> board, std::string_view self, u32 map);
// The host rings again: hosting, fewer than three in the session, a party member online outside.
bool RoomForMore(bool hosting, size_t in_session, size_t party_online);

// Shared trophies on shadNet (a message on the server link, once POST /bloodborne/party/trophy):
// "<member> [<id>...]" with the trophies this member unlocked in a party session; the answer is
// every trophy the party unlocked, one id per line. A member unlocks the ones it has not handled
// yet.
std::string EncodeTrophyUpdate(std::string_view member, std::span<const s32> trophies);
std::optional<std::vector<s32>> ParseTrophyBoard(std::string_view body);
std::vector<s32> TrophiesToUnlock(std::span<const s32> board, std::span<const s32> handled);

// A guest's progress in the host's world never reaches its own save. Picked-up map items
// (ItemLotParam getItemFlagId 5[0-3]MMxxxx, chalice dungeons 529xxxxx left out) set in a party
// session are kept in a file, one flag per line, and set again in the guest's own world.
bool IsMapPickupFlag(s32 flag);
// The kept progress is per character: the save slot of /savedata0/userdata000N (N 0-9).
std::optional<int> CharacterSlotOfSavePath(std::string_view path);
// Pickups once confirmed set in the character's own world never clear again, so a load of a
// slot where none of them is set holds a new character, and the slot's kept lists are dropped.
bool IsNewCharacterInSlot(std::span<const s32> confirmed, size_t confirmed_set_now);
// A save slot's file of its character's (user/bloodborne_party_<list>_slot<N>.txt).
std::string SlotFileName(std::string_view list, int slot);
// The slot's files of its character, dropped (copied aside first) when another character is found
// in the slot (ForgetsSlotLists): its pickups, world changes, confirmed pickups and where it
// resumes (Ada 2026-10-12: a new character resumed at the old one's spot).
// Kaniel 2026-10-16: and the team world it last played ("team_world": its id in its team).
inline constexpr std::array<std::string_view, 5> CharacterSlotFiles{
    "progress", "world", "confirmed", "resume", "team_world"};
// Whether the character loaded now is another one than the one a slot's lists were kept for
// (`progress_kept`, its own world's set event flags then; 0 not known): far fewer set now.
bool IsOtherCharacter(s32 progress_now, s32 progress_kept);
// The set event flags of a load from two reads of the store a moment apart (each empty when it
// missed part of the store): known only when both read the whole store and agree.
std::optional<s32> SettledFlagCount(std::optional<s32> first, std::optional<s32> second);
// Whether a load drops a slot's kept lists (`new_in_slot`: its confirmed pickups all missing):
// never on an unknown count, since a wrong drop loses what the player looted.
bool ForgetsSlotLists(std::optional<s32> progress_now, s32 progress_kept, bool new_in_slot);
// A slot's kept files are copied aside before they are dropped, as `file`.bak-`stamp`; the copies
// of `file` beyond the newest `keep` (stamps sort by time) are removed.
std::string SlotBackupName(std::string_view file, std::string_view stamp);
std::vector<std::string> SlotBackupsToRemove(std::string_view file,
                                             std::span<const std::string> names, size_t keep);
std::string FormatFlagList(std::span<const s32> flags);
std::vector<s32> ParseFlagList(std::string_view text);
// The event flag store (EventFlagMan [0x0553B100]): blocks of +0x1C flags each, kept in a map at
// +0x38 by block id; a flag's bit is at byte index/8, most significant bit first.
struct FlagChanges {
    std::vector<s32> set;
    std::vector<s32> cleared;
};
FlagChanges ChangedFlags(u32 block, u32 flags_per_block, std::span<const u8> before,
                         std::span<const u8> after);
// Ritual altars (see FlagTable, bloodborne_altar.h): a ritual or a join writes "the current
// altar", flags 92900000-92905999, a view of the altar k the player talks to (94k00000-94k05999).
bool IsCurrentAltarFlag(s32 flag);
// A Shared flag whose clears go to the party too (a ritual altar's, see FlagTable).
bool SharesClearsWithParty(s32 flag);
// A ritual altar's talk runs in every game of a party: its "warp to the layer" (72100420 + altar)
// and the layer (72100300 + layer) are not set in a game where the talker is another player's
// character (`talker_is_another_player`), else every game warped its player (Kaniel's playtest
// 2026-10-19). Any other talker, or none known, sets them as before.
bool SkipsOthersAltarWarp(s32 flag, bool talker_is_another_player);
// Team altars: the chalice of an altar's dungeon (its record's depth * 10 + area, the standard
// chalice of that depth and area), which a teammate holding none is given so that its own altar
// opens as the conductor's does (the altar's talk asks for any chalice). Nothing for an unknown
// one.
std::optional<u32> AltarChaliceGoods(u32 depth_area);
// Shared progression: the guest's copy of the host's world follows the host's flag changes, so
// the changes it sees in a session (lamps, shortcuts, bosses, quest steps) are kept, latest
// state per flag, in a file of "<flag> <0|1>" lines and set in the guest's own world once it is
// home. A join reload's bulk change, the death/multiplayer (4xxx) flags and
// the pickups (their own list) are left out. Which flags are kept, shared or each player's own:
// FlagTable (bloodborne_flags.h, docs/FLAGS.md); the flag predicates here read it.
bool CarriesToGuestWorld(s32 flag);
// The Hunter's Dream flags a guest out in the world (any map but the Dream) in a session sets for
// itself: 9401 (Central Yharnam reached: the Dream's first visit, its shop) and 9403 (the
// workshop's doors open, set by Central Yharnam's 12410900 for a live player only). A phantom in
// the host's world never set them, so its own Dream stayed shut (Kaniel 2026-10-11). 9401 only once
// this character has been to the Dream (`dream_visited`): before, it showed the clinic's lamp and
// ended the first death's wake in the Dream (Ada 2026-10-13). 9403 only once the session's world
// has 12410900 (`workshop_opened`: the party walked past where it opens the workshop), not on
// joining.
std::vector<s32> GuestProgressFlags(bool guest, bool in_session, u32 map, bool dream_visited,
                                    bool workshop_opened);
// The messengers' weapon lines: Dream event 12101000, started per slot 0-9, sets 12101000 + slot
// once the player has that slot's badge. A broken patch of that event (2026-10-12) set them all,
// so once per save slot, out of the Dream, they are cleared and the next Dream visit sets them
// again from the player's own badges.
std::span<const s32> ShopLineFlags();
// A game thread that has not run the party tick for GameStallMs is stuck (a load ticks on).
constexpr s64 GameStallMs = 5'000;
bool GameThreadStalled(s64 now_ms, s64 last_tick_ms);
bool RechecksShopLines(bool party_configured, int slot, bool done_for_slot, u32 map);
// A held-item flag (HeldItemFlags) in a party follows what this player holds: the value it must
// take when it differs (on while the player holds the item, off while not), else nothing. Kaniel
// 2026-10-18: a save that took the host's "holds the Sword Hunter Badge" from a team world never
// got its own badge from the Cleric Beast.
std::optional<bool> HeldItemFlagFix(bool party_configured, bool flag_on, bool holds);
// A gesture's flag (GestureFlags) in a party goes off while this player has not learned the
// gesture (a save that took the host's value from a team world never got it from the NPC's talk);
// nothing else (a learned gesture's flag stays off: another talk may give it by its own flag).
std::optional<bool> GestureFlagFix(bool party_configured, bool flag_on, bool learned);
// A Tonsil Stone giver's first-talk flag (GiftTalkFlags) in a party goes off while this player
// holds no Tonsil Stone (a save that took the host's value from a team world got the "talked
// before" line and never the stone); nothing else.
std::optional<bool> GiftTalkFlagFix(bool party_configured, bool flag_on, bool holds);
// Whether a guest's own NPC gift clears its item lot's flag (ItemLotParam getItemFlagId, its
// "taken") before the lot is given: while that flag is on as the host's world (Valtr's rune lot
// 43120 by 6676: the host's came over from the team world and the lot gave the guest nothing).
// A flag of the guest's own (a map pickup, a per-player flag) stays.
bool ClearsNpcGiftLotFlag(s32 lot_flag, bool flag_on);
// A slot of the player's gesture list (PlayerGameData +0x5B8, 30 u32 at +0x10), as talk command
// 66 reads it: bit 0 learned, bits 1-15 the gesture's row in the game's table (0x014EC980,
// gesture + 1); none for an empty slot.
struct GestureSlot {
    s32 gesture;
    bool learned;
    bool operator==(const GestureSlot&) const = default;
};
std::optional<GestureSlot> DecodeGestureSlot(u32 entry);
// Whether a weapon of OwnedWeaponFlags is in `held` (the inventory's weapon ids): any upgrade
// level, its base id + 100 * level up to +10, as the common events check it.
bool HoldsWeapon(u32 weapon, std::span<const u32> held);
// An owned-weapon flag (OwnedWeaponFlags) in a party: on once this player holds the weapon (a
// guest's events never set it: a phantom's do not run), and once per save slot (`recheck`) off
// while it does not, so a save that took the host's flags from a team world sells its own
// weapons only (Kaniel 2026-10-21). The value it must take, else nothing.
std::optional<bool> OwnedWeaponFlagFix(bool party_configured, bool flag_on, bool holds,
                                       bool recheck);
// Common events 5500-5521 (the Uncanny and Lost DLC weapons) end at once unless the local player
// is a host-type character (4:3 10000 type 0, then 1000:2), and an event that ends sets its own
// flag: a phantom guest's game set all 22 on every load in a session, so the shop sold them all
// (krolebord's log 2026-10-21, "set ... 5500 ... 5521" at the join's load). These follow the
// player's weapons every second, not only on the save slot's recheck.
bool RechecksOwnedWeaponFlagAlways(s32 flag);
// A Dream headstone's flag (DreamHeadstones) as this player's own lamps make it: on while one of
// them is lit (`on` reads a flag); nullopt for any other flag.
std::optional<bool> HeadstoneLampsLit(s32 flag, const std::function<bool(s32)>& on);
// The headstones' lamps (DreamHeadstones) this player has lit (`on` reads a flag).
std::vector<s32> LitHeadstoneLamps(const std::function<bool(s32)>& on);
// A party guest's lamps to light again: its own lit ones (`kept`) and the team's world's
// (`team`, any of its flags), each a lamp's active flag that is off now. Kaniel's play test
// 2026-10-21: a guest's join reload (it comes back as the host's phantom) gives its game a new
// flag store with no lamp lit, and its headstones said "no place to awaken".
std::vector<s32> LampsToRelight(const std::set<s32>& kept, std::span<const s32> team,
                                const std::function<bool(s32)>& on);
// Whether a load of the guest's world gets the kept world changes before its map items and
// events: at home, and on a party trip (its own world loads while the session stays up).
bool SetsGuestWorldChangesOnLoad(bool in_session, bool on_trip);
// True once per party trip: the first time it is asked while the trip is on.
class OncePerTrip {
public:
    // `trip`: the trip's number (party_trips_started): one take per trip, even with no load
    // outside a trip between two.
    bool Take(bool on_trip, u64 trip) {
        if (!on_trip || (taken && *taken == trip)) {
            return false;
        }
        taken = trip;
        return true;
    }

private:
    std::optional<u64> taken;
};
// Whether the host posts its characters' shown state (`world_changed_ms`: its world's last load):
// once the load's story has run (ShownSettleMs).
constexpr s64 ShownSettleMs = 8'000;
bool PostsShownState(s64 now_ms, s64 world_changed_ms);
// A lamp's "active" flag (10007810 + map * 10000 + lamp * 20, what the lamp talk scripts check)
// to its lamp entity (map * 10000 + 1950 + lamp); the game clears the flag again for a character
// whose lamp list ([0x0553B120]+0xC70) lacks the lamp.
std::optional<s32> LampOfActiveFlag(s32 flag);
// A lamp's "light it" request (70000000 + map * 10000 + 200 + lamp, maps 211-399, lamps 0-19):
// the lamp talk sets it, and common event 7100 lights the lamp once and ends; on any load with the
// request on it ends at once. Its lamp's character (map * 10000 + 950 + lamp, which common event
// 7000 keeps switched off until the lamp's boss is dead) and active flag (LampOfActiveFlag).
struct LampLight {
    s32 character = 0;
    s32 active_flag = 0;
    bool operator==(const LampLight&) const = default;
};
std::optional<LampLight> LampOfLightRequest(s32 flag);
// Kaniel's playtest 2026-10-20: krolebord lit the Witch's Abode lamp while she lived; his event
// 7100 ran, lit nothing and ended, and the lamp never lit after her kill. A light request is
// refused while its lamp's character is off in this game (`lamp_character_off`, none: no such
// character, left to the game).
bool RefusesLampLight(s32 flag, std::optional<bool> lamp_character_off);
// The light requests on (`on`) of the lamps of `packed_map` whose lamp is not lit for this player
// (its active flag off): the request lit nothing, or it came from another player's world. Cleared
// as the map loads, so its event 7100 lights the lamp again.
std::vector<s32> LightRequestsToClear(u32 packed_map, const std::function<bool(s32)>& on);
// Kaniel's playtest 2026-10-25: right after the team connected nobody could level up at the
// Doll. Her talk (m21_00 t210304) sets 6700 as her level-up menu opens and plays only her words
// while it is on; the team's world held the host's 6700 on and put it in every save. No talk is
// open while a map loads, so a 6700 on then is stuck: cleared as the map loads (in a party).
constexpr s32 DollLevelUpFlag = 6700;
bool ClearsDollLevelUpFlag(bool party_configured, bool flag_on);
// The other player's "set this event flag" (0x017C0570) and "event ended" (0x017C0901) messages
// leave these flags and events alone in a party: what one player received is not given to the
// other (the Hunter's Dream messengers' gift events and flags, 12101020-12101029, and the
// pick in their talk, 72101000-72101099, and each boss's "found" flag, see EncounterInsight).
bool KeepsLocalFlag(bool party_configured, s32 flag);
// The insight for finding a boss: the boss's entrance event gives `amount` (common event 9350)
// unless `once_flag` is on, then sets it; `kill_event` is the boss's defeat event (its flag goes
// on once it has run). From the map event files (the events that start 9350 behind a flag).
struct EncounterInsight {
    s32 kill_event = 0;
    s32 once_flag = 0;
    s32 amount = 0;
    bool operator==(const EncounterInsight&) const = default;
};
std::span<const EncounterInsight> EncounterInsights();
// A guest's insight for finding a boss it saw die in the session (Kaniel 2026-10-11: the host's
// game sent its "found" flag, so the guest's entrance event gave nothing). Tick gives the bosses
// whose defeat event went on while this guest is in a session and whose found flag is off.
class EncounterInsightWatch {
public:
    template <typename FlagOn>
    std::vector<EncounterInsight> Tick(bool guest, bool in_session, FlagOn flag_on) {
        std::vector<EncounterInsight> award;
        if (!in_session) {
            seen.clear();
            return award;
        }
        for (const EncounterInsight& boss : EncounterInsights()) {
            const bool dead = flag_on(boss.kill_event);
            const auto it = seen.find(boss.kill_event);
            if (guest && it != seen.end() && !it->second && dead && !flag_on(boss.once_flag)) {
                award.push_back(boss);
            }
            seen[boss.kill_event] = dead;
        }
        return award;
    }

private:
    std::map<s32, bool> seen; // defeat event -> its flag at the last tick of this session
};
// SHADPS4_BLOODBORNE_TRACE_FLAG: a flag the flag tracer watches in a session (diagnostics).
std::optional<s32> TracedFlagSetting(const char* setting);
struct FlagBit {
    u32 block = 0;
    u32 byte = 0;
    u8 mask = 0;
};
// Where a flag lives in the event flag store (blocks of flags_per_block flags, MSB first).
FlagBit FlagBitLocation(s32 flag, u32 flags_per_block);
// A lamp's first registration for a character (0x0146DA20) writes its flags base..base+11 off
// (+10/+11: lit). Those on before it (bit i of on_before: base + i, a lamp another party member
// lit) are set again in a party.
std::vector<s32> LampFlagsKeptAtRegistration(bool party_configured, s32 base, u16 on_before);
// Where a party death wakes the player (point: GlobalState +0x1528, which the events set on a
// death: the first one to the Hunter's Dream, a kidnapping to the Gaol): moved there while the
// point is in a loaded block, a trip there otherwise, except right after such a trip. A death
// that resets the map (see DeathResetsMap) always makes the trip, so the map reloads. So does a
// fall into the void: moved to the lamp in place, the player got up invisible to itself and to
// the others until its next trip (player report 2026-10-22, Forbidden Woods cliffs).
enum class DeathRespawn { StayPut, MoveToPoint, Travel };
DeathRespawn ChooseDeathRespawn(s32 point, bool in_loaded_block, bool arriving_from_trip,
                                bool reset, bool void_fall = false);
// Whether the trip of this death respawn checks, once arrived, for enemies the member's own save
// left dead (see LeftDeadByItsSave): the trip of a death that resets the map.
bool WipeTripRespawnsOnArrival(bool reset, DeathRespawn respawn);
// Whether a real death was a fall into the void: the void bit (see FallsIntoTheVoid) still set,
// or the fade-out the game starts for it (character +0x20C, 1.0 while shown) begun. Moved to the
// lamp in place, such a player stays faded out (invisible) until its map reloads.
bool DeathWasAVoidFall(bool void_bit, float fade);
// The point a party death wakes this member at (Kaniel 2026-10-13, after a playtest wipe woke the
// host in the Hunter's Dream and the guest at its own Central Yharnam lamp): a guest wakes where
// the host does (host_point: the host's GlobalState +0x1528, from the world log), so the party
// wakes together. But a guest's special death keeps its own point: one that changed its point at
// the death (own_point, now, against own_point_alive, the one that stood before it; see
// ReturnPointBeforeDeath): its first death (the Dream), a kidnapping (the Gaol). The host, a
// guest without the host's point, or one never seen up (nothing to tell a special death by) keep
// their own.
s32 PartyRespawnPoint(bool guest, s32 own_point, std::optional<s32> own_point_alive,
                      std::optional<s32> host_point);
// How long a guest's point must stand while it is up to be its point from before a death: the
// game may set a death's point while the player still stands (a kidnapping's grab), and that one
// is the death's. A lamp rested at just before a death counts as special (its own point), as
// before this rule.
constexpr s64 ReturnPointSettleMs = 10'000;
class ReturnPointBeforeDeath {
public:
    // The point (GlobalState +0x1528) on a tick while the player is up (not down, not dead).
    void Sample(s32 point, s64 now_ms);
    // The last point that stood ReturnPointSettleMs (the first one seen stands at once).
    std::optional<s32> Point() const {
        return settled;
    }
    void Reset() {
        settled.reset();
        changed.reset();
    }

private:
    std::optional<s32> settled;
    std::optional<std::pair<s32, s64>> changed; // a newer point and when it was first seen
};
// A flag every party member gets when another one sees it set (see PartyFlagBoard): a lamp's
// active flags (LampOfActiveFlag and the one after), which the game does not sync itself, and
// a boss fog's "entered" flag, which a guest's (unsynced) fog event sets to start the boss.
bool SharedWithParty(s32 flag);
// A boss fog's "host entered" flag (12414700 for Father Gascoigne): the boss start event waits
// on it. Party guests set it too (party_boss_events.py); it holds for one map visit.
bool IsBossFogEntryFlag(s32 flag);
// The crash catcher's own log line (see bloodborne_crash_dump.cpp): the exception code, the
// address, the eboot offset when it is inside the game, and the thread's name.
std::string FormatCrashLine(u32 code, u64 address, u64 image_base, u64 image_size,
                            std::string_view thread);
// The words of a stack that point into the eboot, as " eboot+0x..." (at most `max` of them).
std::string FormatGameAddresses(std::span<const u64> words, u64 image_base, u64 image_size,
                                size_t max);
// The co-op HP effect a boss start gives the boss (SpEffect 7500/7501/7502, max HP x1.5/x2/x2.5)
// sometimes misses a boss on one side (hidden by its entrance there), so that side keeps the
// unscaled max HP. Given the max HP seen before any co-op effect, the new (max HP, HP) for a
// living boss that has the effect but not the scaled max HP.
std::optional<std::pair<s32, s32>> CoopHpScale(s32 unscaled_max, s32 max, s32 hp,
                                               std::span<const s32> effects);
// What a member posts to the party's flag board from a change it saw: the SharedWithParty flags
// that turned on.
std::vector<std::pair<s32, bool>> FlagsToShare(const FlagChanges& changes);
// What a guest running its map alone (no host there: its game runs the map's story) shares with
// the team besides FlagsToShare: its changes to that map's own flags (TakesHostMapFlag, from
// MapFlagBase(map)), sets and clears, as (flag, on). Kaniel's test 2026-10-23: the hag a lone guest
// killed in Cathedral Ward was alive again for the host, a door it opened shut.
std::vector<std::pair<s32, bool>> LoneGuestFlagsToShare(const FlagChanges& changes, u32 map);
// Which of a lone guest's map changes go to the team (LoneGuestFlagsToShare): not the ones its
// game's load sets up. Changes in the first LoadFlagsMs after a load are that map's load flags
// (remembered for the session, a later visit sets them a moment later as well); after it every
// other map change goes, at once (Kaniel's test 2026-10-23: the hag died 9 s after the load; with
// no wait at all the boss's per-visit flags, 12414802..., went to the host too).
class LoneGuestShares {
public:
    static constexpr s64 LoadFlagsMs = 8'000;
    std::vector<std::pair<s32, bool>> Take(const FlagChanges& changes, u32 map, s64 since_load_ms);

private:
    std::map<u32, std::set<s32>> load_flags;
};
bool IsBulkFlagChange(size_t changed_flags);
void RecordWorldChanges(std::map<s32, bool>& world, const FlagChanges& changes);
std::string FormatWorldChanges(const std::map<s32, bool>& world);
std::map<s32, bool> ParseWorldChanges(std::string_view text);
// Lamps shared by the party (a message on the server link, once POST /bloodborne/party/flags):
// every member posts the lamps it saw lit (FlagsToShare; "<member> <since> [<flag>:<0|1>...]") and
// sets the other members' changes after <since> (the answer: "<latest>", then "<flag> <0|1>"
// lines).
struct FlagBoardAnswer {
    s64 latest = 0;
    std::vector<std::pair<s32, bool>> changes;
    bool operator==(const FlagBoardAnswer&) const = default;
};
std::string EncodeFlagUpdate(std::string_view member, s64 since,
                             std::span<const std::pair<s32, bool>> changes);
std::optional<FlagBoardAnswer> ParseFlagBoard(std::string_view body);

// Kill echoes shared across maps (a message on the server link, once POST
// /bloodborne/party/echoes): the echoes a member got from a kill (AddSouls called from the enemy
// kill 0x018FC99B or the boss kill 0x01385263) while no other member was on its map go to the board
// ("<member> <since> [<echoes>...]"), and the other members add the ones after <since> (the answer:
// "<latest>", then one "<echoes>" line each).
struct KillEchoes {
    s32 amount = 0;
    u32 map = 0; // where the kill was made; 0 from an older post
    bool operator==(const KillEchoes&) const = default;
};
struct EchoBoardAnswer {
    s64 latest = 0;
    std::vector<KillEchoes> echoes;
    bool operator==(const EchoBoardAnswer&) const = default;
};
// `posts`: a party member in the session is on another map (the game shares the echoes with the
// ones on this map itself) and this member is the one here that posts them (PostsKillEchoes).
bool SharesKillEchoes(u64 caller_offset, bool posts);
// The other members' kill echoes read from the board, and which of them this member is owed:
// all but those the game paid it itself, being on the kill's map then (a kill reward of the same
// echoes on that map, PaidWindowMs before the post was read up to ReceivedGraceMs after; each
// reward pays one post). Rig smoke 2026-10-12 (run 151918): deciding by the map this member
// stood on when the grant ran dropped the host's kill for the second guest, read mid-trip and
// granted once it was back on the kill's map.
class KillEchoLedger {
public:
    static constexpr s64 PaidWindowMs = 20'000;
    // How long a post read waits for this game's own reward of the same kill.
    static constexpr s64 ReceivedGraceMs = 2'000;
    // The game gave this member a kill's echoes (its kill reward) on `kill.map`.
    void Paid(KillEchoes kill, s64 now_ms);
    // Another member's post of a kill's echoes, read from the board.
    void Received(KillEchoes kill, s64 now_ms);
    void Received(std::span<const KillEchoes> kills, s64 now_ms) {
        for (const KillEchoes& kill : kills) {
            Received(kill, now_ms);
        }
    }
    // The posts now owed (to add once); those paid by the game are dropped, the newest wait.
    std::vector<KillEchoes> Due(s64 now_ms);

private:
    std::vector<std::pair<KillEchoes, s64>> paid;
    std::vector<std::pair<KillEchoes, s64>> received;
};
// Whether the party boards (lamps and boss fog flags, trophies) are read, posted and applied:
// whenever a party is configured and the player is loaded, in a session or not.
bool SharesWithParty(bool party_configured, bool player_loaded);

// Enemy deaths kept in step across loads (a message on the server link, once POST
// /bloodborne/party/kills; the flag board's format with character handles): the kills this member
// made during its current visit to a map, posted as (handle, true), and taken back as (handle,
// false) when the visit ends. A visit is one load of a map (the map and the player's character).
// Where a party guest resumes (user/bloodborne_party_resume_slotN.txt, "<map> <x> <y> <z>"):
// its last map and position, so the next start goes back there instead of to its save's
// position (where it was first summoned from).
struct ResumePoint {
    u32 map = 0;
    LampPosition position{};
    s32 progress = 0; // event flags set when it was kept (a new character has far fewer)
    bool operator==(const ResumePoint&) const = default;
};
std::string EncodeResumePoint(const ResumePoint& point);
std::optional<ResumePoint> ParseResumePoint(std::string_view text);
// The warp point a trip to a map uses: its first lamp (<area><block>2950), or the Hunter's Dream
// headstones' point.
s32 MapWarpPoint(u32 packed_map);
struct ResumeStep {
    enum class Kind { Stay, Move, Travel, Forget };
    Kind kind = Kind::Stay;
    s32 warp_point = 0;
};
// From where the guest loaded: stay (nothing remembered, or within 5 m), move on the same map,
// travel to the remembered map first, or forget the point (another character in the slot). A
// guest no longer on the map it loaded on (`loaded_map`, 0 if not known) travelled on: it stays.
// One that moved more than a metre from where its load put it (`loaded_at`, when known) set off on
// its own: it stays too (phase 7).
ResumeStep ChooseResumeStep(const ResumePoint& point, u32 map, const LampPosition& at, s32 progress,
                            u32 loaded_map = 0,
                            std::optional<LampPosition> loaded_at = std::nullopt);
// Phase 7: the point a loaded guest resumes at: the server's (where it last stood in its team's
// world: the newest) when it has one, else its own file's, which counts only out of a session.
// Kaniel 2026-10-18: a team's load ends with its hunters together, so a guest's team load takes
// its host's place (`from_host`, see HostPlaceOf) over both when its own is on another map (one on
// the host's map stays: phase 7's way back to where a guest's game died).
std::optional<ResumePoint> PickResumePoint(bool in_session, std::optional<ResumePoint> from_file,
                                           std::optional<ResumePoint> from_server,
                                           std::optional<ResumePoint> from_host = std::nullopt);
// How long a guest's team load waits for its host's place (the host's world may still be loading
// under the team's screen) before it resumes at its own last place.
constexpr s64 HostPlaceWaitMs = 20'000;
bool WaitsForHostPlace(bool team_load, bool host_known, s64 waited_ms);
// Kaniel's play test 2026-10-19: a guest's team load joins its host's session after its own world
// came in, and the join's reload puts it back where its save stood. krolebord's resume moved him
// before that reload, which undid it. A team load resumes once the guest is in the session (its
// reload done), or after `TeamSessionWaitMs` from its first load without one.
constexpr s64 TeamSessionWaitMs = 60'000;
bool WaitsForTeamSession(bool team_load, bool in_session, s64 waited_ms);

// World state per map (a message on the server link, once POST /bloodborne/party/mapstate): the
// host posts the bits of its map's 10000 flags from MapFlagBase (MSB first, as the flag store keeps
// them) as "<member> <map> <hex>", and a guest loading that map sets the TakesHostMapFlag ones to
// the host's.
s32 MapFlagBase(u32 packed_map);
bool TakesHostMapFlag(s32 flag);
struct MemberMapState {
    u32 map = 0;
    std::vector<u8> bits;
    bool operator==(const MemberMapState&) const = default;
};
std::string EncodeMapStateUpdate(std::string_view member, u32 map, std::span<const u8> bits);
std::optional<std::vector<MemberMapState>> ParseMapStateBoard(std::string_view body);

// A member's visit to a map (see VisitKills): it ends when the enemies respawn for that member.
// trips counts the party trips started (lamps, the Dream, a death sent home). The visit is named
// by the map it began on, not the one the player walked on to.
class VisitTracker {
public:
    u64 Update(u32 map, u64 player, bool in_session, u64 trips);

private:
    u32 map = 0;      // where this visit began
    u32 seen_map = 0; // where the player was at the last update
    u64 player = 0;
    u64 trips = 0;
    u32 generation = 0;
};

// Where this player is (docs/WORLD_SYNC.md, stage A): fed one reading per game tick, it is the
// only thing that decides the map and the visit. 53 places read the map themselves and got 0 at
// a fog border, in a load or a cutscene, so features briefly disagreed (a border walk withdrew 72
// kills, the Ward/Yharnam time-of-day reload ping-pong).
enum class PlaceState : u8 { Unknown, Loading, Cutscene, Playing };
enum class PlaceCause : u8 { Load, Border, Lamp, Teleport, DeathReload, Cutscene, CutsceneEnd };
struct Place {
    PlaceState state = PlaceState::Unknown;
    u32 map = 0;      // the last committed map: once one is read it never turns 0 again
    u64 visit = 0;    // committed arrivals (loads, border crossings, teleports): only grows
    s64 since_ms = 0; // when the place last changed
    bool operator==(const Place&) const = default;
};
struct PlaceReading {
    s64 now_ms = 0;
    u32 map = 0;    // the game's map now, 0 when it does not read
    u64 player = 0; // the local player's character, 0 while there is none
    std::optional<LampPosition> at;
    bool loading = false;  // the game's loading flag (GlobalState +0x08)
    bool trip = false;     // a party trip (lamp, headstone, event warp) is under way
    bool cutscene = false; // a cutscene plays (no game signal found yet: always false)
    bool dead = false;     // the local player's HP is 0
};
struct PlaceChange {
    Place from;
    Place to;
    PlaceCause cause = PlaceCause::Load;
    bool operator==(const PlaceChange&) const = default;
};
class PlaceTracker {
public:
    // A map change without a load (a walk across a fog border) commits once the new map has read
    // the same for this long; going back before cancels it.
    static constexpr s64 BorderSteadyMs = 1'500;
    // A map change within TeleportWindowMs of a move this far in one tick is a teleport (an event
    // warp without a load screen), committed at once.
    static constexpr float TeleportMeters = 50.0f;
    static constexpr s64 TeleportWindowMs = 1'000;

    std::optional<PlaceChange> Update(const PlaceReading& reading);
    const Place& Current() const {
        return place;
    }

private:
    std::optional<PlaceChange> Move(PlaceState state, u32 map, bool arrival, PlaceCause cause,
                                    s64 now_ms);
    PlaceCause LoadCause() const;

    Place place;
    u64 player = 0;
    std::optional<LampPosition> last_at;
    std::optional<s64> jumped_ms;
    std::optional<std::pair<u32, s64>> pending; // a border crossing: the new map, read since
    bool arrival_due = false; // a new player character: the world loaded between two ticks
    bool trip_seen = false;
    bool death_seen = false;
};
std::string_view PlaceStateName(PlaceState state);
std::string_view PlaceCauseName(PlaceCause cause);
// "Co-op place: <from> -> <to> map <hex> visit N (<cause>)"
std::string FormatPlaceChange(const PlaceChange& change);
// A change of place that starts a new world (world_changed_ms: the shown board waits for its
// story, deaths then are no kills): a new visit that plays after a load, the first one after the
// title (Unknown -> Playing, which once never counted, so a host posted its characters before its
// story removed Iosefka) and a trip's arrival on another map (Playing -> Playing), not a walk
// across a border or a teleport.
bool StartsNewWorld(const PlaceChange& change);

// What ends the state that lives for a visit or a session (docs/WORLD_SYNC.md): each container
// registers its reset for the events that end it, and one dispatch runs them, instead of a reset
// flag per feature. A wipe, a leave, a join and a new character also start a new session epoch.
// Leave is this member's own; MemberLeft is another member leaving a session that goes on (its
// game died or it went home: rig 2026-10-12, the others ran no lifecycle), and keeps the epoch.
enum class Lifecycle : u8 { Wipe, Trip, Leave, Join, NewCharacter, MemberLeft };
std::string_view LifecycleName(Lifecycle event);
// Other members leaving the session while this one stays: the other session members are counted
// each tick, and a drop that holds for SettleMs is a departure per member gone. Not counted while
// `steady` is false (a load or a trip, when the list is rebuilt) or out of the session (a drop
// that comes with this member's own Leave); a rise is taken at once.
class MemberDepartures {
public:
    static constexpr s64 SettleMs = 1'000;
    // How many members left (once per departure).
    size_t Tick(bool in_session, bool steady, size_t others, s64 now_ms);

private:
    size_t known = 0;
    std::optional<s64> fewer_since;
};
class LifecycleResets {
public:
    void Register(std::string name, std::initializer_list<Lifecycle> events,
                  std::function<void()> reset);
    // Runs the resets registered for `event`, in the order they were registered, and returns
    // their names.
    std::vector<std::string> Dispatch(Lifecycle event);
    u64 Epoch() const {
        return epoch;
    }

private:
    struct Entry {
        std::string name;
        u32 events = 0; // bit per Lifecycle
        std::function<void()> reset;
    };
    std::vector<Entry> entries;
    u64 epoch = 1; // 0 stays free for "unknown"
};

// Enemy HP between members whose worlds are separate (after a trip each member runs its own copy
// of a map, and the game no longer sends HP between them): each member posts the damage its
// copy of each enemy took from it this visit (EnemyHp.hp is that damage), under its own source
// (EnemyHpReport.visit: the member and its visit), and every copy takes the others' damage on
// top, scaled by max HP. Two players hitting one enemy add up (the lowest HP left, shared
// before, let their damage overlap: friend run 2026-10-10). A copy never goes below 1 HP from
// the others: the death comes from the game and the kill board.
struct EnemyCopy {
    u32 handle = 0;
    s32 hp = 0;
    s32 max = 0;
    // What this member's own hits took off this copy (the game's hit function, attacker = the
    // local player), counted up since the game started; nothing when that hook is not in.
    std::optional<s64> own_hits;
};
struct EnemyWrite {
    u32 handle = 0;
    s32 hp = 0;
    bool operator==(const EnemyWrite&) const = default;
};
class SharedDamage {
public:
    struct Step {
        std::vector<EnemyHp> post;      // this member's own damage, to post
        std::vector<EnemyWrite> writes; // the HP to write on this member's copies
    };
    // `active`: in the session with worlds apart (else one world or no session: nothing is posted
    // or written, but the host's copies are still counted, see `role`).
    // `now_ms`: the clock for holding the damage of a member whose hits the game also sends (see
    // SharedDamageHoldMs); 0 holds nothing.
    // `role`: across a switch between one world and worlds apart (rig 20261013-011544, joins,
    // hp-across-blocks) the host's copies are the one world's, so their drop there (its own hits
    // and the guest's, which the game sent) is the host's damage once apart. A guest's drop in one
    // world is not its own: its copies took the host's HP (the game's sync, LowerToHostHp) and its
    // hits reached the host by the game, so its counts start over at a switch.
    Step Update(u64 visit, bool active, std::span<const EnemyCopy> alive,
                std::span<const EnemyHpReport> others, s64 now_ms = 0,
                PartyRole role = PartyRole::Host);
    // A death reset the map: the damage in `others` (reported before the reset, on the world that
    // was) is not taken from now on; the members' reports after their own reloads are.
    void ForgetDamageBefore(std::span<const EnemyHpReport> others);

private:
    struct Track {
        s32 start = 0;       // the copy's HP when first seen this visit (or since it came back up)
        s32 written = 0;     // what the others' damage took off it
        s32 last_own = 0;    // the previous read's own damage: posted is the lower of the two
        s64 hits_before = 0; // EnemyCopy::own_hits when first seen this visit
        s32 paid = 0;        // what the game sent of the others' damage that their posts used up
        // The host in one world, counted hit by hit: the drop its own hits did not take (the
        // guest's hits the game sent), two reads agreeing; its own once the worlds go apart.
        s32 unclaimed = 0;
    };
    u64 visit = 0;
    bool active = false;
    std::map<u32, Track> tracks;
    std::map<std::pair<u64, u32>, s32> written;    // (other member's source, handle) -> taken
    std::map<std::pair<u64, u32>, s32> written_by; // of `written`, what the board wrote
    std::map<std::pair<u64, u32>, s64> held_since; // a member's unwritten damage, held since
    std::set<u64> game_sends;   // sources whose hits the game sent here after a write of them
    std::set<u64> before_reset; // sources that reported on the world before a map reset
};
// A report can come straight over the P2P link before the game's own copy of the same hit; once
// that happened for a member, its new damage waits this long for the game's copy.
constexpr s64 SharedDamageHoldMs = 250;
// How often the HP board is posted and read, and the world's enemies read for it (see
// HpBoardWait).
constexpr std::chrono::milliseconds HpBoardRound{100};
// How long the HP board client waits before its next post: none when this member's own damage
// changed (a hit), else a round.
std::chrono::milliseconds HpBoardWait(bool own_damage_changed);
// Enemy HP straight from game to game (Kaniel 2026-10-11): this member's HP board report also goes
// to every room peer over the port's P2P transport, as a "SHAD" frame of DirectDamageType with the
// map (the board's MemberMapState), at once on a change and again every DirectDamageResendMs. A
// sender's last frame stands for DirectDamageLifeMs. The board stays underneath.
constexpr u8 DirectDamageType = 0x30;
constexpr s64 DirectDamageResendMs = 500;
constexpr s64 DirectDamageLifeMs = 2'000;
std::vector<u8> EncodeDirectDamage(u32 map, std::span<const u8> report);
std::optional<MemberMapState> DecodeDirectDamage(std::span<const u8> frame);
class DirectDamageInbox {
public:
    void Take(u64 sender, MemberMapState state, s64 now_ms);
    // The fresh frames, newest first.
    std::vector<MemberMapState> Fresh(s64 now_ms);
    // Drops every sender but `senders` (a member left: its frames go at once).
    void KeepOnly(std::span<const u64> senders) {
        std::erase_if(by_sender, [&](const auto& entry) {
            return std::ranges::find(senders, entry.first) == senders.end();
        });
    }

private:
    struct Received {
        MemberMapState state;
        s64 at = 0;
    };
    std::map<u64, Received> by_sender;
};
// The other co-op frames on the same link (Kaniel 2026-10-11): a member's vitals line (party
// death) and its standing kills, each at once on a change and again every DirectDamageResendMs,
// standing DirectDamageLifeMs at the receiver. Every type from 0x30 to 0x3F is the co-op channel.
constexpr u8 DirectVitalsType = 0x31;
constexpr u8 DirectKillsType = 0x32;
bool IsCoopFrameType(u8 type);
std::vector<u8> EncodeCoopFrame(u8 type, std::span<const u8> payload);
std::optional<std::pair<u8, std::vector<u8>>> DecodeCoopFrame(std::span<const u8> frame);
// A peer's frames by sender, each standing DirectDamageLifeMs after it came.
template <typename T>
class DirectInbox {
public:
    void Take(u64 sender, T value, s64 now_ms) {
        by_sender[sender] = {std::move(value), now_ms};
    }
    // The fresh ones, newest first.
    std::vector<T> Fresh(s64 now_ms) {
        std::erase_if(by_sender, [now_ms](const auto& entry) {
            return now_ms - entry.second.second > DirectDamageLifeMs;
        });
        std::vector<std::pair<T, s64>> fresh;
        for (const auto& [sender, received] : by_sender) {
            fresh.push_back(received);
        }
        std::ranges::stable_sort(fresh, std::greater{}, &std::pair<T, s64>::second);
        std::vector<T> values;
        for (auto& [value, at] : fresh) {
            values.push_back(std::move(value));
        }
        return values;
    }
    // Drops every sender but `senders` (a member left: its frames go at once).
    void KeepOnly(std::span<const u64> senders) {
        std::erase_if(by_sender, [&](const auto& entry) {
            return std::ranges::find(senders, entry.first) == senders.end();
        });
    }

private:
    std::map<u64, std::pair<T, s64>> by_sender;
};
struct DirectKills {
    std::string member;
    std::vector<u32> handles;
    bool operator==(const DirectKills&) const = default;
};
// Which build each game runs (robustness review 2026-10-12): games on different builds shared a
// party without a word, and the frames above carry no version. Every game sends its hello to the
// room's peers every PeerHelloResendMs and at once to a new peer; a peer on another protocol,
// build or set of event files is logged once. Builds before it drop the type: their decoders
// take only the types they know.
constexpr u8 PeerHelloType = 0x33;
// Raised when a co-op frame's meaning changes. 2: the world log's frames (WorldLogProposeType ...)
// and the hello's features. 3: the shown characters on the world log. 4: the shared flags and the
// host's map state on it (frames 0x39, 0x3A, the flag entries).
// 5: enemy HP on it (frames 0x3B, 0x3C, the HP entries; every entry carries a value).
// 6: a snapshot ends with the recent flag events (frame 0x3D); the world snapshot on the server.
// 7: the flags part's after-wipe byte. 8: the host's return point (the ReturnPoint entry, and the
// map state part ends with it). 9: the members' portraits (PortraitPartType, 0x3E).
// (BloodborneCoop::CoopFrameProtocol: shadNet knows it too, for the build check on join.)
constexpr u8 CoopProtocolVersion = BloodborneCoop::CoopFrameProtocol;
constexpr s64 PeerHelloResendMs = 10'000;
// PeerHello::features: WorldLogFeature when this game runs the world log (it is not switched off,
// SHADPS4_BLOODBORNE_WORLD_LOG=0). Every member runs one build (Kaniel 2026-10-12): a peer on
// another build or with the log switched off is an error the hello check logs (PeerBuildCheck),
// not a path of its own.
constexpr u8 WorldLogFeature = 0x01;
struct PeerHello {
    u8 protocol = CoopProtocolVersion;
    std::string revision; // the build's source revision ("-dirty": changes not in a commit)
    u64 event_files = 0;  // a hash of the map event files the game reads, 0 when unknown
    std::string member;   // the board's name for the member, "" before it has one
    u8 features = 0;      // bits (WorldLogFeature); 0 from a build before protocol 2
    std::string build;    // the build fingerprint (coop/build_fingerprint.h), "" from an older one
    bool operator==(const PeerHello&) const = default;
};
// The payload: protocol, revision (u8 length first), event files (u64), member (u8 length
// first), features (u8, since protocol 2), build fingerprint (u16 length first). A decoder takes a
// longer payload (fields a later build added) and one without the features or the fingerprint (a
// build before them).
std::vector<u8> EncodePeerHello(const PeerHello& hello);
std::optional<PeerHello> DecodePeerHello(std::span<const u8> payload);
class PeerBuildCheck {
public:
    // The warning line when a peer's hello differs from this game's, once per member and build.
    std::optional<std::string> Take(const PeerHello& theirs, const PeerHello& mine);

private:
    std::set<std::tuple<std::string, u8, std::string, u64, u8, std::string>> warned;
};
// A hash of the map event files (*.emevd.dcx) in `folder`, the game's dvdroot_ps4/event (the
// co-op kit replaces some): names and bytes. 0 when there are none or one cannot be read.
u64 EventFilesHash(const std::filesystem::path& folder);
// A hash of one file's bytes (the eboot, patches.bin), 0 when it cannot be read.
u64 FileHash(const std::filesystem::path& file);
// Build check on join (Kaniel 2026-10-24): what the game knows of its build, and from its
// environment (`env`: std::getenv, or a test's) its release (BB_RELEASE_VERSION, the launcher's;
// "dev" without one), the settings that change the game (harder enemies, the gameplay patches
// run.py names in BB_PATCH_GAMEPLAY, the co-op rule switches) and, only for the logs, the visual
// ones (frame rate, mods, other patches). Hashes 0 are unknown.
struct BuildIdentity {
    std::string revision;
    u64 eboot = 0;
    u64 event_files = 0;
    u64 patches_bin = 0;
};
BloodborneCoop::BuildFingerprint CoopBuildFingerprint(
    const BuildIdentity& identity, const std::function<const char*(const char*)>& env);
std::vector<u8> EncodeDirectKills(std::string_view member, std::span<const u32> handles);
std::optional<DirectKills> DecodeDirectKills(std::span<const u8> payload);
std::unordered_set<u32> DirectKillHandles(std::span<const DirectKills> kills);

// The world snapshot on the server (a message on the server link, once POST
// /bloodborne/party/world; Kaniel 2026-10-12): in place of the polled per-feature boards while the
// world log runs, the host posts its whole log state every WorldSnapshotPostMs as one blob, the
// same snapshot frames it sends a member that asks over the link. A member reads it only when its
// copy is not fresh (a join, a restart, its link to the host down) and takes the frames as it takes
// them from the host. The blob: a version byte, then each frame as type (u8), length (u16),
// payload, after the frame count (u16).
constexpr s64 WorldSnapshotPostMs = 2'000;
// A server snapshot older than this is not taken (a party that went on without its host).
constexpr s64 WorldSnapshotMaxAgeMs = 30'000;
using WorldSnapshotFrames = std::vector<std::pair<u8, std::vector<u8>>>;
std::vector<u8> EncodeWorldSnapshot(const WorldSnapshotFrames& frames);
std::optional<WorldSnapshotFrames> DecodeWorldSnapshot(std::span<const u8> blob);
// The board's messages: "<member> [<hex blob>]" (a post with the blob, or a read), and the answer
// "<age ms> <hex blob>\n", or nothing when the party has no snapshot.
std::string EncodeWorldSnapshotUpdate(std::string_view member, std::span<const u8> blob);
struct WorldSnapshotAnswer {
    s64 age_ms = 0;
    std::vector<u8> blob; // empty: none
    bool operator==(const WorldSnapshotAnswer&) const = default;
};
std::optional<WorldSnapshotAnswer> ParseWorldSnapshotAnswer(std::string_view body);
// The game's hit (0x01A2A770, called through the victim's damage module): rdi the module (+0x08
// the victim), rsi the attacker (null for none), rdx the hit. Hooked at its entry (HP before)
// and at the return from its IsNoDead check once the hit is applied (r14 the module again).
constexpr u64 HitOffset = 0x01A2A770;
constexpr std::array<u8, 10> HitPrologue{0x55, 0x48, 0x89, 0xE5, 0x41,
                                         0x57, 0x41, 0x56, 0x41, 0x55};
constexpr u64 HitAppliedOffset = 0x01A2C05F;
// mov rcx, [r14 + 8]; mov rdx, [rcx + 0x3B0]
constexpr std::array<u8, 11> HitApplied{0x49, 0x8B, 0x4E, 0x08, 0x48, 0x8B,
                                        0x91, 0xB0, 0x03, 0x00, 0x00};
// Status effect HP changes (0x01A2FA40, poison and the like; run for every character every frame):
// its two calls of SetHp, hooked just before each, where HP changes. r12 is the HP field block
// (data +0xF0: [r12] the character, [r12 + 8] the HP) and esi the new HP.
constexpr std::array<u64, 2> EffectHpChangeOffsets{0x01A2FB5D, 0x01A2FBF9};
// xor edx, edx; xor ecx, ecx; mov rdi, r12
constexpr std::array<u8, 7> EffectHpChange{0x31, 0xD2, 0x31, 0xC9, 0x4C, 0x89, 0xE7};
constexpr u64 SetHpOffset = 0x01A0D470;

// What this member's own hits took off each enemy, by handle (see EnemyCopy::own_hits), with the
// HP ticks of status effects (poison) on enemies whose last hit was its own.
// Who the game's hit names as its attacker: this player, another player's character, or nobody (a
// thrown item's or a trap's hit).
enum class HitBy { Me, OtherPlayer, Nobody };
class OwnDamage {
public:
    void Hit(u32 handle, bool mine, s32 taken);
    // A hit with no attacker counts as this player's, unless another player hit it last.
    void Hit(u32 handle, HitBy by, s32 taken);
    void Effect(u32 handle, s32 taken);
    s64 Total(u32 handle) const;
    // Whether the last hit on it was this player's (nothing when no hit was seen).
    // A hit's HP left (`hp`, at `now_ms`): one that leaves 0 is when the killing blow landed
    // (SeenDead), until a later hit sees it up again.
    void HitLeft(u32 handle, s32 hp, s64 now_ms);
    std::optional<s64> ZeroHitMs(u32 handle) const;
    std::optional<bool> LastHitMine(u32 handle) const {
        const auto last = last_hit_mine.find(handle);
        return last != last_hit_mine.end() ? std::optional{last->second} : std::nullopt;
    }
    const std::map<u32, s64>& All() const {
        return totals;
    }

private:
    std::map<u32, s64> totals;
    std::map<u32, bool> last_hit_mine;
    std::map<u32, s64> zero_hit_ms;
};
std::vector<EnemyWrite> LowerToHostHp(std::span<const EnemyCopy> mine,
                                      std::span<const EnemyHpReport> reports);
// The HP to write on the local copy, or nothing: only ever down, never to 0 (a death comes
// from the game itself), scaled by each copy's max HP (the co-op HP bonus may differ).
std::optional<s32> SharedEnemyHp(s32 hp, s32 max, s32 other_hp, s32 other_max);
// The HP to write on this member's copy of `handle`: the lowest share any other member in `map`'s
// area reports (their posts, in the map state board's format), or nothing.
std::optional<s32> LowestSharedHp(u32 handle, s32 hp, s32 max, u32 map,
                                  std::span<const MemberMapState> others);
// Whether a guest's copy of a character follows the host's shown board (hidden, switched off):
// not the Dream messengers that give each player its own gifts (their events and flags are each
// player's, see KeepsLocalFlag); `entity` -1 when the character has none.
bool FollowsTheHostsShown(s32 entity);
// Of a guest's own characters (`own`, see FollowsTheHostsShown), those it hid (`hidden_here`) or
// switched off (`switched_off`) to match the host before they were known as its own: to show and
// to switch on again.
struct OwnRestore {
    std::vector<u32> show;
    std::vector<u32> switch_on;
};
OwnRestore RestoresOwnCharacters(std::span<const u32> own,
                                 const std::unordered_set<u32>& hidden_here,
                                 const std::unordered_set<u32>& switched_off);

// A map's story removes some characters (2004:29, the hide byte +0x28) seconds after each load,
// 3.5 to 13 s on the rig, so until then they stood there: Gilbert by the second Central Yharnam
// lamp after a trip, Iosefka (Kaniel and Lisara's playtest 2026-10-15). Every game remembers per
// map which characters its story had removed once a visit settled (StoryHoldMs after its load),
// and on the next visit switches those off (2004:5) as they appear, until the story removes them
// again (then switched on: removed, they stay unseen) or StoryHoldMs passes (the story no longer
// removes them: switched on and forgotten). A guest's characters hidden to match the host's board
// (the hide byte) count as removed by the story (the host's is the same story); those it only
// switched off do not (held, nothing would show the story switched them off too), nor a kill.
struct StoryCharacter {
    u32 handle = 0;
    bool removed = false;  // the hide byte +0x28
    bool disabled = false; // the enable switch off
    bool followed = false; // a guest's, hidden or switched off to match the host's board
    bool killed = false;   // hidden for a member's kill
    bool dead = false;     // dead in its world (IsDeadInItsWorld): no story removal
};
struct StoryHoldStep {
    std::vector<u32> hold;      // switch off now (then Held, or Unheld if that failed)
    std::vector<u32> release;   // switch on again
    std::vector<u32> hand_over; // held, now the co-op code's: no longer held here
    bool operator==(const StoryHoldStep&) const = default;
};
constexpr s64 StoryHoldMs = 20'000;
class StoryRemovals {
public:
    // `seen`: every character loaded now but the players; `visit_ms`: the visit's load.
    StoryHoldStep Step(u32 map, s64 visit_ms, s64 now_ms, std::span<const StoryCharacter> seen);
    void Held(u32 handle, s64 now_ms);
    // Kaniel 2026-10-16: another save (or the same slot started again) plays another world:
    // nothing its story removed counts.
    void Forget();
    bool Remembers(u32 map, u32 handle) const;
    size_t RememberedOn(u32 map) const;

private:
    std::map<u32, std::set<u32>> removed; // per map
    std::map<u32, s64> held;              // handle -> since
};

// A character's death flags (+0x1E3) say it is dead in its world: killed (bits 0 and 2, see
// IsRealDeath) or dead in the save and put down at load (0x26).
bool IsDeadInItsWorld(u8 death_flags);
// Or its HP is 0 of a known max: an enemy dead in the save loads at 0 HP with no death flags,
// neither hidden nor switched off (rig 2026-10-18; Kaniel's play test the same day: two such
// enemies stood for the guests while his own screen had none).
bool IsDeadInItsWorld(u8 death_flags, s32 hp, s32 max);
// The characters the host's shown board has as dead (Kaniel 2026-10-11: after a trip to the
// Dream and back the guests' own worlds had the host's dead enemies alive; they died before the
// session, so no kill board had them).
std::unordered_set<u32> DeadInTheHostsWorld(std::span<const CharacterShown> host);
constexpr float FarFromHostMetres = 50.0f;
// Whether an enemy another member killed dies here by the game's own kill (near this player:
// within DiesNearMetres) rather than being hidden (Kaniel 2026-10-11: they vanished).
constexpr float DiesNearMetres = 40.0f;
bool DiesInsteadOfHiding(const std::optional<LampPosition>& me,
                         const std::optional<LampPosition>& enemy);
bool FarFromHost(const std::optional<LampPosition>& host,
                 const std::optional<LampPosition>& character);
// What to write on this member's copies to match the host: every character both worlds have
// that differs, apart from those this member's kill board hid (`kill_hidden`: another member
// killed them, which the host may not have heard yet). A character far from the host is left as
// it is, except one this member hid (`hidden_here`), which is shown again.
std::vector<CharacterShown> ShownLikeTheHost(std::span<const CharacterShown> host,
                                             std::span<const CharacterShown> mine,
                                             const std::unordered_set<u32>& kill_hidden,
                                             const std::unordered_set<u32>& hidden_here = {});
// A character's enable switch (event command 2004:5; `disabled` = off: not drawn, no hits, no AI).
struct CharacterSwitch {
    u32 handle = 0;
    bool disabled = false;
    bool dead = false; // in this member's world (IsDeadInItsWorld): never switched on
    bool boss = false; // a boss (HandsOutBoss): its fight's own events switch it in every game
    bool operator==(const CharacterSwitch&) const = default;
};
// The host's story removes some characters (2004:29 sets the +0x28 byte) before their model loads;
// a guest's copy, built at its join, stays loaded and drawn with the byte set. What to switch on
// this member's copies (`mine`: each one's switch now): off where the host removed or switched
// it off near it; on where the host has it on near it and alive (its story switched it on: the
// red mist's enemy in the Forbidden Woods), unless it is dead here; back on far from the host
// only if this member turned it off (`switched_off`); never one the kill board hid
// (`kill_hidden`). A boss is left to this member's own fight (the Witch of Hemwick's vanish
// switches her in every game: Kaniel's playtest 2026-10-20, she flickered for a guest), apart from
// switching back on one this member turned off. Off at any distance where the host's switch is
// off for good (`off_for_good`: an ambush its event ended, AmbushSlotFlag).
std::vector<CharacterSwitch> SwitchedLikeTheHost(std::span<const CharacterShown> host,
                                                 std::span<const CharacterSwitch> mine,
                                                 const std::unordered_set<u32>& switched_off,
                                                 const std::unordered_set<u32>& kill_hidden,
                                                 const std::unordered_set<u32>& off_for_good = {});
// The Hemwick woods' mist ambush (m22_00 event 12204000, a slot per enemy 2200500-2200508): the
// slot's done flag of `entity`, if it is one. With the flag on and the host's copy off, the event
// switched it off for good (unarmed: a host under 15 Insight), so a guest follows that far from
// the host too; its own copy of the event does not run (rig 2026-10-21).
std::optional<s32> AmbushSlotFlag(s32 entity);
constexpr u32 AmbushMap = 0x16000000; // Hemwick Charnel Lane (m22_00)
// A character's hits switch (event command 2004:39; `off`: no hits). The Witch of Hemwick's Mad
// Ones wait with their hits off, and her summon (m22_00 event 12204839, which waits on her AI)
// turns them on in the host's game only: the guests' copies were switched on (SwitchedLikeTheHost)
// with no hits (Kaniel 2026-10-20: only the host saw them). What to set on this member's copies
// (`mine`): the host's switch for each one on near the host and alive there, never one the kill
// board hid (`kill_hidden`).
struct CharacterHits {
    u32 handle = 0;
    bool off = false;
    bool operator==(const CharacterHits&) const = default;
};
std::vector<CharacterHits> HitsLikeTheHost(std::span<const CharacterShown> host,
                                           std::span<const CharacterHits> mine,
                                           const std::unordered_set<u32>& kill_hidden);
// On this member's map change: back on, every character it switched off (`switched_off`) that is
// still loaded and off (`mine`). Characters by a map border stay loaded across it.
std::vector<CharacterSwitch> UndoneOnMapChange(const std::unordered_set<u32>& switched_off,
                                               std::span<const CharacterSwitch> mine);
// Whether a re-create that RecreateAfterJoin asks for is a join's (members with no task yet are
// left to the game's own join, see RecreateRemotePlayers) rather than a trip's still pending
// (`pending` and not `for_join`), which keeps building the tasks the trip's load threw away.
bool RecreateIsForAJoin(bool pending, bool for_join);
// A guest joining a session another guest is already in re-creates the other players once, as
// after a trip (Kaniel 2026-10-11: Ada joined after Brom and did not see him until both
// went to the Dream and back). Tick says when: a guest, SettleMs after it `arrived` in the
// session's world, seeing two or more other members within WindowMs of that. The clock starts
// there, not at the join (Krolebord 2026-10-18: joining from the team lobby with another guest,
// his re-create came before the join's reload, which threw the copies away).
class RecreateAfterJoin {
public:
    static constexpr s64 SettleMs = 5000;
    static constexpr s64 WindowMs = 60000;
    bool Tick(bool guest, bool in_session, size_t others, bool arrived, s64 now_ms) {
        if (!in_session) {
            joined_ms.reset();
            done = false;
            return false;
        }
        if (!joined_ms) {
            if (!arrived) {
                return false;
            }
            joined_ms = now_ms;
        }
        const s64 since = now_ms - *joined_ms;
        if (!guest || done || others < 2 || since <= SettleMs || since > WindowMs) {
            return false;
        }
        done = true;
        return true;
    }

private:
    std::optional<s64> joined_ms;
    bool done = false;
};
// What the host's world had for each character the last time it was near the host (Kaniel
// 2026-10-11: a character by a window in Central Yharnam is removed in the host's world even with
// the host beside it, its story; once the host was far, a guest went by its own world and showed
// it). Fill gives the host's post with each far character it saw near before put back to that
// state, as a near one; one never near the host stays far. Another map starts over.
class HostNearStates {
public:
    std::vector<CharacterShown> Fill(u32 map, std::span<const CharacterShown> host);

private:
    u32 map = 0;
    std::map<u32, CharacterShown> near_states;
};

// The kill board's answer names who posted each change ("<handle> <0|1> <member>").
struct KillChange {
    s32 handle = 0;
    bool killed = false;
    std::string member;
    bool operator==(const KillChange&) const = default;
};
struct KillBoardAnswer {
    s64 latest = 0;
    std::vector<KillChange> changes;
    bool operator==(const KillBoardAnswer&) const = default;
};
std::optional<KillBoardAnswer> ParseKillBoard(std::string_view body);
// The other members' standing kills: an enemy stays killed while any member's kill of it stands.
class PartyKills {
public:
    void Apply(std::span<const KillChange> changes);
    bool Contains(u32 handle) const;
    std::unordered_set<u32> All() const;
    // Every member's but `member`'s (the world log's host: its entries are the log's copy).
    std::unordered_set<u32> AllExcept(std::string_view member) const;
    // Drops what `member` posted (the board took back its kills: a post since FreshStart).
    void Forget(std::string_view member) {
        by_member.erase(std::string{member});
    }

private:
    std::map<std::string, std::set<u32>> by_member;
};

// Whether enemy deaths are watched now: sharing, and the world standing (no trip, no load).
bool WatchesKills(bool sharing, bool trip_active, bool loading);
// Whether a character's death flags (+0x1E3) mark a real death: bit 0 from the game's kill (an
// event's ForceCharacterDeath, 0x018BFAD3), bit 2 from a death by damage (0x018C52D1, after the
// kill reward). Both are cleared when it comes back (0x018CC550). Not with bit 5: an enemy its
// world puts down, dead in that save (0x26; cleared at 0x013320DF), is no kill made now.
bool IsRealDeath(u8 death_flags);
// A death the kill board counts: a real one (IsRealDeath), or 0 HP within ZeroHitCountsMs of a hit
// that left it at 0 (`zero_hit_ms`, see OwnDamage::HitLeft): the game sets the death bit only after
// the kill reward, 1-2.4 s later, and the other members' copies waited that long (Ada
// 2026-10-13). Never one its world put down (bit 5).
constexpr s64 ZeroHitCountsMs = 5'000;
bool SeenDead(u8 death_flags, s32 hp, std::optional<s64> zero_hit_ms, s64 now_ms);
// Whether a copy another member killed (`killed_by_member`), at 0 HP here since `zero_since_ms`
// without the game's death (no IsRealDeath bit), gets the game's kill now: after StuckDeathMs.
// Father Gascoigne's copy on a guest sat at 0 HP after the guest's own last hit (death flags 0x40)
// and his fight never ended there, so the lamp beside his arena answered with his talk (playtest
// 2026-10-13). Never one its world put down (bit 5).
constexpr s64 StuckDeathMs = 3'000;
bool FinishesStuckDeath(bool killed_by_member, s32 hp, u8 death_flags,
                        std::optional<s64> zero_since_ms, s64 now_ms);

// A character as the kill board sees it: its handle, its HP (0 once really dead, see
// IsRealDeath) and the object itself, which a world that loads makes anew.
struct SeenCharacter {
    u32 handle = 0;
    s32 hp = 0;
    u64 instance = 0;
    bool settling = false; // just taken over or handed back (see EnemyOwnerChangeSettles)
    // Boss-sized (max HP of DeathTraceMinMaxHp or more) and at its full HP in this read. Its
    // death right after such a read (no damage seen between: no hit of anyone's, as the HP board
    // writes the others' damage on this copy too) is its world putting it down, not a kill (rig
    // smoke 2026-10-12: Gascoigne, dead in the guests' saves, went down at full HP after the
    // settling time and was posted). A boss-sized enemy hurt before it died stays a kill.
    bool untouched_boss = false;
    // Killed by another member (see KilledByAnother): its death is that member's kill, which it
    // posts itself (rig smoke 2026-10-12, run 153353: a guest posted the host's kill too).
    bool others_kill = false;
};
// Whether a death on this member's copy is another member's kill: with the hit hooks in
// (`hooks`), the last hit on it was another player's, or nobody's hit was seen and this member
// is not the host (the world's kill is the host's to post). Without the hooks every death counts.
// Nor when this game runs the map's enemies (`runs_enemies`, phase 4): shadNet takes new kills
// there only from it, so every death on its copies is its to post (play test 2026-10-19).
bool KilledByAnother(bool hooks, std::optional<bool> last_hit_mine, bool hosting,
                     bool runs_enemies = false);

// How long a character's death is only seen, not posted, after this member took it over or
// handed it back (ClaimEnemiesWhenAlone): its own world's state settles on it then (a boss it
// killed for good goes down).
constexpr auto EnemyOwnerChangeSettles = std::chrono::seconds{5};
// Whether a character still settles at `now_ms`: within EnemyOwnerChangeSettles of the last time
// the map's world changed hands (`world_changed_ms`: a load that kept the map's objects, a summon
// into the host's world, or a guest starting or ending its run of the map's enemies) or of its
// own takeover or hand-back (`owner_changed_ms`).
bool CharacterSettles(s64 now_ms, s64 world_changed_ms, std::optional<s64> owner_changed_ms);
// Whether a settling character's death is still no kill: not when this player's own hit was its
// last (Kaniel's test 2026-10-23: a lone guest killed the Cathedral Ward hag 9 s after a trip,
// while its alone state had just changed; the kill was never posted and the host found her alive).
bool SettlingHidesDeath(bool settling, std::optional<bool> last_hit_mine);
// The world a lone guest's enemy claims (ClaimEnemiesWhenAlone) were made in: its map and the
// local player's character. A load makes a new character, also on the same map (a join, a
// respawn), and the claimed words go with the old world's memory (rig 2026-10-13: the hand-back
// wrote into freed memory and the game crashed at 0x15913d8 on the next reload).
class ClaimedWorld {
public:
    // Whether (map, player) is another world than the last one seen; a player of 0 (loading)
    // changes nothing.
    bool NewWorld(u32 map_now, u64 player_now) {
        const bool other_map = !seen || map_now != map;
        const bool other_player = player_now != 0 && player != 0 && player_now != player;
        seen = true;
        map = map_now;
        if (player_now != 0) {
            player = player_now;
        }
        return other_map || other_player;
    }

private:
    bool seen = false;
    u32 map = 0;
    u64 player = 0;
};

// MapsLoadedTogether: server/coop/world_log.h.

class VisitKills {
public:
    // `others_maps`: where the other members are now; `map`: where this member is (0: the
    // visit's map, the high 32 bits of the visit, see VisitTracker). A kill is of the map it died
    // on, which a walk across a border makes another than the visit's (Kaniel 2026-10-15: Ward
    // kills filed under Central Yharnam came back while Lisara stood in the Ward). When the visit
    // ends its kills stand while another member is on their map: they are held instead of taken
    // back, and taken back once neither this member nor another is on that map (Kaniel
    // 2026-10-11: the host's trip to the Dream respawned half of Central Yharnam while Brom was
    // still there).
    std::vector<std::pair<s32, bool>> Observe(u64 visit, std::span<const SeenCharacter> characters,
                                              std::span<const u32> others_maps = {}, u32 map = 0);
    const std::vector<u32>& Killed() const {
        return killed;
    }
    // This member's held kills on `map` (hidden on its own copies there).
    std::vector<u32> Held(u32 map) const;
    // All of this member's standing kills by map: this visit's (on the map each died on) and the
    // held ones (what it proposes to the world log).
    WorldLogKills Standing() const;
    // A death that resets the map (a party wipe): every kill of this member is taken back, the
    // held ones too, and the next visit starts with none.
    std::vector<std::pair<s32, bool>> Forget();
    // The party's log reset these maps (WorldLogHost::ResetMap: everyone on one died while
    // another member stood elsewhere): this member's kills on them are taken back, the held ones
    // too.
    std::vector<std::pair<s32, bool>> ForgetOn(std::span<const u32> maps);

private:
    struct Seen {
        s32 hp = 0;
        u64 instance = 0;
        bool untouched_boss = false; // see SeenCharacter
    };
    u64 current = 0;
    std::map<u32, Seen> seen;
    std::vector<u32> killed;
    std::map<u32, u32> killed_on;         // this visit's kills -> the map each died on
    std::map<u32, std::vector<u32>> held; // map -> kills of an earlier visit standing there
};
std::string EncodeEchoUpdate(std::string_view member, s64 since,
                             std::span<const KillEchoes> echoes);
std::optional<EchoBoardAnswer> ParseEchoBoard(std::string_view body);

// The multiplayer walls (common event 7600, from the maps' event files): the wall object and the
// fog SFX the event shows while the game is in multiplayer. All maps' walls: a neighbouring map's
// walls stand while the player is still on its own map (Cathedral Ward's stairs to the woods).
struct MultiplayerWall {
    s32 object = 0;
    s32 sfx = 0;
};
std::vector<MultiplayerWall> MultiplayerWalls();

// The summoned guest's special effect that cuts its max HP (SpEffectParam row 9006: max HP rate
// 0.7 at +0x10). A party guest keeps its full HP.
constexpr s32 PhantomHpEffect = 9006;
constexpr u64 SpEffectMaxHpRateOffset = 0x10;
// Level sync: a player whose level is above the host's band (host level * (1 + rate) + add,
// from the online settings) gets special effect 9200 (0x01507DDC), and every character whose
// effects include it has its max HP, stamina and attack scaled to the host's level through the
// CalcCorrectGraph rows 120-124 (0x01582940). A party guest keeps its own level.
constexpr s32 LevelSyncEffect = 9200;
bool LevelSyncsGuest(bool in_party_session, bool above_host_band);
// A party guest's lost echoes: joining the host clears GameDataMan +0x28 ("has a bloodstain")
// but keeps the record (echoes +0x38, block +0x3C), so the enemy holding them gives nothing
// back. The mark is set again while the record still holds echoes.
bool RestoresBloodstainMark(bool party_guest, bool mark_before, bool mark_now, s32 echoes,
                            u32 block);
// A load shows a player's own bloodstain only in its own world (0x017C243F tests GlobalState
// +0x1592), so a guest joined in the host's world never saw its stain there (Kaniel 2026-10-11).
// Its game shows it (0x01868A50, as that load does) once loaded, while it has a stain on the map
// it stands on (`record_block`, the record's +0x3C) that is not shown (`shown`: the stain's
// marker exists), up to GuestBloodstainTries times a visit.
constexpr int GuestBloodstainTries = 3;
bool ShowsGuestBloodstain(bool party_guest, bool in_session, bool loading, bool has_stain,
                          s32 echoes, u32 record_block, u32 map, bool shown, int tries);
// Game objects' vtables (01.09 eboot offsets, what each class's constructor stores). The co-op
// code calls a game function with an object only when the object's vtable is one the function
// takes (HasVtable), so a wrong or stale pointer is skipped instead of crashing the game
// (2026-10-11: the bloodstain show given another object crashed two guests).
// Characters: every vtable with the character's AddSpEffect (+0x3F0), teleport (+0x5F8) and
// NpcParam (+0x168) virtuals. The last two are the players': their PlayerGameData virtual
// (+0x1C8) returns +0x3C0, the others' return nothing.
constexpr std::array<u64, 4> CharacterVtables{0x0538E2D0, 0x0538EBC0, 0x0538F310, 0x0538FA80};
constexpr std::array<u64, 2> PlayerVtables{0x0538F310, 0x0538FA80};
constexpr u64 EventFlagManVtable = 0x05320B60; // [0x0553B100], its flag store (0x013CF0E0)
constexpr u64 MapItemManVtable = 0x0532D850;   // [0x0553D6E0], built by 0x017D2960
constexpr u64 BloodstainVtable = 0x0532EA40;   // the own stain, built by 0x018675B0
// Whether `vtable` (an address read from an object) is one of `vtables` (eboot offsets) in the
// game loaded at `image_base`.
bool IsGameVtable(u64 vtable, u64 image_base, std::span<const u64> vtables);
// A refused call is logged (a wrong pointer, or a list above missing a class, which would
// otherwise turn a feature off without a word), once per call site and vtable seen and at most
// MaxLogged times in all, so it can never fill the log.
class RefusedCallLog {
public:
    static constexpr std::size_t MaxLogged = 32;
    // True the first time `site` refuses an object with `vtable`, while there is room.
    bool FirstTime(std::string_view site, u64 vtable);

private:
    std::set<std::pair<std::string, u64>> logged;
};
// A seen vtable for the log: "eboot+0x..." inside the game's image, else the address.
std::string DescribeVtable(u64 vtable, u64 image_base, u64 image_size);
// Friendly NPCs on a summoned guest's side: character creation (0x018E4FB6, 0x018D8934) turns an
// NPC whose NpcParam has isChangeWanderGhost into a wandering ghost (type 4, the ghost model at
// +0x10C), drawn see-through. A party guest sees them as the host does.
constexpr u64 NpcWanderGhostFlagOffset = 0x145;
constexpr u32 NpcWanderGhostFlag = 0x80;
bool NpcTurnsIntoWanderGhost(bool in_party_session, bool param_says_wander_ghost);
// Map items (MapItemMan, 0x017D-0x017E): a summoned guest (matching state at CSMultiPlayMan
// +0x124 is 6, or the session's joined id at +0x98 is not -1) creates no map items
// (0x017DC3CF) and may pick one up only while GlobalState +0x1592 is set (0x017D6831,
// 0x017D6AEF). A party guest reads those checks from a stand-in that answers like a solo
// player, so each player sees and picks up its own copy. It does so whenever a party is
// configured: the guest's map reload for a join runs before the matching state reads as a
// session, and a configured party only ever joins its own members.
constexpr u64 MatchingStateOffset = 0x124;
constexpr u64 JoinedSessionOffset = 0x98;
constexpr u64 GuestItemPickupAllowedOffset = 0x1592;
const std::array<u8, GuestItemPickupAllowedOffset + 8>& SoloMapItemStandIn();
bool GuestHandlesMapItemsAlone(bool party_configured);
// The boss item: 2003:36 (AwardItemLot even for a client, 0x0132E4A0) calls the award
// 0x017DDC50(item man, lot, 0), returning to 0x0132E4B5. For a party guest it lands inside the
// boss-clear sequence and its dialog leaves the guest unable to confirm anything, so it is held
// and given DeferredBossItemDelay later.
constexpr u64 ClientItemLotAwardReturn = 0x0132E4B5;
constexpr auto DeferredBossItemDelay = std::chrono::seconds{8};
bool DefersItemLotAward(bool party_configured, bool summoned_guest, u64 return_offset);
// NPC gifts: events that wait for an NPC talk's trigger flag and award an item lot, but end
// unless the local player is alive, so never for a party guest (common.emevd 9100 and 9040,
// started per gift by its event 0 with (trigger, lot); the maps' own gift events). A guest's own
// talk setting a trigger (talk command 11) gets that lot from us instead, and then what the event
// does once it has given it (Kaniel 2026-10-13: each player its own talk and gift, every NPC).
enum class NpcGiftAfter : u8 {
    ClearTrigger, // gives again whenever the talk sets it (9100, Adeline's 13501940)
    KeepTrigger,  // once: the trigger stays on and ends the event (9040, 12700906, 13600943)
    SetDoneFlag,  // once: the event ends on its own flag (event id, or id + slot)
};
struct NpcGift {
    s32 trigger;
    s32 lot;
    NpcGiftAfter after;
    s32 done_flag = 0; // SetDoneFlag's
};
inline constexpr std::array NpcGifts{
    // common event 9100
    NpcGift{72410359, 24060, NpcGiftAfter::ClearTrigger}, // Blue Elixir (Central Yharnam)
    NpcGift{72410310, 23000, NpcGiftAfter::ClearTrigger}, // Iosefka's Blood Vial
    NpcGift{72400398, 32020, NpcGiftAfter::ClearTrigger}, // Blood of Adella
    NpcGift{72400489, 28000, NpcGiftAfter::ClearTrigger}, // Blood of Arianna
    NpcGift{9043, 17010, NpcGiftAfter::ClearTrigger},     // Queenly Flesh (set by an event)
    // common event 9040
    NpcGift{9042, 17000, NpcGiftAfter::KeepTrigger}, // Annalise's rune
    NpcGift{9045, 22000, NpcGiftAfter::KeepTrigger}, // Pebble
    NpcGift{9046, 23000, NpcGiftAfter::KeepTrigger},
    NpcGift{9047, 24000, NpcGiftAfter::KeepTrigger}, // Numbing Mist ...
    NpcGift{9048, 24010, NpcGiftAfter::KeepTrigger},
    NpcGift{9049, 24020, NpcGiftAfter::KeepTrigger},
    NpcGift{9050, 24030, NpcGiftAfter::KeepTrigger},
    NpcGift{9051, 24040, NpcGiftAfter::KeepTrigger}, // Beast Blood Pellet
    NpcGift{9052, 24050, NpcGiftAfter::KeepTrigger}, // Lead Elixir
    NpcGift{9054, 26000, NpcGiftAfter::KeepTrigger}, // Sedative ...
    NpcGift{9055, 26010, NpcGiftAfter::KeepTrigger},
    NpcGift{9056, 26020, NpcGiftAfter::KeepTrigger},
    NpcGift{9057, 26030, NpcGiftAfter::KeepTrigger},
    NpcGift{9058, 27000, NpcGiftAfter::KeepTrigger}, // Pungent Blood Cocktail
    NpcGift{9059, 27010, NpcGiftAfter::KeepTrigger}, // Beast Blood Pellet ...
    NpcGift{9060, 27020, NpcGiftAfter::KeepTrigger},
    NpcGift{9061, 27030, NpcGiftAfter::KeepTrigger},
    NpcGift{9062, 27040, NpcGiftAfter::KeepTrigger},
    NpcGift{9063, 27050, NpcGiftAfter::KeepTrigger},
    NpcGift{9064, 27060, NpcGiftAfter::KeepTrigger},
    NpcGift{9066, 29000, NpcGiftAfter::KeepTrigger}, // Gilbert's Flamesprayer
    NpcGift{9067, 30000, NpcGiftAfter::KeepTrigger}, // the little girl's Tiny Music Box
    NpcGift{9069, 32000, NpcGiftAfter::KeepTrigger}, // Madman's Knowledge
    NpcGift{9071, 33000, NpcGiftAfter::KeepTrigger}, // Djura's Powder Keg Hunter Badge
    NpcGift{9072, 34000, NpcGiftAfter::KeepTrigger}, // Fire Paper
    NpcGift{9073, 34010, NpcGiftAfter::KeepTrigger}, // Wheel Hunter Badge
    NpcGift{9074, 35000, NpcGiftAfter::KeepTrigger}, // Eileen's Bold Hunter's Mark
    NpcGift{9075, 35010, NpcGiftAfter::KeepTrigger}, // Eileen's rune
    NpcGift{9076, 39000, NpcGiftAfter::KeepTrigger}, // Tonsil Stone
    NpcGift{9077, 39010, NpcGiftAfter::KeepTrigger}, // a rune (Nightmare Frontier)
    // the maps' gift events
    NpcGift{72100112, 14000, NpcGiftAfter::SetDoneFlag, 12100123}, // the Doll's Tear Stone
    NpcGift{72400441, 37000, NpcGiftAfter::SetDoneFlag, 12400403}, // Madman's Knowledge
    NpcGift{72700321, 43120, NpcGiftAfter::KeepTrigger},           // Valtr's rune
    NpcGift{72700322, 43130, NpcGiftAfter::KeepTrigger},           // Valtr's League Staff
    NpcGift{73400424, 43211, NpcGiftAfter::SetDoneFlag, 13400980}, // Holy Moonlight Sword
    NpcGift{73500321, 43010, NpcGiftAfter::SetDoneFlag, 13501921}, // Balcony Key
    NpcGift{73500322, 43020, NpcGiftAfter::SetDoneFlag, 13501922}, // a rune (Research Hall)
    NpcGift{73500320, 43000, NpcGiftAfter::ClearTrigger},          // Blood of Adeline
    NpcGift{73600300, 43600, NpcGiftAfter::KeepTrigger},           // Accursed Brew
};
std::optional<NpcGift> NpcGiftFor(s32 trigger);
std::optional<s32> NpcGiftLot(s32 trigger);
bool GivesGuestNpcGift(bool in_party_session, bool summoned_guest, s32 flag);
// Held this long after the talk sets the trigger, as for the boss items: the gift's dialog comes
// once the talk has moved on.
constexpr auto NpcGiftDelay = std::chrono::seconds{2};
// The menu scripts' "am I a summoned client" condition (0x01A13FD1, matching state 6 or a
// joined session): with a party configured the answer is "no", like the item dialog's host.
bool MenuSeesAClient(bool party_configured, bool summoned_client);
// Map item packets between players (channel 0x2C, read at 0x017D33D8): header word 1, then the
// message kind. The host's item sync clears the guest's map items (0x017D3498-0x017D34A8), so
// in a configured party every such packet reads as empty and each player's items stay its own.
constexpr u32 MapItemPacketChannel = 0x2C;
u32 MapItemPacketHeaderSeen(u32 header, bool party_configured);
// An 8-byte VEX instruction reading [rip+disp32] (`vmovss`, `vmulss`, `vucomiss`) at `address`
// changed to read from `target` instead.
std::array<u8, 8> RetargetRipLoad(std::span<const u8, 8> original, u64 address, u64 target);
// A host's summon in flight (an entry of the summon manager's list at +0x1C0, one per claimed
// sign) takes a summon slot until the game sees the summoned player arrive, which a party join
// never shows it: the summon manager's update (0x01872360) lets the entry go only once it is
// 55 s old (180 s while its +0x0C is set, as at 30 FPS). Until then the first guest fills a
// second slot and the host's game stops searching (SosStatus 0x0186FE40), so a third member
// waited 180 s (rig 2026-10-12). In a configured party both times read this 15.0 among the
// game's constants instead (feature "summon slot hold", RetargetRipLoad).
constexpr u64 PartySummonSlotHoldConstant = 0x049278C4;
constexpr float PartySummonSlotHoldSeconds = 15.0f;
// A loaded PARAM file: the type name at +0x0C, the row count (u16) at +0x0A and sorted 24-byte
// row entries (id, padding, data offset) at +0x40.
bool IsParamOfType(std::span<const u8> param, std::string_view type);
std::optional<u64> FindParamRow(std::span<const u8> param, s32 id);
// An enemy's one-time drop: NpcParam +0x2C is its item lot, ItemLotParam +0x80 the flag a lot
// that drops once sets (0: a random drop). A member's kill gives the others that lot while their
// flag is off (Kaniel 2026-10-12: only the host got a Wandering Nightmare's shards).
constexpr u64 NpcItemLotOffset = 0x2C;
constexpr u64 ItemLotFlagOffset = 0x80;
bool GetsMemberKillDrop(bool in_session, bool member_kill, s32 lot_flag, bool flag_on);
// And a guest's own kill (`own_kill_as_guest`): its game makes no one-time drop (Ada's hag,
// 2026-10-13).
bool GetsKillDrop(bool in_session, bool member_kill, bool own_kill_as_guest, s32 lot_flag,
                  bool flag_on);
// A member's kill on this player's copy (hidden, or the game's kill near): only while the copy is
// alive (`hp`, its HP now: 0 in its own death animation, before the death flags are set).
bool AppliesMemberKill(bool member_killed, s32 hp);
// A copy the world made again in place (no load: new characters for the same handles) is not
// whole for a while: the game's kill on one made 0.1 s before crashed in its death step (Kaniel's
// play log, 2026-10-18 16:16). A member's kill waits until the copy has been seen in the world
// (`seen_ms`: when this game first saw that character) for NewCopySettlesMs.
constexpr s64 NewCopySettlesMs = 2'000;
bool CopyJustMade(s64 now_ms, s64 seen_ms);

// Harder enemies (SHADPS4_BLOODBORNE_HARDER_ENEMIES=<hp>,<damage>,<poise>, e.g. "3,1.5,3"):
// every NpcParam row's HP and poise and every AtkParam_Npc row's damage are multiplied once,
// in the loaded tables, before any enemy is created. Bosses get the game's own co-op HP effect
// (7500-7502) on top. Each multiplier is 1 to 10; "", "0", "off" or all ones mean vanilla.
struct HarderEnemies {
    float hp = 1.0f;
    float damage = 1.0f;
    float poise = 1.0f;
};
std::optional<HarderEnemies> ParseHarderEnemies(std::string_view setting);
// NpcParam (NPC_PARAM_ST, 0x184-byte rows): hp (u32) and superArmorDurability (s16, poise).
constexpr u64 NpcHpOffset = 0x20;
constexpr u64 NpcPoiseOffset = 0x118;
// AtkParam_Npc (ATK_PARAM_ST): atkPhys, atkMag, atkFire, atkThun (u16 each). AtkParam_Pc has
// the same type; the enemies' table is the one with Father Gascoigne's first axe swing.
constexpr u64 AtkPhysOffset = 0x50;
constexpr s32 NpcAttackMarkerRow = 271000;
u32 HarderEnemyHp(u32 hp, float rate);
// Most enemies have no poise (0: any hit staggers), so a multiplier gets a floor too: 15, or 50
// for a boss-sized HP (1000+), times the multiplier. 9999 means never staggered and stays.
s16 HarderEnemyPoise(s16 poise, u32 hp, float rate);
u16 HarderEnemyAttack(u16 attack, float rate);
// The rows changed (each row's data once, rows outside the table skipped); 0 for another table
// (for attacks, also for AtkParam_Pc).
std::size_t MakeNpcParamHarder(std::span<u8> param, const HarderEnemies& harder);
std::size_t MakeNpcAttacksHarder(std::span<u8> param, float damage);
// The game's co-op boss HP bonus (SpEffect 7500-7503, max HP x1.5/x2/x2.5/x3, given at a boss
// start) would stack on the HP multiplier, so with harder enemies its max HP rate becomes 1 and
// the HP multiplier is the only one. The rows' status resistance bonus stays. Returns the rows
// changed.
constexpr std::array<s32, 4> CoopBossHpEffects{7500, 7501, 7502, 7503};
std::size_t RemoveCoopBossHpBonus(std::span<u8> sp_effect_param);
// Kaniel 2026-10-25 (the enemy scaling rule): those rows' max HP rates as the table holds them,
// and put back (a team scaled down to the normal game gets the bonus again).
std::vector<std::pair<s32, float>> CoopBossHpRates(std::span<const u8> sp_effect_param);
void SetCoopBossHpRates(std::span<u8> sp_effect_param,
                        std::span<const std::pair<s32, float>> rates);

// Multiplayer UI polish (Kaniel 2026-10-11), all at runtime for a configured party:
// - The messages say Host and Guest. The game reads its text from MsgRepository (pointer at
//   0x05540300; lookup 0x00F6F340: repository, 0, category, id -> UTF-16 text in the loaded
//   message file, or null). The network banners look in category 0x65 first, then 30 (event
//   text); the name plates show 30:107000 (host) and 30:107100 (guest), the ids the role table
//   at 0x0553D750 (0x80 bytes per role, +0x28) gives them. Each replacement is written over the
//   English original in the loaded file; any other text (another language) stays.
constexpr u64 MsgRepositoryPointerOffset = 0x05540300;
constexpr u64 MessageLookupOffset = 0x00F6F340;
constexpr u32 EventTextCategory = 30;
constexpr u32 EventTextOverrideCategory = 0x65;
constexpr u32 InGameMenuCategory = 70;
constexpr u32 DialogCategory = 78;
constexpr u32 SpDialogCategory = 204;
struct PolishedMessage {
    u32 category;
    s32 id;
    std::u16string_view original;
    std::u16string_view replacement; // never longer than the original
};
std::span<const PolishedMessage> PolishedMessages();
// The text to write over `current` (message `id` of `category`), when `current` is still the
// English original.
std::optional<std::u16string_view> PolishedText(u32 category, s32 id, std::u16string_view current);
// - Players by their characters' names (website shots 2026-10-19: "Joined rig_host."): the name
//   the game puts in its texts and nameplates (0x01AE8680: out, player) is the player's online id
//   (player +0x3D8) when [0x05562878] +0xD10 or GameDataMan's [+0x48] +0x11 is set
//   (`jne` at 0x01AE8713, `je` at 0x01AE8727), else its character's name (PlayerGameData
//   +0x5C0's holder). In a party both tests go: always the character's name.
constexpr u64 PlayerNameForTextOffset = 0x01AE8680;
// - No bell indicators: the co-op bells' resonance effects show a status icon by the HP bar
//   (SpEffectParam iconId, +0x00: 9003 8801, 9004 8813, 9008 8802, 9009 8814). The icon goes;
//   the effects, which the matching runs on, stay. Returns the rows changed.
constexpr u64 SpEffectIconOffset = 0x00;
constexpr std::array<s32, 4> CoopBellIconEffects{9003, 9004, 9008, 9009};
std::size_t RemoveBellIcons(std::span<u8> sp_effect_param);
// - No "?ActionEventInfo?" over a guest (website shots 2026-10-19): a nameplate's role line is the
//   event text of the role table at 0x0553D750 (0x80 bytes per role, the player's role in
//   PlayerGameData +0x5D8, read by 0x018FE420): +0x28, the text id 0x01AE8490 looks up (category
//   0x65, then 30). The table's constructor (0x0187A1D0) gives the co-operator role 7 its texts
//   (+0x28 107100, `mov dword [rax + 0x3A8]` at 0x0187AB15) but leaves role 6 without any, and a
//   party guest can play as role 6 (rig 2026-10-19: one guest of two, on everyone's screen); the
//   lookup of id 0 shows the event text's missing-text mark. Role 7's other texts (+0x24 to
//   +0x4C: its join, death and leave messages, the summon message at +0x30) are missing the same
//   way. Role 6 gets role 7's texts wherever it has none. Returns the texts changed.
constexpr u64 RoleTableOffset = 0x0553D750;
constexpr std::size_t RoleTableEntrySize = 0x80;
constexpr std::size_t RoleTableSize = 0x22 * RoleTableEntrySize;
constexpr std::size_t RoleNameTextAt = 0x28;
constexpr std::size_t RoleTextsAt = 0x24;
constexpr std::size_t RoleTextsEnd = 0x50;
constexpr std::size_t UnnamedGuestRole = 6;
constexpr std::size_t GuestRole = 7;
std::size_t NameUnnamedGuestRole(std::span<u8, RoleTableSize> roles);
// - No "Discovered the chime of a ..." banner right before the join's own banner: the summon's
//   enter direction (CSEnterleaveDirectionStep) shows it with the network banner (0x01BFFE80,
//   message id, 0) for the arriving member (STEP_C2H_Init_forSelfPrologue, 0x0196B4C9) and for
//   the members it arrives to (STEP_C2H_Init_forOthersPrologue, 0x0196B9F4). Both calls become
//   5-byte nops; neither caller uses the result.
constexpr u64 ShowNetworkBannerOffset = 0x01BFFE80;
const std::vector<CodePatch>& QuietSummonBannerPatches();
// Kaniel 2026-10-16: no multiplayer banners at all in a party (joins, leaves, summons, the
// chimes): the network banner (0x01BFFE80, message id in edi) shows none of them.
constexpr std::array<u8, 8> ShowNetworkBannerPrologue{0x55, 0x48, 0x89, 0xE5,
                                                      0x41, 0x57, 0x41, 0x56};
// Kaniel 2026-10-23: the item-received popup ("Oedon Tomb Key 1 / OK") and its pickup sound
// (menu sound 300) come from one function: the give (GiveItems, 0x017D89F0) calls it for each
// item it put in the inventory (0x017D9348) and each it sent to the storage (0x017D94C7) with
// (queue [0x0553B1C0], &handle, category, id, count, to storage); it pushes the item on the
// popup's queue. Its result is never read. Skipped while SilentItemGets lives (see there).
constexpr u64 ItemGetQueuedOffset = 0x016D8850;
constexpr std::array<u8, 8> ItemGetQueuedPrologue{0x55, 0x48, 0x89, 0xE5, 0x41, 0x57, 0x41, 0x56};
// The game's bullet creation (CreateBullet: rdi the bullet manager, rsi the request: +0x00 the
// owner's handle, +0x04 the bullet's BulletParam id). No enemy bullet is ever sent: an enemy's
// (EnemyIns vtable +0x1A8 returns 0, so the request's network bit 0x10 stays off) and an event's
// (2003:5 ShootBullet) are made by each game from its own copy. The rig's bullet tracer logs them.
constexpr u64 CreateBulletOffset = 0x01A03620;
constexpr u64 BulletRequestOwnerOffset = 0x00;
constexpr u64 BulletRequestIdOffset = 0x04;
constexpr std::array<u8, 8> CreateBulletPrologue{0x55, 0x48, 0x89, 0xE5, 0x41, 0x57, 0x41, 0x56};
// Kaniel 2026-10-18: a guest whose connection dropped and came back (the party went on) still
// had "Lost connection to the network. Returning to title menu." (FMG 204, 4701) with its OK
// button over the game. The network script's OnLanCutError (0x01387130, a Lua C function: the
// error dialog with that text and the title return) does nothing in a party: the server keeps
// the session, and a member alone plays on alone.
constexpr u64 LanCutErrorOffset = 0x01387130;
constexpr std::array<u8, 6> LanCutErrorPrologue{0x55, 0x48, 0x89, 0xE5, 0x41, 0x56};

// Auto-rejoin for a configured party: whenever this player is back in its own world with no
// session and no active search, start the role's search again (host: Beckoning, guest: Small
// Resonant) once the world has been stable for a while.
// OnLeave_Limit (event 4046, 0x0138A4A0) resets a player into its own world and warps it there,
// meant for a white phantom whose host is gone. Whether a party member skips it (see
// StayWhenTheHostLeaves): a phantom only while the session has `other_members` besides it (else
// it goes home and resumes where it was); a live player (the host, or a guest after its own
// trip) always, as it is in its own world already.
bool SkipsLeaveLimit(s32 character_type, size_t other_members);

// The placement a guest's sign gets with its claim (for a party guest, its own spot: it joins in
// place) is for that one join. The pre-match warp and the summon handoff move the guest to it, so
// kept after the session it brought, it warped a guest that had travelled on and then lost its
// host back to where it had joined. Spent once that session is over: `session_over` is matching
// idle with no reload or stage transition pending.
class JoinPlacementLife {
public:
    void Received() {
        joined = false;
    }
    // True once, on the tick the placement is spent (forget it then).
    bool Spent(s32 matching_state, bool session_over) {
        if (matching_state == MatchingStateSummonedGuest) {
            joined = true;
            return false;
        }
        if (!joined || !session_over) {
            return false;
        }
        joined = false;
        return true;
    }

private:
    bool joined = false;
};

class PartyController {
public:
    using Clock = std::chrono::steady_clock;
    static constexpr u32 StableTicksBeforeSearch = 20;
    static constexpr Clock::duration RetryDelay = std::chrono::seconds{15};
    // A searching game posts /summon_messenger/get every 5 s (shadNet, rig 2026-10-12: 5.0 to
    // 6.6 s apart at 30 FPS). A bell is stale when neither it went on nor the game searched for
    // two of those and a frame hitch's slack: one late or lost post never ends a search.
    static constexpr Clock::duration SummonSearchInterval = std::chrono::seconds{5};
    static constexpr Clock::duration StaleSearchAfter =
        2 * SummonSearchInterval + std::chrono::seconds{2};

    // `waiting`: the team lobby (phase 5) says the role later (Assign); until then it rings
    // nothing.
    explicit PartyController(PartyRole role, bool waiting = false) : role{role}, waiting{waiting} {}

    s32 SearchEffectId() const {
        return role.load() == PartyRole::Host ? BeckoningBellEffect : SmallResonantBellEffect;
    }
    bool Waiting() const {
        return waiting.load();
    }
    // The team's Start: this game hosts the team's sessions or joins them.
    void Assign(PartyRole assigned) {
        role.store(assigned);
        waiting.store(false);
    }
    // A save played alone after a team's (the title again): it rings nothing until a team's
    // Start assigns a role again.
    void Wait() {
        waiting.store(true);
    }
    PartyAction Tick(const PartyObservation& observation, Clock::time_point now);

private:
    // The stale bell (see Tick) for a host with room: what to do, or nothing to leave it to the
    // usual search rules.
    std::optional<PartyAction> RingAgain(const PartyObservation& observation,
                                         Clock::time_point now);

    std::atomic<PartyRole> role;
    std::atomic<bool> waiting;
    u32 stable_ticks = 0;
    std::optional<Clock::time_point> last_search;
    /// Since when a hosting bell with room for more has been on (see Tick).
    std::optional<Clock::time_point> ringing_since;
    /// When a stale bell was ended (EndSearch); it rings again once the effect is off.
    std::optional<Clock::time_point> ending_since;
};

// Autoplay (SHADPS4_BLOODBORNE_AUTOPLAY=<script file>), for tests with no one at the pad: the
// emulator presses Cross on the title screen until the player is in the world, then runs the
// script, one command per line ("<verb> <args...>", # starts a comment).
struct AutoplayCommand {
    std::string verb;
    std::vector<std::string> args;
    bool operator==(const AutoplayCommand&) const = default;
};
std::vector<AutoplayCommand> ParseAutoplayScript(std::string_view text);
// The pad button a script names (cross, circle, square, triangle, options, up, down, left, right,
// l1, r1, l2, r2, l3, r3), 0 if none.
u32 AutoplayButton(std::string_view name);
// Whether Cross is down `ms` after the title screen's autoplay began.
bool TitlePressesCross(u64 ms);
// shot <name>: a picture of the port's next frame, overlays and all (the rig's look at the party
// HUD). The rig names a request file (BB_SHOT_TRIGGER); the step writes the picture's path into
// it and the port's presenter takes it: <the request's folder>/shots/<name>.png. A name is 1 to 40
// letters, digits, '-' or '_'; nothing for another, or without a request file.
std::optional<std::string> AutoplayShotPath(std::string_view request, std::string_view name);
// sync <name>: the go file the rig writes once every game of the suite reached that sync step:
// <the script's folder>/sync/<name>. A name is 1 to 40 letters, digits, '-' or '_'.
std::optional<std::string> AutoplaySyncPath(std::string_view script, std::string_view name);
// Where the player was over the last few seconds, a point every half second (TickPartyController
// notes it; logged when the map changes, for the rig to replay a border crossing with glide).
class PositionTrail {
public:
    static constexpr size_t Kept = 20;
    static constexpr u64 IntervalMs = 500;
    void Note(u64 ms, const LampPosition& at);
    const std::vector<LampPosition>& Points() const {
        return points;
    }
    void Clear() {
        points.clear();
        last_ms.reset();
    }

private:
    std::vector<LampPosition> points;
    std::optional<u64> last_ms;
};
// A trail as glide's arguments: "x y z x y z ..." (one decimal).
std::string FormatTrail(const std::vector<LampPosition>& points);
// glide <x y z>...: where the player is `elapsed` seconds along `path` (from its first point) at
// walking pace; nothing once it is at the end.
constexpr float GlideMetersPerSecond = 4.0f;
std::optional<LampPosition> GlidePoint(const std::vector<LampPosition>& path, double elapsed);
// pickup: whether the player is no longer at the map item it was put on (fell through ground
// not loaded yet, or pushed out of the item button's reach), so it is put back on it.
bool PickupPlayerOffTheItem(const LampPosition& player, const LampPosition& item);
// keep-going on: where a script goes on after its step at `failed` failed: the next "chapter"
// command after it, nothing if none is left.
std::optional<size_t> NextAutoplayChapter(const std::vector<AutoplayCommand>& script,
                                          size_t failed);
// chapter <name> <seconds>: how long the chapter may take (the rig's time budget), nothing for
// another command or a chapter without one.
std::optional<double> AutoplayChapterBudget(const AutoplayCommand& command);
// keys <token>... / title-keys <token>...: the BB_MENU_KEYS_FILE text the port's overlay reads
// (toggle up down left right enter back l1 r1 kdown kup), nothing for an unknown token or none.
std::optional<std::string> AutoplayMenuKeys(const std::vector<std::string>& tokens);
// The game's own kill (what an event's ForceCharacterDeath does): HP to 0, the dead flag
// (+0x1E3 bit 0), then the death step (0x018BD9D0). `void (character)`.
constexpr u64 KillCharacterOffset = 0x018BF9A0;
constexpr u64 DeathStepOffset = 0x018BD9D0;
// Save backups (Kaniel 2026-10-11): every few minutes the saves are copied to
// user/save-backups/<yyyymmdd-hhmmss>/, the newest few kept. SHADPS4_BLOODBORNE_SAVE_BACKUPS =
// "<minutes>,<kept>" (default 10 minutes, 20 kept; "off" or "0" for none). A backup holds the
// port's user/savedata, the emulator's user/home/<user id>/savedata and the party's files
// (user/bloodborne_party_*.txt), at the same paths under it.
struct SaveBackups {
    std::chrono::minutes every{10};
    std::size_t kept = 20;
    bool operator==(const SaveBackups&) const = default;
};
std::optional<SaveBackups> ParseSaveBackups(std::string_view setting);
constexpr const char* SaveBackupsFolder = "save-backups";
std::string SaveBackupName(std::chrono::sys_seconds when);
// A backup of `user_dir` named `name`: its path, or nothing when there are no saves, when they
// are the same as the newest backup's, or when they changed while being copied (the game was
// writing; the next try gets them).
std::optional<std::filesystem::path> BackUpSaves(const std::filesystem::path& user_dir,
                                                 std::string_view name);
// The launcher's own backups (before each PLAY, before a restore) hold this file; the game's
// pruning leaves them to the launcher, so a long session cannot push out the save it began with.
constexpr const char* LauncherBackupNote = "launcher.txt";
// Deletes all but the newest `kept` of the game's backups; returns how many went.
std::size_t PruneSaveBackups(const std::filesystem::path& user_dir, std::size_t kept);

// The co-op trace (docs/WORLD_SYNC.md, Stage B): every co-op game writes
// user/log/coop-trace-<local time>-<process>.txt, one record per line, "<UTC ms> <kind>
// <key>=<value> ...", each value escaped (TraceEscape) so a line has no spaces or line ends of
// its own. The kinds:
//   trace                the file's first line: version, role, process
//   p2p-out, p2p-in      a co-op frame: type, peer (address:port), payload (hex); in: from-peer
//   board-out, board-in  a board post and its answer: endpoint, status, size, body (at most
//                        TraceBodyCap of it; "same=1" and no body when it is the endpoint's last)
//   life                 event: each OnLifecycle (wipe, trip, leave, join, new character, member
//                        left) with its session epoch; sent-home, slot-lists-forgotten,
//                        peer-join, peer-leave
//   place                a change of place (PlaceTracker): from, to and the cause
//   kills-in             VisitKills's inputs, when they change
//   hp-in                SharedDamage's inputs, every round while they matter
//   shown-in             ShownLikeTheHost's inputs, when they change
//   kill-apply           the other members' kills this game would apply now (AppliesMemberKill)
//   wlog-in              the world log's inputs that changed it (WorldLogTraceFields): the host's
//                        starts and the proposals it took, a member's batches and snapshots
//   wlog-path            the path the kills take: path (log or board) and why
//   trace-dropped        lines lost to a full queue (TraceQueue)
//   net-out, net-in      a game packet sent or queued for this game, only with
//                        SHADPS4_BLOODBORNE_PACKET_TRACE on (see PacketTraceMode)
//   link                 the server link's events (the port's, net_link.h):
//                        event (welcome, refused, differs, quiet, back, member-quiet/-back/-gone,
//                        rtt)
//                        and detail
// An "-in" record leaves out what did not change since its kind's last record, and its lists
// only name what came, changed or went (TraceListDelta). SHADPS4_BLOODBORNE_TRACE=0 turns it off.
// 2: the world log's records. 3: the game's packets.
constexpr int CoopTraceVersion = 3;
constexpr std::size_t TraceBodyCap = 8 * 1024;
constexpr std::uintmax_t TraceFileCap = 50 * 1024 * 1024;
constexpr std::size_t TraceFilesKept = 5;
constexpr std::size_t TraceQueueCap = 20'000; // lines
constexpr const char* CoopTracePrefix = "coop-trace-";
bool CoopTraceEnabled(const char* setting);
struct TraceRecord {
    s64 utc_ms = 0;
    std::string kind;
    std::vector<std::pair<std::string, std::string>> fields; // unescaped, in order
    std::optional<std::string> Field(std::string_view key) const;
    bool operator==(const TraceRecord&) const = default;
};
std::string TraceEscape(std::string_view text);
std::optional<std::string> TraceUnescape(std::string_view text);
std::string EncodeTraceRecord(const TraceRecord& record);
std::optional<TraceRecord> ParseTraceRecord(std::string_view line);
// A board body on a record: its size and at most `cap` bytes of it (cut when body is shorter).
void AddTraceBody(TraceRecord& record, std::string_view body, std::size_t cap = TraceBodyCap);
std::string TraceHex(std::span<const u8> bytes);
std::optional<std::vector<u8>> ParseTraceHex(std::string_view hex);
// Handles as "a,b,c" (hex), and back.
std::string TraceHandles(std::span<const u32> handles);
std::optional<std::vector<u32>> ParseTraceHandles(std::string_view text);
// List entries, each "<key>:<rest>" (the key in hex): a character as VisitKills sees it (alive or
// not, which is all it reads of the HP; a 5th field of flags when any is set: 1 an untouched
// boss-sized one, 2 another member's kill), and a copy as SharedDamage sees it ("-" for no own
// hits).
std::string TraceToken(const SeenCharacter& character);
std::optional<SeenCharacter> ParseSeenToken(std::string_view token);
std::string TraceToken(const EnemyCopy& copy);
std::optional<EnemyCopy> ParseCopyToken(std::string_view token);
// Another member's HP report: "<source>:<handle>/<hp>/<max>+..." (hex source and handles).
std::string TraceToken(const EnemyHpReport& report);
std::optional<EnemyHpReport> ParseReportToken(std::string_view token);
// A list sampled on change: what came or changed ("<token>") and what went ("-<key>"), comma
// separated, "" for no change; after a Reset the whole list ("=<token>,..."). Apply rebuilds it.
class TraceListDelta {
public:
    std::string Next(std::span<const std::string> tokens);
    void Reset() {
        whole = true;
        last.clear();
    }

private:
    bool whole = true;
    std::vector<std::string> last;
};
bool ApplyTraceListDelta(std::vector<std::string>& list, std::string_view delta);
// An "-in" record's fields sampled on change: the values that changed since the last call, and
// the lists' deltas that are not empty. A new trace file (`file`) starts everything whole again.
class TraceSampler {
public:
    using Fields = std::vector<std::pair<std::string, std::string>>;
    Fields Changes(u32 file, const Fields& values,
                   const std::vector<std::pair<std::string, std::vector<std::string>>>& lists);

private:
    std::optional<u32> file;
    std::map<std::string, std::string> last;
    std::map<std::string, TraceListDelta> deltas;
};
// A change of place (see PlaceTracker) on a "place" record: each side's state, map (hex), visit
// and since, and the cause.
TraceSampler::Fields TracePlaceFields(const PlaceChange& change);
std::optional<PlaceChange> ParsePlaceFields(const TraceRecord& record);
// The world log's inputs on a "wlog-in" record: the role ("host" or "member"), what came ("start":
// EncodeWorldLogStart, "propose": a proposal, "entries": a batch, "snapshot": a snapshot part,
// "reset": nothing) and its payload, enough to replay WorldLogHost and WorldLogMember.
TraceSampler::Fields WorldLogTraceFields(std::string_view role, std::string_view op,
                                         std::span<const u8> payload);
// The lines waiting for the trace's writer thread. Push never waits for the writer: a full queue
// drops the line and counts it.
class TraceQueue {
public:
    explicit TraceQueue(std::size_t capacity) : capacity{capacity} {}
    bool Push(std::string line);
    // Waits up to `wait` for a line, then takes every waiting one.
    std::vector<std::string> Take(std::chrono::milliseconds wait);
    // How many were dropped since the last call.
    u64 TakeDropped();

private:
    std::size_t capacity;
    std::mutex mutex;
    std::condition_variable ready;
    std::vector<std::string> lines;
    u64 dropped = 0;
};
// "coop-trace-<yyyymmdd-hhmmss>-<process>.txt", named in local time like the save backups.
std::string CoopTraceFileName(std::chrono::sys_seconds local_when, u32 process);
// Deletes all but the newest `kept` traces in `log_dir`; returns how many went.
std::size_t PruneCoopTraces(const std::filesystem::path& log_dir, std::size_t kept);

// The game's own packets on the co-op trace (docs/SERVER_COOP_PLAN.md phase 0, no change to what
// the game does): every packet the game sends (0x017894E0, once it has passed its checks) and
// every one it queues for a member as it arrives (0x01790C20), on "net-out" / "net-in" records.
// A packet is {u32 channel, u32 size, payload}, channels 0..0x43, 0..6 unreliable.
// SHADPS4_BLOODBORNE_PACKET_TRACE: off unless "1"/"on" (the payloads of the channels not yet
// understood) or "full" (every payload); NativePayloadTraceCap bytes of a payload at most.
enum class PacketTraceMode : u8 { Off, On, Full };
PacketTraceMode ParsePacketTraceMode(const char* setting);
using BloodborneCoop::NativeChannelCount;
constexpr u32 NativePacketMaxBytes = 0x6400; // header and payload
constexpr std::size_t NativePayloadTraceCap = 512;
using BloodborneCoop::IsReliableNativeChannel;
// The channels whose payloads docs/NATIVE_NETCODE.md does not explain yet: the 0x60-byte enemy
// state record (0x29, 0x3D), the 0x14-byte one (0x41), 0x34 and 0x43.
bool NativePayloadUnknown(u32 channel);
// How many of a payload's `size` bytes a record carries in `mode`.
std::size_t TracedPayloadBytes(PacketTraceMode mode, u32 channel, std::size_t size);
struct NativePacketView {
    u32 channel = 0;
    std::span<const u8> payload;
};
// A packet as the game builds it, or nothing for a short or bad one.
std::optional<NativePacketView> DecodeNativePacket(std::span<const u8> bytes);
// A member's online id where the game keeps it (a SceNpId's handle: up to 16 letters, digits, '-'
// or '_', at least 3, NUL after), or nothing.
std::optional<std::string> NpOnlineIdAt(std::span<const u8> bytes);
struct NativePacketRecord {
    bool sent = false;
    u32 channel = 0;
    u32 size = 0;            // the whole payload's, whatever part of it `payload` holds
    std::string peer;        // the other member: online id, else its identity's address
    u32 map = 0;             // this game's map (packed)
    s32 sync_state = -1;     // NetSync +0x84 (4 in the world)
    s32 matching_state = -1; // SprjSessionManager +0x124 (3 host, 6 guest)
    std::vector<u8> payload;
    bool operator==(const NativePacketRecord&) const = default;
};
const char* NativePacketTraceKind(bool sent); // "net-out" / "net-in"
TraceSampler::Fields NativePacketFields(const NativePacketRecord& packet);
std::optional<NativePacketRecord> ParseNativePacketRecord(const TraceRecord& record);
// The hooks (see InstallNativePacketTrace): the send's `mov r12, [rbp - 0xA0]` as it hands the
// packet to the transport, and the receive's queue pushes, reliable (`mov rcx, [rbx + 0x30];
// mov rdi, [rcx + rax*8]`) and unreliable (`lea rsi, [rbp - 0x38]; mov rdi, r13`).
constexpr u64 PacketSendTraceOffset = 0x0178970D;
constexpr std::array<u8, 7> PacketSendTrace{0x4C, 0x8B, 0xA5, 0x60, 0xFF, 0xFF, 0xFF};
constexpr u64 ReliablePacketQueuedOffset = 0x01790E0E;
constexpr std::array<u8, 8> ReliablePacketQueued{0x48, 0x8B, 0x4B, 0x30, 0x48, 0x8B, 0x3C, 0xC1};
constexpr u64 UnreliablePacketQueuedOffset = 0x01790FCA;
constexpr std::array<u8, 7> UnreliablePacketQueued{0x48, 0x8D, 0x75, 0xC8, 0x4C, 0x89, 0xEF};

// The channel table (docs/SERVER_COOP_PLAN.md phase 1) decides where a packet goes at shadNet since
// phase 9 (coop/packet_routes.h): the server reads every game packet it relays and forwards it only
// to a teammate that needs it. A reliable packet it drops arrives as DroppedNativeChannel, which
// the game link hands to nobody. The game still decides what it sends at all: an Ours or Drop
// packet, or one the enemy owners keep from a follower, is "not sent" (the game then keeps the
// state its own way: rig 2026-10-15, a guest whose map item packet went out waited for an answer
// and could not pick the item up). Its hooks on both receive paths make the owner's enemies step
// on a hit.
using BloodborneCoop::ChannelPolicy;
using BloodborneCoop::ChannelPolicyName;
using BloodborneCoop::DroppedNativeChannel;
using BloodborneCoop::NativeChannelPolicy;
// The hooks (see InstallChannelTable): the send's prologue (`push rbp; mov rbp, rsp; push r15`),
// answering 0 (not sent) for a packet our link sent elsewhere; the reliable receive's channel check
// (`movsxd rax, [rbx + 0x64]; cmp rax, 0x43`) and the unreliable one's (`mov r14, rdx; cmp eax,
// 0x43`).
constexpr u64 PacketSendOffset = 0x017894E0;
constexpr std::array<u8, 6> PacketSendPrologue{0x55, 0x48, 0x89, 0xE5, 0x41, 0x57};
constexpr u64 ReliableChannelCheckOffset = 0x01790DDE;
constexpr std::array<u8, 8> ReliableChannelCheck{0x48, 0x63, 0x43, 0x64, 0x48, 0x83, 0xF8, 0x43};
constexpr u64 UnreliableChannelCheckOffset = 0x01790C73;
constexpr std::array<u8, 6> UnreliableChannelCheck{0x49, 0x89, 0xD6, 0x83, 0xF8, 0x43};

// Phase 5, the team lobby (bloodborne_team.h): TitleStep's states (table filled at its start; the
// step's state index at +0x0C, each state's function called every frame until it moves the index
// on). State 6 (0x0195B480) loads the save in GameMan +0x1490 (it copies it to the profile's
// +0x7D0 first) and moves on at once: skipped, the step stays in state 6 and asks again next
// frame, which holds the load for the team. State 2 (0x0195AE80) runs the title menu: the player
// is back at the title.
constexpr u64 TitleLoadStateOffset = 0x0195B480;
// Kaniel's playtest 2026-10-16: a guest's lever pull (ch 0x2D), forced animation (0x24) and event
// (0x31) name their player by its number (player +0x3B8), which the host's game hands out when a
// member joins (0x0178C524: 0x01A6C160 on NetSync +0x18, sent on ch 7). Its counter moves on only
// past a number in use, and a member's counts as used once its character data came (ch 8): the
// team join brings the guests in before that, so two got the same number and each game did the
// other's actions on its own player. Right after the hand-out (eax: the number, r15: NetSync)
// the counter goes on past it.
constexpr u64 PlayerNumberHandedOutOffset = 0x0178C529;
constexpr std::array<u8, 12> PlayerNumberHandedOut{0x89, 0x85, 0x8C, 0xFE, 0xFF, 0xFF,
                                                   0x89, 0x85, 0xDC, 0xFE, 0xFF, 0xFF};
constexpr u64 PlayerNumberCounterOffset = 0x18;
// The number after `n`, as the game's counter goes (past 0x7FFD back to 2; 1 is the host's).
constexpr s32 NextPlayerNumber(s32 n) {
    return n > 0x7FFD ? 2 : n + 1;
}
constexpr u64 TitleMenuStateOffset = 0x0195AE80;
constexpr std::array<u8, 6> TitleStatePrologue{0x55, 0x48, 0x89, 0xE5, 0x41, 0x57};
constexpr u64 TitleLoadSlotOffset = 0x1490; // GameMan's (GlobalState's) save slot to load
// The title's result (docs/native-netcode/title-menu.md): 1 when the save starts at the game's
// beginning (state 5, the first-time start: a new character), 2 for any other load.
constexpr u64 TitleResultOffset = 0x05580334;

// Phase 4 (docs/SERVER_COOP_PLAN.md): one game runs each occupied map's enemies, the one shadNet
// names (bbcoop::EnemyOwners: the session's host while it is there, else the member there first,
// handed on when it leaves, loads or goes silent); the others follow it.
// Always in the port's co-op (phase 6), with the server world and the channel table. The game is
// host-centred: the host runs every enemy (ownership words at WorldChrMan +0xA40 >= 0), sends their
// HP (ch 5, only at state +0x124 == 3) and applies the guests' hits (ch 0x15, sent to the host); a
// guest applies the host's HP (only at state 6, read from the host's member). The owner's game is
// made to act as that host for the enemies, a follower's as a guest:
//   ownership words  owner 0 (AI), follower 0xFFFF (the network controller follows the owner)
//   HP sends         the state checks read 3 on the owner, 6 on a follower (EnemySyncState), and
//                    a follower reads ch 5 from the owner's member, not the host's
//   hits             a follower's 0x15 goes to the owner (our link), the owner's own is dropped
//                    (its hit applied already); a host that follows sends its hits as a guest
//                    does (EnemyHitState)
//   packets          only the owner sends positions, HP, catch-up and join animations (4, 5,
//                    0x29, 0x36) and a follower takes them from the owner only (phase 8: the
//                    owner runs every enemy on its map, wherever the others stand); the game's
//                    own hand-out of enemies (0x16 near lists, 0x17 the host's answer) is dropped
// The AI records (0x3D), per-hit records (0x41) and the hand-over block (0x34) go as they are: the
// game sends 0x34 + 0x3D itself when a character changes hands (phase 0 trace).
struct EnemyOwnerEntry {
    u32 area = 0; // EnemyOwnerArea
    std::string member;
    u64 epoch = 0;
    bool operator==(const EnemyOwnerEntry&) const = default;
};
// What an owner runs: one map (a block, as shadNet's EnemyOwners::AreaOf). The blocks of an area
// are loaded together, but the game wakes a block's enemies only near a player in that world.
constexpr u32 EnemyOwnerArea(u32 map) {
    return map;
}
// The server answers every map (sent each second) with its owners: older ones are stale.
constexpr s64 EnemyOwnersFreshMs = 3'000;
// A new owner's copies take the log's HP (the last owner's reports) for this long.
constexpr s64 EnemyOwnerTakeoverMs = 5'000;
enum class EnemyRole : u8 { Unknown, Owner, Follower };
const EnemyOwnerEntry* EnemyOwnerOf(std::span<const EnemyOwnerEntry> owners, u32 map);
// This member's role on `map`: Unknown (the game's own way) out of the world (map 0), without a
// name, or when nobody runs the area.
EnemyRole EnemyRoleOn(std::span<const EnemyOwnerEntry> owners, std::string_view self, u32 map);
// The session state a host-only check of the HP sync sees instead of `state` (3 hosting, 6 a
// guest), or nothing to change.
std::optional<s32> EnemySyncState(EnemyRole role, s32 state);
// The state the hit on an enemy (0x01A29E40) sees instead: a host that follows sends its hits.
std::optional<s32> EnemyHitState(EnemyRole role, s32 state);
std::optional<u16> EnemyOwnershipWord(EnemyRole role);
// The Doll (m21_00 entity 2100700, 0x10000001 in the Dream) stays in each player's own game
// whatever its role on the map (Kaniel 2026-10-18: the Doll only): handed over in the middle of a
// level-up, she went to where the new owner's copy stood (16 m away) and the talk never ended
// (playtest 2026-10-18, krolebord kneeling at her for good). `entity`: the character's (-1: none).
constexpr s32 HuntersDreamDoll = 2100700;
bool ChangesHandsWithTheMap(s32 entity);
// Her word right after the game's ownership step (see OwnershipStepEndOffset), where the host's
// game hands the network every character its player is not near and a guest's hand-out (0x17)
// the rest of its table: back to this game's own (OwnedEnemyWord) whenever the map has an owner.
std::optional<u16> KeptLocalWord(EnemyRole role, s32 entity, u16 word);
// Whether this owner's copies take the log's HP on becoming the owner: when it takes the map over
// from another (its epoch past the first), not as the map's first owner (Kaniel 2026-10-15).
bool TakesOverEnemyHp(const EnemyOwnerEntry& owner);
// Kaniel 2026-10-15: a guest's own hit on a copy holds for OwnHitHoldMs against the host's older HP
// (ch 5 from before the hit reached the host): the HP to write back, or none.
constexpr s64 OwnHitHoldMs = 1'500;
std::optional<s32> HeldOwnHitHp(s32 current, s32 after_own_hit, s64 age_ms);
// Phase 8 (one HP path): another game's hit (bbcoop::EnemyHit, `hp` of `max` after it) on this
// game's copy (`current` of `copy_max`): the HP to write at once, or none. A follower's copy goes
// down to it (scaled by each copy's max, to 0 for a killing hit: it dies on every screen at
// once); the owner applies the hit itself (the game's 0x15), and without an owner nothing.
std::optional<s32> HitNewsHp(EnemyRole role, s32 current, s32 copy_max, s32 hp, s32 max);
// What the game sends at all (shadNet routes what it sends, coop/packet_routes.h): a follower's
// hit (0x15) and near list (0x16) go to the area's owner, not the session's host the game sends
// them to; what only the owner sends, a follower does not send (the game sees "not sent").
enum class EnemyPacketFate : u8 { Keep, Drop, ToOwner };
EnemyPacketFate EnemyPacketOut(u32 channel, EnemyRole role);
// One line per change of this member's role (or of the owner it follows).
class EnemyRoleLog {
public:
    std::optional<std::string> Change(EnemyRole role, u32 map, const EnemyOwnerEntry* owner);

private:
    std::optional<std::string> last;
};
// The hooks (see InstallEnemyOwner), each with the instructions it copies:
//   0x018EF26C the HP round's `mov rbx, [rbp - 0xF8]` between its `cmp [rax + 0x124], 3` and the
//              jne: the zero flag is the owner's
//   0x018EF578 the dirty HP send's `mov eax, [rdi + 0x124]` (then cmp 3: send, else cmp 6:
//              receive): rdi points at a stand-in with the role's state for that read, put back
//              at 0x018EF587 (`cmp byte [rbx + 0x664], 0`, the send branch) and 0x018EF684
//              (`mov r12, rbx; test rdi, rdi`, the receive branch)
//   0x01789495 the receive from the host's `add rbx, 0xD0` (0x017893E0, channel in r12d): rbx
//              moved so the member read is the owner's
//   0x01A2A102 the hit's `mov ecx, [rax + 0x124]` after the attacker is the local player: rax
//              points at a stand-in (rax is overwritten before its next use)
constexpr u64 EnemyHpRoundCheckOffset = 0x018EF26C;
constexpr std::array<u8, 7> EnemyHpRoundCheck{0x48, 0x8B, 0x9D, 0x08, 0xFF, 0xFF, 0xFF};
constexpr u64 EnemyHpStateReadOffset = 0x018EF578;
constexpr std::array<u8, 6> EnemyHpStateRead{0x8B, 0x87, 0x24, 0x01, 0x00, 0x00};
constexpr u64 EnemyHpSendBranchOffset = 0x018EF587;
constexpr std::array<u8, 7> EnemyHpSendBranch{0x80, 0xBB, 0x64, 0x06, 0x00, 0x00, 0x00};
constexpr u64 EnemyHpReceiveBranchOffset = 0x018EF684;
constexpr std::array<u8, 6> EnemyHpReceiveBranch{0x49, 0x89, 0xDC, 0x48, 0x85, 0xFF};
constexpr u64 ReceiveFromHostMemberOffset = 0x01789495;
constexpr std::array<u8, 7> ReceiveFromHostMember{0x48, 0x81, 0xC3, 0xD0, 0x00, 0x00, 0x00};
constexpr u64 EnemyHitStateReadOffset = 0x01A2A102;
constexpr std::array<u8, 6> EnemyHitStateRead{0x8B, 0x88, 0x24, 0x01, 0x00, 0x00};
constexpr s32 EnemyAwake = 4; // [control +0x14] after the activation pass
// Phase 8 (whole-map owner): WorldChrMan's activation pass (0x019192A0, once a frame) wakes a
// character whose groups share a bit with its block's record (WorldChrMan +0x290 entries): the
// first set (chr +0x218 vs record +0x38) wakes it (4), the third (+0x258 vs +0x78) half (2);
// the second (+0x238 vs +0x18) is only counted. The record follows this game's own player: its
// sets are the groups of the place it stands (a subset of the player's own sets, rig 2026-10-15),
// and the other players' copies in this game carry the groups of where they stand. So the owner
// adds every other player's sets to its block's record for the pass (and puts the record back
// after it): the game wakes the enemies around each of them as around its own player. The pass's
// nearest-first budget (+0x5F70) is -1 (no limit) in Bloodborne (rig 2026-10-15).
constexpr u64 ActivationPassOffset = 0x019192A0;
constexpr std::array<u8, 6> ActivationPassPrologue{0x55, 0x48, 0x89, 0xE5, 0x41, 0x57};
constexpr u64 ActivationPassEndOffset = 0x0191987E; // mov bl, [rbp - 0x69]; and bl, 1 (r14 = it)
constexpr std::array<u8, 6> ActivationPassEnd{0x8A, 0x5D, 0x97, 0x80, 0xE3, 0x01};
// Awake is not run: in a session the host's game hands every enemy its player is not near to the
// network (word 0xFFFF; WorldChrMan's ownership step 0x01915320, then 0x018ECEB0 with the near
// lists), awake or not (rig 2026-10-15: the enemy by the far guest at 4 with word 0xFFFF stood
// still). Right after that step (0x019173B0, called from 0x01916B3F, before the characters'
// updates read the words) the owner keeps every awake enemy of its map as the game keeps its
// own (word 1).
constexpr u64 OwnershipStepEndOffset = 0x019173B0;
constexpr std::array<u8, 6> OwnershipStepEndPrologue{0x55, 0x48, 0x89, 0xE5, 0x41, 0x57};
constexpr u16 OwnedEnemyWord = 1;
// The word the owner gives an enemy after the ownership step, or none: an awake one (`level` its
// [control +0x14], `applied` +0x10) the game let go, unless the game keeps it out.
std::optional<u16> AwakeEnemyWord(EnemyRole role, s32 level, s32 applied, u16 word, bool kept_out,
                                  bool handed_out = false);
// Kaniel's play test 2026-10-18 (krolebord ran Cathedral Ward, the host came: its game ran his hurt
// enemy from its spawn and krolebord's copy jumped 19.9 m there): the session's host coming to a
// map another member runs follows that member's game a while first (shadNet's HostTakeBackMs).
// Its game's ownership step gives its own player's enemies to it all the same: right after it, the
// word such an enemy gets instead (the network's), or none (`hosting`: this game hosts).
std::optional<u16> HostFollowerWord(EnemyRole role, bool hosting, u16 word);
// Kaniel's test 2026-10-23: a guest alone on a map saw the characters its story hides (the
// clinic's dog, every NPC the host's world hides). As the map loads they start loading ([control
// +0x14] wanted, +0x10 applied), then the story hides them (+0x28); the host's game lets them go
// (wanted and applied 0), a guest's game keeps them loaded with no host there to say otherwise, and
// draws them. A lone guest (the owner of its map's enemies) switches such a one off; one it
// switched off goes back on once it is no longer hidden or the guest no longer alone.
bool LoneGuestSwitchesOff(bool lone_guest, bool hidden, bool disabled, s32 wanted, s32 applied);
bool LoneGuestSwitchesBackOn(bool lone_guest, bool hidden);
// Whether the activation pass wakes the enemies around the other players here (WakeAroundOthers):
// on the owner, and on a host that follows (its copies by the owner take the owner's places, so
// they stand there when it takes the map over).
bool WakesAroundOthers(EnemyRole role, bool hosting);
using GroupSet = std::array<u32, 8>;
struct WakeGroups {
    GroupSet wakes{};   // chr +0x218, record +0x38
    GroupSet counted{}; // chr +0x238, record +0x18
    GroupSet half{};    // chr +0x258, record +0x78
    bool operator==(const WakeGroups&) const = default;
};
// A block record's groups for the pass: `record` with each player's of `others` added, when the
// record is this player's block (it shares a group with `own`). A player whose copy has no place
// yet (no groups, or all of them, as right after a join) adds nothing.
WakeGroups WithOthersGroups(const WakeGroups& record, const WakeGroups& own,
                            std::span<const WakeGroups> others);
// Enemy positions (ch 4, docs/native-netcode/characters.md), sent by the map's owner:
// NetWorldChrSync (WorldChrMan +0xA40) walks the local player's map's slots in rounds (0x018ECEB0:
// +0x640 the map index, +0x644 the next slot, +0x648 a round under way, +0x64C the round's period,
// +0x650 its timer, +0x658 "sent last frame"), at most one packet every other frame of up to 8
// entries {u32 chr id, 8 B packed position} (+0x654, the buffer at +0x6C8), only the slots with
// dirty bit 2, a new round at most every 0.25 s. Kaniel 2026-10-15: with the guest away from the
// host on one map, the guest's area woke three times the enemies and each got a position about
// twice a second (every enemy laggy and dumb). The owner sends each full packet itself (our link,
// ch 4 to every member) at the loop's slot step (0x018EE922: rbx the entries, r14 the packet's
// size, r8 the sync, r15 the map's slots, +0x10 their count) and the game fills the next one: a
// round goes in one frame. And a round every frame (Kaniel 2026-10-15: "send every frame?").
constexpr float GamePositionRoundSeconds = 0.25f; // the sync's ctor, 0x018EC19D
constexpr u64 PositionRoundPeriodOffset = 0x64C;  // in NetWorldChrSync
constexpr u64 PositionSlotOffset = 0x644;
constexpr u64 PositionPacketOffset = 0x6C8;
constexpr u32 PositionPacketBytes = 0x60;
constexpr u32 PositionChannel = 0x04;
// 0x018EE910 `mov rax, [rbp - 0x108]` right after an entry went in (`inc ebx`): rbx the entries,
// r14 the packet's size, r12 the sync, r15 the map's slots. 0x018EEBB0 `mov r13d, [r12 - 4]`: a
// received entry's chr id at r12 - 4.
constexpr u64 PositionPacketEntryOffset = 0x018EE910;
constexpr std::array<u8, 7> PositionPacketEntry{0x48, 0x8B, 0x85, 0xF8, 0xFE, 0xFF, 0xFF};
constexpr u64 PositionReceiveEntryOffset = 0x018EEBB0;
constexpr std::array<u8, 5> PositionReceiveEntry{0x45, 0x8B, 0x6C, 0x24, 0xFC};
constexpr u32 MaxPositionPacketsPerFrame = 16;
bool SendsFullPositionPacket(EnemyRole role, s32 entries, s32 per_packet, s32 slot, s32 slots,
                             u32 sent_this_frame);
float PositionRoundSeconds(EnemyRole role);
// The game's own hand-out of enemies (Kaniel 2026-10-15: the nearest player runs each enemy, as
// in vanilla; the host running a far guest's enemies made them lag): a guest sends the enemies
// near it (0x16, period NetWorldChrSync +0x638, 5 s), the host answers which of them that guest
// runs (0x17, period +0x630, 3 s; payload u32 n + n entries, the chr +0x90 index in the low 16
// bits, something that changes each time in the high ones; the guest sets those words
// to 1 and the rest of its table to 0xFFFF, 0x018ED07E). The owner runs the awake enemies it did
// not hand out (AwakeEnemyWord). The slow periods made a hand-over hang for a second: both every
// 0.25 s with an owner.
constexpr float GameHandOutSeconds = 3.0f;  // the sync's ctor, 0x018EC15A
constexpr float GameNearListSeconds = 5.0f; // 0x018EC16E
constexpr u64 HandOutPeriodOffset = 0x630;
constexpr u64 NearListPeriodOffset = 0x638;
float HandOutSeconds(EnemyRole role);
float NearListSeconds(EnemyRole role);
std::vector<u32> HandOutIds(std::span<const u8> payload);
bool HandedOut(std::span<const u32> handed, u32 index);
// An enemy by this player that moved farther in one frame than any enemy runs: a hand-over that
// jumped (Kaniel 2026-10-15: "lagging for a second before moving back").
constexpr float EnemyJumpMetres = 0.75f;
bool EnemyJumped(const std::array<float, 3>& before, const std::array<float, 3>& now);
// A hand-out older than this names nobody (the game sends one each period, empty or not).
constexpr s64 HandOutFreshMs = 1'000;
// Kaniel's play test 2026-10-17 (a dog lagged while both players fought it): with both players
// about as near, the owner's game handed an enemy out and took it back every 0.25-1.3 s, and each
// change of hands jumps it. The owner keeps an enemy (its entry replaced by HandOutHeldEntry)
// until its game has wanted to hand it out to that member for HandOverSettleMs; one the game takes
// back goes back at once (the follower's game drops what a hand-out leaves out, 0x018ED0ED).
// HandOutHeldEntry is block 31, slot 0x7FF: the follower's game skips it (0x018ED127, 0x018ED13D)
// and the payload keeps its size (it must be 4 + 4 n, 0x018ED0A5). The first hand-out to a member
// (or after a gap: a load, a trip, a host that followed the map's owner) goes as the game made it:
// nothing went back and forth yet (Kaniel's play test 2026-10-18: a host taking the map over from a
// guest ran the enemies by that guest a second more, two changes of hands instead of none).
constexpr s64 HandOverSettleMs = 1'000;
constexpr u32 HandOutHeldEntry = 0xFFFF;
class HandOutSettle {
public:
    // The game's hand-out to `member` (the payload's entries): the entries to send.
    std::vector<u32> Filter(const std::string& member, std::span<const u32> entries, s64 now_ms);

private:
    struct Member {
        std::map<u32, s64> wanted_since; // by index, while the game hands it out
        std::set<u32> handed;            // by index, handed out by this filter
        s64 last_ms = 0;
        bool seen = false; // a hand-out since the last gap
    };
    std::map<std::string, Member> members;
};
// The guest's receive of 0x17 (0x018ED083 `mov r8, rbx; mov ebx, eax; test ebx, ebx`, eax the
// size) into the buffer at 0x05570090.
constexpr u64 HandOutReceivedOffset = 0x018ED083;
constexpr std::array<u8, 7> HandOutReceived{0x49, 0x89, 0xD8, 0x89, 0xC3, 0x85, 0xDB};
constexpr u64 HandOutBufferOffset = 0x05570090;
// The enemies awake only by another player get no position from the owner's game: its per-enemy
// net update (0x0151BCA0, which copies the place (physics +0x1E0, +0x1E8) and the turn (+0x1D0,
// +0x1D8) into the sync's position record (+0x20, 0x18 a slot: +0, +8, +0xC, +0x14) and sets bit 2
// of the slot's flags (+0x38)) runs only for the enemies by its own player (rig 2026-10-15: a
// write watch; the packets name an enemy by its chr +0x90 index). The owner does it for every
// awake enemy it runs, every frame as the game does (after the ownership step, before the
// characters' updates).
bool OwnerSendsPositionOf(EnemyRole role, s32 level, s32 applied, u16 word, bool kept_out);
// A follower's measure (its log): the positions it took (`taken` by the packets' id, the chr's
// +0x90 index: map index << 11 | slot) for the enemies `nearby` its player ({chr id, index}), per
// second over `seconds`, fastest first.
std::string PositionRatesText(const std::map<u32, u32>& taken,
                              std::span<const std::pair<u32, u32>> nearby, double seconds);

// The game's packets in our frames (docs/SERVER_COOP_PLAN.md phase 2b, bloodborne_game_link.h,
// see InstallGamePacketLink). The transport send under NetSync's send and its resend
// (0x00C8F0A0: rdi the session, esi reliable, rdx the member id, rcx the packet {u32 channel,
// u32 size, payload}, r8d its size; it answers true for "sent"): a packet our link takes is
// "sent", so the game's resend queue and its 180 s backlog timer (SprjSessionManager +0x2E0 /
// +0x2F0, raised only by a backlog) stay empty. The packets our link took go to the member's
// queues as the game's receive (0x0178D510, NetSync's vtable +0x08) does: the member's peer found
// in the net sync's list ([0x0553D6D0] +0x28: nodes {next, prev, peer}, the peer's member id at
// +0x08), then the game's own fill 0x01790C20(peer, reliable, bytes, size).
// Neither the fill nor the receive's pop of a channel's queue (0x017910F0) takes a lock: the game
// fills and pops on its own threads in turn. Filled from our link's thread at any time, a queue's
// list broke (rig 2026-10-18, party3 leader-returns3 4 of 4: "given memory block seems to be
// improper or freed already", the pop's free on a character thread); a lock of ours over both
// lost the network at party wipes. So the packets go in where the game's own came in: at the
// start of NetSync's update (0x01789B60, its per-frame update on the game's thread, before its
// receive). Only while that update has not run for NetSyncUpdateQuietMs (a load, a join's start:
// rig 2026-10-10, joins stalled when only a per-frame path delivered) does the link deliver them.
constexpr u64 TransportSendOffset = 0x00C8F0A0;
constexpr u64 MemberQueuesFillOffset = 0x01790C20;
constexpr u64 NetSyncUpdateOffset = 0x01789B60;
constexpr s64 NetSyncUpdateQuietMs = 300;
constexpr std::array<u8, 6> GamePacketLinkPrologue{0x55, 0x48, 0x89, 0xE5, 0x41, 0x57};
// A packet for a member the game has no queues for yet (it is still joining) waits this long.
constexpr s64 HeldForUnknownMemberMs = 10'000;

// Boss-death tracer (Kaniel 2026-10-11: Father Gascoigne died in the host's world with nobody
// hitting him, and a guest got "Prey Slaughtered"): the game sets a character's dead flag
// (`or byte ptr [reg + 0x1E3], 1`) in three places, each once per death: the two deaths at 0 HP
// (0x018BF760, 0x018CDE10, both then calling the death step) and its own kill (KillCharacter, an
// event's ForceCharacterDeath). A hook on each logs the character and the game's call chain.
enum class DeathRegister { Rbx, Rcx, R15 };
struct DeathFlagSite {
    u64 offset;
    std::vector<u8> instruction;
    DeathRegister character;
    const char* name;
};
const std::vector<DeathFlagSite>& DeathFlagSites();
// Only characters this big (bosses and the like); ordinary kills would flood the log.
constexpr s32 DeathTraceMinMaxHp = 1500;
bool TracesDeathOf(s32 max_hp);
// A target: "nearest" (0), "self" (AutoplaySelf, the player), "marked" (AutoplayMarked, the one
// the last mark command picked), "player" (AutoplayPlayer, the nearest other player: a duel's
// foe) or a character handle in hex.
constexpr u32 AutoplaySelf = 0xFFFFFFFF;
constexpr u32 AutoplayMarked = 0xFFFFFFFE;
constexpr u32 AutoplayPlayer = 0xFFFFFFFD;
std::optional<u32> ParseAutoplayTarget(std::string_view text);
// watch-effects: the SpEffects a character gained ("+id") and lost ("-id") between two looks, in
// id order, space separated; "" when nothing changed. Each copy of an effect counts.
std::string DescribeEffectChanges(std::vector<s32> before, std::vector<s32> after);
// `value <op> limit` for op <, <=, ==, !=, >=, >; nothing for another op.
std::optional<bool> CompareAutoplay(s64 value, std::string_view op, s64 limit);
// attack <target>: a real hit, so the kill reward and the death run as in play (the game's own
// kill leaves an ordinary enemy standing in a party). Cycle k (AttackCycleSeconds each) stands
// AttackDistance from the target on side k % 4 (+z, +x, -z, -x) and holds R1 from 0.3 s to 0.5 s
// into the cycle, turned towards the target (HeadingTowards).
struct AttackStance {
    float x = 0.0f;
    float z = 0.0f;
};
constexpr double AttackCycleSeconds = 1.5;
constexpr float AttackDistance = 1.2f;
AttackStance AttackStanceFor(float target_x, float target_z, u32 cycle);
// The teleport heading (radians) that turns a player at (x, z) to face (to_x, to_z): heading h
// faces (-sin h, cos h).
float HeadingTowards(float x, float z, float to_x, float to_z);
u32 AttackCycleAt(double elapsed);
bool AttackHoldsR1(double elapsed);

// Party death, after Elden Ring Nightreign (SHADPS4_BLOODBORNE_PARTY_DEATH): in a party session a
// lethal hit puts the player down at 1 HP instead of killing it while another party member stands
// on its map. The others revive it by striking it (their weapons' damage adds up); it can hold
// Circle to give up (today's death). When nobody on a map is left standing, everyone down there
// dies for real (a wipe, decided by shadNet) and wakes at its own last lamp with the map reloaded;
// a lethal hit with nobody else standing on the map is such a wipe at once.
// The setting: "1" on, "0" off, unset or empty `by_default` (the build's: on in the port, which has
// the party HUD; off in the emulator).
bool PartyDeathEnabled(const char* setting, bool by_default);

// IsNoDead(data module) (0x01A577D0): the game's lethal HP changes leave 1 HP instead of 0 when it
// answers true, as the debug Player No Dead does. Its callers by return address (call + 5): a
// lethal check that may put the player down, one that stays a real death (the game's kills), one
// asked after every hit (lethal only when the hit left no HP), or another question about it.
enum class NoDeadCheck { Other, Lethal, Instant, AfterHit };
struct NoDeadCaller {
    u64 return_offset = 0;
    NoDeadCheck check = NoDeadCheck::Other;
};
constexpr u64 IsNoDeadOffset = 0x01A577D0;
// IsNoDead's one exit, `mov al, r14b; add rsp, 8`: the answer in r14b, the data module in rbx and
// the caller's return address at [rbp + 8].
constexpr u64 IsNoDeadExitOffset = 0x01A57923;
constexpr std::array<u8, 7> IsNoDeadExit{0x44, 0x88, 0xF0, 0x48, 0x83, 0xC4, 0x08};
std::span<const NoDeadCaller> NoDeadCallers();
NoDeadCheck ClassifyNoDeadCheck(u64 return_offset);
// The check with the player's HP as it is asked: the one after every hit is lethal only at 0 HP or
// less, and another question otherwise.
NoDeadCheck NoDeadCheckAt(NoDeadCheck check, s32 hp);
// What IsNoDead answers for the local player (true keeps it at 1 HP): `armed` when a lethal hit
// would put it down now, `down` while it is down, `answer` the game's own.
bool KeepsPlayerAlive(NoDeadCheck check, bool armed, bool down, bool answer);

// A party member's state on the vitals board: not playing party death, standing, down, or dead
// for real until it wakes at its lamp. Kaniel 2026-10-25: or out of the fight, spectating its
// teammates (the team's death mode, bloodborne_spectate.h): neither standing nor down.
enum class Vital : u8 { Off, Up, Down, Dead, Spectate };
std::string_view VitalName(Vital vital);
std::optional<Vital> ParseVital(std::string_view name);
// Whether IsNoDead's hook watches the local player now: standing (or not playing party death) in
// the session, down or spectating anywhere, and never while a real death plays out (Dead): a hit
// on the dying body, caught as a lethal hit with a teammate standing, kept it at 1 HP and the
// death never finished (rig 2026-10-26: a guest that gave up beside an enemy never got up).
bool PartyDeathWatches(bool in_session, Vital vital);
// What this member posts on the vitals board, for the log on change only (the rig checks
// SessionArrival by it: a summoned guest goes from "off" to "up" once it arrived).
class VitalsPostLog {
public:
    // The line to log, or nothing when the vital and the map are the ones last logged.
    std::optional<std::string> Next(Vital vital, u32 map);

private:
    std::optional<std::pair<Vital, u32>> last;
};

// Revive damage a downed player needs: its max HP the first time it goes down, twice that the
// second, four times from the third (DownedState forgets old downs); at least 1.
u32 ReviveDamageNeeded(u32 downs, s32 max);
// What one revive hit deals: the striker's attack rating for the weapon in its right hand (the
// equipment menu's), half again for R2 (`heavy`); at least 1.
u32 ReviveHitDamage(s32 attack_rating, bool heavy);
// What a real hit on a downed body counts for its revive: what it dealt, and never less than the
// weapon's attack rating (ReviveHitDamage): the body's own defences cut a teammate's hit, and the
// revive's damage is sized by attack ratings (rig 2026-10-12: bare fists dealt 2 to 3).
u32 RealReviveHitDamage(u32 dealt, s32 attack_rating, bool heavy);
// Real hits counted in proportion to the lightest light hit seen (its baseline): that one counts as
// the attack rating, a hit dealing twice as much twice the rating, never less than
// RealReviveHitDamage (Kaniel 2026-10-13: a charged R2 counted as an R1).
class ReviveHitScale {
public:
    u32 Count(u32 dealt, s32 attack_rating, bool heavy);

private:
    u32 baseline = 0;        // the lightest light hit's damage, for `baseline_rating`
    s32 baseline_rating = 0; // another weapon (another rating) starts a new baseline
};
// The share of a revive done, 0 to 1 (the panel's ring and text).
float ReviveShare(u32 landed, u32 needed);
// The HP a revived player gets up with: half its max, at least 1.
s32 RevivedHp(s32 max);
// How long a revived player takes no damage (the party's last swings, rig 2026-10-12).
constexpr s64 ReviveGraceMs = 2'000;
// The player's kneel (c0000 a00: stand to kneel 2 s; the behaviour goes on to the held kneel
// 101261 by itself, Event101260_to_Event101261) and its stand-up (2 s), the poses of a downed
// player (the game's own events play them on the player, m25_00_00_00).
constexpr s32 DownedKneelAnimation = 101260;
constexpr s32 DownedStandAnimation = 101262;
// The game's play-animation for a character (0x013CBFD0, ForcePlayAnimation's): it writes the
// request only while the character takes one (0x013CC132-0x013CC158: character +0x3B0 its
// modules, +0x58 a module whose +0x08 is a character, that one's +0x3B0, +0x30, byte +0x384 set);
// otherwise the request is dropped, and its return value says nothing. The down comes in the
// middle of the lethal hit, when it is clear (Kaniel 2026-10-13: no kneel), so the kneel waits for
// it, and for the hit's own reaction to pass (DownedKneelDelayMs).
constexpr u64 AnimationPlayOffset = 0x013CBFD0;
constexpr u64 AnimationRequestCheckOffset = 0x013CC132;
constexpr std::array<u8, 7> AnimationRequestCheck{0x48, 0x8B, 0x80, 0xB0, 0x03, 0x00, 0x00};
constexpr std::array<u64, 5> AnimationRequestPath{0x3B0, 0x58, 0x08, 0x3B0, 0x30};
constexpr u64 AnimationRequestFlagAt = 0x384;
constexpr s64 DownedKneelDelayMs = 400;
// Whether `character` takes an animation request now, reading guest memory with `read` (0 for an
// address it can't read).
bool TakesAnimationRequest(u64 character, const std::function<u64(u64 address, size_t size)>& read);
// Whether a downed player still waiting for its kneel asks for it now.
bool KneelsNow(bool waiting, s64 down_for_ms, bool takes_request);
// The pose a downed player plays: none (Kaniel 2026-10-15: no kneel on a down);
// SHADPS4_BLOODBORNE_DOWNED_ANIMATION (`setting`, null when unset) picks one, as the kneel.
s32 DownedAnimation(const char* setting);
// The HP a downed player shows while it is struck back up: 1, rising with the share of the revive
// done to just under its max (the revive sets the real HP), so the game's own HP bar and the
// nameplate bar the others see over it (RemotePCTag, fed by the synced HP) fill with the revive.
s32 DownedHp(s32 max, u32 landed, u32 needed);
// The game's SetTextEffect (event binding; this, id): queues one of the HUD's big banners, ids 1
// to 17 (1 NIGHTMARE SLAIN, 2 YOU DIED, 5 PREY SLAUGHTERED, 17 the PvP Draw). The party HUD
// (bbport/scripts/hud_patch.py) makes the Draw banner the yellow YOU ARE DOWN.
constexpr u64 SetTextEffectOffset = 0x013360B0;
constexpr std::array<u8, 6> SetTextEffectStart{0x8D, 0x46, 0xFF, 0x83, 0xF8, 0x10};
constexpr s32 DownedBannerTextEffect = 17;

// The party HUD's nameplates: the game's tag over each remote player (fe.gfx RemotePCTag0-6). The
// HUD tick updates each tag from a TagData (0x01B59E20: rdi the tag, rsi its data), and its one
// exit (0x01B5A3AE, `add rsp, 0xC8`, r14 the tag and r13 the data still) is where the role line can
// be written over: the game sets every line again on the next frame.
constexpr u64 HudTagUpdateExitOffset = 0x01B5A3AE;
constexpr std::array<u8, 7> HudTagUpdateExit{0x48, 0x81, 0xC4, 0xC8, 0x00, 0x00, 0x00};
// FeData, the HUD's per-frame snapshot (static): the remote players' TagData from +0xBA8, 0xA8
// each.
constexpr u64 HudFeDataOffset = 0x0553EA20;
constexpr u64 HudRemoteTagsAt = 0xBA8;
constexpr u64 HudTagDataSize = 0xA8;
constexpr int HudRemoteTagCount = 7;
constexpr u64 HudTagNameAt = 0x60;      // TagData: the player's name (a game HUD string)
constexpr u64 HudTagRoleLineAt = 0x250; // tag: Post/Value, the blue role line ("Co-operator")
constexpr u64 HudTagNameLineAt = 0x300; // tag: Name/Value, the blue name
// An element of a tag (a 0x58-byte wrapper): its GFx::Value's object interface at +0x38, type at
// +0x40 (& 0x8F nonzero when it is set) and data at +0x48; the interface's vtable slot 0x130 is
// SetText(data, UTF-8 text, html).
constexpr u64 HudElementInterfaceAt = 0x38;
constexpr u64 HudElementTypeAt = 0x40;
constexpr u64 HudElementDataAt = 0x48;
constexpr u64 HudSetTextUtf8Slot = 0x130;
// A game HUD string (UTF-16): the characters at +0x08, in place while the capacity (+0x20) is
// below 8, else behind the pointer there.
constexpr size_t GameHudStringSize = 0x28;
// A whole game HUD string: its allocator at +0x28 (the tag and prompt updates copy through it).
constexpr size_t GameHudStringFullSize = 0x38;
// Its characters as ASCII ('?' for the others), at most 64; `read(address, count)` reads count
// UTF-16 characters from guest memory for a string kept out of place.
std::string GameHudString(std::span<const u8, GameHudStringSize> raw,
                          const std::function<std::vector<char16_t>(u64, size_t)>& read);
// Phase 11: a character's name, the game string at PlayerGameData +0x5D0's name holder (what the
// game sends the others on channel 8, 0x01782088), in UTF-8; "" when it does not read or is longer
// than the 16 characters a name has.
constexpr u64 PlayerGameNameHolderAt = 0x5D0;
std::string GameStringUtf8(std::span<const u8, GameHudStringSize> raw,
                           const std::function<std::vector<char16_t>(u64, size_t)>& read);
// Phase 11: the name line of the nameplate over `name` (the game's, an online id): the member's
// character name as html text, when the panel knows one; else the game's own stays.
std::optional<std::string> NameplateNameLine(const struct PartyPanel& panel, std::string_view name);
// A member's name as the player sees it: its character's (the server's, PanelMember::shown), else
// a Hunter; never its online id (website shots 2026-10-19: "Strike to revive rig_guest").
constexpr std::string_view UnknownHunterName = "Hunter";
std::string ShownName(const struct PanelMember& member);
// Text for a game html field: &, < and > as entities.
std::string HtmlText(std::string_view text);
// Whether any member on the panel is down (only then does a nameplate say anything of ours).
bool PanelHasDowned(const struct PartyPanel& panel);
// The role line of the nameplate over `name` (an online id, or a board name without one): its
// revive, in red html, while that member is down; nothing for anyone else (the game's line stays).
std::optional<std::string> NameplateRoleLine(const struct PartyPanel& panel, std::string_view name);
// Which remote player's tag (0 to 6) a TagData address is, from FeData's address.
std::optional<int> RemoteTagIndex(u64 tag_data, u64 fe_data);

// The native party list: the party HUD's CoopParty0-2 (bbport/scripts/hud_patch.py, three more of
// the tag sprite RemotePCTag0 shows), made and updated with the HUD's own tag code: the child
// lookup (0x01ADB220: out PathRef, parent, printf format, ...), the tag constructor (0x01B58E40:
// tag of HudTagSize bytes, PathRef, 0, 0) and the tag update with its TagData and the frame time.
// Where the HUD tick has updated its own tags (0x01BF8A50, `mov rdi, [r15 + 0xF8]`: r15 the HUD,
// the frame time at [rbp - 0x6C8]) the list's are updated.
constexpr u64 HudChildLookupOffset = 0x01ADB220;
constexpr u64 HudTagConstructorOffset = 0x01B58E40;
constexpr u64 HudTagUpdateOffset = 0x01B59E20;
// An element's SetVisible (0x01ACB760: element, shown), the element wrapper's vtable slot 1; a tag
// starts with such a wrapper of its whole sprite.
constexpr u64 HudSetVisibleOffset = 0x01ACB760;
constexpr u64 HudTagSize = 0x6E0;
constexpr u64 HudPathRefSize = 0x60;
// A PathRef's value (Scaleform's GFx::Value, at +0x30: list links, the object interface at +0x40,
// the type at +0x48, the object at +0x50). One that holds a HUD object (type bit 0x40) is linked
// into the HUD movie's value list until the interface's release (vtable slot 3: interface, value,
// object) unlinks it, as the game does after each tag it makes (0x01BF528B). The movie's
// destructor (0x0001DEE0) releases every value still linked (0x0001E304), so a linked value's
// storage must outlive the HUD (rig wipe3 2026-10-12: the icons' PathRefs were left linked on the
// stack, and a guest's world teardown crashed there on a dead one, eboot+0x1E312).
constexpr u64 HudPathRefValueAt = 0x30;
constexpr u64 HudPathRefInterfaceAt = 0x40;
constexpr u64 HudPathRefTypeAt = 0x48;
constexpr u64 HudPathRefObjectAt = 0x50;
constexpr u32 HudValueLinkedBit = 0x40;
enum class HudValueKeep {
    Drop,          // nothing linked: the storage may go
    Release,       // linked: release it before the storage goes
    KeepForTheHud, // linked and kept: storage that outlives the HUD, never freed
};
HudValueKeep HudValueFate(u32 type, bool kept_for_the_hud);
// The HUD's constructor (rdi the HUD, as r15 in its tick; its only call 0x01F62B53, on a fresh
// 0x980-byte object). A new HUD may be made where an old one was, so the party list goes by the
// HUDs made, not by the address.
constexpr u64 HudConstructOffset = 0x01BF41C0;
constexpr std::array<u8, 8> HudConstructPrologue{0x55, 0x48, 0x89, 0xE5, 0x41, 0x57, 0x41, 0x56};
// Whether the list made in `list_hud` (the `list_generation`th HUD made) is the wrong one for
// `hud`, the `generation`th HUD made: a new HUD, even at the same address, makes its own.
bool PartyListOutdated(u64 hud, u64 generation, u64 list_hud, u64 list_generation);
constexpr u64 HudTicksTagsDoneOffset = 0x01BF8A50;
constexpr std::array<u8, 7> HudTicksTagsDone{0x49, 0x8B, 0xBF, 0xF8, 0x00, 0x00, 0x00};
constexpr s64 HudFrameTimeFrameOffset = -0x6C8;
constexpr int PartyListSize = 3;
struct PartyListEntry {
    bool visible = false;
    s32 hp = 0;
    s32 max = 1;
    std::string role; // html, the game's role line
    std::string name;
    std::string name_line; // the name as shown, in white html (written over the tag's own)
    std::string member;    // its board name (whose portrait the slot shows)
    u32 icon = 1;          // its icon's frame (PartyListIcon...)
    u32 vials = 0;         // Kaniel 2026-10-18: shown beside the name (the overlay draws them)
    u32 bullets = 0;
};
// The icon sprite's frames beside each tag (CoopPartyIcon0-2, our drawings: hud_patch.py
// HUD_ICON_FRAMES), 1-based as GotoAndStop takes them: none, the hunter, the lantern (another map),
// the headstone, then the downed drop with its ring red to 0%, 10%, ... 100%.
constexpr u32 PartyListIconNone = 1;
constexpr u32 PartyListIconHunter = 2;
constexpr u32 PartyListIconElsewhere = 3;
constexpr u32 PartyListIconFallen = 4;
constexpr u32 PartyListIconDowned0 = 5;
constexpr u32 PartyListIconFrames = PartyListIconDowned0 + 10;
// The icon objects' value (a PathRef's) and the object interface's GotoAndPlay(data, frame, stop)
// slot.
constexpr u64 HudGotoFrameSlot = 0x158;
// The party HUD's prompt (CoopPrompt, a clone of the game's interact prompt ActionButtonTop):
// made and updated as the HUD makes its own (alloc HudPromptSize, constructor 0x01B56E60 with the
// PathRef, update 0x01B57460 with its data and the frame time). Its data: +0 shown, +1 a second
// line, +2 the game's own (Cross) button shown, the text from +0x08 (a game HUD string of
// GameHudStringFullSize); FeData +0x13B0 is the game's prompt's.
constexpr u64 HudPromptConstructorOffset = 0x01B56E60;
constexpr u64 HudPromptUpdateOffset = 0x01B57460;
constexpr u64 HudPromptSize = 0x1A8;
constexpr u64 HudPromptDataSize = 0x40;
constexpr u64 HudPromptTextAt = 0x08;
constexpr u64 HudFeDataPromptAt = 0x13B0;
// Party death's prompt: hold the dodge button to give up while down, strike (R1) to revive a downed
// member in reach. `input` is the pad input whose glyph goes before it (runtime_pad.c's order).
struct PartyPrompt {
    bool shown = false;
    std::string text;
    int input = 0;
    bool operator==(const PartyPrompt&) const = default;
};
constexpr int PromptInputCircle = 1;
constexpr int PromptInputR1 = 5;
PartyPrompt PartyPromptFor(const struct PartyPanel& panel);
// The prompt's data for the game's update; `text` is the address of its UTF-16 words, `string` a
// game HUD string to take the allocator from (a nameplate's: the game's own prompt data has none
// until the game shows a prompt itself).
void WritePromptData(std::span<u8, HudPromptDataSize> data,
                     std::span<const u8, GameHudStringFullSize> string, const PartyPrompt& prompt,
                     u64 text, size_t length);

// The list's tags for the panel's first members: HP while up, the revive while down (of 100).
std::array<PartyListEntry, PartyListSize> PartyListEntries(const PartyPanel& panel);
// Kaniel 2026-10-18: what the HUD's own vial and bullet count fields beside a list entry's name say
// (hud_patch.py CoopSupply0-2): its vials and bullets, nothing while the slot shows nobody.
std::array<std::string, 2> SupplyTexts(const PartyListEntry& entry);
// Kaniel 2026-10-18 ("will it work fine with a long name?"): the vial and bullet follow the name.
// CoopSupply's frame 2 shows them where a name up to SupplyNameRoom px wide ends before them; each
// next frame SupplyShiftPx further right (hud_patch.py SUPPLY_SHIFT, SUPPLY_SHIFTS).
constexpr float SupplyShiftPx = 6.0f;
constexpr u32 SupplyShifts = 32;
constexpr float SupplyNameRoom = 71.0f;
// A name's width in the list's font on the 1080p stage, estimated per letter (measured on the rig's
// shots: "gggtt" 55 px).
float SupplyNameWidth(std::string_view name);
// CoopSupply's frame for a list entry: 1 while it shows nobody, else 2 plus the steps its name
// needs.
u32 SupplyFrame(const PartyListEntry& entry);
// Where a tag goes on the HUD's 1920x1080 stage (its HP bar's left end).
std::pair<s32, s32> PartyListPosition(int index);
// The native port on another aspect ratio than 16:9 (its aspect hooks, Kaniel 2026-10-24): how far
// the HUD's groups are moved toward the screen's edges, in the stage's pixels. The list moves with
// the player's own bars (up and to the left by it). 0 (16:9) by default.
void SetHudEdgeShift(float dx, float dy);
// The TagData the tag update takes for an entry; `role` and `name` are the addresses of its UTF-16
// strings (kept by the caller, at most 63 characters each). A game HUD string is 0x38 bytes: the
// tag update copies it with the allocator at +0x28 (and a flag at +0x30), so each string starts as
// the one in `game` (a TagData of the game's own, RemotePCTag0's).
void WriteTagData(std::span<u8, HudTagDataSize> data, std::span<const u8, HudTagDataSize> game,
                  const PartyListEntry& entry, s32 x, s32 y, u64 role, u64 name);

// The local player's side of party death. Times are steady milliseconds.
class DownedState {
public:
    // Revive progress drains a third of the max HP per RefillStepMs once nobody hit for
    // RefillDelayMs.
    static constexpr s64 RefillDelayMs = 3'500;
    static constexpr s64 RefillStepMs = 1'000;
    // The down count starts over after this long up, at a lamp, and after a real death.
    static constexpr s64 PenaltyResetMs = 180'000;

    // `first_id`: the first down's id, different for each run of the game so a party member's
    // hits on an old down never count for a new one.
    explicit DownedState(u32 first_id) : next_id{first_id} {}

    // A lethal hit put the player (of `max` HP) down; returns the down's id.
    u32 GoDown(s64 now_ms, s32 max);
    // The party's revive damage on this down so far (the vitals board's total): true when it
    // revives the player.
    bool Damage(u32 total, s64 now_ms);
    void Tick(s64 now_ms);
    void GetUp(s64 now_ms);
    // A real death (give up, wipe, or a death that skips the down), until the respawn.
    void Died();
    // Kaniel 2026-10-25: a lethal hit in a team that spectates (DeathMode::Spectate): out of the
    // fight until it wakes at its lamp (GetUp) or dies with the party (Died). No down is counted.
    void Spectate();
    void Alive(s64 now_ms);
    void ClearPenalty();

    Vital Current() const {
        return vital;
    }
    u32 DownId() const {
        return down_id;
    }
    u32 DamageNeeded() const {
        return needed;
    }
    u32 DamageLanded() const {
        return landed;
    }

private:
    Vital vital = Vital::Up;
    u32 next_id;
    u32 down_id = 0;
    u32 downs = 0;
    u32 needed = 0;
    u32 landed = 0;
    u32 seen_damage = 0;
    u32 drain_step = 1;
    s64 drain_at = 0;
    std::optional<s64> up_since;
};

// The lethal hits IsNoDead's hook caught (its running count), as party death takes them: a catch
// puts a standing player down. The hook also catches the checks on the body while a real death
// plays out (0 HP, Kaniel 2026-10-25: up at the lamp after giving up, then straight down), and
// those are dropped, not kept for the first tick up again.
class LethalCatches {
public:
    // The hook's count now and this player's state: true when a new catch puts it down.
    bool Take(u32 count, Vital vital);

private:
    u32 seen = 0;
};

// Holding a button for `hold_ms` (giving up): fires once per hold.
class HoldToConfirm {
public:
    explicit HoldToConfirm(s64 hold_ms) : hold_ms{hold_ms} {}
    bool Tick(bool held, s64 now_ms);
    // Starts over (a new down): a button `held` already counts only once let go.
    void Restart(bool held);
    float Progress(s64 now_ms) const;

private:
    s64 hold_ms;
    std::optional<s64> held_since;
    bool fired = false;
};
constexpr s64 GiveUpHoldMs = 3'000;

// The party's vitals board on shadNet (a message on the server link, once POST
// /bloodborne/party/vitals). Each member posts
// "<member> <map> <vital> <hp> <max> <down> <landed> <needed> <x> <y> <z>" (map in hex, down the
// id of its current or last down, landed/needed its revive progress in damage, x y z where it is)
// and its revive damage on others as "<target>:<down>:<damage>" totals. The answer has the same
// line per member with the party's revive damage on that member's down added at the end, then a
// "wipe <id> <map> <member>..." line for each wipe shadNet declared lately.
// Kaniel 2026-10-20 (bloodstains on the compass and map): this player's bloodstain, the echoes it
// dropped where it died. The game keeps one, the latest; map 0: none. Only its own player sees it
// (Kaniel after v1.0.2: "you only see your bloodstain on compass and map").
struct Bloodstain {
    u32 map = 0;
    LampPosition at{};
    s32 echoes = 0;
    bool operator==(const Bloodstain&) const = default;
};
// This player's bloodstain from the game's record (GameDataMan +0x28 its mark; the record at
// [GameDataMan +0x30]: +0x3C its map, +0x00 its position, +0x38 its echoes). The mark stays up
// on an empty record (map -1, echoes -1), and a stain without echoes holds nothing: none then.
std::optional<Bloodstain> BloodstainOf(bool has_mark, u32 map, const LampPosition& at, s32 echoes);
// The same from the record's bytes (`record`, from its start; nothing from a shorter read). Player
// report 2026-10-23: the record keeps two places, as UpDateBloodMark (0x013D2910) writes them:
// +0x00 where the stain lies (the player's last safe ground, [player +0x400] +0x30, which the
// stain's marker copies when the game shows it, 0x01868A50) and +0x18 where the player died (the
// foot of a cliff it fell from). The stain is at the first.
constexpr size_t BloodstainRecordSize = 0x40;
std::optional<Bloodstain> BloodstainOfRecord(bool has_mark, std::span<const u8> record);
struct MemberVitals {
    std::string member;
    u32 map = 0;
    Vital vital = Vital::Off;
    s32 hp = 0;
    s32 max = 0;
    u32 down = 0;
    u32 landed = 0;
    u32 needed = 0;
    LampPosition at{};
    u32 revive_damage = 0; // in the answer only
    u32 cutscene = 0;      // CutsceneVote's state, through the server only (not the direct line)
    // Kaniel 2026-10-18: its Blood Vials and Quicksilver Bullets (the server only, as cutscene).
    u32 vials = 0;
    u32 bullets = 0;
    // Invasions (docs/INVASIONS.md, the server only): this character's level and whether a boss
    // fight runs in its world (posted), its death followed an invader's hit (posted), and on the
    // board the invader's line (bloodborne_invasion.h: never a teammate).
    u32 level = 0;
    bool boss = false;
    bool killed_by_invader = false;
    bool hostile = false;
    // Kaniel 2026-10-25: the lamps it used this run (the server only, as cutscene): a spectating
    // teammate wakes once it goes up (LampUseWatch).
    u32 lamp_uses = 0;
    // The bosses (handles) this member's game runs now (BossClaims), so that every game, a fresh
    // one too, picks the same runner (BossRunnerPick). The server only; the direct line keeps the
    // board's.
    std::vector<u32> bosses;
    bool operator==(const MemberVitals&) const = default;
};
// Where the team's host stands (up or down, on a map) on the vitals board, as a resume point.
std::optional<ResumePoint> HostPlaceOf(std::span<const MemberVitals> board, std::string_view host);
struct ReviveHitTotal {
    std::string target;
    u32 down = 0;
    u32 damage = 0;
    bool operator==(const ReviveHitTotal&) const = default;
};
struct PartyWipe {
    u32 id = 0;
    u32 map = 0;
    std::vector<std::string> members;
    bool operator==(const PartyWipe&) const = default;
};
struct VitalsBoard {
    std::vector<MemberVitals> members;
    std::vector<PartyWipe> wipes;
    bool operator==(const VitalsBoard&) const = default;
};
std::string EncodeVitalsUpdate(const MemberVitals& self, std::span<const ReviveHitTotal> hits);
// Kaniel 2026-10-15: who shows in this world comes from one place, the server's map board. Another
// player's character (by position, nothing when unknown) shows while a member other than `self`
// stands on `map` within PresenceMetres of it; else it is hidden (no model, no collision) until
// one does. The vitals board's positions are a little behind: the margin covers that.
constexpr float PresenceMetres = 25.0f;
// A character that stops matching stays shown this long first (a board a beat behind a sprint).
constexpr s64 PresenceGraceMs = 3'000;
// A character the game has just made (a join, a trip's re-create) stands where it was made until
// its member's positions come: that long before it may be hidden at all (a hidden one's update
// does not run, so it would never reach its member; rig 2026-10-22).
constexpr s64 PresenceNewCharacterMs = 15'000;
// Whether a character by no member (since `unmatched_since_ms`), first seen at `first_seen_ms`,
// is hidden now.
bool PresenceHides(s64 now_ms, s64 first_seen_ms, s64 unmatched_since_ms);
std::vector<bool> ShownOtherPlayers(std::span<const std::optional<LampPosition>> characters,
                                    std::span<const MemberVitals> board, std::string_view self,
                                    u32 map);
// The board member (its index) each character shows by, as ShownOtherPlayers pairs them; none for
// a character by no member, or with no position.
std::vector<std::optional<size_t>> VouchingMembers(
    std::span<const std::optional<LampPosition>> characters, std::span<const MemberVitals> board,
    std::string_view self, u32 map);
// Player report 2026-10-23 (v1.1.9): a player who fell into the void woke at the lamp seen by
// itself (its respawn is a trip) but not by the others: their copy of it had faded out in the
// void too (character +0x20C, 1.0 while shown) and only their own map load fades it back. Per
// other character, each frame: once a member on this map died while it was faded out, it fades
// back in (Tick says so, once) when the member it shows by is up again.
class FadeBack {
public:
    bool Tick(float fade, bool someone_died, std::optional<Vital> member);

private:
    bool died = false;
};
std::optional<VitalsBoard> ParseVitalsBoard(std::string_view body);
// Kaniel's playtest 2026-10-19: one member can end up with two characters here (his old copy
// kept after a crash, bound again to his new join). Both take his packets off one queue on two
// threads (a double free). Of the characters with one identity (0: unknown, no one's), all but the
// one first seen last are stale: true.
std::vector<bool> StaleDuplicatePlayers(std::span<const u64> identities,
                                        std::span<const s64> first_seen_ms);
// Whether `address` is in the game's image [base, base + size): where a game object's table is.
bool InGameImage(u64 address, u64 base, u64 size);
// The same at the game's per-character receive (0x01A6D390): of the network states (one per remote
// player's character) with one member identity, only the one first seen last takes the member's
// packets. A state not seen for ForgetMs is forgotten (its character is gone).
class MemberPacketTakers {
public:
    static constexpr s64 ForgetMs = 2'000;
    bool Takes(u64 network, u64 identity, s64 now_ms);

private:
    struct Taker {
        u64 network = 0;
        u64 identity = 0;
        s64 first_ms = 0;
        s64 last_ms = 0;
    };
    std::vector<Taker> takers;
};
// A member's own update (EncodeVitalsUpdate's line), as a peer sends it straight over the link.
struct DirectVitals {
    MemberVitals self;
    std::vector<ReviveHitTotal> hits;
    bool operator==(const DirectVitals&) const = default;
};
std::optional<DirectVitals> ParseVitalsUpdate(std::string_view line);
// The board's answer with the peers' own lines over its copies of them (or added), and each
// member's revive damage the most of the board's and the peers' hits on its current down. The
// wipes stay the board's: shadNet declares each once for everyone.
VitalsBoard MergeDirectVitals(const VitalsBoard& board, std::span<const DirectVitals> direct);
// The wipe on the board that takes this member, if any.
std::optional<u32> WipeFor(const VitalsBoard& board, std::string_view self);
// Whether another member stands on `map` (a usable map): a lethal hit then puts this player down
// instead of wiping the map at once.
bool SomeoneElseStands(std::span<const MemberVitals> board, std::string_view self, u32 map);
// Kaniel 2026-10-16: whether another member stands anywhere (up, on a map). A death that resets
// this member's map is then its own: not the party's wipe (kills stay while anyone is on their
// map).
bool SomeoneElseStandsAnywhere(std::span<const MemberVitals> board, std::string_view self);
// Only the members of this member's session count (rig 2026-10-12, run 132915: a line from a
// member out of it kept a downed party from its wipe): the board's lines of `self` and of the
// `session`'s other members (board names), all of them while the session's names are unknown.
std::vector<MemberVitals> OnlySessionMembers(
    std::span<const MemberVitals> board, std::string_view self,
    const std::optional<std::vector<std::string>>& session);
// The other session members' board names, when every one of them was found.
std::optional<std::vector<std::string>> SessionBoardNames(
    const std::vector<std::optional<std::string>>& found);
// Whether this member is in the session's world, where it stands for party death (same run: a
// guest summoned while the others were going down posted "up" from its own world, on the host's
// map id, through its 7 s summon load, and the host's lethal hit took it as standing). A host is
// there at once; a summoned guest (`summoned`, matching state 6) once a load ends after its join
// (a new `visit`, `playing`), or LimitMs after the join if none comes. Kept through trips (not
// ticked during one).
class SessionArrival {
public:
    static constexpr s64 LimitMs = 30'000;
    bool Tick(bool in_session, bool summoned, u64 visit, bool playing, s64 now_ms);

private:
    std::optional<std::pair<u64, s64>> joined; // the visit and the time of the join
    bool arrived = false;
};

// This player's revive hits: an attack press (R1 or R2 going down) deals its damage
// (ReviveHitDamage) to the nearest downed member within Reach on its map (ReviveTarget), at most
// one every GapMs.
class ReviveHits {
public:
    static constexpr float Reach = 2.5f;
    static constexpr s64 GapMs = 400;

    // An attack press dealing `damage` at `now_ms` on `target`'s down `down`: true if it counts.
    bool Press(std::string_view target, u32 down, u32 damage, s64 now_ms);
    // A real hit on `target`'s body (down `down`) that dealt `damage` (DownedTeams makes it one).
    void Hit(std::string_view target, u32 down, u32 damage);
    // The totals to post: the damage on downs still current on the board (the rest is dropped).
    // A board that is not `fresh` (a hiccup) says nothing about the downs: all are kept.
    std::vector<ReviveHitTotal> Totals(std::span<const MemberVitals> board, bool fresh = true);

private:
    std::vector<ReviveHitTotal> totals;
    std::optional<s64> last_press;
};
struct DownedMember {
    std::string member;
    u32 down = 0;
};
// Swings at a downed body whose hit never lands, counted as the attack presses were before real
// hits: a body that others saw kneel took no hits at all (playtest 2026-10-13, Lisara had to give
// up), so a swing near a downed member (ReviveTarget) waits WaitMs for its hit, and counts its
// press damage when none landed on that member's body. Kaniel's playtest 2026-10-16: every swing
// at Lisara missed her body and each waited 1.5 s, so the revive came late: the wait is short now
// (a hit landing later is still that swing's, Landed, never counted twice).
class RevivePressFallback {
public:
    static constexpr s64 WaitMs = 400;
    // A hit that lands up to LateMs after its swing was counted is that swing's (Landed).
    static constexpr s64 LateMs = 2'000;
    struct Swing {
        DownedMember target;
        u32 damage = 0;
        s64 at = 0;
    };
    void Swung(DownedMember target, u32 damage, s64 now_ms);
    // A real hit on `member`'s body (down `down`) at `now_ms`: the swings at it waiting are its
    // own. True when it belongs to a swing already counted (Due): the caller then counts it not
    // again.
    bool Landed(std::string_view member, u32 down, s64 now_ms);
    // The swings whose hit never landed, to count now (each once).
    std::vector<Swing> Due(s64 now_ms);

private:
    std::vector<Swing> waiting;
    std::vector<Swing> counted; // at: when it was counted
};

// Kaniel's playtest 2026-10-16: Lisara started Gascoigne's fight alone; the host's game, far
// away, ran him (the game never hands a boss out), so her copy only followed its positions (lag)
// and his AI went for nobody near. A boss-sized enemy is run by the member in its arena: the
// nearest standing one within BossEnterMetres, kept while it stays within BossLeaveMetres (down
// too); none: the game's own runner. Every game picks from the same vitals board, ties by name.
// Kaniel 2026-10-20: a boss changing hands mid-fight (a runner kiting past BossLeaveMetres in a
// big arena) restarts its AI on another game. The runner picked keeps it for the whole fight, at
// any distance, until it leaves the map or is dead for good; BossLeaveMetres only says whether the
// game's own runner is in the arena when the fight starts.
constexpr float BossEnterMetres = 25.0f;
constexpr float BossLeaveMetres = 45.0f;
// Kaniel's playtest 2026-10-16 (later): krolebord joined in the middle of a boss fight and the
// boss stood in a T-pose: the pick handed it to him at once, but his fresh game did not run it.
// While the game's own runner (the map's `owner`) is in the arena, the boss stays with it; and a
// stale board keeps the last pick (Last) rather than flipping back.
// Kaniel's playtest 2026-10-19: with the harder enemies (x3 HP) the boss size caught giants in
// Yharnam and dozens of a chalice dungeon's scaled enemies, and the pick handed them out. Only the
// game's bosses (NpcParam npcType 1) are handed out; a row that does not read (`npc_type` none)
// falls back on the boss size.
// Kaniel's playtest 2026-10-25 (v1.2.2): the Shadows of Yharnam stood still for minutes. Each game
// kept the fight's runner in its own memory: Kaniel's game, restarted, picked krolebord (nearest),
// while krolebord's game still remembered the game's own runner (Kaniel's) from before a trip, so
// each followed the other. The pick now keeps nothing of its own: the runner's game says so on the
// board (MemberVitals::bosses, BossClaims), and a claimed boss stays with its claimer (the game's
// own runner first, then by name) while it is on the map, up or down. With no claim: the game's
// own runner if it is in the arena, else the nearest standing member within BossEnterMetres.
constexpr u64 NpcTypeOffset = 0x12E;
constexpr u8 NpcTypeBoss = 1;
bool HandsOutBoss(std::optional<u8> npc_type, bool boss_sized);
class BossRunnerPick {
public:
    // The member whose game runs the boss `handle` at `boss` on `map` now, "" for the game's own
    // (the map's enemy owner, `owner`).
    std::string Tick(u32 map, u32 handle, const LampPosition& boss,
                     std::span<const MemberVitals> board, std::string_view owner = {});
    const std::string& Last() const {
        return current;
    }

private:
    std::string current;
};
// The bosses this member's game ran lately (it was the pick, or the game's own runner with the
// pick empty, and the boss was awake): what it claims on the board. A claim lasts KeepMs after the
// last run, so a boss left behind (a trip, its death, asleep) is let go.
class BossClaims {
public:
    static constexpr s64 KeepMs = 2'000;
    void Ran(u32 handle, s64 now_ms);
    std::vector<u32> Current(s64 now_ms); // sorted

private:
    std::map<u32, s64> last;
};
// Whether this member (`self`) claims an awake boss it sees: picked itself (`runner`), or the
// game's own runner (`runner` "", `own_runner` this game) in the arena (`distance` to it within
// BossLeaveMetres) or with its claim already on the board (`claimed`). A game's own runner far away
// claims nothing, so a member walking in still takes the fight (Lisara's Gascoigne, 2026-10-16).
bool ClaimsBoss(std::string_view runner, std::string_view self, bool own_runner, bool claimed,
                float distance);

// Real revive hits. A character's team is ChrIns +0x88; the game's relation tables
// ([my team][other's]: hit an enemy 0x04731560, hit a friend 0x04731960, hostile 0x05127B90)
// let the host (19) and co-op guests (20) lock on to and hit Hostile (21, the invaders') and keep
// it no friend of theirs, while the enemies' AI (23-25) leaves it be. So on the others' screens a
// downed member's body takes team 21 until it is up; the game itself sets a team on type changes
// (SetTeamTypeDefault, vtable +0x1A0) and never changes a team of 6.
constexpr u64 CharacterTeamOffset = 0x88;
struct CopyTeam {
    u32 handle = 0;
    s32 team = 0;
    bool operator==(const CopyTeam&) const = default;
};
class DownedTeams {
public:
    static constexpr s32 Hostile = 21;
    static constexpr s32 Unchangeable = 6;
    // `downed`: the bodies of downed members in this world, with their team now; `present`: all
    // the other players' bodies in this world. Returns the teams to write.
    // Invasions: `invaders`, the invader's bodies (handles), are never taken or given a team back
    // (its own is the invaders' already: a downed member's saved team handed to it would make it
    // a friend); with `make_hostile` (invasion_fallback_switch) they always get Hostile.
    std::vector<CopyTeam> Tick(const std::vector<CopyTeam>& downed,
                               const std::vector<CopyTeam>& present,
                               std::span<const u32> invaders = {}, bool make_hostile = false);

private:
    std::vector<CopyTeam> saved; // each taken body's own team
};
// Another player's body in this world and where it stands.
struct BodyPosition {
    u32 handle = 0;
    LampPosition at{};
};
struct DownedBodyPick {
    DownedMember member;
    u32 handle = 0;
};
// Which body here is each downed member's on this map. A downed member posts where it went down,
// and its body is then the one nearest that spot; but the killing hit can push it off, and a
// reviver striking it stands nearer. With three players the nearest body flipped between the two,
// the downed one went back to its own team and the punches passed through it (playtest
// 2026-10-13). So a body picked for a down is kept while it stays within Reach of the spot, and
// a new pick lets the members standing up claim the bodies nearest where they posted first.
class DownedBodyPicker {
public:
    static constexpr float Reach = 4.0f;
    std::vector<DownedBodyPick> Pick(std::span<const MemberVitals> board, std::string_view self,
                                     u32 map, std::span<const BodyPosition> bodies);

private:
    std::vector<DownedBodyPick> kept;
};
// The game's hit, where its final damage is stored (`mov [rcx + 0x22C], eax`, 0x01A2A770's): rcx
// the hit record, eax the damage, r14 the victim's damage module (+0x08 the victim), [rbp - 0x318]
// the attacker (null for none). Before any HP changes, and also for a hit on a remote player (whose
// HP its own game takes).
constexpr u64 HitDamageStoredOffset = 0x01A2BCA3;
constexpr std::array<u8, 6> HitDamageStored{0x89, 0x81, 0x2C, 0x02, 0x00, 0x00};
constexpr s64 HitAttackerFrameOffset = -0x318;
// The hit's knockback and reaction dispatcher (0x01A2E570, called twice from the hit 0x01A2A770):
// rdi the victim's damage module (+0x08 the victim), rsi the attacker, rdx the hit. It pushes the
// victim (its knockback module) and requests its flinch, stagger or grab reaction (damage module
// vtable +0xC0); the hit's damage, effects and poise are applied elsewhere. Skipped for the local
// player while it is down: party members' revive strikes (and enemies) no longer push it about or
// make it flinch (Kaniel 2026-10-12: infinite poise while down).
constexpr u64 HitReactionOffset = 0x01A2E570;
constexpr std::array<u8, 8> HitReactionPrologue{0x55, 0x48, 0x89, 0xE5, 0x41, 0x57, 0x41, 0x56};
// Whether a hit on `victim` skips its push and reaction: the local player's own body while down.
bool SkipsHitReaction(bool down, u64 victim, u64 local_player);
// Kaniel's playtest 2026-10-16: the game's enemy bars (8 slots at [0x05562878] +0x10F8) count
// damage per handle (0x016F9A50: table rdi, handle esi, damage edx; +0x88 += damage). A downed
// body on the invaders' team (DownedTeams) gets a bar too, and every drop of its synced HP adds to
// it: an ever-growing number over the body. No damage is counted for a downed body.
constexpr u64 EnemyBarDamageOffset = 0x016F9A50;
constexpr std::array<u8, 8> EnemyBarDamageStart{0x45, 0x31, 0xC0, 0x48, 0x89, 0xF8, 0x31, 0xC9};
bool SkipsEnemyBarDamage(u32 handle, std::span<const u32> downed_bodies);
// The boss bars: BossGaugeSlots at [0x05562878] +0x1658 (BossGaugesOffset), BossGaugeSize bytes
// each: name id, the boss's handle (-1 for none), two numbers and a shown byte (+0x10). The game's
// ClearBossGauge (0x01336D00, also inlined in Failed_BossAreaMission 0x0138B2D0) puts them back
// to none; a summoned guest's death in a boss area runs Failed_BossAreaMission and the host's
// death reload clears them. A party guest revived in the host's world runs neither (Kaniel's
// playtest 2026-10-22: Paarl's bar stayed up at the lamp after a wipe), so a party death clears
// them itself. Returns the handles of the bars that were up.
constexpr u64 BossGaugesPointerOffset = 0x05562878;
constexpr u64 BossGaugesOffset = 0x1658;
constexpr std::size_t BossGaugeSlots = 3;
constexpr std::size_t BossGaugeSize = 0x14;
std::vector<u32> ClearBossGauges(std::span<u8> gauges);
// The handles of the boss bars that are up (as ClearBossGauges reads them), the bars left alone.
std::vector<u32> BossGaugesUp(std::span<const u8> gauges);
// Kaniel 2026-10-25 (guests saw no boss spells): every game makes its own copies' bullets
// (CreateBulletOffset), so while a boss bar is up each game logs what it made, every
// BossFightBulletSeconds: whether a guest's game made a boss's spells at all. By owner and
// BulletParam id.
constexpr s64 BossFightBulletSeconds = 30;
class BulletTally {
public:
    void Add(u32 owner, s32 bullet);
    // "<id> x<count> from <owner>, ..." for the `most` most made, "none" for none; then empty.
    std::string TakeSummary(std::size_t most);

private:
    std::map<std::pair<u32, s32>, u32> counts;
};
std::optional<DownedMember> ReviveTarget(std::span<const MemberVitals> board, std::string_view self,
                                         u32 map, const LampPosition& me);

// The PS4 pad buttons party death reads (the same bits as AutoplayButton's).
constexpr u32 PadOptions = 0x8;
constexpr u32 PadR2 = 0x200;
constexpr u32 PadR1 = 0x800;
constexpr u32 PadCircle = 0x2000;
struct PadInput {
    u32 buttons = 0;
    std::array<u8, 4> sticks{128, 128, 128, 128}; // lx, ly, rx, ry; 128 is centred
    u8 l2 = 0;
    u8 r2 = 0;
};
constexpr u32 PadR3 = 0x4;
// What a downed player's pad keeps: the camera stick only. No menu or inventory (Options), no
// lock-on (R3), no move, attack, roll, item or heal (Kaniel 2026-10-13); Circle still gives up
// (read before this).
PadInput DownedPad(const PadInput& pad);
// The HUD's lock-on cursor (FeData +0x14F8, fed at 0x01BFE559-0x01BFE59C each frame): -1 hidden,
// 0 on a locked-on target, 1 on another mark. A downed player's held lock-on is let go with an R3
// press of ours (the game's own toggle), at most one every LockOnReleaseGapMs while the cursor
// still shows.
constexpr u64 HudLockCursorAt = 0x14F8;
// Kaniel 2026-10-24 (team pings): where the lock cursor is, on the HUD's 1920 x 1080 screen
// (ints): the lock-on manager's +0xCF0 / +0xCF4 written at 0x01BFE568 / 0x01BFE572 each frame
// (960, 540 while it is on another mark).
constexpr u64 HudLockCursorXAt = 0x14FC;
constexpr u64 HudLockCursorYAt = 0x1500;
constexpr u64 HudLockCursorXWrite = 0x01BFE568;
constexpr std::array<u8, 6> HudLockCursorXStore{0x89, 0x88, 0xFC, 0x14, 0x00, 0x00};
constexpr u64 HudLockCursorYWrite = 0x01BFE572;
constexpr std::array<u8, 6> HudLockCursorYStore{0x89, 0x88, 0x00, 0x15, 0x00, 0x00};
// Kaniel 2026-10-24 (team pings): the game's system sounds, its menus' own. 0x01774690 (this:
// [0x05562FD8], esi: the sound) plays one of the ids of its table (60 of them at 0x04733830; any
// other does nothing): 0 the cursor's tick (a list's selection moving, e.g. 0x015CA596), 100 and
// 200 a page's confirm and cancel, 300 an item got (the item popup's push calls it, 0x016D8981),
// 400 a number stepping. A ping coming in plays the cursor's tick: the quietest.
constexpr u64 SystemSoundOffset = 0x01774690;
constexpr std::array<u8, 8> SystemSoundPrologue{0x55, 0x48, 0x89, 0xE5, 0x41, 0x57, 0x41, 0x56};
constexpr u64 SystemSoundPointerOffset = 0x05562FD8;
constexpr s32 SystemSoundCursor = 0;
constexpr s32 SystemSoundItemGot = 300;
constexpr s64 LockOnReleaseGapMs = 600;
bool ReleasesLockOn(s32 cursor, s64 now_ms, std::optional<s64> last_press_ms);
// R1 or R2 going down between two reads of the pad: an attack, which may be a revive hit.
bool AttackPressed(u32 before, u32 now);
// R2 going down without R1: a heavy attack (ReviveHitDamage).
bool HeavyAttackPressed(u32 before, u32 now);

// What ends a down (checked in this order): a wipe of its map, the give-up hold, the party's
// revive hits, or a vitals board unheard of for BoardLostMs (shadNet gone: nobody stays down for
// good). Every end but a revive is a real death.
enum class DownEnd { None, Wipe, GiveUp, Revive, BoardLost, SessionLost };
constexpr s64 BoardLostMs = 10'000;
// Out of the session this long, a down ends (DownEnd::SessionLost).
constexpr s64 DownSessionLostMs = 3'000;
DownEnd EndOfDown(bool wiped, bool gave_up, bool revived, s64 board_age_ms, bool in_session = true);
// Phase 7: a down has the party around while in a session with another member in it (only the
// server ends a session, so one whose host's game died stays, with this game alone in it).
constexpr bool DownHasThePartyAround(bool in_session, size_t session_members) {
    return in_session && session_members >= 2;
}
// Whether a real death in a party session resets its map (see ChooseDeathRespawn): a wipe, or a
// death with nobody else standing on the map (a wipe of one), as a solo death would.
bool DeathResetsMap(bool wiped, bool someone_else_stands);
// What a death's respawn resets: nothing, this member's own map (another member stood anywhere:
// the party's kills stay, Kaniel 2026-10-16), or the party's whole world (a wipe).
enum class DeathReset : u8 { None, OwnMap, PartyWipe };
// Kaniel's playtest 2026-10-18 (enemies kept the HP they had when the party wiped): who stands is
// read when the death comes, not at its respawn. By then (after the death's animation) a member
// that died first already stood again at the lamp, and the last one to die took the party's wipe
// for its own. Written by the party death tick, taken by the respawn hook.
class DeathResetCall {
public:
    // A death came: `reset`, its respawn resets the map (DeathResetsMap); `others_stand`, another
    // member stands anywhere now (SomeoneElseStandsAnywhere).
    void Died(bool reset, bool others_stand) {
        call.store(Waiting | static_cast<u8>(!reset         ? DeathReset::None
                                             : others_stand ? DeathReset::OwnMap
                                                            : DeathReset::PartyWipe));
    }
    // Kaniel's play test 2026-10-25 (Logarius): the member that fell first while the others stood
    // lay dead while the rest of the party fell; its death was no wipe when it came. Each tick
    // until the respawn: nobody else standing on its map (`someone_else_stands`, SomeoneElseStands)
    // makes the respawn reset the map too, the party's wipe unless another member stands anywhere
    // (`others_stand`). Only ever more; true when it changed.
    bool StillDead(bool someone_else_stands, bool others_stand) {
        if (someone_else_stands) {
            return false;
        }
        const u8 now = static_cast<u8>(others_stand ? DeathReset::OwnMap : DeathReset::PartyWipe);
        u8 seen = call.load();
        while ((seen & Waiting) != 0 && (seen & ~Waiting) < now) {
            if (call.compare_exchange_weak(seen, Waiting | now)) {
                return true;
            }
        }
        return false;
    }
    // Up again: that death's respawn came and went.
    void Cancel() {
        call.store(0);
    }
    // At the death's respawn (once).
    DeathReset TakeAtRespawn() {
        return static_cast<DeathReset>(call.exchange(0) & ~Waiting);
    }

private:
    static constexpr u8 Waiting = 0x80; // a death waits for its respawn
    std::atomic<u8> call{0};
};
// Kaniel's playtest 2026-10-18 (a follower took the owner's HP from the log after a wipe: the same
// damaged values as before it). Until the wipe's trip has loaded the map again this game's enemy
// copies are the ones from before the wipe: the area's owner reports none of their HP (the party's
// new log would keep it) and a follower takes none. `trips` is party_trips_started: the wipe's trip
// ends the hold; a wipe with no trip after it holds WipeHpHoldMs at most.
constexpr s64 WipeHpHoldMs = 30'000;
class WipeHpHold {
public:
    void Wiped(u64 trips, s64 now_ms) {
        wiped = std::pair{trips, now_ms};
    }
    bool Holds(u64 trips, s64 now_ms) const {
        return wiped && wiped->first == trips && now_ms - wiped->second < WipeHpHoldMs;
    }

private:
    std::optional<std::pair<u64, s64>> wiped; // (trips, when)
};

// Enemies forget a downed player at once. Hide and Silence only stop them noticing it again; one
// that already targets it keeps a node in its targeting system ([[[enemy +0x3B8] +0xB0] +0x560],
// nodes from +0x18, next at +0x08) until the node's memory timer (+0x2C) runs out, a few seconds
// of swings at the downed player. A node of a live character target (+0x02: slot type 1, bit 7
// alive; +0x28 kind 1) on a downed player's handle (+0x50) gets ForgottenTargetTimer, and the
// game's own update drops it the next frame (0x012A6AD0). Enemies after anyone else are left be.
bool ForgetsTargetNode(u8 slot, s32 kind, u32 handle, std::span<const u32> downed);
constexpr float ForgottenTargetTimer = 1.0e-4f;
// Kaniel's play test 2026-10-19: the Blood-starved Beast never went for Lisara. What the enemies'
// AI reads to take a character as a target: every ~5 s 0x012A43B0 lists WorldChrMan's players
// (and the current block's characters) whose vtable +0x78 says so (players 0x018F4D70:
// [+0x30] +0x18 is 6 and +0x4B5, the model, is built) in [0x0553B098] +0x08; a search
// (0x012ADC50) then skips one whose control ([+0x18]) is disabled (+0x20 bit 0, or bit 2), has
// +0x20 bit 1 without its info's ([control +0x30]) +0x18 bit 0, is not awake (+0x10 not 4), or
// whose info +0x0D has bit 0x10. Its mode (fourth argument) then picks by info +0x18 bit 0: the
// enemies' own searches (0x012AB9C2, 0x012AE150) pass mode 0 and skip a character with the bit
// set, one (0x012ADA78) passes 1 and takes only those. The team table (0x05127B90) is the
// searcher's side and is left out.
struct TargetCandidate {
    s32 state = 0;      // [character +0x30] +0x18
    bool built = false; // character +0x4B5
    bool listed = false;
    u8 control = 0; // control +0x20
    s32 wake = 0;   // control +0x10
    bool has_info = false;
    u8 info_d = 0;  // info +0x0D
    u8 info_18 = 0; // info +0x18
};
// The first rule that leaves `candidate` out of the enemies' targets, "" when none does.
std::string TargetCandidateBlock(const TargetCandidate& candidate);
// Kaniel's play test 2026-10-25: Logarius went for the host only. A player's death sets its
// info's +0x18 bit 0 (all three at a wipe, the local player as it falls into the void), and the
// enemies' searches skip it (TargetCandidate). On the dying game a trip's load clears it; but a
// teammate who died for real and got up again moves on its own game only, so its body here kept
// the bit and no enemy on this game took it as a target until this game loaded a map. A body that
// is up (dead byte +0x1E3 bits 0 and 2 clear, not a downed one on the invaders' team) and still has
// the bit for HoldMs gets it cleared. Invaders' bodies are left be.
class StaleDeathMarks {
public:
    static constexpr s64 HoldMs = 3000;
    struct Body {
        u32 handle = 0;
        bool marked = false; // info +0x18 bit 0
        bool dead = false;   // +0x1E3 bit 0 or 2
        bool downed = false; // on DownedTeams::Hostile
        bool invader = false;
    };
    // The bodies whose mark to clear now.
    std::vector<u32> Tick(std::span<const Body> bodies, s64 now_ms);

private:
    std::vector<std::pair<u32, s64>> up_since; // marked bodies standing up, since when
};
// Where the board has the other members down on `map` (their characters in this world are found
// there: the enemies near a downed guest may be run by the host's game).
std::vector<LampPosition> OthersDownOnMap(std::span<const MemberVitals> board,
                                          std::string_view self, u32 map);

// The party panel the port's overlay draws, after Elden Ring Nightreign's party list: the other
// members playing party death, in the board's order, and the local player's own down.
struct PanelMember {
    std::string member; // its board name
    std::string name;   // its online id, "" when the board name is not one
    // Phase 11: its character's name, as the server knows it ("" before one is known); what the
    // player sees wherever it is known.
    std::string shown;
    Vital vital = Vital::Up;
    bool here = false; // on the local player's map
    s32 hp = 0;
    s32 max = 0;
    u32 landed = 0; // revive progress while down, in damage (ReviveShare)
    u32 needed = 0;
    // Kaniel 2026-10-17 (the compass): where it stands, on the local player's map only.
    std::optional<LampPosition> at;
    // Kaniel 2026-10-17 (the map screen): the map it is on (0 when the board has none).
    u32 map = 0;
    // Kaniel 2026-10-18: its vials and bullets, shown beside its name.
    u32 vials = 0;
    u32 bullets = 0;
    bool operator==(const PanelMember&) const = default;
};
struct PartyPanel {
    std::vector<PanelMember> members;
    s32 revive_target = -1;   // the member an attack would revive now (ReviveTarget), or -1
    Vital vital = Vital::Off; // the local player's, with its revive progress and give-up hold
    u32 landed = 0;
    u32 needed = 0;
    float give_up = 0.0f; // 0 to 1
    // The compass's view: where the local player stands and the camera's heading
    // (CameraHeading), as of the same tick.
    std::optional<LampPosition> me;
    std::optional<float> camera_heading;
    // Kaniel 2026-10-20: the local player's own bloodstain (map 0: none) and its map (0 loading).
    Bloodstain own_stain;
    u32 map = 0;
    // Kaniel 2026-10-25: while the local player spectates, the name of the teammate it watches
    // (none while nobody stands to watch).
    std::optional<std::string> watching;
};
// The panel's members from the board; `me` is where the local player stands while it can revive
// someone (nothing while it is down or dead). The local player's own fields are left to fill.
PartyPanel BuildPartyPanel(std::span<const MemberVitals> board, std::string_view self, u32 map,
                           const std::optional<LampPosition>& me);

} // namespace Core::Bloodborne
