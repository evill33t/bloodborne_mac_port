// SPDX-License-Identifier: GPL-2.0-or-later
// bbport co-op: team duels' C side for the port's team screen, boxes and overlay (bbcoop.h,
// bbcoop_duel_*), on top of the co-op code's bloodborne_duel.h.
#include <algorithm>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include "bbcoop.h"
#include "core/bloodborne_duel.h"

namespace {

using namespace Core::Bloodborne;

int Copy(const std::string& text, char* out, size_t size) {
    if (out == nullptr || size == 0) {
        return 0;
    }
    const size_t length = std::min(text.size(), size - 1);
    std::memcpy(out, text.data(), length);
    out[length] = '\0';
    return 1;
}

} // namespace

extern "C" {

void bbcoop_duel_view(bbcoop_duel* duel) {
    const Coop::DuelView view = Coop::ReadDuel();
    duel->part = static_cast<int>(view.part);
    duel->may_propose = view.may_propose ? 1 : 0;
    duel->may_cancel = view.may_cancel ? 1 : 0;
    duel->seconds_left = view.seconds_left;
    duel->fighters = static_cast<int>(view.fighters);
    duel->countdown_shown = view.countdown.shown ? 1 : 0;
    duel->countdown_number = view.countdown.number;
    duel->countdown_fight = view.countdown.fight ? 1 : 0;
    duel->countdown_fill = view.countdown.fill;
    duel->teams = view.teams ? 1 : 0;
    duel->team = static_cast<int>(view.team);
}

int bbcoop_duel_candidates(bbcoop_duel_candidate* out, int max) {
    int count = 0;
    for (const Coop::DuelCandidate& candidate : Coop::DuelCandidates()) {
        if (count >= max) {
            break;
        }
        Copy(candidate.member, out[count].member, sizeof out[count].member);
        Copy(candidate.name, out[count].name, sizeof out[count].name);
        out[count].self = candidate.self ? 1 : 0;
        ++count;
    }
    return count;
}

void bbcoop_duel_propose_teams(const bbcoop_duel_candidate* hunters, const int* teams, int count) {
    std::vector<std::pair<std::string, u32>> picks;
    for (int i = 0; i < count; ++i) {
        if (teams[i] >= 1 && teams[i] <= 3) {
            picks.emplace_back(hunters[i].member, static_cast<u32>(teams[i]));
        }
    }
    Coop::ProposeDuel(std::move(picks));
}

int bbcoop_duel_ask_teams(uint32_t id, int* mine, char names[3][256]) {
    const auto teams = Coop::DuelAskTeamsOf(id);
    if (!teams) {
        return 0;
    }
    *mine = static_cast<int>(teams->mine);
    for (int t = 0; t < 3; ++t) {
        std::string line;
        for (const std::string& name : teams->names[t]) {
            line += (line.empty() ? "" : ", ") + name;
        }
        Copy(line, names[t], 256);
    }
    return 1;
}

void bbcoop_duel_propose(void) {
    Coop::ProposeDuel();
}

void bbcoop_duel_cancel(void) {
    Coop::CancelDuel();
}

int bbcoop_duel_ask(uint32_t* id, char* name, size_t size) {
    const auto ask = Coop::DuelAsk();
    if (!ask) {
        return 0;
    }
    *id = ask->first;
    return Copy(ask->second, name, size);
}

int bbcoop_duel_ask_open(uint32_t id) {
    return Coop::DuelAskOpen(id) ? 1 : 0;
}

void bbcoop_duel_answer(uint32_t id, int accept) {
    Coop::AnswerDuel(id, accept != 0);
}

int bbcoop_duel_notice(int* kind, int* team, char* name, size_t size) {
    const auto notice = Coop::NextDuelNotice();
    if (!notice) {
        return 0;
    }
    *kind = static_cast<int>(notice->kind);
    *team = static_cast<int>(notice->team);
    return Copy(notice->name, name, size);
}

} // extern "C"
