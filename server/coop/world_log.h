// SPDX-FileCopyrightText: Copyright 2026 shadNet Project
// SPDX-License-Identifier: GPL-2.0-or-later

// The world log (docs/WORLD_SYNC.md, Stage C; docs/SERVER_COOP_PLAN.md phase 3), shared by the game
// and shadNet: the types, the frames' encodings, the host's log (WorldLogHost) and a member's copy
// (WorldLogMember). Only the standard library: the emulator, the port and shadNet compile it as it
// is (shadNet runs the host's side for a party's world, the games follow it).
#pragma once

#include <algorithm>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

#include "coop/coop_types.h"

namespace BloodborneCoop {

// The flags of a map's state from MapFlagBase (the emulator's): bits MSB first.
constexpr s32 MapFlagCount = 10000;

// Kaniel's playtest 2026-10-17: his Central Yharnam kills came back for him only after a lamp
// trip while both guests stood in Cathedral Ward, where Central Yharnam's enemies are loaded too.
// Maps the game loads together count as one for "someone is on that map" (the Ward and Central
// Yharnam).
constexpr bool MapsLoadedTogether(u32 a, u32 b) {
    constexpr u32 CathedralWard = 0x18000000;
    constexpr u32 CentralYharnam = 0x18010000;
    return a == b || (a == CathedralWard && b == CentralYharnam) ||
           (a == CentralYharnam && b == CathedralWard);
}
// `map` and the maps loaded with it.
inline std::vector<u32> MapsLoadedWith(u32 map) {
    std::vector<u32> maps{map};
    for (const u32 other : {u32{0x18000000}, u32{0x18010000}}) {
        if (other != map && MapsLoadedTogether(other, map)) {
            maps.push_back(other);
        }
    }
    return maps;
}

// Enemy HP kept in step (POST /bloodborne/party/hp, the map state board's format): each member
// posts the enemies it has hurt during its visit, as (handle, HP, max HP), and every member
// shows the lowest share of HP left. Since seamless travel each member runs its own copy of a
// map, so the host's hits never reached the guest's Gascoigne (friend run 2026-10-09).
struct EnemyHp {
    u32 handle = 0;
    s32 hp = 0;
    s32 max = 0;
    bool operator==(const EnemyHp&) const = default;
};
struct EnemyHpReport {
    u64 visit = 0;
    std::vector<EnemyHp> enemies;
    bool operator==(const EnemyHpReport&) const = default;
};

// In one world the game sends enemy HP from the host, but not an NPC's (rig 2026-10-10: the
// host's Oedon Chapel NPC at 422 while the guest's showed 910). There the host posts the HP its
// hurt characters have (EnemyHp.hp is HP, the report's source has HostHpReport set), and a guest
// lowers its copies to that share; the guest's own hits reach the host by the game.
constexpr u64 HostHpReport = u64{1} << 63;

// Characters hidden or shown as the host's world has them (POST /bloodborne/party/shown, the map
// state board's format): the host posts every character of its map with its event disable byte
// (+0x28, "hidden by the story"), and a guest on that map in the session sets its copies the
// same. A guest's world, rebuilt as the host's client when it joins, does not run the events
// that hide story NPCs (Iosefka at her clinic door, the Doll), so it showed them.
struct CharacterShown {
    u32 handle = 0;
    bool hidden = false;
    // Its enable switch off (2004:5; the byte's bit 1): the host's story switches some story
    // NPCs off instead (the copy of Iosefka by her door, 0x10014000, on new characters).
    bool disabled = false;
    // Farther than FarFromHostMetres from the host (bit 2). The host's events park characters far
    // from its player (+0x28 and the switch: the woods' enemies by the Cathedral Ward border), so
    // a guest elsewhere does not follow those (Kaniel's friend, 2026-10-11: an empty forest).
    bool distant = false;
    // Dead in the host's world (bit 3; IsDeadInItsWorld), killed now or dead in its save. Guests
    // whose worlds are apart take these as killed (DeadInTheHostsWorld).
    bool dead = false;
    // Its hits switched off (bit 4; event command 2004:39). The Witch of Hemwick's Mad Ones wait
    // with their hits off, and her summon turns them on only in the host's game (Kaniel
    // 2026-10-20: only the host saw them). Guests follow it (HitsLikeTheHost).
    bool hits_off = false;
    bool operator==(const CharacterShown&) const = default;
};

// The world log (docs/WORLD_SYNC.md, Stage C): one ordered list of world changes, kept by the
// host and sent to every member over the P2P link, instead of a board per feature. Step 1 carries
// the kills (the kill board's job before); shown state, flags and enemy HP come later.
//
// Members propose, the host orders: each member sends its standing kills, whole, to the host
// (WorldLogProposal); the host keeps every member's set and appends an entry when the party's
// union changes (the first member's kill of an enemy is a Kill, the last one's withdrawal an
// Unkill, so two members' kills of one enemy are one entry). Entries carry a seq that only grows
// within the host's log epoch; members apply them in seq order exactly once, and ask for a
// snapshot (the union per map and the last seq) when they find a gap, join late or meet a new
// epoch past its start. A new epoch (the host's new session, wipe or character) starts with a
// Reset of every map. The kill board stays underneath as the fallback copy: the host posts the
// union there.
//
// Step 2 carries the shown characters (the shown board's job before): the host's map and each of
// its characters' CharacterShown bits, as entries when they change (ShownMap, Shown, Unshown),
// and in a snapshot of their own (WorldLogShownSnapshotType). A guest follows them as it followed
// the board's copy. A member that meets a kind it does not know counts it as applied and does
// nothing, so a protocol 2 member skips the shown entries.
enum class WorldLogKind : u8 {
    Reset = 0,    // every kill on `map` goes (map 0: on every map), and the shown state there
    Kill = 1,     // `subject` (a character's handle) is dead on `map`
    Unkill = 2,   // `subject` is no longer
    ShownMap = 3, // the host's shown characters are of `map` from now on (none known yet)
    Unshown = 4,  // `subject` is no longer in the host's world on `map`
    // `subject` on `map` has `value` (ShownBits) in the host's world; on the link the kind byte
    // is 0x10 | the low four bits, and the value has them all.
    Shown = 0x10,
    // Step 3 (the flag boards' job): a shared flag (FlagsToShare: a lamp lit, a boss fog entered)
    // turned on or off in a member's world, `subject` the flag; every member sets it. Events, not
    // state: a snapshot does not carry them (a member that joins later gets none of the earlier
    // ones, as a game's first flag board post gets none), the flag board's copy covers a gap.
    FlagSet = 5,
    FlagClear = 6,
    // The host's map state (the map state board's job): its flags on `map` are from now on all
    // off but the MapFlagOn ones that follow; MapFlagOn / MapFlagOff: flag MapFlagBase(map) +
    // `subject` turned on / off. A guest loading that map takes the TakesHostMapFlag ones.
    MapStateMap = 7,
    MapFlagOn = 8,
    MapFlagOff = 9,
    // Step 4 (the HP board's and the direct damage frames' job): each member's HP report
    // (EnemyHpReport: its damage per enemy this visit, or the host's HP in one world), by source.
    // HpSource: the entries after it are of source `subject` << 32 | `value` on `map`; HpValue:
    // `subject` (a handle) has `value` (damage or HP) of max `map`; HpGone: `subject` is no longer
    // in that report; HpSourceGone: source `subject` << 32 | `value` went (a new visit, a member's
    // restart).
    HpSource = 10,
    HpValue = 11,
    HpGone = 12,
    HpSourceGone = 13,
    // The host's return point (GlobalState +0x1528, where its death respawn wakes it) is `subject`
    // from now on, on `map` (PackedMapOfWarpPoint, 0 when not known): a guest's party death wakes
    // there too (see PartyRespawnPoint). A new log starts with it; a snapshot's map state part
    // carries it.
    ReturnPoint = 14,
};
struct WorldLogEntry {
    u64 seq = 0;
    u64 epoch = 0;
    u32 map = 0;
    WorldLogKind kind = WorldLogKind::Kill;
    u32 subject = 0;
    u32 value = 0; // Shown: the bits; HP entries: see WorldLogKind
    bool operator==(const WorldLogEntry&) const = default;
};
// A character's CharacterShown bits (hidden 1, disabled 2, distant 4, dead 8, hits off 16, as on
// the shown board) and back.
u8 ShownBits(const CharacterShown& character);
CharacterShown ShownFromBits(u32 handle, u8 bits);
// The host's shown characters: its map and each character's bits.
struct WorldLogShown {
    u32 map = 0;
    std::map<u32, u8> bits;
    bool operator==(const WorldLogShown&) const = default;
};
std::vector<CharacterShown> WorldLogShownCharacters(const WorldLogShown& shown);
// The host's map state: its map and the MapFlagCount flags from MapFlagBase(map), MSB first (as on
// the map state board, MemberMapState::bits). Empty bits for map 0.
struct WorldLogMapState {
    u32 map = 0;
    std::vector<u8> bits;
    bool operator==(const WorldLogMapState&) const = default;
};
// The host's return point (WorldLogKind::ReturnPoint): point 0 when none is known.
struct WorldLogReturnPoint {
    s32 point = 0;
    u32 map = 0;
    bool operator==(const WorldLogReturnPoint&) const = default;
};
// A member's shared flag events to the host (frame WorldLogFlagsProposeType): each with a number
// that counts up within the member's run, resent for WorldLogFlagResendWindowMs so a lost frame
// costs nothing; the host takes each number once.
struct WorldLogFlagEvent {
    u32 number = 0;
    s32 flag = 0;
    bool on = true;
    bool operator==(const WorldLogFlagEvent&) const = default;
};
struct WorldLogFlagProposal {
    std::string member;
    u64 epoch = 0; // the host's log this member follows, 0 before it heard one
    u32 run = 0;
    std::vector<WorldLogFlagEvent> events;
    bool operator==(const WorldLogFlagProposal&) const = default;
};
constexpr s64 WorldLogFlagResendWindowMs = 10'000;
constexpr size_t WorldLogFlagEventsPerFrame = 64;
// The same flag turned the same way again this soon is the same event (two members lit one lamp;
// the log's and the flag board's copies of one event).
constexpr s64 WorldLogFlagRepeatMs = 10'000;
// A member's events waiting to be taken: numbered as they come, sent until they are
// WorldLogFlagResendWindowMs old, WorldLogFlagEventsPerFrame a frame: those not out yet, oldest
// first, else the newest again.
class WorldLogFlagOutbox {
public:
    void Add(std::span<const std::pair<s32, bool>> changes, s64 now_ms);
    std::vector<WorldLogFlagEvent> Pending(s64 now_ms);
    // Whether something came since the last call (it goes out at once).
    bool TakeNew() {
        return std::exchange(fresh, false);
    }
    // The events went another way (the flag board): none waits any more.
    void Clear() {
        sent.clear();
        fresh = false;
    }

private:
    u32 next = 0;
    u32 out_to = 0; // the newest number sent
    std::vector<std::pair<WorldLogFlagEvent, s64>> sent;
    bool fresh = false;
};
// One flag event from either copy (the log's entries, the flag board's answers): true the first
// time (flag, on) is seen within WorldLogFlagRepeatMs, and it is remembered.
class FlagEventOnce {
public:
    bool First(s32 flag, bool on, s64 now_ms);
    using Seen = std::map<std::pair<s32, bool>, s64>;
    const Seen& All() const {
        return seen;
    }
    void Restore(Seen saved) {
        seen = std::move(saved);
    }

private:
    Seen seen;
};
// Standing kills by map.
using WorldLogKills = std::map<u32, std::set<u32>>;
size_t WorldLogKillCount(const WorldLogKills& kills);
std::unordered_set<u32> WorldLogHandles(const WorldLogKills& kills);
// A log epoch is the host's run (its upper half, random per game start) and its count of logs
// started in that run. A member follows any epoch of another run (the host's game restarted), and
// within one run only a later one (a frame of an older log that came late is dropped).
u64 WorldLogEpoch(u32 run, u32 count);
bool WorldLogEpochFollows(u64 next, u64 current);

// The frames (on the co-op channel, after the hello): older builds drop these types (their
// decoders take only 0x30-0x33).
constexpr u8 WorldLogProposeType = 0x34;         // member -> host: its standing kills
constexpr u8 WorldLogEntriesType = 0x35;         // host -> members: entries, or a heartbeat
constexpr u8 WorldLogSnapshotRequestType = 0x36; // member -> host
constexpr u8 WorldLogSnapshotType = 0x37;        // host -> that member, in parts
// host -> that member, after the kills' parts: the shown characters at the same seq (protocol 2
// builds drop it, as they drop any type they do not know)
constexpr u8 WorldLogShownSnapshotType = 0x38;
// member -> host: its shared flag events (WorldLogFlagProposal; protocol 3 builds drop it)
constexpr u8 WorldLogFlagsProposeType = 0x39;
// host -> that member, after the shown parts: the host's map state at the same seq
constexpr u8 WorldLogMapStateSnapshotType = 0x3A;
// member -> host: its HP report (WorldLogHpProposal), at once on a change and every
// WorldLogResendMs
constexpr u8 WorldLogHpProposeType = 0x3B;
// host -> that member, after the map state: every HP report at the same seq, in parts
constexpr u8 WorldLogHpSnapshotType = 0x3C;
// host -> that member, last: the flag events of the log's last WorldLogRecentFlags (a member that
// missed some in a gap sets them; one that joins gets none, as a game's first flag board post)
constexpr u8 WorldLogFlagsSnapshotType = 0x3D;
constexpr size_t WorldLogRecentFlags = 64;
bool IsWorldLogFrameType(u8 type);
// A member's proposal and the host's heartbeat go out at once on a change and again every
// WorldLogResendMs; a member's log stands WorldLogLifeMs after the host's last frame (else the
// kill board's copy is used); a snapshot is asked for at most every WorldLogSnapshotRetryMs.
constexpr s64 WorldLogResendMs = 500;
constexpr s64 WorldLogLifeMs = 3'000;
constexpr s64 WorldLogSnapshotRetryMs = 1'000;
// Kept small for the link's 1400-byte frames: entries per entries frame, handles per proposal or
// snapshot part (a member's standing kills above it are cut, as the direct kills frame did).
constexpr size_t WorldLogEntriesPerFrame = 100; // 13 bytes each
// Enemies per HP proposal or HP snapshot part (12 bytes each).
constexpr size_t WorldLogHpPerFrame = 100;
constexpr size_t WorldLogHandlesPerFrame = 280;
// Characters per shown snapshot part (5 bytes each).
constexpr size_t WorldLogShownPerFrame = 240;
// Entries a member keeps past a gap until the snapshot comes.
constexpr size_t WorldLogWaitingCap = 2'000;

struct WorldLogProposal {
    std::string member; // the board's name for the member
    u64 epoch = 0;      // the host's log epoch this member follows, 0 before it heard one
    u32 run = 0;        // this game's run: a restart is a new run (its old kills go)
    u32 number = 0;     // counts up within a run: an older proposal that comes late is dropped
    WorldLogKills kills;
    bool operator==(const WorldLogProposal&) const = default;
};
struct WorldLogBatch {
    std::string host; // the host's board name (its own kill board entries are the log's copy)
    u64 epoch = 0;
    u64 last = 0;                       // the host's last seq: no entries is a heartbeat
    std::vector<WorldLogEntry> entries; // consecutive seqs, all of `epoch`
    bool operator==(const WorldLogBatch&) const = default;
};
struct WorldLogSnapshotRequest {
    std::string member;
    u64 epoch = 0; // what the member has
    u64 seq = 0;
    bool operator==(const WorldLogSnapshotRequest&) const = default;
};
struct WorldLogSnapshot {
    std::string host;
    u64 epoch = 0;
    u64 last = 0;
    u8 part = 0;
    u8 parts = 1;
    WorldLogKills kills; // this part's share
    bool operator==(const WorldLogSnapshot&) const = default;
};
std::vector<u8> EncodeWorldLogProposal(const WorldLogProposal& proposal);
std::optional<WorldLogProposal> DecodeWorldLogProposal(std::span<const u8> payload);
std::vector<u8> EncodeWorldLogBatch(const WorldLogBatch& batch);
std::optional<WorldLogBatch> DecodeWorldLogBatch(std::span<const u8> payload);
std::vector<u8> EncodeWorldLogSnapshotRequest(const WorldLogSnapshotRequest& request);
std::optional<WorldLogSnapshotRequest> DecodeWorldLogSnapshotRequest(std::span<const u8> payload);
std::vector<u8> EncodeWorldLogSnapshot(const WorldLogSnapshot& snapshot);
std::optional<WorldLogSnapshot> DecodeWorldLogSnapshot(std::span<const u8> payload);
struct WorldLogShownSnapshot {
    std::string host;
    u64 epoch = 0;
    u64 last = 0;
    u8 part = 0;
    u8 parts = 1;
    WorldLogShown shown; // the map, and this part's share of the characters
    bool operator==(const WorldLogShownSnapshot&) const = default;
};
std::vector<u8> EncodeWorldLogShownSnapshot(const WorldLogShownSnapshot& snapshot);
std::optional<WorldLogShownSnapshot> DecodeWorldLogShownSnapshot(std::span<const u8> payload);
struct WorldLogMapStateSnapshot {
    std::string host;
    u64 epoch = 0;
    u64 last = 0;
    WorldLogMapState state;           // one part: the bitmap fits a frame
    WorldLogReturnPoint return_point; // the host's, at the same seq
    bool operator==(const WorldLogMapStateSnapshot&) const = default;
};
std::vector<u8> EncodeWorldLogMapStateSnapshot(const WorldLogMapStateSnapshot& snapshot);
std::optional<WorldLogMapStateSnapshot> DecodeWorldLogMapStateSnapshot(std::span<const u8> payload);
std::vector<u8> EncodeWorldLogFlagProposal(const WorldLogFlagProposal& proposal);
std::optional<WorldLogFlagProposal> DecodeWorldLogFlagProposal(std::span<const u8> payload);
// A member's HP report as the host keeps it: the report's map and each enemy's (value, max).
struct WorldLogHpReport {
    u32 map = 0;
    std::map<u32, std::pair<s32, s32>> enemies;
    bool operator==(const WorldLogHpReport&) const = default;
};
using WorldLogHp = std::map<u64, WorldLogHpReport>; // by source (EnemyHpReport::visit)
// The reports as SharedDamage takes them, with their maps.
std::vector<std::pair<u32, EnemyHpReport>> WorldLogHpReports(const WorldLogHp& hp);
struct WorldLogHpProposal {
    std::string member;
    u64 epoch = 0; // the host's log this member follows, 0 before it heard one
    u32 run = 0;
    u32 number = 0; // counts up within the run: a late one is dropped
    u32 map = 0;
    EnemyHpReport report; // at most WorldLogHpPerFrame enemies
    bool operator==(const WorldLogHpProposal&) const = default;
};
std::vector<u8> EncodeWorldLogHpProposal(const WorldLogHpProposal& proposal);
std::optional<WorldLogHpProposal> DecodeWorldLogHpProposal(std::span<const u8> payload);
struct WorldLogHpSnapshot {
    std::string host;
    u64 epoch = 0;
    u64 last = 0;
    u8 part = 0;
    u8 parts = 1;
    WorldLogHp hp; // this part's share (a source may span parts)
    bool operator==(const WorldLogHpSnapshot&) const = default;
};
std::vector<u8> EncodeWorldLogHpSnapshot(const WorldLogHpSnapshot& snapshot);
std::optional<WorldLogHpSnapshot> DecodeWorldLogHpSnapshot(std::span<const u8> payload);
struct WorldLogFlagsSnapshot {
    std::string host;
    u64 epoch = 0;
    u64 last = 0;
    std::vector<WorldLogEntry> events; // FlagSet / FlagClear entries, oldest first
    bool after_wipe = false; // the log started at a wipe (its Reset's WorldLogResetAfterWipe)
    bool operator==(const WorldLogFlagsSnapshot&) const = default;
};
std::vector<u8> EncodeWorldLogFlagsSnapshot(const WorldLogFlagsSnapshot& snapshot);
std::optional<WorldLogFlagsSnapshot> DecodeWorldLogFlagsSnapshot(std::span<const u8> payload);

// `entries` (consecutive) as batches of at most WorldLogEntriesPerFrame; one heartbeat for none.
std::vector<WorldLogBatch> BatchWorldLog(std::string_view host, u64 epoch, u64 last,
                                         std::span<const WorldLogEntry> entries);

// The host's side: every member's standing kills and the log of the party's union.
class WorldLogHost {
public:
    // A new log (a new epoch): a Reset of every map and no kills, the members' sets gone (a new
    // session or character, or a party wipe, which respawns the map for everyone: rig smoke
    // 2026-10-12, run 191219, the kept sets stood the host's own kill again). `after_wipe` marks
    // the Reset (WorldLogResetAfterWipe): a member proposes again only after its own wipe (see
    // WorldLogWipeHold).
    std::vector<WorldLogEntry> Start(u64 epoch, bool after_wipe);
    struct Taken {
        bool stale = false; // dropped: an older one of the member's run, or another log epoch
        std::vector<WorldLogEntry> entries;
    };
    // A member's (or the host's own) proposal. Its set replaces the member's last one.
    Taken Propose(const WorldLogProposal& proposal);
    bool Started() const {
        return epoch != 0;
    }
    u64 Epoch() const {
        return epoch;
    }
    u64 Last() const {
        return last;
    }
    // The party's standing kills.
    WorldLogKills Kills() const;
    // The kills in parts of at most WorldLogHandlesPerFrame handles (one part when there are none).
    std::vector<WorldLogSnapshot> Snapshot(std::string_view host) const;
    // The host's characters on its map now: the entries that bring the log's copy to them (on
    // another map a ShownMap first, then each character).
    std::vector<WorldLogEntry> Show(u32 map, std::span<const CharacterShown> characters);
    // The shown characters left the log (its path went back to the board): the next Show starts
    // with a ShownMap.
    void ForgetShown() {
        shown = {};
    }
    const WorldLogShown& Shown() const {
        return shown;
    }
    // The shown characters in parts of at most WorldLogShownPerFrame (one part when there are
    // none), at the same seq as Snapshot.
    std::vector<WorldLogShownSnapshot> ShownSnapshot(std::string_view host) const;
    // A member's (or the host's own) shared flag events: an entry for each new one (by number
    // within its run), but none for a flag turned the same way within WorldLogFlagRepeatMs.
    Taken ProposeFlags(const WorldLogFlagProposal& proposal, s64 now_ms);
    // The host's map state now (its map's flags): the entries that bring the log's copy to it
    // (another map: a MapStateMap, then each flag on).
    std::vector<WorldLogEntry> MapState(u32 map, std::span<const u8> bits);
    // The flags left the log (their path went back to the boards).
    void ForgetMapState() {
        map_state = {};
    }
    const WorldLogMapState& MapStateNow() const {
        return map_state;
    }
    // The map state part carries the return point too.
    WorldLogMapStateSnapshot MapStateSnapshot(std::string_view host) const;
    // The host's return point now (GlobalState +0x1528 and its map): an entry when it changed. A
    // point that cannot be read (0 or below, a load) leaves the last one standing.
    std::vector<WorldLogEntry> ReturnPoint(s32 point, u32 map);
    const WorldLogReturnPoint& ReturnPointNow() const {
        return return_point;
    }
    // A member's (or the host's own) HP report, whole: the entries for what changed (a new source
    // of the member's first: its old one goes).
    Taken ProposeHp(const WorldLogHpProposal& proposal);
    const WorldLogHp& Hp() const {
        return hp;
    }
    // The HP reports in parts of at most WorldLogHpPerFrame enemies (one part when there are none).
    std::vector<WorldLogHpSnapshot> HpSnapshot(std::string_view host) const;
    // The flag events of the log's last WorldLogRecentFlags entries.
    WorldLogFlagsSnapshot FlagsSnapshot(std::string_view host) const;
    // A member went from the party's world (its own leave or new character, on the server): its
    // kills and HP report go, the entries for that.
    std::vector<WorldLogEntry> Drop(const std::string& member);
    // Kaniel's play test 2026-10-19: every member on `map` died while another stood elsewhere,
    // and the map came back half respawned, different for each of them. Nobody stands on it, so
    // it starts over (as a wipe does, on that map alone): a Reset of `map`, its kills and HP
    // reports gone, the host's shown characters there alive again. The members' kills there go
    // too; a member's proposal that still holds one (sent before it heard the Reset) does not
    // stand it again. The maps loaded with it start over with it (a kill is of the map it died
    // on, which a walk across the Ward's border makes the other one).
    std::vector<WorldLogEntry> ResetMap(u32 map);

    struct Member {
        u32 run = 0;
        u32 number = 0;
        WorldLogKills kills;
        // Its kills on a map reset since (ResetMap) that its proposals still hold: not taken
        // again until a proposal leaves one out.
        std::set<u32> voided;
        bool operator==(const Member&) const = default;
    };
    struct FlagSender {
        u32 run = 0;
        u32 number = 0; // the last event taken
        bool operator==(const FlagSender&) const = default;
    };
    struct Standing {
        u32 map = 0;
        std::set<std::string> by;
        bool operator==(const Standing&) const = default;
    };
    struct HpSender {
        u32 run = 0;
        u32 number = 0;
        u64 source = 0;
        bool operator==(const HpSender&) const = default;
    };
    // Everything the log is, to keep and to load again (shadNet's world store).
    struct State {
        u64 epoch = 0;
        u64 last = 0;
        std::map<std::string, Member> members;
        std::map<u32, Standing> standing;
        WorldLogShown shown;
        std::map<std::string, FlagSender> flag_senders;
        FlagEventOnce::Seen flag_events;
        WorldLogMapState map_state;
        std::map<std::string, HpSender> hp_senders;
        WorldLogHp hp;
        std::vector<WorldLogEntry> recent_flags;
        bool started_after_wipe = false;
        WorldLogReturnPoint return_point;
        bool operator==(const State&) const = default;
    };
    State Save() const;
    void Restore(State state);

private:
    WorldLogEntry Append(WorldLogKind kind, u32 map, u32 subject, u32 value = 0);
    void Own(const std::string& member, u32 map, u32 handle, std::vector<WorldLogEntry>& entries);
    void Disown(const std::string& member, u32 handle, std::vector<WorldLogEntry>& entries);
    void AppendShown(std::vector<WorldLogEntry>& entries);
    void AppendMapState(std::vector<WorldLogEntry>& entries);
    void DropHpSource(u64 source, std::vector<WorldLogEntry>& entries);

    u64 epoch = 0;
    u64 last = 0;
    std::map<std::string, Member> members;
    std::map<u32, Standing> standing; // handle -> its map and the members whose kill it is
    WorldLogShown shown;
    std::map<std::string, FlagSender> flag_senders;
    FlagEventOnce flag_events;
    WorldLogMapState map_state;
    std::map<std::string, HpSender> hp_senders;
    WorldLogHp hp;
    std::vector<WorldLogEntry> recent_flags; // the last WorldLogRecentFlags flag entries
    bool started_after_wipe = false;         // Start's `after_wipe`, for the snapshots
    WorldLogReturnPoint return_point;
};

// A member's side: the entries applied in seq order, exactly once.
class WorldLogMember {
public:
    struct Step {
        std::vector<WorldLogEntry> applied;
        bool new_epoch = false; // the host's log started over (everything before is gone)
        bool snapshot = false;  // a whole snapshot was applied
        bool stale = false;     // an older log's frame: dropped
        bool gap = false;       // entries are missing (a snapshot is due)
        // A snapshot based this member on a log that started at a wipe (see WorldLogWipeHold).
        bool after_wipe = false;
    };
    Step Take(const WorldLogBatch& batch, s64 now_ms);
    Step Take(const WorldLogSnapshot& part, s64 now_ms);
    Step Take(const WorldLogShownSnapshot& part, s64 now_ms);
    // A snapshot is whole with its kills, shown characters and map state parts (one build: the host
    // sends all of them).
    Step Take(const WorldLogMapStateSnapshot& part, s64 now_ms);
    Step Take(const WorldLogHpSnapshot& part, s64 now_ms);
    // The last part: a snapshot is whole with it. Its events after what this member had applied
    // (in the same log) come out in Step::applied, the flag events it missed.
    Step Take(const WorldLogFlagsSnapshot& part, s64 now_ms);
    // Every member's HP report (this member's own too), as the host has them.
    const WorldLogHp& Hp() const {
        return hp;
    }
    const WorldLogMapState& MapState() const {
        return map_state;
    }
    // Whether MapState is the host's (as ShownKnown).
    bool MapStateKnown() const {
        return map_state_known;
    }
    // The host's return point (point 0: not known yet).
    const WorldLogReturnPoint& HostReturnPoint() const {
        return return_point;
    }
    // Whether to ask the host for a snapshot now (a gap, a late join, a new epoch past its start),
    // at most every WorldLogSnapshotRetryMs; true marks it asked.
    bool SnapshotDue(s64 now_ms);
    // In step with the host (no gap) and heard from within WorldLogLifeMs.
    bool Fresh(s64 now_ms) const;
    // This member left the session or became another character: the log is forgotten.
    void Reset();
    u64 Epoch() const {
        return epoch;
    }
    u64 Applied() const {
        return applied;
    }
    const std::string& Host() const {
        return host;
    }
    const WorldLogKills& Kills() const {
        return kills;
    }
    const WorldLogShown& Shown() const {
        return shown;
    }
    // Whether Shown is the host's: the log seen from its start or from a ShownMap, or the last
    // snapshot had the shown characters too.
    bool ShownKnown() const {
        return shown_known;
    }

private:
    bool Follow(u64 next_epoch, std::string_view next_host, s64 now_ms, Step& step);
    void Drain(Step& step);
    void Apply(const WorldLogEntry& entry);
    bool Behind() const;
    struct PartialSnapshot;
    PartialSnapshot& Pending(u64 last);
    void Complete(Step& step);

    u64 epoch = 0;
    u64 applied = 0;
    u64 host_last = 0;
    bool based = false; // has the log from its start or a snapshot
    std::string host;
    std::map<u64, WorldLogEntry> waiting; // past a gap
    std::optional<s64> heard_ms;
    std::optional<s64> asked_ms;
    struct PartialSnapshot {
        u64 last = 0;
        std::optional<u8> parts;
        std::map<u8, WorldLogKills> got;
        std::optional<u8> shown_parts;
        std::map<u8, WorldLogShown> shown_got;
        std::optional<WorldLogMapState> map_state;
        WorldLogReturnPoint return_point; // with map_state
        std::optional<u8> hp_parts;
        std::map<u8, WorldLogHp> hp_got;
        std::optional<std::vector<WorldLogEntry>> flags;
        bool after_wipe = false; // the flags part's WorldLogFlagsSnapshot::after_wipe
    };
    void ApplyMapFlag(const WorldLogEntry& entry);
    void ApplyHp(const WorldLogEntry& entry);
    std::optional<PartialSnapshot> partial;
    WorldLogKills kills;
    WorldLogShown shown;
    bool shown_known = false;
    WorldLogMapState map_state;
    bool map_state_known = false;
    WorldLogReturnPoint return_point;
    WorldLogHp hp;
    u64 hp_source = 0; // the source the HP entries are of (HpSource)
};

// Whether the kills, the shown characters and the shared flags go through the world log: while
// this game has it on and is linked to the room's peers; the boards otherwise (alone, or with the
// log switched off). Every peer runs this build. A moment without peers (a link that drops, a load)
// keeps the log WorldLogUnsureMs, then the boards.
constexpr s64 WorldLogUnsureMs = 5'000;
// SHADPS4_BLOODBORNE_WORLD_LOG: on unless "0" or "off" (everything keeps to the boards).
bool WorldLogEnabled(const char* setting);
class WorldLogPath {
public:
    // Whether the log is the path now, with `peers` room peers linked.
    bool Decide(bool enabled, size_t peers, s64 now_ms);
    bool Active() const {
        return active;
    }
    const std::string& Why() const {
        return why;
    }
    // The line to log at the first decision and when the path or its reason changed since the
    // last call ("Co-op world log: ...").
    std::optional<std::string> TakeChange();

private:
    bool decided = false;
    bool active = false;
    std::string why = "not decided";
    std::optional<s64> unsure_since;
    bool changed = false;
};
// A host's Start on a trace record: the epoch (u64) and after_wipe (u8).
std::vector<u8> EncodeWorldLogStart(u64 epoch, bool after_wipe);
// A Reset entry's subject when the host's log started over after a party wipe (older members read
// a Reset as before).
constexpr u32 WorldLogResetAfterWipe = 1;
// Whether `applied` holds the Reset of a log started after a wipe.
bool StartsAfterAWipe(std::span<const WorldLogEntry> applied);
// A member's proposals around a party wipe: the host's new log has no kills, and a member's set
// from before its own wipe (still standing until its kill step forgets it, or a kill applied from
// a stale copy in between) must not stand them again. From the host's wiped log on, a member
// proposes nothing until its own wipe has run (or had run within WorldLogWipeGraceMs before), or
// WorldLogWipeGraceMs went by (it was elsewhere: no wipe for it); and for WorldLogWipeGraceMs after
// either wipe the kill board's host entries (posted before it) are not taken.
constexpr s64 WorldLogWipeGraceMs = 15'000;
class WorldLogWipeHold {
public:
    void HostWiped(s64 now_ms) {
        host_wiped_ms = now_ms;
    }
    void OwnWipe(s64 now_ms) {
        own_wipe_ms = now_ms;
    }
    bool HoldsProposals(s64 now_ms) const;
    bool BoardStale(s64 now_ms) const;

private:
    std::optional<s64> host_wiped_ms;
    std::optional<s64> own_wipe_ms;
};
// Whether this member's enemy HP reports wait as its kill proposals do (WorldLogWipeHold): a
// guest's to the host's log, and every member's to the server's world, the session's host too.
// Kaniel's play test 2026-10-19: the host's game ran the Blood-starved Beast when the party wiped;
// it heard the server's new log before its own wipe ran and sent the boss's HP from before the
// wipe into it, and after the wipe's trip every game took that HP: the boss came back hurt.
bool HoldsHpReports(const WorldLogWipeHold& hold, bool server_world, bool guest, s64 now_ms);
std::optional<std::pair<u64, bool>> DecodeWorldLogStart(std::span<const u8> payload);
// A host's Show on a trace record: the map (u32) and the characters (EncodeShownCharacters).
std::vector<u8> EncodeWorldLogShow(u32 map, std::span<const CharacterShown> characters);
// A host's MapState on a trace record: the map (u32) and the bits.
std::vector<u8> EncodeWorldLogMapStateInput(u32 map, std::span<const u8> bits);
std::optional<WorldLogMapState> DecodeWorldLogMapStateInput(std::span<const u8> payload);
// A host's ReturnPoint on a trace record: the point (s32) and its map (u32).
std::vector<u8> EncodeWorldLogReturnPointInput(const WorldLogReturnPoint& point);
std::optional<WorldLogReturnPoint> DecodeWorldLogReturnPointInput(std::span<const u8> payload);
std::optional<std::pair<u32, std::vector<CharacterShown>>> DecodeWorldLogShow(
    std::span<const u8> payload);
// One report per source from copies of it (direct and the board): per enemy the most damage, or
// for HP from one world (HostHpReport) the least HP; in the order the sources first come.
std::vector<EnemyHpReport> MergeEnemyHpReports(std::span<const EnemyHpReport> reports);

std::vector<u8> EncodeEnemyHp(u64 visit, std::span<const EnemyHp> enemies);
std::optional<EnemyHpReport> DecodeEnemyHp(std::span<const u8> bytes);

std::vector<u8> EncodeShownCharacters(std::span<const CharacterShown> characters);
std::optional<std::vector<CharacterShown>> DecodeShownCharacters(std::span<const u8> bytes);

} // namespace BloodborneCoop
