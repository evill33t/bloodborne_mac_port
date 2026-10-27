// SPDX-FileCopyrightText: Copyright 2026 shadNet Project
// SPDX-License-Identifier: GPL-2.0-or-later

// Invasions (docs/INVASIONS.md): a solo player invades a team's world as a hostile phantom. The
// rules of who may be invaded and when live here; the service (bloodborne_world.h) matches,
// routes and ends the invasions by them.
#pragma once

#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "coop/coop_types.h"

namespace BloodborneCoop {

// The Hunter's Dream (m21_00): never invaded.
constexpr u32 HuntersDreamMap = 0x15000000;
// The chalice dungeons (m29_xx): not in phase 1.
constexpr u32 ChaliceArea = 0x1D;

// The game's player slots (the host and three): an invader needs a free one, so a session of
// this many is never invaded.
constexpr size_t InvasionMaxPlayers = 4;
// "An invader approaches": the team's notice before the invader joins.
constexpr s64 InvasionNoticeMs = 10'000;
// An invasion longer than this ends (the invader goes home).
constexpr s64 InvasionMaxMs = 10 * 60 * 1000;
// After an invasion a team world rests from every invader, and from that invader longer.
constexpr s64 InvasionTeamCooldownMs = 15 * 60 * 1000;
constexpr s64 InvasionPairCooldownMs = 60 * 60 * 1000;
// An invader's search with no world found ends.
constexpr s64 InvasionSearchMs = 5 * 60 * 1000;
// How often the searches look again.
constexpr s64 InvasionMatchMs = 1'000;

// A map an invader may enter: out in the world (not loading, not the Dream, not a chalice).
constexpr bool InvadableMap(u32 map) {
    return map != 0 && map != HuntersDreamMap && (map >> 24) != ChaliceArea;
}

// Retail's band, roughly: within 20 levels plus 20% of the member's. A level not known (0) is in
// every band.
constexpr bool InLevelBand(u32 invader, u32 member) {
    if (invader == 0 || member == 0) {
        return true;
    }
    const u32 gap = invader > member ? invader - member : member - invader;
    return gap <= 20 + member / 5;
}

// A team world as the matcher sees it.
struct InvasionTarget {
    struct Member {
        u32 map = 0;   // its last map (0: loading)
        u32 level = 0; // its last vitals' level (0: not said)
        bool boss = false;
    };
    std::string world;
    bool allows_invaders = false; // its team's leader allowed them
    bool session_open = false;    // its team started and its host's session is open
    bool invaded = false;         // an invader is on its way or in it
    size_t players = 0;           // the games in its session now
    std::vector<Member> members;
};

class InvasionCooldowns {
public:
    void Ended(const std::string& world, const std::string& invader, s64 now_ms);
    bool TeamRests(const std::string& world, s64 now_ms) const;
    bool PairRests(const std::string& world, const std::string& invader, s64 now_ms) const;
    // The cooldowns over go.
    void Expire(s64 now_ms);
    size_t Size() const {
        return team_until.size() + pair_until.size();
    }

private:
    std::map<std::string, s64> team_until;
    std::map<std::pair<std::string, std::string>, s64> pair_until;
};

// Why `invader` (of `level`) may not invade `target` now; empty: it may.
std::string_view InvasionRefusal(const InvasionTarget& target, const std::string& invader,
                                 u32 level, const InvasionCooldowns& cooldowns, s64 now_ms);

} // namespace BloodborneCoop
