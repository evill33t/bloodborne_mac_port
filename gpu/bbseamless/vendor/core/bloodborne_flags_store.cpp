// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// The flag store and progression: event flags, guest progress, pickups, world changes, trophies.
// Split from bloodborne_re.cpp; the shared declarations are in bloodborne_re_internal.h.

#include "core/bloodborne_altar.h"
#include "core/bloodborne_crash_dump.h"
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
#include <iterator>
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
#include "core/libraries/network/sockets.h"
#include "core/libraries/np/np_handler.h"
#include "core/libraries/np/np_matching2/np_matching2_signaling.h"
#include "core/libraries/np/np_trophy.h"
#include "core/libraries/system/userservice.h"
#include "core/memory.h"

namespace Core::Bloodborne {
namespace Coop {

// Auto-rejoin for a configured party (SHADPS4_BLOODBORNE_PARTY + SHADPS4_BLOODBORNE_PARTY_ROLE).
// Only ticked from the SOS-status hook on the game thread.
std::optional<PartyController> party_controller;

// Shared trophies: what the game unlocked itself while in a party session (posted to the
// party's trophy board), every id this member has dealt with, and the other members' unlocks
// waiting for the game thread to apply.
std::atomic<bool> trophy_board_active{};
std::mutex trophy_mutex;
std::vector<s32> trophies_earned_in_party;

} // namespace Coop

namespace {

std::vector<s32> trophies_handled;
std::vector<s32> trophies_to_unlock;

} // namespace

namespace Coop {

void NoteGameTrophyUnlock(s32 trophy) {
    if (!trophy_board_active.load()) {
        return;
    }
    std::scoped_lock lock{trophy_mutex};
    if (std::ranges::find(trophies_earned_in_party, trophy) == trophies_earned_in_party.end()) {
        trophies_earned_in_party.push_back(trophy);
        trophies_handled.push_back(trophy);
    }
    LOG_INFO(Debug, "Bloodborne party: sharing trophy {} with the party", trophy);
}

void ReadTrophyBoard(std::span<const s32> board) {
    std::scoped_lock lock{trophy_mutex};
    for (const s32 trophy : TrophiesToUnlock(board, trophies_handled)) {
        trophies_handled.push_back(trophy);
        trophies_to_unlock.push_back(trophy);
    }
}

// On the game thread: unlocks the other members' trophies.
void UnlockPartyTrophies(bool sharing) {
    trophy_board_active.store(sharing);
    std::vector<s32> pending;
    {
        std::scoped_lock lock{trophy_mutex};
        pending.swap(trophies_to_unlock);
    }
    for (const s32 trophy : pending) {
        const int result = Libraries::Np::NpTrophy::UnlockTrophyFromParty(trophy);
        LOG_INFO(Debug, "Bloodborne party: unlocked the party's trophy {} result={:#x}", trophy,
                 static_cast<u32>(result));
    }
}

} // namespace Coop

namespace {

// Lamps shared by the party (see FlagsToShare, EncodeFlagUpdate): every member posts the lamps
// it saw lit to the party's flag board and sets the other members' ones.
std::atomic<bool> party_host_role{};

} // namespace

namespace Coop {

std::vector<std::pair<s32, bool>> flags_to_set;

} // namespace Coop

namespace {

// A guest's progress in the host's world (see IsMapPickupFlag): the event flags it set there are
// kept in a file and set again once it has been home for a while, so its next save has them.
// EventFlagMan [0x0553B100]; 0x013CFD80(manager, flag, bits) reads a flag (the
// GetEventFlagValue binding's call).
constexpr u64 EventFlagManPointerOffset = 0x0553B100;
constexpr u64 ReadEventFlagBitsOffset = 0x013CFD80;
constexpr auto GuestProgressInterval = std::chrono::seconds{2};
constexpr auto GuestProgressHomeDelay = std::chrono::seconds{10};

} // namespace

namespace Coop {

bool EventFlagOn(s32 flag) {
    return (EventFlagBits(flag, 1).value_or(0) & 1) != 0;
}

std::optional<u32> EventFlagBits(s32 flag, u32 bits) {
    const u64 manager = ReadValue<u64>(image_base + EventFlagManPointerOffset, 0);
    if (!HasVtable(manager, EventFlagManVtable, "EventFlagOn")) {
        return std::nullopt;
    }
    using ReadBits = u32 PS4_SYSV_ABI (*)(u64 manager, s32 flag, u32 bits);
    return reinterpret_cast<ReadBits>(image_base + ReadEventFlagBitsOffset)(manager, flag, bits);
}

// The ritual altars' objects ([[0x0553E9E8]+8], one per altar, 7), each with its dungeon's
// state at +0x108; 0x01AA40E0 on that state asks it to load again from the altar's flags (state
// 2), as at a save's load. A teammate's ritual put a new record in the flags (team altars).
constexpr u64 RitualAltarsPointerOffset = 0x0553E9E8;
constexpr u64 ReloadRitualAltarOffset = 0x01AA40E0;
constexpr std::array<u8, 7> ReloadRitualAltarBytes{0x55, 0x48, 0x89, 0xE5, 0x41, 0x56, 0x53};
// 0x01AA54D0 on that state, for a status 2-5: its dungeon goes (request 7), to status 1.
constexpr u64 RemoveRitualAltarOffset = 0x01AA54D0;

void ReloadRitualAltar(s32 altar) {
    if (altar < 0 || altar >= 7) {
        return;
    }
    const u64 altars = ReadValue<u64>(image_base + RitualAltarsPointerOffset, 0);
    const u64 table = altars >= 0x10000 ? ReadValue<u64>(altars, 8) : 0;
    const u64 object = table >= 0x10000 ? ReadValue<u64>(table, altar * 8) : 0;
    const u64 state = object >= 0x10000 ? object + 0x108 : 0;
    if (state == 0 || !HasMemoryAccess(state, 0x270, MemoryProt::CpuRead) ||
        std::memcmp(reinterpret_cast<const void*>(image_base + ReloadRitualAltarOffset),
                    ReloadRitualAltarBytes.data(), ReloadRitualAltarBytes.size()) != 0) {
        LOG_WARNING(Debug,
                    "Bloodborne party: ritual altar {} not found; it shows the team's "
                    "dungeon after the next load",
                    altar);
        return;
    }
    // Status +0x25C: 0 not loaded yet, 1 no dungeon, 5 a dungeon; the load starts from 0 only.
    const s32 status = ReadValue<s32>(state, 0x25C);
    if (status != 1 && status != 0) {
        LOG_INFO(Debug, "Bloodborne party: ritual altar {} already has a dungeon (status {})",
                 altar, status);
        return;
    }
    WriteValue(state, 0x25C, s32{0});
    using Reload = bool PS4_SYSV_ABI (*)(u64 state);
    const bool asked = reinterpret_cast<Reload>(image_base + ReloadRitualAltarOffset)(state);
    LOG_INFO(Debug, "Bloodborne party: ritual altar {} loaded the team's record ({}, status {})",
             altar, asked ? "done" : "busy", ReadValue<s32>(state, 0x25C));
}

namespace {

// Altar `altar`'s state, 0 when its object is not there.
u64 RitualAltarState(s32 altar) {
    const u64 altars = ReadValue<u64>(image_base + RitualAltarsPointerOffset, 0);
    const u64 table = altars >= 0x10000 ? ReadValue<u64>(altars, 8) : 0;
    const u64 object =
        table >= 0x10000 && altar >= 0 && altar < 7 ? ReadValue<u64>(table, altar * 8) : 0;
    const u64 state = object >= 0x10000 ? object + 0x108 : 0;
    return state != 0 && HasMemoryAccess(state, 0x270, MemoryProt::CpuRead) ? state : 0;
}

} // namespace

std::optional<s32> RitualAltarStatus(s32 altar) {
    const u64 state = RitualAltarState(altar);
    if (state == 0) {
        return std::nullopt;
    }
    return ReadValue<s32>(state, 0x25C);
}

// A teammate took the chalice off altar `altar` and the flags here are empty now, but the altar's
// object still holds the dungeon it loaded (status 5): its menu would warp into a dungeon that is
// gone and the load never ends (rig 2026-10-30), so the object lets it go too.
void EmptyRitualAltar(s32 altar) {
    const u64 state = RitualAltarState(altar);
    if (state == 0) {
        LOG_WARNING(Debug,
                    "Bloodborne party: ritual altar {} not found; it shows no chalice "
                    "after the next load",
                    altar);
        return;
    }
    const s32 status = ReadValue<s32>(state, 0x25C);
    if (status < 2 || status > 5) {
        return;
    }
    if (std::memcmp(reinterpret_cast<const void*>(image_base + RemoveRitualAltarOffset),
                    ReloadRitualAltarBytes.data(), ReloadRitualAltarBytes.size()) != 0) {
        LOG_WARNING(Debug, "Bloodborne party: ritual altar {} keeps its dungeon: code differs",
                    altar);
        return;
    }
    using Remove = bool PS4_SYSV_ABI (*)(u64 state);
    const bool asked = reinterpret_cast<Remove>(image_base + RemoveRitualAltarOffset)(state);
    LOG_INFO(Debug, "Bloodborne party: ritual altar {} emptied ({}, status {} now {})", altar,
             asked ? "done" : "busy", status, ReadValue<s32>(state, 0x25C));
}

// Kaniel 2026-10-18: a teammate who holds no chalice is given the chalice of the dungeon that came
// to its altar, so the altar's talk (which opens only for a player holding one) opens as the
// conductor's does. Its record's second byte (94k00008) is the dungeon's depth * 10 + area.
void GiveAltarChalice(s32 altar) {
    const s32 base = 94000000 + altar * 100000;
    const auto depth_area = EventFlagBits(base + 8, 8);
    const auto goods = depth_area ? AltarChaliceGoods(*depth_area & 0xFF) : std::nullopt;
    if (!goods || !EventFlagOn(base + 5001) || !PlayerItemsReadable() || !TradeItemsCanBeAdded()) {
        return;
    }
    for (const HeldGoods& held : HeldKeyGoods()) {
        if (held.goods >= 6100 && held.goods <= 6599 && held.count > 0) {
            return; // holds a chalice already
        }
    }
    const TradeItem chalice{BloodborneCoop::TradeItemKind::Goods, *goods, 1};
    if (TradeHeld(chalice) > 0) {
        return;
    }
    AddTradeItems(std::span{&chalice, 1});
    LOG_INFO(Debug,
             "Bloodborne team altar: altar {} has the team's dungeon; given its chalice "
             "(goods {}) to open it",
             altar, *goods);
}

namespace {

// When this game last wrote an altar record itself (a teammate's, a repair): the view its altar's
// reload rewrites is no ritual of this player's (SharesAltarRecord).
std::atomic<s64> altar_written_ms{-1'000'000'000};
// Chalices a cleared altar's record named, to give back once the player's items read.
std::mutex altar_chalices_mutex;
std::vector<u32> altar_chalices_owed;

std::optional<AltarRecord> ReadAltarFlags(s32 base) {
    AltarRecord record{};
    for (size_t byte = 0; byte < record.size(); ++byte) {
        const auto bits = EventFlagBits(base + static_cast<s32>(byte * 8), 8);
        if (!bits) {
            return std::nullopt;
        }
        record[byte] = static_cast<u8>(*bits);
    }
    return record;
}

void WriteAltarFlags(s32 base, const AltarRecord& record) {
    using SetFlag = void PS4_SYSV_ABI (*)(u64, s32, bool);
    altar_written_ms.store(SteadyMilliseconds());
    for (size_t byte = 0; byte < record.size(); ++byte) {
        for (u32 bit = 0; bit < 8; ++bit) {
            reinterpret_cast<SetFlag>(image_base + PartyGhostSetEventFlagOffset)(
                0, base + static_cast<s32>(byte * 8 + bit), (record[byte] & (0x80 >> bit)) != 0);
        }
    }
    altar_written_ms.store(SteadyMilliseconds());
}

std::string RecordName(const AltarRecord& record) {
    return fmt::format(
        "{:02X} {:02X} {:02X} {:02X}, channel {}", record[0], record[1], record[2], record[3],
        u32{record[4]} << 24 | u32{record[5]} << 16 | u32{record[6]} << 8 | record[7]);
}

void GiveBackAltarChalices() {
    std::vector<u32> owed;
    {
        std::scoped_lock lock{altar_chalices_mutex};
        if (altar_chalices_owed.empty() || !PlayerItemsReadable() || !TradeItemsCanBeAdded()) {
            return;
        }
        owed.swap(altar_chalices_owed);
    }
    for (const u32 goods : owed) {
        const TradeItem chalice{BloodborneCoop::TradeItemKind::Goods, goods, 1};
        if (TradeHeld(chalice) > 0) {
            LOG_INFO(Debug, "Bloodborne team altar: chalice {} of a cleared altar is held already",
                     goods);
            continue;
        }
        AddTradeItems(std::span{&chalice, 1});
        LOG_INFO(Debug, "Bloodborne team altar: chalice {} of a cleared altar given back", goods);
    }
}

// The chalice map's altar (bloodborne_altar.h, ChaliceAltarPick): the pick, kept from the
// game's flags each second and other members' warps, decides the altar a chalice map's load
// builds (PickChaliceAltarAtLoad).
std::mutex chalice_altar_mutex;
ChaliceAltarPick chalice_altar_pick;

void ObserveChaliceAltarPick(s64 now_ms) {
    const Place place = CurrentPlace();
    // A load's own flags (its save's, then the team world's) are no pick: seen first once it
    // settled.
    constexpr s64 LoadSettleMs = 5'000;
    const bool settled =
        place.state == PlaceState::Playing && now_ms - place.since_ms >= LoadSettleMs;
    const auto own = settled ? EventFlagBits(AltarPickFlag, AltarPickFlagCount) : std::nullopt;
    const std::vector<s32> team = TeamWorldFlagsOnForGuest();
    std::scoped_lock lock{chalice_altar_mutex};
    if (!settled) {
        chalice_altar_pick.Loaded();
    } else if (own) {
        chalice_altar_pick.ObserveOwn(AltarOfPickBits(*own));
    }
    chalice_altar_pick.ObserveTeam(AltarPickedIn(team));
}

[[maybe_unused]] const bool chalice_altar_reset_registered =
    RegisterLifecycleReset("chalice altar pick", {Lifecycle::NewCharacter}, [] {
        std::scoped_lock lock{chalice_altar_mutex};
        chalice_altar_pick.Reset();
    });

// The altars this player takes a chalice off (Kaniel 2026-10-30: the chalice stayed on the
// teammates' altars), seen each second (KeepRitualAltarsWhole) on the game thread.
AltarRemovals altar_removals;

[[maybe_unused]] const bool altar_removals_reset_registered =
    RegisterLifecycleReset("altar removals", {Lifecycle::NewCharacter}, [] {
        std::scoped_lock lock{chalice_altar_mutex};
        altar_removals.Reset();
    });

// Altar k's dungeon as the game keeps it while the altar is not the current one (the chalice
// map's view 1290xxxx and its items 5290xxxx, + (11 + k) * 100000: 14k0xxxx, 54k0xxxx). A chalice
// taken off ends the dungeon: its lamps lit (Shared, the team world relit them), bosses and
// items go, so the next chalice placed there starts it new on every game.
constexpr s32 ChaliceDungeonFlags = 10'000;
size_t ClearAltarDungeon(s32 altar) {
    using SetFlag = void PS4_SYSV_ABI (*)(u64, s32, bool);
    size_t cleared = 0;
    for (const s32 view : {12900000, 52900000}) {
        const s32 base = view + (11 + altar) * 100000;
        for (s32 byte = 0; byte < ChaliceDungeonFlags / 8; ++byte) {
            const auto bits = EventFlagBits(base + byte * 8, 8);
            if (!bits || (*bits & 0xFF) == 0) {
                continue;
            }
            for (u32 bit = 0; bit < 8; ++bit) {
                if ((*bits & (0x80u >> bit)) != 0) {
                    reinterpret_cast<SetFlag>(image_base + PartyGhostSetEventFlagOffset)(
                        0, base + byte * 8 + static_cast<s32>(bit), false);
                    ++cleared;
                }
            }
        }
    }
    return cleared;
}

// This player took the chalice off altar `altar`: the altar goes to the team empty, and its
// dungeon ends here too.
void ShareAltarRemovals(s64 now_ms) {
    if (std::getenv("BB_RIG_NO_ALTAR_WATCH") != nullptr) {
        return;
    }
    const Place place = CurrentPlace();
    const AltarShareFrame frame{.playing = place.state == PlaceState::Playing,
                                .map = place.map,
                                .since_place_ms = now_ms - place.since_ms,
                                .since_own_write_ms = now_ms - altar_written_ms.load()};
    AltarRemovals::Kinds kinds{};
    for (s32 altar = 0; altar < RitualAltarCount; ++altar) {
        std::array<u8, 8> head{};
        bool readable = true;
        for (size_t byte = 0; byte < head.size() && readable; ++byte) {
            const auto bits = EventFlagBits(AltarFlagBase(altar) + static_cast<s32>(byte * 8), 8);
            readable = bits.has_value();
            head[byte] = static_cast<u8>(bits.value_or(0));
        }
        if (readable) {
            kinds[altar] = ClassifyAltarRecord(head);
        }
    }
    std::vector<s32> emptied;
    {
        std::scoped_lock lock{chalice_altar_mutex};
        emptied = altar_removals.Observe(kinds, SharesAltarRecord(frame));
    }
    for (const s32 altar : emptied) {
        const auto record = ReadAltarFlags(AltarFlagBase(altar));
        if (!record || ClassifyAltarRecord(*record) != AltarRecordKind::Empty) {
            continue;
        }
        SendAltarRecord(altar, *record);
        LOG_INFO(Debug,
                 "Bloodborne team altar: the chalice was taken off altar {}: the altar goes to the "
                 "team empty, its dungeon's {} flags here cleared",
                 altar, ClearAltarDungeon(altar));
    }
}

// The block registration 0x013BEAC0 of a chalice map's view (area 0x1D, block 0) reads the pick
// flags (`call 0x013CFD80` with the manager in r12, 9020, 7) and points the view at altar
// 47 - bit length's blocks; here, right after the read, eax is the value. A party trip's load
// reads the save as written on disk, so this game's later pick goes in instead (Kaniel's playtest
// 2026-10-28: krolebord's trip into altar 5's dungeon built altar 6's, whose 9026 his save still
// had), only for an altar whose record names a dungeon.
constexpr u64 ChaliceViewPickReadOffset = 0x013BEB4D;
constexpr std::array<u8, 8> ChaliceViewPickRead{0x31, 0xC9, 0x85, 0xC0, 0x44, 0x8B, 0x75, 0xD4};

void PS4_SYSV_ABI PickChaliceAltarAtLoad(u64, GuestRegisterSnapshot* registers) {
    if (!InPartySession()) {
        return;
    }
    const u32 bits = static_cast<u32>(registers->rax) & ((1u << AltarPickFlagCount) - 1);
    const std::optional<s32> saved = AltarOfPickBits(bits);
    std::optional<s32> altar;
    {
        std::scoped_lock lock{chalice_altar_mutex};
        altar = chalice_altar_pick.ForLoad(saved);
    }
    if (!altar) {
        return;
    }
    using ReadBits = u32 PS4_SYSV_ABI (*)(u64 manager, s32 flag, u32 bits);
    const auto read = reinterpret_cast<ReadBits>(image_base + ReadEventFlagBitsOffset);
    std::array<u8, 4> head{};
    for (size_t byte = 0; byte < head.size(); ++byte) {
        head[byte] = static_cast<u8>(
            read(registers->r12, AltarFlagBase(*altar) + static_cast<s32>(byte * 8), 8));
    }
    if (!AltarRecordDungeon(head) || !ChaliceDungeonExists(*AltarRecordDungeon(head))) {
        LOG_WARNING(Debug,
                    "Bloodborne chalice map: altar {} was picked last but names no dungeon "
                    "({:02X} {:02X} {:02X} {:02X}): the save's altar {} is built",
                    *altar, head[0], head[1], head[2], head[3], saved.value_or(-1));
        return;
    }
    registers->rax = (registers->rax & ~u64{0xFFFFFFFF}) | AltarPickBits(*altar);
    LOG_INFO(Debug,
             "Bloodborne chalice map: this load builds altar {}'s dungeon ({:02X} {:02X} {:02X} "
             "{:02X}), picked last, not the save's altar {}",
             *altar, head[0], head[1], head[2], head[3],
             saved ? std::to_string(*saved) : std::string{"(none)"});
}

} // namespace

void NoteAltarWarp(s32 altar) {
    std::scoped_lock lock{chalice_altar_mutex};
    chalice_altar_pick.Warps(altar);
}

void InstallChaliceAltarPick(u64 base, u64 image_size) {
    if (!party_controller) {
        return;
    }
    if (ChaliceViewPickReadOffset + ChaliceViewPickRead.size() > image_size ||
        !InstallCoopHook(reinterpret_cast<void*>(base + ChaliceViewPickReadOffset),
                         ChaliceViewPickRead, 0, PickChaliceAltarAtLoad)) {
        coop_features.Skipped("chalice altar pick",
                              SiteFailure(base, image_size, ChaliceViewPickReadOffset));
        return;
    }
    coop_features.On("chalice altar pick");
    LOG_INFO(Debug, "Bloodborne chalice map: a load builds the altar picked last");
}

void RepairRitualAltars() {
    const auto repair = [](s32 base, std::string_view which) {
        const auto record = ReadAltarFlags(base);
        if (!record) {
            return;
        }
        const AltarRepair repair = RepairAltarRecord(*record);
        if (repair.action == AltarRepair::Action::None) {
            return;
        }
        WriteAltarFlags(base, repair.record);
        if (repair.action == AltarRepair::Action::Features) {
            LOG_WARNING(Debug,
                        "Bloodborne team altar: {} ({}) had root features: given its chalice's own",
                        which, RecordName(*record));
            return;
        }
        LOG_WARNING(Debug, "Bloodborne team altar: {} named no dungeon ({}): {} ({})", which,
                    RecordName(*record),
                    repair.action == AltarRepair::Action::Rebuild ? "made again" : "cleared",
                    RecordName(repair.record));
        if (repair.chalice) {
            std::scoped_lock lock{altar_chalices_mutex};
            altar_chalices_owed.push_back(*repair.chalice);
        }
    };
    for (s32 altar = 0; altar < RitualAltarCount; ++altar) {
        repair(AltarFlagBase(altar), fmt::format("altar {}", altar));
    }
    repair(CurrentAltarView, "the current altar");
}

void KeepRitualAltarsWhole() {
    // The rig's chalice-enter-broken: the game names a dungeon's files as its load starts, before
    // any load hook of ours, so a broken record is repaired as soon as it is seen (each record's
    // first two words, every second): a save's in the Dream before its altar is used.
    static s64 checked_ms = 0;
    const s64 now_ms = SteadyMilliseconds();
    if (now_ms - checked_ms < 1'000) {
        return;
    }
    checked_ms = now_ms;
    ObserveChaliceAltarPick(now_ms);
    ShareAltarRemovals(now_ms);
    const auto broken = [](s32 base) {
        std::array<u8, 8> head{};
        for (size_t byte = 0; byte < head.size(); ++byte) {
            const auto bits = EventFlagBits(base + static_cast<s32>(byte * 8), 8);
            if (!bits) {
                return false;
            }
            head[byte] = static_cast<u8>(*bits);
        }
        return ClassifyAltarRecord(head) == AltarRecordKind::Broken;
    };
    bool any = broken(CurrentAltarView);
    for (s32 altar = 0; altar < RitualAltarCount && !any; ++altar) {
        any = broken(AltarFlagBase(altar));
    }
    if (any) {
        RepairRitualAltars();
    }
}

void ShareRitualAltarRecord(size_t changed) {
    const Place place = CurrentPlace();
    const s64 now_ms = SteadyMilliseconds();
    const AltarShareFrame frame{.playing = place.state == PlaceState::Playing,
                                .map = place.map,
                                .since_place_ms = now_ms - place.since_ms,
                                .since_own_write_ms = now_ms - altar_written_ms.load()};
    if (!SharesAltarRecord(frame)) {
        LOG_INFO(Debug,
                 "Bloodborne team altar: the current altar changed ({} flags) on map {:#x} "
                 "({}, {} s there, {} s since this game wrote one): no ritual, kept here",
                 changed, place.map, frame.playing ? "playing" : "not playing",
                 frame.since_place_ms / 1000, frame.since_own_write_ms / 1000);
        return;
    }
    const auto altar = CurrentRitualAltar();
    const auto record = altar ? ReadAltarFlags(AltarFlagBase(*altar)) : std::nullopt;
    if (!record) {
        LOG_INFO(Debug,
                 "Bloodborne team altar: the current altar changed ({} flags); its altar "
                 "is not known",
                 changed);
        return;
    }
    if (ClassifyAltarRecord(*record) != AltarRecordKind::Dungeon) {
        LOG_WARNING(Debug,
                    "Bloodborne team altar: altar {}'s record names no dungeon ({}); not shared",
                    *altar, RecordName(*record));
        return;
    }
    SendAltarRecord(*altar, *record);
    LOG_INFO(Debug, "Bloodborne team altar: altar {}'s record ({}) goes to the team whole", *altar,
             RecordName(*record));
}

std::optional<s32> CurrentRitualAltar() {
    // The current altar's view (92900000) reads the same as its own flags (94k00000); a block
    // the save does not have reads 0xFF.
    const auto header = [](s32 base) -> std::optional<u32> {
        u32 value = 0;
        for (s32 byte = 0; byte < 4; ++byte) {
            const auto bits = EventFlagBits(base + byte * 8, 8);
            if (!bits) {
                return std::nullopt;
            }
            value = (value << 8) | (*bits & 0xFF);
        }
        return value;
    };
    const auto current = header(92900000);
    if (!current || *current == 0xFFFFFFFF) {
        return std::nullopt;
    }
    for (s32 altar = 0; altar < 7; ++altar) {
        const s32 base = 94000000 + altar * 100000;
        if (header(base) == current && EventFlagOn(base + 5001) == EventFlagOn(92905001)) {
            return altar;
        }
    }
    return std::nullopt;
}

} // namespace Coop

namespace {

std::vector<s32> MapPickupFlags() {
    const u64 param = FindLoadedParam("ITEMLOT_PARAM_ST");
    if (param == 0) {
        return {};
    }
    const u16 rows = ReadValue<u16>(param, 0x0A);
    std::vector<s32> flags;
    for (u16 row = 0; row < rows; ++row) {
        const u64 data = ReadValue<u64>(param, 0x40 + u64{row} * 24 + 8);
        const s32 flag = ReadValue<s32>(param + data, 0x80); // getItemFlagId
        if (IsMapPickupFlag(flag) && std::ranges::find(flags, flag) == flags.end()) {
            flags.push_back(flag);
        }
    }
    return flags;
}

} // namespace

namespace Coop {

// The character the kept progress belongs to: the save slot the game opened last (see
// CharacterSlotOfSavePath), -1 until it opened one.
std::atomic<int> character_slot{-1};

} // namespace Coop

namespace {

std::filesystem::path GuestProgressFile() {
    return Common::FS::GetUserPath(Common::FS::PathType::UserDir) /
           SlotFileName("progress", character_slot.load());
}

// Every pickup flag the guest set in a party session, kept for good: its own world gets them
// once it is home, and the host's world gets them each time the guest's map items are created
// there (the join reload brings the host's flags, which don't have the guest's pickups).
std::mutex guest_picked_mutex;
std::optional<std::vector<s32>> guest_picked;
int guest_picked_slot = -1;

std::vector<s32>& GuestPickedFlags() {
    if (!guest_picked || guest_picked_slot != character_slot.load()) {
        guest_picked_slot = character_slot.load();
        std::ifstream file{GuestProgressFile()};
        const std::string text{std::istreambuf_iterator<char>{file}, {}};
        guest_picked = ParseFlagList(text);
    }
    return *guest_picked;
}

void SaveGuestProgress(std::span<const s32> flags) {
    std::ofstream file{GuestProgressFile(), std::ios::trunc};
    file << FormatFlagList(flags);
}

} // namespace

namespace Coop {

// A party guest with a known character (whose kept progress can be read and written).
bool IsPartyGuestRole() {
    return party_controller && party_controller->SearchEffectId() == SmallResonantBellEffect &&
           character_slot.load() >= 0;
}

} // namespace Coop

namespace {

// Sets the guest's remembered pickups that are not set yet; returns how many it set.
size_t SetGuestPickedFlags() {
    std::scoped_lock lock{guest_picked_mutex};
    using SetFlag = void PS4_SYSV_ABI (*)(u64, s32, bool);
    size_t set = 0;
    for (const s32 flag : GuestPickedFlags()) {
        if (!EventFlagOn(flag)) {
            reinterpret_cast<SetFlag>(image_base + PartyGhostSetEventFlagOffset)(0, flag, true);
            set += EventFlagOn(flag) ? 1 : 0;
        }
    }
    return set;
}
void ForgetGuestWorldChanges();

// Pickups confirmed set in the character's own world (see IsNewCharacterInSlot), per slot.
std::filesystem::path GuestConfirmedFile() {
    return Common::FS::GetUserPath(Common::FS::PathType::UserDir) /
           SlotFileName("confirmed", character_slot.load());
}

std::vector<s32> ReadGuestConfirmed() {
    std::ifstream file{GuestConfirmedFile()};
    const std::string text{std::istreambuf_iterator<char>{file}, {}};
    return ParseFlagList(text);
}

// The set event flags of the slot's character's own world when it last loaded (see
// IsOtherCharacter), per slot.
std::filesystem::path GuestOwnerFile() {
    return Common::FS::GetUserPath(Common::FS::PathType::UserDir) /
           fmt::format("bloodborne_party_owner_slot{}.txt", character_slot.load());
}
std::filesystem::path GuestWorldFile();

// The slot's kept lists are copied aside (name.bak-<local time>) before they are dropped, so a
// wrong drop can be undone by hand; the newest 5 copies of each stay.
void BackUpSlotLists() {
    const std::time_t now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    std::tm local{};
#ifdef _WIN32
    localtime_s(&local, &now);
#else
    localtime_r(&now, &local);
#endif
    const std::string stamp =
        fmt::format("{:04}{:02}{:02}-{:02}{:02}{:02}", local.tm_year + 1900, local.tm_mon + 1,
                    local.tm_mday, local.tm_hour, local.tm_min, local.tm_sec);
    for (const std::string_view list : CharacterSlotFiles) {
        const std::filesystem::path file = Common::FS::GetUserPath(Common::FS::PathType::UserDir) /
                                           SlotFileName(list, character_slot.load());
        std::error_code error;
        if (!std::filesystem::exists(file, error)) {
            continue;
        }
        const std::string name = Common::FS::PathToUTF8String(file.filename());
        std::filesystem::copy_file(file, file.parent_path() / SlotBackupName(name, stamp),
                                   std::filesystem::copy_options::overwrite_existing, error);
        if (error) {
            LOG_WARNING(Debug, "Bloodborne party: could not copy {} aside: {}", name,
                        error.message());
            continue;
        }
        std::vector<std::string> names;
        for (const auto& entry : std::filesystem::directory_iterator(file.parent_path(), error)) {
            names.push_back(Common::FS::PathToUTF8String(entry.path().filename()));
        }
        for (const std::string& old : SlotBackupsToRemove(name, names, 5)) {
            std::filesystem::remove(file.parent_path() / old, error);
        }
    }
}

// Before a load of the guest's own world sets its pickups: a slot whose confirmed pickups are
// all missing, or whose world has far fewer event flags set than when it last loaded, holds a
// new character, so the slot's kept lists are dropped (pickups first, as
// SetGuestPickupsBeforeMapItems sets them right after).
void ForgetSlotIfNewCharacter() {
    // Read twice: a load changes the store on another thread, and a count from a partial or torn
    // read decides nothing (a count of 0 read as a new character could wipe the kept pickups).
    const std::optional<s32> settled = SettledFlagCount(CountSetFlags(), CountSetFlags());
    if (!settled) {
        LOG_INFO(Debug,
                 "Bloodborne party: the flag store of save slot {} did not read whole; its kept "
                 "lists stay",
                 character_slot.load());
        return;
    }
    const s32 progress_now = *settled;
    const std::vector<s32> confirmed = ReadGuestConfirmed();
    const size_t set_now = static_cast<size_t>(std::ranges::count_if(confirmed, EventFlagOn));
    s32 progress_kept = 0;
    {
        std::ifstream file{GuestOwnerFile()};
        file >> progress_kept;
    }
    const bool other = IsOtherCharacter(progress_now, progress_kept);
    {
        std::ofstream file{GuestOwnerFile(), std::ios::trunc};
        file << (other ? progress_now : std::max(progress_now, progress_kept)) << '\n';
    }
    if (!ForgetsSlotLists(progress_now, progress_kept, IsNewCharacterInSlot(confirmed, set_now))) {
        return;
    }
    TraceLife("slot-lists-forgotten", {{"slot", std::to_string(character_slot.load())}});
    BackUpSlotLists();
    {
        std::scoped_lock lock{guest_picked_mutex};
        GuestPickedFlags().clear();
        SaveGuestProgress({});
    }
    ForgetGuestWorldChanges();
    std::error_code error;
    std::filesystem::remove(GuestConfirmedFile(), error);
    // And where the old character resumes (read once the load settles, after this).
    std::filesystem::remove(Common::FS::GetUserPath(Common::FS::PathType::UserDir) /
                                SlotFileName("resume", character_slot.load()),
                            error);
    RefuseServerResumePoint(); // the old character's place in its team's world (phase 7)
    LOG_INFO(Debug,
             "Bloodborne party: new character in save slot {} ({} event flags set, {} kept); its "
             "kept lists and resume point start clean (old ones copied aside as .bak files)",
             character_slot.load(), progress_now, progress_kept);
}

// After the pickups are set in the guest's own world: the ones that took are confirmed.
void ConfirmGuestPickupsInOwnWorld() {
    std::vector<s32> confirmed;
    {
        std::scoped_lock lock{guest_picked_mutex};
        for (const s32 flag : GuestPickedFlags()) {
            if (confirmed.size() < 16 && EventFlagOn(flag)) {
                confirmed.push_back(flag);
            }
        }
    }
    if (!confirmed.empty()) {
        std::ofstream file{GuestConfirmedFile(), std::ios::trunc};
        file << FormatFlagList(confirmed);
    }
}

} // namespace

namespace Coop {

// 0x0146CE10(lamp, 0, 0, 0) finds or creates the lamp's entry in this character's lamp list, the
// same call the lamp object's update makes with its own counters.
constexpr u64 LampListEntryOffset = 0x0146CE10;

void ListLamps(std::span<const s32> lamps) {
    if (lamps.empty()) {
        return;
    }
    std::string listed;
    for (const s32 lamp : lamps) {
        using LampEntry = void PS4_SYSV_ABI (*)(s32 lamp, s32, s32, s32);
        reinterpret_cast<LampEntry>(image_base + LampListEntryOffset)(lamp, 0, 0, 0);
        listed += fmt::format(" {}", lamp);
    }
    LOG_INFO(Debug, "Bloodborne party: lamps{} are lit for this player too", listed);
}

// A lamp's light request on with the lamp not lit for this player never lights it again (event
// 7100 ends at once): cleared as the lamp's map loads (LightRequestsToClear), after the team's
// world and the session's changes went in. Kaniel's playtest 2026-10-20: the Witch's Abode lamp
// stayed unlit for the whole party. Raw, as ApplyHostMapState reads it: the map this load makes.
void ClearLightRequestsThatLitNothing() {
    using SetFlag = void PS4_SYSV_ABI (*)(u64, s32, bool);
    const u32 map = GetCurrentPackedMap();
    std::string cleared;
    for (const s32 flag : LightRequestsToClear(map, EventFlagOn)) {
        reinterpret_cast<SetFlag>(image_base + PartyGhostSetEventFlagOffset)(0, flag, false);
        cleared += fmt::format(" {}", flag);
    }
    if (!cleared.empty()) {
        LOG_INFO(Debug,
                 "Bloodborne party: lamp light requests that lit nothing cleared on map {:#x}:{}",
                 map, cleared);
    }
}

// See ClearsDollLevelUpFlag: the Doll's level-up flag, stuck on from a team's world or an old
// save, off as a map loads (after the team's world went in).
void ClearStuckDollLevelUpFlag() {
    if (!ClearsDollLevelUpFlag(party_controller.has_value(), EventFlagOn(DollLevelUpFlag))) {
        return;
    }
    using SetFlag = void PS4_SYSV_ABI (*)(u64, s32, bool);
    reinterpret_cast<SetFlag>(image_base + PartyGhostSetEventFlagOffset)(0, DollLevelUpFlag, false);
    LOG_INFO(Debug,
             "Bloodborne party: the Doll's level-up flag {} was on as map {:#x} loaded; "
             "cleared",
             DollLevelUpFlag, GetCurrentPackedMap());
}

void SetGuestPickupsBeforeMapItems() {
    ApplyTeamWorldOnLoad();
    RepairRitualAltars();
    ClearLightRequestsThatLitNothing();
    ClearStuckDollLevelUpFlag();
    static std::chrono::steady_clock::time_point last{};
    static OncePerTrip trip_load;
    const auto now = std::chrono::steady_clock::now();
    if (!IsPartyGuestRole()) {
        return;
    }
    // A trip loads the guest's own world: its world changes go in once per trip, before the
    // throttle below (which skipped a Hunter's Dream load right after a pass on the old map).
    if (trip_load.Take(PartyTripActive(), party_trips_started.load())) {
        // The team's world first (the guest's game has its own saved world back: Kaniel's play
        // test 2026-10-18, the opening played again), then the host's map state and the
        // session's changes on top. Raw, as ApplyHostMapState reads it: the map this load is
        // creating.
        ApplyTeamWorldOnLoad(true);
        LOG_INFO(Debug, "Bloodborne party: took {} flags of the host's state for map {:#x}",
                 ApplyHostMapState(), GetCurrentPackedMap());
        LOG_INFO(Debug,
                 "Bloodborne party: set {} of the session's world changes as a trip loads the "
                 "guest's own world",
                 SetGuestWorldChanges(true));
        ApplyTeamTimeOfDayOnLoad();
        ClearLightRequestsThatLitNothing();
    }
    if (now - last < std::chrono::seconds{5}) {
        return;
    }
    last = now;
    if (!InPartySession()) {
        ForgetSlotIfNewCharacter();
    }
    if (const size_t set = SetGuestPickedFlags(); set > 0) {
        LOG_INFO(Debug,
                 "Bloodborne party: set {} of the guest's pickup flags before its map "
                 "items were created",
                 set);
    }
    // A load of the guest's own world (a join's reload already reads as a session, a party
    // trip loads its own world too): the session's world changes go in before its events
    // start, so a boss killed in the host's world doesn't show its health bar here until the
    // next load, and a lamp lit in the session is on the headstone.
    const bool in_session = InPartySession();
    if (!in_session) {
        ConfirmGuestPickupsInOwnWorld();
    }
    if (SetsGuestWorldChangesOnLoad(in_session, PartyTripActive())) {
        if (const size_t set = SetGuestWorldChanges(); set > 0) {
            LOG_INFO(Debug,
                     "Bloodborne party: set {} of the session's world changes as the guest's own "
                     "world loads",
                     set);
        }
    }
    // The team's time of day over the session's changes, before the map's events pick by it.
    ApplyTeamTimeOfDayOnLoad();
}

FlagStore ReadFlagStore() {
    FlagStore store;
    const u64 manager = ReadValue<u64>(image_base + EventFlagManPointerOffset, 0);
    if (manager < 0x10000 || !HasMemoryAccess(manager, 0x40, MemoryProt::CpuRead)) {
        return store;
    }
    store.flags_per_block = ReadValue<u32>(manager, 0x1C);
    const u32 block_bytes = ReadValue<u32>(manager, 0x20);
    const u64 pool = ReadValue<u64>(manager, 0x28);
    const u64 header = ReadValue<u64>(manager, 0x38);
    if (store.flags_per_block == 0 || block_bytes == 0 || block_bytes > 0x10000 ||
        header < 0x10000) {
        return store;
    }
    // The game changes the tree while this walks it (a load on another thread: Ada's crash,
    // 2026-10-11, a node pointing past a thread's stack), so every node is checked first.
    const auto readable = [](u64 node) {
        return node >= 0x10000 && HasMemoryAccess(node, 0x38, MemoryProt::CpuRead);
    };
    // A node that can't be read or a tree deeper than the walk keeps leaves the copy part done.
    bool whole = true;
    const auto inner = [&](u64 node) {
        if (!readable(node)) {
            whole = false;
            return false;
        }
        return ReadValue<u8>(node, 0x19) == 0;
    };
    std::vector<u64> stack;
    u64 node = readable(header) ? ReadValue<u64>(header, 0x08) : 0;
    while (inner(node) || !stack.empty()) {
        while (inner(node)) {
            if (stack.size() >= 64) {
                whole = false;
                break;
            }
            stack.push_back(node);
            node = ReadValue<u64>(node, 0x00);
        }
        if (stack.empty()) {
            break;
        }
        node = stack.back();
        stack.pop_back();
        const u32 kind = ReadValue<u32>(node, 0x28);
        const u64 bits = kind == 2   ? ReadValue<u64>(node, 0x30)
                         : kind == 1 ? pool + u64{block_bytes} * ReadValue<u32>(node, 0x30)
                                     : 0;
        if (bits >= 0x10000 && HasMemoryAccess(bits, block_bytes, MemoryProt::CpuRead)) {
            FlagBlock block{ReadValue<u32>(node, 0x20), std::vector<u8>(block_bytes), bits};
            std::memcpy(block.bytes.data(), reinterpret_cast<const void*>(bits), block_bytes);
            store.blocks.push_back(std::move(block));
        } else if (kind == 1 || kind == 2) {
            whole = false;
        }
        if (store.blocks.size() > 20000) {
            whole = false;
            break;
        }
        node = ReadValue<u64>(node, 0x10);
    }
    store.whole = whole;
    return store;
}

} // namespace Coop

namespace {

// Shared progression (see CarriesToGuestWorld): the session's world changes the guest saw,
// latest state per flag, kept in a file until they are set in its own world.
std::mutex guest_world_mutex;
std::optional<std::map<s32, bool>> guest_world;
int guest_world_slot = -1;
// When this game kept each change (KeptChangeYieldsToTeam); none for the file's.
std::map<s32, s64> guest_world_kept_ms;

std::filesystem::path GuestWorldFile() {
    return Common::FS::GetUserPath(Common::FS::PathType::UserDir) /
           SlotFileName("world", character_slot.load());
}

std::map<s32, bool>& GuestWorldChanges() {
    if (!guest_world || guest_world_slot != character_slot.load()) {
        guest_world_slot = character_slot.load();
        std::ifstream file{GuestWorldFile()};
        const std::string text{std::istreambuf_iterator<char>{file}, {}};
        guest_world = ParseWorldChanges(text);
    }
    return *guest_world;
}

void SaveGuestWorldChanges() {
    std::ofstream file{GuestWorldFile(), std::ios::trunc};
    file << FormatWorldChanges(*guest_world);
}

void ForgetGuestWorldChanges() {
    std::scoped_lock lock{guest_world_mutex};
    GuestWorldChanges().clear();
    SaveGuestWorldChanges();
}

void KeepWorldChanges(const FlagChanges& changes) {
    std::scoped_lock lock{guest_world_mutex};
    auto& world = GuestWorldChanges();
    const size_t before = world.size();
    RecordWorldChanges(world, changes);
    const s64 now_ms = SteadyMilliseconds();
    for (const auto& flags : {changes.set, changes.cleared}) {
        for (const s32 flag : flags) {
            guest_world_kept_ms[flag] = now_ms;
        }
    }
    SaveGuestWorldChanges();
    LOG_INFO(Debug, "Bloodborne party: guest's world keeps {} changes ({} new)", world.size(),
             world.size() - before);
}

} // namespace

namespace Coop {

// At home: sets the kept changes in the guest's own world; the ones that took are done.
size_t SetGuestWorldChanges(bool keep) {
    std::scoped_lock lock{guest_world_mutex};
    auto& world = GuestWorldChanges();
    using SetFlag = void PS4_SYSV_ABI (*)(u64, s32, bool);
    size_t set = 0;
    // A team guest's story steps the team's world has had time to carry are the team's: its world
    // went in first (ApplyTeamWorldOnLoad), these never go over it.
    const std::vector<u32> team_blocks = TeamWorldBlocksForGuest();
    static u32 flags_per_block = 0;
    if (!team_blocks.empty() && flags_per_block == 0) {
        flags_per_block = ReadFlagStore().flags_per_block;
    }
    const s64 now_ms = SteadyMilliseconds();
    std::string yielded;
    size_t yielded_count = 0;
    for (auto it = world.begin(); it != world.end();) {
        const auto kept = guest_world_kept_ms.find(it->first);
        if (!team_blocks.empty() && KeptChangeYieldsToTeam(it->first,
                                                           kept != guest_world_kept_ms.end()
                                                               ? std::optional<s64>{kept->second}
                                                               : std::nullopt,
                                                           now_ms, team_blocks, flags_per_block)) {
            if (yielded.size() < 300) {
                yielded += fmt::format(" {}{}", it->second ? "" : "-", it->first);
            }
            ++yielded_count;
            it = keep ? std::next(it) : world.erase(it);
            continue;
        }
        reinterpret_cast<SetFlag>(image_base + PartyGhostSetEventFlagOffset)(0, it->first,
                                                                             it->second);
        if (EventFlagOn(it->first) == it->second) {
            ++set;
            it = keep ? std::next(it) : world.erase(it);
        } else {
            ++it;
        }
    }
    SaveGuestWorldChanges();
    if (yielded_count > 0) {
        LOG_INFO(Debug,
                 "Bloodborne party: {} of the session's story steps left to the team's world "
                 "(-off):{}",
                 yielded_count, yielded);
    }
    return set;
}

} // namespace Coop

namespace {

// Lamps lit by another party member (see LampOfActiveFlag): the lamp is added to this
// character's lamp list (ListLamps), so the game keeps it lit for this player too.
void AddLampsLitInSession(std::span<const s32> set_flags) {
    std::vector<s32> lamps;
    for (const s32 flag : set_flags) {
        if (const auto lamp = LampOfActiveFlag(flag)) {
            lamps.push_back(*lamp);
        }
    }
    Coop::ListLamps(lamps);
}

// Diagnostics for shared progression: every event flag set or cleared while in a party
// session, to see what lighting a lamp, opening a shortcut or killing a boss sets.
// Lamp tracer (diagnostics): a lamp a guest lit gets its active flag set on the host, and the
// host's game clears it again. A hardware write watch on the flag's byte records every write
// (the instruction after it, the bit, and the image return addresses on the stack); the tick logs
// them. Armed on the host when a lamp's active flag turns on in a session.
struct WatchedWrite {
    u64 rip = 0;
    bool bit_on = false;
    s32 dword = 0; // the aligned dword around the watched byte (an enemy's HP)
    std::array<u64, 8> callers{};
};
constexpr size_t WatchedWriteCapacity = 64;
std::array<WatchedWrite, WatchedWriteCapacity> watched_writes;
std::atomic<size_t> watched_write_count{0};
std::atomic<u64> watched_flag_byte{0};
std::atomic<u8> watched_flag_mask{0};
std::atomic<s32> watched_flag{0};

std::atomic<s32> watched_last_value{-1};

void RecordWatchedWrite(u64 rip, u64 rsp) {
    // Rewrites of the same value from the same place are skipped (the per-frame max HP clamp
    // rewrites the HP); a new place is kept even when it writes the same value.
    static std::atomic<u64> last_rip{0};
    const s32 value = *reinterpret_cast<const volatile u8*>(watched_flag_byte.load());
    const bool same_value = watched_last_value.exchange(value) == value;
    if (last_rip.exchange(rip) == rip && same_value) {
        return;
    }
    const size_t index = watched_write_count.fetch_add(1);
    if (index >= WatchedWriteCapacity) {
        return;
    }
    WatchedWrite& write = watched_writes[index];
    write.rip = rip;
    write.bit_on = (*reinterpret_cast<const volatile u8*>(watched_flag_byte.load()) &
                    watched_flag_mask.load()) != 0;
    write.dword = *reinterpret_cast<const volatile s32*>(watched_flag_byte.load() & ~u64{3});
    const u64 low = image_base;
    const u64 high = image_base + MemoryPatcher::g_eboot_image_size;
    size_t found = 0;
    const size_t slots = StackSlotsToScan(ReadableBytesAt(rsp, 256 * 8), 256);
    for (size_t slot = 0; slot < slots && found < write.callers.size(); ++slot) {
        const u64 value = reinterpret_cast<const u64*>(rsp)[slot];
        if (value > low && value < high) {
            write.callers[found++] = value;
        }
    }
}

void WatchFlagWrites(s32 flag, const FlagStore& store) {
    if (store.flags_per_block == 0) {
        return;
    }
    const FlagBit bit = FlagBitLocation(flag, store.flags_per_block);
    for (const FlagBlock& block : store.blocks) {
        if (block.id != bit.block || bit.byte >= block.bytes.size()) {
            continue;
        }
        const u64 address = block.address + bit.byte;
        if (watched_flag.load() == flag && watched_flag_byte.load() == address) {
            return;
        }
        watched_flag_byte.store(address);
        watched_flag_mask.store(bit.mask);
        watched_flag.store(flag);
        // From a thread of its own, so the game thread running the tick is armed too.
        std::thread([address, flag] {
            const bool armed = WatchByteWrites(address, RecordWatchedWrite);
            LOG_INFO(Debug, "Bloodborne flag tracer: watching flag {} at {:#x} (armed={})", flag,
                     address, armed);
        }).detach();
        return;
    }
}

} // namespace

namespace Coop {

// The enemy HP tracer (see LogNearbyEnemyControl): watches the low byte of the HP, which most
// damage changes; the log names the enemy's handle where a flag tracer names its flag.
void WatchEnemyHp(u64 hp_address, u32 handle) {
    if (watched_flag_byte.load() == hp_address) {
        return;
    }
    watched_last_value.store(ReadValue<u8>(hp_address, 0));
    watched_flag_byte.store(hp_address);
    watched_flag_mask.store(0xFF);
    watched_flag.store(static_cast<s32>(handle));
    std::thread([hp_address, handle] {
        const bool armed = WatchByteWrites(hp_address, RecordWatchedWrite);
        LOG_INFO(Debug, "Bloodborne enemy HP tracer: watching {:#x} HP at {:#x} (armed={})", handle,
                 hp_address, armed);
    }).detach();
}

} // namespace Coop

namespace {

// Before a trip's load frees the watched character: the watch moves to a byte of our own (a
// write watch on freed, reused memory fired all over the load).
u8 parked_watch_byte = 0;

} // namespace

namespace Coop {

void ParkEnemyWatch() {
    const u64 parked = reinterpret_cast<u64>(&parked_watch_byte);
    if (std::getenv("SHADPS4_BLOODBORNE_TRACE_ENEMY_HP") == nullptr ||
        watched_flag_byte.load() == 0 || watched_flag_byte.load() == parked) {
        return;
    }
    watched_flag_byte.store(parked);
    std::thread([parked] { WatchByteWrites(parked, RecordWatchedWrite); }).detach();
}

} // namespace Coop

namespace {

// SHADPS4_BLOODBORNE_TRACE_FLAG, else a lamp's active flag turning on (one watch at a time).
const std::optional<s32> traced_flag =
    TracedFlagSetting(std::getenv("SHADPS4_BLOODBORNE_TRACE_FLAG"));

void WatchLampFlag(s32 flag, const FlagStore& store) {
    if (!traced_flag && LampOfActiveFlag(flag)) {
        WatchFlagWrites(flag, store);
    }
}

} // namespace

namespace Coop {

void LogWatchedWrites() {
    static size_t logged = 0;
    static std::chrono::steady_clock::time_point last_arm{};
    const size_t count = std::min(watched_write_count.load(), WatchedWriteCapacity);
    for (; logged < count; ++logged) {
        const WatchedWrite& write = watched_writes[logged];
        std::string callers;
        for (const u64 caller : write.callers) {
            if (caller != 0) {
                callers += fmt::format(" {:#x}", caller - image_base);
            }
        }
        LOG_INFO(Debug, "Bloodborne flag tracer: write to flag {} ({}, dword {}) at {:#x}, stack{}",
                 watched_flag.load(), write.bit_on ? "on" : "off", write.dword,
                 write.rip >= image_base ? write.rip - image_base : write.rip, callers);
    }
    // Threads started since the watch was armed (a load) get armed too.
    const auto now = std::chrono::steady_clock::now();
    if (const u64 address = watched_flag_byte.load();
        address != 0 && now - last_arm > std::chrono::seconds{10}) {
        last_arm = now;
        std::thread([address] { WatchByteWrites(address, RecordWatchedWrite); }).detach();
    }
}

} // namespace Coop

namespace {

// A lamp's first registration for a character (0x0146DA20, from the bonfire registration at
// 0x01327794) writes its flags base..base+11 off; on the host that unlit a lamp a guest had lit
// (found with the lamp tracer). One hook notes which were on before the writes (base in r12d,
// entry in rbx, at `mov [rbx+0x14], r12d; test r12d, r12d`), one sets them again after
// (`mov [rbx+0x18], r15d; mov byte [rbx+0x11], 1`, the registration's end).
constexpr u64 LampRegistrationStartOffset = 0x0146DB21;
constexpr std::array<u8, 7> LampRegistrationStart{0x44, 0x89, 0x63, 0x14, 0x45, 0x85, 0xE4};
constexpr u64 LampRegistrationEndOffset = 0x0146DC0B;
constexpr std::array<u8, 8> LampRegistrationEnd{0x44, 0x89, 0x7B, 0x18, 0xC6, 0x43, 0x11, 0x01};

struct LampRegistration {
    u64 entry = 0;
    s32 base = -1;
    u16 on_before = 0;
};
thread_local LampRegistration lamp_registration;

void PS4_SYSV_ABI NoteLampFlagsBeforeRegistration(u64, GuestRegisterSnapshot* registers) {
    const s32 base = static_cast<s32>(registers->r12);
    lamp_registration = {registers->rbx, base, 0};
    if (!party_controller || base < 0) {
        return;
    }
    for (s32 index = 0; index < 12; ++index) {
        if (EventFlagOn(base + index)) {
            lamp_registration.on_before |= static_cast<u16>(1u << index);
        }
    }
}

void PS4_SYSV_ABI KeepLampLitAfterRegistration(u64, GuestRegisterSnapshot* registers) {
    const LampRegistration noted = std::exchange(lamp_registration, {});
    if (noted.entry != registers->rbx) {
        return;
    }
    const std::vector<s32> kept =
        LampFlagsKeptAtRegistration(party_controller.has_value(), noted.base, noted.on_before);
    using SetFlag = void PS4_SYSV_ABI (*)(u64, s32, bool);
    size_t set = 0;
    for (const s32 flag : kept) {
        if (!EventFlagOn(flag)) {
            reinterpret_cast<SetFlag>(image_base + PartyGhostSetEventFlagOffset)(0, flag, true);
            ++set;
        }
    }
    if (set != 0) {
        LOG_INFO(Debug, "Bloodborne party: lamp registration at flag {} kept {} lit flags",
                 noted.base, set);
    }
}

} // namespace

namespace Coop {

void InstallLampsStayLitAtRegistration(u64 base, u64 image_size) {
    if (!party_controller) {
        return;
    }
    if (LampRegistrationEndOffset + LampRegistrationEnd.size() > image_size ||
        !InstallCoopHook(reinterpret_cast<void*>(base + LampRegistrationStartOffset),
                         LampRegistrationStart, 0, NoteLampFlagsBeforeRegistration)) {
        LOG_ERROR(Debug, "Bloodborne lamp registration hooks unavailable; a lamp a guest lit "
                         "stays unlit for the host");
        coop_features.Skipped("lamp registration",
                              SiteFailure(base, image_size, LampRegistrationStartOffset));
        return;
    }
    if (!InstallCoopHook(reinterpret_cast<void*>(base + LampRegistrationEndOffset),
                         LampRegistrationEnd, 0, KeepLampLitAfterRegistration)) {
        LOG_ERROR(Debug, "Bloodborne lamp registration hooks unavailable; a lamp a guest lit "
                         "stays unlit for the host");
        coop_features.Skipped("lamp registration",
                              SiteFailure(base, image_size, LampRegistrationEndOffset));
        return;
    }
    coop_features.On("lamp registration");
}

} // namespace Coop

namespace {

constexpr std::array<u8, 10> AwardItemLotPrologue{0x55, 0x48, 0x89, 0xE5, 0x41,
                                                  0x57, 0x41, 0x56, 0x41, 0x55};
std::mutex deferred_item_lots_mutex;
std::vector<std::pair<s32, std::chrono::steady_clock::time_point>> deferred_item_lots;
struct HeldNpcGift {
    NpcGift gift;
    std::chrono::steady_clock::time_point due;
};
std::vector<HeldNpcGift> held_npc_gifts; // under deferred_item_lots_mutex

} // namespace

namespace Coop {

thread_local bool giving_deferred_item_lot = false;

} // namespace Coop

namespace {

// An item lot's getItemFlagId (ItemLotParam +0x80); 0 when the lot is not loaded.
s32 ItemLotFlag(s32 lot) {
    const u64 lots = lot > 0 ? FindLoadedParam("ITEMLOT_PARAM_ST") : 0;
    if (lots == 0) {
        return 0;
    }
    const u64 table_size = 0x40 + u64{ReadValue<u16>(lots, 0x0A)} * 24;
    if (!HasMemoryAccess(lots, table_size, MemoryProt::CpuRead)) {
        return 0;
    }
    const auto row = FindParamRow({reinterpret_cast<const u8*>(lots), table_size}, lot);
    if (!row || !HasMemoryAccess(lots + *row, ItemLotFlagOffset + 4, MemoryProt::CpuRead)) {
        return 0;
    }
    return ReadValue<s32>(lots + *row, ItemLotFlagOffset);
}

bool PS4_SYSV_ABI HoldGuestBossItem(u64, const GuestRegisterSnapshot* registers) {
    if (giving_deferred_item_lot) {
        return false;
    }
    const u64 caller = ReadValue<u64>(registers->rsp, 0);
    // Every award, who made it and its lot's flag (NPC gifts, 2026-10-13: common event 9100's
    // is eboot+0x17c180b).
    {
        const s32 lot = static_cast<s32>(registers->rsi);
        const s32 flag = ItemLotFlag(lot);
        LOG_INFO(Debug,
                 "Bloodborne party: item lot {} awarded from eboot+{:#x} (callers eboot+{:#x} "
                 "eboot+{:#x}), its flag {} {}",
                 lot, caller - image_base, ReadValue<u64>(registers->rsp, 8) - image_base,
                 ReadValue<u64>(registers->rsp, 16) - image_base, flag,
                 flag > 0 && EventFlagOn(flag) ? "on" : "off");
    }
    if (!DefersItemLotAward(party_controller.has_value(), IsPartyGuestRole(),
                            caller - image_base)) {
        return false;
    }
    const s32 lot = static_cast<s32>(registers->rsi);
    {
        std::scoped_lock lock{deferred_item_lots_mutex};
        deferred_item_lots.emplace_back(lot,
                                        std::chrono::steady_clock::now() + DeferredBossItemDelay);
    }
    LOG_INFO(Debug, "Bloodborne party: guest's boss item lot {} held until the boss clear ends",
             lot);
    return true;
}

} // namespace

namespace Coop {

void QueueGuestNpcGift(const NpcGift& gift) {
    {
        std::scoped_lock lock{deferred_item_lots_mutex};
        if (std::ranges::any_of(held_npc_gifts, [&](const HeldNpcGift& held) {
                return held.gift.trigger == gift.trigger;
            })) {
            return; // the talk set it again before the gift was given
        }
        held_npc_gifts.push_back({gift, std::chrono::steady_clock::now() + NpcGiftDelay});
    }
    LOG_INFO(Debug, "Bloodborne party: the guest's talk set NPC gift trigger {}: item lot {} next",
             gift.trigger, gift.lot);
}

// The held NPC gifts that are due: each given once, then what its event does once it has given it
// (else the guest's own world's event would give it again at home).
void GiveDueNpcGifts(std::chrono::steady_clock::time_point now) {
    std::vector<HeldNpcGift> due;
    {
        std::scoped_lock lock{deferred_item_lots_mutex};
        std::erase_if(held_npc_gifts, [&](const HeldNpcGift& held) {
            if (now < held.due) {
                return false;
            }
            due.push_back(held);
            return true;
        });
    }
    using AwardItemLot = void PS4_SYSV_ABI (*)(u64, s32, u32);
    using SetFlag = void PS4_SYSV_ABI (*)(u64, s32, bool);
    const auto set_flag = reinterpret_cast<SetFlag>(image_base + PartyGhostSetEventFlagOffset);
    for (const auto& [gift, _] : due) {
        const u64 item_man = ReadValue<u64>(image_base + MapItemManPointerOffset, 0);
        if (!HasVtable(item_man, MapItemManVtable, "NPC gift item lot")) {
            LOG_ERROR(Debug, "Bloodborne party: no item man for NPC gift item lot {}", gift.lot);
            continue;
        }
        // The lot gives nothing while its flag is on: the host's, from the team world (Valtr's
        // rune, Kaniel's playtest 2026-10-25), is not this guest's.
        const s32 lot_flag = ItemLotFlag(gift.lot);
        if (ClearsNpcGiftLotFlag(lot_flag, lot_flag > 0 && EventFlagOn(lot_flag))) {
            set_flag(0, lot_flag, false);
            LOG_INFO(Debug,
                     "Bloodborne party: NPC gift item lot {}: its flag {} was the host's; cleared "
                     "for the guest's own",
                     gift.lot, lot_flag);
        }
        giving_deferred_item_lot = true;
        reinterpret_cast<AwardItemLot>(image_base + AwardItemLotOffset)(item_man, gift.lot, 0);
        giving_deferred_item_lot = false;
        switch (gift.after) {
        case NpcGiftAfter::ClearTrigger:
            set_flag(0, gift.trigger, false);
            break;
        case NpcGiftAfter::KeepTrigger:
            break;
        case NpcGiftAfter::SetDoneFlag:
            set_flag(0, gift.done_flag, true);
            break;
        }
        LOG_INFO(Debug, "Bloodborne party: gave the guest its NPC gift, item lot {} (trigger {})",
                 gift.lot, gift.trigger);
    }
}

// See GivesNpcDeathDrop: every 2 s, a party guest gets the drop of an NPC that died on its map
// (the host's world says so), as the host's event gave the host. The game's thread.
void GiveNpcDeathDrops(bool in_session, u32 map) {
    static s64 checked_ms = 0;
    static int given_slot = -2;
    static std::set<s32> given; // death flags, this character in this session
    const s64 now_ms = SteadyMilliseconds();
    if (const int slot = character_slot.load(); slot != given_slot || !in_session) {
        given_slot = slot;
        given.clear();
    }
    const bool guest = in_session && IsPartyGuestRole();
    if (!guest || now_ms - checked_ms < 2'000 || GetLocalPlayer() < 0x10000 ||
        !PlayerItemsReadable()) {
        return;
    }
    checked_ms = now_ms;
    using AwardItemLot = void PS4_SYSV_ABI (*)(u64, s32, u32);
    using SetFlag = void PS4_SYSV_ABI (*)(u64, s32, bool);
    for (const NpcDeathDrop& drop : NpcDeathDrops) {
        if (drop.map != map) {
            continue; // before reading anything
        }
        const s32 lot_flag = ItemLotFlag(drop.lot);
        const bool lot_flag_on = lot_flag > 0 && EventFlagOn(lot_flag);
        const bool lot_flag_hosts = lot_flag > 0 && ClearsNpcGiftLotFlag(lot_flag, true);
        const bool holds =
            drop.goods != 0 && TradeHeld({BloodborneCoop::TradeItemKind::Goods, drop.goods, 0}) > 0;
        if (!GivesNpcDeathDrop(drop, guest, map, EventFlagOn(drop.death_flag), lot_flag_on,
                               lot_flag_hosts, holds, given.contains(drop.death_flag))) {
            continue;
        }
        const u64 item_man = ReadValue<u64>(image_base + MapItemManPointerOffset, 0);
        if (!HasVtable(item_man, MapItemManVtable, "NPC death drop item lot")) {
            return;
        }
        given.insert(drop.death_flag);
        // The lot gives nothing while its flag is on: the host's (6671, the host holds the badge)
        // is not this guest's.
        if (lot_flag_on && lot_flag_hosts) {
            reinterpret_cast<SetFlag>(image_base + PartyGhostSetEventFlagOffset)(0, lot_flag,
                                                                                 false);
        }
        giving_deferred_item_lot = true;
        reinterpret_cast<AwardItemLot>(image_base + AwardItemLotOffset)(item_man, drop.lot, 0);
        giving_deferred_item_lot = false;
        LOG_INFO(Debug,
                 "Bloodborne party: NPC death flag {} on (map {:#x}); gave the guest its drop, "
                 "item lot {} (its flag {} {})",
                 drop.death_flag, map, drop.lot, lot_flag,
                 lot_flag_on ? (lot_flag_hosts ? "was the host's" : "on") : "off");
    }
}

void GiveDeferredBossItems() {
    std::vector<s32> due;
    {
        std::scoped_lock lock{deferred_item_lots_mutex};
        const auto now = std::chrono::steady_clock::now();
        std::erase_if(deferred_item_lots, [&](const auto& held) {
            if (now < held.second) {
                return false;
            }
            due.push_back(held.first);
            return true;
        });
    }
    GiveDueNpcGifts(std::chrono::steady_clock::now());
    using AwardItemLot = void PS4_SYSV_ABI (*)(u64, s32, u32);
    for (const s32 lot : due) {
        const u64 item_man = ReadValue<u64>(image_base + MapItemManPointerOffset, 0);
        if (!HasVtable(item_man, MapItemManVtable, "held boss item lot")) {
            LOG_ERROR(Debug, "Bloodborne party: no item man for held boss item lot {}", lot);
            continue;
        }
        giving_deferred_item_lot = true;
        reinterpret_cast<AwardItemLot>(image_base + AwardItemLotOffset)(item_man, lot, 0);
        giving_deferred_item_lot = false;
        LOG_INFO(Debug, "Bloodborne party: gave the guest's held boss item lot {}", lot);
    }
}

} // namespace Coop

namespace {

// The other player's "set this event flag" message (see KeepsLocalFlag): at
// `test r15b, r15b; setne al; movzx edx, al; mov esi, ebx` before the call to the setter
// 0x013CFCC0 (flag in ebx, value in r15b). A kept flag gets its own current value, so the
// write changes nothing.
constexpr u64 RemoteSetFlagOffset = 0x017C0565;
constexpr std::array<u8, 11> RemoteSetFlag{0x45, 0x84, 0xFF, 0x0F, 0x95, 0xC0,
                                           0x0F, 0xB6, 0xD0, 0x89, 0xDE};

void PS4_SYSV_ABI KeepLocalFlagsFromOthers(u64, GuestRegisterSnapshot* registers) {
    const s32 flag = static_cast<s32>(registers->rbx);
    // Diagnostics: which Hunter's Dream flags the other player sends (gift investigation).
    if (party_controller &&
        ((flag >= 12100000 && flag < 12110000) || (flag >= 72100000 && flag < 72110000))) {
        LOG_INFO(Debug, "Bloodborne party: the other player sends flag {} = {}", flag,
                 registers->r15 & 0xFF);
        // Another member's warp into a chalice dungeon: the trip this game follows goes there.
        if (const auto altar = AltarOfWarpRequest(flag)) {
            NoteAltarWarp(*altar);
        }
    }
    // A Dream headstone's flag: this game's own lamps decide, also for the message that carries
    // this game's own event's set in a session (HeadstoneLampsLit).
    if (const auto lit = party_controller ? HeadstoneLampsLit(flag, EventFlagOn) : std::nullopt) {
        if (((registers->r15 & 0xFF) != 0) != *lit) {
            LOG_INFO(Debug,
                     "Bloodborne party: headstone flag {} {} by this player's lamps, "
                     "against the other player's",
                     flag, *lit ? "on" : "off");
        }
        registers->r15 = (registers->r15 & ~u64{0xFF}) | (*lit ? 1 : 0);
        return;
    }
    // Invasions: the invaded world's flags never go into the invader's (every one stays its own).
    if (!KeepsLocalFlag(party_controller.has_value(), flag) && !Invading()) {
        return;
    }
    const bool local = EventFlagOn(flag);
    if (((registers->r15 & 0xFF) != 0) != local) {
        LOG_INFO(Debug, "Bloodborne party: kept flag {} {} against the other player's", flag,
                 local ? "on" : "off");
    }
    registers->r15 = (registers->r15 & ~u64{0xFF}) | (local ? 1 : 0);
}

// The other player's "event ended" message (0x017C0901: event id in r15d, slot in r14d, then
// 0x012EFE90 ends that event here): it ended a messenger's gift event for the player who did not
// take the gift, which set the event's own flag (12101020 + n). A kept event gets id -1, which
// 0x012EFE90 ignores.
constexpr u64 RemoteEndEventOffset = 0x017C0901;
constexpr std::array<u8, 14> RemoteEndEvent{0x84, 0xDB, 0x0F, 0x95, 0xC0, 0x0F, 0xB6,
                                            0xC8, 0x44, 0x89, 0xFE, 0x44, 0x89, 0xF2};

void PS4_SYSV_ABI KeepLocalEventsFromOthers(u64, GuestRegisterSnapshot* registers) {
    const s32 event = static_cast<s32>(registers->r15);
    // Diagnostics: which Hunter's Dream events the other player ends (the shop messengers).
    if (party_controller && event >= 12100000 && event < 12110000) {
        LOG_INFO(Debug, "Bloodborne party: the other player ends event {} (slot {})", event,
                 static_cast<s32>(registers->r14));
    }
    if (KeepsLocalFlag(party_controller.has_value(), event) || Invading()) {
        LOG_INFO(Debug, "Bloodborne party: kept event {} running against the other player's end",
                 event);
        registers->r15 = (registers->r15 & ~u64{0xFFFFFFFF}) | u64{0xFFFFFFFF};
    }
}

} // namespace

namespace Coop {

void InstallKeepLocalFlags(u64 base, u64 image_size) {
    if (!party_controller) {
        return;
    }
    std::optional<std::string> failed;
    if (RemoteSetFlagOffset + RemoteSetFlag.size() > image_size ||
        !InstallCoopHook(reinterpret_cast<void*>(base + RemoteSetFlagOffset), RemoteSetFlag, 0,
                         KeepLocalFlagsFromOthers)) {
        LOG_ERROR(Debug, "Bloodborne remote flag hook unavailable; messenger gifts are shared");
        failed = SiteFailure(base, image_size, RemoteSetFlagOffset);
    }
    if (RemoteEndEventOffset + RemoteEndEvent.size() > image_size ||
        !InstallCoopHook(reinterpret_cast<void*>(base + RemoteEndEventOffset), RemoteEndEvent, 0,
                         KeepLocalEventsFromOthers)) {
        LOG_ERROR(Debug, "Bloodborne remote event hook unavailable; messenger gifts are shared");
        failed = failed.value_or(SiteFailure(base, image_size, RemoteEndEventOffset));
    }
    if (failed) {
        coop_features.Skipped("keep local flags", *failed);
    } else {
        coop_features.On("keep local flags");
    }
}

void InstallDeferredBossItems(u64 base, u64 image_size) {
    if (!party_controller) {
        return;
    }
    if (AwardItemLotOffset + AwardItemLotPrologue.size() > image_size ||
        !InstallCoopHook(reinterpret_cast<void*>(base + AwardItemLotOffset), AwardItemLotPrologue,
                         0, HoldGuestBossItem)) {
        LOG_ERROR(Debug, "Bloodborne boss item hook unavailable; a guest's boss item dialog "
                         "may leave it stuck");
        coop_features.Skipped("boss items", SiteFailure(base, image_size, AwardItemLotOffset));
        return;
    }
    coop_features.On("boss items");
}

s64 SteadyMilliseconds() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

void LogFlagChangesInSession(bool sharing) {
    static std::chrono::steady_clock::time_point last{};
    static std::optional<FlagStore> previous;
    const auto now = std::chrono::steady_clock::now();
    if (!party_controller || now - last < std::chrono::seconds{3}) {
        return;
    }
    last = now;
    if (!sharing) {
        previous.reset();
        return;
    }
    FlagStore current = ReadFlagStore();
    if (current.blocks.empty()) {
        return;
    }
    if (traced_flag) {
        WatchFlagWrites(*traced_flag, current);
    }
    if (previous && previous->flags_per_block == current.flags_per_block) {
        std::vector<s32> set;
        std::vector<s32> cleared;
        size_t old_index = 0;
        for (const FlagBlock& block : current.blocks) {
            while (old_index < previous->blocks.size() &&
                   previous->blocks[old_index].id < block.id) {
                ++old_index;
            }
            if (old_index >= previous->blocks.size() ||
                previous->blocks[old_index].id != block.id) {
                continue;
            }
            const FlagChanges changes = ChangedFlags(
                block.id, current.flags_per_block, previous->blocks[old_index].bytes, block.bytes);
            set.insert(set.end(), changes.set.begin(), changes.set.end());
            cleared.insert(cleared.end(), changes.cleared.begin(), changes.cleared.end());
        }
        if (!set.empty() || !cleared.empty()) {
            const auto list = [](std::span<const s32> flags) {
                std::string text;
                for (size_t index = 0; index < std::min<size_t>(flags.size(), 48); ++index) {
                    text += fmt::format(" {}", flags[index]);
                }
                if (flags.size() > 48) {
                    text += fmt::format(" (+{})", flags.size() - 48);
                }
                return text;
            };
            LOG_INFO(Debug, "Bloodborne flags in session: set{} | cleared{}", list(set),
                     list(cleared));
            AddLampsLitInSession(set);
            // A ritual or a join writes the current altar's view (a load into a dungeon and the
            // altar's own reload do too: ShareRitualAltarRecord tells them apart).
            if (std::ranges::any_of(set, IsCurrentAltarFlag) ||
                std::ranges::any_of(cleared, IsCurrentAltarFlag)) {
                ShareRitualAltarRecord(set.size() + cleared.size());
            }
            if (!IsPartyGuestRole()) {
                for (const s32 flag : set) {
                    WatchLampFlag(flag, current);
                }
            }
            if (IsBulkFlagChange(set.size() + cleared.size())) {
                world_changed_ms.store(SteadyMilliseconds()); // deaths now are no kills
                LOG_INFO(Debug,
                         "Bloodborne party: {} flags changed at once (a load), not kept or "
                         "shared",
                         set.size() + cleared.size());
            } else {
                const FlagChanges changes{std::move(set), std::move(cleared)};
                if (IsPartyGuestRole()) {
                    KeepWorldChanges(changes);
                }
                PostWorldChanges(changes);
            }
        }
    }
    previous = std::move(current);
}

void KeepGuestProgress(bool in_session) {
    static std::chrono::steady_clock::time_point last{};
    static std::chrono::steady_clock::time_point home_since{};
    static std::vector<s32> watched;
    static std::unordered_set<s32> seen_on;
    static bool have_baseline = false;
    static bool set_at_home = false;
    static int slot = -1;
    const auto now = std::chrono::steady_clock::now();
    if (!IsPartyGuestRole() || now - last < GuestProgressInterval) {
        return;
    }
    last = now;
    if (slot != character_slot.load()) {
        slot = character_slot.load();
        set_at_home = false;
        home_since = {};
    }
    if (watched.empty()) {
        watched = MapPickupFlags();
        if (watched.empty()) {
            return;
        }
        LOG_INFO(Debug, "Bloodborne party: watching {} pickup flags for the guest's own save",
                 watched.size());
    }
    if (in_session) {
        home_since = {};
        set_at_home = false;
        if (!have_baseline) {
            seen_on.clear();
            for (const s32 flag : watched) {
                if (EventFlagOn(flag)) {
                    seen_on.insert(flag);
                }
            }
            have_baseline = true;
            return;
        }
        std::scoped_lock lock{guest_picked_mutex};
        auto& picked = GuestPickedFlags();
        bool changed = false;
        for (const s32 flag : watched) {
            if (!seen_on.contains(flag) && EventFlagOn(flag)) {
                seen_on.insert(flag);
                if (std::ranges::find(picked, flag) == picked.end()) {
                    picked.push_back(flag);
                    changed = true;
                    LOG_INFO(Debug, "Bloodborne party: guest picked up item flag {}", flag);
                }
            }
        }
        if (changed) {
            SaveGuestProgress(picked);
        }
        return;
    }
    have_baseline = false;
    if (set_at_home || GetLocalPlayer() < 0x10000) {
        if (GetLocalPlayer() < 0x10000) {
            home_since = {};
        }
        return;
    }
    if (home_since == std::chrono::steady_clock::time_point{}) {
        home_since = now;
    }
    if (now - home_since < GuestProgressHomeDelay) {
        return;
    }
    set_at_home = true;
    LOG_INFO(Debug, "Bloodborne party: set {} of the guest's pickup flags in its own world",
             SetGuestPickedFlags());
    LOG_INFO(Debug,
             "Bloodborne party: set {} of the session's world changes in the guest's own "
             "world",
             SetGuestWorldChanges());
}

// Ritual altars whose record came from the team, and when they load it (see ReloadRitualAltar).
std::map<s32, std::chrono::steady_clock::time_point> altar_reloads;
constexpr auto AltarRecordSettle = std::chrono::seconds{3};

// On the host's game thread: sets the guests' world changes in the host's world.
void SetPartyWorldChanges(bool sharing) {
    party_host_role.store(party_controller.has_value() &&
                          party_controller->SearchEffectId() != SmallResonantBellEffect);
    GiveBackAltarChalices();
    if (sharing) {
        const auto now = std::chrono::steady_clock::now();
        // A teammate's ritual: its record goes in whole, and the altar loads it. A teammate's
        // chalice taken off: the altar empties and its dungeon ends (Kaniel 2026-10-30).
        for (const AltarRecordNews& news : TakeAltarRecords()) {
            const s32 base = AltarFlagBase(news.altar);
            const auto mine = news.altar >= 0 && news.altar < RitualAltarCount
                                  ? ReadAltarFlags(base)
                                  : std::nullopt;
            const TeamAltarTake take =
                mine ? TakeTeamAltarRecord(*mine, news.record) : TeamAltarTake::LeftOut;
            if (take == TeamAltarTake::LeftOut) {
                LOG_WARNING(Debug,
                            "Bloodborne team altar: {}'s record for altar {} names no dungeon "
                            "({}); left out",
                            news.member, news.altar, RecordName(news.record));
                continue;
            }
            if (take == TeamAltarTake::Same) {
                LOG_INFO(Debug, "Bloodborne team altar: {}'s record for altar {} is this one's",
                         news.member, news.altar);
                continue;
            }
            WriteAltarFlags(base, news.record);
            if (take == TeamAltarTake::Clear) {
                // The ritual took this player's chalice and the taker got its own back.
                if (const auto chalice = AltarRecordChalice(*mine)) {
                    std::scoped_lock lock{altar_chalices_mutex};
                    altar_chalices_owed.push_back(*chalice);
                }
                LOG_INFO(Debug,
                         "Bloodborne team altar: {} took the chalice off altar {}: emptied here "
                         "too, its dungeon's {} flags cleared",
                         news.member, news.altar, ClearAltarDungeon(news.altar));
                EmptyRitualAltar(news.altar);
                continue;
            }
            altar_reloads[news.altar] = now + AltarRecordSettle;
            LOG_INFO(Debug, "Bloodborne team altar: altar {} has {}'s record ({})", news.altar,
                     news.member, RecordName(news.record));
        }
        for (auto it = altar_reloads.begin(); it != altar_reloads.end();) {
            if (now >= it->second) {
                ReloadRitualAltar(it->first);
                GiveAltarChalice(it->first);
                it = altar_reloads.erase(it);
            } else {
                ++it;
            }
        }
    }
    std::vector<std::pair<s32, bool>> pending;
    {
        std::scoped_lock lock{flag_board_mutex};
        pending.swap(flags_to_set);
    }
    if (pending.empty()) {
        return;
    }
    if (!sharing) {
        // Loading: they wait for the player.
        std::scoped_lock lock{flag_board_mutex};
        flags_to_set.insert(flags_to_set.begin(), pending.begin(), pending.end());
        return;
    }
    using SetFlag = void PS4_SYSV_ABI (*)(u64, s32, bool);
    size_t set = 0;
    for (const auto& [flag, on] : pending) {
        if (SharesClearsWithParty(flag) || EventFlagOn(flag) != on) {
            reinterpret_cast<SetFlag>(image_base + PartyGhostSetEventFlagOffset)(0, flag, on);
            ++set;
        }
    }
    LOG_INFO(Debug, "Bloodborne party: set {} of {} shared flags from the party", set,
             pending.size());
    const FlagStore store = ReadFlagStore();
    for (const auto& [flag, on] : pending) {
        if (on) {
            WatchLampFlag(flag, store);
        }
    }
}

// A party guest resumes where it left (see ResumePoint): its last map and position are kept in
// user/bloodborne_party_resume_slotN.txt every few seconds; the first load after the game starts
// goes back there (a trip to that map's first lamp if needed, then a move like a revive's).
// How many event flags are set (the whole store): only adds up as a character plays. None when
// the store did not read whole (see SettledFlagCount).
std::optional<s32> CountSetFlags() {
    const FlagStore store = ReadFlagStore();
    if (!store.whole || store.blocks.empty()) {
        return std::nullopt;
    }
    s32 count = 0;
    for (const FlagBlock& block : store.blocks) {
        for (const u8 byte : block.bytes) {
            count += std::popcount(byte);
        }
    }
    return count;
}

} // namespace Coop

} // namespace Core::Bloodborne
