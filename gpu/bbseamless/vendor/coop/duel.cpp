// SPDX-FileCopyrightText: Copyright 2026 shadNet Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <set>

#include "coop/duel.h"

namespace BloodborneCoop {

namespace {

const DuelMember* Online(std::span<const DuelMember> online, const std::string& member) {
    const auto found = std::ranges::find(online, member, &DuelMember::name);
    return found == online.end() ? nullptr : &*found;
}

} // namespace

DuelReferee::Seat* DuelReferee::SeatOf(const std::string& member) {
    const auto found = std::ranges::find(seats, member, &Seat::member);
    return found == seats.end() ? nullptr : &*found;
}

// In the fight: accepted and not out.
bool DuelReferee::In(const Seat& seat) const {
    return seat.answer == 1 && !seat.out;
}

bool DuelReferee::Active() const {
    return stage == bbcoop::DuelState::ASKING || stage == bbcoop::DuelState::COUNTDOWN ||
           stage == bbcoop::DuelState::FIGHTING;
}

void DuelReferee::Start(Result& result, s64 now_ms) {
    const auto in = std::ranges::count(seats, u32{1}, &Seat::answer);
    if (in < 2) {
        End(result, std::ranges::any_of(seats, [](const Seat& seat) { return seat.answer == 2; })
                        ? bbcoop::DuelState::DECLINED
                        : bbcoop::DuelState::NO_ANSWER);
        return;
    }
    // Free for all: each duelist its own side, in the order asked.
    u32 side = 0;
    for (Seat& seat : seats) {
        seat.side = seat.answer == 1 ? ++side : 0;
    }
    stage = bbcoop::DuelState::COUNTDOWN;
    until_ms = now_ms + DuelCountdownMs;
    result.changed = true;
    result.log.push_back("duel " + std::to_string(duel) + ": " + std::to_string(in) +
                         " in, the countdown runs");
}

void DuelReferee::End(Result& result, bbcoop::DuelState::End why, const std::string& by) {
    stage = bbcoop::DuelState::OVER;
    end = why;
    ended_by = by;
    if (why == bbcoop::DuelState::WON && !winner.empty()) {
        ++wins[winner];
    }
    result.changed = true;
    result.log.push_back(
        "duel " + std::to_string(duel) + " over: " + bbcoop::DuelState::End_Name(why) +
        (winner.empty() ? "" : ", " + winner + " won") + (by.empty() ? "" : " (" + by + ")"));
}

DuelReferee::Result DuelReferee::Request(const std::string& member,
                                         const bbcoop::DuelRequest& request,
                                         std::span<const DuelMember> online, s64 now_ms) {
    Result result;
    if (request.action() == bbcoop::DuelRequest::PROPOSE) {
        if (Active()) {
            result.refused = "A duel is already under way.";
            return result;
        }
        const DuelMember* self = Online(online, member);
        const u32 on = self != nullptr && self->map != 0 ? self->map : request.map();
        std::vector<Seat> asked{Seat{member, 0, 1, false}};
        for (const DuelMember& other : online) {
            if (other.name != member && on != 0 && other.map == on) {
                asked.push_back(Seat{other.name, 0, 0, false});
            }
        }
        if (asked.size() < 2) {
            result.refused = "Nobody else in your party is here to duel.";
            return result;
        }
        ++duel;
        stage = bbcoop::DuelState::ASKING;
        proposer = member;
        map = on;
        seats = std::move(asked);
        winner.clear();
        end = bbcoop::DuelState::END_NONE;
        ended_by.clear();
        until_ms = now_ms + DuelAskMs;
        result.changed = true;
        result.log.push_back("duel " + std::to_string(duel) + ": " + member + " proposes, " +
                             std::to_string(seats.size() - 1) + " asked");
        return result;
    }
    if (request.duel() != duel || !Active()) {
        return result;
    }
    Seat* seat = SeatOf(member);
    if (seat == nullptr) {
        return result;
    }
    switch (request.action()) {
    case bbcoop::DuelRequest::ACCEPT:
    case bbcoop::DuelRequest::DECLINE:
        if (stage != bbcoop::DuelState::ASKING || seat->answer != 0) {
            return result;
        }
        seat->answer = request.action() == bbcoop::DuelRequest::ACCEPT ? 1 : 2;
        result.changed = true;
        result.log.push_back("duel " + std::to_string(duel) + ": " + member +
                             (seat->answer == 1 ? " accepts" : " declines"));
        if (std::ranges::none_of(seats, [](const Seat& s) { return s.answer == 0; })) {
            Start(result, now_ms);
        }
        return result;
    case bbcoop::DuelRequest::CANCEL:
        if (seat->answer != 1) {
            return result;
        }
        End(result, bbcoop::DuelState::CANCELLED, member);
        return result;
    case bbcoop::DuelRequest::DEFEATED: {
        if (stage != bbcoop::DuelState::FIGHTING || !In(*seat)) {
            return result;
        }
        seat->out = true;
        result.changed = true;
        result.log.push_back("duel " + std::to_string(duel) + ": " + member + " is out");
        std::set<u32> sides;
        for (const Seat& s : seats) {
            if (In(s)) {
                sides.insert(s.side);
            }
        }
        if (sides.size() <= 1) {
            const auto standing = std::ranges::find_if(seats, [&](const Seat& s) { return In(s); });
            winner = standing == seats.end() ? std::string{} : standing->member;
            End(result, bbcoop::DuelState::WON);
        }
        return result;
    }
    default:
        return result;
    }
}

DuelReferee::Result DuelReferee::Tick(std::span<const DuelMember> online, s64 now_ms) {
    Result result;
    if (!Active()) {
        return result;
    }
    if (stage == bbcoop::DuelState::ASKING) {
        // A member gone before it answered: a no.
        for (Seat& seat : seats) {
            if (seat.answer == 0 && Online(online, seat.member) == nullptr) {
                seat.answer = 2;
                result.changed = true;
                result.log.push_back("duel " + std::to_string(duel) + ": " + seat.member +
                                     " went before answering");
            }
        }
        if (now_ms >= until_ms ||
            std::ranges::none_of(seats, [](const Seat& s) { return s.answer == 0; })) {
            const bool unanswered =
                std::ranges::any_of(seats, [](const Seat& s) { return s.answer == 0; });
            for (Seat& seat : seats) {
                seat.answer = seat.answer == 0 ? 2 : seat.answer;
            }
            if (unanswered && std::ranges::count(seats, u32{1}, &Seat::answer) < 2) {
                End(result, bbcoop::DuelState::NO_ANSWER);
            } else {
                Start(result, now_ms);
            }
            return result;
        }
    }
    // A duelist still in gone or off the duel's map ends it (a load says map 0).
    for (const Seat& seat : seats) {
        if (!In(seat)) {
            continue;
        }
        const DuelMember* member = Online(online, seat.member);
        if (member == nullptr) {
            End(result, bbcoop::DuelState::LEFT, seat.member);
            return result;
        }
        if (member->map != map) {
            End(result, bbcoop::DuelState::MAP, seat.member);
            return result;
        }
    }
    if (stage == bbcoop::DuelState::COUNTDOWN && now_ms >= until_ms) {
        stage = bbcoop::DuelState::FIGHTING;
        result.changed = true;
        result.log.push_back("duel " + std::to_string(duel) + ": the fight starts");
    }
    return result;
}

bbcoop::DuelState DuelReferee::State(s64 now_ms) const {
    bbcoop::DuelState state;
    state.set_duel(duel);
    state.set_stage(stage);
    state.set_proposer(proposer);
    state.set_map(map);
    for (const Seat& seat : seats) {
        bbcoop::Duelist& duelist = *state.add_duelists();
        duelist.set_member(seat.member);
        duelist.set_side(seat.side);
        duelist.set_answer(seat.answer);
        duelist.set_out(seat.out);
        const auto won = wins.find(seat.member);
        duelist.set_wins(won == wins.end() ? 0 : won->second);
    }
    state.set_winner(winner);
    state.set_end(end);
    state.set_ended_by(ended_by);
    if (stage == bbcoop::DuelState::ASKING || stage == bbcoop::DuelState::COUNTDOWN) {
        state.set_left_ms(static_cast<u32>(std::max<s64>(0, until_ms - now_ms)));
    }
    return state;
}

} // namespace BloodborneCoop
