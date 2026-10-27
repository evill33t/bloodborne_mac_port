// SPDX-FileCopyrightText: Copyright 2026 shadNet Project
// SPDX-License-Identifier: GPL-2.0-or-later
// The operator's commands on a hosted server (admin.h): each command's effect
// on the link, the worlds, the bans and banned.txt; worlds retired after N
// days idle (an injected clock) and put back.
#include <iostream>

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonObject>
#include <QLocalServer>
#include <QLocalSocket>
#include <QTemporaryDir>

#include "admin.h"
#include "admission.h"
#include "bloodborne_world.h"
#include "coop.pb.h"
#include "coop/link_auth.h"
#include "server_link.h"

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

constexpr qint64 Day = 24 * 60 * 60 * 1000;
constexpr qint64 Now = 1'800'000'000'000; // 2027-01-15

using ServerLink::Endpoint;

QByteArray Bytes(const google::protobuf::MessageLite &message) {
  QByteArray bytes(static_cast<qsizetype>(message.ByteSizeLong()), '\0');
  message.SerializeToArray(bytes.data(), static_cast<int>(bytes.size()));
  return bytes;
}

void WriteFile(const QString &path, const QByteArray &bytes) {
  QDir{}.mkpath(QFileInfo{path}.path());
  QFile file{path};
  file.open(QIODevice::WriteOnly);
  file.write(bytes);
}

QString ReadFile(const QString &path) {
  QFile file{path};
  file.open(QIODevice::ReadOnly);
  return QString::fromUtf8(file.readAll());
}

// Worlds on disk as a server keeps them: worlds.pb and a folder each.
void SeedWorlds(const QString &folder,
                const std::vector<std::pair<std::string, qint64>> &played) {
  bbcoop::StoredWorlds stored;
  stored.set_version(1);
  for (const auto &[id, played_ms] : played) {
    bbcoop::KeptWorld &kept = *stored.add_worlds();
    kept.mutable_info()->set_id(id);
    kept.mutable_info()->set_name(id + "'s world");
    kept.mutable_info()->set_played_ms(played_ms);
    kept.mutable_info()->set_created_ms(played_ms - Day);
    kept.mutable_info()->add_members("aa");
    kept.set_folder("folder-" + id);
    bbcoop::TeamWorld world;
    world.add_on(1);
    world.add_on(2);
    world.set_character("Ada");
    WriteFile(QDir{folder}.filePath(
                  QString::fromStdString("folder-" + id + "/team_world.pb")),
              Bytes(world));
  }
  WriteFile(QDir{folder}.filePath("worlds.pb"), Bytes(stored));
}

struct Rig {
  explicit Rig(const QString &dir)
      : world{QDir{dir}.filePath("worlds")},
        bans{QDir{dir}.filePath("banned.txt")} {}

  Admin::Answer Run(const QString &line, qint64 wall_ms = Now) {
    Admin::Context context{hub, world, guard};
    context.ban_list = bans;
    context.link_ms = link_ms;
    context.wall_ms = wall_ms;
    context.retire_ms = retire_ms;
    context.link_load = 0.25;
    Admin::Answer answer = Admin::Run(line, context);
    if (answer.finish) {
      answer.finish(answer.json); // LinkServer runs it off the link's thread
    }
    return answer;
  }
  // A game linked from `from` as `id`.
  void Link(const Endpoint &from, const QByteArray &id) {
    // Link version 4: a cookie first, then the handshake that signs in.
    LinkAuth::Client game;
    ServerLink::Hello hello;
    hello.online_id = id;
    hello.secret = "secret";
    const auto sealed = [&] {
      if (game.HasCookie()) {
        hello.cookie = QByteArray(
            reinterpret_cast<const char *>(game.CookieToEcho().data()),
            LinkAuth::CookieBytes);
      }
      const QByteArray plain = ServerLink::Encode(hello);
      LinkAuth::Key ephemeral{};
      ephemeral[1] =
          static_cast<quint8>(++nonces); // past the bits X25519 clears
      const auto bytes =
          game.SealHello({reinterpret_cast<const u8 *>(plain.constData()),
                          static_cast<size_t>(plain.size())},
                         ephemeral);
      return QByteArray(reinterpret_cast<const char *>(bytes.data()),
                        static_cast<qsizetype>(bytes.size()));
    };
    for (const auto &send : hub.Receive(from, sealed(), link_ms).sends) {
      LinkAuth::Key key{};
      if (const auto cookie =
              ServerLink::DecodeCookie(send.datagram, nullptr, &key)) {
        game.TakeCookie(*cookie, key);
        hub.Receive(from, sealed(), link_ms);
      }
    }
  }

  ServerLink::Hub hub;
  BloodborneWorld::Service world;
  Admission::Guard guard;
  QString bans;
  qint64 link_ms = 1000;
  qint64 retire_ms = 0;
  int nonces = 0;
};

const Endpoint Alice{0xCB007107, 5000}; // 203.0.113.7
const Endpoint Bob{0xCB007108, 5001};   // 203.0.113.8

int PlayersAreListed() {
  QTemporaryDir dir;
  Rig rig{dir.path()};
  rig.world.Load(Now);
  rig.Link(Alice, "Alice");
  rig.Link(Bob, "Bob");
  const auto answer = rig.Run("players");
  CHECK(answer.json["ok"].toBool());
  const QJsonArray players = answer.json["players"].toArray();
  CHECK(players.size() == 2);
  bool alice = false;
  for (const auto &player : players) {
    const QJsonObject row = player.toObject();
    if (row["id"].toString() == "Alice") {
      alice = row["address"].toString() == "203.0.113.7:5000";
    }
  }
  CHECK(alice);
  CHECK(!rig.Run("nonsense").json["ok"].toBool());
  return 0;
}

int AKickUnlinksAtOnce() {
  QTemporaryDir dir;
  Rig rig{dir.path()};
  rig.Link(Alice, "Alice");
  rig.Link(Bob, "Bob");
  auto answer = rig.Run("kick Alice");
  CHECK(answer.json["ok"].toBool() && answer.json["kicked"].toInt() == 1);
  CHECK(rig.hub.Members() == 1);
  CHECK(answer.link.gone.size() == 1 && answer.link.gone[0] == Alice);
  // By its address too; nobody there: not ok.
  answer = rig.Run("kick 203.0.113.8");
  CHECK(answer.json["kicked"].toInt() == 1 && rig.hub.Members() == 0);
  CHECK(!rig.Run("kick Nobody").json["ok"].toBool());
  // Not banned: its next hello links it again.
  rig.Link(Alice, "Alice");
  CHECK(rig.hub.Members() == 1);
  return 0;
}

int BansAreKeptInTheBanListAndEnd() {
  QTemporaryDir dir;
  Rig rig{dir.path()};
  WriteFile(rig.bans, "# the operator's own\nMallory\n");
  rig.Link(Alice, "Alice");
  auto answer = rig.Run("ban Alice 2h griefing in Yahar'gul");
  CHECK(answer.json["ok"].toBool());
  CHECK(rig.hub.Members() == 0); // kicked as well
  CHECK(rig.guard.BannedId("Alice") && rig.guard.BannedId("Mallory"));
  const QString text = ReadFile(rig.bans);
  CHECK(text.contains("# the operator's own") && text.contains("Mallory"));
  CHECK(text.contains("griefing"));
  // A restart reads it again: banned until its end, then not.
  {
    QSet<quint32> addresses;
    QSet<QByteArray> ids;
    Admission::ParseBanList(text, addresses, ids, Now / 1000 + 3600);
    CHECK(ids.contains("Alice") && ids.contains("Mallory"));
    addresses.clear();
    ids.clear();
    Admission::ParseBanList(text, addresses, ids, Now / 1000 + 3 * 3600);
    CHECK(!ids.contains("Alice") && ids.contains("Mallory"));
  }
  // An address, for ever.
  answer = rig.Run("ban 203.0.113.8");
  CHECK(answer.json["ok"].toBool() && rig.guard.Banned(Bob.ipv4, 0));
  CHECK(rig.Run("bans").json["bans"].toArray().size() == 3);
  for (const auto &ban : rig.Run("bans").json["bans"].toArray()) {
    if (ban.toObject()["who"].toString() == "Alice") {
      CHECK(ban.toObject()["note"].toString() == "griefing in Yahar'gul");
    }
  }
  // The list read later, when the 2 h are over: Alice's ban is gone.
  rig.Run("bans", Now + 3 * 3600 * 1000);
  CHECK(!rig.guard.BannedId("Alice"));
  // Unban: off the file (the operator's lines too), off the guard.
  CHECK(rig.Run("unban Mallory").json["ok"].toBool());
  CHECK(rig.Run("unban 203.0.113.8").json["ok"].toBool());
  CHECK(!rig.guard.BannedId("Mallory") && !rig.guard.Banned(Bob.ipv4, 0));
  CHECK(!ReadFile(rig.bans).contains("Mallory"));
  CHECK(ReadFile(rig.bans).contains("# the operator's own"));
  CHECK(!rig.Run("unban Nobody").json["ok"].toBool());
  // A duration that does not read is refused, not a ban for ever.
  CHECK(!rig.Run("ban Alice 3fortnights").json["ok"].toBool());
  CHECK(!rig.Run("ban").json["ok"].toBool());
  return 0;
}

int WorldsAreListedWithTheirSize() {
  QTemporaryDir dir;
  Rig rig{dir.path()};
  SeedWorlds(QDir{dir.path()}.filePath("worlds"),
             {{"w1", Now - Day}, {"w2", Now - 100 * Day}});
  rig.world.Load(Now);
  const auto answer = rig.Run("worlds");
  const QJsonArray worlds = answer.json["worlds"].toArray();
  CHECK(worlds.size() == 2);
  const QJsonObject first = worlds[0].toObject();
  CHECK(first["id"].toString() == "w1");
  CHECK(first["folder"].toString() == "folder-w1");
  CHECK(first["bytes"].toInteger() > 0);
  CHECK(first["members"].toArray().size() == 1);
  CHECK(first["played"].toString().startsWith("2027-01-14"));
  CHECK(answer.json["aside"].toArray().isEmpty());
  return 0;
}

int IdleWorldsRetireAndComeBack() {
  QTemporaryDir dir;
  const QString worlds = QDir{dir.path()}.filePath("worlds");
  SeedWorlds(worlds, {{"w1", Now - Day}, {"w2", Now - 100 * Day}});
  {
    Rig rig{dir.path()};
    rig.world.Load(Now);
    // Off (0 days): nothing retires.
    CHECK(rig.Run("retire").json["retired"].toArray().isEmpty());
    rig.retire_ms = 90 * Day;
    const auto answer = rig.Run("retire");
    CHECK(answer.json["retired"].toArray().size() == 1);
    CHECK(answer.json["retired"].toArray()[0].toString() == "w2");
    rig.world.Drain();
    CHECK(!QDir{worlds}.exists("folder-w2"));
    CHECK(QDir{worlds}.exists("folder-w1"));
    const QJsonArray aside = rig.Run("worlds").json["aside"].toArray();
    CHECK(aside.size() == 1);
    CHECK(aside[0].toObject()["id"].toString() == "w2");
    CHECK(rig.Run("worlds").json["worlds"].toArray().size() == 1);
  }
  // Never deleted, and kept aside across a restart.
  Rig rig{dir.path()};
  rig.world.Load(Now);
  QJsonArray aside = rig.Run("worlds").json["aside"].toArray();
  CHECK(aside.size() == 1);
  const QString name = aside[0].toObject()["folder"].toString();
  CHECK(name.startsWith("folder-w2-"));
  CHECK(QFile::exists(
      QDir{worlds}.filePath("forgotten/" + name + "/team_world.pb")));
  CHECK(!rig.Run("restore nothing-here").json["ok"].toBool());
  const auto restored = rig.Run("restore " + name);
  CHECK(restored.json["ok"].toBool());
  CHECK(restored.json["id"].toString() == "w2");
  // In its folder at once (no Drain): a game's hello or a Start that comes
  // now reads it there, not an empty folder of the same name.
  CHECK(QFile::exists(QDir{worlds}.filePath("folder-w2/team_world.pb")));
  CHECK(!QDir{worlds}.exists("forgotten/" + name));
  rig.world.Drain();
  CHECK(QFile::exists(QDir{worlds}.filePath("folder-w2/team_world.pb")));
  const QJsonArray listed = rig.Run("worlds").json["worlds"].toArray();
  CHECK(listed.size() == 2);
  bool members = false;
  for (const auto &world : listed) {
    if (world.toObject()["id"].toString() == "w2") {
      members = world.toObject()["members"].toArray().size() == 1;
    }
  }
  CHECK(members); // its players see it again
  CHECK(rig.Run("worlds").json["aside"].toArray().isEmpty());
  // Back for good: its last played is now, the next day's retirement keeps it.
  rig.retire_ms = 90 * Day;
  CHECK(rig.Run("retire", Now + Day).json["retired"].toArray().isEmpty());
  // And on the list after a restart.
  rig.world.Drain();
  BloodborneWorld::Service again{worlds};
  again.Load(Now);
  CHECK(again.WorldRows().size() == 2);
  return 0;
}

int ServerStats() {
  QTemporaryDir dir;
  Rig rig{dir.path()};
  SeedWorlds(QDir{dir.path()}.filePath("worlds"), {{"w1", Now - Day}});
  rig.world.Load(Now);
  rig.Link(Alice, "Alice");
  const QJsonObject stats = rig.Run("stats").json;
  CHECK(stats["ok"].toBool());
  CHECK(stats["players"].toInt() == 1);
  CHECK(stats["worlds"].toInt() == 1);
  CHECK(stats["link_load"].toDouble() == 0.25);
  CHECK(stats.contains("hellos_refused") && stats.contains("bans"));
  // Link version 4: the server's public key, what the launcher pins.
  const QString key =
      QString::fromStdString(LinkAuth::HexOf(rig.hub.PublicKey()));
  CHECK(stats["link_key"].toString() == key && stats.contains("bad_hellos"));
  const QJsonObject asked = rig.Run("key").json;
  CHECK(asked["ok"].toBool() && asked["key"].toString() == key);
  return 0;
}

int Durations() {
  CHECK(Admin::ParseDuration("90s") == 90);
  CHECK(Admin::ParseDuration("30m") == 1800);
  CHECK(Admin::ParseDuration("12h") == 12 * 3600);
  CHECK(Admin::ParseDuration("7d") == 7 * 86400);
  CHECK(!Admin::ParseDuration("7"));
  CHECK(!Admin::ParseDuration("d"));
  CHECK(!Admin::ParseDuration("-3d"));
  return 0;
}

// banned.txt edited by hand and read again (every 30 s): a linked game it bans
// goes at once, not only at its next hello.
int ABanListReadAgainUnlinksTheBanned() {
  QTemporaryDir dir;
  Rig rig{dir.path()};
  rig.Link(Alice, "Alice");
  rig.Link(Bob, "Bob");
  WriteFile(rig.bans, "Bob\n");
  Admin::LoadBans(rig.bans, Now / 1000, rig.guard);
  auto result = Admin::KickBanned(rig.hub, rig.guard, rig.link_ms);
  CHECK(result.gone.size() == 1 && result.gone[0] == Bob);
  CHECK(rig.hub.Members() == 1);
  WriteFile(rig.bans, "203.0.113.7\n");
  Admin::LoadBans(rig.bans, Now / 1000, rig.guard);
  result = Admin::KickBanned(rig.hub, rig.guard, rig.link_ms);
  CHECK(result.gone.size() == 1 && result.gone[0] == Alice);
  CHECK(rig.hub.Members() == 0);
  return 0;
}

// The sizes on disk are not read on the link's thread: the command's answer
// gets them in a step LinkServer runs on another thread.
int WorldSizesAreReadOffTheLinkThread() {
  QTemporaryDir dir;
  Rig rig{dir.path()};
  SeedWorlds(QDir{dir.path()}.filePath("worlds"), {{"w1", Now - Day}});
  rig.world.Load(Now);
  Admin::Context context{rig.hub, rig.world, rig.guard};
  context.wall_ms = Now;
  Admin::Answer answer = Admin::Run("worlds", context);
  CHECK(answer.finish != nullptr);
  CHECK(!answer.json["worlds"].toArray()[0].toObject().contains("bytes"));
  answer.finish(answer.json);
  CHECK(answer.json["worlds"].toArray()[0].toObject()["bytes"].toInteger() > 0);
  return 0;
}

// Only banned.txt's bans unlink linked games when it is read again: an
// address the guard banned for a while on its own (its rate) does not take
// every game behind it (one CGNAT) off every 30 s.
int ARateBanDoesNotUnlinkLinkedGames() {
  QTemporaryDir dir;
  Rig rig{dir.path()};
  rig.Link(Alice, "Alice");
  rig.guard.Ban(Alice.ipv4, rig.link_ms + 60'000);
  CHECK(rig.guard.Banned(Alice.ipv4, rig.link_ms)); // its next hello waits
  const auto result = Admin::KickBanned(rig.hub, rig.guard, rig.link_ms);
  CHECK(result.gone.isEmpty());
  CHECK(rig.hub.Members() == 1);
  return 0;
}

// A second server started on the same admin socket does not take it from the
// running one: it is refused, the running server keeps answering.
int ARunningServersAdminSocketIsKept() {
  QTemporaryDir dir;
  const QString path = QDir{dir.path()}.filePath("admin.sock");
  QLocalServer first;
  QString why;
  CHECK(Admin::ListenAdmin(first, path, why));
  QLocalServer second;
  CHECK(!Admin::ListenAdmin(second, path, why));
  CHECK(!why.isEmpty());
  QLocalSocket client;
  client.connectToServer(path);
  CHECK(client.waitForConnected(1000));
  CHECK(first.waitForNewConnection(1000));
  // One left by a crash (nothing answers) is taken over.
  first.close();
  QFile stale{path};
  stale.open(QIODevice::WriteOnly);
  stale.close();
  QLocalServer third;
  CHECK(Admin::ListenAdmin(third, path, why));
  return 0;
}

} // namespace

int main(int argc, char **argv) {
  QCoreApplication app(argc, argv); // the local sockets
  int failed = 0;
  failed |= PlayersAreListed();
  failed |= AKickUnlinksAtOnce();
  failed |= BansAreKeptInTheBanListAndEnd();
  failed |= WorldsAreListedWithTheirSize();
  failed |= IdleWorldsRetireAndComeBack();
  failed |= ServerStats();
  failed |= Durations();
  failed |= ABanListReadAgainUnlinksTheBanned();
  failed |= WorldSizesAreReadOffTheLinkThread();
  failed |= ARateBanDoesNotUnlinkLinkedGames();
  failed |= ARunningServersAdminSocketIsKept();
  if (failed == 0) {
    std::cout << "admin: all checks passed\n";
  }
  return failed;
}
