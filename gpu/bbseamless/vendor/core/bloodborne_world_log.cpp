// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// The world log's runtime (docs/WORLD_SYNC.md, Stage C, step 1: the kills): which path the kills
// take (WorldLogPath), the host's log (WorldLogHost) and a member's copy (WorldLogMember), their
// frames on the P2P link, and the kill board underneath as the log's fallback copy. Everything
// here runs on the game's thread (the kill step and the frames it reads); the lifecycle resets
// only set flags. Shared declarations are in bloodborne_re_internal.h.

#include "core/bloodborne_party.h"
#include "core/bloodborne_re_internal.h"

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <map>
#include <mutex>
#include <optional>
#include <random>
#include <span>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

#include <fmt/format.h>
#include "common/logging/log.h"
#include "core/libraries/np/np_matching2/np_matching2_signaling.h"

namespace Core::Bloodborne {
namespace {

const bool world_log_switch = WorldLogEnabled(std::getenv("SHADPS4_BLOODBORNE_WORLD_LOG"));
// This game's run: the upper half of the host's log epochs, and the run of its proposals.
const u32 world_log_run = [] {
    std::random_device random;
    return static_cast<u32>(random()) | 1u;
}();

// The kills, the shown characters (step 2) and the shared flags with the map state (step 3) take
// the log together: every member runs one build.
WorldLogPath world_log_path;
// A guest's copy of the host's map state from the log (map_state_mutex), see HostMapStateNow.
std::optional<MemberMapState> log_map_state;
// The path, for the board client's thread (FilterBoardFlags).
std::atomic<bool> flags_on_log{false};
WorldLogFlagOutbox flag_outbox; // this game's shared flag events, until taken
s64 flags_sent_ms = 0;
// Enemy HP (step 4): this game's report proposals, counted within its run.
u32 hp_proposals_made = 0;
s64 hp_sent_ms = 0;
// The world snapshot on the server (see WorldSnapshotPostMs): the host's next blob to post, and a
// member's wish for one and the one read (world_snapshot_mutex: the board client's thread).
std::mutex world_snapshot_mutex;
std::optional<std::vector<u8>> world_snapshot_out;
std::optional<std::pair<s64, std::vector<u8>>> world_snapshot_in; // age ms, blob
std::atomic<bool> world_snapshot_wanted{false};
s64 world_snapshot_made_ms = 0;
FlagEventOnce flag_once; // flag_board_mutex: an event from the log or the flag board, once
WorldLogHost world_log_host;
WorldLogMember world_log_member;
u32 world_logs_started = 0;
u32 proposals_made = 0;
s64 proposal_sent_ms = 0;
s64 entries_sent_ms = 0;
WorldLogKills last_proposed;
std::optional<std::pair<u32, u16>> log_host_peer; // where the host's frames come from
// A guest's copy of the host's return point (0: none), see HostReturnPointNow. Kept apart from
// world_log_member: a new log (the host's wipe) forgets the member's copy until its first entries
// or a snapshot bring it again, which is when the guest's wipe respawn asks for it.
std::atomic<s32> host_return_point{0};

// What the lifecycle asks of the log, taken on the next kill step: the host starts a new log with
// no kills on its wipe (marked, see WorldLogWipeHold), a new session or character; a member
// forgets its copy on a new session or character.
enum class LogRestart : int { None, Wipe, Clear };
std::atomic<int> host_restart{static_cast<int>(LogRestart::None)};
std::atomic<bool> member_reset{false};
// When this member last heard the host's wiped log and ran its own wipe (game thread).
WorldLogWipeHold wipe_hold;

[[maybe_unused]] const bool world_log_wipe_registered =
    RegisterLifecycleReset("world log (wipe)", {Lifecycle::Wipe}, [] {
        int none = static_cast<int>(LogRestart::None);
        host_restart.compare_exchange_strong(none, static_cast<int>(LogRestart::Wipe));
    });
[[maybe_unused]] const bool world_log_session_registered = RegisterLifecycleReset(
    "world log", {Lifecycle::Join, Lifecycle::Leave, Lifecycle::NewCharacter}, [] {
        host_restart.store(static_cast<int>(LogRestart::Clear));
        member_reset.store(true);
    });

std::string LogMemberName() {
    std::scoped_lock lock{vitals_mutex};
    return vitals_member;
}

void TraceLog(std::string_view role, std::string_view op, std::span<const u8> payload) {
    if (CoopTraceOn()) {
        Trace("wlog-in", WorldLogTraceFields(role, op, payload));
    }
}

std::string EntriesText(std::span<const WorldLogEntry> entries) {
    std::string text;
    for (size_t index = 0; index < entries.size() && index < 8; ++index) {
        text += (text.empty() ? "" : ", ") + FormatWorldLogEntry(entries[index]);
    }
    return entries.size() > 8 ? fmt::format("{}, ... ({} in all)", text, entries.size()) : text;
}

// Entries for the log: each kill entry; the shown characters' (which change as the host walks)
// as a count, on at most 400 lines a game.
std::optional<std::string> LoggedEntries(std::span<const WorldLogEntry> entries) {
    std::vector<WorldLogEntry> kills;
    std::ranges::copy_if(entries, std::back_inserter(kills), [](const WorldLogEntry& entry) {
        return entry.kind != WorldLogKind::Shown && entry.kind != WorldLogKind::Unshown;
    });
    static u32 shown_lines = 0;
    const size_t shown = entries.size() - kills.size();
    if (kills.empty() && (shown == 0 || ++shown_lines > 400)) {
        return std::nullopt;
    }
    return EntriesText(kills) + (shown > 0 ? fmt::format(" ({} shown changes)", shown) : "");
}

// The host's entries (or a heartbeat for none) to every room peer.
void SendEntries(const std::string& name, std::span<const WorldLogEntry> entries, s64 now_ms) {
    for (const WorldLogBatch& batch :
         BatchWorldLog(name, world_log_host.Epoch(), world_log_host.Last(), entries)) {
        SendToRoomPeers(EncodeCoopFrame(WorldLogEntriesType, EncodeWorldLogBatch(batch)));
    }
    entries_sent_ms = now_ms;
    if (const auto text = LoggedEntries(entries)) {
        LOG_INFO(Debug, "Co-op world log: the host's log {:#x} took {}", world_log_host.Epoch(),
                 *text);
    }
}

void TakeProposal(const std::string& name, const WorldLogProposal& proposal, s64 now_ms) {
    const WorldLogHost::Taken taken = world_log_host.Propose(proposal);
    if (taken.stale || !taken.entries.empty()) {
        TraceLog("host", "propose", EncodeWorldLogProposal(proposal));
    }
    if (!taken.entries.empty()) {
        SendEntries(name, taken.entries, now_ms);
    }
}

// Flag events the log took (or a snapshot brought, those it missed): set in this member's world,
// each once.
void ApplyFlagEntries(std::span<const WorldLogEntry> entries, s64 now_ms) {
    std::scoped_lock lock{flag_board_mutex};
    for (const WorldLogEntry& entry : entries) {
        if (entry.kind != WorldLogKind::FlagSet && entry.kind != WorldLogKind::FlagClear) {
            continue;
        }
        const s32 flag = static_cast<s32>(entry.subject);
        const bool on = entry.kind == WorldLogKind::FlagSet;
        if (flag_once.First(flag, on, now_ms)) {
            flags_to_set.emplace_back(flag, on);
        }
    }
}

void TakeFlagProposal(const std::string& name, const WorldLogFlagProposal& proposal, s64 now_ms) {
    const WorldLogHost::Taken taken = world_log_host.ProposeFlags(proposal, now_ms);
    if (taken.stale || !taken.entries.empty()) {
        TraceLog("host", "propose-flags", EncodeWorldLogFlagProposal(proposal));
    }
    if (!taken.entries.empty()) {
        SendEntries(name, taken.entries, now_ms);
        ApplyFlagEntries(taken.entries, now_ms);
    }
}

void MakeWorldSnapshot(const std::string& name, s64 now_ms); // below

// The host's return point (GlobalState +0x1528, where its death respawn wakes it) to the log when
// it changed: a guest's party death wakes there too (PartyRespawnPoint; playtest 2026-10-13, a wipe
// woke the host in the Dream and the guest at its own Central Yharnam lamp).
void ShareReturnPoint(const std::string& name, s64 now_ms) {
    const u64 global = GetGlobalState();
    if (global < 0x10000) {
        return;
    }
    const s32 point = ReadValue<s32>(global, 0x1528);
    const WorldLogReturnPoint now{point, point > 0 ? PackedMapOfWarpPoint(point) : 0};
    const std::vector<WorldLogEntry> entries = world_log_host.ReturnPoint(now.point, now.map);
    if (!entries.empty()) {
        TraceLog("host", "return-point", EncodeWorldLogReturnPointInput(now));
        SendEntries(name, entries, now_ms);
    }
}

// The host: a new log when there is none or the lifecycle asked for one, its own kills as a
// proposal of its own, and a heartbeat every WorldLogResendMs.
void RunHostLog(const std::string& name, const WorldLogKills& mine, s64 now_ms) {
    const auto restart =
        static_cast<LogRestart>(host_restart.exchange(static_cast<int>(LogRestart::None)));
    if (!world_log_host.Started() || restart != LogRestart::None) {
        const bool wipe = world_log_host.Started() && restart == LogRestart::Wipe;
        const u64 epoch = WorldLogEpoch(world_log_run, ++world_logs_started);
        TraceLog("host", "start", EncodeWorldLogStart(epoch, wipe));
        LOG_INFO(Debug, "Co-op world log: the host starts log {:#x} ({}, no kills stand)", epoch,
                 wipe ? "a wipe" : "a new session");
        SendEntries(name, world_log_host.Start(epoch, wipe), now_ms);
    }
    TakeProposal(name, {name, world_log_host.Epoch(), world_log_run, ++proposals_made, mine},
                 now_ms);
    ShareReturnPoint(name, now_ms);
    if (auto events = flag_outbox.Pending(now_ms); !events.empty()) {
        TakeFlagProposal(name, {name, world_log_host.Epoch(), world_log_run, std::move(events)},
                         now_ms);
    }
    if (now_ms - entries_sent_ms >= WorldLogResendMs) {
        SendEntries(name, {}, now_ms);
    }
    MakeWorldSnapshot(name, now_ms);
}

// A member: its standing kills to the host at once on a change and every WorldLogResendMs, and a
// snapshot request when its copy fell behind.
void RunMemberLog(const std::string& name, const WorldLogKills& mine, s64 now_ms) {
    static bool held = false;
    if (const bool holds = wipe_hold.HoldsProposals(now_ms); holds != held) {
        held = holds;
        LOG_INFO(Debug, "Co-op world log: {}",
                 held ? "the host's log started over after a wipe; this member's kills wait for "
                        "its own wipe"
                      : "proposing this member's kills again");
    }
    if (!held && (mine != last_proposed || now_ms - proposal_sent_ms >= WorldLogResendMs)) {
        last_proposed = mine;
        proposal_sent_ms = now_ms;
        const WorldLogProposal proposal{name, world_log_member.Epoch(), world_log_run,
                                        ++proposals_made, mine};
        SendToRoomPeers(EncodeCoopFrame(WorldLogProposeType, EncodeWorldLogProposal(proposal)));
    }
    // Its shared flag events: at once when one came, then again for a while.
    if (flag_outbox.TakeNew() || now_ms - flags_sent_ms >= WorldLogResendMs) {
        if (auto events = flag_outbox.Pending(now_ms); !events.empty()) {
            flags_sent_ms = now_ms;
            const WorldLogFlagProposal flags{name, world_log_member.Epoch(), world_log_run,
                                             std::move(events)};
            SendToRoomPeers(
                EncodeCoopFrame(WorldLogFlagsProposeType, EncodeWorldLogFlagProposal(flags)));
        }
    }
    if (log_host_peer && world_log_member.SnapshotDue(now_ms)) {
        const WorldLogSnapshotRequest request{name, world_log_member.Epoch(),
                                              world_log_member.Applied()};
        SendToPeer(
            EncodeCoopFrame(WorldLogSnapshotRequestType, EncodeWorldLogSnapshotRequest(request)),
            log_host_peer->first, log_host_peer->second);
        LOG_INFO(Debug, "Co-op world log: asked the host for a snapshot (log {:#x}, seq {})",
                 world_log_member.Epoch(), world_log_member.Applied());
    }
}

// Which path the kills, shown characters and flags take now (see WorldLogPath).
bool DecideKillPath(const WorldLogKills& mine, s64 now_ms) {
    const size_t peers = Libraries::Np::NpMatching2::GetActiveRoomPeers().size();
    const bool was = world_log_path.Active();
    const bool log = world_log_path.Decide(world_log_switch, peers, now_ms);
    if (const auto line = world_log_path.TakeChange()) {
        LOG_INFO(Debug, "{}", *line);
        Trace("wlog-path", {{"path", log ? "log" : "board"}, {"why", world_log_path.Why()}});
    }
    if (was && !log) {
        world_log_host.ForgetShown(); // back on the log, its next Show starts with a ShownMap
    }
    return log;
}

// On the log path: the party's kills from the log (a member's copy kept by the link, or by the
// world snapshot while it is down). The kill board is not read then.
std::unordered_set<u32> LogKills() {
    return WorldLogHandles(IsPartyGuestRole() ? world_log_member.Kills() : world_log_host.Kills());
}

void LogMemberStart(const WorldLogMember::Step& step) {
    if (step.new_epoch) {
        LOG_INFO(Debug, "Co-op world log: following the host's log {:#x}",
                 world_log_member.Epoch());
    }
    if (step.snapshot) {
        LOG_INFO(Debug,
                 "Co-op world log: took the host's snapshot to seq {} ({} kills, shown "
                 "characters {})",
                 world_log_member.Applied(), WorldLogKillCount(world_log_member.Kills()),
                 world_log_member.ShownKnown()
                     ? fmt::format("{} on {:#x}", world_log_member.Shown().bits.size(),
                                   world_log_member.Shown().map)
                     : "not in it");
    }
}

void LogMemberStep(const WorldLogMember::Step& step, u64 host_last) {
    static bool behind = false;
    LogMemberStart(step);
    if (const auto text = LoggedEntries(step.applied)) {
        LOG_INFO(Debug, "Co-op world log: applied {}", *text);
    }
    if (step.gap != behind && !step.stale) {
        behind = step.gap;
        LOG_INFO(Debug, "Co-op world log: {} (has seq {}, the host {})",
                 behind ? "behind the host" : "in step again", world_log_member.Applied(),
                 host_last);
    }
}

void TakeHpProposal(const std::string& name, const WorldLogHpProposal& proposal, s64 now_ms) {
    const WorldLogHost::Taken taken = world_log_host.ProposeHp(proposal);
    if (taken.stale || !taken.entries.empty()) {
        TraceLog("host", "propose-hp", EncodeWorldLogHpProposal(proposal));
    }
    if (!taken.entries.empty()) {
        SendEntries(name, taken.entries, now_ms);
    }
}

// A snapshot's frames: the kills, then the shown characters, the map state, the HP reports and the
// recent flag events at the same seq (a member waits for them all). To a member that asks over the
// link, and to the server as the world snapshot.
WorldSnapshotFrames SnapshotFrames(const std::string& name) {
    WorldSnapshotFrames frames;
    for (const WorldLogSnapshot& part : world_log_host.Snapshot(name)) {
        frames.emplace_back(WorldLogSnapshotType, EncodeWorldLogSnapshot(part));
    }
    for (const WorldLogShownSnapshot& part : world_log_host.ShownSnapshot(name)) {
        frames.emplace_back(WorldLogShownSnapshotType, EncodeWorldLogShownSnapshot(part));
    }
    frames.emplace_back(WorldLogMapStateSnapshotType,
                        EncodeWorldLogMapStateSnapshot(world_log_host.MapStateSnapshot(name)));
    for (const WorldLogHpSnapshot& part : world_log_host.HpSnapshot(name)) {
        frames.emplace_back(WorldLogHpSnapshotType, EncodeWorldLogHpSnapshot(part));
    }
    frames.emplace_back(WorldLogFlagsSnapshotType,
                        EncodeWorldLogFlagsSnapshot(world_log_host.FlagsSnapshot(name)));
    return frames;
}

void SendSnapshot(const std::string& name, u32 addr, u16 port) {
    for (const auto& [type, payload] : SnapshotFrames(name)) {
        SendToPeer(EncodeCoopFrame(type, payload), addr, port);
    }
}

// The host: its log's state for the server every WorldSnapshotPostMs (the board client posts it).
void MakeWorldSnapshot(const std::string& name, s64 now_ms) {
    if (now_ms - world_snapshot_made_ms < WorldSnapshotPostMs) {
        return;
    }
    world_snapshot_made_ms = now_ms;
    std::vector<u8> blob = EncodeWorldSnapshot(SnapshotFrames(name));
    std::scoped_lock lock{world_snapshot_mutex};
    world_snapshot_out = std::move(blob);
}

void TakeFrameAsHost(u8 type, std::span<const u8> payload, u32 addr, u16 port, s64 now_ms) {
    const std::string name = LogMemberName();
    if (!world_log_host.Started() || name.empty()) {
        return;
    }
    if (type == WorldLogProposeType) {
        if (const auto proposal = DecodeWorldLogProposal(payload)) {
            TakeProposal(name, *proposal, now_ms);
        }
        return;
    }
    if (type == WorldLogFlagsProposeType) {
        if (const auto flags = DecodeWorldLogFlagProposal(payload)) {
            TakeFlagProposal(name, *flags, now_ms);
        }
        return;
    }
    if (type == WorldLogHpProposeType) {
        if (const auto hp = DecodeWorldLogHpProposal(payload)) {
            TakeHpProposal(name, *hp, now_ms);
        }
        return;
    }
    if (type == WorldLogSnapshotRequestType && DecodeWorldLogSnapshotRequest(payload)) {
        SendSnapshot(name, addr, port);
    }
}

// A frame the member's log takes: the step, the host's last seq, and the trace's name for it.
struct MemberTake {
    WorldLogMember::Step step;
    u64 host_last = 0;
    const char* op = "";
};
template <typename Part>
std::optional<MemberTake> TakePart(const std::optional<Part>& part, s64 now_ms, const char* op) {
    if (!part) {
        return std::nullopt;
    }
    return MemberTake{world_log_member.Take(*part, now_ms), part->last, op};
}

std::optional<MemberTake> TakeAsMember(u8 type, std::span<const u8> payload, s64 now_ms) {
    switch (type) {
    case WorldLogEntriesType:
        return TakePart(DecodeWorldLogBatch(payload), now_ms, "entries");
    case WorldLogSnapshotType:
        return TakePart(DecodeWorldLogSnapshot(payload), now_ms, "snapshot");
    case WorldLogShownSnapshotType:
        return TakePart(DecodeWorldLogShownSnapshot(payload), now_ms, "shown-snapshot");
    case WorldLogMapStateSnapshotType:
        return TakePart(DecodeWorldLogMapStateSnapshot(payload), now_ms, "map-state-snapshot");
    case WorldLogHpSnapshotType:
        return TakePart(DecodeWorldLogHpSnapshot(payload), now_ms, "hp-snapshot");
    case WorldLogFlagsSnapshotType:
        return TakePart(DecodeWorldLogFlagsSnapshot(payload), now_ms, "flags-snapshot");
    default:
        return std::nullopt;
    }
}

// `from`: the host's address on the link (none for the server's world snapshot).
void TakeMemberFrame(u8 type, std::span<const u8> payload, s64 now_ms,
                     std::optional<std::pair<u32, u16>> from) {
    const auto taken = TakeAsMember(type, payload, now_ms);
    if (!taken) {
        return;
    }
    const WorldLogMember::Step& step = taken->step;
    if (!step.stale && from) {
        log_host_peer = from;
    }
    if (StartsAfterAWipe(step.applied) || step.after_wipe) {
        wipe_hold.HostWiped(now_ms);
    }
    if (step.stale || step.new_epoch || step.snapshot || step.gap || !step.applied.empty()) {
        TraceLog("member", taken->op, payload);
    }
    ApplyFlagEntries(step.applied, now_ms);
    LogMemberStep(step, taken->host_last);
    if (const s32 point = world_log_member.HostReturnPoint().point;
        point > 0 && host_return_point.exchange(point) != point) {
        LOG_INFO(Debug, "Co-op world log: the host's return point is {} (map {:#x})", point,
                 world_log_member.HostReturnPoint().map);
    }
}

void TakeFrameAsMember(u8 type, std::span<const u8> payload, u32 addr, u16 port, s64 now_ms) {
    TakeMemberFrame(type, payload, now_ms, std::pair{addr, port});
}

// A member: the world snapshot the board client read, taken as the host's snapshot frames are; only
// a recent one, and only of the log this member follows (or any while it follows none: a join, a
// restart), so an old copy never takes it back from a newer host. Asked for while its copy is not
// fresh.
void TakeWorldSnapshot(s64 now_ms) {
    world_snapshot_wanted.store(!world_log_member.Fresh(now_ms));
    std::optional<std::pair<s64, std::vector<u8>>> read;
    {
        std::scoped_lock lock{world_snapshot_mutex};
        read.swap(world_snapshot_in);
    }
    const auto frames = read && read->first <= WorldSnapshotMaxAgeMs
                            ? DecodeWorldSnapshot(read->second)
                            : std::nullopt;
    if (!frames || frames->empty() || frames->front().first != WorldLogSnapshotType) {
        return;
    }
    const auto first = DecodeWorldLogSnapshot(frames->front().second);
    const u64 epoch = world_log_member.Epoch();
    if (!first || (epoch != 0 && (first->epoch >> 32) != (epoch >> 32))) {
        return;
    }
    LOG_INFO(Debug, "Co-op world log: took the world snapshot from the server (log {:#x}, seq {})",
             first->epoch, first->last);
    for (const auto& [type, payload] : *frames) {
        TakeMemberFrame(type, payload, now_ms, std::nullopt);
    }
}

// The shared flags and the map state take the log with the kills. Leaving it, this game's events
// not taken yet go to the flag board, and the host's log stops carrying its map state.
void FollowFlagsPath(bool log, s64 now_ms) {
    if (flags_on_log.exchange(log) && !log) {
        world_log_host.ForgetMapState();
        std::scoped_lock lock{flag_board_mutex};
        for (const WorldLogFlagEvent& event : flag_outbox.Pending(now_ms)) {
            flags_to_post.emplace_back(event.flag, event.on);
        }
        flag_outbox.Clear(); // its numbers go on: the host took them by number
    }
    // What a guest's load takes of the host's map flags: the log's copy (kept by the link, or by
    // the world snapshot while it is down).
    std::optional<MemberMapState> copy;
    if (log && IsPartyGuestRole() && world_log_member.MapStateKnown() &&
        world_log_member.MapState().map != 0) {
        copy = MemberMapState{world_log_member.MapState().map, world_log_member.MapState().bits};
    }
    std::scoped_lock lock{map_state_mutex};
    log_map_state = std::move(copy);
}

// The maps the party's log reset on their own (see TakeLogMapResets); the game thread only.
std::vector<u32> log_map_resets;

// The party's world on the server (phase 3): this member's kills, flags and (the host's) map
// state and return point to it, what came from it applied. The party's kills as it has them.
std::unordered_set<u32> RunServerWorld(const WorldLogKills& mine, s64 now_ms) {
    flags_on_log.store(true);
    const bool in_session = InPartySession();
    const ServerWorldKills world =
        TickServerWorld(mine, in_session && !wipe_hold.HoldsProposals(now_ms), now_ms);
    if (world.host_wiped) {
        wipe_hold.HostWiped(now_ms);
    }
    for (const WorldLogEntry& entry : world.applied) {
        if (entry.kind == WorldLogKind::Reset && entry.map != 0) {
            log_map_resets.push_back(entry.map);
            // A map that starts over while others stand elsewhere (ResetMap): a member dead on
            // it, its own wipe still to run, holds that map's HP from before it as at a party
            // wipe (rig 2026-10-20, wipe-one-map: the host's game sent the hurt enemy's 482 HP
            // into the fresh log, and every game took it back after the reload).
            if (PlayingMap() == entry.map) {
                wipe_hold.HostWiped(now_ms);
            }
        }
    }
    ApplyFlagEntries(world.applied, now_ms);
    if (!in_session) {
        return {};
    }
    if (IsPartyGuestRole()) {
        if (const s32 point = world.return_point.point;
            point > 0 && host_return_point.exchange(point) != point) {
            LOG_INFO(Debug, "Co-op server world: the host's return point is {} (map {:#x})", point,
                     world.return_point.map);
        }
    } else if (const u64 global = GetGlobalState(); global >= 0x10000) {
        const s32 point = ReadValue<s32>(global, 0x1528);
        ReturnPointToServer(point, point > 0 ? PackedMapOfWarpPoint(point) : 0);
    }
    return world.kills;
}

} // namespace

namespace Coop {

std::vector<u32> TakeLogMapResets() {
    return std::exchange(log_map_resets, {});
}

bool WorldLogSwitchOn() {
    return world_log_switch;
}

void TakeWorldLogFrame(u8 type, std::span<const u8> payload, u32 addr, u16 port) {
    const s64 now_ms = SteadyMilliseconds();
    if (IsPartyGuestRole()) {
        TakeFrameAsMember(type, payload, addr, port, now_ms);
    } else {
        TakeFrameAsHost(type, payload, addr, port, now_ms);
    }
}

void ShowOnTheLog(u32 map, std::span<const CharacterShown> characters) {
    if (ServerWorldOn()) {
        if (!IsPartyGuestRole() && map != 0 && InPartySession()) {
            ShowToServer(map, characters);
        }
        return;
    }
    const std::string name = LogMemberName();
    if (!world_log_path.Active() || !world_log_host.Started() || name.empty() || map == 0) {
        return;
    }
    const std::vector<WorldLogEntry> entries = world_log_host.Show(map, characters);
    if (!entries.empty()) {
        TraceLog("host", "show", EncodeWorldLogShow(map, characters));
        SendEntries(name, entries, SteadyMilliseconds());
    }
}

std::optional<MemberMapState> HostShownNow() {
    if (ServerWorldOn()) {
        const std::optional<WorldLogShown> shown = ServerShown();
        if (!shown || shown->map == 0) {
            return std::nullopt;
        }
        return MemberMapState{shown->map, EncodeShownCharacters(WorldLogShownCharacters(*shown))};
    }
    if (world_log_path.Active()) {
        const WorldLogShown& shown = world_log_member.Shown();
        if (!world_log_member.ShownKnown() || shown.map == 0) {
            return std::nullopt;
        }
        return MemberMapState{shown.map, EncodeShownCharacters(WorldLogShownCharacters(shown))};
    }
    return std::nullopt; // the P2P path: no host's copy (the HTTP boards are gone)
}

std::vector<MemberMapState> OthersEnemyHp(u64 own_source) {
    const s64 now_ms = SteadyMilliseconds();
    const bool hosting = !IsPartyGuestRole();
    // The peers' direct frames first: on the log path the fast copy of the same reports (a hit
    // moves the bars on the first hop; each report is cumulative per member and visit and
    // MergeEnemyHpReports takes the most damage of its copies, so nothing counts twice).
    std::vector<MemberMapState> others = direct_damage_inbox.Fresh(now_ms);
    if (ServerWorldOn()) {
        // The reports the server's log has (this member's own left out).
        for (const auto& [map, report] : WorldLogHpReports(ServerHp())) {
            if ((report.visit & ~HostHpReport) != own_source) {
                others.push_back({map, EncodeEnemyHp(report.visit, report.enemies)});
            }
        }
        return others;
    }
    if (world_log_path.Active()) {
        // The reports the host's log has (this member's own left out).
        for (const auto& [map, report] :
             WorldLogHpReports(hosting ? world_log_host.Hp() : world_log_member.Hp())) {
            if ((report.visit & ~HostHpReport) != own_source) {
                others.push_back({map, EncodeEnemyHp(report.visit, report.enemies)});
            }
        }
        return others;
    }
    return others; // the P2P path: the peers' direct frames only (the HP board is gone)
}

void SendEnemyHp(const MemberMapState& mine, bool changed) {
    const s64 now_ms = SteadyMilliseconds();
    const bool held = HoldsHpReports(wipe_hold, ServerWorldOn(), IsPartyGuestRole(), now_ms);
    // Phase 4's owners: the game sends the owner's HP itself (ch 5), no copy of ours.
    if (!held && !EnemyOwnerOn()) {
        SendDirectDamage(mine, changed); // the fast copy (see OthersEnemyHp)
    }
    if (ServerWorldOn()) {
        const auto report = mine.map != 0 ? DecodeEnemyHp(mine.bits) : std::nullopt;
        // A guest's damage from before its own wipe must not stand (see WorldLogWipeHold).
        if (report && !held && InPartySession()) {
            HpToServer(mine.map, *report, changed);
        }
        return;
    }
    if (!world_log_path.Active()) {
        return;
    }
    const std::string name = LogMemberName();
    const auto report = mine.map != 0 ? DecodeEnemyHp(mine.bits) : std::nullopt;
    if (!report || name.empty() || (!changed && now_ms - hp_sent_ms < WorldLogResendMs)) {
        return;
    }
    hp_sent_ms = now_ms;
    if (!IsPartyGuestRole()) {
        if (world_log_host.Started()) {
            TakeHpProposal(name,
                           {name, world_log_host.Epoch(), world_log_run, ++hp_proposals_made,
                            mine.map, *report},
                           now_ms);
        }
        return;
    }
    if (held) {
        return; // its damage from before its own wipe must not stand (see WorldLogWipeHold)
    }
    const WorldLogHpProposal proposal{
        name, world_log_member.Epoch(), world_log_run, ++hp_proposals_made, mine.map, *report};
    SendToRoomPeers(EncodeCoopFrame(WorldLogHpProposeType, EncodeWorldLogHpProposal(proposal)));
}

bool WorldLogCarriesBoards() {
    return ServerWorldOn() || flags_on_log.load();
}

std::optional<std::vector<u8>> TakeWorldSnapshotToPost() {
    std::scoped_lock lock{world_snapshot_mutex};
    return std::exchange(world_snapshot_out, std::nullopt);
}

bool WorldSnapshotWanted() {
    return IsPartyGuestRole() && world_snapshot_wanted.load();
}

void NoteWorldSnapshot(s64 age_ms, std::vector<u8> blob) {
    std::scoped_lock lock{world_snapshot_mutex};
    world_snapshot_in = std::pair{age_ms, std::move(blob)};
}

bool TakeFlagsForTheLog(std::span<const std::pair<s32, bool>> shared) {
    if (ServerWorldOn()) {
        FlagsToServer(shared);
        return true;
    }
    if (!flags_on_log.load()) {
        return false;
    }
    flag_outbox.Add(shared, SteadyMilliseconds());
    return true;
}

void FilterBoardFlags(std::vector<std::pair<s32, bool>>& changes) {
    if (!flags_on_log.load()) {
        return;
    }
    const s64 now_ms = SteadyMilliseconds();
    std::erase_if(changes, [now_ms](const std::pair<s32, bool>& change) {
        return !flag_once.First(change.first, change.second, now_ms);
    });
}

void MapStateOnTheLog(u32 map, std::span<const u8> bits) {
    if (ServerWorldOn()) {
        if (!IsPartyGuestRole() && map != 0 && InPartySession()) {
            MapStateToServer(map, bits);
        }
        return;
    }
    const std::string name = LogMemberName();
    if (!flags_on_log.load() || !world_log_host.Started() || name.empty()) {
        return;
    }
    const std::vector<WorldLogEntry> entries = world_log_host.MapState(map, bits);
    if (!entries.empty()) {
        TraceLog("host", "map-state", EncodeWorldLogMapStateInput(map, bits));
        SendEntries(name, entries, SteadyMilliseconds());
    }
}

std::optional<MemberMapState> HostMapStateNow() {
    if (ServerWorldOn()) {
        const std::optional<WorldLogMapState> state = ServerMapState();
        if (!state || state->map == 0) {
            return std::nullopt;
        }
        return MemberMapState{state->map, state->bits};
    }
    std::scoped_lock lock{map_state_mutex};
    return log_map_state ? log_map_state : host_map_state;
}

std::optional<s32> HostReturnPointNow() {
    const s32 point = host_return_point.load();
    if (point <= 0 || !IsPartyGuestRole() || !InPartySession()) {
        return std::nullopt;
    }
    return point;
}

PartyKillsNow GatherPartyKills(const VisitKills& visit_kills, u32 map,
                               std::span<const std::pair<s32, bool>> changes, bool forgot,
                               const std::unordered_set<u32>& not_mine) {
    const s64 now_ms = SteadyMilliseconds();
    if (forgot) {
        wipe_hold.OwnWipe(now_ms);
    }
    // What this member proposes: its standing kills, but not another member's kill that died here
    // by the game's kill (it counted as this member's own in its visit).
    WorldLogKills mine = visit_kills.Standing();
    for (auto& [on_map, handles] : mine) {
        std::erase_if(handles, [&](u32 handle) { return not_mine.contains(handle); });
    }
    std::erase_if(mine, [](const auto& entry) { return entry.second.empty(); });
    if (member_reset.exchange(false)) {
        host_return_point.store(0); // another session or character: another host
        if (IsPartyGuestRole()) {
            world_log_member.Reset();
            log_host_peer.reset();
            TraceLog("member", "reset", {});
        }
    }
    // With the party's world on the server everything the host's log carried goes there instead
    // (phase 3): neither the host's log nor the boards run.
    const bool server = ServerWorldOn();
    bool log = false;
    if (!server) {
        log = DecideKillPath(mine, now_ms);
        FollowFlagsPath(log, now_ms);
        const std::string name = LogMemberName();
        if (log && !name.empty()) {
            if (IsPartyGuestRole()) {
                RunMemberLog(name, mine, now_ms);
                TakeWorldSnapshot(now_ms);
            } else {
                RunHostLog(name, mine, now_ms);
            }
        }
    }
    PartyKillsNow party;
    if (server) {
        party.members = RunServerWorld(mine, now_ms);
    } else if (log) {
        party.members = LogKills();
    }
    // Its own kills too (a guest pulled into the host's world sees the host's copy alive), and its
    // earlier visit's kills here that stood while another member stayed.
    party.all = party.members;
    party.all.insert(visit_kills.Killed().begin(), visit_kills.Killed().end());
    const std::vector<u32> held = visit_kills.Held(map);
    party.all.insert(held.begin(), held.end());
    // On the board path, straight over the P2P link: this member's standing kills out, the peers'
    // in (the board brings the same a moment later, and to members who load in later).
    if (!server && !log && InPartySession()) {
        std::vector<u32> standing(visit_kills.Killed().begin(), visit_kills.Killed().end());
        standing.insert(standing.end(), held.begin(), held.end());
        SendDirectKills(standing, !changes.empty());
        std::scoped_lock lock{direct_boards_mutex};
        const std::unordered_set<u32> direct = DirectKillHandles(direct_kills_inbox.Fresh(now_ms));
        party.all.insert(direct.begin(), direct.end());
    }
    return party;
}

} // namespace Coop

} // namespace Core::Bloodborne
