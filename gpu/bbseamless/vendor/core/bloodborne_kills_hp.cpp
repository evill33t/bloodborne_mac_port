// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// Kills and HP: enemy HP and kills kept in step, shared damage, shown characters. Split from
// bloodborne_re.cpp; the shared declarations are in bloodborne_re_internal.h.

#include "core/bloodborne_crash_dump.h"
#include "core/bloodborne_party.h"
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

// Kill echoes shared across maps (see SharesKillEchoes): the local kills made apart wait here
// for the board thread, and the other members' wait for the game thread to add those owed.
std::mutex echo_board_mutex;
KillEchoLedger echo_ledger;
// World state per map (see MapFlagBase): the host's own snapshot waiting for the board thread,
// and the latest one read back (on a guest).
// Enemy HP kept in step (see EnemyHpReport): this member's hurt enemies and the others'.
std::mutex enemy_hp_mutex;
MemberMapState local_enemy_hp;
std::mutex map_state_mutex;
MemberMapState local_map_state;
std::optional<MemberMapState> host_map_state;
// Characters hidden or shown as on the host (see CharacterShown): the host's own, waiting for the
// board thread, and the host's latest read back (on a guest).
std::mutex shown_board_mutex;
MemberMapState local_shown;

std::mutex map_board_mutex;
std::vector<MemberMap> map_board;
std::string map_board_member;
std::chrono::steady_clock::time_point map_board_time{};
// Bumped with each new trace file: the "-in" records start whole again in it (see TraceSampler).
std::atomic<u32> coop_trace_file{0};

} // namespace Coop

namespace {

// The boss HP bar that showed two thirds on the guest while the host's was right: a boss start
// gives the co-op HP effect (7500-7502) while the side that did not trigger the entrance may
// still have the boss hidden by it, and there the max HP stays unscaled (see CoopHpScale).
// Every character with a boss-sized max HP ([[+0x3B0]+0x20]: HP +0xF8, max +0xFC and +0x104)
// is remembered without the effect and scaled when it has the effect but not the max HP. Its
// HP fields, entity and co-op effects are logged on change.
std::unordered_map<u64, s32> unscaled_boss_max;
u64 unscaled_boss_visit = 0; // see Place::visit: by character object, which a reload makes anew

} // namespace

namespace Coop {

// Entity ids by character object, as the lookup 0x0191A440 finds them: WorldChrMan +0x90 tables
// at +0x98, each with +0xE0 sorted (entity id, block << 14 | slot) pairs at +0xE8; the character
// is [[+0x28] + block * 0x160 + 0xC0] + slot * 0x38.
std::unordered_map<u64, s32> CharacterEntityIds(u64 world) {
    std::unordered_map<u64, s32> entities;
    const s32 tables = std::min(ReadValue<s32>(world, 0x90), 16);
    const u64 blocks = ReadValue<u64>(world, 0x28);
    for (s32 table = 0; table < tables && blocks >= 0x10000; ++table) {
        const u64 entries = ReadValue<u64>(world, 0x98 + 8 * table);
        const s32 count = entries >= 0x10000 ? std::min(ReadValue<s32>(entries, 0xE0), 4096) : 0;
        const u64 pairs = count > 0 ? ReadValue<u64>(entries, 0xE8) : 0;
        for (s32 index = 0; index < count && pairs >= 0x10000; ++index) {
            const s32 entity = ReadValue<s32>(pairs, 8 * index);
            const s32 packed = ReadValue<s32>(pairs, 8 * index + 4);
            if (packed < 0 || ((packed >> 14) & 0x3F) >= ReadValue<s32>(world, 0x20)) {
                continue;
            }
            const u64 slots = ReadValue<u64>(blocks + ((packed >> 14) & 0x3F) * 0x160, 0xC0);
            if (slots >= 0x10000) {
                entities[ReadValue<u64>(slots + (packed & 0x3FFF) * 0x38, 0)] = entity;
            }
        }
    }
    return entities;
}

} // namespace Coop

namespace {

// The Doll's character in this world (see ChangesHandsWithTheMap), 0 when it has none: looked up
// by her entity id once a second (and on a new world); the game thread only. Compared with the
// world's characters, never read itself (a reload frees the old one).
u64 DollCharacter() {
    const u64 world = ReadValue<u64>(image_base + CandidateLocalStatePointerOffset, 0);
    static u64 looked_in = 0;
    static u64 doll = 0;
    static std::chrono::steady_clock::time_point looked{};
    const auto now = std::chrono::steady_clock::now();
    if (world != looked_in || now - looked >= std::chrono::seconds{1}) {
        looked_in = world;
        looked = now;
        doll = 0;
        if (world >= 0x10000) {
            for (const auto& [character, entity] : CharacterEntityIds(world)) {
                if (!ChangesHandsWithTheMap(entity)) {
                    doll = character;
                }
            }
        }
    }
    return doll;
}

} // namespace

namespace Coop {

void KeepBossHpInStep() {
    // With harder enemies the co-op effects no longer scale the max HP (RemoveCoopBossHpBonus).
    if (EnemiesAreHarder()) {
        return;
    }
    static std::string last;
    static std::chrono::steady_clock::time_point checked{};
    const auto now = std::chrono::steady_clock::now();
    if (now - checked < std::chrono::milliseconds{500}) {
        return;
    }
    checked = now;
    const u64 world = ReadValue<u64>(image_base + CandidateLocalStatePointerOffset, 0);
    if (world < 0x10000) {
        return;
    }
    if (const u64 visit = CurrentPlace().visit; visit != unscaled_boss_visit) {
        unscaled_boss_max.clear();
        unscaled_boss_visit = visit;
    }
    const std::unordered_map<u64, s32> entities = CharacterEntityIds(world);
    std::string text;
    ForEachWorldCharacter(world, [&](u64 character) {
        const u64 modules = ReadValue<u64>(character, 0x3B0);
        const u64 data = modules >= 0x10000 ? ReadValue<u64>(modules, 0x20) : 0;
        if (data < 0x10000 || ReadValue<s32>(data, 0xFC) < 1500) {
            return;
        }
        const auto entity = entities.find(character);
        const std::vector<s32> all_effects = EffectsOf(character);
        std::string effects;
        for (const s32 id : all_effects) {
            if (id >= 7500 && id <= 7502) {
                effects += fmt::format(" {}", id);
            }
        }
        const s32 max = ReadValue<s32>(data, 0xFC);
        if (effects.empty()) {
            unscaled_boss_max[character] = max;
        } else if (const auto it = unscaled_boss_max.find(character);
                   it != unscaled_boss_max.end()) {
            const auto scaled =
                CoopHpScale(it->second, max, ReadValue<s32>(data, 0xF8), all_effects);
            if (scaled && HasMemoryAccess(data, 0x108, MemoryProt::CpuWrite)) {
                WriteValue(data, 0xFC, scaled->first);
                WriteValue(data, 0x104, scaled->first);
                WriteValue(data, 0xF8, scaled->second);
                LOG_INFO(Debug, "Bloodborne boss HP: scaled {:#x} for co-op: max {} -> {}",
                         ReadValue<u32>(character, 0x08), max, scaled->first);
            }
        }
        text += fmt::format(
            " [{:#x} entity {} hp {} {} {} {} effects{}]", ReadValue<u32>(character, 0x08),
            entity != entities.end() ? entity->second : -1, ReadValue<s32>(data, 0xF8),
            ReadValue<s32>(data, 0xFC), ReadValue<s32>(data, 0x100), ReadValue<s32>(data, 0x104),
            effects.empty() ? " none" : effects);
    });
    if (text != last) {
        last = text;
        LOG_INFO(Debug, "Bloodborne boss HP:{}", text.empty() ? " none" : text);
    }
}

} // namespace Coop

namespace {

// The character's entry in WorldChrMan +0xA40's per-block table ([+0x10 + block * 8], words at
// [+0x18], block = [+0x90] bits 11..15, slot = bits 0..10): the controller update 0x018E6540
// gives the character its AI controller (+0x3B8) when the word is >= 0 and the network one
// (+0x3C0) when it is negative.
u64 OwnershipWordAddress(u64 world, u64 character) {
    const s32 index = ReadValue<s32>(character, 0x90);
    const u64 table = ReadValue<u64>(world, 0xA40);
    if (index < 0 || table < 0x10000) {
        return 0;
    }
    const s32 block = (index >> 11) & 0x1F;
    const s32 slot = index & 0x7FF;
    if (block >= ReadValue<s32>(table, 0x08)) {
        return 0;
    }
    const u64 entry = ReadValue<u64>(table, 0x10 + 8 * block);
    if (entry < 0x10000 || slot >= ReadValue<s32>(entry, 0x10)) {
        return 0;
    }
    const u64 words = ReadValue<u64>(entry, 0x18);
    return words >= 0x10000 ? words + 2 * slot : 0;
}

// The eboot offset of the vtable of the object at [character + offset] (0 when none): tells a
// character's AI-driven parts from network-driven ones.
u64 VtableOffsetAt(u64 character, u64 offset) {
    const u64 object = ReadValue<u64>(character, offset);
    const u64 vtable = object >= 0x10000 ? ReadValue<u64>(object, 0) : 0;
    return vtable >= image_base ? vtable - image_base : 0;
}

} // namespace

namespace Coop {

void LogNearbyEnemyControl() {
    static std::string last;
    static std::chrono::steady_clock::time_point checked{};
    const auto now = std::chrono::steady_clock::now();
    if (now - checked < std::chrono::seconds{2}) {
        return;
    }
    checked = now;
    const u64 world = ReadValue<u64>(image_base + CandidateLocalStatePointerOffset, 0);
    const auto me = LocalPlayerPosition();
    if (world < 0x10000 || !me) {
        return;
    }
    std::vector<std::pair<float, u64>> nearby;
    ForEachWorldCharacter(world, [&](u64 character) {
        if (character == GetLocalPlayer() || CharacterHp(character) <= 0) {
            return;
        }
        const u64 modules = ReadValue<u64>(character, 0x3B0);
        const u64 physics = modules >= 0x10000 ? ReadValue<u64>(modules, 0x68) : 0;
        if (physics < 0x10000) {
            return;
        }
        const float dx = ReadValue<float>(physics, 0x1E0) - (*me)[0];
        const float dy = ReadValue<float>(physics, 0x1E4) - (*me)[1];
        const float dz = ReadValue<float>(physics, 0x1E8) - (*me)[2];
        const float distance = std::sqrt(dx * dx + dy * dy + dz * dz);
        if (distance < 40.0f) {
            nearby.emplace_back(distance, character);
        }
    });
    std::ranges::sort(nearby);
    // SHADPS4_BLOODBORNE_TRACE_ENEMY_HP=1: a hardware write watch on the nearest enemy's HP
    // within 6 m, to find the code that applies its damage (and why a guest's never does).
    // =ctrl instead watches the nearest enemy's wake-up target ([+0x18]+0x14: 4 active, 0
    // asleep), set by WorldChrMan's activation pass (0x019192A0), for a guest alone whose
    // enemies stay asleep.
    static const char* trace_enemy = std::getenv("SHADPS4_BLOODBORNE_TRACE_ENEMY_HP");
    if (trace_enemy != nullptr && !nearby.empty() && nearby.front().first < 6.0f) {
        const u64 character = nearby.front().second;
        const u64 modules = ReadValue<u64>(character, 0x3B0);
        const u64 data = modules >= 0x10000 ? ReadValue<u64>(modules, 0x20) : 0;
        const u64 control = ReadValue<u64>(character, 0x18);
        if (std::string_view{trace_enemy} == "sleep") {
            // Who puts a lone guest's enemies to sleep (+0x28, the event disable) seconds after it
            // arrives: armed while it is still off.
            if (ReadValue<u8>(character, 0x28) == 0) {
                WatchEnemyHp(character + 0x28, ReadValue<u32>(character, 0x08));
            }
        } else if (std::string_view{trace_enemy} == "ctrl") {
            if (control >= 0x10000) {
                WatchEnemyHp(control + 0x14, ReadValue<u32>(character, 0x08));
            }
        } else if (data >= 0x10000) {
            WatchEnemyHp(data + 0xF8, ReadValue<u32>(character, 0x08));
        }
    }
    std::string text;
    for (size_t index = 0; index < std::min<size_t>(nearby.size(), 4); ++index) {
        const u64 character = nearby[index].second;
        const u64 control = ReadValue<u64>(character, 0x18);
        std::string words;
        for (u64 offset = 0; offset < 0x40 && control >= 0x10000; offset += 4) {
            words += fmt::format(" {:x}", ReadValue<u32>(control, offset));
        }
        text += fmt::format(
            " [{:#x} type {} b28 {} b29 {} b359 {:#x} auth {:#x} flags {:#x} vt58 {:#x} "
            "vt60 {:#x} vt68 {:#x} own {:#x} effects{} ctrl{}]",
            ReadValue<u32>(character, 0x08), ReadValue<s32>(character, 0x78),
            ReadValue<u8>(character, 0x28), ReadValue<u8>(character, 0x29),
            ReadValue<u8>(character, 0x359), ReadValue<s32>(character, 0x1EC),
            ReadValue<u64>(character, 0x1E0), VtableOffsetAt(character, 0x58),
            VtableOffsetAt(character, 0x60), VtableOffsetAt(character, 0x68),
            ReadValue<u16>(OwnershipWordAddress(world, character), 0),
            [&] {
                // The enemy holding a player's lost echoes (red eyes) is marked somehow; its
                // effects are the first suspect.
                std::string list;
                for (const s32 effect : EffectsOf(character)) {
                    list += fmt::format(" {}", effect);
                }
                return list.empty() ? std::string{" none"} : list;
            }(),
            words);
    }
    if (text != last) {
        last = text;
        LOG_INFO(Debug, "Bloodborne nearby enemies (map {:#x}):{}", CurrentPlace().map,
                 text.empty() ? " none" : text);
    }
}

// When the map's world last changed hands (a bulk flag change, i.e. a load that kept the map's
// objects; a guest starting or ending its run of the map's enemies): see CharacterSettles.
std::atomic<s64> world_changed_ms{-1'000'000'000};
std::atomic<s64> world_loaded_ms{-1'000'000'000};

} // namespace Coop

namespace {

// A guest alone on its map (see NoOneElseOnMap): the host has none of this map's enemies, so
// they stood still and took no deaths. The controller update 0x018E6540 runs a character on its
// network controller while its ownership word (OwnershipWordAddress) is negative, which it is
// on a client. Experiment: a guest alone sets the word of the characters around it to 0, so they
// get their AI controller, and puts the old words back once another member is on its map.
std::unordered_map<u64, u16> lone_guest_ownership; // word address -> original value
std::unordered_map<u64, u64> lone_guest_character; // word address -> its character

} // namespace

namespace Coop {

// In a party session with another member on another map (the party's map board, fresh): the
// game shares a kill's echoes only with the members on the killer's map, and one of those posts
// them (PostsKillEchoes).
bool PostsKillEchoesInSession() {
    const u32 map = PlayingMap();
    if (!InPartySession() || !IsUsablePackedMap(map)) {
        return false;
    }
    std::scoped_lock lock{map_board_mutex};
    return std::chrono::steady_clock::now() - map_board_time < std::chrono::seconds{3} &&
           PostsKillEchoes(map_board, map_board_member, map);
}

} // namespace Coop

namespace {

// In a party session with the host away from this map (its own map state post, the party's map
// board fresh); without the host's post yet, with no other member on this map.
bool HostAwayFromMyMap() {
    const u32 map = PlayingMap();
    if (!InPartySession() || !IsUsablePackedMap(map)) {
        return false;
    }
    const std::optional<u32> host_map = HostMapFromItsPost();
    std::scoped_lock lock{map_board_mutex};
    if (std::chrono::steady_clock::now() - map_board_time >= std::chrono::seconds{3}) {
        return false;
    }
    return HostAway(host_map, map_board, map_board_member, map);
}

} // namespace

namespace Coop {

bool HostAwayFromThisMap() {
    return HostAwayFromMyMap();
}

} // namespace Coop

namespace Coop {

// Adds the echoes the other members got from kills made apart (see SharesKillEchoes) through
// the player's AddSouls (vtable +0x3D0, the call the kill reward makes, with its rates).
void GrantPartyEchoes() {
    const u64 player = GetLocalPlayer();
    if (!HasVirtual(player, 0x3D0, AddSoulsOffset, "GrantPartyEchoes AddSouls")) {
        return;
    }
    const u64 add_souls = image_base + AddSoulsOffset;
    std::vector<KillEchoes> pending;
    {
        // Those the game paid this member itself are left out (see KillEchoLedger).
        std::scoped_lock lock{echo_board_mutex};
        pending = echo_ledger.Due(SteadyMilliseconds());
    }
    using AddSouls = void PS4_SYSV_ABI (*)(u64 player, s32 amount, s32, bool rates);
    for (const KillEchoes& kill : pending) {
        reinterpret_cast<AddSouls>(add_souls)(player, kill.amount, 0, true);
        LOG_INFO(Debug, "Bloodborne party: added {} echoes from a kill another member made apart",
                 kill.amount);
    }
}

} // namespace Coop

namespace {

// When this member took each character over or handed it back (see EnemyOwnerChangeSettles);
// the game thread only.
std::unordered_map<u64, s64> enemy_owner_changed_ms;

} // namespace

namespace Coop {

void ClaimEnemiesWhenAlone(bool in_session, u32 map) {
    static ClaimedWorld claimed_world;
    if (claimed_world.NewWorld(map, GetLocalPlayer())) {
        if (!lone_guest_ownership.empty()) {
            LOG_INFO(Debug, "Bloodborne party: {} enemy claims ended with their world",
                     lone_guest_ownership.size());
        }
        lone_guest_ownership.clear(); // the old world's characters are gone
        lone_guest_character.clear();
        enemy_owner_changed_ms.clear();
    }
    // Not only alone: two guests on a map the host is not on both run its enemies, each in its
    // own copy of the map (the kill and HP boards keep them in step).
    const bool alone = in_session && IsPartyGuestRole() && HostAwayFromMyMap();
    // Starting or ending its run of the map's enemies, the guest's own world settles on them (the
    // rig's dog test: Father Gascoigne, dead in the guest's save, went down when the host left).
    static bool was_alone = false;
    if (alone != was_alone) {
        was_alone = alone;
        world_changed_ms.store(SteadyMilliseconds());
    }
    if (!alone) {
        // Only the words of characters this world still has (memory freed with a world reads as
        // writable all the same).
        std::unordered_set<u64> live_words;
        if (!lone_guest_ownership.empty()) {
            const u64 world = ReadValue<u64>(image_base + CandidateLocalStatePointerOffset, 0);
            if (world >= 0x10000) {
                ForEachWorldCharacter(world, [&](u64 character) {
                    live_words.insert(OwnershipWordAddress(world, character));
                });
            }
        }
        size_t handed_back = 0;
        for (const auto& [word, original] : lone_guest_ownership) {
            if (live_words.contains(word) && HasMemoryAccess(word, 2, MemoryProt::CpuWrite) &&
                ReadValue<u16>(word, 0) == 0) {
                WriteValue(word, 0, original);
                ++handed_back;
            }
            if (const auto character = lone_guest_character.find(word);
                character != lone_guest_character.end()) {
                enemy_owner_changed_ms[character->second] = SteadyMilliseconds();
            }
        }
        if (!lone_guest_ownership.empty()) {
            LOG_INFO(Debug, "Bloodborne party: guest no longer alone; handed back {} enemies",
                     handed_back);
            lone_guest_ownership.clear();
            lone_guest_character.clear();
        }
        return;
    }
    const u64 world = ReadValue<u64>(image_base + CandidateLocalStatePointerOffset, 0);
    const u64 player = GetLocalPlayer();
    if (world < 0x10000) {
        return;
    }
    size_t claimed = 0;
    const u64 doll = DollCharacter();
    ForEachWorldCharacter(world, [&](u64 character) {
        // Handed back when another member comes, the Doll's level-up would stop (see
        // ChangesHandsWithTheMap).
        if (character == player || character == doll || CharacterHp(character) <= 0) {
            return;
        }
        const u64 word = OwnershipWordAddress(world, character);
        if (word == 0 || !HasMemoryAccess(word, 2, MemoryProt::CpuWrite)) {
            return;
        }
        const u16 value = ReadValue<u16>(word, 0);
        if (static_cast<s16>(value) >= 0) {
            return;
        }
        lone_guest_ownership.try_emplace(word, value);
        lone_guest_character[word] = character;
        enemy_owner_changed_ms[character] = SteadyMilliseconds();
        WriteValue(word, 0, u16{0});
        ++claimed;
    });
    static u32 logged = 0;
    if (claimed > 0 && ++logged <= 30) {
        LOG_INFO(Debug, "Bloodborne party: guest alone on map {:#x} took over {} enemies", map,
                 claimed);
    }
}

} // namespace Coop

namespace {

// Dead in this game's world (IsDeadInItsWorld): its death flags, or 0 HP of a known max (an
// enemy dead in the save loads so).
bool DeadInItsWorld(u64 character) {
    const u64 modules = ReadValue<u64>(character, 0x3B0);
    const u64 data = modules >= 0x10000 ? ReadValue<u64>(modules, 0x20) : 0;
    return IsDeadInItsWorld(ReadValue<u8>(character, 0x1E3),
                            data >= 0x10000 ? ReadValue<s32>(data, 0xF8) : 1,
                            data >= 0x10000 ? ReadValue<s32>(data, 0xFC) : 0);
}

// A character the game keeps out of the world for now (its event's +0x28 / +0x29, or disabled):
// an NPC behind a door, a boss before its fight.
bool KeptOutByTheGame(u64 character) {
    return ReadValue<u8>(character, 0x28) != 0 || ReadValue<u8>(character, 0x29) != 0 ||
           IsCharacterDisabled(character);
}

// Phase 4's enemy owners (see EnemyRole): this member's role and the owner it follows, as the last
// party tick found them, for the hooks; when it last became its area's owner (its copies then
// take the log's HP, see ShareEnemyHp); the frames and sends counted for the owner's rates.
std::atomic<u8> enemy_role{static_cast<u8>(EnemyRole::Unknown)};
std::mutex enemy_owner_mutex;
std::string enemy_owner_member;
std::atomic<s64> became_owner_ms{-1'000'000'000};
std::atomic<u32> enemy_frames{0};
std::array<std::atomic<u32>, NativeChannelCount> enemy_sent{};
std::atomic<u64> enemy_frame_serial{0};
// A follower's positions taken (ch 4) by the packets' id (chr +0x90) since its last rates line.
std::mutex positions_taken_mutex;
std::map<u32, u32> positions_taken;
std::atomic<u32> position_packets_in{0}; // ch 4 packets the game's queues took
std::atomic<u32> position_entries_read{0};
// The game's hand-out (0x17, see HandOutSeconds): the owner's latest to each member, a
// follower's latest from the owner, with when.
struct HandOut {
    std::vector<u32> ids;
    s64 at_ms = 0;
};
std::mutex hand_out_mutex;
std::map<std::string, HandOut> handed_by_me;
HandOut handed_to_me;

// Kaniel 2026-10-28 (passive bosses): the bosses BossRunnerOf saw lately, by +0x90 index, with
// their pick's runner and when; the game's hand-out names a boss only to that runner
// (BossesKeptFrom).
constexpr s64 BossIndexKeepMs = 10'000;
std::mutex boss_indices_mutex;
std::map<u32, std::pair<std::string, s64>> boss_indices;

void NoteBossIndex(u32 index, const std::string& runner, s64 now_ms) {
    std::scoped_lock lock{boss_indices_mutex};
    boss_indices[index] = {runner, now_ms};
}

std::vector<BossHand> RecentBosses(s64 now_ms) {
    std::scoped_lock lock{boss_indices_mutex};
    std::erase_if(boss_indices, [&](const auto& entry) {
        return now_ms - entry.second.second > BossIndexKeepMs;
    });
    std::vector<BossHand> bosses;
    for (const auto& [index, seen] : boss_indices) {
        bosses.push_back(BossHand{.index = index, .runner = seen.first});
    }
    return bosses;
}

// The enemies this game handed out (owner) or was handed (follower), from fresh hand-outs only.
std::vector<u32> FreshHandOut(EnemyRole role, s64 now_ms) {
    std::scoped_lock lock{hand_out_mutex};
    std::vector<u32> ids;
    if (role == EnemyRole::Owner) {
        for (const auto& [member, hand_out] : handed_by_me) {
            if (now_ms - hand_out.at_ms < HandOutFreshMs) {
                ids.insert(ids.end(), hand_out.ids.begin(), hand_out.ids.end());
            }
        }
    } else if (role == EnemyRole::Follower && now_ms - handed_to_me.at_ms < HandOutFreshMs) {
        ids = handed_to_me.ids;
    }
    return ids;
}

} // namespace

namespace Coop {

EnemyRole CurrentEnemyRole() {
    return static_cast<EnemyRole>(enemy_role.load(std::memory_order_relaxed));
}

std::string CurrentEnemyOwner() {
    std::scoped_lock lock{enemy_owner_mutex};
    return enemy_owner_member;
}

u64 EnemyFrameSerial() {
    return enemy_frame_serial.load(std::memory_order_relaxed);
}

void PS4_SYSV_ABI CountPositionTaken(u64, const GuestRegisterSnapshot* registers) {
    if (CurrentEnemyRole() != EnemyRole::Follower ||
        !HasMemoryAccess(registers->r12 - 4, 4, MemoryProt::CpuRead)) {
        return;
    }
    const u32 id = ReadValue<u32>(registers->r12 - 4, 0);
    position_entries_read.fetch_add(1, std::memory_order_relaxed);
    std::scoped_lock lock{positions_taken_mutex};
    ++positions_taken[id];
}

// A hand-out's ids on a change (which enemies, by the packets' ids).
void LogHandOut(const std::string& who, const std::vector<u32>& ids) {
    static std::map<std::string, std::vector<u32>> last;
    static u32 logged = 0;
    std::string text;
    for (const u32 id : ids) {
        text += fmt::format(" {:x}", id);
    }
    static std::mutex mutex;
    std::scoped_lock lock{mutex};
    if (last[who] != ids && logged < 120) {
        last[who] = ids;
        ++logged;
        LOG_INFO(Debug, "Bloodborne party: enemy hand-out {}:{}", who,
                 text.empty() ? " none" : text);
    }
}

void NoteHandOutSent(const std::string& member, std::span<u8> payload, bool writable) {
    const s64 now_ms = SteadyMilliseconds();
    u32 count = 0;
    if (payload.size() >= 4) {
        std::memcpy(&count, payload.data(), 4);
    }
    if (writable && count <= 0x800 && payload.size() == 4 + size_t{count} * 4) {
        std::vector<u32> entries(count);
        std::memcpy(entries.data(), payload.data() + 4, size_t{count} * 4);
        static HandOutSettle settle;
        const std::vector<BossHand> picks = RecentBosses(now_ms);
        const std::string board_member = BoardMemberOfOnlineId(member);
        const std::vector<u32> bosses = BossesKeptFrom(picks, board_member);
        const std::vector<u32> given = BossesGivenTo(picks, board_member);
        const std::vector<u32> settled =
            GiveBosses(settle.Filter(member, entries, now_ms, bosses), given);
        // The bosses the game would hand to another member than the pick's (kept here), and
        // those the pick gives this one (each change).
        const auto indices = [](std::span<const u32> from, std::span<const u32> among) {
            std::string which;
            for (const u32 entry : from) {
                if (entry != HandOutHeldEntry && std::ranges::contains(among, entry & 0xFFFF)) {
                    which += fmt::format(" {:x}", entry & 0xFFFF);
                }
            }
            return which.empty() ? std::string{" none"} : which;
        };
        const std::string bosses_text =
            fmt::format("kept{}, given{}", indices(entries, bosses), indices(settled, given));
        static std::map<std::string, std::string> bosses_last;
        static u32 bosses_logged = 0;
        if (bosses_last[member] != bosses_text && bosses_logged < 200) {
            bosses_last[member] = bosses_text;
            ++bosses_logged;
            LOG_INFO(Debug,
                     "Bloodborne enemy owner: bosses in the hand-out to {} by the boss "
                     "pick: {} (t={:.1f})",
                     member, bosses_text, now_ms / 1000.0);
        }
        std::memcpy(payload.data() + 4, settled.data(), size_t{count} * 4);
        const auto held = static_cast<u32>(std::ranges::count(settled, HandOutHeldEntry));
        static std::map<std::string, u32> held_last;
        static u32 held_logged = 0;
        if (held_last[member] != held && held_logged < 400) {
            held_last[member] = held;
            ++held_logged;
            LOG_INFO(Debug,
                     "Bloodborne enemy owner: {} of {} enemies for {} held here until their "
                     "hand-out settles (t={:.1f})",
                     held, count, member, now_ms / 1000.0);
        }
    }
    std::vector<u32> ids = HandOutIds(payload);
    LogHandOut("to " + member, ids);
    std::scoped_lock lock{hand_out_mutex};
    handed_by_me[member] = {std::move(ids), now_ms};
}

void PS4_SYSV_ABI TakeHandOut(u64, const GuestRegisterSnapshot* registers) {
    const u32 size = static_cast<u32>(registers->rax);
    const u64 buffer = image_base + HandOutBufferOffset;
    if (CurrentEnemyRole() != EnemyRole::Follower || size < 4 || size > 0x2004 ||
        !HasMemoryAccess(buffer, size, MemoryProt::CpuRead)) {
        return;
    }
    const std::span<const u8> payload{reinterpret_cast<const u8*>(buffer), size};
    const s64 now_ms = SteadyMilliseconds();
    std::vector<u32> ids = HandOutIds(payload);
    LogHandOut("from the owner", ids);
    std::scoped_lock lock{hand_out_mutex};
    handed_to_me = {std::move(ids), now_ms};
}

void CountPositionPacketIn() {
    position_packets_in.fetch_add(1, std::memory_order_relaxed);
}

void CountEnemyPacketSent(u32 channel) {
    if (channel < enemy_sent.size()) {
        enemy_sent[channel].fetch_add(1, std::memory_order_relaxed);
    }
}

void HoldOwnHits();

} // namespace Coop

namespace {

// Phase 8: the block records the owner's activation pass saw with the other players' groups
// added (see WithOthersGroups), and their own groups, put back after the pass; the game thread.
struct WokenRecord {
    u64 record = 0;
    WakeGroups own;
};
std::vector<WokenRecord> woken_records;
std::atomic<u32> woken_for{0}; // how many other players' groups the last pass added

WakeGroups ReadWakeGroups(u64 at, u64 wakes, u64 counted, u64 half) {
    WakeGroups groups;
    for (size_t i = 0; i < groups.wakes.size(); ++i) {
        groups.wakes[i] = ReadValue<u32>(at, wakes + i * 4);
        groups.counted[i] = ReadValue<u32>(at, counted + i * 4);
        groups.half[i] = ReadValue<u32>(at, half + i * 4);
    }
    return groups;
}

void WriteRecordGroups(u64 record, const WakeGroups& groups) {
    for (size_t i = 0; i < groups.wakes.size(); ++i) {
        WriteValue(record, 0x38 + i * 4, groups.wakes[i]);
        WriteValue(record, 0x18 + i * 4, groups.counted[i]);
        WriteValue(record, 0x78 + i * 4, groups.half[i]);
    }
}

} // namespace

namespace Coop {

void PS4_SYSV_ABI WakeAroundOthers(u64, const GuestRegisterSnapshot* registers) {
    woken_records.clear();
    if (!WakesAroundOthers(CurrentEnemyRole(), !IsPartyGuestRole())) {
        woken_for.store(0, std::memory_order_relaxed);
        return;
    }
    const u64 world = registers->rdi;
    const u64 player = GetLocalPlayer();
    if (world < 0x10000 || player < 0x10000 ||
        !HasMemoryAccess(player + 0x218, 0x60, MemoryProt::CpuRead)) {
        return;
    }
    std::vector<WakeGroups> others;
    for (const OtherPlayer& other : OtherPlayers()) {
        // One hidden by the map board (on another map) wakes nothing here.
        if (other.hp > 0 && !IsCharacterDisabled(other.character) &&
            HasMemoryAccess(other.character + 0x218, 0x60, MemoryProt::CpuRead)) {
            others.push_back(ReadWakeGroups(other.character, 0x218, 0x238, 0x258));
        }
    }
    woken_for.store(static_cast<u32>(others.size()), std::memory_order_relaxed);
    if (others.empty()) {
        return;
    }
    const WakeGroups own = ReadWakeGroups(player, 0x218, 0x238, 0x258);
    const s32 blocks = std::min(ReadValue<s32>(world, 0x278), 62);
    for (s32 index = 0; index < blocks; ++index) {
        const u64 record = ReadValue<u64>(world, 0x290 + static_cast<u64>(index) * 0x18);
        if (record < 0x10000 || !HasMemoryAccess(record + 0x18, 0x80, MemoryProt::CpuWrite)) {
            continue;
        }
        const WakeGroups before = ReadWakeGroups(record, 0x38, 0x18, 0x78);
        const WakeGroups after = WithOthersGroups(before, own, others);
        if (after != before) {
            woken_records.push_back({record, before});
            WriteRecordGroups(record, after);
        }
        static u32 logged = 0;
        if (logged < 40 && (after != before || logged < 4)) {
            ++logged;
            LOG_INFO(Debug,
                     "Bloodborne enemy owner: block record {} groups {:x} -> {:x} (own {:x}, "
                     "{} others)",
                     index, before.wakes[0], after.wakes[0], own.wakes[0], others.size());
        }
    }
}

} // namespace Coop

namespace {

// See OwnerSendsPositionOf: the character's place and turn into its slot's position record, marked
// to send, as the game's own net update does every frame.
bool MarkPositionToSend(u64 world, u64 character) {
    const s32 index = ReadValue<s32>(character, 0x90);
    const u64 table = ReadValue<u64>(world, 0xA40);
    const u64 modules = ReadValue<u64>(character, 0x3B0);
    const u64 physics = modules >= 0x10000 ? ReadValue<u64>(modules, 0x68) : 0;
    if (index < 0 || table < 0x10000 || physics < 0x10000) {
        return false;
    }
    const s32 block = (index >> 11) & 0x1F;
    const s32 slot = index & 0x7FF;
    if (block >= ReadValue<s32>(table, 0x08)) {
        return false;
    }
    const u64 entry = ReadValue<u64>(table, 0x10 + 8 * block);
    if (entry < 0x10000 || slot >= ReadValue<s32>(entry, 0x10)) {
        return false;
    }
    const u64 records = ReadValue<u64>(entry, 0x20);
    const u64 flags = ReadValue<u64>(entry, 0x38);
    if (records < 0x10000 || flags < 0x10000) {
        return false;
    }
    const u64 record = records + 0x18 * static_cast<u64>(slot);
    const u64 flag = flags + 2 * static_cast<u64>(slot);
    if (!HasMemoryAccess(record, 0x18, MemoryProt::CpuWrite) ||
        !HasMemoryAccess(flag, 2, MemoryProt::CpuWrite)) {
        return false;
    }
    const std::array<float, 6> now{
        ReadValue<float>(physics, 0x1E0), ReadValue<float>(physics, 0x1E4),
        ReadValue<float>(physics, 0x1E8), ReadValue<float>(physics, 0x1D0),
        ReadValue<float>(physics, 0x1D4), ReadValue<float>(physics, 0x1D8)};
    for (size_t i = 0; i < now.size(); ++i) {
        WriteValue(record, i * 4, now[i]);
    }
    WriteValue(flag, 0, static_cast<u16>(ReadValue<u16>(flag, 0) | 2));
    return true;
}

} // namespace

namespace {

// See BossRunnerPick: whose game runs a boss-sized enemy (by handle), "" for the game's own; the
// game's thread. And the bosses this game runs, for its vitals (OwnBossClaims).
std::map<u32, BossRunnerPick> boss_runners;
std::mutex boss_claims_mutex;
BossClaims boss_claims;
bool IsBossSized(u64 character); // below, with the characters shown like the host

// The character's NpcParam npcType ([vtable +0x168 (character) +8] is its row, see
// GiveMemberKillDrop), if it reads.
std::optional<u8> NpcTypeOf(u64 character) {
    const u64 vtable = ReadValue<u64>(character, 0);
    const u64 npc_of = vtable >= 0x10000 ? ReadValue<u64>(vtable, 0x168) : 0;
    if (npc_of < 0x10000) {
        return std::nullopt;
    }
    const u64 holder = reinterpret_cast<u64 PS4_SYSV_ABI (*)(u64)>(npc_of)(character);
    const u64 npc = holder >= 0x10000 ? ReadValue<u64>(holder, 8) : 0;
    if (npc < 0x10000 || !HasMemoryAccess(npc, NpcTypeOffset + 1, MemoryProt::CpuRead)) {
        return std::nullopt;
    }
    return ReadValue<u8>(npc, NpcTypeOffset);
}

std::string BossRunnerOf(u64 character) {
    if (!HandsOutBoss(NpcTypeOf(character), IsBossSized(character))) {
        return {};
    }
    const auto at = CharacterPosition(character);
    const VitalsView view = ReadVitalsBoard();
    const u32 handle = ReadValue<u32>(character, 0x08);
    const u32 index = ReadValue<u32>(character, 0x90);
    if (!at || !view.Fresh()) {
        NoteBossIndex(index, boss_runners[handle].Last(), SteadyMilliseconds());
        return boss_runners[handle].Last(); // as last decided, not back and forth
    }
    std::string owner;
    if (CurrentEnemyRole() == EnemyRole::Owner) {
        owner = view.member;
    } else {
        std::scoped_lock lock{enemy_owner_mutex};
        owner = enemy_owner_member;
    }
    const std::string runner =
        boss_runners[handle].Tick(PlayingMap(), handle, *at, view.board.members, owner);
    NoteBossIndex(index, runner, SteadyMilliseconds());
    // Ours to run, awake and alive: claimed on the board (see ClaimsBoss).
    const u64 control = ReadValue<u64>(character, 0x18);
    const auto mine = std::ranges::find(view.board.members, view.member, &MemberVitals::member);
    const auto me = CharacterPosition(GetLocalPlayer());
    float distance = std::numeric_limits<float>::max();
    if (me) {
        float sum = 0.0f;
        for (size_t axis = 0; axis < 3; ++axis) {
            sum += ((*me)[axis] - (*at)[axis]) * ((*me)[axis] - (*at)[axis]);
        }
        distance = std::sqrt(sum);
    }
    if (ClaimsBoss(runner, view.member, CurrentEnemyRole() == EnemyRole::Owner,
                   mine != view.board.members.end() && std::ranges::contains(mine->bosses, handle),
                   distance) &&
        control >= 0x10000 && ReadValue<s32>(control, 0x14) == EnemyAwake &&
        CharacterHp(character) > 0) {
        std::scoped_lock lock{boss_claims_mutex};
        boss_claims.Ran(handle, SteadyMilliseconds());
    }
    static std::map<u32, std::string> logged;
    if (auto& said = logged[handle]; said != runner) {
        said = runner;
        LOG_INFO(Debug, "Bloodborne enemy owner: boss {:#x} is run by {}", handle,
                 runner.empty() ? "the game's own runner" : runner);
    }
    return runner;
}

bool IsMe(const std::string& member) {
    return !member.empty() && member == ReadVitalsBoard().member;
}

} // namespace

namespace Coop {

std::vector<u32> OwnBossClaims() {
    std::scoped_lock lock{boss_claims_mutex};
    return boss_claims.Current(SteadyMilliseconds());
}

void PS4_SYSV_ABI KeepAwakeEnemies(u64, const GuestRegisterSnapshot* registers) {
    const EnemyRole role = CurrentEnemyRole();
    const u64 world = registers->rdi;
    if (role == EnemyRole::Unknown || world < 0x10000) {
        return;
    }
    // Whatever this member's role, the Doll stays in this game's hands (KeptLocalWord).
    const u64 doll = DollCharacter();
    if (doll != 0) {
        ForEachWorldCharacter(world, [&](u64 character) {
            const u64 word = character == doll ? OwnershipWordAddress(world, character) : 0;
            if (word == 0 || !HasMemoryAccess(word, 2, MemoryProt::CpuWrite)) {
                return;
            }
            if (const auto kept = KeptLocalWord(role, HuntersDreamDoll, ReadValue<u16>(word, 0))) {
                WriteValue(word, 0, *kept);
            }
        });
    }
    if (role == EnemyRole::Unknown || (role == EnemyRole::Follower && IsPartyGuestRole())) {
        return;
    }
    // Never a player: the other players' copies follow their own games (an AI controller on one
    // broke its copy: the full rig tier 2026-10-15, players unseen after trips).
    std::unordered_set<u64> players{GetLocalPlayer()};
    for (const OtherPlayer& other : OtherPlayers()) {
        players.insert(other.character);
    }
    if (role == EnemyRole::Follower) {
        // The host follows the map's owner until it takes the map over (HostFollowerWord).
        u32 let_go = 0;
        ForEachWorldCharacter(world, [&](u64 character) {
            const u64 word = OwnershipWordAddress(world, character);
            if (players.contains(character) || character == doll || word == 0 ||
                !HasMemoryAccess(word, 2, MemoryProt::CpuWrite) || IsMe(BossRunnerOf(character))) {
                return;
            }
            if (const auto wanted = HostFollowerWord(role, true, ReadValue<u16>(word, 0))) {
                WriteValue(word, 0, *wanted);
                ++let_go;
            }
        });
        static u32 last_let_go = 0;
        static u32 let_go_logged = 0;
        if (let_go != last_let_go && let_go_logged < 60) {
            last_let_go = let_go;
            ++let_go_logged;
            LOG_INFO(Debug,
                     "Bloodborne enemy owner: this host follows the map's owner; let its game's "
                     "{} enemies go (t={:.1f})",
                     let_go, SteadyMilliseconds() / 1000.0);
        }
        return;
    }
    u32 kept = 0;
    u32 marked = 0;
    const std::vector<u32> handed = FreshHandOut(role, SteadyMilliseconds());
    ForEachWorldCharacter(world, [&](u64 character) {
        if (players.contains(character) || character == doll) {
            return;
        }
        const u64 control = ReadValue<u64>(character, 0x18);
        const u64 word = OwnershipWordAddress(world, character);
        if (control < 0x10000 || word == 0 || !HasMemoryAccess(word, 2, MemoryProt::CpuWrite)) {
            return;
        }
        const s32 level = ReadValue<s32>(control, 0x14);
        const s32 applied = ReadValue<s32>(control, 0x10);
        const bool kept_out = KeptOutByTheGame(character);
        // Kaniel 2026-10-16: a boss another member's game runs (BossRunnerPick) is followed here.
        if (level == EnemyAwake && !kept_out) {
            if (const std::string runner = BossRunnerOf(character);
                !runner.empty() && !IsMe(runner)) {
                if (static_cast<s16>(ReadValue<u16>(word, 0)) >= 0) {
                    WriteValue(word, 0, u16{0xFFFF});
                }
                return;
            }
        }
        if (const auto wanted =
                AwakeEnemyWord(role, level, applied, ReadValue<u16>(word, 0), kept_out,
                               HandedOut(handed, ReadValue<u32>(character, 0x90)))) {
            WriteValue(word, 0, *wanted);
            ++kept;
        }
        if (OwnerSendsPositionOf(role, level, applied, ReadValue<u16>(word, 0), kept_out) &&
            MarkPositionToSend(world, character)) {
            ++marked;
        }
    });
    static u32 marked_logged = 0;
    if (marked > 0 && marked_logged < 3) {
        ++marked_logged;
        LOG_INFO(Debug, "Bloodborne enemy owner: marked {} enemies to send (t={:.1f})", marked,
                 SteadyMilliseconds() / 1000.0);
    }
    static size_t handed_last = 0;
    static u32 handed_logged = 0;
    if (handed.size() != handed_last && handed_logged < 60) {
        handed_last = handed.size();
        ++handed_logged;
        LOG_INFO(Debug,
                 "Bloodborne enemy owner: the game handed {} enemies to the others (t={:.1f})",
                 handed.size(), SteadyMilliseconds() / 1000.0);
    }
    static u32 last = 0;
    static u32 logged = 0;
    // 400: it changes often, and 60 ran out before a long suite did
    // (rig 2026-10-20, together's apart-fight found no line).
    if (kept != last && logged < 400) {
        last = kept;
        ++logged;
        LOG_INFO(Debug, "Bloodborne enemy owner: runs {} awake enemies the game let go (t={:.1f})",
                 kept, SteadyMilliseconds() / 1000.0);
    }
}

void PS4_SYSV_ABI PutBackWokenRecords(u64, const GuestRegisterSnapshot* registers) {
    // How many the pass woke with the others' groups (+0x5F74), on a change.
    static std::pair<size_t, s32> last{};
    static u32 logged = 0;
    const std::pair<size_t, s32> now{woken_records.size(), ReadValue<s32>(registers->r14, 0x5F74)};
    if (now != last && logged < 60) {
        last = now;
        ++logged;
        LOG_INFO(Debug,
                 "Bloodborne enemy owner: the activation pass woke {} with the others' groups "
                 "on {} block(s) (t={:.1f})",
                 now.second, now.first, SteadyMilliseconds() / 1000.0);
    }
    for (const WokenRecord& woken : woken_records) {
        if (HasMemoryAccess(woken.record + 0x18, 0x80, MemoryProt::CpuWrite)) {
            WriteRecordGroups(woken.record, woken.own);
        }
    }
    woken_records.clear();
}

// Phase 8 RE: which groups wake a block's enemies. WorldChrMan's activation pass (0x019192A0)
// wakes a character whose groups (+0x218, +0x238, +0x258) share bits with its block record's
// (WorldChrMan +0x290 entries: +0x38, +0x18, +0x78). Logged on change: each block's record sets,
// this player's own sets and the other players' (their copies in this game), and the budget.
void LogWakeGroups() {
    static s64 checked_ms = 0;
    static std::string last;
    static u32 logged = 0;
    const s64 now_ms = SteadyMilliseconds();
    if (logged >= 600 || now_ms - checked_ms < 2'000) {
        return;
    }
    checked_ms = now_ms;
    const u64 world = ReadValue<u64>(image_base + CandidateLocalStatePointerOffset, 0);
    if (world < 0x10000 || !HasMemoryAccess(world + 0x5F70, 0x20, MemoryProt::CpuRead)) {
        return;
    }
    const auto set = [](u64 at) {
        std::string words;
        for (u64 offset = 0; offset < 0x20; offset += 4) {
            words += fmt::format("{:x}{}", ReadValue<u32>(at, offset), offset < 0x1C ? "." : "");
        }
        return words;
    };
    const auto sets = [&](u64 character) {
        if (character < 0x10000 || !HasMemoryAccess(character + 0x218, 0x60, MemoryProt::CpuRead)) {
            return std::string{"?"};
        }
        return fmt::format("b {} a {} d {}", set(character + 0x218), set(character + 0x238),
                           set(character + 0x258));
    };
    size_t awake_let_go = 0;
    ForEachWorldCharacter(world, [&](u64 character) {
        const u64 control = ReadValue<u64>(character, 0x18);
        const u64 word = OwnershipWordAddress(world, character);
        if (control >= 0x10000 && word != 0 && ReadValue<s32>(control, 0x14) == EnemyAwake &&
            static_cast<s16>(ReadValue<u16>(word, 0)) < 0) {
            ++awake_let_go;
        }
    });
    std::string text = fmt::format(
        "budget {} awake {} counted {} {} (awake let go {}, others' groups {})",
        ReadValue<s32>(world, 0x5F70), ReadValue<s32>(world, 0x5F74), ReadValue<s32>(world, 0x5F88),
        ReadValue<s32>(world, 0x5F8C), awake_let_go, woken_for.load(std::memory_order_relaxed));
    const s32 blocks = std::min(ReadValue<s32>(world, 0x278), 16);
    for (s32 index = 0; index < blocks; ++index) {
        const u64 entry = world + 0x280 + static_cast<u64>(index) * 0x18;
        const u64 record = ReadValue<u64>(entry, 0x10);
        if (record < 0x10000 || !HasMemoryAccess(record + 0x18, 0x80, MemoryProt::CpuRead)) {
            text += fmt::format(" | block {:#x} no record", ReadValue<s32>(entry, 0x08));
            continue;
        }
        text += fmt::format(" | block {:#x} b {} a {} d {}", ReadValue<s32>(entry, 0x08),
                            set(record + 0x38), set(record + 0x18), set(record + 0x78));
    }
    // The characters by each other player, as this game has them (why one sleeps).
    for (const OtherPlayer& other : OtherPlayers()) {
        if (!other.at) {
            continue;
        }
        std::string by_them;
        u32 listed = 0;
        ForEachWorldCharacter(world, [&](u64 character) {
            const u64 modules = ReadValue<u64>(character, 0x3B0);
            const u64 physics = modules >= 0x10000 ? ReadValue<u64>(modules, 0x68) : 0;
            const u64 control = ReadValue<u64>(character, 0x18);
            if (listed >= 6 || physics < 0x10000 || control < 0x10000 ||
                character == other.character || CharacterHp(character) <= 0) {
                return;
            }
            const float dx = ReadValue<float>(physics, 0x1E0) - (*other.at)[0];
            const float dz = ReadValue<float>(physics, 0x1E8) - (*other.at)[2];
            if (dx * dx + dz * dz > 25.0f * 25.0f) {
                return;
            }
            ++listed;
            const u64 word = OwnershipWordAddress(world, character);
            by_them += fmt::format(
                " {:x}:l{}/{} h{}{} w{:x} b{:x} at {:.0f},{:.0f}", ReadValue<u32>(character, 0x08),
                ReadValue<s32>(control, 0x14), ReadValue<s32>(control, 0x10),
                ReadValue<u8>(character, 0x28), ReadValue<u8>(character, 0x29),
                word != 0 ? ReadValue<u16>(word, 0) : 0xBAD, ReadValue<u32>(character, 0x218),
                ReadValue<float>(physics, 0x1E0), ReadValue<float>(physics, 0x1E8));
        });
        text += fmt::format(" | by {:x}:{}", ReadValue<u32>(other.character, 0x08),
                            by_them.empty() ? " none" : by_them);
    }
    text += fmt::format(" | me {}", sets(GetLocalPlayer()));
    for (const OtherPlayer& other : OtherPlayers()) {
        text += fmt::format(" | player {:#x} {}", ReadValue<u32>(other.character, 0x08),
                            sets(other.character));
    }
    if (text != last) {
        last = text;
        ++logged;
        LOG_INFO(Debug, "Bloodborne wake groups: {} (t={:.1f})", text, now_ms / 1000.0);
    }
}

// The enemies within 25 m of this player (alive, not a player), {chr id, +0x90 index}: a
// follower's measure.
std::vector<std::pair<u32, u32>> EnemiesByMe() {
    std::vector<std::pair<u32, u32>> nearby;
    const u64 world = ReadValue<u64>(image_base + CandidateLocalStatePointerOffset, 0);
    const u64 player = GetLocalPlayer();
    const auto place = [](u64 character) -> std::optional<std::array<float, 2>> {
        const u64 modules = ReadValue<u64>(character, 0x3B0);
        const u64 physics = modules >= 0x10000 ? ReadValue<u64>(modules, 0x68) : 0;
        if (physics < 0x10000) {
            return std::nullopt;
        }
        return std::array<float, 2>{ReadValue<float>(physics, 0x1E0),
                                    ReadValue<float>(physics, 0x1E8)};
    };
    const auto me = world >= 0x10000 && player >= 0x10000 ? place(player) : std::nullopt;
    if (!me) {
        return nearby;
    }
    std::unordered_set<u64> players{player};
    for (const OtherPlayer& other : OtherPlayers()) {
        players.insert(other.character);
    }
    ForEachWorldCharacter(world, [&](u64 character) {
        const auto at =
            nearby.size() < 8 && !players.contains(character) && CharacterHp(character) > 0
                ? place(character)
                : std::nullopt;
        if (at && ((*at)[0] - (*me)[0]) * ((*at)[0] - (*me)[0]) +
                          ((*at)[1] - (*me)[1]) * ((*at)[1] - (*me)[1]) <=
                      25.0f * 25.0f) {
            nearby.emplace_back(ReadValue<u32>(character, 0x08), ReadValue<u32>(character, 0x90));
        }
    });
    return nearby;
}

// See EnemyJumped: each frame, the enemies within 25 m of this player against the last frame.
void WatchEnemyJumps(EnemyRole role) {
    static std::unordered_map<u64, std::array<float, 3>> last;
    static std::unordered_map<u64, bool> ran_here; // which game ran each enemy last frame
    static u32 logged = 0;
    static u32 hands_logged = 0;
    const u64 world = ReadValue<u64>(image_base + CandidateLocalStatePointerOffset, 0);
    const u64 player = GetLocalPlayer();
    const auto place = [](u64 character) -> std::optional<std::array<float, 3>> {
        const u64 modules = ReadValue<u64>(character, 0x3B0);
        const u64 physics = modules >= 0x10000 ? ReadValue<u64>(modules, 0x68) : 0;
        if (physics < 0x10000) {
            return std::nullopt;
        }
        return std::array<float, 3>{ReadValue<float>(physics, 0x1E0),
                                    ReadValue<float>(physics, 0x1E4),
                                    ReadValue<float>(physics, 0x1E8)};
    };
    const auto me = role != EnemyRole::Unknown && world >= 0x10000 && player >= 0x10000
                        ? place(player)
                        : std::nullopt;
    if (!me) {
        last.clear();
        return;
    }
    std::unordered_set<u64> players{player};
    for (const OtherPlayer& other : OtherPlayers()) {
        players.insert(other.character);
    }
    std::unordered_map<u64, std::array<float, 3>> now;
    ForEachWorldCharacter(world, [&](u64 character) {
        if (players.contains(character) || CharacterHp(character) <= 0) {
            return;
        }
        const auto at = place(character);
        if (!at || ((*at)[0] - (*me)[0]) * ((*at)[0] - (*me)[0]) +
                           ((*at)[2] - (*me)[2]) * ((*at)[2] - (*me)[2]) >
                       25.0f * 25.0f) {
            return;
        }
        now[character] = *at;
        // An enemy by this player changing hands (its word's sign), each time: a hand-over that
        // goes back and forth shows here.
        const u64 hand_word = OwnershipWordAddress(world, character);
        const bool runs_here =
            hand_word != 0 && static_cast<s16>(ReadValue<u16>(hand_word, 0)) >= 0;
        const auto ran = ran_here.find(character);
        if (ran != ran_here.end() && ran->second != runs_here && hands_logged < 1000) {
            ++hands_logged;
            LOG_INFO(Debug, "Bloodborne party: enemy {:x} ({:x}) changes hands: {} (t={:.1f})",
                     ReadValue<u32>(character, 0x08), ReadValue<u32>(character, 0x90),
                     runs_here ? "runs here" : "follows", SteadyMilliseconds() / 1000.0);
        }
        ran_here[character] = runs_here;
        const auto before = last.find(character);
        if (before != last.end() && EnemyJumped(before->second, *at) && logged < 200) {
            ++logged;
            const u64 word = OwnershipWordAddress(world, character);
            LOG_INFO(Debug,
                     "Bloodborne party: enemy {:x} jumped {:.1f} m in one frame (word {:x}, "
                     "t={:.1f})",
                     ReadValue<u32>(character, 0x08),
                     std::sqrt(((*at)[0] - before->second[0]) * ((*at)[0] - before->second[0]) +
                               ((*at)[1] - before->second[1]) * ((*at)[1] - before->second[1]) +
                               ((*at)[2] - before->second[2]) * ((*at)[2] - before->second[2])),
                     word != 0 ? ReadValue<u16>(word, 0) : 0xBAD, SteadyMilliseconds() / 1000.0);
        }
    });
    last.swap(now);
}

// With an owner: position rounds every 0.1 s and the hand-out every 0.25 s; without, the game's
// own periods (NetWorldChrSync +0x64C, +0x630, +0x638).
void SetSyncPeriods(EnemyRole role) {
    const u64 world = ReadValue<u64>(image_base + CandidateLocalStatePointerOffset, 0);
    const u64 sync = world >= 0x10000 ? ReadValue<u64>(world, 0xA40) : 0;
    if (sync < 0x10000 ||
        !HasMemoryAccess(sync + HandOutPeriodOffset, 0x20, MemoryProt::CpuWrite)) {
        return;
    }
    WriteValue(sync, HandOutPeriodOffset, HandOutSeconds(role));
    WriteValue(sync, NearListPeriodOffset, NearListSeconds(role));
    const float wanted = PositionRoundSeconds(role);
    if (ReadValue<float>(sync, PositionRoundPeriodOffset) != wanted) {
        WriteValue(sync, PositionRoundPeriodOffset, wanted);
        LOG_INFO(Debug, "Bloodborne party: enemy positions go out {}",
                 wanted > 0.0f ? fmt::format("every {:.2f} s", wanted) : "every frame");
    }
}

void TickEnemyOwner(bool in_session, u32 map) {
    HoldOwnHits();
    LogWakeGroups();
    const s64 now_ms = SteadyMilliseconds();
    std::string self;
    {
        std::scoped_lock lock{vitals_mutex};
        self = vitals_member;
    }
    const std::vector<EnemyOwnerEntry> owners =
        in_session ? ServerEnemyOwners(now_ms) : std::vector<EnemyOwnerEntry>{};
    const EnemyRole role = in_session ? EnemyRoleOn(owners, self, map) : EnemyRole::Unknown;
    const EnemyOwnerEntry* owner = EnemyOwnerOf(owners, map);
    const EnemyRole before = CurrentEnemyRole();
    enemy_role.store(static_cast<u8>(role), std::memory_order_relaxed);
    {
        std::scoped_lock lock{enemy_owner_mutex};
        enemy_owner_member = role == EnemyRole::Follower && owner != nullptr ? owner->member : "";
    }
    static EnemyRoleLog role_log;
    if (const auto line = role_log.Change(role, map, owner)) {
        LOG_INFO(Debug, "Bloodborne party: {} (t={:.1f})", *line, now_ms / 1000.0);
    }
    // Taking the area over (or arriving on one this member runs) settles its world: deaths now
    // are no kills, and its copies take the log's HP.
    static u32 owned_area = 0;
    const u32 area = role == EnemyRole::Owner ? EnemyOwnerArea(map) : 0;
    if (area != owned_area) {
        owned_area = area;
        // Only on taking the map over from another: the first owner's world is the enemy (a guest's
        // HP from its own world before it joined killed the dog: Kaniel 2026-10-15).
        if (area != 0 && owner != nullptr && TakesOverEnemyHp(*owner)) {
            became_owner_ms.store(now_ms);
        }
    }
    if (role != before) {
        world_changed_ms.store(now_ms);
    }
    // The owner's frame rate and send rate (its enemies' updates follow its frames).
    static s64 rates_ms = now_ms;
    enemy_frames.fetch_add(1, std::memory_order_relaxed);
    enemy_frame_serial.fetch_add(1, std::memory_order_relaxed);
    SetSyncPeriods(role);
    WatchEnemyJumps(role);
    if (now_ms - rates_ms >= 10'000) {
        const double seconds = (now_ms - rates_ms) / 1000.0;
        rates_ms = now_ms;
        const u32 frames = enemy_frames.exchange(0);
        std::array<u32, NativeChannelCount> sent{};
        for (size_t channel = 0; channel < sent.size(); ++channel) {
            sent[channel] = enemy_sent[channel].exchange(0, std::memory_order_relaxed);
        }
        if (role != EnemyRole::Unknown) {
            LOG_INFO(Debug,
                     "Bloodborne party: enemy {} on map {:#x}: {:.0f} frames/s; sent per s: "
                     "positions {:.1f}, HP {:.1f}, AI records {:.1f}, per-hit {:.1f}, hand-over "
                     "{:.1f}, hits {:.1f}",
                     role == EnemyRole::Owner ? "owner" : "follower", EnemyOwnerArea(map),
                     frames / seconds, sent[0x04] / seconds, sent[0x05] / seconds,
                     sent[0x3D] / seconds, sent[0x41] / seconds, sent[0x34] / seconds,
                     sent[0x15] / seconds);
        }
        std::map<u32, u32> taken;
        {
            std::scoped_lock lock{positions_taken_mutex};
            taken.swap(positions_taken);
        }
        const u32 packets_in = position_packets_in.exchange(0, std::memory_order_relaxed);
        const u32 entries_read = position_entries_read.exchange(0, std::memory_order_relaxed);
        if (role == EnemyRole::Follower) {
            LOG_INFO(Debug,
                     "Bloodborne party: enemy follower on map {:#x}: positions in {:.1f} packets "
                     "and {:.1f} entries read per s; positions taken per s by this player: {}",
                     EnemyOwnerArea(map), packets_in / seconds, entries_read / seconds,
                     PositionRatesText(taken, EnemiesByMe(), seconds));
        }
    }
    const std::optional<u16> wanted = EnemyOwnershipWord(role);
    const u64 world = ReadValue<u64>(image_base + CandidateLocalStatePointerOffset, 0);
    const u64 player = GetLocalPlayer();
    std::unordered_set<u64> players;
    for (const OtherPlayer& other : OtherPlayers()) {
        players.insert(other.character);
    }
    // The session's host runs its map's enemies the game's own way: its game puts its own words
    // back every frame (rig 2026-10-14). Phase 8: no guest runs one at any distance.
    if (!wanted || world < 0x10000 || (role == EnemyRole::Owner && !IsPartyGuestRole())) {
        return;
    }
    size_t changed = 0;
    const std::vector<u32> handed_to_this =
        role == EnemyRole::Follower ? FreshHandOut(role, now_ms) : std::vector<u32>{};
    static size_t handed_last = 0;
    static u32 handed_logged = 0;
    if (role == EnemyRole::Follower && handed_to_this.size() != handed_last && handed_logged < 60) {
        handed_last = handed_to_this.size();
        ++handed_logged;
        LOG_INFO(Debug,
                 "Bloodborne party: enemy follower runs {} enemies the owner handed it (t={:.1f})",
                 handed_to_this.size(), now_ms / 1000.0);
    }
    // The Doll stays this game's (KeepAwakeEnemies): handed over in the middle of a level-up she
    // kept krolebord kneeling (playtest 2026-10-18).
    const u64 doll = DollCharacter();
    ForEachWorldCharacter(world, [&](u64 character) {
        if (character == player || character == doll || players.contains(character) ||
            CharacterHp(character) <= 0) {
            return;
        }
        if (HandedOut(handed_to_this, ReadValue<u32>(character, 0x90))) {
            // The rig 2026-10-28 (passive bosses): a boss the hand-out gives this game (the pick
            // put it there, GiveBosses) is still asked about, so this game claims it on the board
            // (ClaimsBoss); unclaimed, the host walking up took it back and restarted its AI.
            const u64 control = ReadValue<u64>(character, 0x18);
            if (role == EnemyRole::Follower && control >= 0x10000 &&
                ReadValue<s32>(control, 0x14) == EnemyAwake) {
                BossRunnerOf(character);
            }
            return;
        }
        const u64 word = OwnershipWordAddress(world, character);
        if (word == 0 || !HasMemoryAccess(word, 2, MemoryProt::CpuWrite)) {
            return;
        }
        // A word already on the wanted side keeps its value (the game's own >= 0 ones too).
        const bool runs = static_cast<s16>(ReadValue<u16>(word, 0)) >= 0;
        // Kaniel 2026-10-16: a follower runs the boss whose arena it is in (BossRunnerPick).
        const u64 control = ReadValue<u64>(character, 0x18);
        const bool boss_mine = role == EnemyRole::Follower && control >= 0x10000 &&
                               ReadValue<s32>(control, 0x14) == EnemyAwake &&
                               IsMe(BossRunnerOf(character));
        const bool should_run = role == EnemyRole::Owner || boss_mine;
        if (runs == should_run) {
            return;
        }
        WriteValue(word, 0, boss_mine ? u16{0} : *wanted);
        enemy_owner_changed_ms[character] = now_ms;
        ++changed;
    });
    static u32 logged = 0;
    if (changed > 0 && ++logged <= 60) {
        LOG_INFO(Debug, "Bloodborne party: enemy owner: {} {} enemies on map {:#x} (t={:.1f})",
                 role == EnemyRole::Owner ? "took over" : "handed", changed, map, now_ms / 1000.0);
    }
}

} // namespace Coop

namespace {

// Whether some player in this world is a phantom (WorldChrMan's players, type +0x78: 1 a summoned
// client): a guest joined into the host's world, before either made a trip. The game then sends
// enemy HP between them itself; after a trip each member runs its own copy of the map (all type
// 0) and only the HP board keeps the copies in step.
bool SharesOneWorld() {
    const u64 world = ReadValue<u64>(image_base + CandidateLocalStatePointerOffset, 0);
    const s32 count = world >= 0x10000 ? ReadValue<s32>(world, 0x30) : 0;
    const u64 entries = world >= 0x10000 ? ReadValue<u64>(world, 0x38) : 0;
    for (s32 index = 0; entries >= 0x10000 && index < std::min(count, 8); ++index) {
        const u64 player = ReadValue<u64>(entries + static_cast<u64>(index) * 0x38, 0);
        if (player >= 0x10000 && ReadValue<s32>(player, 0x78) == 1) {
            return true;
        }
    }
    return false;
}

} // namespace

namespace Coop {

// This player's own hits, counted at the game's hit function (see HitOffset): what each took off
// its victim, by the victim's handle, since the game started. Another player's hit that the game
// applies here (after a trip a guest's hits still reach the host's copy) is not this player's
// damage (Kaniel's friend run 2026-10-11: each of the guest's hits came back to it a second time).
// Poison and the like count too, on enemies this player hit last (see OwnDamage).
std::mutex own_hits_mutex;

} // namespace Coop

namespace {

std::unordered_map<u64, std::pair<u64, s32>> hits_in_flight; // victim -> (attacker, HP before)
// A guest's own hits, held against the host's older HP (HeldOwnHitHp): victim -> (handle, HP its
// hit left, when).
struct HeldHit {
    u32 handle = 0;
    s32 hp = 0;
    s64 at_ms = 0;
};
std::unordered_map<u64, HeldHit> held_own_hits;

} // namespace

namespace Coop {

// The character whose HP data is at `data` among this world's: a held hit needs the character.
void HoldHitNews(u64 data, u32 handle, s32 hp, s64 now_ms) {
    const u64 world = ReadValue<u64>(image_base + CandidateLocalStatePointerOffset, 0);
    if (world < 0x10000) {
        return;
    }
    u64 victim = 0;
    ForEachWorldCharacter(world, [&](u64 character) {
        const u64 modules = victim == 0 ? ReadValue<u64>(character, 0x3B0) : 0;
        if (modules >= 0x10000 && ReadValue<u64>(modules, 0x20) == data) {
            victim = character;
        }
    });
    if (victim != 0) {
        std::scoped_lock lock{own_hits_mutex};
        held_own_hits[victim] = {handle, hp, now_ms};
    }
}

// Kaniel 2026-10-15: a guest's hit took its copy down, then the host's HP from before the hit came
// (ch 5) and put it back until the host's answer: the guest's own hits hold for a while.
void HoldOwnHits() {
    const s64 now_ms = SteadyMilliseconds();
    std::vector<std::pair<u64, HeldHit>> held;
    {
        std::scoped_lock lock{own_hits_mutex};
        std::erase_if(held_own_hits, [&](const auto& entry) {
            return now_ms - entry.second.at_ms >= OwnHitHoldMs;
        });
        held.assign(held_own_hits.begin(), held_own_hits.end());
    }
    for (const auto& [victim, hit] : held) {
        // Still that character (a freed one reads as anything).
        if (!HasMemoryAccess(victim, 0x3B8, MemoryProt::CpuRead) ||
            ReadValue<u32>(victim, 0x08) != hit.handle) {
            continue;
        }
        const u64 modules = ReadValue<u64>(victim, 0x3B0);
        const u64 data = modules >= 0x10000 ? ReadValue<u64>(modules, 0x20) : 0;
        if (data < 0x10000 || !HasMemoryAccess(data + 0xF8, 4, MemoryProt::CpuWrite)) {
            continue;
        }
        const s32 current = ReadValue<s32>(data, 0xF8);
        if (const auto hp = HeldOwnHitHp(current, hit.hp, now_ms - hit.at_ms)) {
            WriteValue(data, 0xF8, *hp);
            static u32 logged = 0;
            if (++logged <= 40) {
                LOG_INFO(Debug,
                         "Bloodborne party: this guest's hit on {:#x} held: {} -> {} (the host's "
                         "HP from before it, {} ms after)",
                         hit.handle, current, *hp, now_ms - hit.at_ms);
            }
        }
    }
}

} // namespace Coop

namespace Coop {

OwnDamage own_damage;

} // namespace Coop

namespace {

bool hit_hooks_installed = false;
// This player's damage changed, or another game's came: the enemy step runs on the next frame
// instead of waiting out its round, so the damage goes out (or is shown) at once.
std::atomic<bool> enemy_step_due{false};
// Frames the enemy step runs on regardless (an owner got another member's hit: the game applies it
// on one of the next frames, and its HP goes to the others at once).
std::atomic<int> enemy_steps_forced{0};

} // namespace

namespace Coop {

void ForceEnemySteps() {
    enemy_steps_forced.store(6, std::memory_order_relaxed);
}

} // namespace Coop

namespace {

void PS4_SYSV_ABI NoteHitStart(u64, const GuestRegisterSnapshot* registers) {
    const u64 victim = ReadValue<u64>(registers->rdi, 0x08);
    if (victim < 0x10000) {
        return;
    }
    std::scoped_lock lock{own_hits_mutex};
    hits_in_flight[victim] = {registers->rsi, CharacterHp(victim)};
}

void PS4_SYSV_ABI NoteHitApplied(u64, const GuestRegisterSnapshot* registers) {
    const u64 victim = ReadValue<u64>(registers->r14, 0x08);
    u64 attacker = 0;
    s32 before = 0;
    {
        std::scoped_lock lock{own_hits_mutex};
        const auto hit = hits_in_flight.find(victim);
        if (hit == hits_in_flight.end()) {
            return;
        }
        std::tie(attacker, before) = hit->second;
        hits_in_flight.erase(hit);
    }
    const s32 taken = before - std::max(CharacterHp(victim), 0);
    const u64 player = GetLocalPlayer();
    if (victim == player && player != 0 && taken > 0 && attacker >= 0x10000 && attacker != player) {
        NoteHitOnMe(ReadValue<u32>(attacker, 0x08)); // the Rot rule's killer
    }
    if (victim == player || player == 0) {
        return;
    }
    const u32 handle = ReadValue<u32>(victim, 0x08);
    if (attacker == player && taken > 0 && before > 0 && CharacterHp(victim) <= 0) {
        NoteOwnKill(handle);
    }
    // A thrown item's hit names no character (Kaniel's fire bomb 2026-10-13: attacker handle 0).
    const bool named = attacker >= 0x10000 && ReadValue<u32>(attacker, 0x08) != 0;
    const HitBy by = attacker == player ? HitBy::Me : (named ? HitBy::OtherPlayer : HitBy::Nobody);
    {
        std::scoped_lock lock{own_hits_mutex};
        own_damage.Hit(handle, by, taken);
        own_damage.HitLeft(handle, CharacterHp(victim), SteadyMilliseconds());
        if (by == HitBy::Me && taken > 0 && IsPartyGuestRole()) {
            held_own_hits[victim] = {handle, CharacterHp(victim), SteadyMilliseconds()};
        }
    }
    if (by != HitBy::OtherPlayer && taken > 0) {
        enemy_step_due.store(true);
    }
    // Phase 8 (one HP path): this player's hit goes to the others at once, with the HP it left.
    if (by == HitBy::Me && taken > 0 && InPartySession() && EnemyOwnerOn()) {
        const u64 modules = ReadValue<u64>(victim, 0x3B0);
        const u64 data = modules >= 0x10000 ? ReadValue<u64>(modules, 0x20) : 0;
        if (data >= 0x10000) {
            SendEnemyHit(PlayingMap(), handle, std::max(CharacterHp(victim), 0),
                         ReadValue<s32>(data, 0xFC));
        }
    }
    if (taken <= 0) {
        return;
    }
    // Diagnostics: who hits what, for the first hits of each kind of attacker.
    static std::atomic<u32> logged_own{0}, logged_players{0}, logged_others{0};
    if (attacker != player && logged_players.load() >= 20 && logged_others.load() >= 20) {
        return;
    }
    bool by_player = false;
    if (attacker != player && attacker >= 0x10000) {
        for (const OtherPlayer& other : OtherPlayers()) {
            by_player |= other.character == attacker;
        }
    }
    std::atomic<u32>& logged =
        attacker == player ? logged_own : (by_player ? logged_players : logged_others);
    if (logged.fetch_add(1) < 20) {
        // The attacker's address last (the rig reads the rest).
        LOG_INFO(
            Debug, "Bloodborne party: hit on {:#x} by {} {:#x}: {} -> {} (t={:.1f}) attacker {:#x}",
            handle, attacker == player ? "this player" : (by_player ? "another player" : "other"),
            attacker >= 0x10000 ? ReadValue<u32>(attacker, 0x08) : 0, before, before - taken,
            SteadyMilliseconds() / 1000.0, attacker);
    }
}

// A status effect's HP change (see EffectHpChangeOffsets), only reached when HP changes.
void PS4_SYSV_ABI NoteEffectHpChange(u64, const GuestRegisterSnapshot* registers) {
    const u64 victim = ReadValue<u64>(registers->r12, 0x00);
    if (victim < 0x10000 || victim == GetLocalPlayer()) {
        return;
    }
    const s32 before = ReadValue<s32>(registers->r12, 0x08);
    const s32 max = ReadValue<s32>(registers->r12, 0x0C);
    const s32 after = std::clamp(static_cast<s32>(registers->rsi), 0, std::max(max, 0));
    std::scoped_lock lock{own_hits_mutex};
    const u32 handle = ReadValue<u32>(victim, 0x08);
    const s64 total = own_damage.Total(handle);
    own_damage.Effect(handle, before - after);
    if (own_damage.Total(handle) != total) {
        enemy_step_due.store(true);
    }
}

} // namespace

namespace Coop {

void InstallOwnHitCount(u64 base, u64 image_size) {
    if (!party_controller || HitAppliedOffset + HitApplied.size() > image_size) {
        return;
    }
    if (!InstallCoopHook(reinterpret_cast<void*>(base + HitOffset), HitPrologue, 0, NoteHitStart)) {
        LOG_ERROR(Debug, "Bloodborne hit hooks unavailable; shared enemy damage counts every drop");
        coop_features.Skipped("own hits", SiteFailure(base, image_size, HitOffset));
        return;
    }
    if (!InstallCoopHook(reinterpret_cast<void*>(base + HitAppliedOffset), HitApplied, 0,
                         NoteHitApplied)) {
        LOG_ERROR(Debug, "Bloodborne hit hooks unavailable; shared enemy damage counts every drop");
        coop_features.Skipped("own hits", SiteFailure(base, image_size, HitAppliedOffset));
        return;
    }
    hit_hooks_installed = true;
    coop_features.On("own hits");
    std::optional<u64> failed;
    for (const u64 offset : EffectHpChangeOffsets) {
        if (!InstallCoopHook(reinterpret_cast<void*>(base + offset), EffectHpChange, 0,
                             NoteEffectHpChange)) {
            LOG_ERROR(Debug,
                      "Bloodborne status effect HP hook unavailable at {:#x}; poison is not "
                      "shared",
                      offset);
            failed = failed.value_or(offset);
        }
    }
    if (failed) {
        coop_features.Skipped("effect hits", SiteFailure(base, image_size, *failed));
    } else {
        coop_features.On("effect hits");
    }
}

// Enemy HP straight from game to game (see DirectDamageType): the other games' frames, by sender,
// read from the P2P transport on the game's thread. Only the room's peers are heard.
DirectDamageInbox direct_damage_inbox;

// The other games' party death vitals and standing kills over the same link (see
// DirectVitalsType): read with the damage, used over the boards' copies.
std::mutex direct_boards_mutex;
DirectInbox<DirectKills> direct_kills_inbox;

} // namespace Coop

namespace {

// SharedDamage's inputs for the trace (hp-in): every round while the worlds are apart or a guest
// follows the host's HP, else only when they change.
void TraceHpInputs(u64 visit, std::string_view mode, bool forget, std::span<const EnemyCopy> alive,
                   std::span<const EnemyHpReport> reports, s64 now_ms) {
    if (!CoopTraceOn()) {
        return;
    }
    static TraceSampler sampler;
    std::vector<std::string> copies;
    copies.reserve(alive.size());
    for (const EnemyCopy& copy : alive) {
        copies.push_back(TraceToken(copy));
    }
    std::vector<std::string> others;
    for (const EnemyHpReport& report : reports) {
        others.push_back(TraceToken(report));
    }
    auto fields =
        sampler.Changes(coop_trace_file.load(),
                        {{"visit", fmt::format("{:x}", visit)}, {"mode", std::string{mode}}},
                        {{"alive", std::move(copies)}, {"reports", std::move(others)}});
    if (fields.empty() && !forget && mode != "apart" && mode != "one-guest") {
        return;
    }
    fields.insert(fields.begin(),
                  std::pair<std::string, std::string>{"now", std::to_string(now_ms)});
    if (forget) {
        fields.emplace_back("forget", "1");
    }
    Trace("hp-in", std::move(fields));
}

// This game's tag on its HP reports (the upper half of EnemyHpReport.visit), so the others count
// each member's damage apart.
const u32 enemy_hp_source = [] {
    std::random_device random;
    return static_cast<u32>(random()) | 1u;
}();

// Enemy HP kept in step (see SharedDamage): in the session, with the worlds apart, posts the
// damage this member did to its copies during its visit and takes the others' on top. Out of a
// session each world is its member's own, and in one world the game sends the HP itself.
// A death respawn reset the map: the damage the others reported on the world before it is left
// behind (SharedDamage::ForgetDamageBefore).
std::atomic<bool> party_hp_reset{};
[[maybe_unused]] const bool party_hp_reset_registered =
    RegisterLifecycleReset("enemy hp", {Lifecycle::Wipe}, [] { party_hp_reset.store(true); });
// A wipe changes the world as a load does: until the wipe's trip has loaded the map again, the
// host's dead copies are from before it (rig smoke 2026-10-13), so it shows nothing new for a
// while (PostsShownState) and the load starts that wait again.
[[maybe_unused]] const bool shown_wipe_registered = RegisterLifecycleReset(
    "shown state", {Lifecycle::Wipe}, [] { world_changed_ms.store(SteadyMilliseconds()); });
// Until the wipe's trip has loaded the map again, the owner's HP reports and a follower's copies
// are from before the wipe (see WipeHpHold).
std::mutex wipe_hp_hold_mutex;
WipeHpHold wipe_hp_hold;
[[maybe_unused]] const bool wipe_hp_hold_registered =
    RegisterLifecycleReset("enemy hp until the wipe's trip", {Lifecycle::Wipe}, [] {
        std::scoped_lock lock{wipe_hp_hold_mutex};
        wipe_hp_hold.Wiped(party_trips_started.load(), SteadyMilliseconds());
    });

// The HP data (+0xF8) of each standing copy, by handle (for the writes).
using EnemyHpData = std::unordered_map<u32, std::pair<u64, s32>>; // handle -> (data, hp)

// See ShareEnemyHp: phase 4's way, the area's owner runs the HP.
void ShareEnemyHpByOwner(EnemyRole role, u32 map, u64 source, std::span<const EnemyCopy> standing,
                         const EnemyHpData& data, std::span<const MemberMapState> others) {
    const s64 now_ms = SteadyMilliseconds();
    std::vector<EnemyHp> hurt;
    std::vector<EnemyWrite> writes;
    bool wipe_held = false;
    {
        std::scoped_lock lock{wipe_hp_hold_mutex};
        wipe_held = wipe_hp_hold.Holds(party_trips_started.load(), now_ms);
    }
    static bool logged_held = false;
    if (wipe_held != logged_held) {
        logged_held = wipe_held;
        LOG_INFO(Debug, "Bloodborne party: enemy HP {} on map {:#x} (t={:.1f})",
                 wipe_held ? "held until the wipe's trip loads the map again (no reports, no "
                             "copies lowered)"
                           : "no longer held for a wipe",
                 map, now_ms / 1000.0);
    }
    // The owner's copy is the enemy: its HP goes as read, at once (rig hit3 2026-10-14: two reads
    // agreeing made the others' bars 0.4 s late).
    for (const EnemyCopy& copy : standing) {
        if (role == EnemyRole::Owner && !wipe_held && copy.hp < copy.max) {
            hurt.push_back({copy.handle, copy.hp, copy.max});
        }
    }
    // Phase 8 (one HP path): the others' hits, at once on a follower's copies; each holds against
    // the owner's older HP as this game's own hit does (HoldOwnHits).
    for (const EnemyHitNews& hit : TakeEnemyHits()) {
        const auto it = data.find(hit.handle);
        if (hit.map != map || it == data.end()) {
            continue;
        }
        const auto copy = std::ranges::find(standing, hit.handle, &EnemyCopy::handle);
        if (copy == standing.end()) {
            continue;
        }
        const auto hp = HitNewsHp(role, copy->hp, copy->max, hit.hp, hit.max);
        if (!hp || !HasMemoryAccess(it->second.first, 0xFC, MemoryProt::CpuWrite)) {
            continue;
        }
        WriteValue(it->second.first, 0xF8, *hp);
        HoldHitNews(it->second.first, hit.handle, *hp, now_ms);
        static u32 logged_news = 0;
        if (++logged_news <= 400) {
            LOG_INFO(Debug,
                     "Bloodborne party: {}'s hit damage on map {:#x}: {:#x} {}->{} (t={:.1f})",
                     hit.member, map, hit.handle, copy->hp, *hp, now_ms / 1000.0);
        }
        if (*hp == 0) {
            LOG_INFO(Debug, "Bloodborne party: enemy death seen: {:#x} (t={:.1f})", hit.handle,
                     now_ms / 1000.0);
        }
    }
    // The log's HP (the owner's reports): a new owner's copies take it, and a follower's always.
    // The game applies the owner's ch 5 only in one world: after a trip each member has its own
    // copy of the map, and a follower's copy kept its own HP (rig hp-together 2026-10-14). Lower
    // only, so a follower's own hit, which also reaches the owner, counts once.
    const bool takeover = now_ms - became_owner_ms.load() < EnemyOwnerTakeoverMs;
    if (!wipe_held && (role == EnemyRole::Follower || takeover)) {
        const std::vector<EnemyCopy> taking{standing.begin(), standing.end()};
        std::vector<EnemyHpReport> copies;
        for (const MemberMapState& state : others) {
            if (EnemyOwnerArea(state.map) == EnemyOwnerArea(map)) {
                if (auto report = DecodeEnemyHp(state.bits)) {
                    copies.push_back(std::move(*report));
                }
            }
        }
        writes = LowerToHostHp(taking, MergeEnemyHpReports(copies));
    }
    {
        std::scoped_lock lock{enemy_hp_mutex};
        MemberMapState now{map, EncodeEnemyHp(source | HostHpReport, hurt)};
        const bool changed = now != local_enemy_hp;
        if (changed) {
            local_enemy_hp = std::move(now);
        }
        if (role == EnemyRole::Owner) {
            SendEnemyHp(local_enemy_hp, changed);
        }
    }
    std::string which;
    size_t lowered = 0;
    for (const EnemyWrite& write : writes) {
        const auto it = data.find(write.handle);
        if (it == data.end() || write.hp >= it->second.second ||
            !HasMemoryAccess(it->second.first, 0xFC, MemoryProt::CpuWrite)) {
            continue;
        }
        WriteValue(it->second.first, 0xF8, write.hp);
        ++lowered;
        if (which.size() < 200) {
            which += fmt::format(" {:#x} {}->{}", write.handle, it->second.second, write.hp);
        }
    }
    static u32 logged_lowered = 0;
    if (lowered > 0 && ++logged_lowered <= 400) {
        LOG_INFO(Debug,
                 "Bloodborne party: {} copies took the owner's HP from the log: {} enemies on "
                 "map {:#x}:{} (t={:.1f})",
                 role == EnemyRole::Owner ? "the new owner's" : "this follower's", lowered, map,
                 which, now_ms / 1000.0);
    }
    static std::optional<EnemyRole> logged_role;
    if (logged_role != role) {
        logged_role = role;
        LOG_INFO(Debug, "Bloodborne party: enemy HP from the area's owner ({}, map {:#x})",
                 role == EnemyRole::Owner ? "this game runs them" : "this game follows", map);
    }
}

void ShareEnemyHp(u32 map, u64 visit, std::span<const u64> characters) {
    struct Copy {
        u64 data;
        s32 hp;
        s32 max;
    };
    const auto alive_data = [](const auto& alive) {
        EnemyHpData data;
        for (const auto& [handle, copy] : alive) {
            data[handle] = {copy.data, copy.hp};
        }
        return data;
    };
    static SharedDamage damage;
    const bool in_session = InPartySession();
    const bool active = in_session && !SharesOneWorld();
    std::unordered_map<u32, Copy> alive;
    std::vector<EnemyCopy> standing;
    std::map<u32, s64> own_hits;
    if (hit_hooks_installed) {
        std::scoped_lock lock{own_hits_mutex};
        own_hits = own_damage.All();
    }
    for (const u64 character : characters) {
        const u64 modules = ReadValue<u64>(character, 0x3B0);
        const u64 data = modules >= 0x10000 ? ReadValue<u64>(modules, 0x20) : 0;
        if (data < 0x10000 || (ReadValue<u8>(character, 0x1E3) & 1) != 0) {
            continue;
        }
        const u32 handle = ReadValue<u32>(character, 0x08);
        const s32 hp = ReadValue<s32>(data, 0xF8);
        const s32 max = ReadValue<s32>(data, 0xFC);
        if (hp <= 0 || max <= 0) {
            continue;
        }
        alive[handle] = Copy{data, hp, max};
        std::optional<s64> hits;
        if (hit_hooks_installed) {
            const auto counted = own_hits.find(handle);
            hits = counted != own_hits.end() ? counted->second : 0;
        }
        standing.push_back({handle, hp, max, hits});
    }
    // The other members' reports: the world log's, or the direct frames and the HP board (see
    // OthersEnemyHp).
    const u64 source = (u64{enemy_hp_source & 0x7FFFFFFFu} << 32) | (visit & 0xFFFFFFFFu);
    const std::vector<MemberMapState> others = OthersEnemyHp(source);
    // Phase 4: the area's owner runs its enemies' HP (the game sends it to the others, ch 5); it
    // reports its hurt ones to the log, and the copies of a new owner first take the log's HP
    // (the last owner's). A follower reports nothing and writes nothing.
    if (const EnemyRole role = CurrentEnemyRole();
        in_session && EnemyOwnerOn() && role != EnemyRole::Unknown) {
        ShareEnemyHpByOwner(role, map, source, standing, alive_data(alive), others);
        return;
    }
    // The other members' reports for this area (any block of it: one who joined in place stands
    // on another block, and the blocks give their enemies distinct handles).
    std::vector<EnemyHpReport> copies;
    for (const MemberMapState& state : others) {
        if ((state.map >> 24) == (map >> 24)) {
            if (auto report = DecodeEnemyHp(state.bits)) {
                copies.push_back(std::move(*report));
            }
        }
    }
    const std::vector<EnemyHpReport> reports = MergeEnemyHpReports(copies);
    const bool forget_damage = party_hp_reset.exchange(false);
    if (forget_damage) {
        damage.ForgetDamageBefore(reports);
    }
    const s64 now_ms = SteadyMilliseconds();
    TraceHpInputs(visit,
                  !in_session ? "off"
                  : active    ? "apart"
                              : (IsPartyGuestRole() ? "one-guest" : "one-host"),
                  forget_damage, standing, reports, now_ms);
    // Out of a session the counts are this player's alone: visit 0 starts them over at the
    // session (one world's drop carries into worlds apart, a solo player's does not).
    SharedDamage::Step step =
        damage.Update(in_session ? visit : 0, active, standing, reports, now_ms,
                      IsPartyGuestRole() ? PartyRole::Guest : PartyRole::Host);
    // In one world: the host posts its hurt characters' HP (NPCs: the game does not send theirs),
    // a guest lowers its copies to it (see LowerToHostHp).
    const bool one_world = in_session && !active;
    if (one_world && !IsPartyGuestRole()) {
        // Two reads must agree here too (see SharedDamage): the higher of the last two.
        static std::unordered_map<u32, s32> last_hp;
        std::unordered_map<u32, s32> now_hp;
        for (const EnemyCopy& copy : standing) {
            const auto before = last_hp.find(copy.handle);
            const s32 confirmed =
                before != last_hp.end() ? std::max(copy.hp, before->second) : copy.max;
            now_hp[copy.handle] = copy.hp;
            if (confirmed < copy.max) {
                step.post.push_back({copy.handle, confirmed, copy.max});
            }
        }
        last_hp = std::move(now_hp);
    } else if (one_world) {
        step.writes = LowerToHostHp(standing, reports);
    }
    const std::vector<EnemyHp>& hurt = step.post;
    {
        std::scoped_lock lock{enemy_hp_mutex};
        MemberMapState now{map, EncodeEnemyHp(one_world ? (source | HostHpReport) : source, hurt)};
        const bool changed = now != local_enemy_hp;
        if (changed) {
            local_enemy_hp = std::move(now);
            enemy_hp_changed = true; // posted at once (see HpBoardWait)
            enemy_hp_wake.notify_all();
        }
        if (in_session) {
            SendEnemyHp(local_enemy_hp, changed);
        }
    }
    // Diagnostics (rig hp-after-guest-trip: the host's hit reached a guest back from a trip only
    // ~30 s later): what the board hands this member for its area, when that changes.
    {
        static std::string last_seen;
        std::string seen;
        for (const MemberMapState& state : others) {
            if (const auto report =
                    (state.map >> 24) == (map >> 24) ? DecodeEnemyHp(state.bits) : std::nullopt) {
                seen += fmt::format(" [{:#x}: {} hurt]", state.map, report->enemies.size());
            }
        }
        if (seen != last_seen) {
            last_seen = seen;
            LOG_INFO(Debug, "Bloodborne party: HP board for map {:#x}:{} (t={:.1f})", map,
                     seen.empty() ? " nothing" : seen, SteadyMilliseconds() / 1000.0);
        }
        static size_t last_hurt = 0;
        if (hurt.size() != last_hurt) {
            last_hurt = hurt.size();
            LOG_INFO(Debug, "Bloodborne party: posting {} hurt enemies on map {:#x} (t={:.1f})",
                     hurt.size(), map, SteadyMilliseconds() / 1000.0);
        }
    }
    size_t lowered = 0;
    std::string which;
    for (const EnemyWrite& write : step.writes) {
        const auto it = alive.find(write.handle);
        if (it == alive.end() || !HasMemoryAccess(it->second.data, 0xFC, MemoryProt::CpuWrite)) {
            continue;
        }
        WriteValue(it->second.data, 0xF8, write.hp);
        ++lowered;
        if (which.size() < 200) {
            which += fmt::format(" {:#x} {}->{}", write.handle, it->second.hp, write.hp);
        }
    }
    static u32 logged = 0;
    if (lowered > 0 && ++logged <= 400) {
        LOG_INFO(Debug,
                 "Bloodborne party: lowered {} enemies by the other members' damage on map "
                 "{:#x}:{} (t={:.1f})",
                 lowered, map, which, SteadyMilliseconds() / 1000.0);
    }
    static bool was_active = false;
    if (active != was_active) {
        was_active = active;
        LOG_INFO(Debug, "Bloodborne party: enemy HP {} (map {:#x})",
                 active ? "shared over the HP board (worlds apart)"
                        : (in_session ? "sent by the game (one world)" : "own (no session)"),
                 map);
    }
}

void ShowCharactersLikeTheHost(u32 map, std::span<const u64> characters,
                               const std::unordered_set<u32>& kill_hidden);
void HoldStoryRemovals(u32 map, std::span<const u64> characters,
                       const std::unordered_set<u32>& kill_hidden);

// See LoneGuestSwitchesOff: a lone guest's characters its story hid but its game keeps loaded
// switched off, and back on once shown again or no longer alone. The game's thread, every tick.
void SwitchOffWhatALoneGuestKeepsLoaded(u32 map, std::span<const u64> characters) {
    static std::unordered_map<u64, u32> switched; // character -> handle, switched off here
    static u32 switched_map = 0;
    if (map != switched_map) {
        switched.clear(); // the old map's characters are gone
        switched_map = map;
    }
    // Every guest on a map the host is not on, the one that runs its enemies and the others that
    // follow it (Kaniel's playtest 2026-10-24: krolebord, following Lisara's game in the Grand
    // Cathedral, saw Eileen there until the host came).
    const bool lone_guest = IsPartyGuestRole() && (Coop::CurrentEnemyRole() == EnemyRole::Owner ||
                                                   Coop::HostAwayFromThisMap());
    if (!lone_guest && switched.empty()) {
        return;
    }
    std::string off;
    std::string on;
    for (const u64 character : characters) {
        const u64 control = ReadValue<u64>(character, 0x18);
        if (control < 0x10000 || !HasMemoryAccess(character, 0x29, MemoryProt::CpuRead)) {
            continue;
        }
        const bool hidden = ReadValue<u8>(character, 0x28) != 0;
        const u32 handle = ReadValue<u32>(character, 0x08);
        if (const auto it = switched.find(character); it != switched.end()) {
            if (LoneGuestSwitchesBackOn(lone_guest, hidden) &&
                SetCharacterDisabled(character, false)) {
                switched.erase(it);
                on += fmt::format(" {:#x}", handle);
            }
            continue;
        }
        if (LoneGuestSwitchesOff(lone_guest, hidden, IsCharacterDisabled(character),
                                 ReadValue<s32>(control, 0x14), ReadValue<s32>(control, 0x10)) &&
            SetCharacterDisabled(character, true)) {
            switched.emplace(character, handle);
            off += fmt::format(" {:#x}", handle);
        }
    }
    if (!off.empty() || !on.empty()) {
        LOG_INFO(Debug,
                 "Bloodborne party: this lone guest's hidden characters its game kept loaded on "
                 "map {:#x}: off{} | on{}",
                 map, off, on);
    }
}

// See GetsKillDrop: the character's NpcParam row is [vtable +0x168 (character) +8], as the
// respawn walk reads it (0x0190E433). `own`: this guest's own kill. The game's thread only.
void GiveMemberKillDrop(u64 character, u32 handle, bool own = false) {
    if (!IsCharacter(character, "GiveMemberKillDrop")) {
        return;
    }
    const u64 vtable = ReadValue<u64>(character, 0);
    const u64 npc_of = vtable >= 0x10000 ? ReadValue<u64>(vtable, 0x168) : 0;
    if (npc_of < 0x10000) {
        return;
    }
    const u64 holder = reinterpret_cast<u64 PS4_SYSV_ABI (*)(u64)>(npc_of)(character);
    const u64 npc = holder >= 0x10000 ? ReadValue<u64>(holder, 8) : 0;
    if (npc < 0x10000 || !HasMemoryAccess(npc, NpcItemLotOffset + 4, MemoryProt::CpuRead)) {
        return;
    }
    const s32 lot = ReadValue<s32>(npc, NpcItemLotOffset);
    const u64 lots = lot > 0 ? FindLoadedParam("ITEMLOT_PARAM_ST") : 0;
    if (lots == 0) {
        return;
    }
    const u64 table_size = 0x40 + u64{ReadValue<u16>(lots, 0x0A)} * 24;
    if (!HasMemoryAccess(lots, table_size, MemoryProt::CpuRead)) {
        return;
    }
    const auto row = FindParamRow({reinterpret_cast<const u8*>(lots), table_size}, lot);
    if (!row || !HasMemoryAccess(lots + *row, ItemLotFlagOffset + 4, MemoryProt::CpuRead)) {
        return;
    }
    const s32 flag = ReadValue<s32>(lots + *row, ItemLotFlagOffset);
    if (!GetsKillDrop(InPartySession(), !own, own && IsPartyGuestRole(), flag,
                      flag > 0 && EventFlagOn(flag))) {
        return;
    }
    const u64 item_man = ReadValue<u64>(image_base + MapItemManPointerOffset, 0);
    if (!HasVtable(item_man, MapItemManVtable, "member kill drop item lot")) {
        return;
    }
    giving_deferred_item_lot = true;
    reinterpret_cast<void PS4_SYSV_ABI (*)(u64, s32, u32)>(image_base +
                                                           AwardItemLotOffset)(item_man, lot, 0);
    giving_deferred_item_lot = false;
    if (!EventFlagOn(flag)) {
        reinterpret_cast<void PS4_SYSV_ABI (*)(u64, s32, bool)>(
            image_base + PartyGhostSetEventFlagOffset)(0, flag, true);
    }
    LOG_INFO(Debug, "Bloodborne party: {} {:#x}; gave its drop, item lot {} (flag {})",
             own ? "this guest killed" : "a member killed", handle, lot, flag);
}

// VisitKills's inputs for the trace (kills-in), when they change.
void TraceKillsInputs(u64 visit, u32 map, bool forget, std::span<const SeenCharacter> characters,
                      std::span<const u32> others_maps) {
    if (!CoopTraceOn()) {
        return;
    }
    static TraceSampler sampler;
    std::vector<std::string> tokens;
    tokens.reserve(characters.size());
    for (const SeenCharacter& character : characters) {
        tokens.push_back(TraceToken(character));
    }
    auto fields = sampler.Changes(coop_trace_file.load(),
                                  {{"visit", fmt::format("{:x}", visit)},
                                   {"map", fmt::format("{:x}", map)},
                                   {"others", TraceHandles(others_maps)}},
                                  {{"chars", std::move(tokens)}});
    if (forget) {
        fields.emplace_back("forget", "1");
    }
    if (!fields.empty()) {
        Trace("kills-in", std::move(fields));
    }
}

// The kills this game would apply on its copies now (KeepEnemyDeathsInStep), each with its HP
// now (0 in its own death animation), for the trace (kill-apply), when they change.
void TraceMemberKills(std::span<const SeenCharacter> characters, std::span<const u64> pointers,
                      const std::unordered_map<u64, u32>& hidden,
                      const std::unordered_set<u32>& others,
                      const std::unordered_set<u32>& died_for_others) {
    if (!CoopTraceOn()) {
        return;
    }
    static TraceSampler sampler;
    std::string candidates;
    for (size_t index = 0; index < pointers.size(); ++index) {
        const u32 handle = characters[index].handle;
        if (characters[index].hp > 0 && others.contains(handle) &&
            !hidden.contains(pointers[index]) && !died_for_others.contains(handle) &&
            ReadValue<u8>(pointers[index], 0x28) == 0) {
            candidates += fmt::format("{}{:x}:{}", candidates.empty() ? "" : ",", handle,
                                      CharacterHp(pointers[index]));
        }
    }
    if (auto fields = sampler.Changes(coop_trace_file.load(), {{"cands", candidates}}, {});
        !fields.empty()) {
        Trace("kill-apply", std::move(fields));
    }
}

bool IsBossSized(u64 character); // below, with the characters shown like the host

// A boss-sized character at its full HP (`hp`, read now): see SeenCharacter::untouched_boss.
bool IsUntouchedBoss(u64 character, s32 hp) {
    const u64 modules = ReadValue<u64>(character, 0x3B0);
    const u64 data = modules >= 0x10000 ? ReadValue<u64>(modules, 0x20) : 0;
    return IsBossSized(character) && data >= 0x10000 && hp >= ReadValue<s32>(data, 0xFC);
}

// A character as VisitKills sees it now.
SeenCharacter SeeCharacter(u64 character) {
    // Only a real death is a kill: characters of a map being torn down read 0 HP too, and a guest
    // leaving for the Dream "killed" the dog for the host.
    // A hit seen to leave it at 0 counts at once (SeenDead): the death bit comes after the reward.
    const s32 hp = CharacterHp(character);
    const u32 handle = ReadValue<u32>(character, 0x08);
    std::optional<bool> last_hit_mine;
    std::optional<s64> zero_hit_ms;
    {
        std::scoped_lock lock{own_hits_mutex};
        last_hit_mine = own_damage.LastHitMine(handle);
        zero_hit_ms = own_damage.ZeroHitMs(handle);
    }
    const bool dead =
        SeenDead(ReadValue<u8>(character, 0x1E3), hp, zero_hit_ms, SteadyMilliseconds());
    return {handle,
            dead ? 0 : std::max(hp, 1),
            character,
            false,
            IsUntouchedBoss(character, hp),
            KilledByAnother(hit_hooks_installed, last_hit_mine, !IsPartyGuestRole(),
                            Coop::CurrentEnemyRole() == EnemyRole::Owner)};
}

// The copies the kill step put down with the game's kill: their kill reward pays nothing.
std::mutex quiet_kills_mutex;
QuietKills quiet_kills;

// A death respawn reset the map: this member's kills are taken back (VisitKills::Forget).
std::atomic<bool> party_kills_reset{};
[[maybe_unused]] const bool party_kills_reset_registered =
    RegisterLifecycleReset("kills", {Lifecycle::Wipe}, [] { party_kills_reset.store(true); });

} // namespace

namespace Coop {

bool TakesQuietKillReward(u64 character) {
    std::scoped_lock lock{quiet_kills_mutex};
    return quiet_kills.Takes(character, SteadyMilliseconds());
}

// Enemy deaths kept in step across loads: this member's kills during its visit go to the
// party's kill board (VisitKills), and its copies of the enemies another member killed during
// that member's current visit are hidden with the event disable byte (+0x28, see
// KeepBossHpInStep's notes): a guest arriving at the host's map found the host's dead wolf alive.
void KeepEnemyDeathsInStep(bool sharing) {
    static VisitKills visit_kills;
    static VisitTracker visit_tracker;
    static std::unordered_map<u64, u32> hidden; // character -> handle, hidden by us
    static u64 hidden_visit = 0;
    static std::chrono::steady_clock::time_point checked{};
    const auto now = std::chrono::steady_clock::now();
    const u64 global = GetGlobalState();
    const bool loading = global < 0x10000 || ReadValue<u8>(global, 0x08) != 0;
    if (TakeDirectDamage()) {
        enemy_step_due.store(true);
    }
    // Phase 4: the log's entries (the owner's HP) are shown on the next frame, not the next round.
    if (TakeServerWorldNews()) {
        enemy_step_due.store(true);
    }
    SendPeerHello();
    TickPortraits();
    // Not while the world goes away: a trip's start posted the old map's characters as kills.
    const bool forced = enemy_steps_forced.load(std::memory_order_relaxed) > 0 &&
                        enemy_steps_forced.fetch_sub(1, std::memory_order_relaxed) > 0;
    // Every tick, not every round (a hold a round late was drawn a frame), and in a trip's arrival
    // too (a guest's lasts seconds after the load: Gilbert was drawn all along).
    if (const u64 world = ReadValue<u64>(image_base + CandidateLocalStatePointerOffset, 0);
        sharing && !loading && world >= 0x10000 && IsUsablePackedMap(PlayingMap())) {
        const u64 player = GetLocalPlayer();
        std::vector<u64> loaded;
        ForEachWorldCharacter(world, [&](u64 character) {
            if (character != player) {
                loaded.push_back(character);
            }
        });
        std::unordered_set<u32> kill_hidden;
        for (const auto& [character, handle] : hidden) {
            kill_hidden.insert(handle);
        }
        HoldStoryRemovals(PlayingMap(), loaded, kill_hidden);
        SwitchOffWhatALoneGuestKeepsLoaded(PlayingMap(), loaded);
    }
    if (!WatchesKills(sharing, PartyTripActive(), loading)) {
        return;
    }
    if (now - checked < HpBoardRound && !enemy_step_due.exchange(false) && !forced) {
        return;
    }
    checked = now;
    const u64 world = ReadValue<u64>(image_base + CandidateLocalStatePointerOffset, 0);
    const u64 player = GetLocalPlayer();
    const u32 map = PlayingMap();
    if (world < 0x10000 || !IsUsablePackedMap(map)) {
        return;
    }
    const u64 visit =
        visit_tracker.Update(map, player, InPartySession(), party_trips_started.load());
    if (visit != hidden_visit) {
        hidden.clear(); // the old visit's characters are gone
        hidden_visit = visit;
    }
    std::vector<SeenCharacter> characters;
    std::vector<u64> pointers;
    ForEachWorldCharacter(world, [&](u64 character) {
        if (character != player) {
            characters.push_back(SeeCharacter(character));
            pointers.push_back(character);
        }
    });
    const s64 now_ms = SteadyMilliseconds();
    // When this game first saw each character, for the copies the world just made (CopyJustMade).
    static std::unordered_map<u64, s64> seen_since;
    std::erase_if(seen_since, [&](const auto& seen) {
        return std::ranges::find(pointers, seen.first) == pointers.end();
    });
    for (const u64 character : pointers) {
        seen_since.try_emplace(character, now_ms);
    }
    for (size_t index = 0; index < characters.size(); ++index) {
        const auto changed = enemy_owner_changed_ms.find(pointers[index]);
        std::optional<bool> last_hit_mine;
        {
            std::scoped_lock lock{own_hits_mutex};
            last_hit_mine = own_damage.LastHitMine(characters[index].handle);
        }
        characters[index].settling =
            SettlingHidesDeath(CharacterSettles(now_ms, world_changed_ms.load(),
                                                changed != enemy_owner_changed_ms.end()
                                                    ? std::optional{changed->second}
                                                    : std::nullopt),
                               last_hit_mine);
    }
    // Where the others are: this member's kills stand while one of them stays on the map.
    std::vector<u32> others_maps;
    {
        std::scoped_lock lock{map_board_mutex};
        if (std::chrono::steady_clock::now() - map_board_time < std::chrono::seconds{10}) {
            for (const MemberMap& entry : map_board) {
                if (entry.member != map_board_member) {
                    others_maps.push_back(entry.map);
                }
            }
        }
    }
    // Enemies another member killed that died here by the game's kill (DiesInsteadOfHiding) are
    // that member's kills, not this one's.
    static std::unordered_set<u32> died_for_others;
    static u64 died_visit = 0;
    if (visit != died_visit) {
        died_for_others.clear();
        died_visit = visit;
    }
    const bool forget_kills = party_kills_reset.exchange(false);
    TraceKillsInputs(visit, map, forget_kills, characters, others_maps);
    auto changes = forget_kills ? visit_kills.Forget() : std::vector<std::pair<s32, bool>>{};
    if (const std::vector<u32> reset = TakeLogMapResets(); !reset.empty() && !forget_kills) {
        const auto back = visit_kills.ForgetOn(reset);
        changes.insert(changes.end(), back.begin(), back.end());
        std::string maps;
        for (const u32 on : reset) {
            maps += fmt::format("{}{:#x}", maps.empty() ? "" : ", ", on);
        }
        LOG_INFO(Debug,
                 "Bloodborne party: the party's log reset {}: {} of this member's kills "
                 "there taken back",
                 maps, back.size());
    }
    const auto observed = visit_kills.Observe(visit, characters, others_maps, map);
    changes.insert(changes.end(), observed.begin(), observed.end());
    std::erase_if(changes, [](const std::pair<s32, bool>& change) {
        return change.second && died_for_others.contains(static_cast<u32>(change.first));
    });
    ShareEnemyHp(map, visit, pointers);
    // A guest's own kill: its game makes no one-time drop (Ada's hag, 2026-10-13).
    if (IsPartyGuestRole() && InPartySession()) {
        for (const auto& [handle, killed] : observed) {
            if (!killed || died_for_others.contains(static_cast<u32>(handle))) {
                continue;
            }
            const auto at =
                std::ranges::find(characters, static_cast<u32>(handle), &SeenCharacter::handle);
            if (at != characters.end()) {
                GiveMemberKillDrop(pointers[at - characters.begin()], static_cast<u32>(handle),
                                   true);
            }
        }
    }
    if (!changes.empty()) {
        const auto kills = std::ranges::count_if(changes, [](const auto& c) { return c.second; });
        // Which ones, with their death flags (+0x1E3) and HP now: a kill that is not one shows
        // what marked it.
        std::string which;
        for (const auto& [handle, killed] : changes) {
            const auto at =
                std::ranges::find(characters, static_cast<u32>(handle), &SeenCharacter::handle);
            const u64 character = at != characters.end() ? pointers[at - characters.begin()] : 0;
            which += fmt::format(" {}{:#x}", killed ? "" : "back ", static_cast<u32>(handle));
            if (character != 0) {
                which += fmt::format(" (dead {:#x} hp {})", ReadValue<u8>(character, 0x1E3),
                                     CharacterHp(character));
            }
        }
        LOG_INFO(Debug, "Bloodborne party: kill board gets {} kills, {} back (visit {:#x}):{}",
                 kills, changes.size() - kills, visit, which);
    }
    // The other members' kills (the world log's or the kill board's; not what lay dead in a save),
    // and with this member's own and held ones (see GatherPartyKills).
    PartyKillsNow party =
        GatherPartyKills(visit_kills, map, changes, forget_kills, died_for_others);
    // A kill of this member's taken back (a generator brought it back) is no party kill, though
    // the party's log has it until this member's unkill has gone round (see TakenBackKills).
    static TakenBackKills taken_back;
    for (const auto& [handle, killed] : changes) {
        if (!killed) {
            taken_back.TakeBack(static_cast<u32>(handle), now_ms);
        }
    }
    for (const u32 handle : taken_back.Holding(party.all, now_ms)) {
        party.all.erase(handle);
        party.members.erase(handle);
    }
    std::unordered_set<u32>& others = party.all;
    const std::unordered_set<u32>& member_kills = party.members;
    // And what is dead in the host's world, killed before the session too (its save): a guest
    // back from a trip runs its own world, where those were alive (Kaniel 2026-10-11).
    std::optional<std::vector<CharacterShown>> hosts_now;
    if (IsPartyGuestRole() && InPartySession()) {
        const std::optional<MemberMapState> host = HostShownNow();
        if (host && host->map == map) {
            if (hosts_now = DecodeShownCharacters(host->bits); hosts_now) {
                const auto dead = DeadInTheHostsWorld(*hosts_now);
                others.insert(dead.begin(), dead.end());
            }
        }
    }
    // This guest's copies dead by a real death, since when: what the host's map generator brought
    // back while they stayed dead here comes back here too (see BroughtBackByTheHost).
    static std::unordered_map<u32, s64> dead_since;
    static u64 dead_since_visit = 0;
    if (visit != dead_since_visit) {
        dead_since.clear();
        dead_since_visit = visit;
    }
    std::vector<DeadCopy> dead_here;
    for (size_t index = 0; index < pointers.size(); ++index) {
        const u32 handle = characters[index].handle;
        if (characters[index].hp <= 0 && IsRealDeath(ReadValue<u8>(pointers[index], 0x1E3))) {
            dead_here.push_back({handle, dead_since.try_emplace(handle, now_ms).first->second});
        } else {
            dead_since.erase(handle);
        }
    }
    if (hosts_now) {
        static BringBackGuard bring_back_guard;
        const s64 host_moved = session_host_moved_ms.load();
        const std::vector<u32> back = bring_back_guard.Filter(
            visit, BroughtBackByTheHost(*hosts_now, dead_here, others, now_ms), *hosts_now,
            dead_here);
        static u32 held_logged = 0;
        if (!back.empty() &&
            HoldsBringBacks(now_ms, host_moved != 0 ? std::optional{host_moved} : std::nullopt)) {
            if (++held_logged <= 20) {
                LOG_INFO(Debug,
                         "Bloodborne party: {} bring-backs held, the session host moved {} ms ago",
                         back.size(), now_ms - host_moved);
            }
        } else if (BringBackWhatTheHostBroughtBack(back)) {
            bring_back_guard.Brought(back, now_ms);
        }
    }
    TraceMemberKills(characters, pointers, hidden, others, died_for_others);
    size_t newly_hidden = 0;
    size_t newly_died = 0;
    const auto me = LocalPlayerPosition();
    const bool can_kill = Coop::KillCharacterCallable();
    // Copies this game's own generator brought back are hidden, not killed again (LocalRevives).
    static LocalRevives revives;
    static u64 revives_visit = 0;
    if (visit != revives_visit) {
        revives.Clear();
        revives_visit = visit;
    }
    for (const SeenCharacter& seen : characters) {
        revives.See(seen.handle, seen.hp);
    }
    // Copies at 0 HP without the game's death, since when (see FinishesStuckDeath).
    static std::unordered_map<u64, s64> zero_since;
    static std::unordered_set<u64> finished;
    static u64 zero_visit = 0;
    if (visit != zero_visit) {
        zero_since.clear();
        finished.clear();
        zero_visit = visit;
    }
    for (size_t index = 0; index < pointers.size(); ++index) {
        const u64 character = pointers[index];
        const u32 handle = characters[index].handle;
        const s32 hp = characters[index].hp;
        const bool just_made = CopyJustMade(now_ms, seen_since.at(character));
        if (hp > 0) {
            zero_since.erase(character);
        } else if (can_kill && !just_made && !finished.contains(character)) {
            const auto since = zero_since.try_emplace(character, now_ms).first->second;
            const u8 death_flags = ReadValue<u8>(character, 0x1E3);
            if (FinishesStuckDeath(others.contains(handle), hp, death_flags, since, now_ms) &&
                IsCharacter(character, "stuck death")) {
                using Kill = void PS4_SYSV_ABI (*)(u64 character);
                reinterpret_cast<Kill>(image_base + KillCharacterOffset)(character);
                finished.insert(character);
                LOG_INFO(Debug,
                         "Bloodborne party: {:#x} sat at 0 HP without dying (death flags {:#x}) "
                         "after a member's kill; the game's kill now",
                         handle, death_flags);
            }
        }
        const auto it = hidden.find(character);
        if (it == hidden.end() && hp > 0 && !just_made &&
            AppliesMemberKill(others.contains(handle), CharacterHp(character)) &&
            ReadValue<u8>(character, 0x28) == 0 &&
            HasMemoryAccess(character, 0x29, MemoryProt::CpuWrite)) {
            if (can_kill && !died_for_others.contains(handle) && !revives.Revived(handle) &&
                IsCharacter(character, "member kill") &&
                DiesInsteadOfHiding(me, CharacterPosition(character))) {
                // Near this player: it dies as the game kills, not vanishing, and pays no echoes
                // (QuietKills). Its one-time drop first: given with its lot's flag on, the game's
                // death drops no orb of it as well (Kaniel's test 2026-10-23: the host got the
                // hag's drop and saw it lying there too).
                if (member_kills.contains(handle)) {
                    GiveMemberKillDrop(character, handle);
                }
                {
                    std::scoped_lock lock{quiet_kills_mutex};
                    quiet_kills.Killed(character, now_ms);
                }
                using Kill = void PS4_SYSV_ABI (*)(u64 character);
                reinterpret_cast<Kill>(image_base + KillCharacterOffset)(character);
                died_for_others.insert(handle);
                ++newly_died;
                continue;
            }
            WriteValue(character, 0x28, u8{1});
            hidden.emplace(character, handle);
            ++newly_hidden;
            if (member_kills.contains(handle)) {
                GiveMemberKillDrop(character, handle);
            }
        } else if (it != hidden.end() && !others.contains(handle) &&
                   HasMemoryAccess(character, 0x29, MemoryProt::CpuWrite)) {
            // The killer's visit ended: the enemy respawned for it, so it comes back here too.
            WriteValue(character, 0x28, u8{0});
            hidden.erase(it);
        }
    }
    if (newly_hidden + newly_died > 0) {
        LOG_INFO(Debug,
                 "Bloodborne party: hid {} enemies a member killed on map {:#x}, {} near died",
                 newly_hidden, map, newly_died);
    }
    std::unordered_set<u32> kill_hidden;
    for (const auto& [character, handle] : hidden) {
        kill_hidden.insert(handle);
    }
    ShowCharactersLikeTheHost(map, pointers, kill_hidden);
}

} // namespace Coop

namespace {

// SetCharacterDisabledOffset is also what event command 2004:5 runs (bank 2004 handler
// 0x017BA6A0, case 5): off hides the model and stops hits and AI, and it returns at once when
// the switch is already so.
constexpr std::array<u8, 8> SetCharacterDisabledPrologue{0x55, 0x48, 0x89, 0xE5,
                                                         0x41, 0x57, 0x41, 0x56};

} // namespace

namespace Coop {

// The game's thread only.
bool SetCharacterDisabled(u64 character, bool disabled) {
    if (!IsCharacter(character, "SetCharacterDisabled") ||
        ReadValue<u64>(character, 0x18) < 0x10000 ||
        !std::ranges::equal(std::span<const u8>{reinterpret_cast<const u8*>(
                                                    image_base + SetCharacterDisabledOffset),
                                                SetCharacterDisabledPrologue.size()},
                            SetCharacterDisabledPrologue)) {
        return false;
    }
    using Set = void PS4_SYSV_ABI (*)(u64 character, u32 disabled);
    reinterpret_cast<Set>(image_base + SetCharacterDisabledOffset)(character, disabled ? 1 : 0);
    return true;
}

} // namespace Coop

namespace {

// What this guest changed to match the host: characters it hid (+0x28) and switched off. Kept
// across loads of the same map; see UndoGuestChangesOnMapChange.
std::unordered_set<u32> guest_hidden_here;
std::unordered_set<u32> guest_switched_off;

// On a map change, what this guest hid or switched off and is still loaded is shown and switched
// on again before the lists are dropped: characters by a map border stay loaded across it, and
// nothing else ever turned the Forbidden Woods' enemies back on for Kaniel's friend (2026-10-11).
void UndoGuestChangesOnMapChange(u32 map, std::span<const u64> characters) {
    static u32 changed_map = 0;
    if (map == changed_map) {
        return;
    }
    changed_map = map;
    std::vector<CharacterSwitch> mine;
    std::vector<u64> pointers;
    size_t shown = 0;
    for (const u64 character : characters) {
        const u32 handle = ReadValue<u32>(character, 0x08);
        if (guest_hidden_here.contains(handle) && ReadValue<u8>(character, 0x28) != 0 &&
            HasMemoryAccess(character, 0x29, MemoryProt::CpuWrite)) {
            WriteValue(character, 0x28, u8{0});
            ++shown;
        }
        mine.push_back({handle, IsCharacterDisabled(character)});
        pointers.push_back(character);
    }
    size_t switched_on = 0;
    for (const CharacterSwitch& change : UndoneOnMapChange(guest_switched_off, mine)) {
        const auto at = std::ranges::find(mine, change.handle, &CharacterSwitch::handle);
        switched_on += SetCharacterDisabled(pointers[at - mine.begin()], false) ? 1 : 0;
    }
    if (shown + switched_on > 0) {
        LOG_INFO(Debug,
                 "Bloodborne party: map {:#x}: showed {} and switched on {} characters this guest "
                 "had changed to match the host",
                 map, shown, switched_on);
    }
    guest_hidden_here.clear();
    guest_switched_off.clear();
}

// The host's story removes some characters before their model loads (2004:29, the +0x28 byte;
// Iosefka upstairs in the clinic on new characters). This guest's copies, built at its join,
// stay loaded and drawn with the byte set, so they are switched off here (and those the host
// switched off: the copy of Iosefka by her door), and back on once the host shows them again (see
// SwitchedLikeTheHost). Never the players.
void SwitchCharactersLikeTheHost(u32 map, std::span<const u64> characters,
                                 std::span<const CharacterShown> host,
                                 const std::unordered_set<u32>& kill_hidden) {
    auto& switched_off = guest_switched_off;
    std::unordered_set<u64> players;
    for (const OtherPlayer& other : OtherPlayers()) {
        players.insert(other.character);
    }
    std::vector<CharacterSwitch> mine;
    std::vector<u64> pointers;
    for (const u64 character : characters) {
        if (!players.contains(character)) {
            mine.push_back({ReadValue<u32>(character, 0x08), IsCharacterDisabled(character),
                            DeadInItsWorld(character),
                            HandsOutBoss(NpcTypeOf(character), IsBossSized(character))});
            pointers.push_back(character);
        }
    }
    // Ambush enemies the host's event switched off for good (AmbushSlotFlag): their slot done.
    // Looked up only in Hemwick while one off far from the host is on here.
    std::unordered_set<u32> off_for_good;
    const auto far_off_on_here = [&](const CharacterShown& at) {
        return at.distant && (at.hidden || at.disabled) &&
               std::ranges::any_of(mine, [&](const CharacterSwitch& own) {
                   return own.handle == at.handle && !own.disabled;
               });
    };
    if (map == AmbushMap && std::ranges::any_of(host, far_off_on_here)) {
        const u64 world = ReadValue<u64>(image_base + CandidateLocalStatePointerOffset, 0);
        if (world >= 0x10000) {
            for (const auto& [character, entity] : CharacterEntityIds(world)) {
                if (const auto flag = AmbushSlotFlag(entity); flag && EventFlagOn(*flag)) {
                    off_for_good.insert(ReadValue<u32>(character, 0x08));
                }
            }
        }
    }
    static SwitchOnWait switch_on_wait;
    std::string which;
    for (const CharacterSwitch& change : switch_on_wait.Due(
             SwitchedLikeTheHost(host, mine, switched_off, kill_hidden, off_for_good), switched_off,
             SteadyMilliseconds())) {
        const auto at = std::ranges::find(mine, change.handle, &CharacterSwitch::handle);
        if (!SetCharacterDisabled(pointers[at - mine.begin()], change.disabled)) {
            continue;
        }
        if (change.disabled) {
            switched_off.insert(change.handle);
        } else {
            switched_off.erase(change.handle);
        }
        if (which.size() < 200) {
            which += fmt::format(" {}{:#x}", change.disabled ? "" : "+", change.handle);
        }
    }
    static u32 logged = 0;
    if (!which.empty() && ++logged <= 200) {
        LOG_INFO(Debug,
                 "Bloodborne party: characters switched like the host has them on map {:#x} "
                 "(off, +on):{}",
                 map, which);
    }
    // Their hits too (see HitsLikeTheHost): the Witch of Hemwick's summons.
    std::vector<CharacterHits> hits;
    hits.reserve(pointers.size());
    for (const u64 character : pointers) {
        hits.push_back({ReadValue<u32>(character, 0x08), CharacterHitsOff(character)});
    }
    std::string hit_which;
    for (const CharacterHits& change : HitsLikeTheHost(host, hits, kill_hidden)) {
        const auto at = std::ranges::find(hits, change.handle, &CharacterHits::handle);
        if (SetCharacterHitsOff(pointers[at - hits.begin()], change.off) &&
            hit_which.size() < 200) {
            hit_which += fmt::format(" {}{:#x}", change.off ? "" : "+", change.handle);
        }
    }
    static u32 hits_logged = 0;
    if (!hit_which.empty() && ++hits_logged <= 200) {
        LOG_INFO(Debug,
                 "Bloodborne party: characters' hits like the host has them on map {:#x} "
                 "(off, +on):{}",
                 map, hit_which);
    }
}

// See StoryRemovals: what this game's story removed on its last visit of the map is switched off
// from a load until the story removes it again. Hosts and guests alike (a guest far from its host
// follows its own story). The game's thread only.
// Kaniel 2026-10-16: the memory is the save's: another character (or a save loaded from the
// title) starts without it.
std::atomic<bool> story_removals_reset{false};
[[maybe_unused]] const bool story_removals_registered = RegisterLifecycleReset(
    "story removals", {Lifecycle::NewCharacter}, [] { story_removals_reset.store(true); });

} // namespace

namespace Coop {

void ForgetStoryRemovals() {
    story_removals_reset.store(true);
}

} // namespace Coop

namespace {

void HoldStoryRemovals(u32 map, std::span<const u64> characters,
                       const std::unordered_set<u32>& kill_hidden) {
    static StoryRemovals removals;
    if (story_removals_reset.exchange(false)) {
        removals.Forget();
        LOG_INFO(Debug, "Bloodborne party: story removals forgotten (another save)");
    }
    std::unordered_set<u64> players;
    for (const OtherPlayer& other : OtherPlayers()) {
        players.insert(other.character);
    }
    std::vector<StoryCharacter> seen;
    std::unordered_map<u32, u64> pointers;
    for (const u64 character : characters) {
        if (players.contains(character)) {
            continue;
        }
        const u32 handle = ReadValue<u32>(character, 0x08);
        seen.push_back({handle, ReadValue<u8>(character, 0x28) != 0, IsCharacterDisabled(character),
                        guest_hidden_here.contains(handle) || guest_switched_off.contains(handle),
                        kill_hidden.contains(handle), DeadInItsWorld(character)});
        pointers[handle] = character;
    }
    const s64 now = SteadyMilliseconds();
    const StoryHoldStep step = removals.Step(map, world_loaded_ms.load(), now, seen);
    std::string which;
    for (const u32 handle : step.hold) {
        if (SetCharacterDisabled(pointers[handle], true)) {
            removals.Held(handle, now);
            which += fmt::format(" {:#x}", handle);
        }
    }
    for (const u32 handle : step.release) {
        SetCharacterDisabled(pointers[handle], false);
        which += fmt::format(" +{:#x}", handle);
    }
    for (const u32 handle : step.hand_over) {
        // The host's board hid it: left off for SwitchCharactersLikeTheHost to switch on with
        // the host. A kill's hide is the hide byte alone.
        if (guest_hidden_here.contains(handle) || guest_switched_off.contains(handle)) {
            guest_switched_off.insert(handle);
        } else {
            SetCharacterDisabled(pointers[handle], false);
        }
        which += fmt::format(" ={:#x}", handle);
    }
    static u32 logged = 0;
    if (!which.empty() && ++logged <= 200) {
        LOG_INFO(Debug,
                 "Bloodborne party: story removals held on map {:#x} until the story runs (off, "
                 "+on, =handed over):{} ({:.1f} s after the load)",
                 map, which, (now - world_loaded_ms.load()) / 1000.0);
    }
    static std::pair<u32, size_t> remembered{};
    static u32 remembered_logged = 0;
    if (const std::pair now_remembered{map, removals.RememberedOn(map)};
        now_remembered != remembered && ++remembered_logged <= 100) {
        remembered = now_remembered;
        LOG_INFO(Debug, "Bloodborne party: the story removed {} characters on map {:#x}",
                 now_remembered.second, map);
    }
}

// A boss-sized character (max HP [[+0x3B0]+0x20]+0xFC of DeathTraceMinMaxHp or more): boss
// progress stays with whoever beat the boss (Kaniel 2026-10-09), so the host's dead bosses are
// not passed on as dead.
bool IsBossSized(u64 character) {
    const u64 modules = ReadValue<u64>(character, 0x3B0);
    const u64 data = modules >= 0x10000 ? ReadValue<u64>(modules, 0x20) : 0;
    return data >= 0x10000 && ReadValue<s32>(data, 0xFC) >= DeathTraceMinMaxHp;
}

// ShownLikeTheHost's inputs for the trace (shown-in), when they change.
void TraceShownInputs(std::span<const CharacterShown> host, std::span<const CharacterShown> mine,
                      const std::unordered_set<u32>& kill_hidden,
                      const std::unordered_set<u32>& hidden_here) {
    if (!CoopTraceOn()) {
        return;
    }
    static TraceSampler sampler;
    const auto sorted = [](const std::unordered_set<u32>& handles) {
        std::vector<u32> list(handles.begin(), handles.end());
        std::ranges::sort(list);
        return TraceHandles(list);
    };
    if (auto fields = sampler.Changes(coop_trace_file.load(),
                                      {{"host", TraceHex(EncodeShownCharacters(host))},
                                       {"mine", TraceHex(EncodeShownCharacters(mine))},
                                       {"kill", sorted(kill_hidden)},
                                       {"here", sorted(hidden_here)}},
                                      {});
        !fields.empty()) {
        Trace("shown-in", std::move(fields));
    }
}

void ShowFollowingTheHost(u32 map, std::span<const u64> characters,
                          std::span<const CharacterShown> mine,
                          const std::unordered_set<u32>& kill_hidden);

// Every character of the map hidden or shown as the host's world has it (see CharacterShown):
// the host posts its own, a guest in the session on the host's map follows them.
void ShowCharactersLikeTheHost(u32 map, std::span<const u64> characters,
                               const std::unordered_set<u32>& kill_hidden) {
    UndoGuestChangesOnMapChange(map, characters);
    const bool hosting = !IsPartyGuestRole();
    const auto host_at = hosting ? LocalPlayerPosition() : std::nullopt;
    std::vector<CharacterShown> mine;
    mine.reserve(characters.size());
    for (const u64 character : characters) {
        mine.push_back({ReadValue<u32>(character, 0x08), ReadValue<u8>(character, 0x28) != 0,
                        IsCharacterDisabled(character),
                        hosting && FarFromHost(host_at, CharacterPosition(character)),
                        hosting && DeadInItsWorld(character) && !IsBossSized(character),
                        CharacterHitsOff(character)});
    }
    if (hosting) {
        if (!PostsShownState(SteadyMilliseconds(), world_changed_ms.load())) {
            return; // its story is still setting the map up (Iosefka shown, then hidden)
        }
        ShowOnTheLog(map, mine);
        std::scoped_lock lock{shown_board_mutex};
        local_shown = MemberMapState{map, EncodeShownCharacters(mine)};
        return;
    }
    // Each player's own characters (FollowsTheHostsShown) stay as this member's game has them.
    static std::unordered_set<u64> own_characters;
    static std::chrono::steady_clock::time_point own_checked{};
    static u32 own_map = 0;
    static size_t own_count = 0;
    // Again as soon as the map or its characters change, before anything is hidden (a load makes
    // new characters).
    if (const auto now = std::chrono::steady_clock::now();
        map != own_map || characters.size() != own_count ||
        now - own_checked >= std::chrono::seconds{2}) {
        own_checked = now;
        own_map = map;
        own_count = characters.size();
        own_characters.clear();
        const u64 world = ReadValue<u64>(image_base + CandidateLocalStatePointerOffset, 0);
        if (world >= 0x10000) {
            for (const auto& [character, entity] : CharacterEntityIds(world)) {
                if (!FollowsTheHostsShown(entity)) {
                    own_characters.insert(character);
                }
            }
        }
        // One hidden or switched off to match the host before it was known as this player's own
        // (a reload of the map, the entity table filled a moment later) comes back.
        std::vector<u32> own_handles;
        std::vector<u64> own_pointers;
        for (const u64 character : characters) {
            if (own_characters.contains(character)) {
                own_handles.push_back(ReadValue<u32>(character, 0x08));
                own_pointers.push_back(character);
            }
        }
        const OwnRestore restore =
            RestoresOwnCharacters(own_handles, guest_hidden_here, guest_switched_off);
        for (size_t index = 0; index < own_handles.size(); ++index) {
            const u32 handle = own_handles[index];
            if (std::ranges::find(restore.show, handle) != restore.show.end() &&
                HasMemoryAccess(own_pointers[index], 0x29, MemoryProt::CpuWrite)) {
                WriteValue(own_pointers[index], 0x28, u8{0});
                guest_hidden_here.erase(handle);
            }
            if (std::ranges::find(restore.switch_on, handle) != restore.switch_on.end() &&
                SetCharacterDisabled(own_pointers[index], false)) {
                guest_switched_off.erase(handle);
            }
        }
    }
    if (!own_characters.empty()) {
        std::vector<u64> following;
        std::vector<CharacterShown> following_shown;
        for (size_t index = 0; index < characters.size(); ++index) {
            if (!own_characters.contains(characters[index])) {
                following.push_back(characters[index]);
                following_shown.push_back(mine[index]);
            }
        }
        if (following.size() != characters.size()) {
            ShowFollowingTheHost(map, following, following_shown, kill_hidden);
            return;
        }
    }
    ShowFollowingTheHost(map, characters, mine, kill_hidden);
}

// ShowCharactersLikeTheHost for the characters that follow the host's.
void ShowFollowingTheHost(u32 map, std::span<const u64> characters,
                          std::span<const CharacterShown> mine,
                          const std::unordered_set<u32>& kill_hidden) {
    auto& hidden_here = guest_hidden_here;
    const std::optional<MemberMapState> host = HostShownNow();
    // What the host's world had near the host stands once it moves away (see HostNearStates).
    static HostNearStates host_near;
    if (!InPartySession()) {
        host_near = {};
    }
    if (!InPartySession() || !host || host->map != map) {
        return;
    }
    const auto posted = DecodeShownCharacters(host->bits);
    if (!posted) {
        return;
    }
    const std::vector<CharacterShown> hosts = host_near.Fill(map, *posted);
    TraceShownInputs(hosts, mine, kill_hidden, hidden_here);
    size_t hid = 0;
    size_t showed = 0;
    std::string which;
    for (const CharacterShown& change : ShownLikeTheHost(hosts, mine, kill_hidden, hidden_here)) {
        const auto at = std::ranges::find(mine, change.handle, &CharacterShown::handle);
        const u64 character = characters[at - mine.begin()];
        if (!HasMemoryAccess(character, 0x29, MemoryProt::CpuWrite)) {
            continue;
        }
        WriteValue(character, 0x28, static_cast<u8>(change.hidden ? 1 : 0));
        if (change.hidden) {
            hidden_here.insert(change.handle);
        } else {
            hidden_here.erase(change.handle);
        }
        ++(change.hidden ? hid : showed);
        if (which.size() < 200) {
            which += fmt::format(" {}{:#x}", change.hidden ? "" : "+", change.handle);
        }
    }
    static u32 logged = 0;
    if (hid + showed > 0 && ++logged <= 200) {
        LOG_INFO(Debug,
                 "Bloodborne party: characters as the host has them on map {:#x}: hid {}, "
                 "showed {}:{}",
                 map, hid, showed, which);
    }
    SwitchCharactersLikeTheHost(map, characters, hosts, kill_hidden);
}

} // namespace

namespace Coop {

// The host's map state for the board (see MapFlagBase), every 3 s while sharing.
void SnapshotHostMapState(bool sharing) {
    static std::chrono::steady_clock::time_point last{};
    const auto now = std::chrono::steady_clock::now();
    if (!sharing || IsPartyGuestRole() || now - last < std::chrono::seconds{3}) {
        return;
    }
    last = now;
    const u32 map = PlayingMap();
    if (!IsUsablePackedMap(map)) {
        return;
    }
    const FlagStore store = ReadFlagStore();
    if (store.flags_per_block == 0) {
        return;
    }
    const s32 base = MapFlagBase(map);
    std::vector<u8> bits((MapFlagCount + 7) / 8);
    for (s32 index = 0; index < MapFlagCount; ++index) {
        const FlagBit bit = FlagBitLocation(base + index, store.flags_per_block);
        for (const FlagBlock& block : store.blocks) {
            if (block.id == bit.block) {
                if (bit.byte < block.bytes.size() && (block.bytes[bit.byte] & bit.mask) != 0) {
                    bits[index / 8] |= static_cast<u8>(0x80 >> (index % 8));
                }
                break;
            }
        }
    }
    MapStateOnTheLog(map, bits);
    std::scoped_lock lock{map_state_mutex};
    local_map_state = MemberMapState{map, std::move(bits)};
}

// On a guest loading the host's map: its flags in that map's range become the host's
// (TakesHostMapFlag), before the map's events start.
size_t ApplyHostMapState() {
    const std::optional<MemberMapState> state = HostMapStateNow();
    // Raw: called from the map item creation hook, in the load, before the tracker has the map.
    const u32 map = GetCurrentPackedMap();
    if (!state || state->map != map || state->bits.size() * 8 < MapFlagCount) {
        return 0;
    }
    using SetFlag = void PS4_SYSV_ABI (*)(u64, s32, bool);
    const s32 base = MapFlagBase(map);
    size_t changed = 0;
    for (s32 index = 0; index < MapFlagCount; ++index) {
        const s32 flag = base + index;
        const bool host_on = (state->bits[index / 8] & (0x80 >> (index % 8))) != 0;
        if (TakesHostMapFlag(flag) && EventFlagOn(flag) != host_on) {
            reinterpret_cast<SetFlag>(image_base + PartyGhostSetEventFlagOffset)(0, flag, host_on);
            ++changed;
        }
    }
    return changed;
}

bool ChangedLikeTheHost(u32 handle) {
    return guest_hidden_here.contains(handle) || guest_switched_off.contains(handle);
}

} // namespace Coop

} // namespace Core::Bloodborne
