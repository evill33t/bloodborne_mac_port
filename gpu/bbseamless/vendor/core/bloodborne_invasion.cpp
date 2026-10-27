// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "core/bloodborne_invasion.h"

#include <algorithm>
#include <charconv>

namespace Core::Bloodborne {

bool IsHostileMember(const MemberVitals& member) {
    return member.hostile;
}

std::vector<MemberVitals> TakeHostileLines(VitalsBoard& board) {
    std::vector<MemberVitals> hostile;
    std::erase_if(board.members, [&](MemberVitals& member) {
        if (!IsHostileMember(member)) {
            return false;
        }
        hostile.push_back(std::move(member));
        return true;
    });
    // A wipe never counts the invader (the server's), but a line naming it would still be no
    // teammate's.
    for (PartyWipe& wipe : board.wipes) {
        std::erase_if(wipe.members, [&](const std::string& name) {
            return std::ranges::find(hostile, name, &MemberVitals::member) != hostile.end();
        });
    }
    return hostile;
}

std::vector<bool> InvaderBodies(std::span<const std::optional<LampPosition>> positions,
                                std::span<const MemberVitals> teammates,
                                std::span<const MemberVitals> hostile, std::string_view self,
                                u32 map) {
    std::vector<bool> invader(positions.size(), false);
    if (hostile.empty() || map == 0) {
        return invader;
    }
    // As ShownOtherPlayers: each line claims one body, the nearest pairs first. An invader standing
    // over a teammate's body leaves that body to the teammate's line.
    std::vector<const MemberVitals*> lines;
    for (const MemberVitals& member : teammates) {
        lines.push_back(&member);
    }
    for (const MemberVitals& member : hostile) {
        lines.push_back(&member);
    }
    struct Pair {
        float distance;
        size_t body;
        size_t line;
    };
    std::vector<Pair> pairs;
    for (size_t b = 0; b < positions.size(); ++b) {
        if (!positions[b]) {
            continue;
        }
        for (size_t l = 0; l < lines.size(); ++l) {
            const MemberVitals& line = *lines[l];
            if (line.member == self || line.map != map) {
                continue;
            }
            const float dx = line.at[0] - (*positions[b])[0];
            const float dy = line.at[1] - (*positions[b])[1];
            const float dz = line.at[2] - (*positions[b])[2];
            const float distance = dx * dx + dy * dy + dz * dz;
            if (distance <= PresenceMetres * PresenceMetres) {
                pairs.push_back({distance, b, l});
            }
        }
    }
    std::ranges::sort(pairs, {}, &Pair::distance);
    std::vector<bool> body_taken(positions.size(), false);
    std::vector<bool> line_taken(lines.size(), false);
    for (const Pair& pair : pairs) {
        if (body_taken[pair.body] || line_taken[pair.line]) {
            continue;
        }
        body_taken[pair.body] = true;
        line_taken[pair.line] = true;
        invader[pair.body] = IsHostileMember(*lines[pair.line]);
    }
    return invader;
}

size_t TeamSessionMembers(size_t session_members, bool invader_in_session) {
    return invader_in_session && session_members > 1 ? session_members - 1 : session_members;
}

InvasionJoin InvasionJoinOf(const bbcoop::TeamSession& session, bool fallback, u32 coop_kind) {
    if (fallback) {
        return {coop_kind, session.sign(), true};
    }
    return {session.kind(), session.sign(), false};
}

std::string_view InvasionEndReasonName(bbcoop::InvasionEnd::Reason reason) {
    switch (reason) {
    case bbcoop::InvasionEnd::INVADER_DIED:
        return "the invader died";
    case bbcoop::InvasionEnd::TARGET_KILLED:
        return "the target was killed";
    case bbcoop::InvasionEnd::PARTY_WIPED:
        return "the party wiped";
    case bbcoop::InvasionEnd::BOSS:
        return "a boss fight";
    case bbcoop::InvasionEnd::TIMEOUT:
        return "the time ran out";
    case bbcoop::InvasionEnd::INVADER_LEFT:
        return "the invader left";
    case bbcoop::InvasionEnd::HOST_LEFT:
        return "the host left";
    case bbcoop::InvasionEnd::INVADERS_OFF:
        return "invaders turned off";
    case bbcoop::InvasionEnd::NOT_FOUND:
        return "no world found";
    case bbcoop::InvasionEnd_Reason_REFUSED:
        return "refused";
    default:
        return "ended";
    }
}

bool InvaderTrip::Ask(s64 now_ms) {
    if (phase != Phase::None) {
        return false;
    }
    *this = InvaderTrip{};
    phase = Phase::Asked;
    phase_since_ms = now_ms;
    return true;
}

bool InvaderTrip::Cancel(s64 now_ms) {
    switch (phase) {
    case Phase::None:
    case Phase::GoingHome:
        return false;
    case Phase::Asked:
    case Phase::Approaching:
    case Phase::Travelling:
        // Still in its own world: nothing to go home from (a trip on the way stays a trip).
        phase = Phase::None;
        end_reason = bbcoop::InvasionEnd::INVADER_LEFT;
        phase_since_ms = now_ms;
        return true;
    default:
        GoHome(true, false);
        phase_since_ms = now_ms;
        return true;
    }
}

void InvaderTrip::TakeNotice(const bbcoop::InvasionNotice& notice, s64 now_ms) {
    if (phase != Phase::Asked && phase != Phase::Approaching) {
        return;
    }
    phase = Phase::Approaching;
    id = notice.id();
    approach_until_ms = now_ms + notice.in_ms();
    phase_since_ms = now_ms;
}

bool InvaderTrip::TakeStart(const bbcoop::InvasionStart& message, s64 now_ms) {
    if (phase != Phase::Asked && phase != Phase::Approaching) {
        return false;
    }
    if (phase == Phase::Approaching && id != 0 && message.id() != id) {
        return false;
    }
    if (message.session().join().empty()) {
        return false;
    }
    start = message;
    id = message.id();
    phase = Phase::Travelling;
    phase_since_ms = now_ms;
    travel_asked = false;
    return true;
}

void InvaderTrip::TakeEnd(const bbcoop::InvasionEnd& end, s64 now_ms) {
    if (phase == Phase::None) {
        return;
    }
    if (id != 0 && end.id() != 0 && end.id() != id) {
        return; // an older invasion's
    }
    end_reason = end.reason();
    switch (phase) {
    case Phase::Asked:
    case Phase::Approaching:
    case Phase::Travelling:
        phase = Phase::None;
        break;
    case Phase::GoingHome:
        break;
    default:
        GoHome(false, died);
        end_reason = end.reason();
        break;
    }
    phase_since_ms = now_ms;
}

void InvaderTrip::GoHome(bool tell, bool death) {
    if (phase == Phase::GoingHome) {
        return;
    }
    phase = Phase::GoingHome;
    go_home_said = false;
    tell_server = tell;
    died = death;
}

InvaderTrip::Action InvaderTrip::Tick(const Frame& frame) {
    const s64 in_phase = frame.now_ms - phase_since_ms;
    switch (phase) {
    case Phase::None:
        return Action::None;
    case Phase::Asked:
    case Phase::Approaching:
        if (in_phase > SearchMs) {
            phase = Phase::None; // the server's NOT_FOUND never came: the link is lost
            end_reason = bbcoop::InvasionEnd::NOT_FOUND;
        }
        return Action::None;
    case Phase::Travelling:
        if (frame.dead) {
            phase = Phase::None;
            tell_server = true;
            end_reason = bbcoop::InvasionEnd::NONE;
            died = true;
            return Action::None;
        }
        if (in_phase > TravelMs) {
            phase = Phase::None;
            tell_server = true;
            end_reason = bbcoop::InvasionEnd::NONE;
            return Action::None;
        }
        if (!frame.settled) {
            return Action::None;
        }
        if (frame.map == start->map() || start->map() == 0) {
            phase = Phase::Joining;
            phase_since_ms = frame.now_ms;
            join_tries = 1;
            join_tried_ms = frame.now_ms;
            return Action::Join;
        }
        if (!travel_asked) {
            travel_asked = true;
            return Action::Travel;
        }
        return Action::None;
    case Phase::Joining:
        if (frame.in_session) {
            phase = Phase::Visiting;
            phase_since_ms = frame.now_ms;
            session_lost_ms = -1;
            return Action::Place;
        }
        if (frame.dead) {
            GoHome(true, true);
            return Tick(frame);
        }
        if (in_phase > JoinWaitMs) {
            GoHome(true, false);
            return Tick(frame);
        }
        if (join_tries < JoinTries && frame.settled &&
            frame.now_ms - join_tried_ms >= JoinRetryMs) {
            ++join_tries;
            join_tried_ms = frame.now_ms;
            return Action::Join;
        }
        return Action::None;
    case Phase::Visiting:
        if (frame.dead) {
            // The server sees the death in its vitals (INVADER_DIED) and ends it: no cancel.
            GoHome(false, true);
            return Tick(frame);
        }
        if (frame.boss_fog || in_phase > VisitMs) {
            GoHome(true, false);
            return Tick(frame);
        }
        if (!frame.in_session) {
            if (session_lost_ms < 0) {
                session_lost_ms = frame.now_ms;
            } else if (frame.now_ms - session_lost_ms > SessionLostMs) {
                GoHome(true, false);
                return Tick(frame);
            }
        } else {
            session_lost_ms = -1;
        }
        return Action::None;
    case Phase::GoingHome:
        if (!go_home_said) {
            go_home_said = true;
            return Action::GoHome;
        }
        return Action::None;
    }
    return Action::None;
}

void InvaderTrip::Home() {
    if (phase == Phase::GoingHome) {
        phase = Phase::None;
    }
}

InvasionJoin InvaderTrip::Join(bool fallback, u32 coop_kind) const {
    return start ? InvasionJoinOf(start->session(), fallback, coop_kind) : InvasionJoin{};
}

void InvadedTeam::TakeNotice(const bbcoop::InvasionNotice& notice, s64 now_ms) {
    if (phase == Phase::Invaded) {
        return; // one invader at a time (the server's rule)
    }
    phase = Phase::Approaching;
    id = notice.id();
    member = notice.invader();
    target = notice.target();
    name.clear();
    approach_until_ms = now_ms + notice.in_ms();
    end_reason = bbcoop::InvasionEnd::NONE;
}

void InvadedTeam::TakeJoined(const bbcoop::InvaderJoined& joined, s64) {
    phase = Phase::Invaded;
    id = joined.id();
    member = joined.member();
    target = joined.target();
    name = joined.name().empty() ? joined.member() : joined.name();
    end_reason = bbcoop::InvasionEnd::NONE;
}

void InvadedTeam::TakeEnd(const bbcoop::InvasionEnd& end, s64 now_ms) {
    if (phase == Phase::None || (id != 0 && end.id() != 0 && end.id() != id)) {
        return;
    }
    phase = Phase::Ended;
    end_reason = end.reason();
    ended_ms = now_ms;
}

void InvadedTeam::Tick(s64 now_ms) {
    if (phase == Phase::Ended && now_ms - ended_ms >= EndShownMs) {
        *this = InvadedTeam{};
    }
}

InvasionView InvasionViewOf(const InvaderTrip& trip, const InvadedTeam& team, s64 now_ms) {
    InvasionView view;
    const auto seconds_to = [&](s64 until_ms) {
        return until_ms > now_ms ? static_cast<int>((until_ms - now_ms + 999) / 1000) : 0;
    };
    if (trip.Active()) {
        view.role = InvasionView::Role::Invader;
        view.name = trip.Start() ? trip.Start()->target() : std::string{};
        switch (trip.Current()) {
        case InvaderTrip::Phase::Asked:
            view.phase = InvasionView::Phase::Searching;
            break;
        case InvaderTrip::Phase::Approaching:
            view.phase = InvasionView::Phase::Approaching;
            view.seconds = seconds_to(trip.ApproachUntilMs());
            break;
        case InvaderTrip::Phase::GoingHome:
            view.phase = InvasionView::Phase::Returning;
            view.reason = trip.EndReason();
            break;
        default:
            view.phase = InvasionView::Phase::Active;
            break;
        }
        return view;
    }
    // A search that ended before any join (refused, nothing found): said a while.
    if (team.Current() == InvadedTeam::Phase::None &&
        trip.EndReason() != bbcoop::InvasionEnd::NONE &&
        now_ms - trip.PhaseSinceMs() < InvadedTeam::EndShownMs) {
        view.role = InvasionView::Role::Invader;
        view.phase = InvasionView::Phase::Ended;
        view.reason = trip.EndReason();
        return view;
    }
    switch (team.Current()) {
    case InvadedTeam::Phase::None:
        return view;
    case InvadedTeam::Phase::Approaching:
        view.phase = InvasionView::Phase::Approaching;
        view.seconds = seconds_to(team.ApproachUntilMs());
        break;
    case InvadedTeam::Phase::Invaded:
        view.phase = InvasionView::Phase::Active;
        break;
    case InvadedTeam::Phase::Ended:
        view.phase = InvasionView::Phase::Ended;
        view.reason = team.EndReason();
        break;
    }
    view.role = InvasionView::Role::Target;
    view.name = team.Name();
    return view;
}

bool InvaderKillGoesThrough(NoDeadCheck check, bool invader_hit_fresh, bool down) {
    return check == NoDeadCheck::Lethal && invader_hit_fresh && !down;
}

std::string FormatInvasionHome(const InvasionHome& home) {
    return "respawn " + std::to_string(home.respawn) + "\n" + FormatWorldFlags(home.flags);
}

std::optional<InvasionHome> ParseInvasionHome(std::string_view text) {
    constexpr std::string_view Respawn = "respawn ";
    if (!text.starts_with(Respawn)) {
        return std::nullopt;
    }
    const size_t end = text.find('\n');
    if (end == std::string_view::npos) {
        return std::nullopt;
    }
    InvasionHome home;
    const std::string_view number = text.substr(Respawn.size(), end - Respawn.size());
    const auto [ptr, error] =
        std::from_chars(number.data(), number.data() + number.size(), home.respawn);
    if (error != std::errc{} || ptr != number.data() + number.size()) {
        return std::nullopt;
    }
    auto flags = ParseWorldFlags(text.substr(end + 1));
    if (!flags) {
        return std::nullopt;
    }
    home.flags = std::move(*flags);
    return home;
}

} // namespace Core::Bloodborne
