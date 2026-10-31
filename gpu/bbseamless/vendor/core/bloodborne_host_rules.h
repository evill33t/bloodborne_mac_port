// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// Kaniel 2026-10-25: the host rules, the team leader's choice for its whole team (its launcher's
// SHADPS4_BLOODBORNE_HOST_RULES; the server keeps them in the team, TeamRules), all off by
// default:
// - Rot: a real death (not a down a teammate revives) adds one random penalty, up to MaxRot;
//   a lamp rest, a level up, a boss kill, killing the enemy that killed you or picking up your own
//   bloodstain takes it all away.
// - Travel together: a lamp or headstone trip asks "Travel with the team?"; Yes takes everyone
//   there in one load (with the vote: the team has TravelVoteMs to say No, one No stops it).
// - Enemy scaling: enemies grow from the vanilla game (alone) to the harder defaults (a full
//   team) with the hunters on their map.
// - Boss tempo (Kaniel 2026-10-28): Vanilla, Aggressive or Relentless; bosses rest less between
//   attacks and turn faster.
// This file is the pure part: the rules' text, the rot and what it does, the vote and the scale.

#pragma once

#include <array>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "common/types.h"
#include "core/bloodborne_party.h"

namespace Core::Bloodborne {

// bbcoop::BossTempo's numbers.
enum class BossTempo : u8 {
    Vanilla = 0,
    Aggressive = 1,
    Relentless = 2,
};

struct HostRules {
    bool rot = false;
    bool travel_together = false;
    bool travel_vote = false; // only with travel_together
    bool enemy_scaling = false;
    BossTempo boss_tempo = BossTempo::Vanilla;
    bool operator==(const HostRules&) const = default;
};

// The launcher's setting: words split by commas ("rot", "travel", "vote", "scale", and the boss
// tempo's "aggressive" or "relentless", the last one given); anything else (or "off", or
// nothing) is off. "vote" alone is nothing: it is a part of "travel".
HostRules ParseHostRules(std::string_view setting);
// The same words back ("off" for none), for the log and the build's line.
std::string HostRulesText(const HostRules& rules);

// The Rot rule's penalties (bbcoop::RotKind's numbers).
enum class RotKind : u8 {
    None = 0,
    MaxHp = 1,      // -10% max HP
    MaxStamina = 2, // -10% max stamina
    StaminaUse = 3, // +10% stamina use
    Rally = 4,      // -15% rally
    Discovery = 5,  // -10 Item Discovery
};
constexpr std::size_t MaxRot = 5;
constexpr std::size_t RotKinds = 5;

// What takes a player's rot away.
enum class RotCure : u8 {
    LampRest,
    LevelUp,
    BossKill,
    Revenge, // the enemy that killed it
    OwnBloodstain,
};

// The rot's sum, as the player's stats take it.
struct RotEffect {
    float max_hp_rate = 1.0f;
    float max_stamina_rate = 1.0f;
    float stamina_use_rate = 1.0f;
    float rally_rate = 1.0f;
    s32 discovery = 0;
    bool operator==(const RotEffect&) const = default;
};

class Rot {
public:
    // A real death: one penalty more (`roll`, any number, picks which), none past MaxRot. The
    // enemy that killed it (its handle, 0 none) is kept for the Revenge cure. The penalty added.
    RotKind Die(u32 roll, u32 killer = 0);
    // Whether there was rot to take away.
    bool Cure(RotCure cure);
    // A kill by this player: the Revenge cure if it was the one that killed it.
    bool Killed(u32 enemy);
    // The server's (a game that came back), the oldest first; a bad number is left out.
    void Set(std::span<const u32> kinds);
    std::vector<u32> Numbers() const;
    const std::vector<RotKind>& Kinds() const {
        return kinds_;
    }
    u32 Killer() const {
        return killer_;
    }
    RotEffect Effect() const;
    // For the party HUD: each kind's count, one letter per kind as the HUD line shows them.
    std::string HudText() const;

private:
    std::vector<RotKind> kinds_;
    u32 killer_ = 0;
};

// Each penalty is a lasting SpEffect on the player (rows of SpEffectParam no event, param or
// script uses, all like row 0: from the game files 2026-10-25), its one field set to the rot's
// sum of that kind; the rest of the row is RotTemplateRow's (a lasting effect on the player
// itself, its own two fields put back to nothing): no category, so it stacks with the runes.
struct RotRow {
    RotKind kind;
    s32 row;
    u64 field; // a float: a rate (1 nothing), or Item Discovery's points (0 nothing)
};
constexpr std::array<RotRow, RotKinds> RotRows{{
    {RotKind::MaxHp, 5509, 0x10},       // maxHpRate
    {RotKind::MaxStamina, 5514, 0x18},  // maxStaminaRate
    {RotKind::StaminaUse, 5529, 0x1B8}, // consumeStaminaRate
    {RotKind::Rally, 5701, 0x1F4},      // regainRate
    {RotKind::Discovery, 5528, 0x1FC},  // itemDropRate (points)
}};
constexpr s32 RotTemplateRow = 4730; // x0.5 max HP and +10 Item Discovery, lasting, on itself
constexpr u64 SpEffectRowSize = 0x210;
constexpr u64 SpEffectDurationOffset = 0x08; // effectEndurance (-1 lasting)
// The value `row`'s field takes for `effect`.
float RotRowValue(const RotEffect& effect, RotKind kind);
// Makes `row` the penalty with `value` in a loaded SpEffectParam (`param`): false when a row is
// missing or its data is another row's too.
bool WriteRotRow(std::span<u8> param, const RotRow& row, float value);

// Kaniel 2026-10-25: a team trip with the vote waits this long for a No.
constexpr s64 TravelVoteMs = 20'000;

// The team's vote on one trip: one No stops it; when every other member said Yes, or the time is
// up without a No, it goes.
class TravelVote {
public:
    enum class Result : u8 { None, Waiting, Go, Stopped };
    // The asker's trip; `others`, the members asked (the asker's Yes is its asking).
    void Start(u32 id, std::span<const std::string> others, s64 now_ms);
    // A member's answer to trip `id` (another trip's, or a member not asked: nothing).
    void Answer(u32 id, const std::string& member, bool yes);
    // A member out of the team (or off line) has nothing more to say.
    void Drop(const std::string& member);
    Result At(s64 now_ms) const;
    u32 Id() const {
        return id_;
    }
    s64 LeftMs(s64 now_ms) const;
    void Clear();

private:
    u32 id_ = 0;
    s64 started_ms_ = 0;
    bool active_ = false;
    bool stopped_ = false;
    std::vector<std::string> waiting_;
};

// Whether this player's lamp or headstone trip asks "Travel with the team?": the rule is on, it
// plays in a party session with someone else in it, and no boss fight (the game's flag 4000) runs.
constexpr s32 TeamTripBossFightFlag = 4000;
bool OffersTeamTravel(const HostRules& rules, bool in_session, std::size_t others, bool boss_fight);

// What the travel question shows (the overlay).
enum class TeamTravelNotice : u8 {
    None,
    SaidNo,  // this player's trip: a member (notice_from) said No
    Stopped, // a member's trip (notice_from's) is off
};
struct TeamTravelView {
    enum class Question : u8 { None, WithTheTeam, Vote };
    Question question = Question::None;
    s32 point = 0;          // the trip's warp point
    std::string from;       // Vote: the asker (its board name)
    float seconds_left = 0; // Vote, and while this player's vote waits
    bool waiting = false;   // this player's trip waits for the team's answers
    TeamTravelNotice notice = TeamTravelNotice::None;
    std::string notice_from; // the member the notice names
};

// Kaniel 2026-10-25 ("it should use our default values in the order"): the enemies for
// `hunters` players on their map, from the vanilla game (one; none: nullopt) to the launcher's
// harder default (HarderDefaultHp/Damage/Poise, a full team of TeamSlots), evenly in between.
constexpr std::size_t TeamSlots = 4;
constexpr float HarderDefaultHp = 3.0f;
constexpr float HarderDefaultDamage = 1.5f;
constexpr float HarderDefaultPoise = 3.0f;
std::optional<HarderEnemies> TeamScaledEnemies(std::size_t hunters);
// The hunters on `map` by the party's map board: this player and every other member there.
std::size_t HuntersOnMap(std::span<const MemberMap> board, std::string_view self, u32 map);

// Kaniel 2026-10-28 (boss tempo, docs/BOSS_TEMPO.md): a boss's AI scripts give it a breather
// after most of its attacks: the act's GetWellSpace_Odds (often 70) is Common_Battle_Activate's
// chance (GetRandam_Int(1, 100) <= odds) of the after-act move, which adds sub-goals that wait
// (GOAL_COMMON_Wait), back off (LeaveTarget) or strafe (SidewayMove) for a life time, and the
// acts themselves turn (Turn) and strafe between swings. Each level shortens those goals' life
// (`gap`), cuts all but `odds` of the waits, back-offs and strafes to almost nothing (the
// after-act chance x odds), and turns boss rows of NpcParam faster (turnVellocity x `turn`).
// `anim` (the animation speed) is not used yet.
struct BossTempoTuning {
    float gap = 1.0f;
    float odds = 1.0f;
    float turn = 1.0f;
    float anim = 1.0f;
    bool operator==(const BossTempoTuning&) const = default;
    bool Changes() const {
        return *this != BossTempoTuning{};
    }
};
// SHADPS4_BLOODBORNE_BOSS_TEMPO=gap,odds,turn,anim (the dev setting): gap in (0, 1], odds in
// [0, 1], turn and anim in [0.5, 4]; anything else is nothing.
std::optional<BossTempoTuning> ParseBossTempoTuning(std::string_view setting);
// The level's tuning: Vanilla is always the game's; the dev setting (`tuned`) replaces the
// others' presets.
BossTempoTuning BossTempoTuningFor(BossTempo level,
                                   const std::optional<BossTempoTuning>& tuned = std::nullopt);
const char* BossTempoName(BossTempo level);

// The AI goals (the scripts' goal_list.lua).
constexpr s32 GoalCommonWait = 2000;
constexpr s32 GoalCommonTurn = 2001;
constexpr s32 GoalCommonLeaveTarget = 2016;
constexpr s32 GoalCommonSidewayMove = 2017;
bool IsBossTempoGoal(s32 goal);
// A cut goal's life: one frame or two, so the next act comes right away.
constexpr float CutGoalLife = 0.05f;
// The life a boss's `goal` gets for the scripts' `life` (0 or less: no limit, kept), with `roll`
// (0-99) against the tuning's odds; never longer than the game's.
float BossGoalLife(s32 goal, float life, const BossTempoTuning& tuning, u32 roll);

// AddSubGoal (0x0124CC60): goal (rdi), its new sub-goal's id (esi), life time (xmm0), up to
// seven float parameters and more on the stack; every Lua overload and the native goals call it.
// The hook goes after its prologue stored the life at [rbp + AddSubGoalLifeFrameOffset], over
// `mov r14, rdi; xor ecx, ecx`. A goal's [+0] is its AI ([+0x28] the NpcThinkParam id: the
// GetNpcThinkParamID binding, 0x0125BC00); the AI keeps its character at +0x558, which it takes
// from its manipulator ([+0], whose virtuals the IsNpcPlayer binding calls) at 0x0128EDC3. The AI
// runs on the CSChrThread workers, several at once.
constexpr u64 AddSubGoalOffset = 0x0124CC60;
constexpr u64 AddSubGoalLifeHookOffset = 0x0124CCAC;
constexpr std::array<u8, 5> AddSubGoalLifeHook{0x49, 0x89, 0xFE, 0x31, 0xC9};
constexpr s64 AddSubGoalLifeFrameOffset = -0xBC;
// AddSubGoal_Front's (0x0124D4F0: the same arguments, the sub-goal goes first): the same hook
// over the same two instructions, its life at [rbp - 0xB8]. The hooks' tags say which.
constexpr u64 AddSubGoalFrontOffset = 0x0124D4F0;
constexpr u64 AddSubGoalFrontLifeHookOffset = 0x0124D53C;
constexpr s64 AddSubGoalFrontLifeFrameOffset = -0xB8;
constexpr u64 AddSubGoalFrontTag = 1;
constexpr u64 AiCharacterOffset = 0x558;
constexpr u64 AiTakesItsCharacterOffset = 0x0128EDC3;

} // namespace Core::Bloodborne
