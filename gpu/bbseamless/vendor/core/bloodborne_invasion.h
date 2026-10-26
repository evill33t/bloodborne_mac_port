// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// Seamless red invasions on the game's side (docs/INVASIONS.md, docs/invasions/GAME_SIDE_PLAN.md):
// a solo player asks the server for a world to invade (InvadeRequest), travels to its target's
// map, joins the target host's session with the server's kind and sign, and goes home when the
// server ends it or it dies (InvaderTrip). The team's games keep the invader out of every co-op
// layer: its vitals board line (`hostile`) only keeps its body shown (TakeHostileLines,
// InvaderBodies), and its lethal hit is a real death (InvaderHitWindow). Nothing here runs unless
// the server sends invasion messages: a server without Invasions=true never does. Port only (the
// server's world).
#pragma once

#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "common/types.h"
#include "coop.pb.h"
#include "core/bloodborne_party.h"
#include "core/bloodborne_team.h"

namespace Core::Bloodborne {

// Phase 0 (docs/invasions/PHASE0_CAPTURE.md) has not shown yet whether the server's native kind
// and sign (InvasionKind / InvasionSign) make the invader a red phantom. With this on, the invader
// joins as a co-operator (TeamSessionKind and the server's slot byte), turns itself into the
// invader's character type (SetChrType), and every team member's game writes the invaders' team
// (DownedTeams::Hostile) onto its body for the whole visit (GAME_SIDE_PLAN.md section 3).
constexpr bool invasion_fallback_switch = false;
// The character type the fallback gives the invader (ChrIns +0x78; 1 is a white phantom). A guess
// until phase 0: the game's black phantom.
constexpr s32 InvaderCharacterType = 2;

// The invader's line on the team's vitals board: shown, never a teammate.
bool IsHostileMember(const MemberVitals& member);
// Takes the invader's lines out of `board`: what is left is the party (panel, compass, map, downs,
// revives, the cutscene vote, the boss runner, "someone else stands"), the lines returned only
// keep the invader's body shown (ReconcileMemberPresence).
std::vector<MemberVitals> TakeHostileLines(VitalsBoard& board);

// Which of the other players' bodies (their positions, nothing when unknown) are the invader's on
// `map`: each line on the board but `self`'s, the invader's too, claims the nearest body to where
// it stands within PresenceMetres, nearest pairs first; a body the invader's line claimed is its.
std::vector<bool> InvaderBodies(std::span<const std::optional<LampPosition>> positions,
                                std::span<const MemberVitals> teammates,
                                std::span<const MemberVitals> hostile, std::string_view self,
                                u32 map);

// The session's members as the team's join counts them (TeamJoin "alone in the session"): the
// invader in it is nobody to stay for.
size_t TeamSessionMembers(size_t session_members, bool invader_in_session);

// What the invader's JoinTeamSession writes (GameMan +0x16F0 kind, +0x16F4 sign): the server's
// kind and sign as they come (never TeamGuestSign: that is a team guest's), or with the fallback a
// co-operator's kind (`coop_kind`) with the server's slot byte, made hostile after the join.
struct InvasionJoin {
    u32 kind = 0;
    u32 sign = 0;
    bool make_hostile = false;
    bool operator==(const InvasionJoin&) const = default;
};
InvasionJoin InvasionJoinOf(const bbcoop::TeamSession& session, bool fallback, u32 coop_kind);

std::string_view InvasionEndReasonName(bbcoop::InvasionEnd::Reason reason);

// The invader's own side. None -> Asked (InvadeRequest sent) -> Approaching (InvasionNotice) ->
// Travelling (InvasionStart: to the target's map first, a team guest joins where it stands) ->
// Joining (JoinTeamSession) -> Visiting (in the session, placed by the target) -> GoingHome (the
// server's InvasionEnd, its death, a boss fog, a lost session, or the player leaving) -> None once
// home. The game thread ticks it; the stream's thread hands it the server's messages (the caller
// holds a lock around both).
class InvaderTrip {
public:
    enum class Phase { None, Asked, Approaching, Travelling, Joining, Visiting, GoingHome };
    enum class Action {
        None,
        Travel, // a lamp trip towards the target's map (ChooseResumeStep)
        Join,   // JoinTeamSession with Join()
        Place,  // in the session: stand where the target last stood
        GoHome, // InvaderGoHome
    };
    struct Frame {
        s64 now_ms = 0;
        u32 map = 0;          // the map it stands on (0 while loading)
        bool settled = false; // stood on that map, loaded, for a few seconds
        bool in_session = false;
        bool dead = false;     // its own death (a real one: it is in no party)
        bool boss_fog = false; // walked into a boss fog
    };
    // The trip's limits: the lamp trip to the target's map, a join that never comes, a session lost
    // under it, and a search or a visit that outlives the server's own (5 min, 10 min) by a margin
    // (the server ends them; these only cover a lost link).
    static constexpr s64 TravelMs = 120'000;
    static constexpr s64 JoinRetryMs = 5'000;
    static constexpr int JoinTries = 3;
    static constexpr s64 JoinWaitMs = 20'000;
    static constexpr s64 SessionLostMs = 10'000;
    static constexpr s64 SearchMs = 6 * 60'000;
    static constexpr s64 VisitMs = 11 * 60'000;

    // The player asks to invade: false while an invasion is already under way.
    bool Ask(s64 now_ms);
    // The player stops: a search just ends, an invasion goes home. Whether the server must be told
    // (InvadeRequest.cancel).
    bool Cancel(s64 now_ms);
    void TakeNotice(const bbcoop::InvasionNotice& notice, s64 now_ms);
    // False when no invasion of this game's waits for it (ignored).
    bool TakeStart(const bbcoop::InvasionStart& start, s64 now_ms);
    void TakeEnd(const bbcoop::InvasionEnd& end, s64 now_ms);
    Action Tick(const Frame& frame);
    // InvaderGoHome is done (its trip home arrived).
    void Home();

    Phase Current() const {
        return phase;
    }
    bool Active() const {
        return phase != Phase::None;
    }
    // In the target's world, or on its way there or back: its world is not its own.
    bool AwayFromHome() const {
        return phase == Phase::Joining || phase == Phase::Visiting || phase == Phase::GoingHome;
    }
    const std::optional<bbcoop::InvasionStart>& Start() const {
        return start;
    }
    InvasionJoin Join(bool fallback, u32 coop_kind) const;
    // Why it went home: the server's reason, or NONE for one of its own (died, boss, left).
    bbcoop::InvasionEnd::Reason EndReason() const {
        return end_reason;
    }
    // Whether going home must tell the server (it left by itself: the server ends it then).
    bool TellsServer() const {
        return tell_server;
    }
    bool Died() const {
        return died;
    }
    s64 ApproachUntilMs() const {
        return approach_until_ms;
    }
    // When the phase began (an end: when it ended).
    s64 PhaseSinceMs() const {
        return phase_since_ms;
    }

private:
    void GoHome(bool tell, bool death);

    Phase phase = Phase::None;
    std::optional<bbcoop::InvasionStart> start;
    u64 id = 0;
    s64 phase_since_ms = 0;
    s64 approach_until_ms = 0;
    bool travel_asked = false;
    int join_tries = 0;
    s64 join_tried_ms = 0;
    s64 session_lost_ms = -1;
    bool go_home_said = false;
    bool tell_server = false;
    bool died = false;
    bbcoop::InvasionEnd::Reason end_reason = bbcoop::InvasionEnd::NONE;
};

// The team's side: an invader approaches (InvasionNotice, the countdown), is in the world
// (InvaderJoined: its board name, its name as players see it), and the end (InvasionEnd, shown for
// EndShownMs).
class InvadedTeam {
public:
    enum class Phase { None, Approaching, Invaded, Ended };
    static constexpr s64 EndShownMs = 6'000;
    void TakeNotice(const bbcoop::InvasionNotice& notice, s64 now_ms);
    void TakeJoined(const bbcoop::InvaderJoined& joined, s64 now_ms);
    void TakeEnd(const bbcoop::InvasionEnd& end, s64 now_ms);
    void Tick(s64 now_ms);
    Phase Current() const {
        return phase;
    }
    // The invader's board name while it is in the world ("" otherwise).
    std::string Invader() const {
        return phase == Phase::Invaded ? member : std::string{};
    }
    const std::string& Name() const {
        return name;
    }
    const std::string& Target() const {
        return target;
    }
    s64 ApproachUntilMs() const {
        return approach_until_ms;
    }
    bbcoop::InvasionEnd::Reason EndReason() const {
        return end_reason;
    }

private:
    Phase phase = Phase::None;
    u64 id = 0;
    std::string member, name, target;
    s64 approach_until_ms = 0;
    s64 ended_ms = 0;
    bbcoop::InvasionEnd::Reason end_reason = bbcoop::InvasionEnd::NONE;
};

// What the overlay's invasion notice shows (bbcoop_invasion_view): whose side, the phase, the
// seconds left of the countdown, the other side's name and how it ended.
struct InvasionView {
    enum class Role { None, Target, Invader };
    enum class Phase { None, Searching, Approaching, Active, Returning, Ended };
    Role role = Role::None;
    Phase phase = Phase::None;
    int seconds = 0;
    std::string name;
    bbcoop::InvasionEnd::Reason reason = bbcoop::InvasionEnd::NONE;
    bool operator==(const InvasionView&) const = default;
};
InvasionView InvasionViewOf(const InvaderTrip& trip, const InvadedTeam& team, s64 now_ms);

// Kill attribution (GAME_SIDE_PLAN.md section 5): the hit hook notes the invader's hits on the
// local player; a lethal check soon after one is a real death, not a down, and its vitals say
// killed_by_invader. A downed member is safe from the invader (party death's no-damage flags).
class InvaderHitWindow {
public:
    static constexpr s64 WindowMs = 1'000;
    void Hit(s64 now_ms) {
        last_hit_ms = now_ms;
    }
    bool Fresh(s64 now_ms) const {
        return last_hit_ms >= 0 && now_ms - last_hit_ms < WindowMs;
    }

private:
    s64 last_hit_ms = -1;
};
// Whether party death lets this lethal check kill the player (the invader's hit just landed).
bool InvaderKillGoesThrough(NoDeadCheck check, bool invader_hit_fresh, bool down);

// Save hygiene (GAME_SIDE_PLAN.md section 8): the invader's own world before the visit, written to
// its slot's file before anything changes (a crash mid-visit is put right at the next load): the
// respawn lamp (GlobalState +0x1528) and every event flag in the store's blocks.
struct InvasionHome {
    s32 respawn = 0;
    WorldFlags flags;
    bool operator==(const InvasionHome&) const = default;
};
std::string FormatInvasionHome(const InvasionHome& home);
std::optional<InvasionHome> ParseInvasionHome(std::string_view text);

} // namespace Core::Bloodborne
