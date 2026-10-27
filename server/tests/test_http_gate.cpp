// SPDX-FileCopyrightText: Copyright 2026 shadNet Project
// SPDX-License-Identifier: GPL-2.0-or-later
// Who may use the HTTP ports (http_gate.h): a request is answered only from an
// address with a signed-in link, and only as one of the players linked there.
#include <iostream>

#include "http_gate.h"

using HttpGate::Decide;
using HttpGate::Gate;
using HttpGate::Verdict;

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

constexpr quint32 Home = 0x0A000001;  // a household's router: two games
constexpr quint32 Other = 0x0A000002; // nobody linked

int TheRule() {
  const QStringList home{QStringLiteral("pAlice"), QStringLiteral("pBob")};
  CHECK(Decide(home, QStringLiteral("pAlice")) == Verdict::Allowed);
  CHECK(Decide(home, QStringLiteral("pbob")) == Verdict::Allowed); // the
  // backend's store keeps accounts lower-cased; ids are one account either way
  CHECK(Decide(home, QStringLiteral("pMallory")) == Verdict::NotLinkedHere);
  // A request that acts as nobody (ss.info, a summon search without an id)
  // needs only the link.
  CHECK(Decide(home, std::nullopt) == Verdict::Allowed);
  CHECK(Decide({}, std::nullopt) == Verdict::NoLink);
  CHECK(Decide({}, QStringLiteral("pAlice")) == Verdict::NoLink);
  CHECK(Decide(home, QString()) == Verdict::NotLinkedHere);
  return 0;
}

int LinksComeAndGo() {
  Gate gate;
  gate.SetEnforced(true);
  gate.Linked(1, Home, QStringLiteral("pAlice"));
  gate.Linked(2, Home, QStringLiteral("pBob"));
  CHECK(gate.ForPlayer(Home, QStringLiteral("pAlice")) == Verdict::Allowed);
  CHECK(gate.ForPlayer(Home, QStringLiteral("pBob")) == Verdict::Allowed);
  CHECK(gate.ForPlayer(Other, QStringLiteral("pAlice")) == Verdict::NoLink);
  CHECK(gate.ForPlayer(Home, QStringLiteral("pCarol")) ==
        Verdict::NotLinkedHere);
  // A link that says hello again as someone else is that one now.
  gate.Linked(2, Home, QStringLiteral("pCarol"));
  CHECK(gate.ForPlayer(Home, QStringLiteral("pBob")) == Verdict::NotLinkedHere);
  CHECK(gate.ForPlayer(Home, QStringLiteral("pCarol")) == Verdict::Allowed);
  gate.Gone(1);
  CHECK(gate.ForPlayer(Home, QStringLiteral("pAlice")) ==
        Verdict::NotLinkedHere);
  gate.Gone(2);
  CHECK(gate.ForPlayer(Home, std::nullopt) == Verdict::NoLink);
  CHECK(gate.At(Home).isEmpty());
  gate.Gone(7); // never linked: nothing
  return 0;
}

int OneAccountTwoLinks() {
  // The same account from two ports of one address (a restart the hub has not
  // forgotten yet): gone once, still linked by the other.
  Gate gate;
  gate.SetEnforced(true);
  gate.Linked(1, Home, QStringLiteral("pAlice"));
  gate.Linked(2, Home, QStringLiteral("pAlice"));
  CHECK(gate.At(Home).size() == 1);
  gate.Gone(1);
  CHECK(gate.ForPlayer(Home, QStringLiteral("pAlice")) == Verdict::Allowed);
  // Moved to another address: there, not here.
  gate.Linked(2, Other, QStringLiteral("pAlice"));
  CHECK(gate.ForPlayer(Home, std::nullopt) == Verdict::NoLink);
  CHECK(gate.ForPlayer(Other, QStringLiteral("pAlice")) == Verdict::Allowed);
  return 0;
}

int UserIdsAreTheGamesNumbers() {
  // The game's requests name the backend's UserId, not the account: the
  // backend says which UserId each account has.
  Gate gate;
  gate.SetEnforced(true);
  gate.Linked(1, Home, QStringLiteral("pAlice"));
  gate.Linked(2, Home, QStringLiteral("pBob"));
  // Without the backend (BloodborneBackend=false) only the address is checked.
  CHECK(gate.ForUserId(Home, 99) == Verdict::Allowed);
  CHECK(gate.ForUserId(Other, 99) == Verdict::NoLink);
  int asked = 0;
  gate.SetUserIdOf([&asked](const QString &account) -> std::optional<qint64> {
    ++asked;
    if (account == QStringLiteral("pAlice"))
      return 11;
    if (account == QStringLiteral("pBob"))
      return 12;
    return std::nullopt;
  });
  CHECK(gate.ForUserId(Home, 11) == Verdict::Allowed);
  CHECK(gate.ForUserId(Home, 12) == Verdict::Allowed);
  CHECK(gate.ForUserId(Home, 13) == Verdict::NotLinkedHere);
  CHECK(gate.ForUserId(Home, std::nullopt) == Verdict::Allowed);
  CHECK(gate.ForUserId(Other, 11) == Verdict::NoLink);
  CHECK(asked > 0);
  return 0;
}

int OffAnswersEveryone() {
  // A host's own server (not hosted, HttpLinkedOnly off): as before.
  Gate gate;
  CHECK(!gate.Enforced());
  CHECK(gate.ForPlayer(Other, QStringLiteral("pAnyone")) == Verdict::Allowed);
  CHECK(gate.ForUserId(Other, 5) == Verdict::Allowed);
  gate.SetEnforced(true);
  CHECK(gate.ForUserId(Other, 5) == Verdict::NoLink);
  return 0;
}

int Ipv4OfAddresses() {
  CHECK(HttpGate::Ipv4Of(QHostAddress(QStringLiteral("10.0.0.1"))) == Home);
  CHECK(HttpGate::Ipv4Of(QHostAddress(QStringLiteral("::ffff:10.0.0.1"))) ==
        Home);
  CHECK(!HttpGate::Ipv4Of(QHostAddress(QStringLiteral("::1"))));
  CHECK(!HttpGate::Ipv4Of(QHostAddress()));
  return 0;
}

} // namespace

int main() {
  for (int (*test)() :
       {TheRule, LinksComeAndGo, OneAccountTwoLinks, UserIdsAreTheGamesNumbers,
        OffAnswersEveryone, Ipv4OfAddresses}) {
    if (test() != 0)
      return 1;
  }
  return 0;
}
