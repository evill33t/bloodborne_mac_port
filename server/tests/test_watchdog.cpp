// SPDX-FileCopyrightText: Copyright 2026 shadNet Project
// SPDX-License-Identifier: GPL-2.0-or-later
// Phase 11: the watchdog (watchdog.h) restarts shadNet when it stops or hangs,
// and gives a crashing one a rest instead of restarting it forever.
#include <iostream>

#include "watchdog.h"

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

using Action = WatchdogPolicy::Action;

int StartsAndRestartsAStoppedServer() {
  WatchdogPolicy dog;
  CHECK(dog.Tick(0, false, std::nullopt) == Action::Start);
  dog.Started(0);
  CHECK(dog.Tick(1000, true, std::nullopt) ==
        Action::None); // no beat yet: still starting
  CHECK(dog.Tick(5000, true, 4000) == Action::None);
  // It stopped (crashed, or someone closed it): started again after a short
  // wait.
  CHECK(dog.Tick(6000, false, 4000) == Action::None);
  CHECK(dog.Tick(6500, false, 4000) == Action::None);
  CHECK(dog.Tick(7000, false, 4000) == Action::Start);
  dog.Started(7000);
  CHECK(dog.Restarts() == 1);
  std::cout << "watchdog: starts and restarts a stopped server\n";
  return 0;
}

int KillsAHungServer() {
  WatchdogPolicy dog;
  dog.Tick(0, false, std::nullopt);
  dog.Started(0);
  CHECK(dog.Tick(14'000, true, std::nullopt) == Action::None);
  // Never a beat in its first 30 s: hung at its start.
  CHECK(dog.Tick(30'001, true, std::nullopt) == Action::Kill);
  dog.Started(31'000);
  CHECK(dog.Tick(40'000, true, 39'000) == Action::None);
  // Its beats stopped for longer than 15 s: hung (its event loop is stuck).
  CHECK(dog.Tick(54'001, true, 39'000) == Action::Kill);
  CHECK(dog.Why().contains("no beat"));
  std::cout << "watchdog: kills a hung server\n";
  return 0;
}

int GivesACrashingServerARest() {
  WatchdogPolicy dog;
  qint64 now = 0;
  for (int run = 0; run < 5; ++run) {
    while (dog.Tick(now, false, std::nullopt) != Action::Start) {
      now += 500;
    }
    dog.Started(now);
    now += 2000; // it crashes 2 s in
  }
  // Five starts within two minutes: a minute's rest before the next.
  CHECK(dog.Tick(now, false, std::nullopt) == Action::None);
  CHECK(dog.Why().contains("crashes"));
  CHECK(dog.Tick(now + 30'000, false, std::nullopt) == Action::None);
  CHECK(dog.Tick(now + 61'000, false, std::nullopt) == Action::Start);
  std::cout << "watchdog: gives a crashing server a rest\n";
  return 0;
}

} // namespace

int main() {
  if (StartsAndRestartsAStoppedServer() != 0 || KillsAHungServer() != 0 ||
      GivesACrashingServerARest() != 0) {
    return 1;
  }
  return 0;
}
