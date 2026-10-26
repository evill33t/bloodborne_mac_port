// SPDX-FileCopyrightText: Copyright 2026 shadNet Project
// SPDX-License-Identifier: GPL-2.0-or-later

// Team duels (Kaniel 2026-10-25), the server's side: one referee per party, shared by shadNet and
// the games (the texts). The messages are coop.proto's Duel*:
//   - a member proposes from the team screen (DuelRequest PROPOSE): the members whose games are
//     linked and stand on its map are asked, the proposer is in at once;
//   - each asked member answers on its own game; once all answered (or DuelAskMs ran out) and at
//     least two are in, the countdown runs (DuelCountdownMs), then the fight;
//   - a duelist's lethal hit puts it out (DEFEATED, never the party's death); the last side
//     standing wins;
//   - a duelist cancelling, its game going or it leaving the duel's map ends it for everyone.
// Every change goes to every member as the whole DuelState; the games give back what a duel took
// once it is over. Sides: free for all now (each duelist its own); teams of two later share one.
#pragma once

#include <map>
#include <span>
#include <string>
#include <vector>

#include "coop.pb.h"
#include "coop/coop_types.h"

namespace BloodborneCoop {

constexpr s64 DuelAskMs = 30'000;
constexpr s64 DuelCountdownMs = 3'000;

// A party member whose game is linked now, and the map it last said (0: loading).
struct DuelMember {
    std::string name;
    u32 map = 0;
};

class DuelReferee {
public:
    struct Result {
        bool changed = false; // send State() to every member
        std::string refused;  // to the requester only (State() with `refused`)
        std::vector<std::string> log;
    };

    // A member's request; `online`: the party's members with a game linked now.
    Result Request(const std::string& member, const bbcoop::DuelRequest& request,
                   std::span<const DuelMember> online, s64 now_ms);
    // The question and the countdown running out, duelists gone or off the duel's map.
    Result Tick(std::span<const DuelMember> online, s64 now_ms);

    bbcoop::DuelState State(s64 now_ms) const;
    // Asking, counting down or fighting.
    bool Active() const;

private:
    struct Seat {
        std::string member;
        u32 side = 0;
        u32 answer = 0;
        bool out = false;
    };
    void Start(Result& result, s64 now_ms);
    void End(Result& result, bbcoop::DuelState::End end, const std::string& by = {});
    Seat* SeatOf(const std::string& member);
    bool In(const Seat& seat) const;

    u32 duel = 0;
    bbcoop::DuelState::Stage stage = bbcoop::DuelState::STAGE_NONE;
    std::string proposer;
    u32 map = 0;
    std::vector<Seat> seats;
    std::string winner;
    bbcoop::DuelState::End end = bbcoop::DuelState::END_NONE;
    std::string ended_by;
    s64 until_ms = 0; // the question's or the countdown's end
    std::map<std::string, u32> wins;
};

} // namespace BloodborneCoop
