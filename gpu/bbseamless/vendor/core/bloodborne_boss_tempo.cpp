// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// Kaniel 2026-10-28: the boss tempo host rule in the game (the pure part is in
// bloodborne_host_rules.cpp, BossTempoTuning): the AI's AddSubGoal hook shortens a boss's
// breathers on the game that runs it (its AI runs nowhere else), and MakeEnemiesHarder turns the
// boss rows of NpcParam faster on every member's game. The hook also logs a boss's breathers in
// the game's own tempo (Vanilla), so a rig run can compare the gaps.

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdlib>
#include <random>
#include <unordered_map>
#include <utility>

#include "common/logging/log.h"
#include "core/bloodborne_host_rules.h"
#include "core/bloodborne_re_internal.h"
#include "core/bloodborne_re_sites.h"
#include "core/memory.h"

namespace Core::Bloodborne {

namespace {

using namespace Coop;

// SHADPS4_BLOODBORNE_BOSS_TEMPO=gap,odds,turn,anim, the dev setting (a level that is on).
const std::optional<BossTempoTuning> tempo_tuned = [] {
    const char* setting = std::getenv("SHADPS4_BLOODBORNE_BOSS_TEMPO");
    return setting != nullptr ? ParseBossTempoTuning(setting) : std::nullopt;
}();

// The level this game plays by, as the game thread's step last read it (for the hook).
std::atomic<u8> tempo_level{0};

// The bosses' characters (NpcParam npcType 1) on this map, as the game thread's step last found
// them: the hook's whole check.
constexpr std::size_t MaxTempoBosses = 16;
std::array<std::atomic<u64>, MaxTempoBosses> tempo_bosses{};

std::atomic<u32> boss_goals_seen{0};
std::atomic<u32> boss_goals_changed{0};
std::atomic<u32> goal_lines{0};

bool IsTempoBoss(u64 character) {
    if (character < 0x10000) {
        return false;
    }
    for (const auto& boss : tempo_bosses) {
        if (boss.load(std::memory_order_relaxed) == character) {
            return true;
        }
    }
    return false;
}

const char* GoalName(s32 goal) {
    switch (goal) {
    case GoalCommonWait:
        return "Wait";
    case GoalCommonTurn:
        return "Turn";
    case GoalCommonLeaveTarget:
        return "LeaveTarget";
    case GoalCommonSidewayMove:
        return "SidewayMove";
    }
    return "?";
}

// The character's NpcParam npcType, as NpcTypeOf reads it ([vtable +0x168 (character) +8] is its
// row).
std::optional<u8> TempoNpcType(u64 character);

} // namespace

namespace Coop {

void PS4_SYSV_ABI BossTempoGoal(u64 tag, const GuestRegisterSnapshot* registers) {
    const s32 goal = static_cast<s32>(static_cast<u32>(registers->rsi));
    if (!IsBossTempoGoal(goal)) {
        return;
    }
    const u64 parent = registers->rdi;
    const u64 ai = parent >= 0x10000 ? ReadValue<u64>(parent, 0) : 0;
    const u64 character = ai >= 0x10000 ? ReadValue<u64>(ai, AiCharacterOffset) : 0;
    if (!IsTempoBoss(character)) {
        return;
    }
    const u64 at =
        registers->rbp + static_cast<u64>(tag == AddSubGoalFrontTag ? AddSubGoalFrontLifeFrameOffset
                                                                    : AddSubGoalLifeFrameOffset);
    const auto life = TryRead<float>(at, 0);
    if (!life) {
        return;
    }
    const BossTempo level = static_cast<BossTempo>(tempo_level.load(std::memory_order_relaxed));
    const BossTempoTuning tuning = BossTempoTuningFor(level, tempo_tuned);
    thread_local std::minstd_rand random{std::random_device{}()};
    const u32 roll = static_cast<u32>(random() % 100);
    const float now = BossGoalLife(goal, *life, tuning, roll);
    const bool changed = now != *life && WriteValue(at, 0, now);
    boss_goals_seen.fetch_add(1, std::memory_order_relaxed);
    if (changed) {
        boss_goals_changed.fetch_add(1, std::memory_order_relaxed);
    }
    // Every one while tracing (the rig's gap count), else the first few.
    if (CoopTraceOn() || goal_lines.fetch_add(1, std::memory_order_relaxed) < 20) {
        LOG_INFO(Debug,
                 "Bloodborne boss tempo: boss {:#x} (think {}) goal {} {} life {:.2f} -> {:.2f} "
                 "({})",
                 ReadValue<u32>(character, 0x08), ReadValue<s32>(ai, 0x28), goal, GoalName(goal),
                 *life, now, BossTempoName(level));
    }
}

} // namespace Coop

namespace {

std::optional<u8> TempoNpcType(u64 character) {
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

} // namespace

namespace Coop {

BossTempoTuning ActiveBossTempoTuning() {
    return BossTempoTuningFor(ActiveHostRules().boss_tempo, tempo_tuned);
}

// Once a second on the game's thread: the level, and the bosses on this map.
void TickBossTempo() {
    static s64 next_ms = 0;
    const s64 now_ms = SteadyMilliseconds();
    if (now_ms < next_ms) {
        return;
    }
    next_ms = now_ms + 1000;
    static bool told = false;
    if (tempo_tuned && !std::exchange(told, true)) {
        LOG_INFO(Debug,
                 "Bloodborne boss tempo: the dev setting tunes a level that is on: gap x{:.2f}, "
                 "breathers kept {:.0f}%, turn x{:.2f}, animations x{:.2f} (not used yet)",
                 tempo_tuned->gap, tempo_tuned->odds * 100.0f, tempo_tuned->turn,
                 tempo_tuned->anim);
    }
    const BossTempo level = ActiveHostRules().boss_tempo;
    if (tempo_level.exchange(static_cast<u8>(level)) != static_cast<u8>(level)) {
        const BossTempoTuning tuning = BossTempoTuningFor(level, tempo_tuned);
        LOG_INFO(Debug,
                 "Bloodborne boss tempo: {}: breathers x{:.2f}, {:.0f}% of them kept, turn "
                 "x{:.2f}",
                 BossTempoName(level), tuning.gap, tuning.odds * 100.0f, tuning.turn);
    }
    // A character's npcType never changes; a map's load makes new characters.
    static std::unordered_map<u64, bool> is_boss;
    static u32 map_seen = 0;
    const u32 map = PlayingMap();
    if (map != map_seen || is_boss.size() > 4096) {
        is_boss.clear();
        map_seen = map;
    }
    std::array<u64, MaxTempoBosses> found{};
    std::size_t count = 0;
    const u64 world = ReadValue<u64>(image_base + CandidateLocalStatePointerOffset, 0);
    if (world >= 0x10000 && map != 0) {
        ForEachWorldCharacter(world, [&](u64 character) {
            auto [at, fresh] = is_boss.try_emplace(character, false);
            if (fresh) {
                at->second = TempoNpcType(character) == NpcTypeBoss;
            }
            if (at->second && count < found.size()) {
                found[count++] = character;
            }
        });
    }
    std::sort(found.begin(), found.begin() + count);
    static std::array<u64, MaxTempoBosses> last{};
    if (found != last) {
        last = found;
        std::string handles;
        for (std::size_t i = 0; i < count; ++i) {
            handles += fmt::format("{}{:#x}", i == 0 ? "" : ", ", ReadValue<u32>(found[i], 0x08));
        }
        LOG_INFO(Debug,
                 "Bloodborne boss tempo: {} boss(es) on map {:#x}{}{} ({} goals seen, {} "
                 "changed)",
                 count, map, count != 0 ? ": " : "", handles, boss_goals_seen.load(),
                 boss_goals_changed.load());
    }
    for (std::size_t i = 0; i < tempo_bosses.size(); ++i) {
        tempo_bosses[i].store(found[i], std::memory_order_relaxed);
    }
}

} // namespace Coop

} // namespace Core::Bloodborne
