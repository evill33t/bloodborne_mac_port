// SPDX-FileCopyrightText: Copyright 2026 shadNet Project
// SPDX-License-Identifier: GPL-2.0-or-later
// Party-scoped matching: signs advertised with a party key are only visible to,
// and only claimable by, searches carrying the same key. Signs without a key
// behave as before.
#include <cstdlib>
#include <iostream>

#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>

#include "bloodborne_summon_broker.h"

namespace {

QJsonObject Parse(const QByteArray &raw) {
  QJsonParseError error{};
  const QJsonDocument document = QJsonDocument::fromJson(raw, &error);
  if (error.error != QJsonParseError::NoError || !document.isObject()) {
    std::cerr << "JSON parse failed: " << error.errorString().toStdString()
              << '\n';
    std::exit(1);
  }
  return document.object();
}

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

QByteArray Advertisement(const char *session, int userId) {
  return QByteArray(R"({"MessageId":"SummonDataCreateRequest","SessionId":")") +
         session + R"(","UserId":)" + QByteArray::number(userId) +
         R"(,"CharaId":1,"AreaId":385875968,"AreaRegionId":230100,"ChannelId":0,"MatchingLevel":46,"SummonData":"d","SummonDataVersion":3,"SummonMethod":0,"SummonType":0,"SummonWord":null,"PosX":143,"PosY":-116,"PosZ":-87})";
}

QByteArray Search(int userId) {
  return R"({"MessageId":"SummonDataGetListRequest","SessionId":"host","UserId":)" +
         QByteArray::number(userId) +
         R"(,"AreaId":385875968,"AreaRegionId":230100,"ChannelId":0,"MatchingLevel":46,"SummonDataVersion":3,"SummonMethod":0,"SummonTypeList":[{"SummonType":0}],"SummonWord":null,"DistanceThreshold":100,"GetMaxCount":20,"PosX":143,"PosY":-116,"PosZ":-87})";
}

QByteArray Claim(const char *session, int hostUserId, int targetUserId) {
  return QByteArray(R"({"MessageId":"SummonDataSummonRequest","SessionId":")") +
         session + R"(","UserId":)" + QByteArray::number(hostUserId) +
         R"(,"TargetUserId":)" + QByteArray::number(targetUserId) +
         R"(,"TargetCharaId":1,"HostData":"h"})";
}

} // namespace

int main() {
  using Broker = Bloodborne::SummonBroker;
  const QByteArray partyA =
      "hunters:"
      "5e884898da28047151d0e56f8dc6292773603d0d6aabbdd62a11ef721d1542d8";
  const QByteArray partyB =
      "others:6b86b273ff34fce19d6b804eff5a3f5747ada4eaa22f1d49c01e52ddb7875b4b";

  const QByteArray signA = Advertisement("guest-a", 100);
  const QByteArray signB = Advertisement("guest-b", 200);
  const QByteArray signPublic = Advertisement("guest-public", 300);

  // Party signs are only visible to the same party.
  {
    Broker broker(60'000);
    broker.Advertise(Parse(signA), signA, 10, partyA);
    broker.Advertise(Parse(signB), signB, 10, partyB);
    broker.Advertise(Parse(signPublic), signPublic, 10);

    const QList<QByteArray> forA =
        broker.Search(Parse(Search(1)), 20, {}, partyA);
    CHECK(forA.size() == 1);
    CHECK(forA.front() == signA);

    const QList<QByteArray> forB =
        broker.Search(Parse(Search(2)), 20, {}, partyB);
    CHECK(forB.size() == 1);
    CHECK(forB.front() == signB);

    // A search without a party never sees party signs: they are private.
    const QList<QByteArray> forPublic = broker.Search(Parse(Search(3)), 20);
    CHECK(forPublic.size() == 1);
    CHECK(forPublic.front() == signPublic);
  }

  // Claims only succeed within the same party.
  {
    Broker broker(60'000);
    broker.Advertise(Parse(signA), signA, 10, partyA);

    const QByteArray outsiderClaim = Claim("guest-a", 2, 100);
    CHECK(broker.Claim(Parse(outsiderClaim), outsiderClaim, 20, {}, partyB)
              .status == Broker::ClaimStatus::NotFound);
    CHECK(broker.Claim(Parse(outsiderClaim), outsiderClaim, 21).status ==
          Broker::ClaimStatus::NotFound);
    CHECK(broker.StateFor(QStringLiteral("guest-a"), 100, 22) ==
          Broker::State::Advertised);

    const QByteArray memberClaim = Claim("guest-a", 1, 100);
    const auto claimed =
        broker.Claim(Parse(memberClaim), memberClaim, 30, {}, partyA);
    CHECK(claimed.status == Broker::ClaimStatus::Claimed);
    CHECK(claimed.targetUserId == 100);
  }

  // Party signs still show up for seamless cross-map searches of the same party
  // only.
  {
    Broker::Options options;
    options.seamlessCoop = true;
    Broker broker(options);
    broker.Advertise(Parse(signA), signA, 10, partyA);
    CHECK(broker.Search(Parse(Search(1)), 20, {}, partyA).size() == 1);
    CHECK(broker.Search(Parse(Search(2)), 20, {}, partyB).isEmpty());
  }

  // A player has one sign at a time. When a guest re-rings in a new game
  // session (after dying or restarting), its old, already-claimed sign must not
  // keep being offered to the host: that wastes a claim cycle on a dead
  // session.
  {
    Broker::Options options;
    options.seamlessCoop = true;
    Broker broker(options);
    const QByteArray oldSign = Advertisement("old-session", 100);
    const QByteArray newSign = Advertisement("new-session", 100);
    broker.Advertise(Parse(oldSign), oldSign, 10, partyA);
    const QByteArray oldClaim = Claim("old-session", 1, 100);
    CHECK(broker.Claim(Parse(oldClaim), oldClaim, 20, {}, partyA).status ==
          Broker::ClaimStatus::Claimed);

    broker.Advertise(Parse(newSign), newSign, 30, partyA);
    CHECK(!broker.StateFor(QStringLiteral("old-session"), 100, 31).has_value());
    const QList<QByteArray> found =
        broker.Search(Parse(Search(1)), 31, {}, partyA);
    CHECK(found.size() == 1);
    CHECK(found.front() == newSign);

    // Another user's sign is untouched.
    broker.Advertise(Parse(signB), signB, 40, partyA);
    CHECK(broker.StateFor(QStringLiteral("new-session"), 100, 41) ==
          Broker::State::Advertised);
  }

  // Once a delivered claim has been consumed the guest is in the host's world.
  // If that same game session advertises again (it died and was sent home),
  // the old claim points at a room that no longer exists: the sign must go
  // back to a plain advertisement the host can claim afresh.
  {
    Broker::Options options;
    options.seamlessCoop = true;
    Broker broker(options);
    const QByteArray sign = Advertisement("guest-session", 100);
    const QByteArray removal =
        R"({"MessageId":"SummonDataRemoveRequest","SessionId":"guest-session","UserId":100})";
    const QByteArray claim = Claim("guest-session", 1, 100);

    broker.Advertise(Parse(sign), sign, 10, partyA);
    CHECK(broker.Claim(Parse(claim), claim, 20, {}, partyA).status ==
          Broker::ClaimStatus::Claimed);
    CHECK(broker.Advertise(Parse(sign), sign, 30, partyA).state ==
          Broker::State::Delivered);
    CHECK(broker.Consume(Parse(removal), 40, partyA).retained == 1); // joined

    const auto again = broker.Advertise(Parse(sign), sign, 50, partyA);
    CHECK(again.state == Broker::State::Advertised);
    CHECK(again.pendingClaim.isEmpty());
    CHECK(broker.Claim(Parse(claim), claim, 60, {}, partyA).status ==
          Broker::ClaimStatus::Claimed);
    CHECK(broker.Advertise(Parse(sign), sign, 70, partyA).state ==
          Broker::State::Delivered);
  }

  // A party of three: once the first guest has joined, the host's search must
  // not offer that guest's sign again, or the host never takes the second
  // guest's (port rig 2026-10-09: the second guest kept its sign up for
  // minutes).
  {
    Broker::Options options;
    options.seamlessCoop = true;
    Broker broker(options);
    const QByteArray first = Advertisement("guest-one", 100);
    const QByteArray second = Advertisement("guest-two", 200);
    const QByteArray removal =
        R"({"MessageId":"SummonDataRemoveRequest","SessionId":"guest-one","UserId":100})";
    const QByteArray claim = Claim("guest-one", 1, 100);
    broker.Advertise(Parse(first), first, 10, partyA);
    broker.Advertise(Parse(second), second, 10, partyA);
    broker.Claim(Parse(claim), claim, 20, {}, partyA);
    CHECK(broker.Advertise(Parse(first), first, 30, partyA).state ==
          Broker::State::Delivered);
    CHECK(broker.Consume(Parse(removal), 40, partyA).retained == 1); // joined
    broker.Advertise(Parse(second), second, 45, partyA);
    const QByteArray search = Search(1);
    const auto found = broker.Search(Parse(search), 50, {}, partyA);
    CHECK(found.size() == 1);
    CHECK(Parse(found.front()).value("UserId").toInt() == 200);
  }

  // A consume before delivery is the cross-map handoff, not a join: the claim
  // must survive it so the guest still receives it at the destination.
  {
    Broker::Options options;
    options.seamlessCoop = true;
    Broker broker(options);
    const QByteArray sign = Advertisement("guest-session", 100);
    const QByteArray removal =
        R"({"MessageId":"SummonDataRemoveRequest","SessionId":"guest-session","UserId":100})";
    const QByteArray claim = Claim("guest-session", 1, 100);
    broker.Advertise(Parse(sign), sign, 10, partyA);
    broker.Claim(Parse(claim), claim, 20, {}, partyA);
    CHECK(broker.Consume(Parse(removal), 30, partyA).retained == 1);
    const auto delivered = broker.Advertise(Parse(sign), sign, 40, partyA);
    CHECK(delivered.state == Broker::State::Delivered);
    CHECK(delivered.pendingClaim == claim);
  }

  // A sign its game stopped refreshing (the guest quit or crashed) is not
  // offered any more, even though seamless keeps records for 15 minutes.
  // Otherwise a host claims the dead sign of a previous run and connects to a
  // guest still loading.
  {
    Broker::Options options;
    options.seamlessCoop = true;
    Broker broker(options);
    const QByteArray sign = Advertisement("previous-run", 100);
    const QByteArray search = Search(1);
    broker.Advertise(Parse(sign), sign, 10, partyA);
    CHECK(
        broker
            .Search(Parse(search), 10 + Broker::SignRefreshWindowMs, {}, partyA)
            .size() == 1);
    CHECK(
        broker
            .Search(Parse(search), 11 + Broker::SignRefreshWindowMs, {}, partyA)
            .isEmpty());
    // Refreshing brings it back.
    broker.Advertise(Parse(sign), sign, 20 + Broker::SignRefreshWindowMs,
                     partyA);
    CHECK(
        broker
            .Search(Parse(search), 30 + Broker::SignRefreshWindowMs, {}, partyA)
            .size() == 1);
  }

  // A party guest on another map joins where it is (Kaniel 2026-10-08: no
  // teleport to the host on a join). Its sign used to wait in Preparing until
  // the guest came to the host's map, which a guest joining in place never
  // does: the rig's host in the Cathedral Ward never saw the sign of its guest
  // in Central Yharnam. Now it is found at once, as if beside the host, and the
  // claim reaches the guest on its own map.
  {
    Broker::Options options;
    options.seamlessCoop = true;
    Broker broker(options);
    QByteArray sign = Advertisement("guest-yharnam", 100);
    sign.replace("\"AreaId\":385875968", "\"AreaId\":402718720");
    QByteArray search = Search(1);
    search.replace("\"AreaId\":385875968", "\"AreaId\":402653184");
    const QByteArray wardPlacement =
        "1,18000000,43110000,c2e80000,42800000,c016cbe4,-240100";
    const QByteArray claim = Claim("guest-yharnam", 1, 100);
    broker.Advertise(Parse(sign), sign, 10, partyA);
    const auto found = broker.Search(Parse(search), 20, wardPlacement, partyA);
    CHECK(found.size() == 1);
    CHECK(found.front().contains("\"AreaId\":402653184"));
    CHECK(broker.StateFor(QStringLiteral("guest-yharnam"), 100, 20) ==
          Broker::State::Advertised);
    CHECK(broker.Claim(Parse(claim), claim, 30, wardPlacement, partyA).status ==
          Broker::ClaimStatus::Claimed);
    const auto delivered = broker.Advertise(Parse(sign), sign, 40, partyA);
    CHECK(delivered.state == Broker::State::Delivered);
    CHECK(delivered.pendingClaim == claim);

    // Without a party a sign still comes to the host's map first.
    Broker publicBroker(options);
    publicBroker.Advertise(Parse(sign), sign, 10);
    CHECK(publicBroker.Search(Parse(search), 20, wardPlacement).isEmpty());
    CHECK(publicBroker.StateFor(QStringLiteral("guest-yharnam"), 100, 20) ==
          Broker::State::Preparing);
  }

  // A removal is the party's like everything else (the emulator sends its key
  // with every summon request): one from outside the party, by session or by
  // user, touches nothing.
  {
    Broker broker(60'000);
    broker.Advertise(Parse(signA), signA, 10, partyA);
    const QByteArray bySession =
        R"({"MessageId":"SummonDataRemoveRequest","SessionId":"guest-a"})";
    const QByteArray byUser =
        R"({"MessageId":"SummonDataRemoveRequest","UserId":100})";
    CHECK(broker.Consume(Parse(bySession), 20).consumed == 0);
    CHECK(broker.Consume(Parse(byUser), 20, partyB).consumed == 0);
    CHECK(broker.StateFor(QStringLiteral("guest-a"), 100, 21) ==
          Broker::State::Advertised);
    CHECK(broker.Consume(Parse(byUser), 30, partyA).consumed == 1);
  }

  // Header values are untrusted: trimmed, capped, and anything with control
  // characters or without a "name:secret" shape is treated as no party at all.
  {
    using Bloodborne::NormalizePartyKey;
    CHECK(NormalizePartyKey("  hunters:abc  ") == "hunters:abc");
    CHECK(NormalizePartyKey("").isEmpty());
    CHECK(NormalizePartyKey("nocolon").isEmpty());
    CHECK(NormalizePartyKey(":secret").isEmpty());
    CHECK(NormalizePartyKey("name:").isEmpty());
    CHECK(NormalizePartyKey("bad\r\nname:x").isEmpty());
    CHECK(NormalizePartyKey(QByteArray(200, 'a') + ":x").isEmpty());
  }

  std::cout << "Bloodborne summon party test passed\n";
  return 0;
}
