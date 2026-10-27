// SPDX-FileCopyrightText: Copyright 2026 shadNet Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "coop/invasions.h"

#include <algorithm>

namespace BloodborneCoop {

u32 InvasionVictoryEchoes(u32 level, bool invader_won) {
    if (level < 10 || level > 544) {
        return 0;
    }
    const s64 l = level;
    const s64 previous_cost = std::max<s64>(0,
        (2 * l * l * l + 306 * l * l + 10'560 * l - 89'500) / 100);
    return static_cast<u32>(previous_cost * (invader_won ? 10 : 30) / 100);
}

void InvasionCooldowns::Ended(const std::string& world, const std::string& invader, s64 now_ms) {
    team_until[world] = now_ms + InvasionTeamCooldownMs;
    pair_until[{world, invader}] = now_ms + InvasionPairCooldownMs;
}

bool InvasionCooldowns::TeamRests(const std::string& world, s64 now_ms) const {
    const auto found = team_until.find(world);
    return found != team_until.end() && now_ms < found->second;
}

bool InvasionCooldowns::PairRests(const std::string& world, const std::string& invader,
                                  s64 now_ms) const {
    const auto found = pair_until.find({world, invader});
    return found != pair_until.end() && now_ms < found->second;
}

void InvasionCooldowns::Expire(s64 now_ms) {
    std::erase_if(team_until, [&](const auto& entry) { return now_ms >= entry.second; });
    std::erase_if(pair_until, [&](const auto& entry) { return now_ms >= entry.second; });
}

std::string_view InvasionRefusal(const InvasionTarget& target, const std::string& invader,
                                 u32 level, const InvasionCooldowns& cooldowns, s64 now_ms) {
    if (!target.allows_invaders) {
        return "its team does not allow invaders";
    }
    if (!target.session_open) {
        return "its session is not open";
    }
    if (target.invaded) {
        return "it has an invader";
    }
    if (target.players >= InvasionMaxPlayers) {
        return "its session is full";
    }
    if (cooldowns.TeamRests(target.world, now_ms)) {
        return "it rests after an invasion";
    }
    if (cooldowns.PairRests(target.world, invader, now_ms)) {
        return "this invader was there lately";
    }
    if (std::ranges::any_of(target.members, [](const auto& m) { return m.boss; })) {
        return "a boss fight runs";
    }
    const bool someone = std::ranges::any_of(target.members, [&](const auto& m) {
        return InvadableMap(m.map) && InLevelBand(level, m.level);
    });
    return someone ? std::string_view{} : "nobody in its level band out in the world";
}

} // namespace BloodborneCoop
