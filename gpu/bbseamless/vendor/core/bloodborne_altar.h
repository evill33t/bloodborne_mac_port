// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// Team altars (Kaniel 2026-10-17): a ritual at one member's altar puts its dungeon on every
// member's. Ritual altar k (0-6) keeps its dungeon in event flags 94k00000-94k05999, a record of
// 750 bytes packed into the flags (flag = base + byte * 8 + bit, the first flag a byte's top bit,
// as EventFlagBits reads it); 92900000-92905999 is "the current altar", the game's own view of
// the altar the player talks to, and of the dungeon a chalice map loads. The record's first word
// names its dungeon: 0x1D, depth * 10 + area (the key), the variant and the layout, which make
// the map files m29_<key>_<variant>_00/m29_<key>_<variant>_<layout> (the story Pthumeru Chalice:
// 1D 0A 5A 00, m29_10_90_00); its second word is the server's ChannelId (big endian), 2000000000 +
// the key for a standard chalice's dungeon. An altar with no dungeon starts FF FF FF FF.
//
// Kaniel's playtest 2026-10-24 (Kaniel, krolebord, Lisara crashed entering a chalice dungeon): the
// team shared DIFFS of the current altar's view, mapped onto an altar, and each load into a
// dungeon changed the view back to an older record; the receivers set just those bits, so altar 6
// became the good record OR'd with a stale one (1D 0F FF FF: m29_15_i5_00, no such files) and
// the game faulted loading it. Now a record goes whole, only from a ritual or a join in the
// Hunter's Dream, only when it names a dungeon in the game's files, and a broken one is repaired as
// soon as it is seen (each second, and at every load: the game names a dungeon's files as its
// load starts).

#pragma once

#include <array>
#include <optional>
#include <span>
#include <utility>
#include <vector>

#include "common/types.h"

namespace Core::Bloodborne {

inline constexpr s32 RitualAltarCount = 7;
inline constexpr s32 AltarRecordFlags = 6000;
inline constexpr size_t AltarRecordBytes = AltarRecordFlags / 8;
inline constexpr s32 CurrentAltarView = 92900000;
using AltarRecord = std::array<u8, AltarRecordBytes>;

// Altar k's first flag (94k00000).
constexpr s32 AltarFlagBase(s32 altar) {
    return 94000000 + altar * 100000;
}
// The altar whose record `flag` is in (94k00000-94k05999), if any.
std::optional<s32> AltarOfFlag(s32 flag);

// The dungeon a record's first word names (1D, key, variant, layout); none without the 0x1D.
struct AltarDungeon {
    u8 key = 0;
    u8 variant = 0;
    u8 layout = 0;
    bool operator==(const AltarDungeon&) const = default;
};
std::optional<AltarDungeon> AltarRecordDungeon(std::span<const u8> record);
// Whether the game's files have that dungeon's map (dvdroot_ps4/map/mapstudio/m29_*, the chalice
// keys 10-53 only).
bool ChaliceDungeonExists(const AltarDungeon& dungeon);

enum class AltarRecordKind : u8 {
    Empty,   // no dungeon (FF FF FF FF)
    Dungeon, // a dungeon of the game's files
    Broken,  // anything else: loading it crashes the game
};
AltarRecordKind ClassifyAltarRecord(std::span<const u8> record);

// The standard chalice key (10, 20, 21, 30, 31, 40, 42, 50, 52, 53) a record's ChannelId names
// (2000000000 + key, the server's own dungeon for that chalice), if it does.
std::optional<u8> StandardChannelKey(std::span<const u8> record);

// Every flag of altar `altar`'s record as `record` has it, on and off, in flag order.
std::vector<std::pair<s32, bool>> AltarRecordChanges(s32 altar, const AltarRecord& record);

// Whether a change of the current altar's view is a ritual's or a join's new record, to go to
// the team whole: the player stands in the Hunter's Dream, playing, settled since its load, and
// this game did not write an altar record from the team just before (its own altar's reload
// rewrites the view).
inline constexpr u32 HuntersDreamMap = 0x15000000;
inline constexpr s64 AltarShareSettleMs = 10'000;
inline constexpr s64 AltarOwnWriteQuietMs = 20'000;
struct AltarShareFrame {
    bool playing = false;   // the place is Playing (not loading, not a cutscene)
    u32 map = 0;            // the place's map
    s64 since_place_ms = 0; // how long the place has held
    s64 since_own_write_ms = 0;
};
bool SharesAltarRecord(const AltarShareFrame& frame);

// What a teammate's record for one of this game's altars (`mine` as it is) does (Kaniel
// 2026-10-30: a chalice taken off an altar stayed on the teammates' altars, and the same chalice
// placed again changed nothing for them, their altar had that record already): a dungeon of the
// game's files goes in, an empty one (the chalice taken off) clears the altar, the same record
// changes nothing, anything else is left out.
enum class TeamAltarTake : u8 { LeftOut, Same, Take, Clear };
TeamAltarTake TakeTeamAltarRecord(std::span<const u8> mine, std::span<const u8> news);

// The altars this player took a chalice off: a record that named a dungeon is empty, seen while
// the player stands in the Hunter's Dream (SharesAltarRecord: not a load's, not this game's own
// write of a teammate's record). The game's flags are read each second.
class AltarRemovals {
public:
    // Each altar's record as read now (none when not readable).
    using Kinds = std::array<std::optional<AltarRecordKind>, RitualAltarCount>;
    // The altars emptied since the last look, when `own` (the frame shares); either way the look
    // is kept.
    std::vector<s32> Observe(const Kinds& kinds, bool own);
    // Another session or character.
    void Reset();

private:
    Kinds seen{};
};

// What a loaded save's altar record needs (Kaniel 2026-10-24: repair the altars the merge broke).
// A broken record of a standard chalice's dungeon is made again whole; any other broken one is
// cleared (no dungeon, as the other altars), and the chalice it names (its goods, 6100-6599) goes
// back to the player if not held (the ritual took it). A standard chalice's story dungeon with
// other feature slots than the chalice's own (the server's before 2026-10-25 gave root features,
// so its last bosses never gave the next chalice: Kaniel's playtest 2026-10-25) gets the
// chalice's slots and feature flags, all else kept (Features).
struct AltarRepair {
    enum class Action : u8 { None, Rebuild, Clear, Features };
    Action action = Action::None;
    AltarRecord record{};
    std::optional<u32> chalice; // the goods to give back (Clear)
};
AltarRepair RepairAltarRecord(std::span<const u8> record);

// The chalice a record names (FormData's goods, byte 16; 6100-6599): the ritual took it from the
// player, so it goes back when the dungeon ends without that player taking it off.
std::optional<u32> AltarRecordChalice(std::span<const u8> record);

// The record of the story chalice dungeon of standard key `key` as a ritual writes it, its state
// (flags 5000-5999) taken from `state` when that is a whole record, and an altar's with no
// dungeon.
std::optional<AltarRecord> StandardAltarRecord(u8 key, std::span<const u8> state = {});
AltarRecord EmptyAltarRecord();

// The current altar's pick (Kaniel's playtest 2026-10-28: krolebord's game built altar 6's old
// dungeon inside the party's altar 5 one, an open door and no boss room). Flags 9020-9026 are
// "altar k is the current one" (9020 + k, set by the altar's talk and the Dream's warp event
// 12107100); the game reads them as one 7-flag value (9020 its top bit) as a load registers a
// chalice map's flag blocks, and points the map's view blocks (12900-12909, 52900-52909,
// 72900-72909, 92900-92909) at that altar's. A party trip's load reads the save as written on
// disk, which can name an altar picked before.
inline constexpr s32 AltarPickFlag = 9020;
inline constexpr u32 AltarPickFlagCount = 7;
// The altar a 7-flag value names (exactly one flag on), if any.
std::optional<s32> AltarOfPickBits(u32 bits);
// The 7-flag value naming `altar`.
constexpr u32 AltarPickBits(s32 altar) {
    return 1u << (AltarPickFlagCount - 1 - static_cast<u32>(altar));
}
// The altar the pick flags among `on` (sorted, a team world's flags on) name, if one.
std::optional<s32> AltarPickedIn(std::span<const s32> on);
// The altar a ritual altar's "warp to the layer" request (72100420 + altar) names, if any.
std::optional<s32> AltarOfWarpRequest(s32 flag);

// The altar whose dungeon this game's next chalice map load should build: the one picked last,
// by this player's own talk (its warp request, or a change of the game's flags while playing, not
// a load's), by another member's warp into a dungeon (the trip this game follows goes there), or
// by the team host's altar changing (as a guest joins, the host's dungeon).
class ChaliceAltarPick {
public:
    // This game's own pick flags as read now (none when not readable).
    void ObserveOwn(std::optional<s32> altar);
    // The team world's (its host's) pick flags as they came now.
    void ObserveTeam(std::optional<s32> altar);
    // A warp request into altar `altar`'s dungeon: this player's own talk's, or another
    // member's (the trip this game follows goes there).
    void Warps(s32 altar);
    // A load began: its save's flags are no pick of this player's.
    void Loaded();
    // Another session or character.
    void Reset();
    // The altar a load whose save names `saved` should build instead, if another.
    std::optional<s32> ForLoad(std::optional<s32> saved) const;

private:
    std::optional<s32> picked;
    bool own_seen = false;
    std::optional<s32> own;
    std::optional<s32> team;
};

} // namespace Core::Bloodborne
