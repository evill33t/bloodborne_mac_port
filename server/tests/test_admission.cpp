// SPDX-FileCopyrightText: Copyright 2026 shadNet Project
// SPDX-License-Identifier: GPL-2.0-or-later
// Who may come in on a hosted server (admission.h): hellos, links and accounts
// an address, each link's rate, bans.
#include <iostream>

#include "admission.h"

using Admission::Guard;
using Admission::Limits;

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

constexpr quint32 Home = 0x0A000001;
constexpr quint32 Other = 0x0A000002;

int HellosFromOneAddressAreLimited() {
  Limits limits;
  limits.hellos_per_second = 2;
  limits.hello_burst = 3;
  Guard guard{limits};
  qint64 now = 1000;
  CHECK(guard.AllowHello(Home, now));
  CHECK(guard.AllowHello(Home, now));
  CHECK(guard.AllowHello(Home, now));
  CHECK(!guard.AllowHello(Home, now)); // the burst is spent
  CHECK(guard.AllowHello(Other, now)); // another address has its own
  now += 500;                          // one more token
  CHECK(guard.AllowHello(Home, now));
  CHECK(!guard.AllowHello(Home, now));
  CHECK(guard.Count().hellos_refused == 2);
  return 0;
}

int LinksAndAccountsAnAddress() {
  Limits limits;
  limits.links_per_address = 2;
  limits.new_accounts_per_hour = 2;
  Guard guard{limits};
  CHECK(guard.AllowLink(Home, 0) && guard.AllowLink(Home, 1));
  CHECK(!guard.AllowLink(Home, 2));
  qint64 now = 0;
  CHECK(guard.MayMakeAccount(Home, now));
  guard.Made(Home, now);
  guard.Made(Home, now);
  CHECK(!guard.MayMakeAccount(Home, now));
  guard.Unmade(Home); // one queued made none after all
  CHECK(guard.MayMakeAccount(Home, now));
  guard.Made(Home, now);
  now += 3'600'001; // an hour later
  CHECK(guard.MayMakeAccount(Home, now));
  return 0;
}

int AFloodingLinkIsDroppedThenBanned() {
  Limits limits;
  limits.datagrams_per_second = 100;
  limits.datagram_burst = 100;
  limits.strikes_to_ban = 3;
  Guard guard{limits};
  qint64 now = 0;
  int passed = 0;
  for (int i = 0; i < 1000; ++i) {
    passed += guard.AllowDatagram(1, Home, 100, now);
  }
  CHECK(passed == 100);
  CHECK(!guard.Banned(Home, now));
  for (int second = 1; second <= 3; ++second) {
    now = second * 1000;
    for (int i = 0; i < 1000; ++i) {
      guard.AllowDatagram(1, Home, 100, now);
    }
  }
  CHECK(guard.Banned(Home, now));
  CHECK(!guard.Banned(Other, now));
  CHECK(!guard.Banned(Home, now + limits.ban_ms + 1)); // for a while only
  return 0;
}

int ABigLinkWithinItsRateIsFine() {
  Guard guard{};
  qint64 now = 0;
  // A game's ~200 datagrams of ~100 bytes a second for a minute.
  for (int ms = 0; ms < 60'000; ms += 5) {
    CHECK(guard.AllowDatagram(1, Home, 100, now + ms));
  }
  CHECK(guard.Count().datagrams_dropped == 0);
  return 0;
}

int TheOperatorsBanList() {
  QSet<quint32> addresses;
  QSet<QByteArray> ids;
  Admission::ParseBanList("# griefers\n203.0.113.7\nMallory  # spam\n\n",
                          addresses, ids);
  CHECK(addresses.contains(0xCB007107));
  CHECK(ids.contains("Mallory") && ids.size() == 1);
  Guard guard{};
  guard.SetBanned(addresses, ids);
  CHECK(guard.Banned(0xCB007107, 0));
  CHECK(guard.BannedId("Mallory"));
  return 0;
}

} // namespace

// A new account's work: the base, 2 bits more for each doubling of the
// accounts made in the last minute over the baseline, never past the cap.
int SignupWorkRisesWithTheRate() {
  Admission::SignupWork work{20, 26, 30};
  qint64 now = 5000;
  CHECK(work.Bits(now) == 20);
  for (int index = 0; index < 59; ++index) {
    work.Made(now);
  }
  CHECK(work.Bits(now) == 20); // under twice the baseline
  work.Made(now);
  CHECK(work.Bits(now) == 22); // 60 a minute
  for (int index = 0; index < 60; ++index) {
    work.Made(now + 30'000);
  }
  CHECK(work.Bits(now + 30'000) == 24); // 120
  // The first 60 leave the minute.
  CHECK(work.Bits(now + 60'000) == 22);
  for (int index = 0; index < 10'000; ++index) {
    work.Made(now + 60'000);
  }
  CHECK(work.Bits(now + 60'000) == 26); // the cap
  CHECK(work.Bits(now + 119'000) == 26);
  CHECK(work.Bits(now + 120'000) == 20); // a quiet minute: the base
  CHECK(work.Bits(now + 1'000'000) == 20);
  Admission::SignupWork none{0, 26, 30};
  none.Made(now);
  CHECK(none.Bits(now) == 0);
  return 0;
}

// Every server, hosted or not: a hello that opens costs Diffie-Hellmans, so one
// address gets a few a second; nothing is banned for it.
int HomeHellosAreBudgeted() {
  Admission::HelloBudget budget{20};
  int allowed = 0;
  for (int i = 0; i < 100; ++i) {
    allowed += budget.Allow(0x0A000001, 5000 + i) ? 1 : 0; // 100 in 0.1 s
  }
  CHECK(allowed == 20);
  CHECK(budget.Allow(0x0A000002, 5100)); // another address has its own
  CHECK(budget.Allow(0x0A000001, 6000)); // the next second
  return 0;
}

int main() {
  int failed = 0;
  failed |= HellosFromOneAddressAreLimited();
  failed |= LinksAndAccountsAnAddress();
  failed |= AFloodingLinkIsDroppedThenBanned();
  failed |= ABigLinkWithinItsRateIsFine();
  failed |= TheOperatorsBanList();
  failed |= SignupWorkRisesWithTheRate();
  failed |= HomeHellosAreBudgeted();
  if (failed == 0) {
    std::cout << "admission: all checks passed\n";
  }
  return failed;
}
