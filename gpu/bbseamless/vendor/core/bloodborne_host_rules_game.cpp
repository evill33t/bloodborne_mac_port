// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// Kaniel 2026-10-25: the host rules in the game (the pure part is bloodborne_host_rules.cpp):
// the Rot rule's deaths, cures and effects on the player.

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <map>
#include <mutex>
#include <optional>
#include <random>

#include "common/logging/log.h"
#include "core/bloodborne_host_rules.h"
#include "core/bloodborne_re_internal.h"
#include "core/bloodborne_re_sites.h"
#include "core/memory.h"

namespace Core::Bloodborne {

namespace {

using namespace Coop;

std::mutex rot_mutex;
Rot rot;                            // this player's
std::atomic<u32> rot_hit_by{0};     // the enemy (its handle) whose hit last landed on this player
std::atomic<s64> rot_hit_at_ms{0};  // when
std::atomic<u32> rot_own_kill{0};   // the enemy this player killed last (its handle)
std::atomic<u32> rot_kills_seen{0}; // counts this player's kills
std::atomic<u32> rot_kills{0};

// The hit that killed this player is one of the last few seconds.
constexpr s64 KillerHitMs = 10'000;

const char* KindName(RotKind kind) {
    switch (kind) {
    case RotKind::MaxHp:
        return "max HP -10%";
    case RotKind::MaxStamina:
        return "max stamina -10%";
    case RotKind::StaminaUse:
        return "stamina use +10%";
    case RotKind::Rally:
        return "rally -15%";
    case RotKind::Discovery:
        return "Item Discovery -10";
    case RotKind::None:
        break;
    }
    return "nothing";
}

const char* CureName(RotCure cure) {
    switch (cure) {
    case RotCure::LampRest:
        return "a lamp";
    case RotCure::LevelUp:
        return "a level up";
    case RotCure::BossKill:
        return "a boss kill";
    case RotCure::Revenge:
        return "killing its killer";
    case RotCure::OwnBloodstain:
        return "its own bloodstain";
    }
    return "?";
}

// The player's stats (PlayerGameData +0x40 every 8 bytes): a level up raises one.
s64 StatsSum() {
    const u64 data = OwnGameData();
    if (data < 0x10000 || !HasMemoryAccess(data + 0x40, 0x58, MemoryProt::CpuRead)) {
        return -1;
    }
    s64 sum = 0;
    for (u64 at = 0x40; at <= 0x90; at += 8) {
        sum += ReadValue<s32>(data, at);
    }
    return sum;
}

// The penalties as lasting effects on the player: each kind's row holds the sum of its rot, and
// the player has exactly the rows of the kinds it has.
void ApplyRotEffects(u64 player, const Rot& now) {
    static std::array<float, RotKinds> written{};
    static bool rows_ok = false;
    static u64 rows_table = 0;
    const TickStepTimer step{"ApplyRotEffects"};
    const RotEffect effect = now.Effect();
    const u64 table = FindLoadedParam("SP_EFFECT_PARAM_ST");
    if (table == 0) {
        return;
    }
    std::array<bool, RotKinds> wanted{};
    for (const RotKind kind : now.Kinds()) {
        wanted[static_cast<std::size_t>(kind) - 1] = true;
    }
    const std::vector<s32> has = LocalEffects();
    const auto present = [&](s32 id) { return std::find(has.begin(), has.end(), id) != has.end(); };
    for (std::size_t i = 0; i < RotRows.size(); ++i) {
        const RotRow& row = RotRows[i];
        const float value = RotRowValue(effect, row.kind);
        if (!wanted[i]) {
            if (present(row.row)) {
                EndPlayerEffect(player, row.row);
            }
            continue;
        }
        if (table != rows_table || !rows_ok || written[i] != value) {
            const auto span = WritableSpEffects(table);
            if (!span || !WriteRotRow(*span, row, value)) {
                static std::atomic<u32> logged{0};
                if (logged.fetch_add(1) < 5) {
                    LOG_WARNING(Debug, "Co-op rot: SpEffect {} could not be written", row.row);
                }
                rows_ok = false;
                continue;
            }
            rows_table = table;
            rows_ok = true;
            written[i] = value;
            // The game takes a row's values when the effect starts: started again.
            if (present(row.row)) {
                EndPlayerEffect(player, row.row);
            }
        } else if (present(row.row)) {
            continue;
        }
        if (HasVirtual(player, CharacterAddSpEffectSlot, AddSpEffectOffset, "rot AddSpEffect")) {
            reinterpret_cast<AddSpEffectFn>(image_base + AddSpEffectOffset)(
                player, row.row, player, 0, 1, 0, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 0);
        }
    }
}

} // namespace

namespace Coop {

void NoteHitOnMe(u32 attacker) {
    if (attacker != 0) {
        rot_hit_by.store(attacker);
        rot_hit_at_ms.store(SteadyMilliseconds());
    }
}

void NoteOwnKill(u32 enemy) {
    rot_own_kill.store(enemy);
    rot_kills.fetch_add(1);
}

RotView ReadRotView() {
    std::scoped_lock lock{rot_mutex};
    RotView view;
    view.on = ActiveHostRules().rot;
    view.kinds = rot.Numbers();
    view.text = rot.HudText();
    return view;
}

void TickHostRules() {
    MakeEnemiesHarder(); // the enemy scaling rule follows the hunters on this map
    TickBossTempo();
    TickTeamTravel();
    static s64 next_ms = 0;
    const s64 now_ms = SteadyMilliseconds();
    if (now_ms < next_ms) {
        return;
    }
    next_ms = now_ms + 250;
    static bool adopted = false;
    std::scoped_lock lock{rot_mutex};
    if (!adopted) {
        if (const auto kept = TakeKeptRot()) {
            adopted = true;
            if (!kept->empty()) {
                rot.Set(*kept);
                LOG_INFO(Debug, "Co-op rot: the team's server kept {} penalties for this player",
                         rot.Kinds().size());
            }
        }
    }
    const HostRules rules = ActiveHostRules();
    const u64 player = GetLocalPlayer();
    static bool was_alive = false;
    static s64 died_ms = 0;
    static u32 lamps_seen = lamp_uses.load();
    static s64 stats_seen = -1;
    static int slot_seen = -2;
    static std::optional<bool> had_stain;
    static std::map<s32, bool> bosses_seen;
    static s64 bosses_next_ms = 0;
    const auto report = [&] {
        const std::vector<u32> numbers = rot.Numbers();
        ReportRot(numbers);
    };
    const auto cure = [&](RotCure why) {
        if (rot.Cure(why)) {
            LOG_INFO(Debug, "Co-op rot: cured by {}", CureName(why));
            report();
        }
    };
    if (player < 0x10000) {
        was_alive = false;
        had_stain.reset();
        bosses_seen.clear();
        return;
    }
    if (!rules.rot) {
        if (!rot.Kinds().empty()) {
            rot.Cure(RotCure::LampRest); // the rule went off: no rot
            LOG_INFO(Debug, "Co-op rot: the rule is off; the rot is gone");
            report();
        }
        ApplyRotEffects(player, rot);
        lamps_seen = lamp_uses.load();
        stats_seen = -1;
        return;
    }
    // A real death: its HP at 0 two steps running (a down keeps some; the lethal hit's check
    // keeps it up within the step).
    static int at_zero = 0;
    at_zero = CharacterHp(player) > 0 ? 0 : at_zero + 1;
    const bool alive = at_zero < 2;
    if (was_alive && !alive) {
        const u32 killer = now_ms - rot_hit_at_ms.load() <= KillerHitMs ? rot_hit_by.load() : 0;
        static std::mt19937 random{std::random_device{}()};
        const RotKind kind = rot.Die(static_cast<u32>(random()), killer);
        died_ms = now_ms;
        LOG_INFO(Debug, "Co-op rot: died for real: {} ({} of {}; killer {:#x})", KindName(kind),
                 rot.Kinds().size(), MaxRot, killer);
        report();
    }
    was_alive = alive;
    // Cures.
    if (const u32 lamps = lamp_uses.load(); lamps != lamps_seen) {
        lamps_seen = lamps;
        cure(RotCure::LampRest);
    }
    if (const int slot = character_slot.load(); slot != slot_seen) {
        slot_seen = slot;
        stats_seen = -1;
    }
    if (const s64 stats = StatsSum(); stats >= 0) {
        if (stats_seen >= 0 && stats > stats_seen) {
            cure(RotCure::LevelUp);
        }
        stats_seen = stats;
    }
    if (const u32 kills = rot_kills.load(); kills != rot_kills_seen.load()) {
        rot_kills_seen.store(kills);
        if (rot.Killed(rot_own_kill.load())) {
            LOG_INFO(Debug, "Co-op rot: cured by {}", CureName(RotCure::Revenge));
            report();
        }
    }
    if (alive) {
        const bool stain = ReadOwnBloodstain().has_value();
        if (had_stain && *had_stain && !stain && now_ms - died_ms > 5'000) {
            cure(RotCure::OwnBloodstain);
        }
        had_stain = stain;
    }
    if (alive && now_ms >= bosses_next_ms) {
        bosses_next_ms = now_ms + 1'000;
        for (const EncounterInsight& boss : EncounterInsights()) {
            const bool dead = EventFlagOn(boss.kill_event);
            const auto it = bosses_seen.find(boss.kill_event);
            if (it != bosses_seen.end() && !it->second && dead) {
                cure(RotCure::BossKill);
            }
            bosses_seen[boss.kill_event] = dead;
        }
    }
    if (alive) {
        ApplyRotEffects(player, rot);
    }
}

} // namespace Coop

// Kaniel 2026-10-25, the travel together rule: this player's lamp or headstone trip asks first
// ("Travel with the team?"); with the vote the others have TravelVoteMs to say No.
namespace {

struct TravelAsked {
    u32 id = 0;
    s32 point = 0;
    std::string from;
    s64 at_ms = 0;
};

std::mutex travel_mutex;
std::optional<s32> own_question;  // this player's trip, waiting for its answer
std::optional<TravelAsked> asked; // a member's trip, waiting for this player's vote
TravelVote vote;                  // this player's trip with the vote
s32 vote_point = 0;
u32 trip_ids = 0;
TeamTravelNotice notice = TeamTravelNotice::None;
std::string notice_from;
s64 notice_until_ms = 0;
std::vector<bbcoop::TeamTravel> travel_news; // from the server, for the game thread

void SendTravel(bbcoop::TeamTravel::Step step, u32 id, s32 point, bool yes = false,
                const std::string& asker = {}) {
    bbcoop::TeamTravel travel;
    travel.set_step(step);
    travel.set_id(id);
    travel.set_point(point);
    travel.set_yes(yes);
    travel.set_asker(asker);
    SendTeamTravel(travel);
}

void Notice(TeamTravelNotice what, std::string from = {}) {
    notice = what;
    notice_from = std::move(from);
    notice_until_ms = SteadyMilliseconds() + 4'000;
}

// The party's other members (their board names), from its map board.
std::vector<std::string> OtherMembers() {
    std::vector<std::string> others;
    std::scoped_lock lock{map_board_mutex};
    if (std::chrono::steady_clock::now() - map_board_time > std::chrono::seconds{5}) {
        return others;
    }
    for (const MemberMap& m : map_board) {
        if (m.member != map_board_member) {
            others.push_back(m.member);
        }
    }
    return others;
}

} // namespace

namespace Coop {

bool OfferTeamTravel(s32 point) {
    const HostRules rules = ActiveHostRules();
    if (!rules.travel_together) {
        return false; // never reads a flag with the rule off
    }
    const bool boss_fight = EventFlagOn(TeamTripBossFightFlag);
    if (!OffersTeamTravel(rules, InPartySession(), OtherMembers().size(), boss_fight)) {
        return false;
    }
    std::scoped_lock lock{travel_mutex};
    if (own_question || vote.At(SteadyMilliseconds()) == TravelVote::Result::Waiting) {
        return true; // one question at a time: this trip waits for it
    }
    own_question = point;
    LOG_INFO(Debug, "Co-op team trip: asks this player about travelling to {} with the team",
             point);
    return true;
}

void TakeTeamTravelNews(const bbcoop::TeamTravel& travel) {
    std::scoped_lock lock{travel_mutex};
    if (travel_news.size() < 32) {
        travel_news.push_back(travel);
    }
}

void AnswerTeamTravel(bool yes) {
    std::scoped_lock lock{travel_mutex};
    const s64 now_ms = SteadyMilliseconds();
    if (own_question) {
        const s32 point = *own_question;
        own_question.reset();
        if (!yes) {
            LOG_INFO(Debug, "Co-op team trip: this player travels alone to {}", point);
            RequestTeamTrip(point);
            return;
        }
        const u32 id = ++trip_ids;
        if (ActiveHostRules().travel_vote) {
            const std::vector<std::string> others = OtherMembers();
            vote.Start(id, others, now_ms);
            vote_point = point;
            SendTravel(bbcoop::TeamTravel::ASK, id, point);
            LOG_INFO(Debug, "Co-op team trip {}: the team votes on {} ({} asked)", id, point,
                     others.size());
            return;
        }
        SendTravel(bbcoop::TeamTravel::GO, id, point);
        LOG_INFO(Debug, "Co-op team trip {}: the team travels to {}", id, point);
        RequestTeamTrip(point);
        return;
    }
    if (asked) {
        SendTravel(bbcoop::TeamTravel::ANSWER, asked->id, asked->point, yes, asked->from);
        LOG_INFO(Debug, "Co-op team trip {}: this player says {}", asked->id, yes ? "yes" : "no");
        asked.reset();
    }
}

std::optional<TeamTravelView> ReadTeamTravel() {
    std::scoped_lock lock{travel_mutex};
    const s64 now_ms = SteadyMilliseconds();
    TeamTravelView view;
    if (own_question) {
        view.question = TeamTravelView::Question::WithTheTeam;
        view.point = *own_question;
    } else if (asked) {
        view.question = TeamTravelView::Question::Vote;
        view.point = asked->point;
        view.from = asked->from;
        view.seconds_left = static_cast<float>(
            static_cast<double>(std::max<s64>(0, TravelVoteMs - (now_ms - asked->at_ms))) / 1000.0);
    } else if (vote.At(now_ms) == TravelVote::Result::Waiting) {
        view.waiting = true;
        view.point = vote_point;
        view.seconds_left = static_cast<float>(static_cast<double>(vote.LeftMs(now_ms)) / 1000.0);
    }
    if (now_ms < notice_until_ms) {
        view.notice = notice;
        view.notice_from = notice_from;
    }
    if (view.question == TeamTravelView::Question::None && !view.waiting &&
        view.notice == TeamTravelNotice::None) {
        return std::nullopt;
    }
    return view;
}

// The game thread: the news, the vote's end, a question nobody answered.
void TickTeamTravel() {
    std::vector<bbcoop::TeamTravel> news;
    std::scoped_lock lock{travel_mutex};
    news.swap(travel_news);
    const s64 now_ms = SteadyMilliseconds();
    for (const bbcoop::TeamTravel& travel : news) {
        switch (travel.step()) {
        case bbcoop::TeamTravel::ASK:
            asked = TravelAsked{travel.id(), travel.point(), travel.member(), now_ms};
            LOG_INFO(Debug, "Co-op team trip {}: {} asks the team to travel to {}", travel.id(),
                     travel.member(), travel.point());
            break;
        case bbcoop::TeamTravel::ANSWER:
            if (travel.asker() == map_board_member) {
                if (!travel.yes() && travel.id() == vote.Id()) {
                    notice_from = travel.member();
                }
                vote.Answer(travel.id(), travel.member(), travel.yes());
                LOG_INFO(Debug, "Co-op team trip {}: {} says {}", travel.id(), travel.member(),
                         travel.yes() ? "yes" : "no");
            }
            break;
        case bbcoop::TeamTravel::GO:
            if (asked && asked->id == travel.id() && asked->from == travel.member()) {
                asked.reset();
            }
            LOG_INFO(Debug, "Co-op team trip {}: {} takes the team to {}", travel.id(),
                     travel.member(), travel.point());
            RequestTeamTrip(travel.point());
            break;
        case bbcoop::TeamTravel::STOPPED:
            if (asked && asked->id == travel.id() && asked->from == travel.member()) {
                asked.reset();
            }
            Notice(TeamTravelNotice::Stopped, travel.member());
            LOG_INFO(Debug, "Co-op team trip {}: {}'s trip is off", travel.id(), travel.member());
            break;
        default:
            break;
        }
    }
    if (asked && now_ms - asked->at_ms > TravelVoteMs + 5'000) {
        asked.reset(); // the asker's go or stop never came
    }
    switch (vote.At(now_ms)) {
    case TravelVote::Result::Go:
        SendTravel(bbcoop::TeamTravel::GO, vote.Id(), vote_point);
        LOG_INFO(Debug, "Co-op team trip {}: the vote goes; the team travels to {}", vote.Id(),
                 vote_point);
        RequestTeamTrip(vote_point);
        vote.Clear();
        break;
    case TravelVote::Result::Stopped:
        SendTravel(bbcoop::TeamTravel::STOPPED, vote.Id(), vote_point);
        LOG_INFO(Debug, "Co-op team trip {}: {} said no; nobody travels", vote.Id(), notice_from);
        Notice(TeamTravelNotice::SaidNo, notice_from);
        vote.Clear();
        break;
    default:
        break;
    }
}

} // namespace Coop

} // namespace Core::Bloodborne
