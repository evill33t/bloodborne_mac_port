// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// Party rules: no fog walls, harder enemies, levels, solid NPCs, shared loot. Split from
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

std::vector<KillEchoes> echoes_to_post;

} // namespace Coop

namespace {

// The multiplayer walls (MultiplayerWalls) turned off in a party session the way the event
// scripts' own Lua_InvalidMultiWall does: collision and drawing off, the fog SFX invalidated.
// Event 7600 shows them again on its next multiplayer edge, so this runs about once a second.
// Every map's walls: the entity lookup (0x013CABD0) searches the loaded blocks and skips an
// id it does not find, so a wall of a map not loaded costs a search and nothing else.
constexpr u64 SetColiEnableOffset = 0x0132DEF0;
constexpr u64 SetDrawEnableOffset = 0x0132DEC0;
constexpr u64 InvalidSfxOffset = 0x01330870;

} // namespace

namespace Coop {

void TurnOffMultiplayerWalls(bool in_session) {
    static std::chrono::steady_clock::time_point last{};
    const auto now = std::chrono::steady_clock::now();
    if (!in_session || now - last < std::chrono::seconds{1}) {
        return;
    }
    last = now;
    using EntitySwitch = void PS4_SYSV_ABI (*)(u64 unused, s32 entity, bool on);
    for (const MultiplayerWall& wall : MultiplayerWalls()) {
        reinterpret_cast<EntitySwitch>(image_base + SetColiEnableOffset)(0, wall.object, false);
        reinterpret_cast<EntitySwitch>(image_base + SetDrawEnableOffset)(0, wall.object, false);
        reinterpret_cast<EntitySwitch>(image_base + InvalidSfxOffset)(0, wall.sfx, true);
    }
}

} // namespace Coop

namespace {

// The loaded SpEffectParam, found through SoloParamRepository (0x05540340): its param slots hold
// a pointer whose +0x70 -> +0x70 is the PARAM file (the goods lookup at 0x01F1E3D0 reads one this
// way). Once found, row 9006 (the summoned guest's) gets a max HP rate of 1.0.
constexpr u64 SoloParamRepositoryPointerOffset = 0x05540340;

// Calls `found` with each loaded PARAM file of `type` (AtkParam_Pc and AtkParam_Npc share one)
// until it returns true; returns that file, or 0. The slot it was in last time first
// (LoadedParamSlots), the whole repository only when that slot no longer holds it.
template <typename Found>
u64 FindLoadedParamWhere(std::string_view type, Found&& found) {
    const TickStepTimer step{"FindLoadedParam"};
    const u64 repository = ReadValue<u64>(image_base + SoloParamRepositoryPointerOffset, 0);
    if (repository < 0x10000 || !HasMemoryAccess(repository, 0x2000, MemoryProt::CpuRead)) {
        return 0;
    }
    const auto param_in = [&](u64 slot) -> u64 {
        const u64 holder = ReadValue<u64>(repository, slot);
        if (holder < 0x10000 || !HasMemoryAccess(holder, 0x78, MemoryProt::CpuRead)) {
            return 0;
        }
        const u64 inner = ReadValue<u64>(holder, 0x70);
        if (inner < 0x10000 || !HasMemoryAccess(inner, 0x78, MemoryProt::CpuRead)) {
            return 0;
        }
        const u64 param = ReadValue<u64>(inner, 0x70);
        if (param < 0x10000 || !HasMemoryAccess(param, 0x40, MemoryProt::CpuRead)) {
            return 0;
        }
        return IsParamOfType({reinterpret_cast<const u8*>(param), 0x40}, type) && found(param)
                   ? param
                   : 0;
    };
    static std::mutex slots_mutex;
    static LoadedParamSlots slots;
    std::optional<u64> last;
    {
        std::scoped_lock lock{slots_mutex};
        last = slots.Slot(type);
    }
    if (last) {
        if (const u64 param = param_in(*last); param != 0) {
            return param;
        }
    }
    for (u64 slot = 0; slot < 0x2000; slot += 8) {
        if (const u64 param = param_in(slot); param != 0) {
            std::scoped_lock lock{slots_mutex};
            slots.Found(type, slot);
            return param;
        }
    }
    return 0;
}

} // namespace

namespace Coop {

u64 FindLoadedParam(std::string_view type) {
    return FindLoadedParamWhere(type, [](u64) { return true; });
}

void KeepGuestFullHp() {
    static bool done = false;
    static int attempts = 0;
    static std::chrono::steady_clock::time_point last{};
    const auto now = std::chrono::steady_clock::now();
    if (done || !party_controller || now - last < std::chrono::seconds{1}) {
        return;
    }
    last = now;
    const u64 param = FindLoadedParam("SP_EFFECT_PARAM_ST");
    if (param == 0) {
        if (++attempts == 30) {
            LOG_ERROR(Debug, "Bloodborne SpEffectParam not found in SoloParamRepository");
        }
        return;
    }
    done = true;
    const u16 rows = ReadValue<u16>(param, 0x0A);
    const u64 table_size = 0x40 + u64{rows} * 24;
    if (!HasMemoryAccess(param, table_size, MemoryProt::CpuRead)) {
        return;
    }
    const auto row =
        FindParamRow({reinterpret_cast<const u8*>(param), table_size}, PhantomHpEffect);
    if (!row || !HasMemoryAccess(param + *row, SpEffectMaxHpRateOffset + 4, MemoryProt::CpuWrite)) {
        LOG_ERROR(Debug, "Bloodborne SpEffect {} not found; summoned guests keep 70% HP",
                  PhantomHpEffect);
        return;
    }
    const float before = ReadValue<float>(param + *row, SpEffectMaxHpRateOffset);
    WriteValue(param + *row, SpEffectMaxHpRateOffset, 1.0f);
    LOG_INFO(Debug, "Bloodborne party guests keep full HP (SpEffect {} max HP rate {} -> 1)",
             PhantomHpEffect, before);
}

} // namespace Coop

namespace {

// Harder enemies (see HarderEnemies): NpcParam's HP and poise and AtkParam_Npc's damage are
// scaled in the loaded tables, which the game reads when it creates a character, so this runs
// from the title screen on (the pad reader and the save slot's opening call it). Row 271000 of
// each table (Father Gascoigne) shows whether the scaled values are still there; a table that
// was loaded again is scaled again. Kaniel 2026-10-25 (the enemy scaling host rule): the rate
// changes with the hunters on the map, so each table's own values are kept and every new rate
// starts from them.
const std::optional<HarderEnemies> harder_enemies = [] {
    const char* setting = std::getenv("SHADPS4_BLOODBORNE_HARDER_ENEMIES");
    return ParseHarderEnemies(setting != nullptr ? setting : "");
}();
std::mutex harder_enemies_mutex;

// A loaded PARAM file up to the end of its last row, when all of it is writable.
std::optional<std::span<u8>> WritableParam(u64 param, u64 row_size) {
    const u16 rows = ReadValue<u16>(param, 0x0A);
    const u64 entries = 0x40 + u64{rows} * 24;
    if (rows == 0 || !HasMemoryAccess(param, entries, MemoryProt::CpuRead)) {
        return std::nullopt;
    }
    // The entries in one span: a ReadValue each was a memory check a row (21 000 a second).
    const u64 size = ParamTableSize({reinterpret_cast<const u8*>(param), entries}, rows, row_size);
    if (size > 0x1000000 || !HasMemoryAccess(param, size, MemoryProt::CpuWrite)) {
        return std::nullopt;
    }
    return std::span<u8>{reinterpret_cast<u8*>(param), size};
}

// Row 271000's {HP, poise, turnVellocity's bits} or {attack, 0, 0} as this table holds it now.
using ParamMarker = std::array<s32, 3>;
std::optional<ParamMarker> HarderEnemiesMarker(std::span<const u8> param, u64 first, u64 second) {
    const auto row = FindParamRow(param, NpcAttackMarkerRow);
    if (!row || *row + std::max<u64>(second + 2, first + 4) > param.size()) {
        return std::nullopt;
    }
    u32 one = 0;
    s16 two = 0;
    s32 turn = 0;
    std::memcpy(&one, param.data() + *row + first, first == NpcHpOffset ? 4 : 2);
    if (second != 0) {
        std::memcpy(&two, param.data() + *row + second, sizeof(two));
    }
    if (first == NpcHpOffset) {
        std::memcpy(&turn, param.data() + *row + NpcTurnVelocityOffset, sizeof(turn));
    }
    return ParamMarker{static_cast<s32>(one), static_cast<s32>(two), turn};
}

} // namespace

namespace Coop {

std::atomic<bool> enemies_harder_now{false};

std::optional<std::span<u8>> WritableSpEffects(u64 table) {
    return WritableParam(table, 0x210);
}

std::size_t HuntersOnMyMap() {
    // Held through a trip's load (its map reads 0, then the old one: the tables would flip).
    static std::atomic<std::size_t> last{1};
    const u32 map = PlayingMap();
    if (map == 0 || PartyTripActive()) {
        return last.load();
    }
    std::scoped_lock lock{map_board_mutex};
    const std::size_t hunters =
        std::chrono::steady_clock::now() - map_board_time > std::chrono::seconds{3}
            ? 1
            : HuntersOnMap(map_board, map_board_member, map);
    last.store(hunters);
    return hunters;
}

bool EnemiesAreHarder() {
    return enemies_harder_now.load();
}

namespace {

bool SameRate(const std::optional<HarderEnemies>& a, const std::optional<HarderEnemies>& b) {
    if (!a || !b) {
        return a.has_value() == b.has_value();
    }
    return a->hp == b->hp && a->damage == b->damage && a->poise == b->poise;
}

std::string RateText(const std::optional<HarderEnemies>& rate) {
    return rate ? fmt::format("x{:.2f} HP, x{:.2f} damage, x{:.2f} poise", rate->hp, rate->damage,
                              rate->poise)
                : std::string{"the normal game"};
}

// One loaded table as the game had it (its own values) and the rate it holds now. Kaniel
// 2026-10-28: and NpcParam's boss turn speed (the boss tempo rule), 1 the game's.
struct ScaledParam {
    u64 table = 0;
    std::vector<u8> own;
    std::optional<ParamMarker> marker; // as last written
    std::optional<HarderEnemies> rate;
    float turn = 1.0f;
    bool Scaled() const {
        return rate.has_value() || turn != 1.0f;
    }
};

// Puts `wanted` and `turn` into `table` (from its own values); whether it changed anything.
// `scale` gets the table and the rate (none: only the turn speed changes).
template <typename Scale>
bool ScaleParam(ScaledParam& kept, u64 address, std::span<u8> table,
                std::optional<ParamMarker> marker, const std::optional<HarderEnemies>& wanted,
                float turn, Scale&& scale) {
    const bool fresh = address != kept.table || marker != kept.marker;
    if (fresh) {
        // A new table (or one loaded again): its values are the game's own.
        if (!wanted && turn == 1.0f) {
            kept = ScaledParam{.table = address, .marker = marker};
            return false;
        }
        kept.table = address;
        kept.own.assign(table.begin(), table.end());
        kept.rate.reset();
        kept.turn = 1.0f;
    } else if (SameRate(kept.rate, wanted) && kept.turn == turn) {
        return false;
    }
    if (kept.own.size() != table.size() && !kept.Scaled()) {
        kept.own.assign(table.begin(), table.end()); // still the game's own values
    }
    if (kept.own.size() == table.size()) {
        std::memcpy(table.data(), kept.own.data(), table.size());
    }
    if (wanted || turn != 1.0f) {
        scale(table, wanted);
    }
    kept.rate = wanted;
    kept.turn = turn;
    return true;
}

} // namespace

void MakeEnemiesHarder() {
    // Kaniel 2026-10-25: with the enemy scaling rule the team's size on this map picks the rate,
    // over the player's own setting.
    const HostRules rules = ActiveHostRules();
    const std::size_t hunters = rules.enemy_scaling ? HuntersOnMyMap() : 0;
    const std::optional<HarderEnemies> wanted =
        rules.enemy_scaling ? TeamScaledEnemies(hunters) : harder_enemies;
    // Kaniel 2026-10-28: the boss tempo rule's turn speed for the boss rows.
    const float turn = ActiveBossTempoTuning().turn;
    static ScaledParam npc_kept;
    static ScaledParam attack_kept;
    static std::optional<std::vector<std::pair<s32, float>>> boss_bonus_own;
    std::scoped_lock lock{harder_enemies_mutex};
    if (!wanted && turn == 1.0f && !npc_kept.Scaled() && !attack_kept.rate && !boss_bonus_own) {
        enemies_harder_now.store(false);
        return; // never scaled: the game's tables as they are
    }
    static std::chrono::steady_clock::time_point last{};
    static std::optional<HarderEnemies> last_wanted;
    static float last_turn = 1.0f;
    const auto now = std::chrono::steady_clock::now();
    if (now - last < std::chrono::seconds{1} && SameRate(wanted, last_wanted) &&
        turn == last_turn) {
        return;
    }
    last = now;
    const TickStepTimer step{"MakeEnemiesHarder"};
    last_turn = turn;
    if (!SameRate(wanted, last_wanted) && rules.enemy_scaling) {
        LOG_INFO(Debug, "Bloodborne enemy scaling: {} hunter(s) on this map: {}", hunters,
                 RateText(wanted));
    }
    last_wanted = wanted;
    enemies_harder_now.store(wanted.has_value());
    constexpr u64 NpcRowSize = 0x184;
    constexpr u64 AtkRowSize = 0xA4;
    const u64 npc = FindLoadedParam("NPC_PARAM_ST");
    const u64 attacks = FindLoadedParamWhere("ATK_PARAM_ST", [](u64 param) {
        const u16 rows = ReadValue<u16>(param, 0x0A);
        const u64 entries = 0x40 + u64{rows} * 24;
        return HasMemoryAccess(param, entries, MemoryProt::CpuRead) &&
               FindParamRow({reinterpret_cast<const u8*>(param), entries}, NpcAttackMarkerRow);
    });
    const bool in_world = GetLocalPlayer() >= 0x10000;
    if (const auto table = npc != 0 ? WritableParam(npc, NpcRowSize) : std::nullopt) {
        const auto marker = HarderEnemiesMarker(*table, NpcHpOffset, NpcPoiseOffset);
        std::size_t rows = 0;
        std::size_t turned = 0;
        // What this step changes: the harder rate (or its table) and the boss turn speed.
        const bool rate_changes = !SameRate(npc_kept.rate, wanted) || npc_kept.table != npc;
        const bool turn_changes = npc_kept.turn != turn || (npc_kept.table != npc && turn != 1.0f);
        if (ScaleParam(npc_kept, npc, *table, marker, wanted, turn,
                       [&](std::span<u8> param, const std::optional<HarderEnemies>& rate) {
                           rows = rate ? MakeNpcParamHarder(param, *rate) : 0;
                           turned = ScaleBossTurnSpeed(param, turn);
                       })) {
            npc_kept.marker = HarderEnemiesMarker(*table, NpcHpOffset, NpcPoiseOffset);
            if (rate_changes) {
                LOG_INFO(Debug,
                         "Bloodborne harder enemies: {} NpcParam rows, {} (Father Gascoigne HP "
                         "{}, poise {}){}",
                         rows, RateText(wanted), npc_kept.marker ? (*npc_kept.marker)[0] : -1,
                         npc_kept.marker ? (*npc_kept.marker)[1] : -1,
                         in_world ? "; enemies already on this map keep their old stats" : "");
            }
            if (turn_changes) {
                float gascoigne = 0.0f;
                if (const auto row = FindParamRow(*table, NpcAttackMarkerRow)) {
                    std::memcpy(&gascoigne, table->data() + *row + NpcTurnVelocityOffset,
                                sizeof(gascoigne));
                }
                LOG_INFO(Debug,
                         "Bloodborne boss tempo: {} boss NpcParam rows turn x{:.2f} (Father "
                         "Gascoigne {:.0f} degrees a second){}",
                         turned, turn, gascoigne,
                         in_world ? "; the bosses already on this map turn as before" : "");
            }
        }
    }
    if (const auto table = attacks != 0 ? WritableParam(attacks, AtkRowSize) : std::nullopt) {
        const auto marker = HarderEnemiesMarker(*table, AtkPhysOffset, 0);
        std::size_t rows = 0;
        if (ScaleParam(attack_kept, attacks, *table, marker, wanted, 1.0f,
                       [&](std::span<u8> param, const std::optional<HarderEnemies>& rate) {
                           rows = rate ? MakeNpcAttacksHarder(param, rate->damage) : 0;
                       })) {
            attack_kept.marker = HarderEnemiesMarker(*table, AtkPhysOffset, 0);
            LOG_INFO(Debug,
                     "Bloodborne harder enemies: {} AtkParam_Npc rows, x{:.2f} damage (Father "
                     "Gascoigne's first swing {}){}",
                     rows, wanted ? wanted->damage : 1.0f,
                     attack_kept.marker ? (*attack_kept.marker)[0] : -1,
                     in_world ? "; enemies already on this map keep their old stats" : "");
        }
    }
    // Checked every time: a reloaded SpEffectParam gets the bonus back. Back to the normal game
    // (the scaling rule, alone on a map), the bonus is the game's again.
    if (const u64 effects = FindLoadedParam("SP_EFFECT_PARAM_ST"); effects != 0) {
        if (const auto table = WritableParam(effects, 0x210)) {
            if (wanted) {
                if (!boss_bonus_own) {
                    boss_bonus_own = CoopBossHpRates(*table);
                }
                if (const std::size_t rows = RemoveCoopBossHpBonus(*table); rows != 0) {
                    LOG_INFO(Debug,
                             "Bloodborne harder enemies: the co-op boss HP bonus (SpEffect "
                             "7500-7503) is off in {} rows; bosses get x{:.2f} HP only",
                             rows, wanted->hp);
                }
            } else if (boss_bonus_own) {
                SetCoopBossHpRates(*table, *boss_bonus_own);
                boss_bonus_own.reset();
                LOG_INFO(Debug, "Bloodborne harder enemies: the co-op boss HP bonus is back");
            }
        }
    }
}

} // namespace Coop

namespace {

// The text of message `id` the way the game shows it (MessageLookupOffset), or null.
const char16_t* GameMessage(u64 repository, u32 category, s32 id) {
    using Lookup =
        const char16_t* PS4_SYSV_ABI (*)(u64 repository, u32 version, u32 category, s32 id);
    return reinterpret_cast<Lookup>(image_base + MessageLookupOffset)(repository, 0, category, id);
}

// See PolishedMessages: writes each replacement over the English text the game holds. Returns
// how many messages changed (0 once done, or for another language).
std::size_t PolishMessages() {
    const u64 repository = ReadValue<u64>(image_base + MsgRepositoryPointerOffset, 0);
    if (repository < 0x10000 || !HasMemoryAccess(repository, 0x10, MemoryProt::CpuRead)) {
        return 0;
    }
    std::size_t changed = 0;
    for (const PolishedMessage& message : PolishedMessages()) {
        // The network banners read the override category first, as the game does.
        const char16_t* text = nullptr;
        if (message.category == EventTextCategory) {
            text = GameMessage(repository, EventTextOverrideCategory, message.id);
        }
        if (text == nullptr) {
            text = GameMessage(repository, message.category, message.id);
        }
        const u64 address = reinterpret_cast<u64>(text);
        const std::size_t capacity = message.original.size() + 1;
        if (text == nullptr ||
            !HasMemoryAccess(address, capacity * sizeof(char16_t), MemoryProt::CpuReadWrite)) {
            continue;
        }
        const std::u16string_view current{text, message.original.size()};
        if (text[message.original.size()] != u'\0' ||
            PolishedText(message.category, message.id, current) != message.replacement) {
            continue;
        }
        std::u16string written{message.replacement};
        written.resize(capacity, u'\0');
        std::memcpy(reinterpret_cast<void*>(address), written.data(),
                    written.size() * sizeof(char16_t));
        ++changed;
    }
    return changed;
}

} // namespace

namespace Coop {

// Multiplayer UI polish for a party (Kaniel 2026-10-11): Host/Guest text, a name for guest role 6
// and no bell icons, in the loaded message files, the role table and SpEffectParam. Checked every
// second: a reloaded file gets it again.
void PolishMultiplayerUi() {
    static std::chrono::steady_clock::time_point last{};
    const auto now = std::chrono::steady_clock::now();
    if (!party_controller || now - last < std::chrono::seconds{1}) {
        return;
    }
    last = now;
    const TickStepTimer step{"PolishMultiplayerUi"};
    if (const std::size_t messages = PolishMessages(); messages != 0) {
        LOG_INFO(Debug, "Bloodborne party UI: {} messages now say Host and Guest", messages);
    }
    if (HasMemoryAccess(image_base + RoleTableOffset, RoleTableSize, MemoryProt::CpuReadWrite)) {
        const std::span<u8, RoleTableSize> roles{
            reinterpret_cast<u8*>(image_base + RoleTableOffset), RoleTableSize};
        if (NameUnnamedGuestRole(roles) != 0) {
            LOG_INFO(Debug, "Bloodborne party UI: guest role {} is named as role {}",
                     UnnamedGuestRole, GuestRole);
        }
    }
    if (const u64 effects = FindLoadedParam("SP_EFFECT_PARAM_ST"); effects != 0) {
        if (const auto table = WritableParam(effects, 0x210)) {
            if (const std::size_t rows = RemoveBellIcons(*table); rows != 0) {
                LOG_INFO(Debug, "Bloodborne party UI: no bell icons ({} SpEffect rows)", rows);
            }
        }
    }
}

} // namespace Coop

namespace {

// The level sync check (see LevelSyncEffect): `seta bl` has just decided whether this player is
// above the host's band; the next instructions compare it with the last answer ([r14+0x2C]) and
// add or remove effect 9200 when it changed. Clearing bl in a party session removes the effect.
constexpr u64 LevelSyncDecisionOffset = 0x01507DB5;
constexpr std::array<u8, 8> LevelSyncDecision{0x41, 0x0F, 0xB6, 0x46, 0x2C, 0x0F, 0xB6, 0xCB};

void PS4_SYSV_ABI KeepPartyGuestsLevel(u64, GuestRegisterSnapshot* registers) {
    if (!LevelSyncsGuest(InPartySession(), (registers->rbx & 0xFF) != 0)) {
        registers->rbx &= ~u64{0xFF};
    }
}

} // namespace

namespace Coop {

void InstallNoLevelSyncInParty(u64 base, u64 image_size) {
    if (!party_controller) {
        return;
    }
    if (LevelSyncDecisionOffset + LevelSyncDecision.size() > image_size ||
        !InstallCoopHook(reinterpret_cast<void*>(base + LevelSyncDecisionOffset), LevelSyncDecision,
                         0, KeepPartyGuestsLevel)) {
        LOG_ERROR(Debug, "Bloodborne level sync hook unavailable; party guests are synced to the "
                         "host's level");
        coop_features.Skipped("level sync", SiteFailure(base, image_size, LevelSyncDecisionOffset));
        return;
    }
    coop_features.On("level sync");
    LOG_INFO(Debug, "Bloodborne party guests keep their own level (no effect {})", LevelSyncEffect);
}

} // namespace Coop

namespace {

// Both NPC creation sites read the flag with `mov reg, [rax+0x145]` and test bit 7 right after.
// Pointing rax at an all-zero stand-in row for that read keeps the NPC a live character.
constexpr u64 NpcWanderCheckOffset = 0x018E4FB6;
constexpr std::array<u8, 9> NpcWanderCheck{0x8B, 0x88, 0x45, 0x01, 0x00, 0x00, 0xF6, 0xC1, 0x80};
constexpr u64 NpcWanderTypeCheckOffset = 0x018D8934;
constexpr std::array<u8, 8> NpcWanderTypeCheck{0x8B, 0x80, 0x45, 0x01, 0x00, 0x00, 0xA8, 0x80};
alignas(8) constexpr std::array<u8, NpcWanderGhostFlagOffset + 8> no_wander_npc_param{};

void PS4_SYSV_ABI KeepNpcsSolidForPartyGuests(u64, GuestRegisterSnapshot* registers) {
    if (registers->rax < 0x10000 ||
        !HasMemoryAccess(registers->rax + NpcWanderGhostFlagOffset, 4, MemoryProt::CpuRead)) {
        return;
    }
    const bool wander =
        (ReadValue<u32>(registers->rax, NpcWanderGhostFlagOffset) & NpcWanderGhostFlag) != 0;
    if (wander && !NpcTurnsIntoWanderGhost(InPartySession(), wander)) {
        registers->rax = reinterpret_cast<u64>(no_wander_npc_param.data());
    }
}

// The floor guard. A guest crashed at 0x018CB16A (access violation) after the host stayed in
// Cathedral Ward while the guest walked on and its game streamed in the next map block. The
// character floor update 0x018CB0E0 (per character) starts:
//     MOV RSI, [RBX + 0x288]            <- 0x018CB14F: the character's floor (a collision part)
//     TEST RSI, RSI / JZ no_floor
//     MOV RCX, [RSI + 0x20] / TEST RCX, RCX / JZ ...
//     MOV RDX, [RSI + 0x18] / MOV RDX, [RDX + 0x88]   <- the crash: [RSI + 0x18] was garbage
//     CMP [RDX], 3 / then [[RCX + 0xB8] + 0x0A] or [[RCX + 0x70] + 0x27]
// A character standing on the collision of a map block this game has since dropped keeps a stale
// floor pointer. Without co-op the fog walls keep everyone in one block. In a party the hook checks
// the chain the function is about to read and clears a floor that no longer reads back as one, so
// the function takes its own "no floor" path (as in mid-air). Best effort: a freed floor whose
// memory still reads sane passes. From deadinside28's bbport 0.6 (BBSC floor guard, by CLS_AMG).
constexpr u64 ChrFloorUpdateOffset = 0x018CB14F;
constexpr std::array<u8, 10> ChrFloorRead{0x48, 0x8B, 0xB3, 0x88, 0x02,
                                          0x00, 0x00, 0x48, 0x85, 0xF6};
constexpr size_t ChrFloorOffset = 0x288;
// Kaniel's crash 2026-10-17 (0x0191B7C0): the query "does any character stand on the part with
// entity id X" (0x0191B6E0, a lift-like object's update) reads every character's floor chain,
// disabled ones too. A remote player copy hidden by presence (disabled: its floor update no longer
// runs) kept the floor of a map block the host's game had dropped. The query's read of the floor
// gets the same check.
constexpr u64 StandsOnPartFloorOffset = 0x0191B79E;
constexpr std::array<u8, 10> StandsOnPartFloorRead{0x48, 0x8B, 0x8E, 0x88, 0x02,
                                                   0x00, 0x00, 0x48, 0x85, 0xC9};

// Kaniel's crash 2026-10-19: a call to 0x47F8 from 0x01524B24. A remote player's per-frame
// update (0x01524860) reads the object its physics holds as the one it stands on:
//     MOV R13, [RAX + 0xA8]             <- 0x01524B0C (RAX: the physics module)
//     TEST R13, R13 / JZ none
//     MOV RAX, [R13] / CALL [RAX + 0x18]
// Lisara's copy, hidden while she was on another map, kept an object of the map block Kaniel's
// game had since dropped; the memory was reused and the "table" pointed at the heap. A game
// object's table is in the game's image: an object whose table is not is gone, and the physics
// lets go of it (as with none).
constexpr u64 ContactObjectReadOffset = 0x01524B0C;
constexpr std::array<u8, 7> ContactObjectRead{0x4C, 0x8B, 0xA8, 0xA8, 0x00, 0x00, 0x00};
u64 guard_image_base = 0;
u64 guard_image_size = 0;

bool ReadableAt(u64 address, size_t size) {
    return address >= 0x10000 && HasMemoryAccess(address, size, MemoryProt::CpuRead);
}

bool FloorReadsBack(u64 floor) {
    if (!ReadableAt(floor, 0x28)) {
        return false;
    }
    const u64 part = ReadValue<u64>(floor, 0x20);
    if (part == 0) {
        return true; // the function does not follow the chain for this floor
    }
    const u64 block = ReadValue<u64>(floor, 0x18);
    if (!ReadableAt(block, 0x90)) {
        return false;
    }
    const u64 version = ReadValue<u64>(block, 0x88);
    if (!ReadableAt(version, 4)) {
        return false;
    }
    const u32 layout = ReadValue<u32>(version, 0);
    if (layout > 0x10) {
        return false; // a real map layout version is a small number
    }
    return layout >= 3 ? ReadableAt(ReadValue<u64>(part, 0xB8), 0x0B)
                       : ReadableAt(ReadValue<u64>(part, 0x70), 0x28);
}

void ClearStaleFloor(u64 character) {
    if (!ReadableAt(character + ChrFloorOffset, sizeof(u64))) {
        return;
    }
    const u64 floor = ReadValue<u64>(character, ChrFloorOffset);
    if (floor == 0 || FloorReadsBack(floor)) {
        return;
    }
    WriteValue<u64>(character, ChrFloorOffset, 0);
    // One line per character (the last 64 seen).
    static std::mutex logged_mutex;
    static std::array<u64, 64> logged{};
    static size_t logged_next = 0;
    std::scoped_lock lock{logged_mutex};
    if (std::ranges::find(logged, character) != logged.end()) {
        return;
    }
    logged[logged_next] = character;
    logged_next = (logged_next + 1) % logged.size();
    LOG_WARNING(Debug,
                "Bloodborne floor guard: character {:#x} stood on a floor that is gone ({:#x}, map "
                "{:#x}): floor cleared instead of a crash",
                character, floor, GetCurrentPackedMap());
}

void PS4_SYSV_ABI ClearFloorThatIsGone(u64, GuestRegisterSnapshot* registers) {
    if (InPartySession()) {
        ClearStaleFloor(registers->rbx);
    }
}

void PS4_SYSV_ABI ClearContactObjectThatIsGone(u64, GuestRegisterSnapshot* registers) {
    if (!InPartySession()) {
        return;
    }
    const u64 physics = registers->rax;
    if (!ReadableAt(physics + PhysicsContactObjectOffset, sizeof(u64))) {
        return;
    }
    const u64 object = ReadValue<u64>(physics, PhysicsContactObjectOffset);
    if (object == 0 ||
        (ReadableAt(object, sizeof(u64)) &&
         InGameImage(ReadValue<u64>(object, 0), guard_image_base, guard_image_size))) {
        return;
    }
    WriteValue<u64>(physics, PhysicsContactObjectOffset, 0);
    static std::atomic<int> logged{0};
    if (logged.fetch_add(1) < 8) {
        LOG_WARNING(Debug,
                    "Bloodborne floor guard: physics {:#x} held an object that is gone ({:#x}, map "
                    "{:#x}): let go of instead of a crash",
                    physics, object, GetCurrentPackedMap());
    }
}

// The game's per-character receive of a remote player's state (0x01A6D390, rdi: the character's
// network state, whose first word is its member's identity; channel 6 through 0x01789200, which
// frees each packet it takes off the member's queue). Two copies of one member (Kaniel's playtest
// 2026-10-19) run it on two character threads against one queue: a double free (0x020B56D2) in
// every other game when he joined back. Only the copy seen last takes them; the other's receive
// takes nothing (returns 0, as with an empty queue).
constexpr u64 MemberReceiveOffset = 0x01A6D390;
constexpr std::array<u8, 6> MemberReceivePrologue{0x55, 0x48, 0x89, 0xE5, 0x41, 0x57};

bool PS4_SYSV_ABI SkipSecondCopyReceive(u64, GuestRegisterSnapshot* registers) {
    if (!InPartySession()) {
        return false;
    }
    const u64 network = registers->rdi;
    const u64 identity = ReadableAt(network, sizeof(u64)) ? ReadValue<u64>(network, 0) : 0;
    static std::mutex takers_mutex;
    static MemberPacketTakers takers;
    bool takes = true;
    {
        std::scoped_lock lock{takers_mutex};
        takes = takers.Takes(network, identity, SteadyMilliseconds());
    }
    if (takes) {
        return false;
    }
    static std::atomic<int> logged{0};
    if (logged.fetch_add(1) < 8) {
        LOG_WARNING(Debug,
                    "Bloodborne packet guard: a second copy of member {:#x} (state {:#x}) takes "
                    "none of its packets (the newest copy does)",
                    identity, network);
    }
    registers->rax = 0;
    return true;
}

void PS4_SYSV_ABI ClearQueriedFloorThatIsGone(u64, GuestRegisterSnapshot* registers) {
    // Only a disabled character's: an enabled one's floor update checks its own every frame, and
    // this query runs for every character for every such object.
    const u64 character = registers->rsi;
    if (InPartySession() && character >= 0x10000 && IsCharacterDisabled(character)) {
        ClearStaleFloor(character);
    }
}

// The flag block guard (rig 2026-10-16: a removed member invited back, and host-crash-apart's
// guest, crashed at 0x013CFFBA when their game joined a session the second time). The event flag
// manager's read of a value of several bits, 0x013CFD80, checks the block holding a value inside
// one block and reads 0 when it is missing; a value over two blocks looks both up (the first in
// rax, the second in rdx) and reads them unchecked. Here a missing block reads as zeros, as a
// missing block of a one-block value does.
constexpr u64 FlagValueFirstBlockOffset = 0x013CFF79;
constexpr std::array<u8, 9> FlagValueFirstBlock{0x41, 0xFF, 0xC9, 0x44, 0x89,
                                                0xC9, 0xC1, 0xE9, 0x03};
constexpr u64 FlagValueSecondBlockOffset = 0x013CFFEC;
constexpr std::array<u8, 9> FlagValueSecondBlock{0x45, 0x29, 0xD0, 0x41, 0x8D,
                                                 0x58, 0xFF, 0x89, 0xD8};
// A block's bytes never reach 0x10000 (ReadFlagStore's bound).
alignas(64) constexpr std::array<u8, 0x10000> MissingFlagBlock{};

void NoteMissingFlagBlock(const char* which) {
    static std::atomic<int> logged{0};
    if (logged.fetch_add(1) < 8) {
        LOG_WARNING(Debug,
                    "Bloodborne flag block guard: a flag value's {} block is missing: read as "
                    "zeros instead of a crash (map {:#x})",
                    which, GetCurrentPackedMap());
    }
}

void PS4_SYSV_ABI GuardFlagValueFirstBlock(u64, GuestRegisterSnapshot* registers) {
    const u64 zeros = reinterpret_cast<u64>(MissingFlagBlock.data());
    if (registers->rax == 0) {
        NoteMissingFlagBlock("first");
    }
    if (registers->rdx == 0) {
        NoteMissingFlagBlock("second");
    }
    registers->rax = GuardedFlagBlock(registers->rax, zeros);
    registers->rdx = GuardedFlagBlock(registers->rdx, zeros);
}

void PS4_SYSV_ABI GuardFlagValueSecondBlock(u64, GuestRegisterSnapshot* registers) {
    if (registers->rdx == 0) {
        NoteMissingFlagBlock("second");
    }
    registers->rdx =
        GuardedFlagBlock(registers->rdx, reinterpret_cast<u64>(MissingFlagBlock.data()));
}

} // namespace

namespace Coop {

void InstallFlagBlockGuard(u64 base, u64 image_size) {
    if (!party_controller) {
        return;
    }
    // Both or neither: the second alone leaves the first block's read unguarded.
    const auto install = [&](u64 offset, std::span<const u8> bytes, GuestRegisterEditHook hook) {
        return offset + bytes.size() <= image_size &&
               InstallCoopHook(reinterpret_cast<void*>(base + offset), bytes, 0, hook);
    };
    if (!install(FlagValueFirstBlockOffset, FlagValueFirstBlock, GuardFlagValueFirstBlock)) {
        coop_features.Skipped("flag block guard",
                              SiteFailure(base, image_size, FlagValueFirstBlockOffset));
        return;
    }
    if (!install(FlagValueSecondBlockOffset, FlagValueSecondBlock, GuardFlagValueSecondBlock)) {
        RemoveGuestCodeHook(reinterpret_cast<void*>(base + FlagValueFirstBlockOffset));
        coop_features.Skipped("flag block guard",
                              SiteFailure(base, image_size, FlagValueSecondBlockOffset));
        return;
    }
    coop_features.On("flag block guard");
    LOG_INFO(Debug,
             "Bloodborne flag block guard: a flag value over a missing block reads as zeros");
}

void InstallFloorGuard(u64 base, u64 image_size) {
    if (!party_controller) {
        return;
    }
    if (ChrFloorUpdateOffset + ChrFloorRead.size() > image_size ||
        !InstallCoopHook(reinterpret_cast<void*>(base + ChrFloorUpdateOffset), ChrFloorRead, 0,
                         ClearFloorThatIsGone)) {
        LOG_ERROR(Debug,
                  "Bloodborne floor guard unavailable; a stale floor can still crash a guest");
        coop_features.Skipped("floor guard", SiteFailure(base, image_size, ChrFloorUpdateOffset));
        return;
    }
    if (StandsOnPartFloorOffset + StandsOnPartFloorRead.size() > image_size ||
        !InstallCoopHook(reinterpret_cast<void*>(base + StandsOnPartFloorOffset),
                         StandsOnPartFloorRead, 0, ClearQueriedFloorThatIsGone)) {
        coop_features.Skipped("floor guard",
                              SiteFailure(base, image_size, StandsOnPartFloorOffset));
        return;
    }
    guard_image_base = base;
    guard_image_size = image_size;
    if (ContactObjectReadOffset + ContactObjectRead.size() > image_size ||
        !InstallCoopHook(reinterpret_cast<void*>(base + ContactObjectReadOffset), ContactObjectRead,
                         0, ClearContactObjectThatIsGone)) {
        coop_features.Skipped("floor guard",
                              SiteFailure(base, image_size, ContactObjectReadOffset));
        return;
    }
    coop_features.On("floor guard");
    LOG_INFO(Debug, "Bloodborne floor guard: a floor of a map block that is gone is cleared");
}

void InstallMemberPacketGuard(u64 base, u64 image_size) {
    if (!party_controller) {
        return;
    }
    if (MemberReceiveOffset + MemberReceivePrologue.size() > image_size ||
        !InstallCoopHook(reinterpret_cast<void*>(base + MemberReceiveOffset), MemberReceivePrologue,
                         0, SkipSecondCopyReceive)) {
        LOG_ERROR(Debug, "Bloodborne packet guard unavailable; two copies of one member can still "
                         "crash a game");
        coop_features.Skipped("packet guard", SiteFailure(base, image_size, MemberReceiveOffset));
        return;
    }
    coop_features.On("packet guard");
    LOG_INFO(Debug, "Bloodborne packet guard: one copy of each member takes its packets");
}

void InstallSolidNpcsInParty(u64 base, u64 image_size) {
    if (!party_controller) {
        return;
    }
    for (const auto& [offset, bytes] :
         {std::pair<u64, std::span<const u8>>{NpcWanderCheckOffset, NpcWanderCheck},
          std::pair<u64, std::span<const u8>>{NpcWanderTypeCheckOffset, NpcWanderTypeCheck}}) {
        if (offset + bytes.size() > image_size ||
            !InstallCoopHook(reinterpret_cast<void*>(base + offset), bytes, 0,
                             KeepNpcsSolidForPartyGuests)) {
            LOG_ERROR(Debug, "Bloodborne NPC hooks unavailable; party guests see NPCs as ghosts");
            coop_features.Skipped("solid NPCs", SiteFailure(base, image_size, offset));
            return;
        }
    }
    coop_features.On("solid NPCs");
    LOG_INFO(Debug, "Bloodborne party guests see NPCs as the host does");
}
std::array<std::atomic<u64>, ItemModuleChecks.size()> item_module_check_hits{};

void PS4_SYSV_ABI CountItemModuleCheck(u64 tag, const GuestRegisterSnapshot*) {
    if (tag < item_module_check_hits.size()) {
        item_module_check_hits[tag].fetch_add(1, std::memory_order_relaxed);
    }
}

} // namespace Coop

namespace {

// Shared loot (see SoloMapItemStandIn): each hook points the checked register at the stand-in.
// In every spot the register is reloaded or overwritten right after the branch.
constexpr u64 MapItemCreateStateCheckOffset = 0x017DC3CF;   // cmp [rax+0x124], 6
constexpr u64 MapItemCreateSessionCheckOffset = 0x017DC3DC; // cmp [rdx+0x98], -1
constexpr std::array<u8, 7> MapItemCreateSessionCheck{0x83, 0xBA, 0x98, 0x00, 0x00, 0x00, 0xFF};
// A new map item gets its own handle (counter << 4 | kind in [item+0x20]) unless this player is
// a summoned guest, which waits for the host's; without one the pickup prompt never finds it.
constexpr u64 MapItemHandleStateCheckOffset = 0x017D52D2;   // cmp [rax+0x124], 6
constexpr u64 MapItemHandleSessionCheckOffset = 0x017D52DB; // cmp [r15+0x98], -1
constexpr std::array<u8, 8> MapItemHandleSessionCheck{0x41, 0x83, 0xBF, 0x98,
                                                      0x00, 0x00, 0x00, 0xFF};
// Kaniel's playtest 2026-10-16: an enemy's death drop (0x017D5320, the MapItemMan +0x38 list;
// counter +0x10) has the same guest branch at its handle: on a guest every drop kept the kind-only
// handle 0x2, so one pickup took them all (krolebord found every kill's drop on the first wolf).
constexpr u64 DropHandleStateCheckOffset = 0x017D5642;   // cmp [rax+0x124], 6
constexpr u64 DropHandleSessionCheckOffset = 0x017D564B; // cmp [r15+0x98], -1
// Kaniel's playtest 2026-10-16: only the host got the Cathedral Ward hag's loot. An enemy's item
// lot roll (0x017DD0B0) on a summoned guest (its flag at [rbp-0x208]) keeps only the lot's items
// for co-op phantoms, so the one-time drops never came; the guest's roll as solo play's (each
// world's own lot flag still gives a one-time item once). rax and r15 are reloaded after.
constexpr u64 DropLotStateCheckOffset = 0x017DD17C;                       // cmp [rax+0x124], 6
constexpr u64 DropLotSessionCheckOffset = 0x017DD186;                     // cmp [r15+0x98], -1
constexpr std::array<u64, 2> MapItemPickupChecks{0x017D6831, 0x017D6AEF}; // [rax+0x1592], 0
// Picking up a networked map item (0x017D8570): a summoned guest only asks the host for it.
constexpr u64 MapItemTakeStateCheckOffset = 0x017D85F4; // cmp [rax+0x124], 6
constexpr std::array<u8, 7> MapItemPickupCheck{0x80, 0xB8, 0x92, 0x15, 0x00, 0x00, 0x00};

u64 SoloMapItemStandInAddress() {
    return reinterpret_cast<u64>(SoloMapItemStandIn().data());
}

void PS4_SYSV_ABI CreateMapItemsForPartyGuest(u64, GuestRegisterSnapshot* registers) {
    SetGuestPickupsBeforeMapItems();
    if (GuestHandlesMapItemsAlone(party_controller.has_value())) {
        registers->rax = SoloMapItemStandInAddress();
    }
}

void PS4_SYSV_ABI CreateMapItemsInJoinedSession(u64, GuestRegisterSnapshot* registers) {
    if (GuestHandlesMapItemsAlone(party_controller.has_value())) {
        registers->rdx = SoloMapItemStandInAddress();
    }
}

// A drop's handle check (DropHandleStateCheckOffset): as a solo player's.
void PS4_SYSV_ABI GiveDropsAHandle(u64, GuestRegisterSnapshot* registers) {
    if (GuestHandlesMapItemsAlone(party_controller.has_value())) {
        registers->rax = SoloMapItemStandInAddress();
    }
}

void PS4_SYSV_ABI GiveMapItemsAHandleInJoinedSession(u64, GuestRegisterSnapshot* registers) {
    if (GuestHandlesMapItemsAlone(party_controller.has_value())) {
        registers->r15 = SoloMapItemStandInAddress();
    }
}

void PS4_SYSV_ABI LetPartyGuestPickUpMapItems(u64, GuestRegisterSnapshot* registers) {
    if (GuestHandlesMapItemsAlone(party_controller.has_value())) {
        registers->rax = SoloMapItemStandInAddress();
    }
}

// The map item packet just received ([rbp-0x1A0], see MapItemPacketHeaderSeen): the hook runs
// before `mov eax, [rbp-0x1A0]; cmp ax, 1`, so an emptied header makes the loop skip it.
constexpr u64 MapItemPacketHeaderOffset = 0x017D33E1;
constexpr std::array<u8, 10> MapItemPacketHeader{0x8B, 0x85, 0x60, 0xFE, 0xFF,
                                                 0xFF, 0x66, 0x83, 0xF8, 0x01};

void PS4_SYSV_ABI IgnorePartyMapItemSync(u64, GuestRegisterSnapshot* registers) {
    const u64 header_address = registers->rbp - 0x1A0;
    const u32 header = ReadValue<u32>(header_address, 0);
    const u32 seen = MapItemPacketHeaderSeen(header, party_controller.has_value());
    if (seen != header) {
        WriteValue(header_address, 0, seen);
        static std::array<std::atomic<bool>, 8> logged{};
        const u32 kind = header >> 16;
        if (kind < logged.size() && !logged[kind].exchange(true)) {
            LOG_INFO(Debug, "Bloodborne party: ignored a map item sync packet (kind {})", kind);
        }
    }
}

// Full echoes for party guests (see RetargetRipLoad): the guest rate loads in the kill reward
// (0x018FC947), its calculation (0x018FCAA7) and the boss reward (0x01335589, a phantom's echoes
// x0.5 while more than one player is in the session) are pointed at the 1.0 at 0x04927EBC.
constexpr std::array<u64, 3> GuestEchoRateLoads{0x018FC947, 0x018FCAA7, 0x01335589};
constexpr std::array<std::array<u8, 8>, 3> GuestEchoRateLoadBytes{{
    {0xC5, 0xFA, 0x10, 0x1D, 0x71, 0xB5, 0x02, 0x03},
    {0xC5, 0xFA, 0x10, 0x1D, 0x21, 0xB4, 0x02, 0x03},
    {0xC5, 0xFA, 0x59, 0x05, 0x8B, 0x05, 0x5F, 0x03},
}};
constexpr u64 FullEchoRateOffset = 0x04927EBC;
constexpr std::array<u8, 11> AddSoulsPrologue{0x55, 0x48, 0x89, 0xE5, 0x41, 0x56,
                                              0x53, 0x48, 0x83, 0xEC, 0x10};

void PS4_SYSV_ABI NoteAddSouls(u64, GuestRegisterSnapshot* registers) {
    const u64 caller = ReadValue<u64>(registers->rsp, 0);
    const s32 amount = static_cast<s32>(registers->rsi);
    // The enemy kill reward keeps the dying character in r14 (see QuietKills): a party kill the
    // kill step put down again pays nothing, to this member or the party.
    if (amount > 0 && caller == image_base + EnemyKillRewardOffset &&
        TakesQuietKillReward(registers->r14)) {
        registers->rsi &= ~u64{0xFFFFFFFF};
        LOG_INFO(Debug, "Bloodborne echoes: +{} for {:#x} not paid: a party kill put down again",
                 amount, ReadValue<u32>(registers->r14, 0x08));
        return;
    }
    LOG_INFO(Debug, "Bloodborne echoes: +{} (rates {}) from {:#x}", amount, registers->rcx & 0xFF,
             caller >= image_base ? caller - image_base : caller);
    // A kill reward: the game paid this member (a post of the same kill is not owed here).
    if (amount <= 0 || caller < image_base || !SharesKillEchoes(caller - image_base, true)) {
        return;
    }
    // Invasions: an invader shares no kill echoes (it is in no party; the server drops them too).
    if (Invading()) {
        return;
    }
    const KillEchoes kill{amount, CurrentPlace().map};
    const bool posts = PostsKillEchoesInSession();
    std::scoped_lock lock{echo_board_mutex};
    echo_ledger.Paid(kill, SteadyMilliseconds());
    if (posts) {
        echoes_to_post.push_back(kill);
    }
}

void GiveGuestsFullEchoes(u64 base, u64 image_size) {
    for (size_t index = 0; index < GuestEchoRateLoads.size(); ++index) {
        const u64 offset = GuestEchoRateLoads[index];
        if (offset + 8 > image_size ||
            !std::ranges::equal(std::span<const u8>{reinterpret_cast<const u8*>(base + offset), 8},
                                GuestEchoRateLoadBytes[index])) {
            LOG_ERROR(Debug, "Bloodborne guest echo rate not found at {:#x}", offset);
            coop_features.Skipped("full echoes",
                                  fmt::format("signature mismatch at {:#x}", offset));
            return;
        }
    }
    for (size_t index = 0; index < GuestEchoRateLoads.size(); ++index) {
        const u64 offset = GuestEchoRateLoads[index];
        const auto patched =
            RetargetRipLoad(GuestEchoRateLoadBytes[index], offset, FullEchoRateOffset);
        std::memcpy(reinterpret_cast<void*>(base + offset), patched.data(), patched.size());
    }
    coop_features.On("full echoes");
    LOG_INFO(Debug, "Bloodborne party guests get full echoes from kills");
    if (AddSoulsOffset + AddSoulsPrologue.size() > image_size ||
        !InstallCoopHook(reinterpret_cast<void*>(base + AddSoulsOffset), AddSoulsPrologue, 0,
                         NoteAddSouls)) {
        LOG_ERROR(Debug, "Bloodborne echo diagnostics unavailable");
        coop_features.Skipped("echo diagnostics", SiteFailure(base, image_size, AddSoulsOffset));
    } else {
        coop_features.On("echo diagnostics");
    }
}

// See MenuSeesAClient: the condition ends `or cl, al; movzx esi, cl` (al: matching state 6,
// cl: a joined session) and returns esi; cleared before that, the menu sees no client.
constexpr u64 MenuClientConditionOffset = 0x01A13FE5;
constexpr std::array<u8, 5> MenuClientCondition{0x08, 0xC1, 0x0F, 0xB6, 0xF1};

void PS4_SYSV_ABI MenusTreatPartyGuestsAsHosts(u64, GuestRegisterSnapshot* registers) {
    const bool client = (registers->rax & 0xFF) != 0 || (registers->rcx & 0xFF) != 0;
    if (!MenuSeesAClient(party_controller.has_value(), client)) {
        registers->rax &= ~u64{0xFF};
        registers->rcx &= ~u64{0xFF};
    }
}

} // namespace

namespace Coop {

void InstallSharedLoot(u64 base, u64 image_size) {
    if (!party_controller) {
        return;
    }
    GiveGuestsFullEchoes(base, image_size);
    if (MenuClientConditionOffset + MenuClientCondition.size() > image_size ||
        !InstallCoopHook(reinterpret_cast<void*>(base + MenuClientConditionOffset),
                         MenuClientCondition, 0, MenusTreatPartyGuestsAsHosts)) {
        LOG_ERROR(Debug, "Bloodborne menu client hook unavailable");
        coop_features.Skipped("menu client",
                              SiteFailure(base, image_size, MenuClientConditionOffset));
    } else {
        coop_features.On("menu client");
    }
    if (channel_table_installed) {
        // The channel table already prevents a host pickup from clearing the guest's items.
        coop_features.On("map item sync");
    } else if (MapItemPacketHeaderOffset + MapItemPacketHeader.size() > image_size ||
               !InstallCoopHook(reinterpret_cast<void*>(base + MapItemPacketHeaderOffset),
                                MapItemPacketHeader, 0, IgnorePartyMapItemSync)) {
        LOG_ERROR(Debug, "Bloodborne map item sync hook unavailable; the host's sync clears the "
                         "guest's map items");
        coop_features.Skipped("map item sync",
                              SiteFailure(base, image_size, MapItemPacketHeaderOffset));
    } else {
        coop_features.On("map item sync");
    }
    struct LootHook {
        u64 offset;
        std::span<const u8> bytes;
        GuestRegisterEditHook edit;
    };
    const std::array<LootHook, 9> loot_hooks{{
        {MapItemCreateStateCheckOffset, GuestStateCheck, CreateMapItemsForPartyGuest},
        {MapItemCreateSessionCheckOffset, MapItemCreateSessionCheck, CreateMapItemsInJoinedSession},
        {MapItemHandleStateCheckOffset, GuestStateCheck, CreateMapItemsForPartyGuest},
        {MapItemTakeStateCheckOffset, GuestStateCheck, CreateMapItemsForPartyGuest},
        {MapItemHandleSessionCheckOffset, MapItemHandleSessionCheck,
         GiveMapItemsAHandleInJoinedSession},
        {DropHandleStateCheckOffset, GuestStateCheck, GiveDropsAHandle},
        {DropHandleSessionCheckOffset, MapItemHandleSessionCheck,
         GiveMapItemsAHandleInJoinedSession},
        {DropLotStateCheckOffset, GuestStateCheck, GiveDropsAHandle},
        {DropLotSessionCheckOffset, MapItemHandleSessionCheck, GiveMapItemsAHandleInJoinedSession},
    }};
    const auto hook = [base, image_size](u64 offset, std::span<const u8> bytes,
                                         GuestRegisterEditHook edit) {
        return offset + bytes.size() <= image_size &&
               InstallCoopHook(reinterpret_cast<void*>(base + offset), bytes, 0, edit);
    };
    const auto loot_failed = std::ranges::find_if(loot_hooks, [&hook](const LootHook& loot) {
        return !hook(loot.offset, loot.bytes, loot.edit);
    });
    if (loot_failed != loot_hooks.end()) {
        LOG_ERROR(Debug, "Bloodborne map item hooks unavailable; party guests see no map items");
        coop_features.Skipped("shared loot", SiteFailure(base, image_size, loot_failed->offset));
        return;
    }
    coop_features.On("shared loot");
    std::optional<u64> pickup_failed;
    for (const u64 offset : MapItemPickupChecks) {
        if (!hook(offset, MapItemPickupCheck, LetPartyGuestPickUpMapItems)) {
            LOG_ERROR(Debug, "Bloodborne map item pickup hook unavailable at {:#x}", offset);
            pickup_failed = pickup_failed.value_or(offset);
        }
    }
    if (pickup_failed) {
        coop_features.Skipped("map item pickup", SiteFailure(base, image_size, *pickup_failed));
    } else {
        coop_features.On("map item pickup");
    }
    LOG_INFO(Debug, "Bloodborne party guests see and pick up their own map items");
}

} // namespace Coop

} // namespace Core::Bloodborne
