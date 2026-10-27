// SPDX-FileCopyrightText: Copyright 2026 shadNet Project
// SPDX-License-Identifier: GPL-2.0-or-later
// Invasions (docs/INVASIONS.md): which team world an invader may enter
// (coop/invasions.h), and the cooldowns after one.
#include <iostream>
#include <string>
#include <utility>
#include <vector>

#include "coop/invasions.h"

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

constexpr u32 CentralYharnam = 0x18010000;
constexpr u32 CathedralWard = 0x18000000;
constexpr u32 ChaliceDepth = 0x1D000000;
constexpr s64 Minute = 60 * 1000;

InvasionTarget Open() {
  InvasionTarget target;
  target.world = "w1";
  target.allows_invaders = true;
  target.session_open = true;
  target.players = 2;
  target.members = {{CentralYharnam, 50, false}, {CathedralWard, 0, false}};
  return target;
}

bool May(const InvasionTarget &target, u32 level = 0,
         const InvasionCooldowns &cooldowns = {}, s64 now_ms = 0) {
  return InvasionRefusal(target, "red", level, cooldowns, now_ms).empty();
}

int AnOpenWorldMayBeInvaded() {
  CHECK(May(Open()));
  CHECK(May(Open(), 60));
  return 0;
}

int OnlyAWorldThatAllowsInvaders() {
  InvasionTarget target = Open();
  target.allows_invaders = false;
  CHECK(!May(target));
  target = Open();
  target.session_open = false; // not started, or its host's session closed
  CHECK(!May(target));
  return 0;
}

int OneInvaderAtATime() {
  InvasionTarget target = Open();
  target.invaded = true;
  CHECK(!May(target));
  return 0;
}

int AFullSessionHasNoSlotForAnInvader() {
  InvasionTarget target = Open();
  target.players = InvasionMaxPlayers - 1;
  CHECK(May(target));
  target.players = InvasionMaxPlayers;
  CHECK(!May(target));
  return 0;
}

int NeverInTheDreamOrAChaliceOrABossFight() {
  CHECK(!InvadableMap(0));
  CHECK(!InvadableMap(HuntersDreamMap));
  CHECK(!InvadableMap(ChaliceDepth));
  CHECK(InvadableMap(CentralYharnam));
  InvasionTarget target = Open();
  target.members = {{HuntersDreamMap, 50, false}, {0, 50, false}};
  CHECK(!May(target));
  target.members = {{HuntersDreamMap, 50, false}, {CathedralWard, 50, false}};
  CHECK(May(target)); // one member out in the world is enough
  // A boss fight anywhere in the world: nobody comes.
  target.members = {{CentralYharnam, 50, true}, {CathedralWard, 50, false}};
  CHECK(!May(target));
  return 0;
}

int TheLevelBandWhenLevelsAreKnown() {
  CHECK(InLevelBand(50, 50));
  CHECK(InLevelBand(80, 50)); // 20 + 20% of 50 = 30
  CHECK(!InLevelBand(81, 50));
  CHECK(!InLevelBand(19, 50));
  CHECK(InLevelBand(0, 50) && InLevelBand(99, 0)); // not known: no band
  InvasionTarget target = Open();
  target.members = {{CentralYharnam, 50, false}};
  CHECK(!May(target, 120));
  CHECK(May(target, 70));
  CHECK(May(target, 0));
  target.members = {{CentralYharnam, 50, false}, {CathedralWard, 115, false}};
  CHECK(May(target, 120)); // a member in its band
  return 0;
}

int CooldownsPerTeamAndPerInvaderAndTeam() {
  InvasionCooldowns cooldowns;
  const s64 now = 1'000'000;
  cooldowns.Ended("w1", "red", now);
  const InvasionTarget target = Open();
  // The team rests 15 minutes from anyone.
  CHECK(!InvasionRefusal(target, "blue", 0, cooldowns, now + 14 * Minute)
             .empty());
  CHECK(
      InvasionRefusal(target, "blue", 0, cooldowns, now + 15 * Minute).empty());
  // The same invader stays away an hour.
  CHECK(!May(target, 0, cooldowns, now + 59 * Minute));
  CHECK(May(target, 0, cooldowns, now + 60 * Minute));
  // Another world is not resting.
  InvasionTarget other = Open();
  other.world = "w2";
  CHECK(May(other, 0, cooldowns, now + Minute));
  // Old cooldowns go (nothing grows for ever).
  CHECK(cooldowns.Size() == 2);
  cooldowns.Expire(now + 60 * Minute);
  CHECK(cooldowns.Size() == 0);
  return 0;
}

} // namespace

int main() {
  const std::vector<std::pair<std::string, int (*)()>> tests{
      {"AnOpenWorldMayBeInvaded", AnOpenWorldMayBeInvaded},
      {"OnlyAWorldThatAllowsInvaders", OnlyAWorldThatAllowsInvaders},
      {"OneInvaderAtATime", OneInvaderAtATime},
      {"AFullSessionHasNoSlotForAnInvader", AFullSessionHasNoSlotForAnInvader},
      {"NeverInTheDreamOrAChaliceOrABossFight",
       NeverInTheDreamOrAChaliceOrABossFight},
      {"TheLevelBandWhenLevelsAreKnown", TheLevelBandWhenLevelsAreKnown},
      {"CooldownsPerTeamAndPerInvaderAndTeam",
       CooldownsPerTeamAndPerInvaderAndTeam},
  };
  int failed = 0;
  for (const auto &[name, test] : tests) {
    const int result = test();
    std::cout << (result == 0 ? "PASS " : "FAIL ") << name << '\n';
    failed += result != 0 ? 1 : 0;
  }
  return failed == 0 ? 0 : 1;
}
