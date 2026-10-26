// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// Kaniel 2026-10-25: team duels in the game (the server's side is server/coop/duel.h). A member
// proposes a duel from the team screen, the others on its map answer a yes/no box, and those in
// fight: their bodies take the invaders' team on each other's screens (so strikes land), a lethal
// hit puts a duelist out instead of down (no party death, wipe, map reset, echo loss, bloodstain
// or lamp), and the end gives each duelist back the HP, vials and bullets the duel took. This file
// is the pure part: following the server's state, the bodies to make hostile, what to give back.

#pragma once

#include <array>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "common/types.h"
#include "coop.pb.h"
#include "core/bloodborne_team.h"

namespace Core::Bloodborne {

// What a duel's state means for this player (`self`, its board name).
enum class DuelPart {
    None,     // no duel, or one this player is not in
    Asked,    // asked, not answered yet
    Answered, // said yes, the others still answering
    Countdown,
    Fighting,
    Out, // defeated, the others still fighting
};

// What changed for this player with a new state (DuelFollower::Take).
struct DuelNews {
    bool asked = false;  // a question for this player to answer (AskedDuel)
    bool joined = false; // this player is in a duel now (countdown): keep what it has
    bool fight = false;  // the fight starts
    bool out = false;    // this player is out
    bool over = false;   // the duel this player was in is over: give back what it took
    bool won = false;    // ... and this player's side won
    bool lost = false;   // ... and another side won
    std::string refused; // the server refused this player's request: why
    bool operator==(const DuelNews&) const = default;
};

class DuelFollower {
public:
    // `now_ms`: this game's clock as it came (the countdown's times are kept on it).
    DuelNews Take(const bbcoop::DuelState& state, std::string_view self, s64 now_ms = 0);
    DuelPart Part() const {
        return part;
    }
    const bbcoop::DuelState& State() const {
        return state;
    }
    // The duel waiting for this player's answer, if any.
    std::optional<u32> AskedDuel() const;
    // The duel this player is in (counting down, fighting or out).
    std::optional<u32> InDuel() const;
    // The members this player may strike now: in the fight, on another side (empty unless this
    // player fights).
    std::vector<std::string> Foes() const;
    // Whether this player may propose a duel now (none under way).
    bool MayPropose() const;
    // The world this game shows is back to friendly co-op and nothing is owed: a duel's end (or
    // a game without one).
    void Forget() {
        part = DuelPart::None;
    }
    // When this game's countdown of the duel ends (the server's time left as it came), and when
    // the server's fight came; nothing before.
    std::optional<s64> CountdownUntil() const {
        return countdown_until;
    }
    std::optional<s64> FightSince() const {
        return fight_since;
    }

private:
    bbcoop::DuelState state;
    std::string self;
    DuelPart part = DuelPart::None;
    u32 countdown_duel = 0; // the duel the times are of
    std::optional<s64> countdown_until;
    std::optional<s64> fight_since;
};

// Kaniel 2026-10-25: the countdown as the screen shows it: the number (3, 2, 1), the bar's fill
// (0 to 1 as the fight comes), then the fight's start for DuelFightShownMs.
constexpr s64 DuelFightShownMs = 1'200;
struct DuelCountdownShown {
    bool shown = false;
    int number = 0;
    float fill = 0.0f;
    bool fight = false;
    bool operator==(const DuelCountdownShown&) const = default;
};
DuelCountdownShown DuelCountdownAt(std::optional<s64> until_ms, std::optional<s64> fight_ms,
                                   s64 now_ms);

// What a duel takes, kept as it starts and given back at its end: the HP (the HP to set, nothing
// when it is the same; `max_hp` the player's max now) ...
std::optional<s32> DuelHpBack(s32 before, s32 now, s32 max_hp);
// ... and every consumable (Kaniel 2026-10-25: all of them, so a duel may use anything): the goods
// in the inventory (not the key items) back to their counts from the start, by what to add. Goods
// picked up meanwhile stay; nothing is taken away.
std::vector<HeldGoods> DuelGoodsGiveBack(std::span<const HeldGoods> before,
                                         std::span<const HeldGoods> now);

// Which bodies in this world are foes'. The board posts where each member stands; a body is the
// member's posted nearest to it (each member one body, the nearest pairs first), within
// DuelBodyReach. With no other member here but foes, every body is a foe's.
constexpr float DuelBodyReach = 8.0f;
struct DuelBody {
    u32 handle = 0;
    std::array<float, 3> at{};
};
struct DuelMemberAt {
    std::string member;
    std::array<float, 3> at{};
};
std::vector<u32> DuelFoeBodies(std::span<const DuelBody> bodies,
                               std::span<const DuelMemberAt> others,
                               std::span<const std::string> foes);

// The game's banners (SetTextEffect ids, its TE_* list): the end's for the winner and the
// defeat's, as the game's own PvP shows them.
constexpr s32 DuelWonTextEffect = 7; // TE_KillBlack: ADVERSARY EXPUNGED
constexpr s32 DuelOutTextEffect = 2; // TE_Died: YOU DIED
// The status ailments a duel's end takes off when the duel put them on: SpEffectParam rows of
// stateInfo 2 (poison) and 5 (rapid poison) under 10000 that last and drain HP (1.09's params).
constexpr std::array<s32, 52> DuelAilmentEffects{
    1200, 1210, 1230, 2120, 2121, 2130, 2131, 3958, 3959, 3971, 4012, 4013, 4021,
    4655, 4720, 4721, 4722, 4723, 4724, 4725, 4726, 4727, 4801, 5248, 5249, 5250,
    5251, 5252, 5253, 5254, 5255, 5259, 5260, 5261, 5262, 5263, 5264, 5265, 5269,
    5660, 5661, 5705, 6190, 6500, 6501, 6502, 6503, 6504, 6505, 6620, 6630, 6760};
// The ailments to take off at the end: on now, not on as the duel started.
std::vector<s32> DuelAilmentsToEnd(std::span<const s32> before, std::span<const s32> now);

namespace Coop {

// What the port's team screen, boxes and overlay show (the game thread keeps it).
struct DuelView {
    DuelPart part = DuelPart::None;
    bbcoop::DuelState::Stage stage = bbcoop::DuelState::STAGE_NONE;
    float seconds_left = 0.0f; // the question's or the countdown's
    bool may_propose = false;  // in a party with someone else on this map, no duel under way
    bool may_cancel = false;   // in the duel (it is under way)
    u32 fighters = 0;          // duelists still standing
    DuelCountdownShown countdown;
};
DuelView ReadDuel();
// The team screen's buttons (any thread; the game thread sends them).
void ProposeDuel();
void CancelDuel();
// The question waiting for this player: the duel and the proposer's character name.
std::optional<std::pair<u32, std::string>> DuelAsk();
bool DuelAskOpen(u32 duel);
void AnswerDuel(u32 duel, bool accept);
// What the port says once (a box): the kinds, and whose name goes in.
enum class DuelNoticeKind {
    Busy,      // a duel is already under way
    Alone,     // nobody else of the party here
    Declined,  // fewer than two said yes
    Cancelled, // `name` cancelled it
    Left,      // `name`'s game went
    Moved,     // `name` left the duel's map
};
struct DuelNotice {
    DuelNoticeKind kind;
    std::string name;
};
std::optional<DuelNotice> NextDuelNotice();
// Each game tick.
void TickDuel();
// The party death hook (any thread): while this player fights a duel a lethal hit puts it out,
// never down or dead (NoteDuelCatch counts each catch for the tick).
bool DuelCatchesLethal();
void NoteDuelCatch();

} // namespace Coop

} // namespace Core::Bloodborne
