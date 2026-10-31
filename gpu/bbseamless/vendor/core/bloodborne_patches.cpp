// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// The co-op patch table (Sites) and its installer: code patches, hooks, checks. Split from
// bloodborne_re.cpp; the shared declarations are in bloodborne_re_internal.h.

#include "core/bloodborne_crash_dump.h"
#include "core/bloodborne_game_link.h"
#include "core/bloodborne_party.h"
#include "core/bloodborne_re.h"
#include "core/bloodborne_re_internal.h"
#include "core/bloodborne_send_pace.h"
#include "core/bloodborne_team.h"
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
#include <set>
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
#include "core/libraries/np/np_matching2/np_matching2_mm.h"
#include "core/libraries/np/np_matching2/np_matching2_signaling.h"
#include "core/libraries/np/np_signaling/np_signaling.h"
#include "core/libraries/np/np_trophy.h"
#include "core/libraries/system/userservice.h"
#include "core/memory.h"

namespace Core::Bloodborne {
bool NativeVerified109EbootFile();
namespace Coop {

bool negative_area_patch_installed{};
bool area_flag_patch_installed{};
bool responder_bell_area_patch_installed{};
bool responder_bell_common_patch_installed{};
bool active_bell_negative_area_patch_installed{};
bool active_bell_area_flag_patch_installed{};
bool responder_search_negative_area_patch_installed{};
bool sos_status_area_patch_installed{};
bool summon_candidate_area_patch_installed{};
bool summon_build_area_patch_installed{};
bool summon_build_world_state_patch_installed{};
bool summon_build_negative_event_patch_installed{};
bool stage_transition_keep_matching_patch_installed{};
bool world_teardown_hook_installed{};
bool ghost_death_revive_hook_installed{};
bool seamless_travel_hooks_installed{};
bool stop_request_hook_installed{};
bool host_death_respawn_hook_installed{};
bool summon_build_host_placement_hook_installed{};
bool cross_map_guest_handoff_hook_installed{};
bool summon_reload_state_hook_installed{};
bool deferred_summon_reload_hook_installed{};
bool healing_fountain_host_availability_hook_installed{};
bool channel_table_installed{};
bool team_join_ready{};

bool EnvFlagEnabled(const char* name) {
    const char* value = std::getenv(name);
    return value != nullptr && value[0] != '\0' && std::string_view{value} != "0";
}

std::string BytesToHex(std::span<const u8> bytes) {
    std::ostringstream out;
    out << std::hex << std::setfill('0');
    for (const u8 byte : bytes) {
        out << std::setw(2) << static_cast<u32>(byte);
    }
    return out.str();
}

// What the co-op install did with each feature (see CoopFeatureReport), logged in one line at its
// end.
CoopFeatureReport coop_features;

} // namespace Coop

namespace {

void LogCoopFeatures() {
    if (coop_features.SkippedRequired().empty()) {
        LOG_INFO(Debug, "{}", coop_features.Summary());
    } else {
        LOG_ERROR(Debug, "{}", coop_features.Summary());
    }
}

std::span<const u8> EbootImage(u64 base, u64 image_size) {
    return {reinterpret_cast<const u8*>(base), static_cast<size_t>(image_size)};
}

// Whether every site of `feature` holds the bytes the co-op site table has for it; if not, logs
// the first that does not, with what the player loses, and reports the feature skipped.
bool CoopSitesMatch(u64 base, u64 image_size, std::string_view feature,
                    std::string_view consequence) {
    const CoopSite* site = FirstMismatchedCoopSite(EbootImage(base, image_size), feature);
    if (site == nullptr) {
        return true;
    }
    const size_t size = HexBytes(site->original).size();
    const auto actual = site->offset <= image_size && size <= image_size - site->offset
                            ? EbootImage(base, image_size).subspan(site->offset, size)
                            : std::span<const u8>{};
    LOG_ERROR(Debug, "Bloodborne {} signature mismatch at {:#x} ({}): expected={} actual={}; {}",
              feature, site->offset, site->name, site->original, BytesToHex(actual), consequence);
    coop_features.Skipped(feature, fmt::format("signature mismatch at {:#x}", site->offset));
    return false;
}

} // namespace

namespace Coop {

// The first site of `feature` the game does not hold as the table has it, as a reason.
std::string FeatureMismatch(u64 base, u64 image_size, std::string_view feature) {
    const CoopSite* site = FirstMismatchedCoopSite(EbootImage(base, image_size), feature);
    return site != nullptr ? fmt::format("signature mismatch at {:#x}", site->offset)
                           : std::string{"not found"};
}

// Why a hook at `offset` did not go in: the game's bytes there, or the hook itself.
std::string SiteFailure(u64 base, u64 image_size, u64 offset) {
    const std::span<const u8> image = EbootImage(base, image_size);
    for (const CoopSite& site : CoopSites) {
        const size_t size = HexBytes(site.original).size();
        if (site.offset == offset && site.kind == CoopSiteKind::Hook &&
            (offset > image.size() || size > image.size() - offset ||
             !MatchesCoopSite(image.subspan(offset, size), site))) {
            return fmt::format("signature mismatch at {:#x}", offset);
        }
    }
    return fmt::format("hook failed at {:#x}", offset);
}

} // namespace Coop

namespace {

void WriteCoopPatches(u64 base, std::string_view feature) {
    for (const CodePatch& patch : CoopPatchesOf(feature)) {
        std::memcpy(reinterpret_cast<void*>(base + patch.offset), patch.patched.data(),
                    patch.patched.size());
    }
}

// The guest code hooks of co-op, each checked against the co-op site table first: the launcher
// warns about game patches over the sites it lists, so a hook missing from it is a bug (logged,
// and still installed).
void CheckCoopHookRegistered(const void* address, std::span<const u8> bytes) {
    const u64 offset = reinterpret_cast<u64>(address) - MemoryPatcher::g_eboot_address;
    if (!IsRegisteredCoopHook(offset, bytes)) {
        LOG_ERROR(Debug, "Bloodborne hook at {:#x} ({} bytes) is not in the co-op site table",
                  offset, bytes.size());
    }
}

} // namespace

namespace Coop {

bool InstallCoopHook(void* address, std::span<const u8> bytes, u64 tag, GuestCodeHook hook) {
    CheckCoopHookRegistered(address, bytes);
    return InstallGuestCodeHook(address, bytes, tag, hook);
}

bool InstallCoopHook(void* address, std::span<const u8> bytes, u64 tag,
                     GuestFunctionOverride hook) {
    CheckCoopHookRegistered(address, bytes);
    return InstallGuestFunctionOverride(address, bytes, tag, hook);
}

bool InstallCoopHook(void* address, std::span<const u8> bytes, u64 tag, GuestFunctionResult hook) {
    CheckCoopHookRegistered(address, bytes);
    return InstallGuestFunctionResult(address, bytes, tag, hook);
}

bool InstallCoopHook(void* address, std::span<const u8> bytes, u64 tag,
                     GuestRegisterEditHook hook) {
    CheckCoopHookRegistered(address, bytes);
    return InstallGuestRegisterEditHook(address, bytes, tag, hook);
}

} // namespace Coop

namespace {

bool MatchesImage(u64 base, u64 image_size, const CodePatch& patch) {
    return patch.offset <= image_size && patch.original.size() <= image_size - patch.offset &&
           std::ranges::equal(std::span<const u8>{reinterpret_cast<const u8*>(base + patch.offset),
                                                  patch.original.size()},
                              patch.original);
}

// Deaths that keep the party: a dead guest revives in the host's world instead of being sent
// home. Optional and all-or-nothing: the byte patches alone would leave the guest dead.
void InstallGhostDeathRevive(u64 base, u64 image_size) {
    const auto& patches = GhostDeathKeepSessionPatches();
    for (const auto& patch : patches) {
        if (!MatchesImage(base, image_size, patch)) {
            LOG_ERROR(Debug,
                      "Bloodborne ghost-death signature mismatch at {:#x}; deaths still send "
                      "guests home",
                      patch.offset);
            coop_features.Skipped("ghost death",
                                  fmt::format("signature mismatch at {:#x}", patch.offset));
            return;
        }
    }
    const auto site = std::ranges::find_if(
        Sites, [](const TraceSite& site) { return site.offset == GhostDeathReviveOffset; });
    if (site == Sites.end() ||
        !InstallCoopHook(reinterpret_cast<void*>(base + GhostDeathReviveOffset),
                         std::span<const u8>{site->prologue.data(), site->prologue_size},
                         static_cast<size_t>(std::distance(Sites.begin(), site)), TraceEntry)) {
        LOG_ERROR(Debug, "Failed to install Bloodborne ghost-death revive hook at {:#x}",
                  GhostDeathReviveOffset);
        coop_features.Skipped("ghost death", SiteFailure(base, image_size, GhostDeathReviveOffset));
        return;
    }
    ghost_death_revive_hook_installed = true;
    WriteCoopPatches(base, "ghost death");
    coop_features.On("ghost death");
    // Optional: without it the guest's stain is still dropped, it just can't pick it up.
    if (CoopSitesMatch(base, image_size, "guest bloodstain",
                       "a guest cannot pick up its own bloodstain")) {
        WriteCoopPatches(base, "guest bloodstain");
        coop_features.On("guest bloodstain");
    }
    LOG_INFO(Debug, "Bloodborne ghost-death patch: party guests revive in the host's world");
}

// See NoEchoTheftPatches: no enemy steals a bloodstain, in or out of a party.
void InstallNoEchoTheft(u64 base, u64 image_size) {
    if (!CoopSitesMatch(base, image_size, "no echo theft", "enemies steal bloodstains")) {
        return;
    }
    WriteCoopPatches(base, "no echo theft");
    coop_features.On("no echo theft");
    LOG_INFO(Debug, "Bloodborne no echo theft: enemies leave bloodstains where they are");
}

// A host death with a party revives the host like a guest (no world reload) and the guests skip
// HostDead's warp home. All-or-nothing: guests that stay with a host that reloads its world
// would be stranded.
void InstallHostDeathKeepsGuests(u64 base, u64 image_size) {
    if (!world_teardown_hook_installed) {
        LOG_ERROR(Debug, "Bloodborne host-death patch unavailable; host deaths send guests home");
        coop_features.Skipped("host death", "needs world teardown");
        return;
    }
    if (!CoopSitesMatch(base, image_size, "host death", "host deaths send guests home")) {
        return;
    }
    const auto site = std::ranges::find_if(
        Sites, [](const TraceSite& site) { return site.offset == SoloPlayDeathRespawnOffset; });
    if (site == Sites.end() ||
        !InstallCoopHook(reinterpret_cast<void*>(base + SoloPlayDeathRespawnOffset),
                         std::span<const u8>{site->prologue.data(), site->prologue_size},
                         static_cast<size_t>(std::distance(Sites.begin(), site)),
                         OverrideHostDeathRespawn)) {
        LOG_ERROR(Debug, "Failed to install Bloodborne host-death respawn hook at {:#x}",
                  SoloPlayDeathRespawnOffset);
        coop_features.Skipped("host death",
                              SiteFailure(base, image_size, SoloPlayDeathRespawnOffset));
        return;
    }
    host_death_respawn_hook_installed = true;
    WriteCoopPatches(base, "host death");
    coop_features.On("host death");
    LOG_INFO(Debug, "Bloodborne host-death patch: a host with a party revives without a reload");
}

// A boss kill keeps the party together: nobody leaves the session and the guest is not warped
// home. All-or-nothing: half of it would leave a guest in a session its host already left.
void InstallBossClearKeepsSession(u64 base, u64 image_size) {
    if (!CoopSitesMatch(base, image_size, "boss clear", "boss kills end sessions")) {
        return;
    }
    WriteCoopPatches(base, "boss clear");
    coop_features.On("boss clear");
    LOG_INFO(Debug, "Bloodborne boss-clear patch: a boss kill keeps the session");
}

// A party joins in any area, whatever the player's own world has opened (see JoinAnywherePatches).
void InstallJoinAnywhere(u64 base, u64 image_size) {
    if (!party_controller) {
        return;
    }
    if (!CoopSitesMatch(base, image_size, "join anywhere", "closed areas stay closed")) {
        return;
    }
    WriteCoopPatches(base, "join anywhere");
    coop_features.On("join anywhere");
    LOG_INFO(Debug, "Bloodborne join-anywhere patch: a party joins in any area");
}

// See DeathFlagSites: logs a big character's death and the game's call chain to it.
void PS4_SYSV_ABI TraceCharacterDeath(u64 tag, GuestRegisterSnapshot* registers) {
    const auto& sites = DeathFlagSites();
    if (tag >= sites.size()) {
        return;
    }
    const DeathFlagSite& site = sites[tag];
    const u64 character = site.character == DeathRegister::Rbx   ? registers->rbx
                          : site.character == DeathRegister::Rcx ? registers->rcx
                                                                 : registers->r15;
    const u64 modules = ReadValue<u64>(character, 0x3B0);
    const u64 data = modules >= 0x10000 ? ReadValue<u64>(modules, 0x20) : 0;
    const s32 max_hp = data >= 0x10000 ? ReadValue<s32>(data, 0xFC) : 0;
    if (!TracesDeathOf(max_hp)) {
        return;
    }
    LOG_INFO(Debug,
             "Bloodborne boss death: {:#x} ({}) hp {}/{} flags {:#x} map {:#x} in session {}",
             ReadValue<u32>(character, 0x08), site.name, ReadValue<s32>(data, 0xF8), max_hp,
             ReadValue<u64>(character, 0x1E0), CurrentPlace().map, InPartySession());
    LogGameCallChainAt("boss death", registers->rsp);
}

void InstallBossDeathTracer(u64 base, u64 image_size) {
    if (!party_controller) {
        return;
    }
    const auto& sites = DeathFlagSites();
    for (u64 index = 0; index < sites.size(); ++index) {
        const DeathFlagSite& site = sites[index];
        if (site.offset + site.instruction.size() > image_size ||
            !InstallCoopHook(reinterpret_cast<void*>(base + site.offset), site.instruction, index,
                             TraceCharacterDeath)) {
            LOG_ERROR(Debug, "Bloodborne boss-death tracer unavailable at {:#x}", site.offset);
            coop_features.Skipped("boss death tracer", SiteFailure(base, image_size, site.offset));
            return;
        }
    }
    coop_features.On("boss death tracer");
    LOG_INFO(Debug, "Bloodborne boss-death tracer: big characters' deaths are logged with their "
                    "call chain");
}

// See CreateBulletOffset: every bullet a game makes goes in the boss fight's tally
// (LogBossFightBullets); the rig's games (autoplay) also log the first BulletTraceLines each with
// its request (owner, BulletParam id, the spawn matrix).
constexpr u64 BulletTraceLines = 3000;
std::atomic<u64> bullets_made{0};
bool bullets_logged_each = false;
std::mutex bullet_tally_mutex;
BulletTally bullet_tally;
std::map<s32, u64> bullets_by_id; // every bullet this run made, by BulletParam id (wait-bullets)

void PS4_SYSV_ABI TraceBullet(u64, const GuestRegisterSnapshot* registers) {
    const u64 request = registers->rsi;
    if (!HasMemoryAccess(request, 0x90, MemoryProt::CpuRead)) {
        return;
    }
    {
        std::scoped_lock lock{bullet_tally_mutex};
        bullet_tally.Add(ReadValue<u32>(request, BulletRequestOwnerOffset),
                         ReadValue<s32>(request, BulletRequestIdOffset));
        ++bullets_by_id[ReadValue<s32>(request, BulletRequestIdOffset)];
    }
    const u64 made = bullets_made.fetch_add(1) + 1;
    if (!bullets_logged_each || made > BulletTraceLines) {
        return;
    }
    std::string fields;
    for (u64 offset = 0x08; offset < 0x2C; offset += 4) {
        fields += fmt::format(" {:x}", ReadValue<u32>(request, offset));
    }
    std::string at;
    for (u64 offset = 0x30; offset < 0x90; offset += 4) {
        at += fmt::format(" {:.2f}", ReadValue<float>(request, offset));
    }
    LOG_INFO(Debug, "Bloodborne bullet: {} from {:#x} (#{} in this game) fields{} at{}",
             ReadValue<s32>(request, BulletRequestIdOffset),
             ReadValue<u32>(request, BulletRequestOwnerOffset), made, fields, at);
}

void InstallBulletTracer(u64 base, u64 image_size) {
    if (!party_controller ||
        !CoopSitesMatch(base, image_size, "bullet tracer", "bullets are not logged")) {
        return;
    }
    const char* autoplay = std::getenv("SHADPS4_BLOODBORNE_AUTOPLAY");
    bullets_logged_each = autoplay != nullptr && *autoplay != '\0';
    if (!InstallCoopHook(reinterpret_cast<void*>(base + CreateBulletOffset), CreateBulletPrologue,
                         0, TraceBullet)) {
        coop_features.Skipped("bullet tracer", "the hook did not go in");
        return;
    }
    coop_features.On("bullet tracer");
}

} // namespace

namespace Coop {

u64 BulletsMade(s32 bullet) {
    std::scoped_lock lock{bullet_tally_mutex};
    const auto it = bullets_by_id.find(bullet);
    return it == bullets_by_id.end() ? 0 : it->second;
}

// See BossFightBulletSeconds: while a boss bar is up, what this game made, every 30 s.
void LogBossFightBullets() {
    static s64 last_ms = SteadyMilliseconds();
    const s64 now_ms = SteadyMilliseconds();
    if (now_ms - last_ms < BossFightBulletSeconds * 1000) {
        return;
    }
    last_ms = now_ms;
    const u64 holder = ReadValue<u64>(image_base + BossGaugesPointerOffset, 0);
    constexpr std::size_t Size = BossGaugeSlots * BossGaugeSize;
    const std::vector<u32> bars =
        holder >= 0x10000 && HasMemoryAccess(holder + BossGaugesOffset, Size, MemoryProt::CpuRead)
            ? BossGaugesUp({reinterpret_cast<const u8*>(holder + BossGaugesOffset), Size})
            : std::vector<u32>{};
    std::string made;
    {
        std::scoped_lock lock{bullet_tally_mutex};
        made = bullet_tally.TakeSummary(8);
    }
    if (!bars.empty()) {
        LOG_INFO(Debug, "Bloodborne boss fight bullets in {} s (bars {:#x}): {}",
                 BossFightBulletSeconds, fmt::join(bars, " "), made);
    }
}

} // namespace Coop

namespace {

// See PacketTraceMode: the game's own packets on the co-op trace, nothing changed.
PacketTraceMode packet_trace_mode = PacketTraceMode::Off;

// A member as the trace names it: its online id where its identity keeps it (the SceNpId, +0x36),
// else the identity's address. Looked up once per identity.
std::string TracedPeer(u64 identity) {
    static std::mutex mutex;
    static std::unordered_map<u64, std::string> names;
    std::scoped_lock lock{mutex};
    const auto [it, added] = names.try_emplace(identity);
    if (added) {
        std::optional<std::string> id;
        if (identity >= 0x10000 && HasMemoryAccess(identity + 0x36, 17, MemoryProt::CpuRead)) {
            id = NpOnlineIdAt({reinterpret_cast<const u8*>(identity + 0x36), 17});
        }
        it->second = id.value_or(fmt::format("{:x}", identity));
    }
    return it->second;
}

void TraceNativePacket(bool sent, u32 channel, u64 member_id, u64 payload, u32 size) {
    NativePacketRecord record{sent, channel, size, TracedPeer(ReadValue<u64>(member_id, 0)),
                              GetCurrentPackedMap()};
    const u64 net_sync = ReadValue<u64>(image_base + SummonSessionRulesPointerOffset, 0);
    const u64 session = GetMatchingState();
    record.sync_state = net_sync >= 0x10000 ? ReadValue<s32>(net_sync, 0x84) : -1;
    record.matching_state = session >= 0x10000 ? ReadValue<s32>(session, MatchingStateOffset) : -1;
    const std::size_t shown = TracedPayloadBytes(packet_trace_mode, channel, size);
    if (shown > 0 && payload >= 0x10000 && HasMemoryAccess(payload, shown, MemoryProt::CpuRead)) {
        const auto* bytes = reinterpret_cast<const u8*>(payload);
        record.payload.assign(bytes, bytes + shown);
    }
    Trace(NativePacketTraceKind(sent), NativePacketFields(record));
}

// 0x0178970D in the send (0x017894E0), after its checks, as it hands the packet to the
// transport: rcx the packet (the static buffer 0x055640F0), rdx the member id.
void PS4_SYSV_ABI TracePacketSent(u64, const GuestRegisterSnapshot* registers) {
    const u64 packet = registers->rcx;
    if (packet < 0x10000 || !HasMemoryAccess(packet, 8, MemoryProt::CpuRead)) {
        return;
    }
    const u32 size = ReadValue<u32>(packet, 4);
    TraceNativePacket(true, ReadValue<u32>(packet, 0), registers->rdx, packet + 8, size);
}

// A whole packet queued for its member (0x01790C20, rbx the member's queues, their member id at
// +0x08): reliable ones (0x01790E0E) once reassembled, channel rax, the node {u32 size, payload*}
// at +0x68; unreliable ones (0x01790FCA) channel [rbp - 0x40], node [rbp - 0x38].
void PS4_SYSV_ABI TracePacketReceived(u64 tag, const GuestRegisterSnapshot* registers) {
    const bool reliable = tag == 0;
    const u64 queues = registers->rbx;
    const u32 channel =
        reliable ? static_cast<u32>(registers->rax) : ReadValue<u32>(registers->rbp - 0x40, 0);
    const u64 node =
        reliable ? ReadValue<u64>(queues, 0x68) : ReadValue<u64>(registers->rbp - 0x38, 0);
    if (node < 0x10000) {
        return;
    }
    TraceNativePacket(false, channel, queues + 0x08, ReadValue<u64>(node, 0x08),
                      ReadValue<u32>(node, 0x00));
}

void InstallNativePacketTrace(u64 base, u64 image_size) {
    packet_trace_mode = ParsePacketTraceMode(std::getenv("SHADPS4_BLOODBORNE_PACKET_TRACE"));
    if (packet_trace_mode == PacketTraceMode::Off || !CoopTraceOn()) {
        return;
    }
    if (!CoopSitesMatch(base, image_size, "native packet trace", "game packets are not traced")) {
        return;
    }
    const bool installed =
        InstallCoopHook(reinterpret_cast<void*>(base + PacketSendTraceOffset), PacketSendTrace, 0,
                        TracePacketSent) &&
        InstallCoopHook(reinterpret_cast<void*>(base + ReliablePacketQueuedOffset),
                        ReliablePacketQueued, 0, TracePacketReceived) &&
        InstallCoopHook(reinterpret_cast<void*>(base + UnreliablePacketQueuedOffset),
                        UnreliablePacketQueued, 1, TracePacketReceived);
    if (!installed) {
        coop_features.Skipped("native packet trace", "a hook did not go in");
        return;
    }
    coop_features.On("native packet trace");
    LOG_INFO(Debug, "Bloodborne native packet trace: on ({})",
             packet_trace_mode == PacketTraceMode::Full ? "every payload" : "unknown payloads");
}

// A member's name on the boards (`member_id` the game's {identity*, lock}), or "".
std::string BoardMemberOfMemberId(u64 member_id) {
    const u64 identity = member_id >= 0x10000 ? ReadValue<u64>(member_id, 0) : 0;
    if (identity < 0x10000 || !HasMemoryAccess(identity + 0x36, 17, MemoryProt::CpuRead)) {
        return {};
    }
    const auto online_id = NpOnlineIdAt({reinterpret_cast<const u8*>(identity + 0x36), 17});
    return online_id ? BoardMemberOfOnlineId(*online_id) : std::string{};
}

// Phase 4's enemy owners are on (see InstallEnemyOwner).
bool enemy_owner_installed = false;

std::string OnlineIdOfMember(u64 member_id); // below, with the game link

bool SendHitToOwner(const GuestRegisterSnapshot* registers); // below, with the game link

// The send's entry (0x017894E0: rdi the net sync, rsi the member id, edx the channel, rcx the
// payload, r8d its size): true returns 0, "not sent", as the game's own checks do. Which member
// gets a packet is shadNet's (coop/packet_routes.h); here the game decides what it sends at all
// (see ChannelPolicy), and a follower's hit and near list go to the area's owner instead of the
// session's host.
bool PS4_SYSV_ABI DecidePacketSend(u64, const GuestRegisterSnapshot* registers) {
    const u32 channel = static_cast<u32>(registers->rdx);
    if (enemy_owner_installed && channel == 0x17 && CurrentEnemyRole() == EnemyRole::Owner) {
        const u32 size = static_cast<u32>(registers->r8);
        if (size >= 4 && size <= 0x2004 &&
            HasMemoryAccess(registers->rcx, size, MemoryProt::CpuRead)) {
            NoteHandOutSent(OnlineIdOfMember(registers->rsi),
                            {reinterpret_cast<u8*>(registers->rcx), size},
                            HasMemoryAccess(registers->rcx, size, MemoryProt::CpuWrite));
        }
    }
    if (enemy_owner_installed) {
        switch (EnemyPacketOut(channel, CurrentEnemyRole())) {
        case EnemyPacketFate::ToOwner:
            if (SendHitToOwner(registers)) {
                CountEnemyPacketSent(channel);
                return true;
            }
            break; // no link to the owner: the game's own way (shadNet routes it)
        case EnemyPacketFate::Drop:
            return true;
        case EnemyPacketFate::Keep:
            break;
        }
    }
    const ChannelPolicy policy = NativeChannelPolicy(channel);
    if (policy == ChannelPolicy::Ours || policy == ChannelPolicy::Drop) {
        static std::array<std::atomic<bool>, NativeChannelCount> logged{};
        if (channel < NativeChannelCount && !logged[channel].exchange(true)) {
            LOG_INFO(Debug, "Bloodborne channel table: first packet on channel {:#x} not sent ({})",
                     channel, ChannelPolicyName(policy));
        }
        return true;
    }
    CountEnemyPacketSent(channel);
    return false;
}

// The receive's channel checks in 0x01790C20 (rbx / rax the reliable and unreliable channel): a
// hit on an enemy this game runs makes its enemies step at once.
void PS4_SYSV_ABI DecidePacketReceived(u64 tag, GuestRegisterSnapshot* registers) {
    const u32 channel =
        tag == 0 ? ReadValue<u32>(registers->rbx, 0x64) : static_cast<u32>(registers->rax);
    if (channel == 0x15 && enemy_owner_installed && CurrentEnemyRole() == EnemyRole::Owner) {
        ForceEnemySteps();
    }
    if (channel == PositionChannel && enemy_owner_installed) {
        CountPositionPacketIn();
    }
}

void InstallChannelTable(u64 base, u64 image_size) {
#ifdef BLOODBORNE_NATIVE_PORT
    // Always with the port's co-op (phase 6: no switch); phase 4's enemy owners decide at the
    // table's hooks too.
    const bool wanted = party_controller.has_value();
#else
    // The port only (docs/SERVER_COOP_PLAN.md); shadPS4 keeps the old path.
    const bool wanted = false;
#endif
    if (!wanted) {
        return;
    }
    if (!CoopSitesMatch(base, image_size, "channel table", "packets keep the old path")) {
        return;
    }
    const bool installed =
        InstallCoopHook(reinterpret_cast<void*>(base + PacketSendOffset), PacketSendPrologue, 0,
                        DecidePacketSend) &&
        InstallCoopHook(reinterpret_cast<void*>(base + ReliableChannelCheckOffset),
                        ReliableChannelCheck, 0, DecidePacketReceived) &&
        InstallCoopHook(reinterpret_cast<void*>(base + UnreliableChannelCheckOffset),
                        UnreliableChannelCheck, 1, DecidePacketReceived);
    if (!installed) {
        coop_features.Skipped("channel table", "a hook did not go in");
        return;
    }
    channel_table_installed = true;
    coop_features.On("channel table");
    LOG_INFO(Debug, "Bloodborne channel table: on (shadNet routes the packets; a follower's hits "
                    "go to the enemies' owner)");
}

// See bloodborne_game_link.h: with the server link on, the game's packets leave at its transport
// send in our frames (SHAD GamePacketsType, through the link) and come back into the member's
// queues on the game's thread; the game's own transport sends nothing (phase 9).
struct InboundGamePacket {
    std::string member; // the sender's online id
    PacketClass packet_class;
    std::vector<u8> packet;
    s64 received_ms;
};

struct GamePacketLink {
    std::mutex mutex;
    std::string self;
    u32 instance = 0;
    std::map<std::string, GameLinkPeer> peers;
    std::deque<InboundGamePacket> inbound;
    std::map<std::string, bool> reported_silent;
    // Where each member's frames go (signaling's address for it, else shadNet's), refreshed on the
    // link's thread; a member not found yet is asked for there.
    std::map<std::string, std::pair<u32, u16>> addresses;
    std::set<std::string> unresolved;
    u64 sent = 0;
    u64 no_member = 0;      // packets for a member without an online id: dropped
    u64 dropped_routed = 0; // reliable packets shadNet dropped (DroppedNativeChannel)
    u64 taken = 0;
    u64 dropped_unknown = 0;
};
GamePacketLink game_link;
bool game_link_installed = false;

s64 GameLinkNowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

// A member id's online id ({identity*, lock}, the identity's SceNpId at +0x36), or "".
std::string OnlineIdOfMember(u64 member_id) {
    const u64 identity = member_id >= 0x10000 ? ReadValue<u64>(member_id, 0) : 0;
    if (identity < 0x10000 || !HasMemoryAccess(identity + 0x36, 17, MemoryProt::CpuRead)) {
        return {};
    }
    return NpOnlineIdAt({reinterpret_cast<const u8*>(identity + 0x36), 17}).value_or("");
}

GameLinkPeer& GameLinkPeerOf(const std::string& member) {
    auto found = game_link.peers.find(member);
    if (found == game_link.peers.end()) {
        found = game_link.peers.try_emplace(member, game_link.self, game_link.instance).first;
    }
    return found->second;
}

// 0x00C8F0A0's entry: every packet is taken ("sent"); never the game's own transport (phase 9:
// nothing falls back to it). A member whose address is not known yet gets its packets once it is;
// one the game names without an online id gets nothing.
bool PS4_SYSV_ABI TakeGamePacket(u64, GuestRegisterSnapshot* registers) {
    const u64 packet = registers->rcx;
    const u32 size = static_cast<u32>(registers->r8);
    if (packet < 0x10000 || size < 8 || size > 0x6400 + 8 ||
        !HasMemoryAccess(packet, size, MemoryProt::CpuRead)) {
        return false;
    }
    const std::string member = OnlineIdOfMember(registers->rdx);
    const auto packet_class =
        static_cast<u32>(registers->rsi) != 0 ? PacketClass::Reliable : PacketClass::Unreliable;
    {
        std::scoped_lock lock{game_link.mutex};
        if (game_link.self.empty()) {
            game_link.self = LocalOnlineId();
        }
        if (member.empty() || game_link.self.empty()) {
            ++game_link.no_member;
        } else {
            if (!game_link.addresses.contains(member)) {
                game_link.unresolved.insert(member);
            }
            GameLinkPeerOf(member).Send(packet_class, {reinterpret_cast<const u8*>(packet), size},
                                        GameLinkNowMs());
            ++game_link.sent;
        }
    }
    registers->rax = 1;
    return true;
}

// A follower's hit on an enemy (ch 0x15, 12 bytes {chr id, damage, poise}) to the area's owner,
// through our link (the game sends it to the session's host): true when it went.
bool SendHitToOwner(const GuestRegisterSnapshot* registers) {
    const std::string owner = CurrentEnemyOwner();
    const u64 payload = registers->rcx;
    const u32 size = static_cast<u32>(registers->r8);
    if (owner.empty() || payload < 0x10000 || size == 0 || size > 0x1000 ||
        !HasMemoryAccess(payload, size, MemoryProt::CpuRead)) {
        return false;
    }
    std::vector<u8> packet(8 + size);
    const u32 channel = static_cast<u32>(registers->rdx);
    std::memcpy(packet.data(), &channel, 4);
    std::memcpy(packet.data() + 4, &size, 4);
    std::memcpy(packet.data() + 8, reinterpret_cast<const void*>(payload), size);
    std::scoped_lock lock{game_link.mutex};
    if (game_link.self.empty()) {
        return false;
    }
    for (const auto& [member, address] : game_link.addresses) {
        if (BoardMemberOfOnlineId(member) == owner) {
            // Reliable, as the game sends channels above 6.
            GameLinkPeerOf(member).Send(PacketClass::Reliable, packet, GameLinkNowMs());
            ++game_link.sent;
            static std::atomic<u32> logged{0};
            if (logged.fetch_add(1) < 10) {
                std::string bytes;
                for (u32 i = 0; i < size; ++i) {
                    bytes += fmt::format("{:02x}", ReadValue<u8>(payload, i));
                }
                LOG_INFO(Debug,
                         "Bloodborne enemy owner: channel {:#x} ({:#x}) went to the owner {} ({})",
                         channel, ReadValue<u32>(payload, 0), owner, bytes.substr(0, 64));
            }
            return true;
        }
    }
    return false;
}

// The owner's full position packet (see SendsFullPositionPacket) to every other member of the
// session, as the game's own (ch 4, unreliable): the game's loop then fills the next one.
void PS4_SYSV_ABI SendFullPositionPacket(u64, GuestRegisterSnapshot* registers) {
    static u64 frame = 0;
    static u32 sent_this_frame = 0;
    const u64 now_frame = EnemyFrameSerial();
    if (now_frame != frame) {
        frame = now_frame;
        sent_this_frame = 0;
    }
    const u64 sync = registers->r12;
    const u64 slots = registers->r15;
    if (sync < 0x10000 || slots < 0x10000 ||
        !HasMemoryAccess(sync + PositionPacketOffset, PositionPacketBytes, MemoryProt::CpuRead) ||
        !SendsFullPositionPacket(CurrentEnemyRole(), static_cast<s32>(registers->rbx),
                                 static_cast<s32>(registers->r14),
                                 ReadValue<s32>(sync, PositionSlotOffset),
                                 ReadValue<s32>(slots, 0x10), sent_this_frame)) {
        return;
    }
    const u64 session = ReadValue<u64>(image_base + SessionManagerPointerOffset, 0);
    const u64 first = session >= 0x10000 ? ReadValue<u64>(session, 0xF8) : 0;
    const u64 end = session >= 0x10000 ? ReadValue<u64>(session, 0x100) : 0;
    if (first < 0x10000 || end <= first || end - first > 0x20 * 8) {
        return;
    }
    std::vector<u8> packet(8 + PositionPacketBytes);
    std::memcpy(packet.data(), &PositionChannel, 4);
    std::memcpy(packet.data() + 4, &PositionPacketBytes, 4);
    std::memcpy(packet.data() + 8, reinterpret_cast<const void*>(sync + PositionPacketOffset),
                PositionPacketBytes);
    u32 members = 0;
    {
        std::scoped_lock lock{game_link.mutex};
        if (game_link.self.empty()) {
            return;
        }
        for (u64 entry = first; entry < end; entry += 0x20) {
            const std::string member = OnlineIdOfMember(entry);
            if (member.empty() || member == game_link.self) {
                continue;
            }
            if (!game_link.addresses.contains(member)) {
                game_link.unresolved.insert(member);
            }
            GameLinkPeerOf(member).Send(PacketClass::Unreliable, packet, GameLinkNowMs());
            ++game_link.sent;
            ++members;
        }
    }
    if (members == 0) {
        return;
    }
    CountEnemyPacketSent(PositionChannel);
    ++sent_this_frame;
    registers->rbx = 0; // the game fills a new packet from its first entry
    static std::atomic<u32> logged{0};
    if (logged.fetch_add(1) < 3) {
        LOG_INFO(Debug,
                 "Bloodborne enemy owner: sent a full position packet to {} member(s), the round "
                 "goes on (slot {} of {})",
                 members, ReadValue<s32>(sync, PositionSlotOffset), ReadValue<s32>(slots, 0x10));
    }
}

// Phase 4's hooks (see EnemyRole and the offsets beside it). Stand-ins of SprjSessionManager for
// one read of its state (+0x124): the host's and a guest's.
struct SessionStateStandIn {
    alignas(16) std::array<u8, 0x130> bytes{};
    explicit SessionStateStandIn(s32 state) {
        std::memcpy(bytes.data() + MatchingStateOffset, &state, sizeof(state));
    }
    u64 Address() const {
        return reinterpret_cast<u64>(bytes.data());
    }
};
const SessionStateStandIn hosting_stand_in{MatchingStateHosting};
const SessionStateStandIn guest_stand_in{MatchingStateSummonedGuest};

u64 StandInFor(s32 state) {
    return state == MatchingStateHosting ? hosting_stand_in.Address() : guest_stand_in.Address();
}

// The game's thread runs the HP sync: the session manager the stand-in replaced, put back on the
// branch taken.
thread_local u64 enemy_hp_session = 0;

void PS4_SYSV_ABI SetEnemyHpRoundState(u64, GuestRegisterSnapshot* registers) {
    const s32 state = ReadValue<s32>(registers->rax, MatchingStateOffset);
    if (const auto wanted = EnemySyncState(CurrentEnemyRole(), state)) {
        constexpr u64 ZeroFlag = u64{1} << 6;
        registers->rflags = *wanted == MatchingStateHosting ? (registers->rflags | ZeroFlag)
                                                            : (registers->rflags & ~ZeroFlag);
    }
}

void PS4_SYSV_ABI SetEnemyHpSendState(u64, GuestRegisterSnapshot* registers) {
    enemy_hp_session = 0;
    const s32 state = ReadValue<s32>(registers->rdi, MatchingStateOffset);
    if (const auto wanted = EnemySyncState(CurrentEnemyRole(), state)) {
        enemy_hp_session = registers->rdi;
        registers->rdi = StandInFor(*wanted);
    }
}

void PS4_SYSV_ABI RestoreEnemyHpSession(u64, GuestRegisterSnapshot* registers) {
    if (enemy_hp_session != 0) {
        registers->rdi = enemy_hp_session;
        enemy_hp_session = 0;
    }
}

// The receive from the host (0x017893E0) of the enemies' HP (ch 5, r12d): a follower reads the
// owner's member instead (the session's members at [+0xF8, +0x100), 0x20 each; rbx is the session
// manager before `add rbx, 0xD0`).
void PS4_SYSV_ABI ReceiveHpFromOwner(u64, GuestRegisterSnapshot* registers) {
    if (static_cast<u32>(registers->r12) != 0x05 || CurrentEnemyRole() != EnemyRole::Follower) {
        return;
    }
    const std::string owner = CurrentEnemyOwner();
    const u64 session = registers->rbx;
    if (owner.empty() || session < 0x10000) {
        return;
    }
    const u64 first = ReadValue<u64>(session, 0xF8);
    const u64 end = ReadValue<u64>(session, 0x100);
    if (first < 0x10000 || end < first || end - first > 0x20 * 8) {
        return;
    }
    for (u64 entry = first; entry < end; entry += 0x20) {
        if (BoardMemberOfMemberId(entry) == owner) {
            registers->rbx = entry - 0xD0;
            return;
        }
    }
}

void PS4_SYSV_ABI SetEnemyHitState(u64, GuestRegisterSnapshot* registers) {
    const s32 state = ReadValue<s32>(registers->rax, MatchingStateOffset);
    if (const auto wanted = EnemyHitState(CurrentEnemyRole(), state)) {
        registers->rax = StandInFor(*wanted);
    }
}

// Phase 5 (bloodborne_team.h): the title's load waits for the team's Start (state 6 skipped while
// HoldTitleLoadForTeam says so), and the title menu tells the lobby the player is back.
bool PS4_SYSV_ABI HoldTitleLoad(u64, const GuestRegisterSnapshot*) {
    const u64 game = GetGlobalState();
    const s32 slot = game >= 0x10000 ? ReadValue<s32>(game, TitleLoadSlotOffset) : -1;
    const bool fresh = ReadValue<u32>(image_base + TitleResultOffset, 0) == 1;
    return HoldTitleLoadForTeam(slot < 0 ? 0u : static_cast<u32>(slot), fresh);
}

void PS4_SYSV_ABI NoteTitleMenu(u64, const GuestRegisterSnapshot*) {
    TeamBackAtTitle();
}

void InstallTeamLobby(u64 base, u64 image_size) {
    if (!TeamModeOn()) {
        return;
    }
    if (!CoopSitesMatch(base, image_size, "team lobby", "loads do not wait for the team")) {
        return;
    }
    const bool installed =
        InstallCoopHook(reinterpret_cast<void*>(base + TitleLoadStateOffset), TitleStatePrologue, 0,
                        GuestFunctionOverride{HoldTitleLoad}) &&
        InstallCoopHook(reinterpret_cast<void*>(base + TitleMenuStateOffset), TitleStatePrologue, 0,
                        GuestCodeHook{NoteTitleMenu});
    if (!installed) {
        coop_features.Skipped("team lobby", "a hook did not go in");
        return;
    }
    coop_features.On("team lobby");
    LOG_INFO(Debug, "Bloodborne team lobby: a save's load waits for the team's Start");
    // Phase 6: the team joins its host's session without signs or a bell.
    if (CoopSitesMatch(base, image_size, "team join", "the team cannot join its host")) {
        team_join_ready = true;
        coop_features.On("team join");
    }
    // Kaniel 2026-10-15: a cutscene under the team's loading screen is skipped (Options pressed
    // for it while the cutscene player is in its playing step).
    if (!CoopSitesMatch(base, image_size, "cutscene skip", "cutscenes under a load play")) {
        return;
    }
    struct RemoStep {
        u64 offset;
        std::span<const u8> bytes;
    };
    const std::array<RemoStep, 7> steps{{
        {SprjRemoManStepOffsets[0], RemoStepEntry},
        {SprjRemoManStepOffsets[1], RemoStepEntry},
        {SprjRemoManStepOffsets[2], RemoStepEntry},
        {SprjRemoManStepOffsets[3], RemoStepEntry},
        {SprjRemoManStepOffsets[4], RemoStepEntry},
        {SprjRemoManStepOffsets[5], RemoStepEntry},
        {SprjRemoManStepOffsets[6], RemoEpilogWaitEntry},
    }};
    for (size_t step = 0; step < steps.size(); ++step) {
        if (!InstallCoopHook(reinterpret_cast<void*>(base + steps[step].offset), steps[step].bytes,
                             step, GuestCodeHook{Coop::NoteRemoStep})) {
            coop_features.Skipped("cutscene skip", "a cutscene step hook did not go in");
            return;
        }
    }
    coop_features.On("cutscene skip");
}

void InstallEnemyOwner(u64 base, u64 image_size) {
    if (!EnemyOwnerOn()) {
        return;
    }
    if (!channel_table_installed) {
        coop_features.Skipped("enemy owner", "the channel table is off");
        return;
    }
    if (!CoopSitesMatch(base, image_size, "enemy owner", "enemies keep the old path")) {
        return;
    }
    struct Hook {
        u64 offset;
        std::span<const u8> bytes;
        GuestRegisterEditHook hook;
    };
    const std::array<Hook, 6> hooks{{
        {EnemyHpRoundCheckOffset, EnemyHpRoundCheck, SetEnemyHpRoundState},
        {EnemyHpStateReadOffset, EnemyHpStateRead, SetEnemyHpSendState},
        {EnemyHpSendBranchOffset, EnemyHpSendBranch, RestoreEnemyHpSession},
        {EnemyHpReceiveBranchOffset, EnemyHpReceiveBranch, RestoreEnemyHpSession},
        {ReceiveFromHostMemberOffset, ReceiveFromHostMember, ReceiveHpFromOwner},
        {EnemyHitStateReadOffset, EnemyHitStateRead, SetEnemyHitState},
    }};
    // All or none: a stand-in nobody puts back would break the game.
    for (size_t installed = 0; installed < hooks.size(); ++installed) {
        if (!InstallCoopHook(reinterpret_cast<void*>(base + hooks[installed].offset),
                             hooks[installed].bytes, installed, hooks[installed].hook)) {
            for (size_t undo = 0; undo < installed; ++undo) {
                RemoveGuestCodeHook(reinterpret_cast<void*>(base + hooks[undo].offset));
            }
            coop_features.Skipped("enemy owner",
                                  SiteFailure(base, image_size, hooks[installed].offset));
            return;
        }
    }
    enemy_owner_installed = true;
    coop_features.On("enemy owner");
    // Phase 8: both or neither (a record left with the others' groups would wake them for good).
    if (InstallCoopHook(reinterpret_cast<void*>(base + ActivationPassOffset),
                        ActivationPassPrologue, 0, WakeAroundOthers)) {
        if (InstallCoopHook(reinterpret_cast<void*>(base + ActivationPassEndOffset),
                            ActivationPassEnd, 0, PutBackWokenRecords)) {
            coop_features.On("whole-map owner");
            // The position hooks stand alone (a follower's only counts).
            if (InstallCoopHook(reinterpret_cast<void*>(base + PositionPacketEntryOffset),
                                PositionPacketEntry, 0, SendFullPositionPacket) &&
                InstallCoopHook(reinterpret_cast<void*>(base + PositionReceiveEntryOffset),
                                PositionReceiveEntry, 0, CountPositionTaken) &&
                InstallCoopHook(reinterpret_cast<void*>(base + HandOutReceivedOffset),
                                HandOutReceived, 0, TakeHandOut)) {
                coop_features.On("whole-map positions");
            } else {
                coop_features.Skipped("whole-map positions",
                                      SiteFailure(base, image_size, PositionPacketEntryOffset));
            }
            if (!InstallCoopHook(reinterpret_cast<void*>(base + OwnershipStepEndOffset),
                                 OwnershipStepEndPrologue, 0, KeepAwakeEnemies)) {
                coop_features.Skipped("whole-map runs",
                                      SiteFailure(base, image_size, OwnershipStepEndOffset));
            } else {
                coop_features.On("whole-map runs");
            }
        } else {
            RemoveGuestCodeHook(reinterpret_cast<void*>(base + ActivationPassOffset));
            coop_features.Skipped("whole-map owner",
                                  SiteFailure(base, image_size, ActivationPassEndOffset));
        }
    } else {
        coop_features.Skipped("whole-map owner",
                              SiteFailure(base, image_size, ActivationPassOffset));
    }
    LOG_INFO(Debug, "Bloodborne enemy owner: on (the server's owner of each area runs its "
                    "enemies, the others follow it)");
}

// The peer (its queues) of a member in NetSync's list, or 0.
u64 NetSyncPeerOf(u64 net_sync, const std::string& member) {
    const u64 head = ReadValue<u64>(net_sync, 0x28);
    if (head < 0x10000) {
        return 0;
    }
    u32 steps = 0;
    for (u64 node = ReadValue<u64>(head, 0); node >= 0x10000 && node != head && steps < 16;
         node = ReadValue<u64>(node, 0), ++steps) {
        const u64 peer = ReadValue<u64>(node, 0x10);
        if (peer >= 0x10000 && OnlineIdOfMember(peer + 0x08) == member) {
            return peer;
        }
    }
    return 0;
}

// The packets our link took go into the members' queues, in the order they came: from NetSync's
// update on the game's thread, or from the link's while that update is quiet (see
// NetSyncUpdateOffset). One delivery at a time.
std::mutex deliver_mutex;
std::atomic<s64> net_sync_updated_ms{0};

void DeliverGamePackets();

void PS4_SYSV_ABI DeliverAtNetSyncUpdate(u64, const GuestRegisterSnapshot*) {
    net_sync_updated_ms.store(GameLinkNowMs(), std::memory_order_relaxed);
    // The game's thread never waits for the link's delivery: it goes on, next frame delivers.
    std::unique_lock lock{deliver_mutex, std::try_to_lock};
    if (lock.owns_lock()) {
        DeliverGamePackets();
    }
}

void DeliverGamePackets() {
    std::deque<InboundGamePacket> inbound;
    {
        std::scoped_lock lock{game_link.mutex};
        inbound.swap(game_link.inbound);
    }
    if (inbound.empty()) {
        return;
    }
    // Not mapped yet while the game starts (rig 2026-10-13: a read here crashed a booting game).
    const u64 net_sync_pointer = image_base + SummonSessionRulesPointerOffset;
    const u64 net_sync = HasMemoryAccess(net_sync_pointer, 8, MemoryProt::CpuRead)
                             ? ReadValue<u64>(net_sync_pointer, 0)
                             : 0;
    if (net_sync < 0x10000) {
        std::scoped_lock lock{game_link.mutex};
        for (auto it = inbound.rbegin(); it != inbound.rend(); ++it) {
            game_link.inbound.push_front(std::move(*it));
        }
        return;
    }
    using Fill = u8 PS4_SYSV_ABI (*)(u64 peer, u32 reliable, const u8* bytes, u32 size);
    const auto fill = reinterpret_cast<Fill>(image_base + MemberQueuesFillOffset);
    const s64 now_ms = GameLinkNowMs();
    std::deque<InboundGamePacket> waiting;
    std::set<std::string> missing; // a member's packets keep their order: once one waits, all do
    u64 taken = 0;
    u64 dropped = 0;
    for (InboundGamePacket& packet : inbound) {
        const u64 peer =
            missing.contains(packet.member) ? 0 : NetSyncPeerOf(net_sync, packet.member);
        if (peer == 0) {
            missing.insert(packet.member);
            if (now_ms - packet.received_ms < HeldForUnknownMemberMs) {
                waiting.push_back(std::move(packet));
            } else {
                ++dropped;
            }
            continue;
        }
        fill(peer, packet.packet_class == PacketClass::Reliable ? 1 : 0, packet.packet.data(),
             static_cast<u32>(packet.packet.size()));
        ++taken;
    }
    std::scoped_lock lock{game_link.mutex};
    game_link.taken += taken;
    game_link.dropped_unknown += dropped;
    // Before what came meanwhile.
    for (auto it = waiting.rbegin(); it != waiting.rend(); ++it) {
        game_link.inbound.push_front(std::move(*it));
    }
}

// A member's address: signaling's connection to it, else shadNet's record of its link (the
// matching server's RequestSignalingInfos, which waits for the server: the link's thread only).
std::optional<std::pair<u32, u16>> ResolveMember(const std::string& member, bool ask_server) {
    u32 addr = 0;
    u16 port = 0;
    if (Libraries::Np::NpSignaling::GetPeerAddress(member, &addr, &port) && addr != 0 &&
        port != 0) {
        return std::pair{addr, port};
    }
    if (ask_server && Libraries::Np::NpMatching2::RequestSignalingInfos(member, &addr, &port) &&
        addr != 0 && port != 0) {
        return std::pair{addr, port};
    }
    return std::nullopt;
}

// The members' addresses: new ones (asked for every 2 s until found), and every known one again
// every 5 s (a member whose game restarted has a new port).
void RefreshMemberAddresses(s64 now_ms, s64& refreshed_ms, s64& asked_ms) {
    std::vector<std::string> wanted;
    std::vector<std::string> known;
    {
        std::scoped_lock lock{game_link.mutex};
        wanted.assign(game_link.unresolved.begin(), game_link.unresolved.end());
        for (const auto& [member, address] : game_link.addresses) {
            known.push_back(member);
        }
    }
    const bool ask = !wanted.empty() && now_ms - asked_ms >= 2000;
    const bool refresh = now_ms - refreshed_ms >= 5000;
    if (!ask && !refresh) {
        return;
    }
    std::vector<std::pair<std::string, std::pair<u32, u16>>> found;
    if (ask) {
        asked_ms = now_ms;
        for (const std::string& member : wanted) {
            if (const auto address = ResolveMember(member, true)) {
                found.emplace_back(member, *address);
            }
        }
    }
    if (refresh) {
        refreshed_ms = now_ms;
        for (const std::string& member : known) {
            if (const auto address = ResolveMember(member, false)) {
                found.emplace_back(member, *address);
            }
        }
    }
    std::scoped_lock lock{game_link.mutex};
    for (const auto& [member, address] : found) {
        auto& known_address = game_link.addresses[member];
        if (known_address != address) {
            LOG_INFO(Debug, "Bloodborne game packet link: {} at {:#x}:{}", member,
                     ntohl(address.first), ntohs(address.second));
        }
        known_address = address;
        game_link.unresolved.erase(member);
    }
}

void RunGamePacketLink() {
    std::vector<u8> buffer(70'000);
    s64 stats_ms = GameLinkNowMs();
    s64 refreshed_ms = 0;
    s64 asked_ms = 0;
    while (true) {
        RefreshMemberAddresses(GameLinkNowMs(), refreshed_ms, asked_ms);
        const s64 now_ms = GameLinkNowMs();
        std::vector<std::pair<std::pair<u32, u16>, std::vector<u8>>> out;
        std::vector<std::string> lines;
        {
            std::scoped_lock lock{game_link.mutex};
            if (game_link.self.empty()) {
                game_link.self = LocalOnlineId();
            }
            u32 from_addr = 0;
            u16 from_port = 0;
            for (int read = 0; read < 512; ++read) {
                const int size = Libraries::Net::P2PGamePacketsRecvFrom(
                    buffer.data(), static_cast<u32>(buffer.size()), &from_addr, &from_port);
                if (size < 5) {
                    break;
                }
                const auto frame =
                    DecodeGameFrame({buffer.data() + 5, static_cast<size_t>(size) - 5});
                if (!frame || frame->sender.empty() || game_link.self.empty()) {
                    continue;
                }
                for (auto& packet : GameLinkPeerOf(frame->sender).Receive(*frame, now_ms)) {
                    if (NativePacketChannel(packet) >= NativeChannelCount) {
                        ++game_link.dropped_routed; // shadNet's: it keeps the stream's order
                        continue;
                    }
                    const bool reliable =
                        (packet[0] | packet[1] << 8 | packet[2] << 16 | packet[3] << 24) >= 7;
                    game_link.inbound.push_back(
                        {frame->sender, reliable ? PacketClass::Reliable : PacketClass::Unreliable,
                         std::move(packet), now_ms});
                }
            }
            for (auto& [member, peer] : game_link.peers) {
                const auto address = game_link.addresses.find(member);
                if (address == game_link.addresses.end()) {
                    game_link.unresolved.insert(member);
                    continue; // its frames wait
                }
                for (const GameFrame& frame : peer.Flush(now_ms)) {
                    std::vector<u8> bytes{'S', 'H', 'A', 'D', GamePacketsType};
                    const auto payload = EncodeGameFrame(frame);
                    bytes.insert(bytes.end(), payload.begin(), payload.end());
                    out.emplace_back(address->second, std::move(bytes));
                }
                const auto silent = peer.SilentFor(now_ms);
                bool& reported = game_link.reported_silent[member];
                if (silent && *silent > 3000 && !reported) {
                    reported = true;
                    lines.push_back(fmt::format("nothing from {} for {} ms", member, *silent));
                } else if (silent && *silent <= 3000 && reported) {
                    reported = false;
                    lines.push_back(fmt::format("{} heard again", member));
                }
            }
            if (now_ms - stats_ms >= 10'000) {
                stats_ms = now_ms;
                std::string peers;
                for (const auto& [member, peer] : game_link.peers) {
                    peers += fmt::format(" {}: {} unacked, {} resends, {} late dropped;", member,
                                         peer.Unacked(), peer.Resends(), peer.DroppedOld());
                }
                lines.push_back(fmt::format(
                    "{} packets taken from the game, {} handed to it, {} dropped (no member), "
                    "{} dropped by shadNet, {} for no member id;{}",
                    game_link.sent, game_link.taken, game_link.dropped_unknown,
                    game_link.dropped_routed, game_link.no_member, peers));
            }
        }
        if (now_ms - net_sync_updated_ms.load(std::memory_order_relaxed) >= NetSyncUpdateQuietMs) {
            std::scoped_lock lock{deliver_mutex};
            DeliverGamePackets();
        }
        for (const auto& [address, bytes] : out) {
            Libraries::Net::P2PCoopSendTo(bytes.data(), static_cast<u32>(bytes.size()),
                                          address.first, address.second);
        }
        for (const std::string& line : lines) {
            LOG_INFO(Debug, "Bloodborne game packet link: {}", line);
            Trace("glink", {{"what", line}});
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{4});
    }
}

void InstallGamePacketLink(u64 base, u64 image_size) {
#ifdef BLOODBORNE_NATIVE_PORT
    const bool wanted = party_controller.has_value() && Libraries::Net::ServerLinkSwitchOn();
#else
    // The port only (docs/SERVER_COOP_PLAN.md); shadPS4 keeps the old path.
    const bool wanted = false;
#endif
    if (!wanted) {
        return;
    }
    if (!CoopSitesMatch(base, image_size, "game packet link", "the game sends its own packets")) {
        return;
    }
    std::random_device random;
    game_link.instance = random() | 1;
    const auto update = reinterpret_cast<void*>(base + NetSyncUpdateOffset);
    bool installed = InstallCoopHook(update, GamePacketLinkPrologue, 0, DeliverAtNetSyncUpdate);
    if (installed && !InstallCoopHook(reinterpret_cast<void*>(base + TransportSendOffset),
                                      GamePacketLinkPrologue, 0, TakeGamePacket)) {
        RemoveGuestCodeHook(update);
        installed = false;
    }
    if (!installed) {
        coop_features.Skipped("game packet link", "a hook did not go in");
        return;
    }
    game_link_installed = true;
    std::thread(RunGamePacketLink).detach();
    coop_features.On("game packet link");
    LOG_INFO(Debug,
             "Bloodborne game packet link: on (the game's packets in our frames, "
             "instance {:08x})",
             game_link.instance);
}

// See SaveBackups: a thread of its own copies the saves a minute after the start, then every few
// minutes, and keeps the newest few.
void StartSaveBackups() {
    const char* setting = std::getenv("SHADPS4_BLOODBORNE_SAVE_BACKUPS");
    const auto backups = ParseSaveBackups(setting != nullptr ? setting : "");
    if (!backups) {
        LOG_INFO(Debug, "Bloodborne save backups are off");
        return;
    }
    const std::filesystem::path user_dir = Common::FS::GetUserPath(Common::FS::PathType::UserDir);
    std::thread([user_dir, backups = *backups] {
        std::this_thread::sleep_for(std::chrono::minutes{1});
        while (true) {
            if (const auto made = BackUpSaves(user_dir, SaveBackupName(LocalTimeNow()))) {
                LOG_INFO(Debug, "Bloodborne save backup: {} ({} older ones removed)",
                         Common::FS::PathToUTF8String(*made),
                         PruneSaveBackups(user_dir, backups.kept));
            }
            std::this_thread::sleep_for(backups.every);
        }
    }).detach();
    LOG_INFO(Debug, "Bloodborne save backups: every {} min into {}, the newest {} kept",
             backups->every.count(), Common::FS::PathToUTF8String(user_dir / SaveBackupsFolder),
             backups->kept);
}

// The game frees a released Scaleform movie player 5 frames later (0x01F60620's delayed delete),
// and the port's render thread can still have a draw of it queued then (New Game crashed on
// GXRenderThread at eboot+0x1F591B0, Kaniel 2026-10-13): 60 frames.
void InstallMoviePlayerDeleteDelay(u64 base, u64 image_size) {
    if (!CoopSitesMatch(base, image_size, "movie player delete delay",
                        "movie players are freed after 5 frames")) {
        return;
    }
    WriteCoopPatches(base, "movie player delete delay");
    coop_features.On("movie player delete delay");
}

// The send pace (bloodborne_send_pace.h): with the opt-in unlocked frame rate online the game's
// frame-counted packets go no more often than in the 60 FPS mode. Each hook runs on the game's
// frame, before the game reads or tests its counter, and holds a send back by changing it.
SendPace send_pace;
std::mutex send_pace_mutex;

bool SendPaceAllows(u64 object, std::chrono::microseconds interval) {
    std::scoped_lock lock{send_pace_mutex};
    return send_pace.Allow(object, SendPace::Clock::now(), interval);
}

// 0x01529496 (CS ChrSync's frame, r12 the sync object), before `mov ebx, [r12+0xC8]`.
void PS4_SYSV_ABI PacePlayerState(u64, GuestRegisterSnapshot* registers) {
    const u64 sync = registers->r12;
    const auto loaded = TryRead<s32>(sync, 0xC8);
    if (!loaded || *loaded > 1) {
        return;
    }
    const s32 held = HeldCountdown(*loaded, SendPaceAllows(sync, PlayerStatePaceInterval));
    if (held != *loaded) {
        WriteValue(sync, 0xC8, held);
    }
}

// 0x0152C3E9, `mov [rbx+0xF8], edx` after the decrement (edx the count left).
void PS4_SYSV_ABI PaceSecondCounter(u64, GuestRegisterSnapshot* registers) {
    const s32 left = static_cast<s32>(registers->rdx);
    if (left > 0) {
        return;
    }
    const s32 held =
        HeldDecremented(left, SendPaceAllows(registers->rbx, SecondCounterPaceInterval));
    registers->rdx = static_cast<u32>(held);
}

// 0x018EF279 (the enemy HP round, rbx the NWCS object), before `mov eax, [rbx+0x680]`.
void PS4_SYSV_ABI PaceEnemyHpRound(u64, GuestRegisterSnapshot* registers) {
    const u64 round = registers->rbx;
    const auto loaded = TryRead<u32>(round, 0x680);
    if (!loaded || !RoundSends(*loaded)) {
        return;
    }
    const u32 held = HeldRound(*loaded, SendPaceAllows(round, EnemyHpPaceInterval));
    if (held != *loaded) {
        WriteValue(round, 0x680, held);
    }
}

// 0x012EE540, the event sync's send (rdi the event's state): skipped when this event and slot
// already sent its 60 for this second (each condition group on its own).
bool PS4_SYSV_ABI PaceEventSync(u64, const GuestRegisterSnapshot* registers) {
    const u64 event = registers->rdi;
    const auto id = TryRead<u32>(event, 0x28);
    const auto slot = TryRead<u16>(event, 0x2C);
    const auto group = TryRead<u32>(event, 0x48);
    if (!id || !slot || !group) {
        return false;
    }
    std::scoped_lock lock{send_pace_mutex};
    return !send_pace.AllowRate(EventSyncKey(*id, *slot, *group), SendPace::Clock::now(),
                                EventSyncPerSecond);
}

void InstallSendPace(u64 base, u64 image_size) {
    if (!party_controller || !EnvFlagEnabled("SHADPS4_BLOODBORNE_SEND_PACE")) {
        return;
    }
    if (!CoopSitesMatch(base, image_size, "send pace",
                        "an unlocked frame rate sends the game's state as often as it draws")) {
        return;
    }
    const std::array<std::pair<const char*, GuestRegisterEditHook>, 3> hooks{{
        {"player state send count", PacePlayerState},
        {"second sync send count", PaceSecondCounter},
        {"enemy HP round count", PaceEnemyHpRound},
    }};
    const auto install = [&](std::string_view name, auto hook) {
        const auto site = std::ranges::find(CoopSites, name, &CoopSite::name);
        const std::vector<u8> bytes = HexBytes(site->original);
        if (InstallCoopHook(reinterpret_cast<void*>(base + site->offset), bytes, 0, hook)) {
            return true;
        }
        LOG_ERROR(Debug, "Bloodborne send pace unavailable at {:#x} ({})", site->offset, name);
        coop_features.Skipped("send pace", SiteFailure(base, image_size, site->offset));
        return false;
    };
    for (const auto& [name, hook] : hooks) {
        if (!install(name, hook)) {
            return;
        }
    }
    if (!install("event sync send", GuestFunctionOverride{PaceEventSync})) {
        return;
    }
    coop_features.On("send pace");
    LOG_INFO(Debug, "Bloodborne send pace: player state, sync, enemy HP and event sync packets at "
                    "the 60 FPS mode's rates");
}

// A co-op guest hits the friendly NPCs as the host does (see the "guests hit NPCs" sites).
void InstallGuestsHitNpcs(u64 base, u64 image_size) {
    if (!party_controller) {
        return;
    }
    if (!CoopSitesMatch(base, image_size, "guests hit NPCs", "guests cannot hit NPCs")) {
        return;
    }
    WriteCoopPatches(base, "guests hit NPCs");
    coop_features.On("guests hit NPCs");
}

// See PlayerNumberHandedOut: the host's counter goes on past the number it just handed out.
void PS4_SYSV_ABI MovePlayerNumberOn(u64, const GuestRegisterSnapshot* registers) {
    const u64 net_sync = registers->r15;
    const s32 given = static_cast<s32>(registers->rax);
    if (net_sync < 0x10000 || given <= 0) {
        return;
    }
    WriteValue(net_sync, PlayerNumberCounterOffset, NextPlayerNumber(given));
    LOG_INFO(Debug, "Bloodborne party: a joining member got player number {}", given);
}

void InstallOwnPlayerNumbers(u64 base, u64 image_size) {
    if (!party_controller || !CoopSitesMatch(base, image_size, "own player numbers",
                                             "two joining guests may share a "
                                             "player number")) {
        return;
    }
    if (!InstallCoopHook(reinterpret_cast<void*>(base + PlayerNumberHandedOutOffset),
                         PlayerNumberHandedOut, 0, GuestCodeHook{MovePlayerNumberOn})) {
        coop_features.Skipped("own player numbers", "the hook did not go in");
        return;
    }
    coop_features.On("own player numbers");
}

// Trading (bloodborne_trade.cpp, the port with the server world): the game functions it calls to
// take items out of the inventory and put them in.
void InstallTrading(u64 base, u64 image_size) {
    if (!TradeSwitchOn() || !CoopSitesMatch(base, image_size, "trading", "no trading")) {
        return;
    }
    EnableTrading();
    coop_features.On("trading");
}

// See ShowNetworkBannerPrologue: no multiplayer banner in a party.
bool PS4_SYSV_ABI SkipNetworkBanner(u64, const GuestRegisterSnapshot* registers) {
    static std::atomic<u32> logged{0};
    if (logged.fetch_add(1) < 50) {
        LOG_INFO(Debug, "Bloodborne party: multiplayer banner {} not shown",
                 static_cast<s32>(registers->rdi));
    }
    return true;
}

// See MemberLeftNoticeOffset: no notice of a member that left in a party (it held the pad).
bool PS4_SYSV_ABI SkipMemberLeftNotice(u64, const GuestRegisterSnapshot*) {
    static std::atomic<u32> logged{0};
    if (logged.fetch_add(1) < 50) {
        LOG_INFO(Debug, "Bloodborne party: a left member's notice not shown");
    }
    return true;
}

// See SessionLostTestOffset: FrpgNetMan as the "Session lost." window's test reads it, with the
// bit that skips the window set.
struct SessionLostStandIn {
    alignas(16) std::array<u8, 0xA10> bytes{};
    SessionLostStandIn() {
        bytes[0xA05] = 1;
    }
};
const SessionLostStandIn session_lost_stand_in;

void PS4_SYSV_ABI SkipSessionLostWindow(u64, GuestRegisterSnapshot* registers) {
    registers->rax = reinterpret_cast<u64>(session_lost_stand_in.bytes.data());
    static std::atomic<u32> logged{0};
    if (logged.fetch_add(1) < 20) {
        LOG_INFO(Debug, "Bloodborne party: the session's end shows no \"Session lost.\" window");
    }
}

// See LanCutErrorOffset: the network script's lost-connection error does nothing in a party.
bool PS4_SYSV_ABI SkipLanCutError(u64, const GuestRegisterSnapshot*) {
    static std::atomic<u32> logged{0};
    if (logged.fetch_add(1) < 20) {
        LOG_INFO(Debug, "Bloodborne party: lost-connection popup (OnLanCutError) not shown");
    }
    return true;
}

void InstallNoLostConnectionPopup(u64 base, u64 image_size) {
    if (!party_controller || !CoopSitesMatch(base, image_size, "no lost-connection popup",
                                             "a dropped connection shows the title-return popup")) {
        return;
    }
    if (!InstallCoopHook(reinterpret_cast<void*>(base + LanCutErrorOffset), LanCutErrorPrologue, 0,
                         GuestFunctionOverride{SkipLanCutError})) {
        coop_features.Skipped("no lost-connection popup", "the hook did not go in");
        return;
    }
    coop_features.On("no lost-connection popup");
}

// See ItemGetQueuedOffset: the item-received popup's queue push, skipped while a SilentItemGets
// lives on this thread. Each push is logged (the rig's popup detector).
bool PS4_SYSV_ABI SkipSilentItemGet(u64, const GuestRegisterSnapshot* registers) {
    const bool silent = ItemGetsSilent();
    static std::atomic<u32> logged{0};
    if (logged.fetch_add(1) < 300) {
        const u32 category = static_cast<u32>(registers->rdx) & 0xF0000000;
        const char* kind = category == 0x40000000   ? "goods"
                           : category == 0x10000000 ? "armor"
                           : category == 0x20000000 ? "accessory"
                           : category == 0          ? "weapon"
                                                    : "item";
        LOG_INFO(Debug, "Bloodborne party: item-get popup {} {} x {} {}", kind,
                 static_cast<u32>(registers->rcx), static_cast<s32>(registers->r8),
                 silent ? "not shown (a silent grant)" : "queued");
    }
    return silent;
}

void InstallSilentKeyItems(u64 base, u64 image_size) {
    if (!party_controller || !CoopSitesMatch(base, image_size, "silent key items",
                                             "a joining guest's key items show their popups")) {
        return;
    }
    if (!InstallCoopHook(reinterpret_cast<void*>(base + ItemGetQueuedOffset), ItemGetQueuedPrologue,
                         0, GuestFunctionOverride{SkipSilentItemGet})) {
        coop_features.Skipped("silent key items", "the hook did not go in");
        return;
    }
    coop_features.On("silent key items");
}

// Kaniel 2026-10-28: the boss tempo rule's hook (BossTempoGoal), on whatever the rules say: it
// changes nothing at Vanilla, and logs a boss's breathers there too.
void InstallBossTempo(u64 base, u64 image_size) {
    if (!CoopSitesMatch(base, image_size, "boss tempo",
                        "the boss tempo rule leaves the bosses' breathers as they are")) {
        return;
    }
    if (!InstallCoopHook(reinterpret_cast<void*>(base + AddSubGoalLifeHookOffset),
                         AddSubGoalLifeHook, 0, GuestCodeHook{BossTempoGoal}) ||
        !InstallCoopHook(reinterpret_cast<void*>(base + AddSubGoalFrontLifeHookOffset),
                         AddSubGoalLifeHook, AddSubGoalFrontTag, GuestCodeHook{BossTempoGoal})) {
        coop_features.Skipped("boss tempo", "the hook did not go in");
        return;
    }
    coop_features.On("boss tempo");
}

// Kaniel 2026-10-25: the co-op cameras (spectating teammates, the free camera; see
// CoopCameraStep), in the port only.
void InstallCoopCamera(u64 base, u64 image_size) {
#ifdef BLOODBORNE_NATIVE_PORT
    if (!CoopSitesMatch(base, image_size, "co-op camera",
                        "spectating and the free camera keep the game's camera")) {
        return;
    }
    static constexpr std::array<u8, 7> Bytes{0x49, 0x8B, 0x85, 0x28, 0x01, 0x00, 0x00};
    if (!InstallCoopHook(reinterpret_cast<void*>(base + CoopCameraHookOffset), Bytes, 0,
                         GuestCodeHook{CoopCameraStep})) {
        coop_features.Skipped("co-op camera", "the hook did not go in");
        return;
    }
    coop_features.On("co-op camera");
#else
    (void)base;
    (void)image_size;
#endif
}

void InstallNoMultiplayerBanners(u64 base, u64 image_size) {
    if (!party_controller || !CoopSitesMatch(base, image_size, "no multiplayer banners",
                                             "join and leave banners show")) {
        return;
    }
    if (!InstallCoopHook(reinterpret_cast<void*>(base + ShowNetworkBannerOffset),
                         ShowNetworkBannerPrologue, 0, GuestFunctionOverride{SkipNetworkBanner}) ||
        !InstallCoopHook(reinterpret_cast<void*>(base + MemberLeftNoticeOffset),
                         MemberLeftNoticePrologue, 0,
                         GuestFunctionOverride{SkipMemberLeftNotice}) ||
        !InstallCoopHook(reinterpret_cast<void*>(base + SessionLostTestOffset), SessionLostTest, 0,
                         SkipSessionLostWindow)) {
        coop_features.Skipped("no multiplayer banners", "the hook did not go in");
        return;
    }
    coop_features.On("no multiplayer banners");
}

// See PlayerNameForTextOffset: in a party the game names players by their characters.
void InstallCharacterNames(u64 base, u64 image_size) {
    if (!party_controller) {
        return;
    }
    if (!CoopSitesMatch(base, image_size, "character names",
                        "nameplates and messages show online ids")) {
        return;
    }
    WriteCoopPatches(base, "character names");
    coop_features.On("character names");
}

// See QuietSummonBannerPatches: a party's joins show one banner, not two.
void InstallQuietSummonBanners(u64 base, u64 image_size) {
    if (!party_controller) {
        return;
    }
    if (!CoopSitesMatch(base, image_size, "quiet summon banners", "joins keep the chime banner")) {
        return;
    }
    WriteCoopPatches(base, "quiet summon banners");
    coop_features.On("quiet summon banners");
    LOG_INFO(Debug, "Bloodborne summon banner patch: no chime banner before a join's own");
}

// See PartySummonSlotHoldConstant: a joined guest's summon stops holding a slot after 15 s, so
// the host searches for the next member at once instead of 180 s later.
void InstallShortSummonSlotHold(u64 base, u64 image_size) {
    if (!party_controller) {
        return;
    }
    if (!CoopSitesMatch(base, image_size, "summon slot hold",
                        "a third member waits up to 180 s to join")) {
        return;
    }
    WriteCoopPatches(base, "summon slot hold");
    coop_features.On("summon slot hold");
    LOG_INFO(Debug, "Bloodborne summon slot patch: a summon in flight holds its slot for {} s",
             PartySummonSlotHoldSeconds);
}

// Phase 7: the server says who is in a session (the rooms it keeps, its link's liveness), so the
// game never leaves one on its own: no idle time-out, resend overflow, room time-out or irregular
// leave (the sites' comments). A member that really went is the room's MEMBER_LEFT.
void InstallServerOwnsLeaves(u64 base, u64 image_size) {
    if (!party_controller) {
        return;
    }
    if (!CoopSitesMatch(base, image_size, "server owns leaves",
                        "the game may end its own session (a save, a stall)")) {
        return;
    }
    WriteCoopPatches(base, "server owns leaves");
    coop_features.On("server owns leaves");
    LOG_INFO(Debug, "Bloodborne party: only the server ends a session (the game's own time-outs "
                    "are off)");
}

constexpr u64 LeaveLimitOffset = 0x0138A4A0;
constexpr std::array<u8, 6> LeaveLimitPrologue{0x55, 0x48, 0x89, 0xE5, 0x53, 0x50};

void InstallGuestStaysWhenTheHostLeaves(u64 base, u64 image_size) {
    if (!party_controller) {
        return;
    }
    if (LeaveLimitOffset + LeaveLimitPrologue.size() > image_size ||
        !InstallCoopHook(reinterpret_cast<void*>(base + LeaveLimitOffset), LeaveLimitPrologue, 0,
                         StayWhenTheHostLeaves)) {
        LOG_ERROR(Debug, "Bloodborne leave-limit hook unavailable; a guest whose host leaves "
                         "goes home");
        coop_features.Skipped("host leaves", SiteFailure(base, image_size, LeaveLimitOffset));
        return;
    }
    coop_features.On("host leaves");
    LOG_INFO(Debug, "Bloodborne party guests stay where they are when their host leaves");
}

// Faster sign posts for party joins. Optional: on a mismatch the game keeps its own timers.
void PatchSignTimers(u64 base, u64 image_size) {
    for (const auto& site : SignTimerSites) {
        const auto expected = EncodeSignTimerStore(site.modrm, site.original_seconds);
        if (site.offset > image_size || expected.size() > image_size - site.offset ||
            !IsSignTimerStore(std::span<const u8>{reinterpret_cast<const u8*>(base + site.offset),
                                                  expected.size()},
                              site)) {
            const u8* found = reinterpret_cast<const u8*>(base + site.offset);
            LOG_ERROR(Debug,
                      "Bloodborne sign timer signature mismatch at {:#x} (found {}); "
                      "keeping the game's sign timers",
                      site.offset,
                      BytesToHex(std::span<const u8>{found, std::min<size_t>(expected.size(), 8)}));
            coop_features.Skipped("sign timers",
                                  fmt::format("signature mismatch at {:#x}", site.offset));
            return;
        }
    }
    for (const auto& site : SignTimerSites) {
        const auto patched = EncodeSignTimerStore(site.modrm, site.patched_seconds);
        std::memcpy(reinterpret_cast<void*>(base + site.offset), patched.data(), patched.size());
    }
    coop_features.On("sign timers");
    LOG_INFO(Debug, "Bloodborne sign timers patched: first post after {} s, re-post every {} s",
             FirstSignPostDelaySeconds, SignRepostSeconds);
}

} // namespace

namespace Coop {

bool VerifySites() {
    const auto image_size = MemoryPatcher::g_eboot_image_size;
    for (const auto& site : Sites) {
        if ((summon_build_host_placement_hook_installed && site.offset == SummonBuildEntryOffset) ||
            (cross_map_guest_handoff_hook_installed && site.offset == CrossMapGuestHandoffOffset) ||
            (summon_reload_state_hook_installed &&
             site.offset == StageWarpDescriptorFinalizeOffset) ||
            (deferred_summon_reload_hook_installed && site.offset == SosStatusUpdateOffset) ||
            (healing_fountain_host_availability_hook_installed &&
             site.offset == HealingFountainAvailabilityOffset) ||
            (world_teardown_hook_installed && site.offset == WorldTeardownMatchingOffset) ||
            (ghost_death_revive_hook_installed && site.offset == GhostDeathReviveOffset) ||
            (host_death_respawn_hook_installed && site.offset == SoloPlayDeathRespawnOffset) ||
            (seamless_travel_hooks_installed && site.offset == StageTransitionEntryOffset) ||
            (stop_request_hook_installed && site.offset == MultiPlayStopRequestOffset)) {
            continue;
        }
        const auto expected = std::span<const u8>{site.prologue.data(), site.prologue_size};
        if (site.offset > image_size || expected.size() > image_size - site.offset) {
            LOG_ERROR(Debug, "Bloodborne RE site {} lies outside the eboot image", site.name);
            return false;
        }
        const auto actual = std::span<const u8>{
            reinterpret_cast<const u8*>(image_base + site.offset), expected.size()};
        if (!std::ranges::equal(actual, expected)) {
            LOG_ERROR(Debug,
                      "Bloodborne RE signature mismatch at {} ({:#x}): expected={} actual={}",
                      site.name, site.offset, BytesToHex(expected), BytesToHex(actual));
            return false;
        }
    }
    return true;
}

} // namespace Coop

namespace {

// Whether the seamless core is in already (its byte patches and its hooks).
bool SeamlessCoreInstalled() {
    return negative_area_patch_installed && area_flag_patch_installed &&
           responder_bell_area_patch_installed && responder_bell_common_patch_installed &&
           active_bell_negative_area_patch_installed && active_bell_area_flag_patch_installed &&
           responder_search_negative_area_patch_installed && sos_status_area_patch_installed &&
           summon_candidate_area_patch_installed && summon_build_area_patch_installed &&
           summon_build_world_state_patch_installed &&
           summon_build_negative_event_patch_installed &&
           summon_build_host_placement_hook_installed && cross_map_guest_handoff_hook_installed &&
           summon_reload_state_hook_installed && deferred_summon_reload_hook_installed &&
           healing_fountain_host_availability_hook_installed;
}

// Whether this game is the one the seamless patches are for: Bloodborne CUSA03173 at 01.09.
bool IsSeamlessCoopGame() {
    // Native port: regional 1.09 executables must still pass CoopSitesMatch in full.
    constexpr std::array<std::string_view,9> titles{"CUSA03173", "CUSA00207", "CUSA00208",
        "CUSA00900", "CUSA03179", "CUSA01363", "CUSA03023", "CUSA00299", "CUSA03014"};
    if (std::ranges::find(titles, MemoryPatcher::g_game_serial) == titles.end()) {
        return false;
    }
    const auto* param_sfo = Common::Singleton<PSF>::Instance();
    const std::string_view app_version = param_sfo->GetString("APP_VER").value_or("Unknown");
    if (app_version != "01.09" && !NativeVerified109EbootFile()) {
        LOG_ERROR(Debug,
                  "Bloodborne seamless patches require the verified 01.09 executable; loaded version is {}",
                  app_version);
        return false;
    }
    return true;
}

// The RE tracer's site at `offset` (Sites), or null.
const TraceSite* FindSite(u64 offset) {
    const auto site =
        std::ranges::find_if(Sites, [&](const TraceSite& site) { return site.offset == offset; });
    return site == Sites.end() ? nullptr : &*site;
}

// One of the seamless core's hooks: where, its site, its installed flag and its name for the log.
struct CoreHook {
    u64 offset;
    const TraceSite* site;
    bool* installed;
    std::string_view what;
};

// The core's hooks, all or none: one that fails (no room for its trampoline) takes the others
// out again, so the game is left as it was.
bool InstallCoreHooks(u64 base, u64 image_size, std::span<const CoreHook> core_hooks) {
    for (size_t index = 0; index < core_hooks.size(); ++index) {
        const CoreHook& hook = core_hooks[index];
        const auto expected =
            std::span<const u8>{hook.site->prologue.data(), hook.site->prologue_size};
        if (InstallCoopHook(reinterpret_cast<void*>(base + hook.offset), expected,
                            static_cast<size_t>(hook.site - Sites.data()), TraceEntry)) {
            *hook.installed = true;
            continue;
        }
        LOG_ERROR(Debug, "Failed to install Bloodborne {} hook at {:#x}", hook.what, hook.offset);
        for (size_t installed = 0; installed < index; ++installed) {
            RemoveGuestCodeHook(reinterpret_cast<void*>(base + core_hooks[installed].offset));
            *core_hooks[installed].installed = false;
        }
        coop_features.Skipped(
            "seamless core",
            fmt::format("{}; rolled back", SiteFailure(base, image_size, hook.offset)));
        LogCoopFeatures();
        return false;
    }
    return true;
}

// Optional: without it a guest quitting to the title stays in the room, but every other patch
// still applies.
void InstallWorldTeardownHook(u64 base, u64 image_size, const TraceSite& world_teardown) {
    const auto world_teardown_expected =
        std::span<const u8>{world_teardown.prologue.data(), world_teardown.prologue_size};
    world_teardown_hook_installed = InstallCoopHook(
        reinterpret_cast<void*>(base + WorldTeardownMatchingOffset), world_teardown_expected,
        static_cast<size_t>(&world_teardown - Sites.data()), TraceEntry);
    if (!world_teardown_hook_installed) {
        LOG_ERROR(Debug, "Failed to install Bloodborne world-teardown hook at {:#x}",
                  WorldTeardownMatchingOffset);
        coop_features.Skipped("world teardown",
                              SiteFailure(base, image_size, WorldTeardownMatchingOffset));
    } else {
        coop_features.On("world teardown");
    }
}

// The features on top of the core, each on its own (see coop_features), and the services they
// run.
void InstallCoopFeatures(u64 base, u64 image_size) {
    InstallGhostDeathRevive(base, image_size);
    InstallNoEchoTheft(base, image_size);
    InstallHostDeathKeepsGuests(base, image_size);
    InstallBossClearKeepsSession(base, image_size);
    InstallJoinAnywhere(base, image_size);
    InstallQuietSummonBanners(base, image_size);
    InstallNoMultiplayerBanners(base, image_size);
    InstallSilentKeyItems(base, image_size);
    InstallBossTempo(base, image_size);
    InstallCharacterNames(base, image_size);
    InstallNoLostConnectionPopup(base, image_size);
    InstallGuestsHitNpcs(base, image_size);
    InstallOwnPlayerNumbers(base, image_size);
    InstallMoviePlayerDeleteDelay(base, image_size);
    InstallSendPace(base, image_size);
    InstallShortSummonSlotHold(base, image_size);
    InstallBossDeathTracer(base, image_size);
    InstallBulletTracer(base, image_size);
    InstallGuestStaysWhenTheHostLeaves(base, image_size);
    InstallServerOwnsLeaves(base, image_size);
    InstallSeamlessTravel(base, image_size);
    InstallLampsAsCampfires(base, image_size);
    InstallNoLevelSyncInParty(base, image_size);
    InstallSolidNpcsInParty(base, image_size);
    // Before the shared loot: the table takes over its map item packets.
    InstallChannelTable(base, image_size);
    InstallGamePacketLink(base, image_size);
    InstallEnemyOwner(base, image_size);
    InstallTeamLobby(base, image_size);
    InstallFloorGuard(base, image_size);
    InstallFlagBlockGuard(base, image_size);
    InstallChaliceAltarPick(base, image_size);
    InstallMemberPacketGuard(base, image_size);
    InstallSharedLoot(base, image_size);
    InstallLampsStayLitAtRegistration(base, image_size);
    InstallDeferredBossItems(base, image_size);
    InstallKeepLocalFlags(base, image_size);
    InstallItemModuleDiagnostics(base, image_size);
    InstallItemButtonDiagnostics(base, image_size);
    InstallPartyDeath(base, image_size);
    InstallOwnHitCount(base, image_size);
    InstallTrading(base, image_size);
    InstallCoopCamera(base, image_size);
    StartSaveBackups();
    StartCoopTrace();
    InstallNativePacketTrace(base, image_size);
    StartRestBoardClient();
    StartVitalsBoardClient();
    PatchSignTimers(base, image_size);
}

// The core's byte patches, written.
void WriteSeamlessCoreBytePatches(u64 base) {
    WriteCoopPatches(base, "seamless core");
    negative_area_patch_installed = true;
    area_flag_patch_installed = true;
    responder_bell_area_patch_installed = true;
    responder_bell_common_patch_installed = true;
    active_bell_negative_area_patch_installed = true;
    active_bell_area_flag_patch_installed = true;
    responder_search_negative_area_patch_installed = true;
    sos_status_area_patch_installed = true;
    summon_candidate_area_patch_installed = true;
    summon_build_area_patch_installed = true;
    summon_build_world_state_patch_installed = true;
    summon_build_negative_event_patch_installed = true;
    stage_transition_keep_matching_patch_installed = true;
}

} // namespace

void InstallSeamlessCoopPatches() {
    if (SeamlessCoreInstalled() || !EnvFlagEnabled("SHADPS4_BLOODBORNE_SEAMLESS_COOP")) {
        return;
    }
    if (!IsSeamlessCoopGame()) {
        return;
    }

    const auto base = MemoryPatcher::g_eboot_address;
    const auto image_size = MemoryPatcher::g_eboot_image_size;
    coop_features = {};
    const TraceSite* summon_build_entry = FindSite(SummonBuildEntryOffset);
    const TraceSite* cross_map_guest_handoff = FindSite(CrossMapGuestHandoffOffset);
    const TraceSite* deferred_summon_reload = FindSite(SosStatusUpdateOffset);
    const TraceSite* summon_reload_state = FindSite(StageWarpDescriptorFinalizeOffset);
    const TraceSite* healing_fountain_host_availability =
        FindSite(HealingFountainAvailabilityOffset);
    const TraceSite* world_teardown = FindSite(WorldTeardownMatchingOffset);
    if (!summon_build_entry || !cross_map_guest_handoff || !deferred_summon_reload ||
        !summon_reload_state || !healing_fountain_host_availability || !world_teardown) {
        LOG_ERROR(Debug, "Bloodborne seamless functional hook site is missing");
        return;
    }
    if (base == 0) {
        LOG_ERROR(Debug, "Bloodborne seamless patches lie outside the eboot image");
        return;
    }

    // All of the core or none of it. Every site is checked before anything is written: its byte
    // patches, its hooks and the game functions the cross-map summon calls (the co-op site table,
    // bloodborne_patch_sites.h).
    if (!CoopSitesMatch(base, image_size, "seamless core", "no seamless co-op")) {
        LogCoopFeatures();
        return;
    }
    image_base = base;
    // Then the hooks (InstallCoreHooks: all or none).
    const std::array<CoreHook, 5> core_hooks{{
        {SummonBuildEntryOffset, summon_build_entry, &summon_build_host_placement_hook_installed,
         "cross-map host-placement"},
        {CrossMapGuestHandoffOffset, cross_map_guest_handoff,
         &cross_map_guest_handoff_hook_installed, "cross-map guest-handoff"},
        {StageWarpDescriptorFinalizeOffset, summon_reload_state,
         &summon_reload_state_hook_installed, "summon-reload state"},
        {SosStatusUpdateOffset, deferred_summon_reload, &deferred_summon_reload_hook_installed,
         "deferred summon-reload"},
        {HealingFountainAvailabilityOffset, healing_fountain_host_availability,
         &healing_fountain_host_availability_hook_installed, "healing-fountain host-availability"},
    }};
    if (!InstallCoreHooks(base, image_size, core_hooks)) {
        return;
    }
    coop_features.On("seamless core");
    InitPartyController();
    InstallWorldTeardownHook(base, image_size, *world_teardown);

    InstallCoopFeatures(base, image_size);

    WriteSeamlessCoreBytePatches(base);
    LOG_INFO(Debug,
             "Bloodborne seamless patches enabled Beckoning, Small Resonant, and Sinister Bell "
             "use, active search, cross-map candidate selection, and native summon construction "
             "with host-placement transport, guest stage handoff, retained matching during "
             "stage transitions, deferred native reload, and host lantern actions in negative "
             "and area-complete SOS regions ({} byte patches, {} hooks)",
             CoopPatchesOf("seamless core").size(), core_hooks.size());
    LogCoopFeatures();
}

bool NativeSeamlessReady() { return SeamlessCoreInstalled(); }

} // namespace Core::Bloodborne
