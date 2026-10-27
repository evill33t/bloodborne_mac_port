// SPDX-FileCopyrightText: Copyright 2026 shadNet Project
// SPDX-License-Identifier: GPL-2.0-or-later
// Team duels, the server's side (coop/duel.h): the referee one party keeps,
// from the proposal through the answers, the countdown and the fight to its
// end, as the members' games ask and come and go.
#include <iostream>
#include <string>
#include <vector>

#include "coop/duel.h"

using namespace BloodborneCoop;

namespace {

bool Check(bool condition, const char *expression, int line) {
  if (!condition) {
    std::cerr << "check failed at line " << line << ": " << expression << '\n';
  }
  return condition;
}

#define CHECK(expression)                                                      \
  do {                                                                         \
    if (!Check((expression), #expression, __LINE__))                           \
      return 1;                                                                \
  } while (false)

constexpr u32 Yharnam = 0x18010000;
constexpr u32 Ward = 0x18000000;

bbcoop::DuelRequest Ask(bbcoop::DuelRequest::Action action, u32 duel = 0,
                        u32 map = Yharnam) {
  bbcoop::DuelRequest request;
  request.set_action(action);
  request.set_duel(duel);
  request.set_map(map);
  return request;
}

const bbcoop::Duelist *Find(const bbcoop::DuelState &state,
                            const std::string &member) {
  for (const bbcoop::Duelist &duelist : state.duelists()) {
    if (duelist.member() == member) {
      return &duelist;
    }
  }
  return nullptr;
}

// Two members on one map: the proposer is in at once, the other is asked; its
// yes starts the countdown, and the fight follows it.
int TestProposeAcceptFight() {
  DuelReferee referee;
  const std::vector<DuelMember> online{{"ada", Yharnam}, {"bob", Yharnam}};
  DuelReferee::Result result =
      referee.Request("ada", Ask(bbcoop::DuelRequest::PROPOSE), online, 1'000);
  CHECK(result.changed);
  CHECK(result.refused.empty());
  bbcoop::DuelState state = referee.State(1'000);
  CHECK(state.duel() == 1);
  CHECK(state.stage() == bbcoop::DuelState::ASKING);
  CHECK(state.proposer() == "ada");
  CHECK(state.map() == Yharnam);
  CHECK(state.duelists_size() == 2);
  CHECK(Find(state, "ada")->answer() == 1);
  CHECK(Find(state, "bob")->answer() == 0);
  CHECK(state.left_ms() == DuelAskMs);
  CHECK(referee.Active());

  result = referee.Request("bob", Ask(bbcoop::DuelRequest::ACCEPT, 1), online,
                           2'000);
  CHECK(result.changed);
  state = referee.State(2'000);
  CHECK(state.stage() == bbcoop::DuelState::COUNTDOWN);
  CHECK(state.left_ms() == DuelCountdownMs);
  // Free for all: each its own side.
  CHECK(Find(state, "ada")->side() != 0);
  CHECK(Find(state, "bob")->side() != 0);
  CHECK(Find(state, "ada")->side() != Find(state, "bob")->side());

  CHECK(!referee.Tick(online, 2'000 + DuelCountdownMs - 1).changed);
  CHECK(referee.Tick(online, 2'000 + DuelCountdownMs).changed);
  CHECK(referee.State(5'000).stage() == bbcoop::DuelState::FIGHTING);
  return 0;
}

// Nobody else on the proposer's map, or a duel already on: refused, said only
// to the proposer, nothing changes.
int TestProposeRefused() {
  DuelReferee referee;
  DuelReferee::Result result = referee.Request(
      "ada", Ask(bbcoop::DuelRequest::PROPOSE),
      std::vector<DuelMember>{{"ada", Yharnam}, {"bob", Ward}}, 1'000);
  CHECK(!result.changed);
  CHECK(!result.refused.empty());
  CHECK(referee.State(1'000).stage() == bbcoop::DuelState::STAGE_NONE);

  const std::vector<DuelMember> online{{"ada", Yharnam}, {"bob", Yharnam}};
  CHECK(referee.Request("ada", Ask(bbcoop::DuelRequest::PROPOSE), online, 1'000)
            .changed);
  result =
      referee.Request("bob", Ask(bbcoop::DuelRequest::PROPOSE), online, 1'500);
  CHECK(!result.changed);
  CHECK(!result.refused.empty());
  CHECK(referee.State(1'500).proposer() == "ada");
  return 0;
}

// A no from the only other member ends it before it starts; so does the
// question running out with fewer than two in.
int TestDeclinedAndNoAnswer() {
  DuelReferee referee;
  const std::vector<DuelMember> online{{"ada", Yharnam}, {"bob", Yharnam}};
  referee.Request("ada", Ask(bbcoop::DuelRequest::PROPOSE), online, 1'000);
  CHECK(referee
            .Request("bob", Ask(bbcoop::DuelRequest::DECLINE, 1), online, 2'000)
            .changed);
  bbcoop::DuelState state = referee.State(2'000);
  CHECK(state.stage() == bbcoop::DuelState::OVER);
  CHECK(state.end() == bbcoop::DuelState::DECLINED);
  CHECK(!referee.Active());

  referee.Request("ada", Ask(bbcoop::DuelRequest::PROPOSE), online, 3'000);
  CHECK(referee.State(3'000).duel() == 2);
  CHECK(!referee.Tick(online, 3'000 + DuelAskMs - 1).changed);
  CHECK(referee.Tick(online, 3'000 + DuelAskMs).changed);
  state = referee.State(3'000 + DuelAskMs);
  CHECK(state.stage() == bbcoop::DuelState::OVER);
  CHECK(state.end() == bbcoop::DuelState::NO_ANSWER);
  return 0;
}

// Three on the map: one says no, the other two fight; the question running out
// starts it with those who said yes.
int TestThreeAskedTwoFight() {
  DuelReferee referee;
  const std::vector<DuelMember> online{
      {"ada", Yharnam}, {"bob", Yharnam}, {"cy", Yharnam}};
  referee.Request("ada", Ask(bbcoop::DuelRequest::PROPOSE), online, 1'000);
  CHECK(referee.State(1'000).duelists_size() == 3);
  referee.Request("cy", Ask(bbcoop::DuelRequest::DECLINE, 1), online, 1'500);
  CHECK(referee.State(1'500).stage() == bbcoop::DuelState::ASKING);
  referee.Request("bob", Ask(bbcoop::DuelRequest::ACCEPT, 1), online, 2'000);
  bbcoop::DuelState state = referee.State(2'000);
  CHECK(state.stage() == bbcoop::DuelState::COUNTDOWN);
  CHECK(Find(state, "cy")->answer() == 2);
  CHECK(Find(state, "cy")->side() == 0);

  DuelReferee late;
  late.Request("ada", Ask(bbcoop::DuelRequest::PROPOSE), online, 1'000);
  late.Request("bob", Ask(bbcoop::DuelRequest::ACCEPT, 1), online, 1'500);
  CHECK(late.State(1'500).stage() == bbcoop::DuelState::ASKING);
  late.Tick(online, 1'000 + DuelAskMs);
  state = late.State(1'000 + DuelAskMs);
  CHECK(state.stage() == bbcoop::DuelState::COUNTDOWN);
  CHECK(Find(state, "cy")->side() == 0);
  return 0;
}

DuelReferee Fighting(const std::vector<DuelMember> &online) {
  DuelReferee referee;
  referee.Request(online[0].name, Ask(bbcoop::DuelRequest::PROPOSE), online,
                  1'000);
  for (size_t i = 1; i < online.size(); ++i) {
    referee.Request(online[i].name, Ask(bbcoop::DuelRequest::ACCEPT, 1), online,
                    1'000);
  }
  referee.Tick(online, 1'000 + DuelCountdownMs);
  return referee;
}

// The last side standing wins; its win is counted and kept for the next duel.
int TestDefeatedWins() {
  const std::vector<DuelMember> online{{"ada", Yharnam}, {"bob", Yharnam}};
  DuelReferee referee = Fighting(online);
  CHECK(referee.State(5'000).stage() == bbcoop::DuelState::FIGHTING);
  CHECK(
      referee
          .Request("bob", Ask(bbcoop::DuelRequest::DEFEATED, 1), online, 6'000)
          .changed);
  bbcoop::DuelState state = referee.State(6'000);
  CHECK(state.stage() == bbcoop::DuelState::OVER);
  CHECK(state.end() == bbcoop::DuelState::WON);
  CHECK(state.winner() == "ada");
  CHECK(Find(state, "bob")->out());
  CHECK(Find(state, "ada")->wins() == 1);
  CHECK(Find(state, "bob")->wins() == 0);

  // The next one keeps the score.
  referee.Request("bob", Ask(bbcoop::DuelRequest::PROPOSE), online, 7'000);
  state = referee.State(7'000);
  CHECK(state.duel() == 2);
  CHECK(Find(state, "ada")->wins() == 1);
  CHECK(!Find(state, "ada")->out());
  return 0;
}

// Free for all with three: one out leaves two fighting; the second ends it.
// A defeat said twice, or for another duel, changes nothing.
int TestThreeFreeForAll() {
  const std::vector<DuelMember> online{
      {"ada", Yharnam}, {"bob", Yharnam}, {"cy", Yharnam}};
  DuelReferee referee = Fighting(online);
  CHECK(referee
            .Request("cy", Ask(bbcoop::DuelRequest::DEFEATED, 1), online, 6'000)
            .changed);
  CHECK(referee.State(6'000).stage() == bbcoop::DuelState::FIGHTING);
  CHECK(
      !referee
           .Request("cy", Ask(bbcoop::DuelRequest::DEFEATED, 1), online, 6'100)
           .changed);
  CHECK(
      !referee
           .Request("bob", Ask(bbcoop::DuelRequest::DEFEATED, 7), online, 6'200)
           .changed);
  CHECK(
      referee
          .Request("ada", Ask(bbcoop::DuelRequest::DEFEATED, 1), online, 6'500)
          .changed);
  const bbcoop::DuelState state = referee.State(6'500);
  CHECK(state.stage() == bbcoop::DuelState::OVER);
  CHECK(state.winner() == "bob");
  return 0;
}

// A duelist cancels from the menu: over for everyone. A member not in it
// cannot.
int TestCancel() {
  const std::vector<DuelMember> online{
      {"ada", Yharnam}, {"bob", Yharnam}, {"cy", Ward}};
  DuelReferee referee = Fighting(online);
  CHECK(
      !referee.Request("cy", Ask(bbcoop::DuelRequest::CANCEL, 1), online, 6'000)
           .changed);
  CHECK(
      referee.Request("bob", Ask(bbcoop::DuelRequest::CANCEL, 1), online, 6'000)
          .changed);
  const bbcoop::DuelState state = referee.State(6'000);
  CHECK(state.stage() == bbcoop::DuelState::OVER);
  CHECK(state.end() == bbcoop::DuelState::CANCELLED);
  CHECK(state.ended_by() == "bob");
  CHECK(state.winner().empty());
  CHECK(Find(state, "ada")->wins() == 0);
  return 0;
}

// A duelist's game gone, or the duelist off the duel's map (a load says map
// 0): over. A member not in the duel coming and going changes nothing.
int TestLeftAndMap() {
  const std::vector<DuelMember> three{
      {"ada", Yharnam}, {"bob", Yharnam}, {"cy", Ward}};
  DuelReferee referee = Fighting(three);
  CHECK(!referee
             .Tick(std::vector<DuelMember>{{"ada", Yharnam}, {"bob", Yharnam}},
                   6'000)
             .changed);
  CHECK(referee.Tick(std::vector<DuelMember>{{"ada", Yharnam}}, 6'500).changed);
  bbcoop::DuelState state = referee.State(6'500);
  CHECK(state.stage() == bbcoop::DuelState::OVER);
  CHECK(state.end() == bbcoop::DuelState::LEFT);
  CHECK(state.ended_by() == "bob");

  referee = Fighting(three);
  CHECK(referee
            .Tick(std::vector<DuelMember>{{"ada", Yharnam},
                                          {"bob", 0},
                                          {"cy", Ward}},
                  6'000)
            .changed);
  state = referee.State(6'000);
  CHECK(state.end() == bbcoop::DuelState::MAP);
  CHECK(state.ended_by() == "bob");

  // While asking, a member gone counts as a no.
  DuelReferee asking;
  asking.Request("ada", Ask(bbcoop::DuelRequest::PROPOSE),
                 std::vector<DuelMember>{{"ada", Yharnam}, {"bob", Yharnam}},
                 1'000);
  CHECK(asking.Tick(std::vector<DuelMember>{{"ada", Yharnam}}, 2'000).changed);
  CHECK(asking.State(2'000).end() == bbcoop::DuelState::DECLINED);
  return 0;
}

// An out duelist's game going, or the map changing for it, does not end a
// fight the others still have.
int TestOutDuelistMayLeave() {
  const std::vector<DuelMember> online{
      {"ada", Yharnam}, {"bob", Yharnam}, {"cy", Yharnam}};
  DuelReferee referee = Fighting(online);
  referee.Request("cy", Ask(bbcoop::DuelRequest::DEFEATED, 1), online, 6'000);
  CHECK(!referee
             .Tick(std::vector<DuelMember>{{"ada", Yharnam}, {"bob", Yharnam}},
                   7'000)
             .changed);
  CHECK(referee.State(7'000).stage() == bbcoop::DuelState::FIGHTING);
  return 0;
}

} // namespace

int main() {
  int failed = 0;
  failed += TestProposeAcceptFight();
  failed += TestProposeRefused();
  failed += TestDeclinedAndNoAnswer();
  failed += TestThreeAskedTwoFight();
  failed += TestDefeatedWins();
  failed += TestThreeFreeForAll();
  failed += TestCancel();
  failed += TestLeftAndMap();
  failed += TestOutDuelistMayLeave();
  if (failed != 0) {
    std::cerr << failed << " duel test(s) failed\n";
    return 1;
  }
  std::cout << "duel tests passed\n";
  return 0;
}
