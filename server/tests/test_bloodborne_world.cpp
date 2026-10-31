// SPDX-FileCopyrightText: Copyright 2026 shadNet Project
// SPDX-License-Identifier: GPL-2.0-or-later
// The party's world on the server (bloodborne_world.h): games that follow it
// through the reliable stream as the real ones do (coop/coop_stream.h,
// coop/world_log.h WorldLogMember), the store on disk across a restart of the
// service, and the lifecycle rules.
#include <algorithm>
#include <functional>
#include <iostream>
#include <map>
#include <memory>
#include <random>
#include <set>
#include <string>
#include <unordered_set>
#include <vector>

#include <QDate>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QTemporaryDir>

#include "bloodborne_world.h"
#include "coop/coop_messages.h"
#include "coop/invasions.h"
#include "coop/packet_routes.h"
#include "coop/trade.h"

using namespace BloodborneCoop;
using BloodborneWorld::Service;

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

const QByteArray Party = "kaniel:secret";

// A game as the emulator runs it against the server: its stream, its copy of
// the log.
struct Game {
  Game(quint32 port, std::string name, quint32 instance,
       QByteArray authenticated = {})
      : endpoint{0x7F000001, static_cast<quint16>(port)}, name{std::move(name)},
        stream{this->name, instance},
        authenticated{authenticated.isEmpty()
                          ? QByteArray::fromStdString(this->name)
                          : std::move(authenticated)} {}

  void Send(const bbcoop::Envelope &envelope, qint64 now_ms) {
    stream.Send(EncodeEnvelope(envelope), now_ms);
  }
  void Hello(qint64 now_ms, const QByteArray &party = Party) {
    bbcoop::Envelope envelope;
    envelope.mutable_hello()->set_protocol(CoopProtocol);
    envelope.mutable_hello()->set_party(party.toStdString());
    envelope.mutable_hello()->set_member(name);
    envelope.mutable_hello()->set_run(run);
    envelope.mutable_hello()->set_host(claims_host.value_or(name == "host"));
    envelope.mutable_hello()->set_enemy_owner(enemy_owner);
    Send(envelope, now_ms);
  }
  void Map(u32 map, qint64 now_ms) {
    bbcoop::Envelope envelope;
    envelope.mutable_map()->set_map(map);
    Send(envelope, now_ms);
  }
  // The owner of an area as this game heard it ("" for none).
  std::string OwnerOf(u32 area) const {
    for (const bbcoop::EnemyOwner &owner : owners.owners()) {
      if (owner.map() == area) {
        return owner.member();
      }
    }
    return {};
  }
  u64 EpochOf(u32 area) const {
    for (const bbcoop::EnemyOwner &owner : owners.owners()) {
      if (owner.map() == area) {
        return owner.epoch();
      }
    }
    return 0;
  }
  void Show(u32 map, const std::vector<CharacterShown> &characters,
            qint64 now_ms) {
    bbcoop::Envelope envelope;
    *envelope.mutable_shown() = ToMessage(map, characters);
    Send(envelope, now_ms);
  }
  void MapState(u32 map, const std::vector<u8> &bits, qint64 now_ms) {
    bbcoop::Envelope envelope;
    envelope.mutable_map_state()->set_map(map);
    envelope.mutable_map_state()->set_bits(
        std::string(bits.begin(), bits.end()));
    Send(envelope, now_ms);
  }
  void ReturnPoint(s32 point, u32 map, qint64 now_ms) {
    bbcoop::Envelope envelope;
    envelope.mutable_return_point()->set_point(point);
    envelope.mutable_return_point()->set_map(map);
    Send(envelope, now_ms);
  }
  void Flags(std::vector<WorldLogFlagEvent> events, qint64 now_ms) {
    bbcoop::Envelope envelope;
    *envelope.mutable_flags() = ToMessage(
        WorldLogFlagProposal{name, member.Epoch(), run, std::move(events)});
    Send(envelope, now_ms);
  }
  void Hp(u64 source, std::vector<EnemyHp> enemies, qint64 now_ms) {
    bbcoop::Envelope envelope;
    *envelope.mutable_hp() =
        ToMessage(WorldLogHpProposal{name,
                                     member.Epoch(),
                                     run,
                                     ++number,
                                     0x18010000,
                                     {source, std::move(enemies)}});
    Send(envelope, now_ms);
  }
  void Propose(const WorldLogKills &kills, qint64 now_ms) {
    bbcoop::Envelope envelope;
    *envelope.mutable_kills() =
        ToMessage(WorldLogProposal{name, member.Epoch(), run, ++number, kills});
    Send(envelope, now_ms);
  }
  void Lifecycle(bbcoop::Lifecycle::Event event, bool host, qint64 now_ms,
                 u32 map = 0) {
    bbcoop::Envelope envelope;
    envelope.mutable_lifecycle()->set_event(event);
    envelope.mutable_lifecycle()->set_host(host);
    envelope.mutable_lifecycle()->set_map(map);
    Send(envelope, now_ms);
  }
  void LobbyHello(bool playing, qint64 now_ms) {
    bbcoop::Envelope envelope;
    envelope.mutable_lobby_hello()->set_protocol(CoopProtocol);
    envelope.mutable_lobby_hello()->set_member(name);
    envelope.mutable_lobby_hello()->set_name("Player " + name);
    envelope.mutable_lobby_hello()->set_playing(playing);
    envelope.mutable_lobby_hello()->set_invasion_rewards(invasion_rewards);
    envelope.mutable_lobby_hello()->set_covenant_rewards(covenant_rewards);
    Send(envelope, now_ms);
  }
  void Team(bbcoop::TeamRequest::Action action, qint64 now_ms,
            std::string other = {}, std::string team = {}, u32 slot = 0,
            std::string character = {}) {
    bbcoop::Envelope envelope;
    bbcoop::TeamRequest &request = *envelope.mutable_team_request();
    request.set_action(action);
    request.set_member(other);
    request.set_team(team);
    request.set_slot(slot);
    request.set_character(character);
    Send(envelope, now_ms);
  }
  void Invade(u32 level, qint64 now_ms, bool cancel = false) {
    bbcoop::Envelope envelope;
    envelope.mutable_invade_request()->set_level(level);
    envelope.mutable_invade_request()->set_cancel(cancel);
    envelope.mutable_invade_request()->set_covenant_effect(covenant_effect);
    Send(envelope, now_ms);
  }
  void Vitals(u32 map, u32 vital, qint64 now_ms, u32 level = 0,
              bool boss = false, bool killed_by_invader = false) {
    bbcoop::Envelope envelope;
    envelope.mutable_vitals()->set_map(map);
    envelope.mutable_vitals()->set_vital(vital);
    envelope.mutable_vitals()->set_hp(vital == 1 ? 100 : 0);
    envelope.mutable_vitals()->set_max(100);
    envelope.mutable_vitals()->set_level(level);
    envelope.mutable_vitals()->set_boss(boss);
    envelope.mutable_vitals()->set_killed_by_invader(killed_by_invader);
    envelope.mutable_vitals()->set_covenant_effect(covenant_effect);
    envelope.mutable_vitals()->mutable_at()->set_x(1);
    Send(envelope, now_ms);
  }
  void Take(const bbcoop::Envelope &envelope, qint64 now_ms) {
    if (envelope.has_invasion_notice()) {
      notices.push_back(envelope.invasion_notice());
    } else if (envelope.has_invasion_start()) {
      invasion = envelope.invasion_start();
    } else if (envelope.has_invader_joined()) {
      joined.push_back(envelope.invader_joined());
    } else if (envelope.has_invasion_end()) {
      ends.push_back(envelope.invasion_end());
    } else if (envelope.has_lobby_state()) {
      lobby = envelope.lobby_state();
      ++lobby_states;
    } else if (envelope.has_team_start()) {
      starts.push_back(envelope.team_start());
    } else if (envelope.has_team_world()) {
      team_world = envelope.team_world();
      ++team_worlds;
    } else if (envelope.has_team_session()) {
      session = envelope.team_session();
      ++sessions;
    } else if (envelope.has_resume_at()) {
      resume_at = envelope.resume_at();
    } else if (envelope.has_load_progress()) {
      loads[envelope.load_progress().member()] = envelope.load_progress();
    } else if (envelope.has_enemy_hit()) {
      hits.push_back(envelope.enemy_hit());
    } else if (envelope.has_altar_record()) {
      altar_records.push_back(envelope.altar_record());
    } else if (envelope.has_ping()) {
      pings.push_back(envelope.ping());
    } else if (envelope.has_team_travel()) {
      travels.push_back(envelope.team_travel());
    } else if (envelope.has_duel_state()) {
      duels.push_back(envelope.duel_state());
    } else if (envelope.has_map_board()) {
      map_board = envelope.map_board();
    } else if (envelope.has_rest_board()) {
      rest_board = envelope.rest_board();
    } else if (envelope.has_trophies()) {
      trophies = envelope.trophies();
    } else if (envelope.has_vitals_board()) {
      vitals_board = envelope.vitals_board();
    } else if (envelope.has_echoes()) {
      for (const bbcoop::EchoKill &kill : envelope.echoes().kills()) {
        echoes.push_back(kill.echoes());
      }
    } else if (envelope.has_portrait()) {
      portraits[envelope.portrait().member()] = envelope.portrait();
    } else if (envelope.has_enemy_owners()) {
      owners = envelope.enemy_owners();
      ++owner_messages;
    } else if (IsTradeMessage(envelope)) {
      if (envelope.has_trade_grant()) {
        if (on_grant) on_grant(envelope.trade_grant());
        grants.push_back(envelope.trade_grant());
      }
      TradeClient::Taken taken = trades.Take(envelope);
      for (TradeItems &items : taken.add) {
        added.push_back(std::move(items));
      }
      for (const bbcoop::Envelope &reply : taken.replies) {
        if (ack_trades) Send(reply, now_ms);
      }
    } else if (envelope.has_welcome()) {
      welcome = envelope.welcome();
    } else if (envelope.has_entries()) {
      const auto step = member.Take(BatchOf(envelope.entries()), now_ms);
      applied.insert(applied.end(), step.applied.begin(), step.applied.end());
    } else if (envelope.has_snapshot()) {
      const SnapshotParts parts = PartsOf(envelope.snapshot());
      member.Take(parts.kills, now_ms);
      member.Take(parts.shown, now_ms);
      member.Take(parts.map_state, now_ms);
      member.Take(parts.hp, now_ms);
      member.Take(parts.flags, now_ms);
      ++snapshots;
    }
  }

  ServerLink::Endpoint endpoint;
  std::string name;
  CoopStream stream;
  QByteArray authenticated;
  WorldLogMember member;
  std::optional<bbcoop::Welcome> welcome;
  std::vector<WorldLogEntry> applied;
  int snapshots = 0;
  bbcoop::MapBoard map_board;
  bbcoop::RestBoard rest_board;
  bbcoop::Trophies trophies;
  bbcoop::VitalsBoard vitals_board;
  std::vector<s32> echoes;
  std::map<std::string, bbcoop::Portrait> portraits;
  bbcoop::EnemyOwners owners;
  int owner_messages = 0;
  bbcoop::LobbyState lobby;
  int lobby_states = 0;
  std::vector<bbcoop::TeamStart> starts;
  std::optional<bbcoop::TeamWorld> team_world;
  int team_worlds = 0;
  std::optional<bbcoop::TeamSession> session;
  std::optional<bbcoop::ResumeAt> resume_at;
  std::optional<bool> claims_host; // its hello's host (default: named "host")
  int sessions = 0;
  std::map<std::string, bbcoop::LoadProgress> loads;
  std::vector<bbcoop::EnemyHit> hits;
  std::vector<bbcoop::AltarRecord> altar_records;
  std::vector<bbcoop::Ping> pings;
  std::vector<bbcoop::TeamTravel> travels;
  std::vector<bbcoop::DuelState> duels;
  void Duel(bbcoop::DuelRequest::Action action, u32 duel, qint64 now_ms,
            u32 map = 0x18010000) {
    bbcoop::Envelope envelope;
    envelope.mutable_duel_request()->set_action(action);
    envelope.mutable_duel_request()->set_duel(duel);
    envelope.mutable_duel_request()->set_map(map);
    Send(envelope, now_ms);
  }
  bool enemy_owner = true; // runs phase 4's owners (its hello says)
  bool invasion_rewards = false;
  bool covenant_rewards = false;
  u32 covenant_effect = 0;
  std::vector<bbcoop::TradeGrant> grants;
  TradeClient trades;
  bool ack_trades = true;
  std::function<void(const bbcoop::TradeGrant&)> on_grant;
  std::vector<TradeItems> added; // what its trades added
  u32 run = 0x1001;
  u32 number = 0;
  bool online = true; // false: its datagrams are lost
  bool deaf = false;  // true: what the server sends it is lost (it never acks)
  QByteArray online_id; // its link's player id (empty: its name)
  std::vector<bbcoop::InvasionNotice> notices;
  std::optional<bbcoop::InvasionStart> invasion;
  std::vector<bbcoop::InvaderJoined> joined;
  std::vector<bbcoop::InvasionEnd> ends;
};

// Datagrams both ways until nothing moves (the service's sends go to the game
// at their endpoint).
void Pump(Service &service, const std::vector<Game *> &games, qint64 now_ms) {
  for (int round = 0; round < 20; ++round) {
    bool moved = false;
    QList<Service::Send> sends;
    for (Game *game : games) {
      for (const std::vector<u8> &body : game->stream.Flush(now_ms)) {
        if (!game->online) {
          continue;
        }
        moved = true;
        const auto result = service.Receive(
            game->endpoint,
            game->online_id.isEmpty() ? game->authenticated : game->online_id,
            QByteArray(reinterpret_cast<const char *>(body.data()),
                       static_cast<qsizetype>(body.size())),
            now_ms);
        sends.append(result.sends);
      }
    }
    sends.append(service.Tick(now_ms).sends);
    for (const Service::Send &send : sends) {
      for (Game *game : games) {
        if (game->online && !game->deaf && game->endpoint == send.to) {
          moved = true;
          const auto bytes =
              std::span{reinterpret_cast<const u8 *>(send.body.constData()),
                        static_cast<size_t>(send.body.size())};
          for (const std::vector<u8> &message :
               game->stream.Receive(bytes, now_ms)) {
            if (const auto envelope = DecodeEnvelope(message)) {
              game->Take(*envelope, now_ms);
            }
          }
        }
      }
    }
    if (!moved) {
      return;
    }
  }
}

bool Killed(const Game &game, u32 handle) {
  return WorldLogHandles(game.member.Kills()).contains(handle);
}

int MembersFollowTheServersLog() {
  Service service{{}};
  Game host{1000, "host", 11};
  Game guest{1001, "guest", 12};
  qint64 now = 1'000'000;
  host.Hello(now);
  guest.Hello(now);
  Pump(service, {&host, &guest}, now);
  CHECK(host.welcome && host.welcome->accepted());
  CHECK(guest.welcome && guest.welcome->accepted());
  CHECK(host.member.Epoch() != 0);
  CHECK(host.member.Epoch() == guest.member.Epoch());
  CHECK(service.Parties() == 1);

  // A kill by either member stands for both; the second member's kill of it is
  // no new entry.
  now += 100;
  host.Propose({{0x18000000, {0x10014066}}}, now);
  Pump(service, {&host, &guest}, now);
  CHECK(Killed(host, 0x10014066));
  CHECK(Killed(guest, 0x10014066));
  now += 100;
  guest.Propose({{0x18000000, {0x10014066}}}, now);
  host.Propose({}, now);
  Pump(service, {&host, &guest}, now);
  CHECK(Killed(guest, 0x10014066)); // the guest's kill keeps it
  now += 100;
  guest.Propose({}, now);
  Pump(service, {&host, &guest}, now);
  CHECK(!Killed(host, 0x10014066));
  CHECK(!Killed(guest, 0x10014066));
  return 0;
}

int AWrongPartyOrProtocolIsRefused() {
  Service service{{}};
  Game game{1000, "host", 11};
  qint64 now = 1'000'000;
  game.Hello(now, "no-secret");
  Pump(service, {&game}, now);
  CHECK(game.welcome && !game.welcome->accepted());
  CHECK(service.Parties() == 0);
  // Nothing it proposes counts before a hello is taken.
  game.Propose({{0x18000000, {1}}}, now);
  Pump(service, {&game}, now);
  CHECK(service.Parties() == 0);
  return 0;
}

int AWipeStartsOneLogForTheParty() {
  Service service{{}};
  Game host{1000, "host", 11};
  Game guest{1001, "guest", 12};
  qint64 now = 1'000'000;
  host.Hello(now);
  guest.Hello(now);
  Pump(service, {&host, &guest}, now);
  host.Propose({{0x18000000, {7}}}, now);
  Pump(service, {&host, &guest}, now);
  const u64 before = guest.member.Epoch();
  CHECK(Killed(guest, 7));

  now += 1000;
  host.Lifecycle(bbcoop::Lifecycle::WIPE, true, now);
  guest.Lifecycle(bbcoop::Lifecycle::WIPE, false, now + 10);
  Pump(service, {&host, &guest}, now + 10);
  const u64 after = guest.member.Epoch();
  CHECK(after != before);
  CHECK(WorldLogEpochFollows(after, before));
  CHECK((after & 0xFFFFFFFF) ==
        (before & 0xFFFFFFFF) + 1); // one new log, not two
  CHECK(!Killed(guest, 7));
  CHECK(StartsAfterAWipe(guest.applied));
  // A member's set from before its wipe, proposed for the old log, does not
  // stand.
  host.number = 100;
  bbcoop::Envelope late;
  *late.mutable_kills() = ToMessage(
      WorldLogProposal{"host", before, host.run, 101, {{0x18000000, {7}}}});
  host.Send(late, now + 20);
  Pump(service, {&host, &guest}, now + 20);
  CHECK(!Killed(guest, 7));
  // A wipe after the grace is a new one.
  now += WorldLogWipeGraceMs + 1000;
  guest.Lifecycle(bbcoop::Lifecycle::WIPE, false, now);
  Pump(service, {&host, &guest}, now);
  CHECK((guest.member.Epoch() & 0xFFFFFFFF) == (after & 0xFFFFFFFF) + 1);
  return 0;
}

int TheHostsSessionEventsStartANewLogAGuestsTakeItsShare() {
  Service service{{}};
  Game host{1000, "host", 11};
  Game guest{1001, "guest", 12};
  qint64 now = 1'000'000;
  host.Hello(now);
  guest.Hello(now);
  Pump(service, {&host, &guest}, now);
  host.Propose({{0x18000000, {1}}}, now);
  guest.Propose({{0x18000000, {2}}}, now);
  Pump(service, {&host, &guest}, now);
  CHECK(Killed(host, 1) && Killed(host, 2));
  const u64 epoch = host.member.Epoch();

  now += 100;
  guest.Lifecycle(bbcoop::Lifecycle::LEAVE, false, now);
  Pump(service, {&host, &guest}, now);
  CHECK(host.member.Epoch() == epoch); // the same log
  CHECK(Killed(host, 1));
  CHECK(!Killed(host, 2)); // its share went

  now += 100;
  host.Lifecycle(bbcoop::Lifecycle::JOIN, true, now);
  Pump(service, {&host, &guest}, now);
  CHECK(host.member.Epoch() != epoch);
  CHECK(!Killed(host, 1));
  return 0;
}

int TheWorldStandsThroughARestartOfTheServer() {
  QTemporaryDir dir;
  CHECK(dir.isValid());
  qint64 now = 1'000'000;
  u64 epoch = 0;
  {
    Service service{dir.path(), 1000, 3,
                    30'000}; // writes the world whole every 3 records
    Game host{1000, "host", 11};
    Game guest{1001, "guest", 12};
    host.Hello(now);
    guest.Hello(now);
    Pump(service, {&host, &guest}, now);
    for (u32 handle = 1; handle <= 5; ++handle) {
      now += 100;
      host.Propose({{0x18000000,
                     [&] {
                       std::set<u32> handles;
                       for (u32 at = 1; at <= handle; ++at) {
                         handles.insert(at);
                       }
                       return handles;
                     }()}},
                   now);
      Pump(service, {&host, &guest}, now);
    }
    guest.Propose({{0x18010000, {9}}}, now);
    Pump(service, {&host, &guest}, now);
    CHECK(Killed(guest, 5) && Killed(host, 9));
    epoch = host.member.Epoch();
  }
  const QString folder =
      QDir{dir.path()}.filePath(BloodborneWorld::PartyFolderName(Party));
  CHECK(QFile::exists(QDir{folder}.filePath("world.pb")));
  CHECK(QFile::exists(QDir{folder}.filePath("world.pb.1")));

  // A new server process: the world comes back from the folder, the games say
  // hello again.
  Service service{dir.path()};
  const auto loaded = service.Load(now);
  CHECK(service.Parties() == 1);
  const WorldLogHost *world = service.World(Party);
  CHECK(world != nullptr);
  CHECK(world->Epoch() == epoch);
  CHECK(WorldLogKillCount(world->Kills()) == 6);
  Game host{1000, "host", 21};
  Game guest{1001, "guest", 22};
  host.run = guest.run = 0x2001; // the games restarted too
  now += 5000;
  host.Hello(now);
  guest.Hello(now);
  Pump(service, {&host, &guest}, now);
  CHECK(host.member.Epoch() == epoch);
  CHECK(Killed(guest, 5) && Killed(guest, 9) && Killed(host, 1));
  // The members' shares came back too: the host's withdrawal takes its kills
  // only.
  now += 100;
  host.Propose({}, now);
  Pump(service, {&host, &guest}, now);
  CHECK(!Killed(guest, 1));
  CHECK(Killed(guest, 9));
  return 0;
}

int AJournalCutShortLoadsUpToItsLastRecord() {
  QTemporaryDir dir;
  qint64 now = 1'000'000;
  {
    Service service{dir.path(), 1000, 1000,
                    1'000'000}; // never compacted: the journal only
    Game host{1000, "host", 11};
    host.Hello(now);
    Pump(service, {&host}, now);
    host.Propose({{0x18000000, {1}}}, now + 1);
    Pump(service, {&host}, now + 1);
    host.Propose({{0x18000000, {1, 2}}}, now + 2);
    Pump(service, {&host}, now + 2);
  }
  const QString folder =
      QDir{dir.path()}.filePath(BloodborneWorld::PartyFolderName(Party));
  // The new world was written whole at once; the kills are in its journal. The
  // last record is cut short (a crash in the write).
  CHECK(QFile::exists(QDir{folder}.filePath("world.pb")));
  QFile journal{QDir{folder}.filePath("journal.pb")};
  CHECK(journal.exists());
  CHECK(journal.resize(journal.size() - 3));
  Service service{dir.path()};
  const auto loaded = service.Load(now + 10);
  CHECK(loaded.log.join(QStringLiteral(" | ")).contains("cut record"));
  const WorldLogHost *world = service.World(Party);
  CHECK(world != nullptr);
  CHECK(WorldLogHandles(world->Kills()) == std::unordered_set<u32>{1});
  // A world file that does not read: the backup.
  return 0;
}

// A journal whose tail does not read (a crash in a write): the world is written
// whole at the load, so the records after it are not written behind the tail
// that does not read.
int AJournalsBrokenTailIsNotWrittenAfter() {
  QTemporaryDir dir;
  qint64 now = 1'000'000;
  {
    Service service{dir.path(), 1000, 1, 30'000}; // every record written whole
    Game host{1000, "host", 11};
    host.Hello(now);
    Pump(service, {&host}, now);
    host.Propose({{0x18000000, {1}}}, now + 1);
    Pump(service, {&host}, now + 1);
  }
  const QString folder =
      QDir{dir.path()}.filePath(BloodborneWorld::PartyFolderName(Party));
  {
    QFile journal{QDir{folder}.filePath("journal.pb")};
    CHECK(journal.open(QIODevice::WriteOnly | QIODevice::Append));
    CHECK(journal.size() == 0);
    journal.write(QByteArray("\x09\x00\x00\x00ab", 6)); // a cut record
  }
  {
    Service service{dir.path(), 1000, 1000, 1'000'000}; // the journal only
    service.Load(now + 10);
    Game host{1000, "host", 21};
    host.run = 0x2001;
    host.Hello(now + 20);
    Pump(service, {&host}, now + 20);
    host.Propose({{0x18000000, {1, 2}}}, now + 30);
    Pump(service, {&host}, now + 30);
  }
  Service service{dir.path()};
  service.Load(now + 40);
  const WorldLogHost *world = service.World(Party);
  CHECK(world != nullptr);
  CHECK((WorldLogHandles(world->Kills()) == std::unordered_set<u32>{1, 2}));
  return 0;
}

int ABrokenWorldFileFallsBackToABackup() {
  QTemporaryDir dir;
  qint64 now = 1'000'000;
  {
    Service service{dir.path(), 1000, 1, 30'000}; // every record written whole
    Game host{1000, "host", 11};
    host.Hello(now);
    Pump(service, {&host}, now);
    host.Propose({{0x18000000, {1}}}, now + 1);
    Pump(service, {&host}, now + 1);
    host.Propose({{0x18000000, {1, 2}}}, now + 2);
    Pump(service, {&host}, now + 2);
  }
  const QString folder =
      QDir{dir.path()}.filePath(BloodborneWorld::PartyFolderName(Party));
  QFile world_file{QDir{folder}.filePath("world.pb")};
  CHECK(world_file.open(QIODevice::WriteOnly | QIODevice::Truncate));
  world_file.write("not a world");
  world_file.close();
  Service service{dir.path()};
  const auto loaded = service.Load(now + 10);
  CHECK(loaded.log.join(QStringLiteral(" | ")).contains("backup 1"));
  const WorldLogHost *world = service.World(Party);
  CHECK(world != nullptr);
  CHECK(WorldLogHandles(world->Kills()) ==
        std::unordered_set<u32>{1}); // the world before
  return 0;
}

// Phase 11: besides the three backups of the last writes, one copy a day for a
// week (a bad day's worlds can be taken back), in backups/.
int AWorldKeepsADatedCopyADayForAWeek() {
  QTemporaryDir dir;
  BloodborneWorld::WorldStore store{dir.path()};
  bbcoop::StoredWorld world;
  world.set_version(1);
  world.set_world(7);
  const QDir backups{QDir{dir.path()}.filePath("backups")};
  CHECK(store.Compact(world, QDate{2026, 10, 1}));
  CHECK(store.Compact(world, QDate{2026, 10, 1}));
  CHECK(backups.entryList(QDir::Files) ==
        QStringList{"world-20261001.pb"}); // one a day
  for (int day = 2; day <= 10; ++day) {
    world.set_world(static_cast<u64>(day));
    CHECK(store.Compact(world, QDate{2026, 10, day}));
  }
  const QStringList kept = backups.entryList(QDir::Files, QDir::Name);
  CHECK(kept.size() == 7);
  CHECK(kept.first() == "world-20261004.pb" &&
        kept.last() == "world-20261010.pb");
  // Each is the world as that day's first write left it.
  QFile copy{backups.filePath("world-20261010.pb")};
  CHECK(copy.open(QIODevice::ReadOnly));
  bbcoop::StoredWorld read;
  const QByteArray bytes = copy.readAll();
  CHECK(read.ParseFromArray(bytes.constData(), static_cast<int>(bytes.size())));
  CHECK(read.world() == 10);
  return 0;
}

int TheServerRestartingMidSessionLosesNothing() {
  QTemporaryDir dir;
  qint64 now = 1'000'000;
  Game host{1000, "host", 11};
  Game guest{1001, "guest", 12};
  auto service = std::make_unique<Service>(dir.path());
  host.Hello(now);
  guest.Hello(now);
  Pump(*service, {&host, &guest}, now);
  host.Propose({{0x18000000, {0x10014066}}}, now);
  Pump(*service, {&host, &guest}, now);
  CHECK(Killed(guest, 0x10014066));
  const u64 epoch = guest.member.Epoch();

  // shadNet stops and starts again; the games keep running and find a new
  // stream.
  service.reset();
  service = std::make_unique<Service>(dir.path());
  service->Load(now);
  now += 3000;
  // The first frames of the games are for the old server: the new one answers,
  // they start over.
  host.Propose({{0x18000000, {0x10014066}}}, now);
  Pump(*service, {&host, &guest}, now);
  CHECK(host.stream.TakeRestarted());
  CHECK(guest.stream.TakeRestarted());
  host.Hello(now);
  guest.Hello(now);
  Pump(*service, {&host, &guest}, now);
  CHECK(guest.member.Epoch() == epoch);
  CHECK(Killed(guest, 0x10014066)); // the kill stands for all
  now += 100;
  guest.Propose({{0x18000000, {0x10014067}}}, now);
  Pump(*service, {&host, &guest}, now);
  CHECK(Killed(host, 0x10014067));
  return 0;
}

int ALostDatagramIsSentAgain() {
  Service service{{}};
  Game host{1000, "host", 11};
  Game guest{1001, "guest", 12};
  qint64 now = 1'000'000;
  host.Hello(now);
  guest.Hello(now);
  Pump(service, {&host, &guest}, now);
  guest.online = false;
  host.Propose({{0x18000000, {3}}}, now);
  Pump(service, {&host, &guest}, now);
  CHECK(!Killed(guest, 3));
  guest.online = true;
  now += 500; // past the resend time
  Pump(service, {&host, &guest}, now);
  CHECK(Killed(guest, 3));
  return 0;
}

bool FlagApplied(const Game &game, s32 flag, bool on) {
  return std::ranges::any_of(game.applied, [&](const WorldLogEntry &entry) {
    return entry.kind ==
               (on ? WorldLogKind::FlagSet : WorldLogKind::FlagClear) &&
           static_cast<s32>(entry.subject) == flag;
  });
}

std::vector<u8> MapBits(size_t lit) {
  std::vector<u8> bits((MapFlagCount + 7) / 8);
  bits[lit / 8] = static_cast<u8>(0x80 >> (lit % 8));
  return bits;
}

int TheHostsWorldAndEveryonesFlagsAndHpReachEveryMember() {
  QTemporaryDir dir;
  qint64 now = 1'000'000;
  const std::vector<CharacterShown> characters{
      {0x10014000, false, true, false, false}, // in handle order, as the log
      {0x10014005, true, false, false, false}};
  {
    Service service{dir.path()};
    Game host{1000, "host", 11};
    Game guest{1001, "guest", 12};
    host.Hello(now);
    guest.Hello(now);
    Pump(service, {&host, &guest}, now);
    host.Show(0x18010000, characters, now);
    host.MapState(0x18010000, MapBits(13), now);
    host.ReturnPoint(2412950, 0x18010000, now);
    // A guest's world is not the party's: its characters and map state count
    // for nothing.
    guest.Show(0x18010000, {{0x10014005, false, false, false, false}}, now);
    guest.MapState(0x18010000, MapBits(2), now);
    guest.Flags({{1, 2412950, true}}, now);
    guest.Hp(0x1111000000000003, {{0x10014066, 49, 450}}, now);
    Pump(service, {&host, &guest}, now);
    CHECK(guest.member.ShownKnown());
    CHECK(WorldLogShownCharacters(guest.member.Shown()) == characters);
    CHECK(guest.member.MapState() ==
          (WorldLogMapState{0x18010000, MapBits(13)}));
    CHECK(guest.member.HostReturnPoint() ==
          (WorldLogReturnPoint{2412950, 0x18010000}));
    CHECK(FlagApplied(host, 2412950, true));
    CHECK(FlagApplied(guest, 2412950, true));
    CHECK(host.member.Hp().contains(0x1111000000000003));
    // The same event again (a resend) is no new entry.
    const size_t applied = host.applied.size();
    guest.Flags({{1, 2412950, true}}, now + 100);
    Pump(service, {&host, &guest}, now + 100);
    CHECK(host.applied.size() == applied);
  }
  // The world comes back from the disk with all of it: a game after the restart
  // takes it whole.
  Service service{dir.path()};
  service.Load(now + 1000);
  Game late{1002, "late", 13};
  late.Hello(now + 1000);
  Pump(service, {&late}, now + 1000);
  CHECK(late.snapshots >= 1);
  CHECK(WorldLogShownCharacters(late.member.Shown()) == characters);
  CHECK(late.member.MapState() == (WorldLogMapState{0x18010000, MapBits(13)}));
  CHECK(late.member.HostReturnPoint() ==
        (WorldLogReturnPoint{2412950, 0x18010000}));
  CHECK(late.member.Hp().contains(0x1111000000000003));
  return 0;
}

bbcoop::Envelope VitalsOf(u32 map, u32 vital, u32 down = 0,
                          std::vector<bbcoop::ReviveHit> hits = {}) {
  bbcoop::Envelope envelope;
  bbcoop::Vitals &vitals = *envelope.mutable_vitals();
  vitals.set_map(map);
  vitals.set_vital(vital);
  vitals.set_down(down);
  for (const bbcoop::ReviveHit &hit : hits) {
    *vitals.add_hits() = hit;
  }
  return envelope;
}

const bbcoop::VitalsBoard::Member *VitalsFor(const Game &game,
                                             const std::string &member) {
  for (const auto &entry : game.vitals_board.members()) {
    if (entry.member() == member) {
      return &entry;
    }
  }
  return nullptr;
}

int TheBoardsGoToEveryMember() {
  Service service{{}};
  Game host{1000, "host", 11};
  Game guest{1001, "guest", 12};
  qint64 now = 1'000'000;
  host.Hello(now);
  guest.Hello(now);
  Pump(service, {&host, &guest}, now);

  // Maps: every member's, to everyone.
  bbcoop::Envelope map;
  map.mutable_map()->set_map(0x18010000);
  host.Send(map, now);
  map.mutable_map()->set_map(0x18000000);
  guest.Send(map, now);
  // A rest with its place.
  bbcoop::Envelope rest;
  rest.mutable_rest()->set_lamp(2412950);
  rest.mutable_rest()->mutable_at()->set_x(-215.5f);
  guest.Send(rest, now);
  // Trophies: the party's union.
  bbcoop::Envelope trophies;
  trophies.mutable_trophies()->add_ids(5);
  trophies.mutable_trophies()->add_ids(500); // no such trophy
  host.Send(trophies, now);
  trophies.mutable_trophies()->set_ids(0, 7);
  guest.Send(trophies, now);
  // Echoes: to the others only, once.
  bbcoop::Envelope echoes;
  bbcoop::EchoKill *kill = echoes.mutable_echoes()->add_kills();
  kill->set_echoes(120);
  kill->set_map(0x18000000);
  guest.Send(echoes, now);
  Pump(service, {&host, &guest}, now);
  CHECK(host.map_board.members_size() == 2);
  CHECK(guest.map_board.members_size() == 2);
  CHECK(host.rest_board.rests_size() == 1);
  CHECK(host.rest_board.rests(0).member() == "guest");
  CHECK(host.rest_board.rests(0).lamp() == 2412950);
  CHECK(host.rest_board.rests(0).at().x() == -215.5f);
  CHECK(host.trophies.ids_size() == 2);
  CHECK(host.trophies.ids(0) == 5 && host.trophies.ids(1) == 7);
  CHECK(host.echoes == std::vector<s32>{120});
  CHECK(guest.echoes.empty());

  // Standing up takes the rest off; a member not heard of goes from the boards.
  rest.mutable_rest()->set_lamp(0);
  guest.Send(rest, now + 100);
  Pump(service, {&host, &guest}, now + 100);
  CHECK(host.rest_board.rests_size() == 0);
  map.mutable_map()->set_map(0x18010000);
  host.Send(map, now + 4000);
  Pump(service, {&host, &guest}, now + 4000);
  Pump(service, {&host, &guest}, now + 6000);
  CHECK(host.map_board.members_size() == 1);
  CHECK(host.map_board.members(0).member() == "host");

  // A game that comes later gets the boards as they are.
  Game late{1002, "late", 13};
  late.Hello(now + 6100);
  Pump(service, {&host, &guest, &late}, now + 6100);
  CHECK(late.trophies.ids_size() == 2);
  CHECK(late.map_board.members_size() == 1);
  return 0;
}

// Kaniel's playtest 2026-10-16: Lisara went down alone in the Hunter's Dream
// (a wipe of her map) while Kaniel and krolebord stood in Central Yharnam; her
// WIPE started a new log for the whole party, so Kaniel's game showed every
// enemy they had killed there again. A member's wipe while another member
// stands anywhere is its own: the party's log stands.
int AMembersOwnWipeKeepsThePartysLog() {
  Service service{{}};
  Game host{1000, "host", 11};
  Game guest{1001, "guest", 12};
  Game other{1002, "other", 13};
  qint64 now = 1'000'000;
  for (Game *game : {&host, &guest, &other}) {
    game->Hello(now);
  }
  Pump(service, {&host, &guest, &other}, now);
  constexpr u32 Up = 1;
  constexpr u32 Down = 2;
  host.Propose({{0x18010000, {7}}}, now);
  host.Send(VitalsOf(0x18010000, Up), now);
  other.Send(VitalsOf(0x18010000, Up), now);
  guest.Send(VitalsOf(0x15000000, Down, 3), now);
  Pump(service, {&host, &guest, &other}, now);
  const u64 before = other.member.Epoch();
  CHECK(Killed(other, 7));
  now += 1000;
  guest.Lifecycle(bbcoop::Lifecycle::WIPE, false, now);
  Pump(service, {&host, &guest, &other}, now);
  CHECK(other.member.Epoch() == before);
  CHECK(Killed(other, 7));
  // Everyone down: the party's wipe, a new log.
  now += WorldLogWipeGraceMs + 1000;
  host.Send(VitalsOf(0x18010000, Down, 5), now);
  other.Send(VitalsOf(0x18010000, Down, 6), now);
  Pump(service, {&host, &guest, &other}, now);
  host.Lifecycle(bbcoop::Lifecycle::WIPE, true, now + 10);
  Pump(service, {&host, &guest, &other}, now + 10);
  CHECK(other.member.Epoch() != before);
  CHECK(!Killed(other, 7));
  return 0;
}

// Kaniel's play test 2026-10-19: Kaniel and Lisara died on Cathedral Ward
// while krolebord stood in the Hunter's Dream, and the Ward came back half
// respawned, different for each of them. Nobody stands on the Ward, so it
// starts over alone (and Central Yharnam, loaded with it): its kills go, even
// krolebord's held ones, while the Dream's stay and no new log starts. With
// someone standing on a map loaded with it, the log stands.
int AWipeOfOneMapStartsThatMapOverAlone() {
  Service service{{}};
  Game host{1000, "host", 11};
  Game guest{1001, "guest", 12};
  Game other{1002, "other", 13};
  qint64 now = 1'000'000;
  for (Game *game : {&host, &guest, &other}) {
    game->Hello(now);
  }
  Pump(service, {&host, &guest, &other}, now);
  constexpr u32 Up = 1;
  constexpr u32 Down = 2;
  constexpr u32 Ward = 0x18000000;
  constexpr u32 Yharnam = 0x18010000;
  constexpr u32 Dream = 0x15000000;
  host.Propose({{Ward, {7}}}, now);
  other.Propose({{Ward, {8}}, {Dream, {9}}}, now);
  host.Send(VitalsOf(Ward, Down, 5), now);
  guest.Send(VitalsOf(Ward, Down, 6), now);
  other.Send(VitalsOf(Dream, Up), now);
  Pump(service, {&host, &guest, &other}, now);
  const u64 before = other.member.Epoch();
  CHECK(Killed(other, 7) && Killed(other, 8) && Killed(other, 9));
  // The host respawned first and stands at its lamp on the Ward again before
  // it tells its wipe: a member of the wipe does not count as standing there.
  now += 5000;
  host.Send(VitalsOf(Ward, Up), now);
  host.Lifecycle(bbcoop::Lifecycle::WIPE, true, now, Ward);
  Pump(service, {&host, &guest, &other}, now);
  CHECK(other.member.Epoch() == before);
  CHECK(!Killed(other, 7) && !Killed(other, 8) && !Killed(host, 7));
  CHECK(Killed(other, 9));
  // krolebord's game still holds its Ward kill: it does not stand again.
  now += 100;
  other.Propose({{Ward, {8}}, {Dream, {9}}}, now);
  guest.Lifecycle(bbcoop::Lifecycle::WIPE, false, now, Ward);
  Pump(service, {&host, &guest, &other}, now);
  CHECK(!Killed(guest, 8) && Killed(guest, 9));
  // A new kill there stands.
  now += 1000;
  host.Propose({{Ward, {10}}}, now);
  Pump(service, {&host, &guest, &other}, now);
  CHECK(Killed(other, 10));
  // Someone stands on Central Yharnam, loaded with the Ward: the log stands.
  now += WorldLogWipeGraceMs + 1000;
  other.Send(VitalsOf(Yharnam, Up), now);
  guest.Send(VitalsOf(Ward, Down, 7), now);
  host.Send(VitalsOf(Ward, Down, 8), now);
  Pump(service, {&host, &guest, &other}, now);
  guest.Lifecycle(bbcoop::Lifecycle::WIPE, false, now + 10, Ward);
  Pump(service, {&host, &guest, &other}, now + 10);
  CHECK(Killed(other, 10));
  CHECK(other.member.Epoch() == before);
  return 0;
}

// Kaniel's play test 2026-10-25 (Logarius): Lisara fell into the arena's pit
// while the others stood, krolebord fell after her while Kaniel lay down, and
// Kaniel's down ended in the wipe. By the time krolebord's and Kaniel's games
// told their wipe (after the death's animation) Lisara stood at the lamp
// again, so shadNet took both wipes for their own and the log kept the boss's
// HP. A member that was not standing since the wiping member fell does not
// count as standing.
int AMemberUpAgainAfterDyingFirstDoesNotStopTheWipe() {
  Service service{{}};
  Game host{1000, "kaniel", 11};
  Game lisara{1001, "lisara", 12};
  Game krolebord{1002, "krolebord", 13};
  qint64 now = 1'000'000;
  for (Game *game : {&host, &lisara, &krolebord}) {
    game->Hello(now);
  }
  Pump(service, {&host, &lisara, &krolebord}, now);
  constexpr u32 Up = 1;
  constexpr u32 Down = 2;
  constexpr u32 Dead = 3;
  constexpr u32 Castle = 0x19000000;
  host.Propose({{Castle, {7}}}, now);
  for (Game *game : {&host, &lisara, &krolebord}) {
    game->Send(VitalsOf(Castle, Up), now);
  }
  Pump(service, {&host, &lisara, &krolebord}, now);
  const u64 before = krolebord.member.Epoch();
  CHECK(Killed(krolebord, 7));
  host.Send(VitalsOf(Castle, Down, 18), now + 1000);
  lisara.Send(VitalsOf(Castle, Dead), now + 6000);      // the pit
  krolebord.Send(VitalsOf(Castle, Dead), now + 10'000); // the pit too
  Pump(service, {&host, &lisara, &krolebord}, now + 10'000);
  host.Send(VitalsOf(Castle, Dead, 18), now + 10'050); // the server's wipe
  Pump(service, {&host, &lisara, &krolebord}, now + 10'050);
  // Lisara stands at the lamp again; then the others' respawns tell the wipe.
  lisara.Send(VitalsOf(Castle, Up), now + 15'000);
  Pump(service, {&host, &lisara, &krolebord}, now + 15'000);
  krolebord.Lifecycle(bbcoop::Lifecycle::WIPE, false, now + 19'000);
  Pump(service, {&host, &lisara, &krolebord}, now + 19'000);
  CHECK(krolebord.member.Epoch() != before);
  CHECK(!Killed(krolebord, 7) && !Killed(lisara, 7));
  // Someone who stood all along still keeps the log: no wipe of one stops it.
  const u64 after = krolebord.member.Epoch();
  now += 10 * 60'000;
  for (Game *game : {&host, &lisara, &krolebord}) {
    game->Send(VitalsOf(Castle, Up), now);
  }
  host.Propose({{Castle, {8}}}, now);
  Pump(service, {&host, &lisara, &krolebord}, now);
  krolebord.Send(VitalsOf(Castle, Dead), now + 1000);
  Pump(service, {&host, &lisara, &krolebord}, now + 1000);
  krolebord.Send(VitalsOf(Castle, Up), now + 9000);
  krolebord.Lifecycle(bbcoop::Lifecycle::WIPE, false, now + 9000);
  Pump(service, {&host, &lisara, &krolebord}, now + 9000);
  CHECK(krolebord.member.Epoch() == after);
  CHECK(Killed(krolebord, 8));
  return 0;
}

int ThePartyIsWipedWhenNobodyOnTheMapStands() {
  Service service{{}};
  Game host{1000, "host", 11};
  Game guest{1001, "guest", 12};
  qint64 now = 1'000'000;
  host.Hello(now);
  guest.Hello(now);
  Pump(service, {&host, &guest}, now);
  constexpr u32 Up = 1;
  constexpr u32 Down = 2;
  host.Send(VitalsOf(0x18010000, Up), now);
  guest.Send(VitalsOf(0x18010000, Down, 4), now);
  Pump(service, {&host, &guest}, now);
  CHECK(host.vitals_board.members_size() == 2);
  CHECK(host.vitals_board.wipes_size() == 0);
  // The host's revive hits count on the guest's down.
  bbcoop::ReviveHit hit;
  hit.set_target("guest");
  hit.set_down(4);
  hit.set_damage(90);
  host.Send(VitalsOf(0x18010000, Up, 0, {hit}), now + 100);
  Pump(service, {&host, &guest}, now + 100);
  const auto *downed = VitalsFor(guest, "guest");
  CHECK(downed != nullptr && downed->revive_damage() == 90);
  CHECK(VitalsFor(guest, "host")->vitals().hits_size() == 0);
  // Both down on the map: one wipe, for both.
  host.Send(VitalsOf(0x18010000, Down, 9), now + 200);
  Pump(service, {&host, &guest}, now + 200);
  CHECK(guest.vitals_board.wipes_size() == 1);
  CHECK(guest.vitals_board.wipes(0).members_size() == 2);
  guest.Send(VitalsOf(0x18010000, Down, 4), now + 300);
  Pump(service, {&host, &guest}, now + 300);
  CHECK(guest.vitals_board.wipes_size() == 1); // not again
  // It goes after a while.
  host.Send(VitalsOf(0x18010000, Up), now + 21'000);
  guest.Send(VitalsOf(0x18010000, Up), now + 21'000);
  Pump(service, {&host, &guest}, now + 21'000);
  CHECK(guest.vitals_board.wipes_size() == 0);
  return 0;
}

// Kaniel 2026-10-25: a member spectating its teammates (vital 4) neither
// stands for the others nor is in a wipe: alone on its map nothing is wiped,
// and a wipe of the members down there leaves it out (its own game sees the
// map's party gone and wakes it). Its lamp count reaches the others.
int ASpectatorIsNoOneTheServerWipes() {
  Service service{{}};
  Game host{1000, "host", 11};
  Game guest{1001, "guest", 12};
  qint64 now = 1'000'000;
  host.Hello(now);
  guest.Hello(now);
  Pump(service, {&host, &guest}, now);
  constexpr u32 Down = 2;
  constexpr u32 Spectate = 4;
  bbcoop::Envelope spectating = VitalsOf(0x18010000, Spectate);
  spectating.mutable_vitals()->set_lamp_uses(3);
  guest.Send(spectating, now);
  Pump(service, {&host, &guest}, now);
  CHECK(host.vitals_board.wipes_size() == 0);
  CHECK(VitalsFor(host, "guest") != nullptr &&
        VitalsFor(host, "guest")->vitals().vital() == Spectate &&
        VitalsFor(host, "guest")->vitals().lamp_uses() == 3);
  host.Send(VitalsOf(0x18010000, Down, 5), now + 100);
  Pump(service, {&host, &guest}, now + 100);
  CHECK(host.vitals_board.wipes_size() == 1);
  CHECK(host.vitals_board.wipes(0).members_size() == 1 &&
        host.vitals_board.wipes(0).members(0) == "host");
  return 0;
}

// Whatever a client sends (the HTTP boards' parsers had a fuzz test; the
// stream's frames and our messages have this one): random bytes, random frames
// and messages with random fields never crash the service, and a member that
// behaves still follows the log after them.
int WhateverAGameSendsTheServiceKeepsWorking() {
  Service service{{}};
  std::mt19937 random{20261013};
  const auto bytes = [&](size_t size) {
    std::vector<u8> out(size);
    for (u8 &byte : out) {
      byte = static_cast<u8>(random());
    }
    return out;
  };
  qint64 now = 1'000'000;
  for (int round = 0; round < 3000; ++round) {
    const ServerLink::Endpoint from{0x7F000001,
                                    static_cast<quint16>(2000 + round % 7)};
    std::vector<u8> body;
    switch (round % 3) {
    case 0:
      body = bytes(random() % 300);
      break;
    case 1: {
      // A well-formed frame with random chunks in it.
      GameFrame frame{"fuzz", 77, 0, 0, {}};
      for (u32 entry = 0; entry < random() % 4; ++entry) {
        frame.entries.push_back(
            {PacketClass::Reliable, entry, bytes(random() % 200)});
      }
      body = EncodeGameFrame(frame);
      break;
    }
    default: {
      // A hello, then a well-formed message of a random kind with random
      // fields.
      CoopStream stream{"fuzz", static_cast<u32>(round + 1)};
      bbcoop::Envelope hello;
      hello.mutable_hello()->set_protocol(CoopProtocol);
      hello.mutable_hello()->set_party("fuzz:party");
      hello.mutable_hello()->set_member("f" + std::to_string(round % 5));
      hello.mutable_hello()->set_host(round % 2 == 0);
      stream.Send(EncodeEnvelope(hello), now);
      std::vector<u8> message = bytes(random() % 120);
      if (!message.empty()) {
        message[0] =
            static_cast<u8>(((random() % 21 + 1) << 3) | 2); // a field's tag
      }
      stream.Send(message, now);
      for (const std::vector<u8> &frame_body : stream.Flush(now)) {
        service.Receive(
            from, "fuzz",
            QByteArray(reinterpret_cast<const char *>(frame_body.data()),
                       static_cast<qsizetype>(frame_body.size())),
            now);
      }
      continue;
    }
    }
    service.Receive(from, "fuzz",
                    QByteArray(reinterpret_cast<const char *>(body.data()),
                               static_cast<qsizetype>(body.size())),
                    now);
    if (round % 100 == 0) {
      now += 700;
      service.Tick(now);
    }
  }
  Game host{1000, "host", 11};
  Game guest{1001, "guest", 12};
  host.Hello(now);
  guest.Hello(now);
  Pump(service, {&host, &guest}, now);
  host.Propose({{0x18000000, {42}}}, now + 10);
  Pump(service, {&host, &guest}, now + 10);
  CHECK(Killed(guest, 42));
  return 0;
}

int APortraitGoesToTheOthersAndToLaterMembers() {
  Service service{{}};
  Game host{1000, "host", 11};
  Game guest{1001, "guest", 12};
  qint64 now = 1'000'000;
  host.Hello(now);
  guest.Hello(now);
  Pump(service, {&host, &guest}, now);
  bbcoop::Envelope portrait;
  portrait.mutable_portrait()->set_member("spoofed");
  portrait.mutable_portrait()->set_id(0xabcd);
  // 128 px in RGB888 (Kaniel 2026-10-17: sharper, true colour): the stream
  // carries it whole.
  portrait.mutable_portrait()->set_pixels(std::string(128 * 128 * 3, 'p'));
  host.Send(portrait, now);
  Pump(service, {&host, &guest}, now);
  CHECK(guest.portraits.contains("host"));
  CHECK(guest.portraits.at("host").pixels().size() == 128 * 128 * 3);
  CHECK(!guest.portraits.contains("spoofed"));
  CHECK(host.portraits.empty());
  Game late{1002, "late", 13};
  late.Hello(now + 10);
  Pump(service, {&host, &guest, &late}, now + 10);
  CHECK(late.portraits.contains("host"));
  CHECK(late.portraits.at("host").id() == 0xabcd);
  return 0;
}

// Phase 6: the host's open session reaches its guests (and later ones) to join;
// a guest's says nothing, and a closed one goes too.
int TheHostsSessionReachesItsGuestsToJoin() {
  Service service{{}};
  Game host{1000, "host", 11};
  Game guest{1001, "guest", 12};
  qint64 now = 1'000'000;
  host.Hello(now);
  guest.Hello(now);
  Pump(service, {&host, &guest}, now);
  CHECK(!guest.session.has_value());
  bbcoop::Envelope open;
  open.mutable_team_session()->set_join(std::string("NXRV\x05\x84", 6));
  open.mutable_team_session()->set_kind(0xFFFFFFFFu);
  open.mutable_team_session()->set_sign(7);
  open.mutable_team_session()->set_session(1);
  open.mutable_team_session()->set_host("spoofed");
  host.Send(open, now);
  Pump(service, {&host, &guest}, now);
  CHECK(guest.session.has_value());
  CHECK(guest.session->join() == std::string("NXRV\x05\x84", 6));
  CHECK(guest.session->kind() == 0xFFFFFFFFu && guest.session->sign() == 7);
  CHECK(guest.session->host() == "host");
  CHECK(!host.session.has_value());
  // A guest's "session" is no team's.
  bbcoop::Envelope other = open;
  other.mutable_team_session()->set_join("guest's");
  guest.Send(other, now + 5);
  Pump(service, {&host, &guest}, now + 5);
  CHECK(!host.session.has_value());
  Game late{1002, "late", 13};
  late.Hello(now + 10);
  Pump(service, {&host, &guest, &late}, now + 10);
  CHECK(late.session.has_value() && late.session->session() == 1);
  // Closed: everyone hears it, and a later member gets none.
  bbcoop::Envelope closed;
  closed.mutable_team_session()->set_session(1);
  host.Send(closed, now + 20);
  Pump(service, {&host, &guest, &late}, now + 20);
  CHECK(guest.session.has_value() && guest.session->join().empty());
  Game later{1003, "later", 14};
  later.Hello(now + 30);
  Pump(service, {&host, &guest, &late, &later}, now + 30);
  CHECK(!later.session.has_value());
  return 0;
}

// Rig 2026-10-14 (team-world): a host whose game died left its session on the
// server, and the guests joined a room that was gone. A new run of the host, or
// the host's link gone, closes it.
int AHostsSessionGoesWithItsGame() {
  Service service{{}};
  Game host{1000, "host", 11};
  Game guest{1001, "guest", 12};
  qint64 now = 1'000'000;
  host.Hello(now);
  guest.Hello(now);
  Pump(service, {&host, &guest}, now);
  bbcoop::Envelope open;
  open.mutable_team_session()->set_join("NXRV");
  open.mutable_team_session()->set_session(1);
  host.Send(open, now);
  Pump(service, {&host, &guest}, now);
  CHECK(guest.session.has_value() && !guest.session->join().empty());
  // The host's game starts again (a crash, a quit): its old session is no more.
  Game host_again{1003, "host", 21};
  host_again.run = 0x2002;
  host_again.Hello(now + 10);
  Pump(service, {&host_again, &guest}, now + 10);
  CHECK(guest.session.has_value() && guest.session->join().empty());
  Game late{1002, "late", 13};
  late.Hello(now + 20);
  Pump(service, {&host_again, &guest, &late}, now + 20);
  CHECK(!late.session.has_value());
  return 0;
}

// Kaniel 2026-10-14: the team loading screen. Each member's load reaches the
// others, under its own name, and a member that says hello later gets the
// others' as they are.
int EachMembersLoadReachesTheOthers() {
  Service service{{}};
  Game host{1000, "host", 11};
  Game guest{1001, "guest", 12};
  qint64 now = 1'000'000;
  host.Hello(now);
  guest.Hello(now);
  Pump(service, {&host, &guest}, now);
  bbcoop::Envelope load;
  load.mutable_load_progress()->set_stage(bbcoop::LoadProgress::JOINING);
  load.mutable_load_progress()->set_percent(70);
  load.mutable_load_progress()->set_member("spoofed");
  guest.Send(load, now);
  Pump(service, {&host, &guest}, now);
  CHECK(host.loads.contains("guest"));
  CHECK(host.loads.at("guest").percent() == 70);
  CHECK(host.loads.at("guest").stage() == bbcoop::LoadProgress::JOINING);
  CHECK(!host.loads.contains("spoofed"));
  CHECK(guest.loads.empty());
  Game late{1002, "late", 13};
  late.Hello(now + 10);
  Pump(service, {&host, &guest, &late}, now + 10);
  CHECK(late.loads.contains("guest") && late.loads.at("guest").percent() == 70);
  return 0;
}

// Phase 8 (one HP path): a game's own hit goes to every other member at once,
// under its name, and never into the log.
int AHitReachesTheOthersAtOnce() {
  Service service{{}};
  Game host{1000, "host", 11};
  Game guest{1001, "guest", 12};
  Game other{1002, "other", 13};
  const std::vector<Game *> games{&host, &guest, &other};
  qint64 now = 1'000'000;
  for (Game *game : games) {
    game->Hello(now);
  }
  Pump(service, games, now);
  bbcoop::Envelope hit;
  hit.mutable_enemy_hit()->set_map(0x18010000);
  hit.mutable_enemy_hit()->set_handle(0x1001400b);
  hit.mutable_enemy_hit()->set_hp(300);
  hit.mutable_enemy_hit()->set_max(484);
  hit.mutable_enemy_hit()->set_member("spoofed");
  guest.Send(hit, now + 10);
  Pump(service, games, now + 10);
  for (Game *game : {&host, &other}) {
    CHECK(game->hits.size() == 1);
    CHECK(game->hits[0].member() == "guest");
    CHECK(game->hits[0].handle() == 0x1001400bu && game->hits[0].hp() == 300);
  }
  CHECK(guest.hits.empty());
  CHECK(host.member.Hp().empty());
  return 0;
}

// Kaniel's playtest 2026-10-24 (team altars): a ritual's altar record goes to
// every other member whole and as it is, under its sender's name.
int AnAltarRecordReachesTheOthersWhole() {
  Service service{{}};
  Game host{1000, "host", 11};
  Game guest{1001, "guest", 12};
  Game other{1002, "other", 13};
  const std::vector<Game *> games{&host, &guest, &other};
  qint64 now = 1'000'000;
  for (Game *game : games) {
    game->Hello(now);
  }
  Pump(service, games, now);
  std::string bits(750, '\0');
  bits[0] = '\x1d';
  bits[1] = '\x0a';
  bits[2] = '\x5a';
  bits[749] = '\x01';
  bbcoop::Envelope record;
  record.mutable_altar_record()->set_altar(6);
  record.mutable_altar_record()->set_bits(bits);
  record.mutable_altar_record()->set_member("spoofed");
  guest.Send(record, now + 10);
  Pump(service, games, now + 10);
  for (Game *game : {&host, &other}) {
    CHECK(game->altar_records.size() == 1);
    CHECK(game->altar_records[0].member() == "guest");
    CHECK(game->altar_records[0].altar() == 6);
    CHECK(game->altar_records[0].bits() == bits);
  }
  CHECK(guest.altar_records.empty());
  return 0;
}

// Kaniel 2026-10-30: a chalice taken off an altar goes to the others as an
// empty record, and the chalice dungeons' log starts over: the same chalice
// placed again built the dungeon with the old kills standing.
int AChaliceTakenOffStartsTheDungeonsOver() {
  Service service{{}};
  Game host{1000, "host", 11};
  Game guest{1001, "guest", 12};
  const std::vector<Game *> games{&host, &guest};
  qint64 now = 1'000'000;
  for (Game *game : games) {
    game->Hello(now);
  }
  Pump(service, games, now);
  constexpr u32 Chalice = 0x1d000000;
  now += 100;
  guest.Propose({{Chalice, {0x10014066}}, {0x18000000, {0x10014067}}}, now);
  Pump(service, games, now);
  CHECK(Killed(host, 0x10014066));
  // A ritual's or a join's record changes nothing of the log.
  std::string bits(750, '\xff');
  bits[0] = '\x1d';
  bits[1] = '\x0a';
  bits[2] = '\x5a';
  bits[3] = '\x00';
  bbcoop::Envelope ritual;
  ritual.mutable_altar_record()->set_altar(5);
  ritual.mutable_altar_record()->set_bits(bits);
  now += 100;
  host.Send(ritual, now);
  Pump(service, games, now);
  CHECK(Killed(host, 0x10014066));
  bbcoop::Envelope removal;
  removal.mutable_altar_record()->set_altar(5);
  removal.mutable_altar_record()->set_bits(std::string(750, '\xff'));
  now += 100;
  host.Send(removal, now);
  Pump(service, games, now);
  CHECK(guest.altar_records.size() == 2);
  CHECK(guest.altar_records.back().bits() == std::string(750, '\xff'));
  for (Game *game : games) {
    CHECK(!Killed(*game, 0x10014066));
    CHECK(Killed(*game, 0x10014067)); // another map's stand
  }
  // The guest's proposal that still holds the old kill does not stand it again.
  now += 100;
  guest.Propose({{Chalice, {0x10014066}}, {0x18000000, {0x10014067}}}, now);
  Pump(service, games, now);
  CHECK(!Killed(host, 0x10014066));
  return 0;
}

// Kaniel 2026-10-25 (travel together): a member's team trip goes to the
// party's other members at once, under the sender's name; not to another
// party, and nothing is kept.
int ATeamTripReachesThePartyAtOnce() {
  Service service{{}};
  Game host{1000, "host", 11};
  Game guest{1001, "guest", 12};
  Game stranger{1003, "stranger", 14};
  const std::vector<Game *> games{&host, &guest, &stranger};
  qint64 now = 1'000'000;
  host.Hello(now);
  guest.Hello(now);
  stranger.Hello(now, "someone:else");
  Pump(service, games, now);
  bbcoop::Envelope ask;
  ask.mutable_team_travel()->set_step(bbcoop::TeamTravel::ASK);
  ask.mutable_team_travel()->set_id(4);
  ask.mutable_team_travel()->set_point(2102962);
  ask.mutable_team_travel()->set_member("spoofed");
  host.Send(ask, now + 10);
  Pump(service, games, now + 10);
  CHECK(guest.travels.size() == 1);
  CHECK(guest.travels[0].member() == "host");
  CHECK(guest.travels[0].step() == bbcoop::TeamTravel::ASK);
  CHECK(guest.travels[0].id() == 4u && guest.travels[0].point() == 2102962);
  CHECK(host.travels.empty() && stranger.travels.empty());
  bbcoop::Envelope answer;
  answer.mutable_team_travel()->set_step(bbcoop::TeamTravel::ANSWER);
  answer.mutable_team_travel()->set_id(4);
  answer.mutable_team_travel()->set_asker("host");
  answer.mutable_team_travel()->set_yes(false);
  guest.Send(answer, now + 20);
  Pump(service, games, now + 20);
  CHECK(host.travels.size() == 1);
  CHECK(host.travels[0].member() == "guest" && !host.travels[0].yes());
  CHECK(host.travels[0].asker() == "host");
  Game late{1004, "late", 15};
  late.Hello(now + 30);
  Pump(service, {&host, &guest, &late}, now + 30);
  CHECK(late.travels.empty());
  return 0;
}

// Kaniel 2026-10-24: a team ping goes to every other member of the party at
// once, under the sender's name (never the name it claims), wherever they are;
// not back to its sender and not to another party. The server keeps nothing.
int APingReachesTheTeamAtOnce() {
  Service service{{}};
  Game host{1000, "host", 11};
  Game guest{1001, "guest", 12};
  Game other{1002, "other", 13};
  Game stranger{1003, "stranger", 14};
  const std::vector<Game *> games{&host, &guest, &other, &stranger};
  qint64 now = 1'000'000;
  host.Hello(now);
  guest.Hello(now);
  other.Hello(now);
  stranger.Hello(now, "someone:else");
  Pump(service, games, now);
  other.Map(0x17000000, now); // another map: still in the team
  Pump(service, games, now);
  bbcoop::Envelope ping;
  ping.mutable_ping()->set_map(0x18010000);
  ping.mutable_ping()->mutable_at()->set_x(-215.5f);
  ping.mutable_ping()->mutable_at()->set_y(-54.25f);
  ping.mutable_ping()->mutable_at()->set_z(178.0f);
  ping.mutable_ping()->set_enemy(0x1001400b);
  ping.mutable_ping()->set_seq(3);
  ping.mutable_ping()->set_member("spoofed");
  guest.Send(ping, now + 10);
  Pump(service, games, now + 10);
  for (Game *game : {&host, &other}) {
    CHECK(game->pings.size() == 1);
    CHECK(game->pings[0].member() == "guest");
    CHECK(game->pings[0].map() == 0x18010000u && game->pings[0].seq() == 3u);
    CHECK(game->pings[0].at().x() == -215.5f &&
          game->pings[0].at().z() == 178.0f);
    CHECK(game->pings[0].enemy() == 0x1001400bu);
  }
  CHECK(guest.pings.empty());
  CHECK(stranger.pings.empty());
  // A member joining later hears no old ping: pings are not kept.
  Game late{1004, "late", 15};
  late.Hello(now + 20);
  Pump(service, {&host, &guest, &other, &late}, now + 20);
  CHECK(late.pings.empty());
  return 0;
}

// Kaniel 2026-10-25: team duels are the server's. A proposal asks the members
// on the proposer's map; every member hears every change of the duel's state;
// a second proposal is refused to its sender only; a defeat ends it with the
// winner; a duelist's game going ends the next one; a late game hears the last.
int ADuelIsTheServers() {
  Service service{{}};
  Game host{1000, "host", 11};
  Game guest{1001, "guest", 12};
  Game other{1002, "other", 13};
  const std::vector<Game *> games{&host, &guest, &other};
  qint64 now = 1'000'000;
  for (Game *game : games) {
    game->Hello(now);
  }
  Pump(service, games, now);
  host.Map(0x18010000, now);
  guest.Map(0x18010000, now);
  other.Map(0x17000000, now);
  Pump(service, games, now);

  host.Duel(bbcoop::DuelRequest::PROPOSE, 0, now + 10);
  Pump(service, games, now + 10);
  for (Game *game : games) {
    CHECK(game->duels.size() == 1);
    CHECK(game->duels.back().duel() == 1u);
    CHECK(game->duels.back().stage() == bbcoop::DuelState::ASKING);
    CHECK(game->duels.back().proposer() == "host");
    CHECK(game->duels.back().duelists_size() == 2); // not "other"
  }
  guest.Duel(bbcoop::DuelRequest::PROPOSE, 0, now + 20);
  Pump(service, games, now + 20);
  CHECK(guest.duels.size() == 2 && !guest.duels.back().refused().empty());
  CHECK(host.duels.size() == 1);

  guest.Duel(bbcoop::DuelRequest::ACCEPT, 1, now + 30);
  Pump(service, games, now + 30);
  CHECK(host.duels.back().stage() == bbcoop::DuelState::COUNTDOWN);
  Pump(service, games, now + 30 + DuelCountdownMs);
  for (Game *game : games) {
    CHECK(game->duels.back().stage() == bbcoop::DuelState::FIGHTING);
  }

  guest.Duel(bbcoop::DuelRequest::DEFEATED, 1, now + 6'000);
  Pump(service, games, now + 6'000);
  for (Game *game : games) {
    CHECK(game->duels.back().stage() == bbcoop::DuelState::OVER);
    CHECK(game->duels.back().end() == bbcoop::DuelState::WON);
    CHECK(game->duels.back().winner() == "host");
  }

  // The next one ends when a duelist's game goes.
  guest.Duel(bbcoop::DuelRequest::PROPOSE, 0, now + 7'000);
  Pump(service, games, now + 7'000);
  host.Duel(bbcoop::DuelRequest::ACCEPT, 2, now + 7'000);
  Pump(service, games, now + 7'000);
  CHECK(host.duels.back().duel() == 2u);
  CHECK(host.duels.back().stage() == bbcoop::DuelState::COUNTDOWN);
  service.Forget(guest.endpoint);
  Pump(service, {&host, &other}, now + 7'500);
  CHECK(host.duels.back().stage() == bbcoop::DuelState::OVER);
  CHECK(host.duels.back().end() == bbcoop::DuelState::LEFT);
  CHECK(host.duels.back().ended_by() == "guest");

  Game late{1004, "late", 15};
  late.Hello(now + 8'000);
  Pump(service, {&host, &other, &late}, now + 8'000);
  CHECK(!late.duels.empty() && late.duels.back().duel() == 2u);
  return 0;
}

// Phase 8: no guest runs an enemy on a map that has an owner. A follower's HP
// reports for that map are never the log's.
int AFollowersHpIsNeverTheLogs() {
  Service service{{}};
  Game host{1000, "host", 11};
  Game guest{1001, "guest", 12};
  Game other{1002, "other", 13};
  const std::vector<Game *> games{&host, &guest, &other};
  qint64 now = 1'000'000;
  for (Game *game : games) {
    game->Hello(now);
  }
  Pump(service, games, now);
  host.Map(0x18010000, now);
  Pump(service, games, now);
  guest.Map(0x18010000, now + 100);
  other.Map(0x18010000, now + 100);
  Pump(service, games, now + 100);
  CHECK(guest.OwnerOf(0x18010000) == "host");
  guest.Hp(0x2222000000000001, {{0x1001400b, 300, 484}}, now + 300);
  Pump(service, games, now + 300);
  CHECK(!host.member.Hp().contains(0x2222000000000001));
  CHECK(!other.member.Hp().contains(0x2222000000000001));
  host.Hp(0x1111000000000001, {{0x1001400b, 300, 484}}, now + 400);
  Pump(service, games, now + 400);
  CHECK(guest.member.Hp().contains(0x1111000000000001));
  // Nor its new kills there.
  guest.Propose({{0x18010000, {0x1001400b}}}, now + 500);
  Pump(service, games, now + 500);
  CHECK(!Killed(host, 0x1001400b));
  return 0;
}

int TheFirstMemberOnAnAreaRunsItsEnemies() {
  Service service{{}};
  Game host{1000, "host", 11};
  Game guest{1001, "guest", 12};
  Game guest2{1002, "guest2", 13};
  const std::vector<Game *> games{&host, &guest, &guest2};
  qint64 now = 1'000'000;
  for (Game *game : games) {
    game->Hello(now);
  }
  Pump(service, games, now);
  constexpr u32 Yharnam = 0x18010000; // Central Yharnam
  constexpr u32 Ward = 0x18000000; // Cathedral Ward: another block of its area
  constexpr u32 Woods = 0x15010000;

  // The first one there runs the map.
  host.Map(0x18010000, now);
  Pump(service, games, now);
  CHECK(host.OwnerOf(Yharnam) == "host");
  CHECK(guest2.OwnerOf(Yharnam) == "host");
  const u64 first = host.EpochOf(Yharnam);
  CHECK(first != 0);
  guest.Map(Yharnam, now + 100);
  guest2.Map(Ward, now + 100);
  Pump(service, games, now + 100);
  CHECK(guest.OwnerOf(Yharnam) == "host");
  CHECK(guest.EpochOf(Yharnam) == first);
  // Another block of the area has its own: the game wakes a block's enemies
  // only near a player in that world, so the host's copies of the Ward's stand
  // still.
  CHECK(host.OwnerOf(Ward) == "guest2");
  guest2.Map(Woods, now + 150);
  Pump(service, games, now + 150);
  CHECK(host.OwnerOf(Ward).empty());
  CHECK(host.OwnerOf(Woods) == "guest2");

  // Every answer to a member's map carries the owners, changed or not.
  const int heard = guest.owner_messages;
  guest.Map(Yharnam, now + 200);
  Pump(service, games, now + 200);
  CHECK(guest.owner_messages > heard);

  // A load hands the area on at once, to the member there longest.
  host.Map(0, now + 300);
  Pump(service, games, now + 300);
  CHECK(guest2.OwnerOf(Yharnam) == "guest");
  const u64 second = guest2.EpochOf(Yharnam);
  CHECK(second > first);
  guest2.Map(Yharnam, now + 400);
  Pump(service, games, now + 400);
  CHECK(host.OwnerOf(Yharnam) == "guest");
  CHECK(host.OwnerOf(Woods).empty()); // nobody is there any more
  // The session's host takes back the area it comes to: its game is the one the
  // others' are built to follow. Not at once (Kaniel's play test 2026-10-18: a
  // guest's hurt enemy went back to its spawn as the host came, the host's new
  // copy stood there): it follows the owner's game for HostTakeBackMs first, so
  // its copies stand where that game has them.
  const auto all_there = [&](qint64 at) {
    host.Map(0x18010000, at);
    guest.Map(Yharnam, at);
    guest2.Map(0x18010000, at);
    Pump(service, games, at);
  };
  all_there(now + 1300);
  CHECK(guest.OwnerOf(Yharnam) == "guest");
  all_there(now + 2300);
  all_there(now + 3300);
  CHECK(guest.OwnerOf(Yharnam) == "guest"); // 2 s: not yet
  CHECK(guest.EpochOf(Yharnam) == second);
  all_there(now + 4300);
  CHECK(guest.OwnerOf(Yharnam) == "host");
  CHECK(guest.EpochOf(Yharnam) > second);
  // A host that steps over a border and back (its map flips) starts again.
  host.Map(Ward, now + 4400);
  Pump(service, games, now + 4400);
  CHECK(guest.OwnerOf(Yharnam) == "guest");
  all_there(now + 4500);
  CHECK(guest.OwnerOf(Yharnam) == "guest");
  all_there(now + 7500);
  CHECK(guest.OwnerOf(Yharnam) == "host");

  // An owner not heard of for a while (its game died) hands it on.
  host.Map(0, now + 7600);
  Pump(service, games, now + 7600);
  CHECK(guest2.OwnerOf(Yharnam) == "guest");
  guest2.Map(0x18010000, now + 8500);
  Pump(service, games, now + 8500);
  guest2.Map(0x18010000, now + 9500);
  Pump(service, games, now + 9500);
  CHECK(guest2.OwnerOf(Yharnam) == "guest"); // 2 s: not yet
  guest2.Map(0x18010000, now + 10500);
  Pump(service, games, now + 10500);
  CHECK(guest2.OwnerOf(Yharnam) == "guest2");

  // An area nobody is on has no owner.
  guest2.Map(0, now + 10600);
  Pump(service, games, now + 10600);
  CHECK(host.OwnerOf(Yharnam).empty());
  return 0;
}

int OnlyTheOwnersKillsAndHpCount() {
  Service service{{}};
  Game host{1000, "host", 11};
  Game guest{1001, "guest", 12};
  Game old{1002, "old", 13};
  old.enemy_owner = false; // a game without phase 4's owners: counts as before
  const std::vector<Game *> games{&host, &guest, &old};
  qint64 now = 1'000'000;
  for (Game *game : games) {
    game->Hello(now);
  }
  Pump(service, games, now);
  host.Map(0x18010000, now);
  Pump(service, games, now);
  guest.Map(0x18010000, now + 100);
  old.Map(0x18010000, now + 100);
  Pump(service, games, now + 100);
  CHECK(guest.OwnerOf(0x18010000) == "host");

  // A kill on an area another member runs is not the guest's to make.
  now += 200;
  guest.Propose({{0x18010000, {0x10014066}}}, now);
  Pump(service, games, now);
  CHECK(!Killed(host, 0x10014066));
  host.Propose({{0x18010000, {0x10014066}}}, now + 10);
  Pump(service, games, now + 10);
  CHECK(Killed(guest, 0x10014066));
  // Its HP report neither.
  guest.Hp(0x2222000000000001, {{0x1001400b, 100, 484}}, now + 20);
  host.Hp(0x1111000000000001, {{0x1001400b, 300, 484}}, now + 20);
  Pump(service, games, now + 20);
  CHECK(!guest.member.Hp().contains(0x2222000000000001));
  CHECK(guest.member.Hp().contains(0x1111000000000001));
  // A game without the owners counts everywhere.
  old.Propose({{0x18010000, {0x1001402e}}}, now + 30);
  Pump(service, games, now + 30);
  CHECK(Killed(host, 0x1001402e));
  // An area nobody runs (nobody is there yet, or the server just started):
  // counted.
  guest.Propose({{0x21000000, {0x10210001}}}, now + 40);
  Pump(service, games, now + 40);
  CHECK(Killed(host, 0x10210001));

  // The owner leaving keeps the kills it made there, but makes no new ones.
  host.Map(0, now + 100);
  Pump(service, games, now + 100);
  CHECK(guest.OwnerOf(0x18010000) == "guest");
  host.Propose({{0x18010000, {0x10014066, 0x10014009}}}, now + 110);
  Pump(service, games, now + 110);
  CHECK(Killed(guest, 0x10014066));
  CHECK(!Killed(guest, 0x10014009));
  guest.Propose({{0x18010000, {0x10014009}}, {0x21000000, {0x10210001}}},
                now + 120);
  Pump(service, games, now + 120);
  CHECK(Killed(host, 0x10014009));
  return 0;
}

int ATradeGoesThroughTheServerAndOutlivesItsRestart() {
  QTemporaryDir folder;
  CHECK(folder.isValid());
  const TradeItem vials{TradeItemKind::Goods, BloodVialGoods, 3};
  const TradeItem echoes{TradeItemKind::Echoes, 0, 100};
  qint64 now = 1'000'000;
  auto service = std::make_unique<Service>(folder.path());
  service->Load(now);
  Game host{1000, "host", 11};
  Game guest{1001, "guest", 12};
  host.Hello(now);
  guest.Hello(now);
  Pump(*service, {&host, &guest}, now);
  CHECK(host.welcome && host.welcome->trades());

  // The host offers three vials for a hundred echoes, then its game goes.
  host.Send(host.trades.Offer("guest", "Guest", "Host", {vials}, {echoes}),
            now);
  Pump(*service, {&host, &guest}, now);
  CHECK(guest.trades.Asked().size() == 1);
  CHECK(guest.trades.Asked().front().from_name() == "Host");
  host.online = false;
  guest.Send(
      guest.trades.Answer(guest.trades.Asked().front().id(), true, {echoes}),
      now + 10);
  Pump(*service, {&host, &guest}, now + 10);
  CHECK(guest.added.size() == 1 && guest.added.front() == TradeItems{vials});

  // The server restarts; the host's part waits on disk for its next game.
  service = std::make_unique<Service>(folder.path());
  service->Load(now + 20);
  Game host_again{1002, "host", 21};
  host_again.run = 0x2002;
  host_again.Hello(now + 20);
  Game guest_again{1003, "guest", 22};
  guest_again.run = 0x2003;
  guest_again.Hello(now + 20);
  Pump(*service, {&host_again, &guest_again}, now + 20);
  CHECK(host_again.added.size() == 1 &&
        host_again.added.front() == TradeItems{echoes});
  CHECK(guest_again.added.empty()); // the guest's grant was taken already

  // An offer nobody answers comes back to the giver.
  guest_again.Send(
      guest_again.trades.Offer("host", "Host", "Guest", {vials}, {}), now + 30);
  Pump(*service, {&host_again, &guest_again}, now + 30);
  CHECK(host_again.trades.Asked().size() == 1);
  Pump(*service, {&host_again, &guest_again}, now + 30 + TradeExpiryMs);
  CHECK(host_again.trades.Asked().empty());
  CHECK(guest_again.added.size() == 1 &&
        guest_again.added.front() == TradeItems{vials});
  return 0;
}

} // namespace

// Phase 5: the lobby on the service. Players sign in before any party, form a
// team, ready up and start. Kaniel 2026-10-17: the team is only a session (a
// new server has none); its world outlives the server.
int ATeamFormsAndStartsThroughTheService() {
  QTemporaryDir dir;
  CHECK(dir.isValid());
  qint64 now = 1'000'000;
  std::string team;
  {
    Service service{dir.path()};
    Game a{1000, "aa", 11};
    Game b{1001, "bb", 12};
    a.LobbyHello(false, now);
    b.LobbyHello(false, now);
    Pump(service, {&a, &b}, now);
    CHECK(a.lobby.players_size() == 1 &&
          a.lobby.players(0).name() == "Player bb");
    a.Team(bbcoop::TeamRequest::CREATE, now);
    Pump(service, {&a, &b}, now);
    CHECK(a.lobby.has_team() && a.lobby.team().leader() == "aa");
    team = a.lobby.team().id();
    a.Team(bbcoop::TeamRequest::INVITE, now, "bb");
    Pump(service, {&a, &b}, now);
    CHECK(b.lobby.invites_size() == 1 && b.lobby.invites(0).team() == team);
    b.Team(bbcoop::TeamRequest::ACCEPT, now, {}, team);
    Pump(service, {&a, &b}, now);
    CHECK(a.lobby.team().members_size() == 2);
    // A refused request comes back with a notice to that player only.
    const int a_states = a.lobby_states;
    b.Team(bbcoop::TeamRequest::START, now);
    Pump(service, {&a, &b}, now);
    CHECK(!b.lobby.notice().empty());
    CHECK(a.lobby_states == a_states);
    a.Team(bbcoop::TeamRequest::READY, now, {}, {}, 3);
    b.Team(bbcoop::TeamRequest::READY, now, {}, {}, 1);
    Pump(service, {&a, &b}, now); // every member ready before the Start
    a.Team(bbcoop::TeamRequest::START, now);
    Pump(service, {&a, &b}, now);
    CHECK(a.starts.size() == 1 && b.starts.size() == 1);
    CHECK(b.starts[0].party() == "world:" + b.starts[0].world() &&
          !b.starts[0].world().empty() && b.starts[0].host() == "aa");
    CHECK(b.starts[0].slot() == 1);
    // The team's party hello plays its world like any party's.
    a.Hello(now, QByteArray::fromStdString(b.starts[0].party()));
    Pump(service, {&a, &b}, now);
    CHECK(a.welcome && a.welcome->accepted());
    // b's game goes away: offline in a's lobby.
    b.online = false;
    service.Forget(b.endpoint);
    now += 2000;
    Pump(service, {&a}, now);
    CHECK(a.lobby.team().members(1).state() == bbcoop::LobbyPlayer::OFFLINE);
  }
  // A new server: no team, but the world is on its list.
  Service service{dir.path()};
  service.Load(now);
  Game b{1001, "bb", 22};
  b.LobbyHello(false, now);
  Pump(service, {&b}, now);
  CHECK(!b.lobby.has_team());
  CHECK(b.lobby.worlds_size() == 1 && b.lobby.worlds(0).played_ms() > 0);
  return 0;
}

// Kaniel 2026-10-17: a team lives while one of its members' games is there: a
// member's crash keeps its place (the grace), every member's game gone ends it.
int ATeamEndsWhenEveryMembersGameIsGone() {
  Service service{{}};
  Game a{1000, "aa", 11};
  Game b{1001, "bb", 12};
  qint64 now = 1'000'000;
  a.LobbyHello(false, now);
  b.LobbyHello(false, now);
  Pump(service, {&a, &b}, now);
  a.Team(bbcoop::TeamRequest::CREATE, now);
  Pump(service, {&a, &b}, now);
  const std::string team = a.lobby.team().id();
  a.Team(bbcoop::TeamRequest::INVITE, now, "bb");
  Pump(service, {&a, &b}, now);
  b.Team(bbcoop::TeamRequest::ACCEPT, now, {}, team);
  Pump(service, {&a, &b}, now);
  // a's game goes: b's team still has it, offline.
  service.Forget(a.endpoint);
  now += 2000;
  Pump(service, {&b}, now);
  CHECK(b.lobby.team().members_size() == 2 &&
        b.lobby.team().members(0).state() == bbcoop::LobbyPlayer::OFFLINE);
  // b's game goes too: back, b is in no team.
  service.Forget(b.endpoint);
  now += 2000;
  Pump(service, {}, now);
  Game b2{1002, "bb", 13};
  b2.LobbyHello(false, now);
  Pump(service, {&b2}, now);
  CHECK(!b2.lobby.has_team());
  return 0;
}

// Phase 5b: a team's world. Its host's game reports it whole; the server keeps
// it on disk and every member gets it after its hello and when it changes; a
// guest's report counts for nothing, and an ordinary party has none.
int ATeamsWorldComesFromItsHostAndOutlivesTheServer() {
  QTemporaryDir dir;
  CHECK(dir.isValid());
  const QByteArray team = "world:00ff00ff00ff00ff";
  qint64 now = 1'000'000;
  const auto report = [](Game &game, std::vector<s32> on, qint64 at) {
    bbcoop::Envelope envelope;
    for (const s32 flag : on) {
      envelope.mutable_team_world()->add_on(flag);
    }
    envelope.mutable_team_world()->add_blocks(1);
    game.Send(envelope, at);
  };
  {
    Service service{dir.path()};
    Game host{1000, "host", 11};
    Game guest{1001, "guest", 12};
    host.Hello(now, team);
    guest.Hello(now, team);
    Pump(service, {&host, &guest}, now);
    // No world yet: the members hear so.
    CHECK(guest.team_world && !guest.team_world->exists());
    CHECK(host.team_world && !host.team_world->exists());
    // A guest's report is no team world.
    report(guest, {7}, now);
    Pump(service, {&host, &guest}, now);
    CHECK(!host.team_world->exists());
    // The host's is: every member gets it.
    now += 100;
    report(host, {1000, 1001, 12401800}, now);
    Pump(service, {&host, &guest}, now);
    CHECK(guest.team_world && guest.team_world->exists());
    CHECK(guest.team_world->on_size() == 3 &&
          guest.team_world->on(2) == 12401800);
    CHECK(guest.team_world->version() == 1);
    // The same again changes nothing (no new version).
    const int heard = guest.team_worlds;
    report(host, {1000, 1001, 12401800}, now);
    Pump(service, {&host, &guest}, now);
    CHECK(guest.team_worlds == heard);
    // An ordinary party has no team world.
    Game other{1002, "other", 13};
    other.Hello(now);
    Pump(service, {&other}, now);
    CHECK(!other.team_world);
  }
  // A new server: the world is on disk.
  Service service{dir.path()};
  service.Load(now);
  Game guest{1001, "guest", 22};
  guest.Hello(now, team);
  Pump(service, {&guest}, now);
  CHECK(guest.team_world && guest.team_world->exists());
  CHECK(guest.team_world->on_size() == 3 && guest.team_world->version() == 1);
  return 0;
}

// Kaniel 2026-10-25: a team's world has backups as a party's world.pb does
// (the broken chalice altar of 2026-10-24 went into the team's world): the
// last writes as team_world.pb.1 to .3, a copy a day for a week, and a
// team_world.pb that does not read gives way to its newest backup.
int ATeamsWorldKeepsBackups() {
  QTemporaryDir dir;
  CHECK(dir.isValid());
  const QByteArray team = "world:00ff00ff00ff00ff";
  constexpr qint64 Day = 24LL * 60 * 60 * 1000;
  qint64 now = QDateTime{QDate{2026, 10, 1}, QTime{12, 0}}.toMSecsSinceEpoch();
  const auto report = [](Game &game, s32 flag, qint64 at) {
    bbcoop::Envelope envelope;
    envelope.mutable_team_world()->add_on(flag);
    envelope.mutable_team_world()->add_blocks(1);
    game.Send(envelope, at);
  };
  QString world_dir;
  {
    Service service{dir.path()};
    Game host{1000, "host", 11};
    host.Hello(now, team);
    Pump(service, {&host}, now);
    for (int day = 0; day < 9; ++day) {
      for (int write = 0; write < 2; ++write) {
        now += 1000;
        report(host, 1000 + 10 * day + write, now);
        Pump(service, {&host}, now);
      }
      now += Day;
    }
  }
  // The server gone: its writer thread has written everything.
  for (const QString &name :
       QDir{dir.path()}.entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
    if (QFile::exists(QDir{dir.filePath(name)}.filePath("team_world.pb"))) {
      world_dir = dir.filePath(name);
    }
  }
  CHECK(!world_dir.isEmpty());
  const QDir world{world_dir};
  CHECK(world.exists("team_world.pb.1") && world.exists("team_world.pb.3") &&
        !world.exists("team_world.pb.4"));
  const QStringList days = QDir{world.filePath("backups")}.entryList(
      {"team_world-*.pb"}, QDir::Files, QDir::Name);
  CHECK(days.size() == 7);
  CHECK(days.first() == "team_world-20261003.pb" &&
        days.last() == "team_world-20261009.pb");
  // Each day's copy is that day's first write.
  QFile copy{QDir{world.filePath("backups")}.filePath(days.last())};
  CHECK(copy.open(QIODevice::ReadOnly));
  const QByteArray bytes = copy.readAll();
  bbcoop::TeamWorld read;
  CHECK(read.ParseFromArray(bytes.constData(), static_cast<int>(bytes.size())));
  CHECK(read.on_size() == 1 && read.on(0) == 1080);

  // team_world.pb cut short: the newest backup is the world (one write back).
  {
    QFile broken{world.filePath("team_world.pb")};
    CHECK(broken.open(QIODevice::WriteOnly | QIODevice::Truncate));
  }
  Service service{dir.path()};
  service.Load(now);
  Game guest{1001, "guest", 22};
  guest.Hello(now, team);
  Pump(service, {&guest}, now);
  CHECK(guest.team_world && guest.team_world->exists());
  CHECK(guest.team_world->on_size() == 1 && guest.team_world->on(0) == 1080);
  return 0;
}

// Kaniel 2026-10-16: a new character got the team's old world. Each world is
// kept apart; the server's list says whose world each one is (its host's
// reports).
int ATeamsWorldsAreKeptApart() {
  QTemporaryDir dir;
  CHECK(dir.isValid());
  qint64 now = 1'000'000;
  std::string team;
  {
    Service service{dir.path()};
    Game a{1000, "aa", 11};
    Game b{1001, "bb", 12};
    a.claims_host = true; // the leader hosts
    a.LobbyHello(false, now);
    b.LobbyHello(false, now);
    Pump(service, {&a, &b}, now);
    a.Team(bbcoop::TeamRequest::CREATE, now);
    Pump(service, {&a, &b}, now);
    team = a.lobby.team().id();
    a.Team(bbcoop::TeamRequest::INVITE, now, "bb");
    Pump(service, {&a, &b}, now);
    b.Team(bbcoop::TeamRequest::ACCEPT, now, {}, team);
    Pump(service, {&a, &b}, now);
    const auto play = [&] {
      a.Team(bbcoop::TeamRequest::READY, now, {}, {}, 3);
      b.Team(bbcoop::TeamRequest::READY, now, {}, {}, 1);
      Pump(service, {&a, &b}, now);
      a.Team(bbcoop::TeamRequest::START, now);
      Pump(service, {&a, &b}, now);
      const bbcoop::TeamStart go = a.starts.back();
      a.Hello(now, QByteArray::fromStdString(go.party()));
      b.Hello(now, QByteArray::fromStdString(go.party()));
      a.LobbyHello(true, now);
      Pump(service, {&a, &b}, now);
      return go;
    };
    const auto stop = [&] {
      a.LobbyHello(false, now);
      b.LobbyHello(false, now);
      Pump(service, {&a, &b}, now);
    };
    const bbcoop::TeamStart first = play();
    CHECK(b.team_world && !b.team_world->exists());
    // The host's report: the team's world, and its line in the team's list.
    bbcoop::Envelope envelope;
    envelope.mutable_team_world()->add_on(1000);
    envelope.mutable_team_world()->add_on(12401800);
    envelope.mutable_team_world()->add_blocks(1);
    envelope.mutable_team_world()->set_character("Kaniel");
    now += 100;
    a.Send(envelope, now);
    Pump(service, {&a, &b}, now);
    CHECK(b.team_world && b.team_world->exists());
    CHECK(b.lobby.worlds_size() == 1);
    CHECK(b.lobby.worlds(0).id() == first.world());
    CHECK(b.lobby.worlds(0).character() == "Kaniel");
    CHECK(b.lobby.worlds(0).flags_on() == 2);
    CHECK(b.lobby.worlds(0).played_ms() > 0);
    // Kaniel 2026-10-17: when it was made, and who played in it.
    CHECK(b.lobby.worlds(0).created_ms() > 0 &&
          b.lobby.worlds(0).created_ms() <= b.lobby.worlds(0).played_ms());
    // (its game's hello names "Player aa", its report "Kaniel").
    CHECK(b.lobby.worlds(0).characters_size() == 2 &&
          b.lobby.worlds(0).characters(0) == "Player aa" &&
          b.lobby.worlds(0).characters(1) == "Kaniel");
    stop();
    // The leader picks a new world: none of the first one's in it.
    bbcoop::Envelope pick;
    pick.mutable_team_request()->set_action(bbcoop::TeamRequest::WORLD);
    pick.mutable_team_request()->set_world("");
    a.Send(pick, now);
    Pump(service, {&a, &b}, now);
    const bbcoop::TeamStart second = play();
    CHECK(second.world() != first.world() && second.party() != first.party());
    CHECK(b.team_world && !b.team_world->exists());
    CHECK(b.lobby.worlds_size() == 2);
    stop();
    // And back to the first: its world again.
    pick.mutable_team_request()->set_world(first.world());
    a.Send(pick, now);
    Pump(service, {&a, &b}, now);
    const bbcoop::TeamStart third = play();
    CHECK(third.party() == first.party());
    CHECK(b.team_world && b.team_world->exists() &&
          b.team_world->on_size() == 2);
  }
  return 0;
}

namespace {

void WriteMessage(const QString &path, const google::protobuf::MessageLite &m) {
  QDir{}.mkpath(QFileInfo{path}.path());
  QFile file{path};
  file.open(QIODevice::WriteOnly);
  const std::string bytes = m.SerializeAsString();
  file.write(bytes.data(), static_cast<qint64>(bytes.size()));
}

QString FolderOfKey(const QString &root, const std::string &key) {
  return QDir{root}.filePath(
      BloodborneWorld::PartyFolderName(QByteArray::fromStdString(key)));
}

} // namespace

// Kaniel 2026-10-17: the worlds kept under their teams' keys before the list
// ("team:<team>/<world>", "team:<team>" for the first) are on it after the
// update, with their ids and lines from the old teams file, and play from
// their folders; the teams file is not read again.
int TheWorldsOfTheOldTeamsAreOnTheList() {
  QTemporaryDir dir;
  CHECK(dir.isValid());
  bbcoop::TeamWorld stored;
  stored.add_on(1000);
  stored.add_on(12107810); // a lamp
  stored.add_blocks(1);
  stored.set_exists(true);
  stored.set_version(4);
  WriteMessage(
      QDir{FolderOfKey(dir.path(), "team:t9/w1")}.filePath("team_world.pb"),
      stored);
  stored.clear_on();
  stored.add_on(1000);
  WriteMessage(
      QDir{FolderOfKey(dir.path(), "team:t9")}.filePath("team_world.pb"),
      stored);
  bbcoop::OldStoredTeams teams;
  bbcoop::OldTeam &team = *teams.add_teams();
  team.set_id("t9");
  bbcoop::TeamWorldInfo &w1 = *team.add_worlds();
  w1.set_id("w1");
  w1.set_character("Ada");
  w1.set_played_ms(5000);
  WriteMessage(QDir{dir.path()}.filePath("teams.pb"), teams);
  qint64 now = 1'000'000;
  {
    Service service{dir.path()};
    service.Load(now);
    Game b{1001, "bb", 22};
    b.LobbyHello(false, now);
    Pump(service, {&b}, now);
    CHECK(b.lobby.worlds_size() == 2);
    const bbcoop::TeamWorldInfo *ada = nullptr;
    const bbcoop::TeamWorldInfo *first = nullptr;
    for (const bbcoop::TeamWorldInfo &world : b.lobby.worlds()) {
      (world.id() == "w1" ? ada : first) = &world;
    }
    CHECK(ada && ada->character() == "Ada" && ada->played_ms() == 5000);
    CHECK(ada->flags_on() == 2 && ada->lamps() == 1);
    // The earliest time known of it, and its host among the characters.
    CHECK(ada->created_ms() == 5000);
    CHECK(ada->characters_size() == 1 && ada->characters(0) == "Ada");
    CHECK(first && first->id() != "0" && !first->id().empty());
    CHECK(first->flags_on() == 1 && first->played_ms() > 0);
    CHECK(first->created_ms() > 0 && first->created_ms() <= first->played_ms());
    CHECK(!QFile::exists(QDir{dir.path()}.filePath("teams.pb")));
    CHECK(QFile::exists(QDir{dir.path()}.filePath("teams.pb.old")));
    // Its party plays the world in its old folder.
    Game a{1000, "aa", 11};
    a.Hello(now, "world:w1");
    Pump(service, {&a}, now);
    CHECK(a.team_world && a.team_world->exists() &&
          a.team_world->version() == 4);
  }
  // Again: the same list, from worlds.pb.
  Service service{dir.path()};
  service.Load(now);
  Game b{1001, "bb", 23};
  b.LobbyHello(false, now);
  Pump(service, {&b}, now);
  CHECK(b.lobby.worlds_size() == 2);
  Game a{1000, "aa", 12};
  a.Hello(now, "world:w1");
  Pump(service, {&a}, now);
  CHECK(a.team_world && a.team_world->exists() && a.team_world->version() == 4);
  return 0;
}

// Kaniel 2026-10-17: a leader forgets a world in the picker: off the list, its
// folder put aside on the server (nothing deleted).
int AForgottenWorldsFolderIsPutAside() {
  QTemporaryDir dir;
  CHECK(dir.isValid());
  qint64 now = 1'000'000;
  Service service{dir.path()};
  service.Load(now);
  Game a{1000, "aa", 11};
  a.claims_host = true;
  a.LobbyHello(false, now);
  Pump(service, {&a}, now);
  a.Team(bbcoop::TeamRequest::CREATE, now);
  a.Team(bbcoop::TeamRequest::READY, now, {}, {}, 1);
  a.Team(bbcoop::TeamRequest::START, now);
  Pump(service, {&a}, now);
  const bbcoop::TeamStart go = a.starts.at(0);
  a.Hello(now, QByteArray::fromStdString(go.party()));
  a.LobbyHello(true, now);
  Pump(service, {&a}, now);
  const QString folder = FolderOfKey(dir.path(), go.party());
  service.Drain();
  CHECK(QDir{folder}.exists());
  bbcoop::Envelope forget;
  forget.mutable_team_request()->set_action(bbcoop::TeamRequest::FORGET_WORLD);
  forget.mutable_team_request()->set_world(go.world());
  a.Send(forget, now);
  Pump(service, {&a}, now);
  CHECK(!a.lobby.notice().empty()); // played now
  // Its game leaves the world (back at the title, out of the party).
  service.Forget(a.endpoint);
  Game a2{1002, "aa", 12};
  a2.LobbyHello(false, now);
  Pump(service, {&a2}, now);
  a2.Team(bbcoop::TeamRequest::CREATE, now);
  Pump(service, {&a2}, now);
  a2.Send(forget, now);
  Pump(service, {&a2}, now);
  service.Drain(); // the folder goes aside on the writer's thread
  CHECK(a2.lobby.notice().empty());
  CHECK(a2.lobby.worlds_size() == 0);
  CHECK(!QDir{folder}.exists());
  const QStringList aside =
      QDir{QDir{dir.path()}.filePath("forgotten")}.entryList(
          QDir::Dirs | QDir::NoDotAndDotDot);
  CHECK(aside.size() == 1 &&
        aside[0].startsWith(QFileInfo{folder}.fileName() + "-"));
  // Not on the list after a restart either.
  Service again{dir.path()};
  again.Load(now);
  Game a3{1003, "aa", 13};
  a3.LobbyHello(false, now);
  Pump(again, {&a3}, now);
  CHECK(a3.lobby.worlds_size() == 0);
  return 0;
}

// Provi 2026-10-29: a leader resets a world's lamps in the picker: its team
// world loses every lamp's flags (a version up), the rest stays, and the one
// before is kept among the backups.
int AWorldsLampsAreReset() {
  QTemporaryDir dir;
  CHECK(dir.isValid());
  qint64 now = 1'000'000;
  Service service{dir.path()};
  service.Load(now);
  Game a{1000, "aa", 11};
  a.claims_host = true;
  a.LobbyHello(false, now);
  Pump(service, {&a}, now);
  a.Team(bbcoop::TeamRequest::CREATE, now);
  a.Team(bbcoop::TeamRequest::READY, now, {}, {}, 1);
  a.Team(bbcoop::TeamRequest::START, now);
  Pump(service, {&a}, now);
  const bbcoop::TeamStart go = a.starts.at(0);
  a.Hello(now, QByteArray::fromStdString(go.party()));
  a.LobbyHello(true, now);
  Pump(service, {&a}, now);
  bbcoop::Envelope report;
  for (const s32 flag : {1000, 12117810, 12117811, 12401800, 72110200}) {
    report.mutable_team_world()->add_on(flag);
  }
  report.mutable_team_world()->add_blocks(1);
  now += 100;
  a.Send(report, now);
  Pump(service, {&a}, now);
  CHECK(a.lobby.worlds_size() == 1 && a.lobby.worlds(0).lamps() == 1);
  bbcoop::Envelope reset;
  reset.mutable_team_request()->set_action(bbcoop::TeamRequest::RESET_LAMPS);
  reset.mutable_team_request()->set_world(go.world());
  a.Send(reset, now);
  Pump(service, {&a}, now);
  CHECK(!a.lobby.notice().empty()); // played now
  service.Forget(a.endpoint);
  Game a2{1002, "aa", 12};
  a2.LobbyHello(false, now);
  Pump(service, {&a2}, now);
  a2.Team(bbcoop::TeamRequest::CREATE, now);
  Pump(service, {&a2}, now);
  a2.Send(reset, now);
  Pump(service, {&a2}, now);
  service.Drain();
  CHECK(a2.lobby.notice().empty());
  CHECK(a2.lobby.worlds_size() == 1 && a2.lobby.worlds(0).lamps() == 0);
  const QString folder = FolderOfKey(dir.path(), go.party());
  QFile file{QDir{folder}.filePath("team_world.pb")};
  CHECK(file.open(QIODevice::ReadOnly));
  const QByteArray bytes = file.readAll();
  bbcoop::TeamWorld stored;
  CHECK(
      stored.ParseFromArray(bytes.constData(), static_cast<int>(bytes.size())));
  CHECK(stored.on_size() == 2 && stored.on(0) == 1000 &&
        stored.on(1) == 12401800);
  CHECK(stored.version() == 2);
  CHECK(QFile::exists(QDir{folder}.filePath("team_world.pb.1")));
  return 0;
}

// Phase 7: the link relays only inside a team, so the service says each link's
// party.
int EachLinksPartyIsKnownAfterItsHello() {
  Service service{{}};
  Game host{1000, "host", 11};
  Game other{1001, "other", 12};
  qint64 now = 1'000'000;
  CHECK(service.PartyOf(host.endpoint).isEmpty());
  host.Hello(now);
  other.Hello(now, "world:t9");
  Pump(service, {&host, &other}, now);
  CHECK(!service.PartyOf(host.endpoint).isEmpty());
  CHECK(!service.PartyOf(other.endpoint).isEmpty());
  CHECK(service.PartyOf(host.endpoint) != service.PartyOf(other.endpoint));
  service.Forget(host.endpoint);
  CHECK(service.PartyOf(host.endpoint).isEmpty());
  return 0;
}

// Phase 7: a guest whose game died comes back where it was. The server keeps
// each member's last place in the world (its vitals: map and position) and
// tells the member's next run where to resume.
int AMembersNextRunResumesWhereItWas() {
  Service service{{}};
  Game host{1000, "host", 11};
  Game guest{1001, "guest", 12};
  qint64 now = 1'000'000;
  host.Hello(now);
  guest.Hello(now);
  Pump(service, {&host, &guest}, now);
  CHECK(!guest.resume_at.has_value()); // nowhere known yet
  bbcoop::Envelope vitals = VitalsOf(0x18010000, 1);
  vitals.mutable_vitals()->mutable_at()->set_x(-120.0f);
  vitals.mutable_vitals()->mutable_at()->set_y(-51.0f);
  vitals.mutable_vitals()->mutable_at()->set_z(100.0f);
  guest.Send(vitals, now + 10);
  Pump(service, {&host, &guest}, now + 10);
  // Down where it stands counts too (rig crashes/resume: the guest was down
  // when it went).
  bbcoop::Envelope down = VitalsOf(0x18010000, 2);
  down.mutable_vitals()->mutable_at()->set_x(-215.5f);
  down.mutable_vitals()->mutable_at()->set_z(172.25f);
  vitals.mutable_vitals()->mutable_at()->set_x(-100.0f);
  guest.Send(down, now + 11);
  Pump(service, {&host, &guest}, now + 11);
  // Its game dies and starts again (a new link, a new run).
  service.Forget(guest.endpoint);
  Game guest_again{1002, "guest", 22};
  guest_again.run = 0x3003;
  guest_again.Hello(now + 20);
  Pump(service, {&host, &guest_again}, now + 20);
  CHECK(guest_again.resume_at.has_value());
  CHECK(guest_again.resume_at->map() == 0x18010000);
  CHECK(guest_again.resume_at->at().x() == -215.5f &&
        guest_again.resume_at->at().z() == 172.25f);
  // The host's own save is its place: it is told nothing.
  CHECK(!host.resume_at.has_value());
  return 0;
}

// Phase 7, host migration: the host's game dies; the room passes to a guest,
// whose game takes the session host's role in place (a new hello saying so)
// and offers the session; the other guest is told who hosts now, and the old
// host's next run joins that session as a guest instead of closing it.
int TheSessionHostMovesToAGuest() {
  Service service{{}};
  Game host{1000, "host", 11};
  Game first{1001, "first", 12};
  Game second{1002, "second", 13};
  qint64 now = 1'000'000;
  for (Game *game : {&host, &first, &second}) {
    game->Hello(now);
  }
  Pump(service, {&host, &first, &second}, now);
  bbcoop::Envelope open;
  open.mutable_team_session()->set_join("NXRV-host");
  open.mutable_team_session()->set_session(1);
  host.Send(open, now);
  Pump(service, {&host, &first, &second}, now);
  CHECK(second.session && second.session->host() == "host");
  // The host's game dies; the first guest takes over.
  host.online = false;
  service.Quiet(host.endpoint, true, now + 10); // its link went quiet
  first.claims_host = true;
  first.Hello(now + 10);
  bbcoop::Envelope moved;
  moved.mutable_team_session()->set_join("NXRV-first");
  moved.mutable_team_session()->set_session(1);
  first.Send(moved, now + 10);
  Pump(service, {&first, &second}, now + 10);
  CHECK(second.session && second.session->host() == "first" &&
        second.session->join() == "NXRV-first");
  // The old host's next run says it hosts (its team's leader): the session
  // stays the first guest's, and it is told to join it.
  Game host_again{1003, "host", 21};
  host_again.run = 0x2002;
  host_again.Hello(now + 20);
  Pump(service, {&host_again, &first, &second}, now + 20);
  CHECK(second.session && second.session->join() == "NXRV-first");
  CHECK(host_again.session && host_again.session->host() == "first");
  // What it offers or closes does not count: it is not the session's host.
  bbcoop::Envelope stray;
  stray.mutable_team_session()->set_join("NXRV-stray");
  host_again.Send(stray, now + 30);
  bbcoop::Envelope closed;
  closed.mutable_team_session()->set_session(5);
  host_again.Send(closed, now + 31);
  Pump(service, {&host_again, &first, &second}, now + 31);
  CHECK(second.session && second.session->join() == "NXRV-first");
  return 0;
}

// Phase 7, host migration: a team's leader whose game died comes back to a
// session another member hosts now: its team start names that host, so its
// game joins as a guest instead of opening a session of its own.
int AReturningLeaderStartsAsAGuestOfTheNewHost() {
  Service service{{}};
  Game a{1000, "aa", 11};
  Game b{1001, "bb", 12};
  qint64 now = 1'000'000;
  a.LobbyHello(false, now);
  b.LobbyHello(false, now);
  Pump(service, {&a, &b}, now);
  a.Team(bbcoop::TeamRequest::CREATE, now);
  Pump(service, {&a, &b}, now);
  const std::string team = a.lobby.team().id();
  a.Team(bbcoop::TeamRequest::INVITE, now, "bb");
  Pump(service, {&a, &b}, now);
  b.Team(bbcoop::TeamRequest::ACCEPT, now, {}, team);
  Pump(service, {&a, &b}, now);
  a.Team(bbcoop::TeamRequest::READY, now, {}, {}, 1);
  b.Team(bbcoop::TeamRequest::READY, now, {}, {}, 2);
  Pump(service, {&a, &b}, now);
  a.Team(bbcoop::TeamRequest::START, now);
  Pump(service, {&a, &b}, now);
  CHECK(a.starts.size() == 1 && a.starts[0].host() == "aa");
  a.LobbyHello(true, now); // both play now
  b.LobbyHello(true, now);
  const QByteArray party = QByteArray::fromStdString(a.starts[0].party());
  // aa hosts the team's session; its game dies and bb takes over.
  a.claims_host = true;
  a.Hello(now, party);
  b.claims_host = false;
  b.Hello(now, party);
  Pump(service, {&a, &b}, now);
  bbcoop::Envelope open;
  open.mutable_team_session()->set_join("NXRV-aa");
  a.Send(open, now);
  Pump(service, {&a, &b}, now);
  a.online = false;
  service.Quiet(a.endpoint, true, now + 10); // its link went quiet
  b.claims_host = true;
  b.Hello(now + 10, party);
  bbcoop::Envelope moved;
  moved.mutable_team_session()->set_join("NXRV-bb");
  b.Send(moved, now + 10);
  Pump(service, {&b}, now + 10);
  // bb hosts the session: it leads (Kaniel 2026-10-25).
  CHECK(b.lobby.team().leader() == "bb");
  // aa's next run: back in the lobby (at the title while bb plays: out of the
  // team, invited back by bb), the invite accepted, ready: in at once.
  Game a_again{1002, "aa", 21};
  a_again.run = 0x2002;
  a_again.LobbyHello(false, now + 20);
  Pump(service, {&a_again, &b}, now + 20);
  CHECK(!a_again.lobby.has_team() && a_again.lobby.invites_size() == 1);
  a_again.Team(bbcoop::TeamRequest::ACCEPT, now + 20, {}, team);
  a_again.Team(bbcoop::TeamRequest::READY, now + 20, {}, {}, 1);
  Pump(service, {&a_again, &b}, now + 20);
  CHECK(!a_again.starts.empty());
  CHECK(a_again.starts.back().host() == "bb");
  return 0;
}

// Kaniel 2026-10-25: a team of three playing its world, aa leading and hosting
// the session; the party key of its world.
QByteArray PlayingTeamOfThree(Service &service, Game &a, Game &b, Game &c,
                              qint64 now) {
  const std::vector<Game *> games{&a, &b, &c};
  for (Game *game : games) {
    game->LobbyHello(false, now);
  }
  Pump(service, games, now);
  a.Team(bbcoop::TeamRequest::CREATE, now);
  Pump(service, games, now);
  const std::string team = a.lobby.team().id();
  for (Game *guest : {&b, &c}) {
    a.Team(bbcoop::TeamRequest::INVITE, now, guest->name);
    Pump(service, games, now);
    guest->Team(bbcoop::TeamRequest::ACCEPT, now, {}, team);
    Pump(service, games, now);
  }
  for (Game *game : games) {
    game->Team(bbcoop::TeamRequest::READY, now, {}, {}, 1);
  }
  Pump(service, games, now);
  a.Team(bbcoop::TeamRequest::START, now);
  Pump(service, games, now);
  const QByteArray party = QByteArray::fromStdString(a.starts.at(0).party());
  for (Game *game : games) {
    game->LobbyHello(true, now);
    game->claims_host = game == &a;
    game->Hello(now, party);
  }
  Pump(service, games, now);
  bbcoop::Envelope open;
  open.mutable_team_session()->set_join("NXRV-aa");
  a.Send(open, now);
  Pump(service, games, now);
  return party;
}

// Two guests' games both open a session when their host leaves (one took the
// room, the other stood alone): the team has one host, the first. The second's
// session is not the team's; it is told the first's to join, and the lead stays
// with the first (rig host-quits-title3 2026-10-25: the lead went back and
// forth).
int OneSessionHostWhenTwoGuestsTakeOver() {
  Service service{{}};
  Game a{1000, "aa", 11};
  Game b{1001, "bb", 12};
  Game c{1002, "cc", 13};
  qint64 now = 1'000'000;
  const QByteArray party = PlayingTeamOfThree(service, a, b, c, now);
  a.LobbyHello(false, now + 10);
  Pump(service, {&a, &b, &c}, now + 10);
  for (Game *guest : {&c, &b}) {
    guest->claims_host = true;
    guest->Hello(now + 20, party);
    bbcoop::Envelope open;
    open.mutable_team_session()->set_join("NXRV-" + guest->name);
    guest->Send(open, now + 20);
    Pump(service, {&a, &b, &c}, now + 20);
  }
  CHECK(b.lobby.team().leader() == "cc" && c.lobby.team().leader() == "cc");
  CHECK(b.session && b.session->host() == "cc" &&
        b.session->join() == "NXRV-cc");
  // The new run of aa joins cc's.
  a.Team(bbcoop::TeamRequest::ACCEPT, now + 30, {}, a.lobby.invites(0).team());
  a.Team(bbcoop::TeamRequest::READY, now + 30, {}, {}, 1);
  Pump(service, {&a, &b, &c}, now + 30);
  CHECK(a.starts.back().host() == "cc");
  return 0;
}

// The leader and host quits to the title while the others play: it is out of
// the team at once, the others hear its session is closed, and the guest whose
// game takes the session (it publishes it as host) leads. The old host has an
// invite back from it; accepted, it starts as a guest of the new host.
int AHostBackAtTheTitleHandsTheTeamToTheNewHost() {
  Service service{{}};
  Game a{1000, "aa", 11};
  Game b{1001, "bb", 12};
  Game c{1002, "cc", 13};
  qint64 now = 1'000'000;
  const QByteArray party = PlayingTeamOfThree(service, a, b, c, now);
  CHECK(c.session && c.session->join() == "NXRV-aa");
  a.LobbyHello(false, now + 10);
  Pump(service, {&a, &b, &c}, now + 10);
  CHECK(!a.lobby.has_team() && a.lobby.invites_size() == 1);
  CHECK(c.session && c.session->join().empty()); // closed with its host
  CHECK(c.lobby.team().members_size() == 2);
  // cc's game took the room: it hosts now, and leads.
  c.claims_host = true;
  c.Hello(now + 20, party);
  bbcoop::Envelope moved;
  moved.mutable_team_session()->set_join("NXRV-cc");
  c.Send(moved, now + 20);
  Pump(service, {&a, &b, &c}, now + 20);
  CHECK(b.session && b.session->join() == "NXRV-cc");
  CHECK(b.lobby.team().leader() == "cc");
  CHECK(a.lobby.invites_size() == 1 && a.lobby.invites(0).from() == "cc");
  a.Team(bbcoop::TeamRequest::ACCEPT, now + 30, {}, a.lobby.invites(0).team());
  a.Team(bbcoop::TeamRequest::READY, now + 30, {}, {}, 1);
  Pump(service, {&a, &b, &c}, now + 30);
  CHECK(a.starts.size() == 2 && a.starts.back().host() == "cc");
  CHECK(a.lobby.team().leader() == "cc");
  for (const bbcoop::TeamMember &m : a.lobby.team().members()) {
    CHECK(m.member() != "cc" || m.sign() == 0);
    CHECK(m.member() != "aa" || m.sign() == 6); // bb keeps its 7
  }
  // Back in the world it joins cc's session as a guest.
  a.claims_host = false;
  a.run = 0x2002;
  a.Hello(now + 40, party);
  Pump(service, {&a, &b, &c}, now + 40);
  CHECK(a.session && a.session->host() == "cc");
  return 0;
}

// The host's game is gone (its link quiet): a few seconds later it is out of
// the team and out of its world (the others hear its session is closed), and
// the guest whose game took the session leads. A game heard again in time
// stays in.
int AHostsGameGoneHandsTheTeamOver() {
  Service service{{}};
  Game a{1000, "aa", 11};
  Game b{1001, "bb", 12};
  Game c{1002, "cc", 13};
  qint64 now = 1'000'000;
  const QByteArray party = PlayingTeamOfThree(service, a, b, c, now);
  service.Quiet(b.endpoint, true, now + 10);
  service.Quiet(b.endpoint, false, now + 20);
  a.online = false;
  service.Quiet(a.endpoint, true, now + 100);
  Pump(service, {&b, &c}, now + 100 + LeftGraceMs - 1);
  CHECK(c.lobby.team().members_size() == 3);
  Pump(service, {&b, &c}, now + 100 + LeftGraceMs);
  CHECK(c.lobby.team().members_size() == 2 && b.lobby.team().started());
  CHECK(c.session && c.session->join().empty());
  b.claims_host = true;
  b.Hello(now + 200 + LeftGraceMs, party);
  bbcoop::Envelope moved;
  moved.mutable_team_session()->set_join("NXRV-bb");
  b.Send(moved, now + 200 + LeftGraceMs);
  Pump(service, {&b, &c}, now + 200 + LeftGraceMs);
  CHECK(c.lobby.team().leader() == "bb");
  CHECK(c.session && c.session->join() == "NXRV-bb");
  // aa's next run: an invite back from bb at the title.
  Game a_again{1003, "aa", 21};
  a_again.run = 0x3003;
  a_again.LobbyHello(false, now + 300 + LeftGraceMs);
  Pump(service, {&a_again, &b, &c}, now + 300 + LeftGraceMs);
  CHECK(!a_again.lobby.has_team() && a_again.lobby.invites_size() == 1 &&
        a_again.lobby.invites(0).from() == "bb");
  return 0;
}

// Phase 7: the resume point is the character's: a new character (another one
// readied in the slot) starts where its own save does.
int TheResumePointIsTheCharactersOwn() {
  Service service{{}};
  Game a{1000, "aa", 11};
  Game b{1001, "bb", 12};
  qint64 now = 1'000'000;
  a.LobbyHello(false, now);
  b.LobbyHello(false, now);
  Pump(service, {&a, &b}, now);
  a.Team(bbcoop::TeamRequest::CREATE, now);
  Pump(service, {&a, &b}, now);
  const std::string team = a.lobby.team().id();
  a.Team(bbcoop::TeamRequest::INVITE, now, "bb");
  Pump(service, {&a, &b}, now);
  b.Team(bbcoop::TeamRequest::ACCEPT, now, {}, team);
  Pump(service, {&a, &b}, now);
  a.Team(bbcoop::TeamRequest::READY, now, {}, {}, 1, "Gehrman");
  b.Team(bbcoop::TeamRequest::READY, now, {}, {}, 2, "Eileen");
  Pump(service, {&a, &b}, now);
  a.Team(bbcoop::TeamRequest::START, now);
  Pump(service, {&a, &b}, now);
  a.LobbyHello(true, now);
  b.LobbyHello(true, now);
  const QByteArray party = QByteArray::fromStdString(b.starts.at(0).party());
  a.Hello(now, party);
  b.Hello(now, party);
  Pump(service, {&a, &b}, now);
  bbcoop::Envelope vitals = VitalsOf(0x18010000, 1);
  vitals.mutable_vitals()->mutable_at()->set_x(-215.5f);
  b.Send(vitals, now + 10);
  Pump(service, {&a, &b}, now + 10);
  // The same character's next run comes back there.
  b.online = false;
  service.Forget(b.endpoint);
  Game b2{1002, "bb", 22};
  b2.run = 0x3003;
  b2.LobbyHello(false, now + 20);
  b2.Team(bbcoop::TeamRequest::READY, now + 20, {}, {}, 2, "Eileen");
  b2.Hello(now + 20, party);
  Pump(service, {&a, &b2}, now + 20);
  CHECK(b2.resume_at.has_value() && b2.resume_at->at().x() == -215.5f);
  // Another character in the slot: nowhere to resume.
  b2.online = false;
  service.Forget(b2.endpoint);
  Game b3{1003, "bb", 23};
  b3.run = 0x4004;
  b3.LobbyHello(false, now + 30);
  b3.Team(bbcoop::TeamRequest::READY, now + 30, {}, {}, 2, "Newcomer");
  b3.Hello(now + 30, party);
  Pump(service, {&a, &b3}, now + 30);
  CHECK(!b3.resume_at.has_value());
  return 0;
}

// Phase 9: the hub asks the service whether a game packet goes from one link to
// another: by the channel table, the members' maps and each map's owner.
int GamePacketsGoByMapAndOwner() {
  Service service{{}};
  Game host{1000, "host", 11};
  Game guest{1001, "guest", 12};
  Game guest2{1002, "guest2", 13};
  const std::vector<Game *> games{&host, &guest, &guest2};
  qint64 now = 1'000'000;
  for (Game *game : games) {
    game->Hello(now);
  }
  Pump(service, games, now);
  constexpr u32 Yharnam = 0x18010000;
  constexpr u32 Ward = 0x18000000;
  const auto routes = [&](const Game &from, const Game &to, u32 channel) {
    return service.RoutesPacket(from.endpoint, to.endpoint, channel);
  };
  // No map said yet: what the game does on its own.
  CHECK(routes(host, guest, 0x06) && routes(host, guest, 0x04));
  CHECK(!routes(host, guest, 0x2C) && !routes(host, guest, 0x27));

  host.Map(Yharnam, now);
  guest.Map(Yharnam, now);
  guest2.Map(Ward, now);
  Pump(service, games, now);
  CHECK(routes(host, guest, 0x06) && routes(guest, host, 0x06));
  CHECK(!routes(host, guest2, 0x06) && !routes(guest2, guest, 0x31));
  CHECK(routes(host, guest2, 0x10)); // the game's own: any map
  // The host runs Central Yharnam's enemies: their HP comes from it only, the
  // guest's hits and near list go to it, and its hand-out (0x17) to the guest.
  // Positions come from whichever game runs an enemy.
  CHECK(routes(host, guest, 0x05) && !routes(guest, host, 0x05));
  CHECK(routes(host, guest, 0x04) && routes(guest, host, 0x04));
  CHECK(routes(guest, host, 0x15) && !routes(host, guest, 0x15));
  CHECK(routes(guest, host, 0x16) && routes(host, guest, 0x17));
  CHECK(!routes(guest, host, 0x17));
  // A load lets everything of the map through again, then the new map counts.
  guest2.Map(0, now + 100);
  Pump(service, games, now + 100);
  CHECK(routes(host, guest2, 0x06));
  guest2.Map(Yharnam, now + 200);
  Pump(service, games, now + 200);
  CHECK(routes(host, guest2, 0x06) && routes(host, guest2, 0x05));
  CHECK(!routes(guest2, guest, 0x05));
  // A link the service does not know (no hello): only the game's own.
  const ServerLink::Endpoint stranger{0x7F000001, 999};
  CHECK(!service.RoutesPacket(stranger, host.endpoint, 0x06));
  CHECK(service.RoutesPacket(stranger, host.endpoint, 0x10));
  return 0;
}

// Kaniel 2026-10-16 (the team menu in game): the leader removes a member while
// both play. The server takes it out of the team's world at once: it hears
// nothing more of it, the others' boards forget it, and no game packet goes
// between it and the team (its game leaves the session; the server's word
// stands even if it did not). Invited again, its hello brings it back in.
// Amanises 2026-10-25: a removed member's goodbye goes through for CutGraceMs
// only; then nothing goes between it and the team's games.
int ARemovedMembersGoodbyeEndsAfterTheGrace() {
  Service service{{}};
  Game a{1000, "aa", 11};
  Game b{1001, "bb", 12};
  const std::vector<Game *> games{&a, &b};
  qint64 now = 1'000'000;
  a.LobbyHello(false, now);
  b.LobbyHello(false, now);
  Pump(service, games, now);
  a.Team(bbcoop::TeamRequest::CREATE, now);
  Pump(service, games, now);
  const std::string team = a.lobby.team().id();
  a.Team(bbcoop::TeamRequest::INVITE, now, "bb");
  Pump(service, games, now);
  b.Team(bbcoop::TeamRequest::ACCEPT, now, {}, team);
  Pump(service, games, now);
  a.Team(bbcoop::TeamRequest::READY, now, {}, {}, 1);
  b.Team(bbcoop::TeamRequest::READY, now, {}, {}, 2);
  Pump(service, games, now);
  a.Team(bbcoop::TeamRequest::START, now);
  Pump(service, games, now);
  a.LobbyHello(true, now);
  b.LobbyHello(true, now);
  const QByteArray party = QByteArray::fromStdString(a.starts.at(0).party());
  a.Hello(now, party);
  b.claims_host = false;
  b.Hello(now, party);
  a.Map(0x18010000, now);
  b.Map(0x18010000, now);
  Pump(service, games, now);
  a.Team(bbcoop::TeamRequest::REMOVE, now + 10, "bb");
  Pump(service, games, now + 10);
  CHECK(!service.PartyOf(b.endpoint).isEmpty());
  // Half way through: still saying goodbye; past the grace: gone.
  Pump(service, games, now + 10 + Service::CutGraceMs / 2);
  CHECK(!service.PartyOf(b.endpoint).isEmpty());
  Pump(service, games, now + 10 + Service::CutGraceMs);
  CHECK(service.PartyOf(b.endpoint).isEmpty());
  for (const u32 channel : {0x06u, 0x10u, 0x07u}) {
    CHECK(!service.RoutesPacket(a.endpoint, b.endpoint, channel) &&
          !service.RoutesPacket(b.endpoint, a.endpoint, channel));
  }
  return 0;
}

int ARemovedMemberIsOutOfTheTeamsWorld() {
  Service service{{}};
  Game a{1000, "aa", 11};
  Game b{1001, "bb", 12};
  const std::vector<Game *> games{&a, &b};
  qint64 now = 1'000'000;
  a.LobbyHello(false, now);
  b.LobbyHello(false, now);
  Pump(service, games, now);
  a.Team(bbcoop::TeamRequest::CREATE, now);
  Pump(service, games, now);
  const std::string team = a.lobby.team().id();
  a.Team(bbcoop::TeamRequest::INVITE, now, "bb");
  Pump(service, games, now);
  b.Team(bbcoop::TeamRequest::ACCEPT, now, {}, team);
  Pump(service, games, now);
  a.Team(bbcoop::TeamRequest::READY, now, {}, {}, 1);
  b.Team(bbcoop::TeamRequest::READY, now, {}, {}, 2);
  Pump(service, games, now);
  a.Team(bbcoop::TeamRequest::START, now);
  Pump(service, games, now);
  a.LobbyHello(true, now);
  b.LobbyHello(true, now);
  const QByteArray party = QByteArray::fromStdString(a.starts.at(0).party());
  a.Hello(now, party);
  b.claims_host = false;
  b.Hello(now, party);
  constexpr u32 Yharnam = 0x18010000;
  a.Map(Yharnam, now);
  b.Map(Yharnam, now);
  Pump(service, games, now);
  const auto routes = [&](const Game &from, const Game &to, u32 channel) {
    return service.RoutesPacket(from.endpoint, to.endpoint, channel);
  };
  CHECK(routes(a, b, 0x06) && routes(b, a, 0x10));
  CHECK(a.map_board.members_size() == 2);

  a.Team(bbcoop::TeamRequest::REMOVE, now + 10, "bb");
  Pump(service, games, now + 10);
  CHECK(!b.lobby.has_team());
  CHECK(a.map_board.members_size() == 1 &&
        a.map_board.members(0).member() == "aa");
  // Amanises 2026-10-25: for a moment its game's own packets still go both ways
  // (its game says goodbye to the session's host), none of the team's world.
  CHECK(service.PartyOf(b.endpoint) == service.PartyOf(a.endpoint));
  for (const u32 channel : {0x06u, 0x10u, 0x07u}) {
    const bool native = BloodborneCoop::NativeChannelPolicy(channel) ==
                            BloodborneCoop::ChannelPolicy::Pass &&
                        channel < BloodborneCoop::NativeChannelCount;
    CHECK(routes(a, b, channel) == native && routes(b, a, channel) == native);
  }
  CHECK(!routes(a, b, 0x10u) || BloodborneCoop::NativeChannelPolicy(0x10u) ==
                                    BloodborneCoop::ChannelPolicy::Pass);
  // What its game still sends of the team's world counts for nothing.
  b.Map(0x18000000, now + 20);
  Pump(service, games, now + 20);
  CHECK(a.map_board.members_size() == 1);

  // Invited again: in the team, its hello brings it back to the team's world.
  a.Team(bbcoop::TeamRequest::INVITE, now + 30, "bb");
  Pump(service, games, now + 30);
  b.Team(bbcoop::TeamRequest::ACCEPT, now + 30, {}, team);
  b.Team(bbcoop::TeamRequest::READY, now + 30, {}, {}, 2);
  Pump(service, games, now + 30);
  CHECK(b.starts.size() == 2 && b.starts.back().party() == party.toStdString());
  b.Hello(now + 40, party);
  b.Map(Yharnam, now + 40);
  Pump(service, games, now + 40);
  CHECK(routes(a, b, 0x06) && routes(b, a, 0x06));
  CHECK(a.map_board.members_size() == 2);

  // A member that leaves by itself is out the same way (its goodbye first, as
  // above).
  b.Team(bbcoop::TeamRequest::LEAVE, now + 50);
  Pump(service, games, now + 50);
  CHECK(service.PartyOf(b.endpoint) == service.PartyOf(a.endpoint));
  const bool native06 = BloodborneCoop::NativeChannelPolicy(0x06u) ==
                            BloodborneCoop::ChannelPolicy::Pass &&
                        0x06u < BloodborneCoop::NativeChannelCount;
  CHECK(routes(a, b, 0x06) == native06 && routes(b, a, 0x06) == native06);
  return 0;
}

// Seamless Hunt is public: a client that authenticated as one account cannot
// claim another account's board/member id in either the team lobby or a party
// world.
int PublicLobbyIdentityComesFromTheAuthenticatedAccount() {
  Service service{{}};
  service.RequireAuthenticatedIdentity();
  const QByteArray alice = "alice";
  Game impostor{1000, "626f62", 11,
                alice}; // hex("bob"), while signed in as alice
  impostor.LobbyHello(false, 1'000'000);
  Pump(service, {&impostor}, 1'000'000);
  CHECK(impostor.lobby.notice() ==
        "member identity does not match the signed-in account");
  CHECK(service.Lobby().SignedIn().empty());

  Game genuine{1001, "616c696365", 12, alice}; // hex("alice")
  genuine.LobbyHello(false, 1'000'010);
  genuine.Hello(1'000'010);
  Pump(service, {&genuine}, 1'000'010);
  CHECK(genuine.lobby.notice().empty());
  CHECK(service.Lobby().SignedIn() == std::vector<std::string>{"616c696365"});
  CHECK(genuine.welcome && genuine.welcome->accepted());
  return 0;
}

// Amanises 2026-10-25: the lobby's friends as shadNet's friend table keeps them
// (lobby_friends_db.cpp's rules), in memory: a request is the asker's bit, a
// friendship both.
struct MemoryFriendBook : BloodborneWorld::FriendBook {
  std::map<std::string, std::string> accounts;         // member -> name
  std::set<std::pair<std::string, std::string>> asked; // (from, to)

  bool Asked(const std::string &a, const std::string &b) const {
    return asked.count({a, b}) != 0;
  }
  BloodborneWorld::FriendEntry Entry(const std::string &m) const {
    return {m, accounts.at(m)};
  }
  BloodborneWorld::FriendRelations Of(const std::string &member) override {
    BloodborneWorld::FriendRelations out;
    for (const auto &[other, name] : accounts) {
      const bool mine = Asked(member, other), theirs = Asked(other, member);
      if (mine && theirs) {
        out.friends.push_back(Entry(other));
      } else if (mine) {
        out.sent.push_back(Entry(other));
      } else if (theirs) {
        out.received.push_back(Entry(other));
      }
    }
    return out;
  }
  std::string Add(const std::string &member,
                  const std::string &other) override {
    if (!accounts.count(other)) {
      return "no such player";
    }
    if (Asked(member, other)) {
      return Asked(other, member) ? "already friends" : "request already sent";
    }
    asked.insert({member, other});
    return {};
  }
  std::string Remove(const std::string &member,
                     const std::string &other) override {
    if (!Asked(member, other)) {
      return "not a friend";
    }
    asked.erase({member, other});
    asked.erase({other, member});
    return {};
  }
  std::string Decline(const std::string &member,
                      const std::string &other) override {
    if (!Asked(other, member) || Asked(member, other)) {
      return "no request from this player";
    }
    asked.erase({other, member});
    return {};
  }
  std::vector<BloodborneWorld::FriendEntry>
  Search(const std::string &, const std::string &query, size_t limit) override {
    std::vector<BloodborneWorld::FriendEntry> out;
    for (const auto &[member, name] : accounts) {
      if (out.size() < limit && name.find(query) != std::string::npos) {
        out.push_back({member, name});
      }
    }
    return out;
  }
};

// Find a player by name, send a request, the other accepts it: both see a
// friend, online, with its team; removing it clears both; a declined request is
// gone for both; a server without the book refuses.
int FriendsAreFoundAskedAndAnsweredInTheLobby() {
  using bbcoop::LobbyPlayer;
  using bbcoop::TeamRequest;
  MemoryFriendBook book;
  book.accounts = {{"aa", "Player aa"},
                   {"bb", "Player bb"},
                   {"cc", "Player cc"},
                   {"dd", "Offline dd"}};
  Service service{{}};
  service.SetFriendBook(&book);
  qint64 now = 1'000'000;
  Game a{1000, "aa", 11};
  Game b{1001, "bb", 12};
  Game c{1002, "cc", 13};
  a.LobbyHello(false, now);
  b.LobbyHello(true, now);
  c.LobbyHello(false, now);
  Pump(service, {&a, &b, &c}, now);
  const auto search = [&](Game &game, const std::string &text) {
    bbcoop::Envelope envelope;
    envelope.mutable_team_request()->set_action(TeamRequest::SEARCH);
    envelope.mutable_team_request()->set_query(text);
    game.Send(envelope, now);
    Pump(service, {&a, &b, &c}, now);
  };

  // Search: an offline account is found too; the searcher sees itself as SELF.
  search(a, "a");
  CHECK(a.lobby.notice() == "type at least 2 letters" &&
        a.lobby.search_size() == 0);
  search(a, "dd");
  CHECK(a.lobby.search_query() == "dd" && a.lobby.search_size() == 1);
  CHECK(a.lobby.search(0).member() == "dd" &&
        a.lobby.search(0).state() == LobbyPlayer::OFFLINE);
  search(a, "Player");
  CHECK(a.lobby.search_size() == 3);
  CHECK(a.lobby.search(0).relation() == LobbyPlayer::SELF);
  CHECK(b.lobby.search_size() == 0); // only the searcher's

  // A request: the asker sees it sent, the other received (with its online
  // state).
  a.Team(TeamRequest::FRIEND_ADD, now, "bb");
  Pump(service, {&a, &b, &c}, now);
  CHECK(a.lobby.notice().empty());
  CHECK(a.lobby.friend_requests_sent_size() == 1 &&
        a.lobby.friend_requests_sent(0).member() == "bb");
  CHECK(b.lobby.friend_requests_size() == 1 &&
        b.lobby.friend_requests(0).member() == "aa");
  CHECK(b.lobby.friend_requests(0).state() == LobbyPlayer::TITLE);
  CHECK(a.lobby.search(1).relation() == LobbyPlayer::REQUEST_SENT);
  a.Team(TeamRequest::FRIEND_ADD, now, "bb");
  Pump(service, {&a, &b, &c}, now);
  CHECK(a.lobby.notice() == "request already sent");

  // Accepted: friends both ways, listed first among the players online.
  a.Team(TeamRequest::CREATE, now);
  b.Team(TeamRequest::FRIEND_ADD, now, "aa");
  Pump(service, {&a, &b, &c}, now);
  CHECK(a.lobby.friends_size() == 1 && b.lobby.friends_size() == 1);
  CHECK(a.lobby.friend_requests_sent_size() == 0 &&
        b.lobby.friend_requests_size() == 0);
  CHECK(a.lobby.friends(0).member() == "bb" &&
        a.lobby.friends(0).state() == LobbyPlayer::PLAYING);
  CHECK(b.lobby.friends(0).team() == a.lobby.team().id());
  CHECK(b.lobby.players(0).member() == "aa" &&
        b.lobby.players(0).relation() == LobbyPlayer::FRIEND);

  // Removed: gone for both.
  b.Team(TeamRequest::FRIEND_REMOVE, now, "aa");
  Pump(service, {&a, &b, &c}, now);
  CHECK(a.lobby.friends_size() == 0 && b.lobby.friends_size() == 0);

  // Declined: gone for both; a decline with no request is refused.
  c.Team(TeamRequest::FRIEND_ADD, now, "aa");
  Pump(service, {&a, &b, &c}, now);
  CHECK(a.lobby.friend_requests_size() == 1);
  a.Team(TeamRequest::FRIEND_DECLINE, now, "cc");
  Pump(service, {&a, &b, &c}, now);
  CHECK(a.lobby.friend_requests_size() == 0 &&
        c.lobby.friend_requests_sent_size() == 0);
  a.Team(TeamRequest::FRIEND_DECLINE, now, "cc");
  Pump(service, {&a, &b, &c}, now);
  CHECK(a.lobby.notice() == "no request from this player");
  a.Team(TeamRequest::FRIEND_ADD, now, "aa");
  Pump(service, {&a, &b, &c}, now);
  CHECK(a.lobby.notice() == "no such player");

  // Kaniel 2026-10-26: one who declined is not asked again at once; it may ask
  // itself.
  c.Team(TeamRequest::FRIEND_ADD, now, "aa");
  Pump(service, {&a, &b, &c}, now);
  CHECK(c.lobby.notice() == "this hunter declined: try again later");
  CHECK(a.lobby.friend_requests_size() == 0);
  a.Team(TeamRequest::FRIEND_ADD, now, "cc");
  Pump(service, {&a, &b, &c}, now);
  CHECK(a.lobby.notice().empty() && c.lobby.friend_requests_size() == 1);
  c.Team(TeamRequest::FRIEND_ADD, now, "aa"); // the answer to a's request
  Pump(service, {&a, &b, &c}, now);
  CHECK(c.lobby.notice().empty() && c.lobby.friends_size() == 1);
  c.Team(TeamRequest::FRIEND_REMOVE, now, "aa");
  Pump(service, {&a, &b, &c}, now);

  // Kaniel 2026-10-26: at most FriendAddsPerMinute requests and
  // SearchesPerMinute searches a minute (a made 3 requests and 3 searches
  // above).
  for (size_t i = 3; i < Service::FriendAddsPerMinute; ++i) {
    a.Team(TeamRequest::FRIEND_ADD, now, "zz");
    Pump(service, {&a, &b, &c}, now);
    CHECK(a.lobby.notice() == "no such player");
  }
  a.Team(TeamRequest::FRIEND_ADD, now, "zz");
  Pump(service, {&a, &b, &c}, now);
  CHECK(a.lobby.notice() == "too many friend requests: try again in a minute");
  for (size_t i = 3; i < Service::SearchesPerMinute; ++i) {
    search(a, "Player");
    CHECK(a.lobby.notice().empty());
  }
  search(a, "Player");
  CHECK(a.lobby.notice() == "too many searches: try again in a minute");
  search(a, ""); // clearing is no search
  CHECK(a.lobby.notice().empty());
  now += 60'000;
  search(a, "Player");
  CHECK(a.lobby.notice().empty() && a.lobby.search_size() == 3);
  a.Team(TeamRequest::FRIEND_ADD, now, "zz");
  Pump(service, {&a, &b, &c}, now);
  CHECK(a.lobby.notice() == "no such player");

  // No book: refused, and the team lobby works as before.
  Service bare{{}};
  Game e{1003, "ee", 14};
  e.LobbyHello(false, now);
  Pump(bare, {&e}, now);
  e.Team(TeamRequest::FRIEND_ADD, now, "aa");
  Pump(bare, {&e}, now);
  CHECK(e.lobby.notice() == "friends are not kept on this server");
  CHECK(e.lobby.friends_size() == 0);
  return 0;
}

// Kaniel 2026-10-26: on a hosted server a search lists the players signed in
// now, and one not signed in only by its whole name; the accounts are not
// searched (a stranger lists nobody's account by two letters).
int HostedSearchListsThePlayersOnline() {
  const auto hex = [](const char *id) {
    return QByteArray(id).toHex().toStdString();
  };
  MemoryFriendBook book;
  book.accounts = {{hex("Kaniel"), "Kaniel"},
                   {hex("Ada"), "Ada"},
                   {hex("Gone"), "Gone"},
                   {hex("Never"), "Never"}};
  Service service{{}};
  BloodborneWorld::HostedRules rules;
  rules.enabled = true;
  service.SetHosted(rules);
  service.SetFriendBook(&book);
  Game kaniel{1000, hex("Kaniel"), 11};
  kaniel.online_id = "Kaniel";
  Game ada{1001, hex("Ada"), 12};
  ada.online_id = "Ada";
  Game gone{1002, hex("Gone"), 13};
  gone.online_id = "Gone";
  std::vector<Game *> games{&kaniel, &ada, &gone};
  qint64 now = 1'000'000;
  const auto step = [&] {
    now += 300; // past the lobby's interval
    Pump(service, games, now);
  };
  for (Game *game : games) {
    game->LobbyHello(false, now);
  }
  step();
  service.Forget(gone.endpoint);
  games.pop_back();
  step();
  const auto search = [&](const std::string &text) {
    bbcoop::Envelope envelope;
    envelope.mutable_team_request()->set_action(bbcoop::TeamRequest::SEARCH);
    envelope.mutable_team_request()->set_query(text);
    kaniel.Send(envelope, now);
    step();
    return kaniel.lobby.search_size();
  };
  CHECK(search("Player") == 2); // Kaniel and Ada, signed in
  CHECK(search(hex("Ada").substr(0, 4)) == 1);
  CHECK(search("Never") == 0); // an account never in the lobby: not searched
  CHECK(search("Player " + hex("Go")) == 0); // signed out: its whole name only
  CHECK(search("player " + hex("Gone")) == 1);
  CHECK(kaniel.lobby.search(0).member() == hex("Gone"));
  return 0;
}

// Kaniel 2026-10-26: a member cut from its team says goodbye only to the
// session's host, not to the other guests.
int ACutMembersGoodbyeIsForTheHostOnly() {
  Service service{{}};
  Game host{1000, "host", 11};
  Game b{1001, "bb", 12};
  Game c{1002, "cc", 13};
  const std::vector<Game *> games{&host, &b, &c};
  qint64 now = 1'000'000;
  for (Game *game : games) {
    game->LobbyHello(false, now);
  }
  Pump(service, games, now);
  host.Team(bbcoop::TeamRequest::CREATE, now);
  Pump(service, games, now);
  const std::string team = host.lobby.team().id();
  for (Game *guest : {&b, &c}) {
    host.Team(bbcoop::TeamRequest::INVITE, now, guest->name);
    Pump(service, games, now);
    guest->Team(bbcoop::TeamRequest::ACCEPT, now, {}, team);
    Pump(service, games, now);
  }
  host.Team(bbcoop::TeamRequest::READY, now, {}, {}, 1);
  b.Team(bbcoop::TeamRequest::READY, now, {}, {}, 2);
  c.Team(bbcoop::TeamRequest::READY, now, {}, {}, 3);
  Pump(service, games, now);
  host.Team(bbcoop::TeamRequest::START, now);
  Pump(service, games, now);
  const QByteArray party = QByteArray::fromStdString(host.starts.at(0).party());
  for (Game *game : games) {
    game->LobbyHello(true, now);
    game->Hello(now, party);
    game->Map(0x18010000, now);
  }
  Pump(service, games, now);
  host.Team(bbcoop::TeamRequest::REMOVE, now + 10, "cc");
  Pump(service, games, now + 10);
  bool any_native = false;
  for (u32 channel = 0; channel < BloodborneCoop::NativeChannelCount;
       ++channel) {
    const bool native = BloodborneCoop::NativeChannelPolicy(channel) ==
                        BloodborneCoop::ChannelPolicy::Pass;
    any_native = any_native || native;
    CHECK(service.RoutesPacket(c.endpoint, host.endpoint, channel) == native);
    CHECK(service.RoutesPacket(host.endpoint, c.endpoint, channel) == native);
    CHECK(!service.RoutesPacket(c.endpoint, b.endpoint, channel));
    CHECK(!service.RoutesPacket(b.endpoint, c.endpoint, channel));
  }
  CHECK(any_native);
  CHECK(service.RoutesPacket(host.endpoint, b.endpoint, 0x06));
  return 0;
}

// Hosted (docs/HOSTED_SERVER.md): a player's names are its own id's, and a
// party hello is only for the world its team plays.
int HostedPartiesAreTheirTeamsOnly() {
  Service service{{}};
  BloodborneWorld::HostedRules rules;
  rules.enabled = true;
  service.SetHosted(rules);
  const auto hex = [](const char *id) {
    return QByteArray(id).toHex().toStdString();
  };
  Game leader{1000, hex("Kaniel"), 11};
  leader.online_id = "Kaniel";
  leader.claims_host = true;
  Game guest{1001, hex("Ada"), 12};
  guest.online_id = "Ada";
  Game thief{1002, hex("Mallory"), 13};
  thief.online_id = "Mallory";
  Game fake{1003, hex("Kaniel"), 14}; // Mallory's second game, in Kaniel's name
  fake.online_id = "Mallory";
  const std::vector<Game *> games{&leader, &guest, &thief, &fake};
  qint64 now = 1'000'000;
  const auto step = [&] {
    now += 300; // past the lobby's interval
    Pump(service, games, now);
  };
  for (Game *game : games) {
    game->LobbyHello(false, now);
  }
  step();
  CHECK(!fake.lobby.notice().empty()); // not its name
  CHECK(thief.lobby.notice().empty());
  leader.Team(bbcoop::TeamRequest::CREATE, now);
  step();
  leader.Team(bbcoop::TeamRequest::INVITE, now, guest.name);
  step();
  CHECK(guest.lobby.invites_size() == 1);
  guest.Team(bbcoop::TeamRequest::ACCEPT, now, {},
             guest.lobby.invites(0).team());
  step();
  leader.Team(bbcoop::TeamRequest::READY, now, {}, {}, 0, "Hunter");
  guest.Team(bbcoop::TeamRequest::READY, now, {}, {}, 1, "Doll");
  step();
  leader.Team(bbcoop::TeamRequest::START, now);
  step();
  CHECK(!leader.starts.empty());
  const QByteArray party =
      QByteArray::fromStdString(leader.starts.back().party());
  CHECK(party.startsWith("world:"));
  leader.Hello(now, party);
  guest.Hello(now, party);
  thief.Hello(now, party); // not in the team that plays it
  step();
  CHECK(leader.welcome && leader.welcome->accepted());
  CHECK(guest.welcome && guest.welcome->accepted());
  CHECK(thief.welcome && !thief.welcome->accepted());
  // Nor a party of its own making.
  thief.welcome.reset();
  thief.Hello(now, "mallory:anything");
  step();
  CHECK(thief.welcome && !thief.welcome->accepted());
  // The thief's world list does not show the team's world.
  CHECK(thief.lobby.worlds_size() == 0);
  CHECK(leader.lobby.worlds_size() == 1);
  return 0;
}

// Invasions (docs/INVASIONS.md): a started team of two ("aa" hosts, "bb") in
// Central Yharnam whose leader allows invaders, and "rr", a player in no team.
constexpr u32 InvadedMap = 0x18010000;
constexpr u32 RulesKind = 0x1234;
constexpr u32 RulesSign = 4;

struct InvasionRig {
  Service service;
  Game a{1000, "aa", 11};
  Game b{1001, "bb", 12};
  Game red{1002, "rr", 13};
  std::vector<Game *> games{&a, &b, &red};
  qint64 now = 1'000'000;
  QByteArray party;

  explicit InvasionRig(bool enabled = true, bool allow = true,
                       const QString &folder = {},
                       const BloodborneWorld::Limits &limits = {})
      : service{folder} {
    service.SetLimits(limits);
    if (!folder.isEmpty()) {
      service.Load(now);
    }
    BloodborneWorld::InvasionRules rules;
    rules.enabled = enabled;
    rules.kind = RulesKind;
    rules.sign = RulesSign;
    service.SetInvasions(rules);
    for (Game *game : games) {
      game->LobbyHello(false, now);
    }
    Step();
    a.Team(bbcoop::TeamRequest::CREATE, now);
    Step();
    a.Team(bbcoop::TeamRequest::INVITE, now, "bb");
    Step();
    b.Team(bbcoop::TeamRequest::ACCEPT, now, {}, a.lobby.team().id());
    Step();
    a.Team(bbcoop::TeamRequest::READY, now, {}, {}, 1);
    b.Team(bbcoop::TeamRequest::READY, now, {}, {}, 2);
    Step();
    a.Team(bbcoop::TeamRequest::START, now);
    Step();
    party = QByteArray::fromStdString(a.starts.at(0).party());
    for (Game *game : games) {
      game->LobbyHello(true, now);
    }
    a.claims_host = true;
    b.claims_host = false;
    a.Hello(now, party);
    b.Hello(now, party);
    Step();
    bbcoop::Envelope session;
    session.mutable_team_session()->set_join("NXRV-join");
    session.mutable_team_session()->set_kind(0xFFFFFFFF);
    session.mutable_team_session()->set_sign(7);
    session.mutable_team_session()->set_session(1);
    a.Send(session, now);
    a.Map(InvadedMap, now);
    b.Map(InvadedMap, now);
    a.Vitals(InvadedMap, 1, now, 50);
    b.Vitals(InvadedMap, 1, now, 55);
    Step();
    if (allow) {
      Allow(true);
    }
  }
  void EnableRewards() {
    for (Game* game : games) {
      game->invasion_rewards = true;
      game->LobbyHello(true, now);
    }
    Step();
  }
  void EnableCovenants(u32 host_oath, u32 invader_oath) {
    a.covenant_effect = host_oath;
    red.covenant_effect = invader_oath;
    for (Game* game : games) {
      game->invasion_rewards = true;
      game->covenant_rewards = true;
      game->LobbyHello(true, now);
    }
    a.Vitals(InvadedMap, 1, now, 50);
    Step();
  }
  void Allow(bool allow) {
    bbcoop::Envelope envelope;
    envelope.mutable_team_request()->set_action(bbcoop::TeamRequest::INVADERS);
    envelope.mutable_team_request()->set_allow(allow);
    a.Send(envelope, now);
    Step();
  }
  void Step(qint64 ms = 0) {
    now += ms;
    Pump(service, games, now);
  }
  bool Routes(const Game &from, const Game &to, u32 channel) const {
    return service.RoutesPacket(from.endpoint, to.endpoint, channel);
  }
  // rr asks, is told, and joins after the notice.
  bool Invade() {
    red.Invade(60, now);
    red.Map(InvadedMap, now);
    Step();
    if (red.notices.empty() || a.notices.empty() || b.notices.empty()) {
      return false;
    }
    Step(InvasionNoticeMs);
    // The maps again (a member's map not heard of for seconds leaves the
    // enemy owners).
    for (Game *game : games) {
      game->Map(InvadedMap, now);
    }
    Step();
    return red.invasion.has_value();
  }
};

bool EndedFor(const Game &game, bbcoop::InvasionEnd::Reason reason) {
  return !game.ends.empty() && game.ends.back().reason() == reason;
}

int AnInvaderJoinsATeamWorldAfterANotice() {
  InvasionRig rig;
  Game &red = rig.red;
  red.Invade(60, rig.now);
  red.Map(InvadedMap, rig.now);
  rig.Step();
  // Matched: both sides hear "an invader approaches", nobody joins yet.
  CHECK(red.notices.size() == 1 && rig.a.notices.size() == 1 &&
        rig.b.notices.size() == 1);
  CHECK(rig.a.notices[0].in_ms() == InvasionNoticeMs);
  CHECK(rig.a.notices[0].invader() == "rr" &&
        rig.a.notices[0].target() == "aa");
  CHECK(!red.invasion);
  CHECK(!rig.Routes(red, rig.a, 0x06));
  CHECK(rig.service.VisitingOf(red.endpoint).isEmpty());
  rig.Step(InvasionNoticeMs - 1);
  CHECK(!red.invasion);
  rig.a.Map(InvadedMap, rig.now); // the games say their maps every second
  rig.b.Map(InvadedMap, rig.now);
  // Past the notice: the invader gets the host's session with its own kind
  // and sign, the team that it joined.
  rig.Step(1);
  CHECK(red.invasion.has_value());
  CHECK(red.invasion->session().join() == "NXRV-join");
  CHECK(red.invasion->session().kind() == RulesKind &&
        red.invasion->session().sign() == RulesSign);
  CHECK(red.invasion->session().host() == "aa");
  CHECK(red.invasion->target() == "aa" && red.invasion->map() == InvadedMap);
  CHECK(rig.a.joined.size() == 1 && rig.b.joined.size() == 1 &&
        rig.a.joined[0].member() == "rr" &&
        rig.a.joined[0].name() == "Player rr");
  // It travels to the target first: where it stood, and whose the enemies are.
  CHECK(red.invasion->has_at());
  CHECK(red.invasion->owners().owners_size() >= 1);
  // The hub sees it in the team's routing, its own party still its own.
  CHECK(rig.service.VisitingOf(red.endpoint) ==
        rig.service.PartyOf(rig.a.endpoint));
  CHECK(rig.service.PartyOf(red.endpoint).isEmpty());
  // One invader a world: a second one is not matched, and is told so once
  // its search runs out.
  Game blue{1003, "bl", 14};
  rig.games.push_back(&blue);
  blue.LobbyHello(true, rig.now);
  blue.Invade(60, rig.now);
  rig.Step();
  CHECK(blue.notices.empty());
  rig.Step(InvasionSearchMs);
  CHECK(EndedFor(blue, bbcoop::InvasionEnd::NOT_FOUND));
  CHECK(red.ends.empty());
  return 0;
}

int AnInvasionNeedsTheServerAndTheTeamToAllowIt() {
  {
    InvasionRig rig{false}; // the kill switch: off by default
    rig.red.Invade(60, rig.now);
    rig.Step();
    CHECK(EndedFor(rig.red, bbcoop::InvasionEnd::REFUSED));
    CHECK(rig.a.notices.empty());
  }
  {
    InvasionRig rig{true, false}; // the leader did not allow invaders
    rig.red.Invade(60, rig.now);
    rig.Step();
    CHECK(rig.red.notices.empty() && rig.a.notices.empty());
    rig.Step(InvasionSearchMs);
    CHECK(EndedFor(rig.red, bbcoop::InvasionEnd::NOT_FOUND));
  }
  {
    InvasionRig rig; // a team's member does not invade
    rig.b.Invade(60, rig.now);
    rig.Step();
    CHECK(EndedFor(rig.b, bbcoop::InvasionEnd::REFUSED));
  }
  {
    InvasionRig rig; // the team in the Hunter's Dream is not invaded
    rig.a.Map(HuntersDreamMap, rig.now);
    rig.b.Map(HuntersDreamMap, rig.now);
    rig.Step();
    rig.red.Invade(60, rig.now);
    rig.Step();
    CHECK(rig.red.notices.empty());
    // Out in the world again: found at the next search.
    rig.a.Map(InvadedMap, rig.now);
    rig.Step(2000);
    CHECK(rig.red.notices.size() == 1);
  }
  return 0;
}

int AnInvadersPacketsGoByTheHostileTable() {
  InvasionRig rig;
  CHECK(rig.Invade());
  Game &red = rig.red;
  Game &a = rig.a; // hosts, and runs Central Yharnam's enemies
  Game &b = rig.b;
  CHECK(a.OwnerOf(InvadedMap) == "aa");
  // Players and their hits on one map, both ways; the join handshake.
  CHECK(rig.Routes(red, a, 0x06) && rig.Routes(a, red, 0x06));
  CHECK(rig.Routes(red, b, 0x14) && rig.Routes(b, red, 0x14));
  CHECK(rig.Routes(red, a, 0x07) && rig.Routes(a, red, 0x10));
  // Never its events, objects, enemy hits, items or Insight.
  for (const u32 channel :
       {0x0Eu, 0x0Fu, 0x31u, 0x2Du, 0x15u, 0x16u, 0x34u, 0x2Cu, 0x39u}) {
    CHECK(!rig.Routes(red, a, channel) && !rig.Routes(red, b, channel));
  }
  // The enemies reach it from their owner only, and never come from it.
  CHECK(rig.Routes(a, red, 0x05) && rig.Routes(a, red, 0x04));
  CHECK(!rig.Routes(b, red, 0x05) && !rig.Routes(b, red, 0x04));
  CHECK(!rig.Routes(red, a, 0x05) && !rig.Routes(red, b, 0x04));
  // The team among itself as before.
  CHECK(rig.Routes(a, b, 0x0E) && rig.Routes(a, b, 0x05));
  // Another map: nothing of the players.
  b.Map(0x18000000, rig.now);
  rig.Step();
  CHECK(!rig.Routes(red, b, 0x06) && !rig.Routes(b, red, 0x14));
  CHECK(rig.Routes(red, a, 0x06));
  return 0;
}

int AnInvaderNeverChangesTheTeamsWorld() {
  InvasionRig rig;
  CHECK(rig.Invade());
  Game &red = rig.red;
  Game &a = rig.a;
  const BloodborneCoop::WorldLogHost *world = rig.service.World(rig.party);
  CHECK(world != nullptr);
  const u64 last = world->Last();
  red.Propose({{InvadedMap, {0x10014066}}}, rig.now);
  red.Flags({{1, 12345, true}}, rig.now);
  red.Hp(0x77, {{0x10014067, 10, 100}}, rig.now);
  bbcoop::Envelope echoes;
  echoes.mutable_echoes()->add_kills()->set_echoes(500);
  red.Send(echoes, rig.now);
  red.Vitals(InvadedMap, 1, rig.now);
  rig.Step();
  CHECK(world->Last() == last);
  CHECK(!Killed(a, 0x10014066) && !FlagApplied(a, 12345, true));
  CHECK(a.echoes.empty());
  CHECK(a.map_board.members_size() == 2);
  // Its vitals are a hostile line on the board (its body stays shown), never a
  // member's.
  bool hostile_line = false;
  for (const auto &entry : a.vitals_board.members()) {
    if (entry.member() == "rr") {
      hostile_line = entry.hostile();
    } else {
      CHECK(!entry.hostile());
    }
  }
  CHECK(hostile_line);
  CHECK(a.OwnerOf(InvadedMap) == "aa");
  // A member's kill (the area's owner's) still stands.
  a.Propose({{InvadedMap, {0x10014066}}}, rig.now);
  rig.Step();
  CHECK(Killed(a, 0x10014066));
  return 0;
}

// Every end the server owns: InvasionEnd to everyone in it, and the invader out
// of the team's routing again.
int EachEndTellsEveryoneAndRestoresTheRouting() {
  using Reason = bbcoop::InvasionEnd;
  const std::vector<
      std::pair<Reason::Reason, std::function<void(InvasionRig &)>>>
      ends{
          {Reason::INVADER_DIED,
           [](InvasionRig &rig) { rig.red.Vitals(InvadedMap, 3, rig.now); }},
          {Reason::TARGET_KILLED,
           [](InvasionRig &rig) { rig.a.Vitals(InvadedMap, 3, rig.now); }},
          {Reason::PARTY_WIPED,
           [](InvasionRig &rig) {
             rig.a.Vitals(InvadedMap, 2, rig.now);
             rig.b.Vitals(InvadedMap, 2, rig.now);
           }},
          {Reason::BOSS,
           [](InvasionRig &rig) {
             rig.b.Vitals(InvadedMap, 1, rig.now, 55, true);
           }},
          {Reason::TIMEOUT,
           [](InvasionRig &rig) {
             rig.Step(InvasionMaxMs);
             rig.Step(1000);
           }},
          {Reason::INVADER_LEFT,
           [](InvasionRig &rig) { rig.red.Invade(0, rig.now, true); }},
          {Reason::INVADER_LEFT,
           [](InvasionRig &rig) { rig.service.LinkLost(rig.red.endpoint); }},
          {Reason::HOST_LEFT,
           [](InvasionRig &rig) {
             bbcoop::Envelope closed;
             closed.mutable_team_session()->set_session(1);
             rig.a.Send(closed, rig.now);
           }},
          {Reason::HOST_LEFT,
           [](InvasionRig &rig) { rig.service.LinkLost(rig.a.endpoint); }},
          {Reason::INVADERS_OFF, [](InvasionRig &rig) { rig.Allow(false); }},
      };
  for (const auto &[reason, end] : ends) {
    InvasionRig rig;
    rig.EnableRewards();
    CHECK(rig.Invade());
    CHECK(rig.Routes(rig.red, rig.a, 0x06));
    end(rig);
    rig.Step();
    CHECK(EndedFor(rig.red, reason));
    CHECK(EndedFor(rig.a, reason) && EndedFor(rig.b, reason));
    CHECK(rig.red.ends.size() == 1 && rig.a.ends.size() == 1);
    CHECK(rig.red.grants.empty() && rig.b.grants.empty());
    CHECK(rig.a.grants.size() == (reason == Reason::INVADER_DIED ? 1 : 0));
    CHECK(rig.service.VisitingOf(rig.red.endpoint).isEmpty());
    CHECK(!rig.Routes(rig.red, rig.a, 0x06) &&
          !rig.Routes(rig.a, rig.red, 0x14));
    // The team rests: the same invader is not matched again at once.
    rig.red.ends.clear();
    rig.red.notices.clear();
    rig.red.Invade(60, rig.now);
    rig.Step(2000);
    CHECK(rig.red.notices.empty());
  }
  // A link forgotten ends it too.
  InvasionRig rig;
  CHECK(rig.Invade());
  rig.red.online = false;
  rig.service.Forget(rig.red.endpoint);
  rig.Step();
  CHECK(EndedFor(rig.a, Reason::INVADER_LEFT));
  // Ended during the notice: the invader never joins.
  InvasionRig early;
  early.red.Invade(60, early.now);
  early.red.Map(InvadedMap, early.now);
  early.Step();
  CHECK(early.red.notices.size() == 1);
  early.Allow(false);
  early.Step(InvasionNoticeMs);
  CHECK(!early.red.invasion && EndedFor(early.red, Reason::INVADERS_OFF));
  return 0;
}

// Caps (docs/HOSTED_SERVER.md): a message past the limit drops the member's
// stream; its game starts the stream over and says hello again.
int AMessagePastTheLimitDropsTheStream() {
  Service service{{}};
  BloodborneWorld::Limits limits;
  limits.message_bytes = 64 * 1024;
  service.SetLimits(limits);
  Game host{1000, "host", 11};
  qint64 now = 1'000'000;
  host.Hello(now);
  Pump(service, {&host}, now);
  CHECK(host.welcome && host.welcome->accepted());
  CHECK(!service.PartyOf(host.endpoint).isEmpty());
  host.stream.Send(std::vector<u8>(100 * 1024, 0x0A), now);
  Pump(service, {&host}, now);
  CHECK(service.PartyOf(host.endpoint).isEmpty());
  CHECK(host.stream.TakeRestarted());
  host.welcome.reset();
  now += 100;
  host.Hello(now);
  Pump(service, {&host}, now);
  CHECK(host.welcome && host.welcome->accepted());
  CHECK(!service.PartyOf(host.endpoint).isEmpty());
  return 0;
}

// A member that never acks what the server queues to it is dropped past the
// limit; the others play on.
int AMemberThatNeverAcksIsDropped() {
  Service service{{}};
  BloodborneWorld::Limits limits;
  limits.unacked_bytes = 200 * 1024;
  service.SetLimits(limits);
  Game host{1000, "host", 11};
  Game guest{1001, "guest", 12};
  qint64 now = 1'000'000;
  host.Hello(now);
  guest.Hello(now);
  Pump(service, {&host, &guest}, now);
  CHECK(guest.welcome && guest.welcome->accepted());
  guest.deaf = true;
  for (u32 id = 1; id <= 8; ++id) {
    bbcoop::Envelope envelope;
    envelope.mutable_portrait()->set_id(id);
    envelope.mutable_portrait()->set_pixels(std::string(40'000, 'p'));
    host.Send(envelope, now);
    Pump(service, {&host, &guest}, now);
  }
  CHECK(service.PartyOf(guest.endpoint).isEmpty());
  CHECK(!service.PartyOf(host.endpoint).isEmpty());
  return 0;
}

// A message with a field past its limit is dropped whole.
int AMessagePastAFieldsLimitIsDropped() {
  Service service{{}};
  Game host{1000, "host", 11};
  Game guest{1001, "guest", 12};
  qint64 now = 1'000'000;
  host.Hello(now);
  guest.Hello(now);
  Pump(service, {&host, &guest}, now);
  bbcoop::Envelope portrait;
  portrait.mutable_portrait()->set_id(1);
  portrait.mutable_portrait()->set_pixels(
      std::string(BloodborneWorld::MaxPortraitBytes + 1, 'p'));
  host.Send(portrait, now);
  host.MapState(0x18000000, std::vector<u8>(5000, 0xFF), now);
  std::vector<u8> bits((MapFlagCount + 7) / 8);
  bits[0] = 0x80;
  WorldLogKills kills;
  for (u32 handle = 0; handle < 40'000; ++handle) {
    kills[0x18000000].insert(handle);
  }
  host.Propose(kills, now);
  Pump(service, {&host, &guest}, now);
  CHECK(guest.portraits.empty());
  CHECK(service.World(Party)->MapStateNow().bits.empty());
  CHECK(service.World(Party)->Kills().empty());
  // Within the limits they go through.
  portrait.mutable_portrait()->set_pixels(
      std::string(BloodborneWorld::MaxPortraitBytes, 'p'));
  host.Send(portrait, now);
  host.MapState(0x18000000, bits, now);
  Pump(service, {&host, &guest}, now);
  CHECK(guest.portraits.size() == 1);
  CHECK(!service.World(Party)->MapStateNow().bits.empty());
  // A hello with a name past the limit gets no answer.
  Game long_name{1002, std::string(200, 'x'), 13};
  long_name.Hello(now);
  Pump(service, {&host, &guest, &long_name}, now);
  CHECK(!long_name.welcome);
  return 0;
}

// A world nobody is in goes out of memory after a while, and comes back from
// its folder as it was with the next hello.
int AnEmptyPartyWorldIsCollected() {
  QTemporaryDir folder;
  CHECK(folder.isValid());
  Service service{folder.path()};
  BloodborneWorld::Limits limits;
  limits.idle_world_ms = 60'000;
  service.SetLimits(limits);
  qint64 now = 1'000'000;
  service.Load(now);
  Game host{1000, "host", 11};
  host.Hello(now);
  Pump(service, {&host}, now);
  host.Propose({{0x18000000, {0x10014066}}}, now);
  Pump(service, {&host}, now);
  CHECK(Killed(host, 0x10014066));
  service.Forget(host.endpoint);
  service.Tick(now + 1000);
  CHECK(service.Parties() == 1);
  service.Tick(now + 30'000);
  CHECK(service.Parties() == 1);
  service.Tick(now + 70'000);
  CHECK(service.Parties() == 0);
  Game again{1001, "host", 21};
  again.run = 0x2002;
  again.Hello(now + 80'000);
  Pump(service, {&again}, now + 80'000);
  CHECK(again.welcome && again.welcome->accepted());
  CHECK(service.Parties() == 1);
  CHECK(Killed(again, 0x10014066));
  return 0;
}

// So many worlds in memory at once: a new one past them only when a world
// nobody is in can go.
int TheWorldsInMemoryAreCapped() {
  QTemporaryDir folder;
  CHECK(folder.isValid());
  Service service{folder.path()};
  BloodborneWorld::Limits limits;
  limits.party_worlds = 2;
  service.SetLimits(limits);
  qint64 now = 1'000'000;
  service.Load(now);
  Game a{1000, "a", 11};
  Game b{1001, "b", 12};
  Game c{1002, "c", 13};
  a.Hello(now, "a:1");
  b.Hello(now, "b:1");
  Pump(service, {&a, &b}, now);
  c.Hello(now, "c:1");
  Pump(service, {&a, &b, &c}, now);
  CHECK(a.welcome && a.welcome->accepted());
  CHECK(b.welcome && b.welcome->accepted());
  CHECK(c.welcome && !c.welcome->accepted());
  CHECK(service.Parties() == 2);
  service.Forget(a.endpoint);
  c.welcome.reset();
  c.Hello(now + 10, "c:1");
  Pump(service, {&b, &c}, now + 10);
  CHECK(c.welcome && c.welcome->accepted());
  CHECK(service.Parties() == 2);
  return 0;
}

// A world collected while nobody is in it keeps where its guests stood: a
// guest's next run resumes there as before.
int ACollectedWorldKeepsItsPlaces() {
  QTemporaryDir folder;
  CHECK(folder.isValid());
  Service service{folder.path()};
  BloodborneWorld::Limits limits;
  limits.idle_world_ms = 60'000;
  service.SetLimits(limits);
  qint64 now = 1'000'000;
  service.Load(now);
  Game host{1000, "host", 11};
  Game guest{1001, "guest", 12};
  host.Hello(now);
  guest.Hello(now);
  Pump(service, {&host, &guest}, now);
  bbcoop::Envelope vitals = VitalsOf(0x18010000, 1);
  vitals.mutable_vitals()->mutable_at()->set_x(-215.5f);
  vitals.mutable_vitals()->mutable_at()->set_z(172.25f);
  guest.Send(vitals, now + 10);
  Pump(service, {&host, &guest}, now + 10);
  service.Forget(host.endpoint);
  service.Forget(guest.endpoint);
  service.Tick(now + 1000);
  service.Tick(now + 70'000);
  CHECK(service.Parties() == 0);
  Game host_again{1002, "host", 21};
  Game guest_again{1003, "guest", 22};
  host_again.run = guest_again.run = 0x2002;
  host_again.Hello(now + 80'000);
  guest_again.Hello(now + 80'000);
  Pump(service, {&host_again, &guest_again}, now + 80'000);
  CHECK(guest_again.welcome && guest_again.welcome->accepted());
  CHECK(guest_again.resume_at.has_value());
  CHECK(guest_again.resume_at->map() == 0x18010000);
  CHECK(guest_again.resume_at->at().x() == -215.5f);
  return 0;
}

// At the cap a new party's world takes the place of a few of the oldest worlds
// nobody is in, not of all of them (each is written whole first).
int AtTheCapOnlyAFewWorldsAreCollected() {
  QTemporaryDir folder;
  CHECK(folder.isValid());
  Service service{folder.path()};
  BloodborneWorld::Limits limits;
  limits.party_worlds = 20;
  service.SetLimits(limits);
  qint64 now = 1'000'000;
  service.Load(now);
  for (int i = 0; i < 20; ++i) {
    Game game{static_cast<quint32>(1000 + i), "g" + std::to_string(i),
              static_cast<quint32>(11 + i)};
    game.Hello(now, QByteArray("p:") + QByteArray::number(i));
    Pump(service, {&game}, now);
    CHECK(game.welcome && game.welcome->accepted());
    service.Forget(game.endpoint);
  }
  CHECK(service.Parties() == 20);
  Game late{2000, "late", 99};
  late.Hello(now + 10, "late:1");
  Pump(service, {&late}, now + 10);
  CHECK(late.welcome && late.welcome->accepted());
  CHECK(service.Parties() > 10);
  CHECK(service.Parties() <= 20);
  return 0;
}

// A game started again on the same link (a new stream, no new link) hears its
// lobby state when it says its lobby hello, as a new game does.
int ARestartedGameHearsItsLobby() {
  for (const bool hosted : {false, true}) {
    Service service{{}};
    if (hosted) {
      BloodborneWorld::HostedRules rules;
      rules.enabled = true;
      service.SetHosted(rules);
    }
    const std::string name =
        hosted ? QByteArray("Kaniel").toHex().toStdString() : std::string("aa");
    qint64 now = 1'000'000;
    Game a{1000, name, 11};
    a.online_id = "Kaniel";
    a.LobbyHello(false, now);
    Pump(service, {&a}, now);
    now += 1000;
    Pump(service, {&a}, now);
    CHECK(a.lobby_states >= 1);
    Game again{1000, name, 21};
    again.online_id = "Kaniel";
    now += 1000;
    again.LobbyHello(false, now);
    Pump(service, {&again}, now);
    now += 1000;
    Pump(service, {&again}, now);
    CHECK(again.lobby_states >= 1);
  }
  return 0;
}

// Hosted: a trade only between members of one started team.
int HostedTradesAreWithinAStartedTeam() {
  Service service{{}};
  BloodborneWorld::HostedRules rules;
  rules.enabled = true;
  service.SetHosted(rules);
  const auto hex = [](const char *id) {
    return QByteArray(id).toHex().toStdString();
  };
  Game leader{1000, hex("Kaniel"), 11};
  leader.online_id = "Kaniel";
  leader.claims_host = true;
  Game guest{1001, hex("Ada"), 12};
  guest.online_id = "Ada";
  const std::vector<Game *> games{&leader, &guest};
  qint64 now = 1'000'000;
  const auto step = [&] {
    now += 300;
    Pump(service, games, now);
  };
  for (Game *game : games) {
    game->LobbyHello(false, now);
  }
  step();
  leader.Team(bbcoop::TeamRequest::CREATE, now);
  step();
  leader.Team(bbcoop::TeamRequest::INVITE, now, guest.name);
  step();
  guest.Team(bbcoop::TeamRequest::ACCEPT, now, {},
             guest.lobby.invites(0).team());
  step();
  leader.Team(bbcoop::TeamRequest::READY, now, {}, {}, 0, "Hunter");
  guest.Team(bbcoop::TeamRequest::READY, now, {}, {}, 1, "Doll");
  step();
  leader.Team(bbcoop::TeamRequest::START, now);
  step();
  CHECK(!leader.starts.empty());
  const QByteArray party =
      QByteArray::fromStdString(leader.starts.back().party());
  leader.Hello(now, party);
  guest.Hello(now, party);
  for (Game *game : games) {
    game->LobbyHello(true, now);
  }
  step();
  CHECK(guest.welcome && guest.welcome->accepted());
  const TradeItem vials{TradeItemKind::Goods, BloodVialGoods, 3};
  leader.Send(leader.trades.Offer(guest.name, "Doll", "Hunter", {vials}, {}),
              now);
  step();
  CHECK(guest.trades.Asked().size() == 1);

  // Both back at the title, the guest first (back while the leader plays, it
  // leaves the team: #403): the team is not started, its games still linked.
  guest.LobbyHello(false, now);
  step();
  leader.LobbyHello(false, now);
  step();
  CHECK(service.Lobby().TeamOf(leader.name) &&
        !service.Lobby().TeamOf(leader.name)->started());
  CHECK(!service.PartyOf(guest.endpoint).isEmpty());
  leader.Send(leader.trades.Offer(guest.name, "Doll", "Hunter", {vials}, {}),
              now);
  step();
  CHECK(guest.trades.Asked().size() == 1);
  CHECK(leader.added.size() == 1 && leader.added.front() == TradeItems{vials});
  return 0;
}

// Merged with the caps and the world collector (docs/HOSTED_SERVER.md): an
// invader whose stream breaks its limit is dropped like any game, and its
// invasion ends; a world with an invasion is never put out of memory, and the
// invader keeps no world loaded.
int VictoryRewardsUseSoloMailboxesAndSurviveRestart() {
  CHECK(InvasionVictoryEchoes(30, true) == 556);
  CHECK(InvasionVictoryEchoes(30, false) == 1670);
  CHECK(InvasionVictoryEchoes(0, true) == 0);
  CHECK(InvasionVictoryEchoes(545, false) == 0);
  CHECK(InvasionVictoryEchoes(4, false) == 0);
  CHECK(InvasionVictoryEchoes(9, true) == 0);
  CHECK(InvasionVictoryEchoes(10, true) == 48);
  CHECK(InvasionVictoryEchoes(10, false) == 146);
  // Freeze the defeated level at join/match, not an arbitrary death report.
  {
    InvasionRig rig;
    rig.EnableRewards();
    CHECK(rig.Invade());
    rig.red.Vitals(InvadedMap, 3, rig.now, 544);
    rig.Step();
    CHECK(rig.a.grants.size() == 1 && rig.red.grants.empty() && rig.b.grants.empty());
    CHECK((TradeItemsOf(rig.a.grants[0].items()) == TradeItems{
        {TradeItemKind::Insight, 0, 1}, {TradeItemKind::Echoes, 0, 6233}}));
    rig.red.Vitals(InvadedMap, 3, rig.now, 544);
    rig.Step();
    CHECK(rig.a.grants.size() == 1); // duplicate death cannot pay twice
  }
  {
    InvasionRig early;
    early.EnableRewards();
    early.red.Invade(60, early.now);
    early.red.Map(InvadedMap, early.now);
    early.Step();
    CHECK(early.red.notices.size() == 1 && !early.red.invasion);
    early.red.Vitals(InvadedMap, 3, early.now, 60);
    early.Step();
    CHECK(EndedFor(early.red, bbcoop::InvasionEnd::INVADER_DIED));
    CHECK(early.a.grants.empty() && early.b.grants.empty() && early.red.grants.empty());
  }
  {
    InvasionRig legacy;
    CHECK(legacy.Invade());
    legacy.a.Vitals(InvadedMap, 3, legacy.now, 50, false, true);
    legacy.Step();
    CHECK(EndedFor(legacy.red, bbcoop::InvasionEnd::TARGET_KILLED));
    CHECK(legacy.red.grants.empty()); // legacy client never receives Insight kind 4
  }
  QTemporaryDir folder;
  CHECK(folder.isValid());
  qint64 now = 0;
  {
    InvasionRig rig{true, true, folder.path()};
    rig.EnableRewards();
    CHECK(rig.Invade());
    CHECK(rig.service.PartyOf(rig.red.endpoint).isEmpty()); // real solo: no party Hello
    rig.red.deaf = true; // reward cannot reach it, so it remains owed on disk
    rig.a.Vitals(InvadedMap, 3, rig.now, 99, false, true);
    rig.Step();
    CHECK(rig.a.grants.empty() && rig.b.grants.empty() && rig.red.grants.empty());
    rig.service.Drain();
    now = rig.now + 100;
  }
  Service service{folder.path()};
  service.Load(now);
  Game old{1002, "rr", 100};
  old.LobbyHello(true, now);
  Pump(service, {&old}, now);
  CHECK(old.grants.empty()); // reconnect by an old client preserves but does not send the grant
  // A different authenticated ID cannot acknowledge another identity's mailbox.
  Game thief{1009, "thief", 101};
  thief.invasion_rewards = true;
  thief.LobbyHello(true, now);
  bbcoop::Envelope stolen_ack;
  stolen_ack.mutable_trade_granted()->set_id(InvasionRewardGrantBit | 1);
  thief.Send(stolen_ack, now);
  Pump(service, {&old, &thief}, now);
  CHECK(thief.grants.empty());
  old.invasion_rewards = true;
  old.LobbyHello(true, now + 10);
  Pump(service, {&old, &thief}, now + 10);
  CHECK(old.grants.size() == 1);
  const auto& grant = old.grants.front();
  CHECK(grant.reason() == static_cast<u32>(TradeGrantReason::InvasionReward));
  CHECK(grant.id() == (InvasionRewardGrantBit | 1));
  CHECK((TradeItemsOf(grant.items()) == TradeItems{
      {TradeItemKind::Insight, 0, 1}, {TradeItemKind::Echoes, 0, 1453}}));
  CHECK(old.added.size() == 1);
  CHECK(service.PartyOf(old.endpoint).isEmpty()); // ACK worked without ever joining a team/world
  service.Drain();
  // ACK survives restart too: another process with the same identity receives nothing.
  Service after_ack{folder.path()};
  after_ack.Load(now + 20);
  Game newer{1002, "rr", 102};
  newer.invasion_rewards = true;
  newer.LobbyHello(true, now + 20);
  Pump(after_ack, {&newer}, now + 20);
  CHECK(newer.grants.empty() && newer.added.empty());
  {
    InvasionRig next{true, true, folder.path()};
    next.EnableRewards();
    CHECK(next.Invade());
    next.a.Vitals(InvadedMap, 3, next.now, 50, false, true);
    next.Step();
    CHECK(next.red.grants.size() == 1);
    CHECK(next.red.grants.front().id() == (InvasionRewardGrantBit | 2));
  }
  return 0;
}

int CovenantRewardsFollowFrozenNativeOaths() {
  CHECK(NormalizeCovenantEffect(6100) == CorruptionCovenantEffect);
  CHECK(NormalizeCovenantEffect(6110) == RadianceCovenantEffect);
  CHECK(NormalizeCovenantEffect(6120) == HunterCovenantEffect);
  CHECK(NormalizeCovenantEffect(6130) == BloodAddledHunterCovenantEffect);
  CHECK(NormalizeCovenantEffect(12345) == 0);
  CHECK(NormalizeCovenantEffect(0xffffffff) == 0);
  // Native lots 5500/5510 contain one Dreg; 5600/5610/5700/5710/5800/5810 are empty.
  for (const u32 effect : {0u, 6100u, 6110u, 6120u, 6130u, 12345u}) {
    for (const bool host_wins : {true, false}) {
      InvasionRig rig;
      rig.EnableCovenants(effect, effect);
      CHECK(rig.Invade());
      // Neither swapping out a Corruption rune nor swapping one in mid-visit changes rewards.
      rig.a.covenant_effect = effect == 6100 ? 0 : 6100;
      rig.red.covenant_effect = rig.a.covenant_effect;
      rig.a.Vitals(InvadedMap, 1, rig.now, 50);
      rig.red.Vitals(InvadedMap, 1, rig.now, 60);
      rig.Step();
      if (host_wins) {
        rig.red.Vitals(InvadedMap, 3, rig.now, 60);
      } else {
        rig.a.Vitals(InvadedMap, 3, rig.now, 50, false, true);
      }
      rig.Step();
      const auto& winner = host_wins ? rig.a : rig.red;
      const auto& loser = host_wins ? rig.red : rig.a;
      CHECK(winner.grants.size() == 1 && loser.grants.empty() && rig.b.grants.empty());
      const auto& grant = winner.grants.front();
      CHECK(!TradeGrantItemsProblem(grant));
      const auto items = TradeItemsOf(grant.items());
      CHECK(items.size() == (effect == 6100 ? 3 : 2));
      const auto dregs = std::ranges::count_if(items, [](const TradeItem& item) {
        return item.kind == TradeItemKind::Goods && item.id == BloodDregGoods && item.count == 1;
      });
      CHECK(dregs == (effect == 6100 ? 1 : 0));
    }
  }
  // A reward-capable client without the covenant extension keeps the original two-item grant.
  for (const bool host_wins : {true, false}) {
    InvasionRig old;
    old.EnableRewards();
    old.a.covenant_effect = old.red.covenant_effect = CorruptionCovenantEffect;
    old.a.Vitals(InvadedMap, 1, old.now, 50);
    old.Step();
    CHECK(old.Invade());
    if (host_wins) old.red.Vitals(InvadedMap, 3, old.now, 60);
    else old.a.Vitals(InvadedMap, 3, old.now, 50, false, true);
    old.Step();
    const auto& winner = host_wins ? old.a : old.red;
    CHECK(winner.grants.size() == 1);
    CHECK(winner.grants.front().items_size() == 2);
  }
  return 0;
}

int ExtendedCovenantGrantsWaitForACapableReconnect() {
  QTemporaryDir folder;
  CHECK(folder.isValid());
  qint64 now = 0;
  {
    InvasionRig rig{true, true, folder.path()};
    rig.EnableCovenants(RadianceCovenantEffect, CorruptionCovenantEffect);
    CHECK(rig.Invade());
    rig.red.deaf = true;
    rig.a.Vitals(InvadedMap, 3, rig.now, 50, false, true);
    rig.Step();
    rig.service.Drain();
    now = rig.now + 100;
  }
  {
    Service service{folder.path()};
    service.Load(now);
    Game old{1002, "rr", 800};
    old.invasion_rewards = true;
    old.LobbyHello(true, now);
    Pump(service, {&old}, now);
    CHECK(old.grants.empty() && old.added.empty()); // old validator cannot accept a Dreg
    old.covenant_rewards = true;
    old.LobbyHello(true, now + 10);
    Pump(service, {&old}, now + 10);
    CHECK(old.grants.size() == 1 && old.added.size() == 1);
    CHECK((old.added.front() == TradeItems{
        {TradeItemKind::Insight, 0, 1}, {TradeItemKind::Echoes, 0, 1453},
        {TradeItemKind::Goods, BloodDregGoods, 1}}));
    CHECK(service.PartyOf(old.endpoint).isEmpty());
    service.Drain();
  }
  Service after_ack{folder.path()};
  after_ack.Load(now + 20);
  Game again{1002, "rr", 801};
  again.invasion_rewards = again.covenant_rewards = true;
  again.LobbyHello(true, now + 20);
  Pump(after_ack, {&again}, now + 20);
  CHECK(again.grants.empty() && again.added.empty());
  return 0;
}

int RewardPublicationAndAckAreDurableWithoutDrain() {
  QTemporaryDir folder;
  CHECK(folder.isValid());
  const QString mailbox = QDir{folder.path()}.filePath(
      BloodborneWorld::PartyFolderName(QByteArray{"invasion-rewards:"} + QByteArray{"rr"}.toHex()));
  bool durable_at_delivery = false;
  qint64 now = 0;
  {
    InvasionRig rig{true, true, folder.path()};
    rig.EnableCovenants(0, CorruptionCovenantEffect);
    rig.red.ack_trades = false;
    rig.red.on_grant = [&](const bbcoop::TradeGrant& grant) {
      const auto stored = BloodborneWorld::WorldStore{mailbox}.LoadTrades();
      durable_at_delivery = stored && stored->next_id() == 2 && stored->grants_size() == 1 &&
          stored->grants(0).grant().SerializeAsString() == grant.SerializeAsString();
    };
    CHECK(rig.Invade());
    rig.a.Vitals(InvadedMap, 3, rig.now, 50, false, true);
    rig.Step();
    CHECK(rig.red.grants.size() == 1 && durable_at_delivery);
    CHECK(!QFile::exists(QDir{mailbox}.filePath("world.pb"))); // trades-only mailbox
    now = rig.now + 100;
  }
  // A crash after persistence and before an application ACK resumes the same grant/counter,
  // without requiring unrelated world metadata to have survived.
  Service restarted{folder.path()};
  restarted.Load(now);
  Game red{1002, "rr", 901};
  red.invasion_rewards = red.covenant_rewards = true;
  red.LobbyHello(true, now);
  Pump(restarted, {&red}, now);
  CHECK(red.grants.size() == 1 && red.added.size() == 1);
  CHECK(red.grants.front().id() == (InvasionRewardGrantBit | 1));
  const auto acked = BloodborneWorld::WorldStore{mailbox}.LoadTrades();
  CHECK(acked && acked->next_id() == 2 && acked->grants_size() == 0); // no Drain
  return 0;
}

int FailedRewardWritesPublishNothingAndPreserveAckState() {
  QTemporaryDir folder;
  CHECK(folder.isValid());
  const QString mailbox = QDir{folder.path()}.filePath(
      BloodborneWorld::PartyFolderName(QByteArray{"invasion-rewards:"} + QByteArray{"rr"}.toHex()));
  BloodborneWorld::WorldStore store{mailbox};
  TradeDesk initial;
  CHECK(store.SaveTrades(initial.Stored()));
  InvasionRig rig{true, true, folder.path()};
  rig.EnableCovenants(0, CorruptionCovenantEffect); // loads the valid empty mailbox
  rig.red.ack_trades = false;
  CHECK(rig.Invade());
  const QString ledger = QDir{mailbox}.filePath("trades.pb");
  CHECK(QFile::remove(ledger));
  CHECK(QDir{}.mkpath(ledger)); // atomic replacement cannot replace a directory
  rig.a.Vitals(InvadedMap, 3, rig.now, 50, false, true);
  rig.Step();
  CHECK(rig.red.grants.empty() && !EndedFor(rig.red, bbcoop::InvasionEnd::TARGET_KILLED));
  CHECK(!rig.service.VisitingOf(rig.red.endpoint).isEmpty());
  CHECK(QDir{}.rmdir(ledger));
  rig.a.Vitals(InvadedMap, 3, rig.now + 100, 50, false, true);
  rig.Step(100);
  CHECK(rig.red.grants.empty()); // bounded failure backoff, even after the disk recovers
  rig.a.Vitals(InvadedMap, 3, rig.now + 1000, 50, false, true);
  rig.Step(1000);
  CHECK(rig.red.grants.size() == 1 && EndedFor(rig.red, bbcoop::InvasionEnd::TARGET_KILLED));
  CHECK(store.LoadTrades()->next_id() == 2); // failed attempt did not publish/reuse an observed id
  const QString saved = ledger + ".saved";
  CHECK(QFile::rename(ledger, saved));
  CHECK(QDir{}.mkpath(ledger));
  bbcoop::Envelope ack;
  ack.mutable_trade_granted()->set_id(InvasionRewardGrantBit | 1);
  rig.red.Send(ack, rig.now);
  rig.Step(); // failed ACK persistence must leave the confirmed in-memory grant pending
  CHECK(QDir{}.rmdir(ledger));
  CHECK(QFile::rename(saved, ledger));
  rig.red.invasion_rewards = false;
  rig.red.LobbyHello(true, rig.now + 1000);
  rig.Step(1000);
  rig.red.invasion_rewards = true;
  rig.red.ack_trades = true;
  rig.red.LobbyHello(true, rig.now);
  rig.Step();
  CHECK(rig.red.grants.size() == 2); // replay proves failed ACK did not erase current memory
  CHECK(rig.red.added.size() == 1); // duplicate application is suppressed
  const auto complete = store.LoadTrades();
  CHECK(complete && complete->next_id() == 2 && complete->grants_size() == 0);
  return 0;
}

int CorruptRewardLedgerNeverResetsItsCounter() {
  QTemporaryDir folder;
  CHECK(folder.isValid());
  const QString mailbox = QDir{folder.path()}.filePath(
      BloodborneWorld::PartyFolderName(QByteArray{"invasion-rewards:"} + QByteArray{"rr"}.toHex()));
  CHECK(QDir{}.mkpath(mailbox));
  QFile ledger{QDir{mailbox}.filePath("trades.pb")};
  CHECK(ledger.open(QIODevice::WriteOnly));
  const QByteArray corrupt = "not-a-protobuf-ledger";
  CHECK(ledger.write(corrupt) == corrupt.size());
  ledger.close();
  InvasionRig rig{true, true, folder.path()};
  rig.EnableCovenants(0, CorruptionCovenantEffect);
  CHECK(rig.Invade());
  rig.a.Vitals(InvadedMap, 3, rig.now, 50, false, true);
  rig.Step();
  CHECK(rig.red.grants.empty());
  CHECK(ledger.open(QIODevice::ReadOnly));
  CHECK(ledger.readAll() == corrupt); // never overwritten with a fresh next_id=1
  ledger.close();
  CHECK(ledger.open(QIODevice::WriteOnly | QIODevice::Truncate));
  ledger.close(); // an empty protobuf parses, but its absent/zero counter is not a ledger
  rig.a.Vitals(InvadedMap, 3, rig.now + 1000, 50, false, true);
  rig.Step(1000);
  CHECK(rig.red.grants.empty() && ledger.size() == 0);
  return 0;
}

int AnInvasionLivesWithTheCapsAndTheCollector() {
  {
    BloodborneWorld::Limits limits;
    limits.message_bytes = 64 * 1024;
    InvasionRig rig{true, true, {}, limits};
    CHECK(rig.Invade());
    bbcoop::Envelope big;
    big.mutable_portrait()->set_pixels(std::string(100'000, 'p'));
    rig.red.Send(big, rig.now);
    rig.Step();
    CHECK(EndedFor(rig.a, bbcoop::InvasionEnd::INVADER_LEFT) &&
          EndedFor(rig.b, bbcoop::InvasionEnd::INVADER_LEFT));
    CHECK(!rig.Routes(rig.red, rig.a, 0x06));
  }
  QTemporaryDir folder;
  CHECK(folder.isValid());
  BloodborneWorld::Limits limits;
  limits.idle_world_ms = 1000;
  InvasionRig rig{true, true, folder.path(), limits};
  const int parties = rig.service.Parties();
  CHECK(rig.Invade());
  rig.Step(5000);
  CHECK(rig.service.Parties() == parties);
  CHECK(rig.service.World(rig.party) != nullptr);
  CHECK(rig.service.PartyOf(rig.red.endpoint).isEmpty());
  CHECK(rig.red.ends.empty() && rig.Routes(rig.red, rig.a, 0x06));
  return 0;
}

// Hosted: a friend the lobby does not list joins by the team's code (typed in
// its game); wrong codes are counted on the service's clock.
int HostedFriendsJoinByTheTeamsCode() {
  Service service{{}};
  BloodborneWorld::HostedRules rules;
  rules.enabled = true;
  rules.lobby_players = 0; // nobody listed: only the code finds the team
  service.SetHosted(rules);
  const auto hex = [](const char *id) {
    return QByteArray(id).toHex().toStdString();
  };
  Game leader{1000, hex("Kaniel"), 11};
  leader.online_id = "Kaniel";
  Game friend_{1001, hex("Ada"), 12};
  friend_.online_id = "Ada";
  const std::vector<Game *> games{&leader, &friend_};
  qint64 now = 1'000'000;
  const auto step = [&] {
    now += 300; // past the lobby's interval
    Pump(service, games, now);
  };
  leader.LobbyHello(false, now);
  friend_.LobbyHello(false, now);
  step();
  CHECK(leader.lobby.players_size() == 0);
  leader.Team(bbcoop::TeamRequest::CREATE, now);
  step();
  const std::string code = leader.lobby.team().code();
  CHECK(code.size() == 6);
  const auto join = [&](const std::string &typed) {
    bbcoop::Envelope envelope;
    envelope.mutable_team_request()->set_action(bbcoop::TeamRequest::JOIN_CODE);
    envelope.mutable_team_request()->set_code(typed);
    friend_.Send(envelope, now);
    step();
  };
  for (size_t n = 0; n < TeamLobby::LookupTries; ++n) {
    join(code == "222222" ? "333333" : "222222");
    CHECK(friend_.lobby.notice() == "no team has that code");
  }
  join(code);
  CHECK(friend_.lobby.notice().find("too many") != std::string::npos);
  CHECK(!friend_.lobby.has_team());
  now += TeamLobby::LookupWindowMs;
  join(code);
  CHECK(friend_.lobby.has_team() &&
        friend_.lobby.team().id() == leader.lobby.team().id());
  CHECK(friend_.lobby.team().code().empty()); // the leader's to hand out
  CHECK(leader.lobby.team().members_size() == 2);
  return 0;
}

// The operator's player list (admin.h): where each linked game is, by its
// link: its lobby name, its team, the world it plays and its map.
int APlayersPlaceIsKnownByItsLink() {
  Service service{{}};
  Game a{1000, "aa", 11};
  a.claims_host = true;
  qint64 now = 1'000'000;
  CHECK(!service.PlaceOf(a.endpoint));
  a.LobbyHello(false, now);
  Pump(service, {&a}, now);
  a.Team(bbcoop::TeamRequest::CREATE, now);
  a.Team(bbcoop::TeamRequest::READY, now, {}, {}, 1);
  a.Team(bbcoop::TeamRequest::START, now);
  Pump(service, {&a}, now);
  const bbcoop::TeamStart go = a.starts.at(0);
  a.Hello(now, QByteArray::fromStdString(go.party()));
  a.Map(24, now);
  Pump(service, {&a}, now);
  const auto place = service.PlaceOf(a.endpoint);
  CHECK(place && place->lobby == "aa");
  CHECK(!place->team.empty() && place->world == go.world());
  CHECK(place->map == 24);
  service.Forget(a.endpoint);
  CHECK(!service.PlaceOf(a.endpoint));
  return 0;
}

// Hosted: a world its last player forgot goes aside with the line that names
// that player; restored, it is listed to it again (the one who forgot it
// before, for itself, does not get it back).
int AForgottenHostedWorldIsRestoredToItsPlayer() {
  QTemporaryDir dir;
  CHECK(dir.isValid());
  Service service{dir.path()};
  BloodborneWorld::HostedRules rules;
  rules.enabled = true;
  service.SetHosted(rules);
  qint64 now = 1'000'000;
  service.Load(now);
  const auto hex = [](const char *id) {
    return QByteArray(id).toHex().toStdString();
  };
  Game leader{1000, hex("Kaniel"), 11};
  leader.online_id = "Kaniel";
  leader.claims_host = true;
  Game guest{1001, hex("Ada"), 12};
  guest.online_id = "Ada";
  const std::vector<Game *> games{&leader, &guest};
  const auto step = [&] {
    now += 300;
    Pump(service, games, now);
  };
  leader.LobbyHello(false, now);
  guest.LobbyHello(false, now);
  step();
  leader.Team(bbcoop::TeamRequest::CREATE, now);
  step();
  leader.Team(bbcoop::TeamRequest::INVITE, now, guest.name);
  step();
  CHECK(guest.lobby.invites_size() == 1);
  guest.Team(bbcoop::TeamRequest::ACCEPT, now, {},
             guest.lobby.invites(0).team());
  step();
  leader.Team(bbcoop::TeamRequest::READY, now, {}, {}, 0, "Hunter");
  guest.Team(bbcoop::TeamRequest::READY, now, {}, {}, 1, "Doll");
  step();
  leader.Team(bbcoop::TeamRequest::START, now);
  step();
  CHECK(!leader.starts.empty());
  const std::string world = leader.starts.back().world();
  guest.Team(bbcoop::TeamRequest::LEAVE, now);
  leader.Team(bbcoop::TeamRequest::LEAVE, now);
  step();
  bbcoop::Envelope forget;
  forget.mutable_team_request()->set_action(bbcoop::TeamRequest::FORGET_WORLD);
  forget.mutable_team_request()->set_world(world);
  leader.Send(forget, now);
  step();
  guest.Send(forget, now);
  step();
  service.Drain();
  CHECK(service.Lobby().Worlds().empty());
  const auto aside = service.AsideRows();
  CHECK(aside.size() == 1 && aside[0].info);
  QString said;
  BloodborneWorld::Service::Result result;
  CHECK(service.Restore(aside[0].folder, now, said, result));
  CHECK(said == QString::fromStdString(world));
  CHECK(service.Lobby().StateFor(guest.name).worlds_size() == 1);
  CHECK(service.Lobby().StateFor(leader.name).worlds_size() == 0);
  return 0;
}

// What a collected world keeps for its next read (where its guests stood) is
// dropped once old, and only so many are kept: worlds that never come back do
// not stay in memory for ever.
int WhatCollectedWorldsKeepIsBounded() {
  QTemporaryDir folder;
  CHECK(folder.isValid());
  Service service{folder.path()};
  BloodborneWorld::Limits limits;
  limits.idle_world_ms = 60'000;
  limits.kept_ms = 3'600'000;
  limits.kept_worlds = 2;
  service.SetLimits(limits);
  qint64 now = 1'000'000;
  service.Load(now);
  for (int i = 0; i < 3; ++i) {
    Game host{static_cast<quint32>(1000 + 2 * i), "host",
              static_cast<quint32>(11 + i)};
    Game guest{static_cast<quint32>(1001 + 2 * i), "guest",
               static_cast<quint32>(31 + i)};
    const QByteArray party = "p:" + QByteArray::number(i);
    host.Hello(now, party);
    guest.Hello(now, party);
    Pump(service, {&host, &guest}, now);
    bbcoop::Envelope vitals = VitalsOf(0x18010000, 1);
    vitals.mutable_vitals()->mutable_at()->set_x(-215.5f);
    guest.Send(vitals, now + 10);
    Pump(service, {&host, &guest}, now + 10);
    service.Forget(host.endpoint);
    service.Forget(guest.endpoint);
    service.Tick(now + 1000);
    now += 70'000;
    service.Tick(now);
  }
  CHECK(service.Parties() == 0);
  CHECK(service.KeptWorlds() == 2); // the oldest went
  service.Tick(now + 3'600'000 + 1);
  CHECK(service.KeptWorlds() == 0);
  return 0;
}

int main() {
  const std::vector<std::pair<const char *, std::function<int()>>> tests{
      {"FriendsAreFoundAskedAndAnsweredInTheLobby",
       FriendsAreFoundAskedAndAnsweredInTheLobby},
      {"MembersFollowTheServersLog", MembersFollowTheServersLog},
      {"HostedPartiesAreTheirTeamsOnly", HostedPartiesAreTheirTeamsOnly},
      {"APlayersPlaceIsKnownByItsLink", APlayersPlaceIsKnownByItsLink},
      {"AMessagePastTheLimitDropsTheStream",
       AMessagePastTheLimitDropsTheStream},
      {"AMemberThatNeverAcksIsDropped", AMemberThatNeverAcksIsDropped},
      {"AMessagePastAFieldsLimitIsDropped", AMessagePastAFieldsLimitIsDropped},
      {"AnEmptyPartyWorldIsCollected", AnEmptyPartyWorldIsCollected},
      {"TheWorldsInMemoryAreCapped", TheWorldsInMemoryAreCapped},
      {"ACollectedWorldKeepsItsPlaces", ACollectedWorldKeepsItsPlaces},
      {"AtTheCapOnlyAFewWorldsAreCollected",
       AtTheCapOnlyAFewWorldsAreCollected},
      {"ARestartedGameHearsItsLobby", ARestartedGameHearsItsLobby},
      {"HostedTradesAreWithinAStartedTeam", HostedTradesAreWithinAStartedTeam},
      {"HostedFriendsJoinByTheTeamsCode", HostedFriendsJoinByTheTeamsCode},
      {"ATeamEndsWhenEveryMembersGameIsGone",
       ATeamEndsWhenEveryMembersGameIsGone},
      {"TheWorldsOfTheOldTeamsAreOnTheList",
       TheWorldsOfTheOldTeamsAreOnTheList},
      {"AForgottenWorldsFolderIsPutAside", AForgottenWorldsFolderIsPutAside},
      {"AWorldsLampsAreReset", AWorldsLampsAreReset},
      {"EachLinksPartyIsKnownAfterItsHello",
       EachLinksPartyIsKnownAfterItsHello},
      {"AMembersNextRunResumesWhereItWas", AMembersNextRunResumesWhereItWas},
      {"TheResumePointIsTheCharactersOwn", TheResumePointIsTheCharactersOwn},
      {"TheSessionHostMovesToAGuest", TheSessionHostMovesToAGuest},
      {"AReturningLeaderStartsAsAGuestOfTheNewHost",
       AReturningLeaderStartsAsAGuestOfTheNewHost},
      {"AHostBackAtTheTitleHandsTheTeamToTheNewHost",
       AHostBackAtTheTitleHandsTheTeamToTheNewHost},
      {"AHostsGameGoneHandsTheTeamOver", AHostsGameGoneHandsTheTeamOver},
      {"OneSessionHostWhenTwoGuestsTakeOver",
       OneSessionHostWhenTwoGuestsTakeOver},
      {"AWrongPartyOrProtocolIsRefused", AWrongPartyOrProtocolIsRefused},
      {"AWipeStartsOneLogForTheParty", AWipeStartsOneLogForTheParty},
      {"TheHostsSessionEventsStartANewLogAGuestsTakeItsShare",
       TheHostsSessionEventsStartANewLogAGuestsTakeItsShare},
      {"TheWorldStandsThroughARestartOfTheServer",
       TheWorldStandsThroughARestartOfTheServer},
      {"AJournalCutShortLoadsUpToItsLastRecord",
       AJournalCutShortLoadsUpToItsLastRecord},
      {"ABrokenWorldFileFallsBackToABackup",
       ABrokenWorldFileFallsBackToABackup},
      {"AJournalsBrokenTailIsNotWrittenAfter",
       AJournalsBrokenTailIsNotWrittenAfter},
      {"TheServerRestartingMidSessionLosesNothing",
       TheServerRestartingMidSessionLosesNothing},
      {"AWorldKeepsADatedCopyADayForAWeek", AWorldKeepsADatedCopyADayForAWeek},
      {"ALostDatagramIsSentAgain", ALostDatagramIsSentAgain},
      {"TheHostsWorldAndEveryonesFlagsAndHpReachEveryMember",
       TheHostsWorldAndEveryonesFlagsAndHpReachEveryMember},
      {"TheBoardsGoToEveryMember", TheBoardsGoToEveryMember},
      {"ThePartyIsWipedWhenNobodyOnTheMapStands",
       ThePartyIsWipedWhenNobodyOnTheMapStands},
      {"ASpectatorIsNoOneTheServerWipes", ASpectatorIsNoOneTheServerWipes},
      {"AMembersOwnWipeKeepsThePartysLog", AMembersOwnWipeKeepsThePartysLog},
      {"AWipeOfOneMapStartsThatMapOverAlone",
       AWipeOfOneMapStartsThatMapOverAlone},
      {"AMemberUpAgainAfterDyingFirstDoesNotStopTheWipe",
       AMemberUpAgainAfterDyingFirstDoesNotStopTheWipe},
      {"APortraitGoesToTheOthersAndToLaterMembers",
       APortraitGoesToTheOthersAndToLaterMembers},
      {"WhateverAGameSendsTheServiceKeepsWorking",
       WhateverAGameSendsTheServiceKeepsWorking},
      {"TheHostsSessionReachesItsGuestsToJoin",
       TheHostsSessionReachesItsGuestsToJoin},
      {"AHostsSessionGoesWithItsGame", AHostsSessionGoesWithItsGame},
      {"EachMembersLoadReachesTheOthers", EachMembersLoadReachesTheOthers},
      {"AFollowersHpIsNeverTheLogs", AFollowersHpIsNeverTheLogs},
      {"AHitReachesTheOthersAtOnce", AHitReachesTheOthersAtOnce},
      {"AnAltarRecordReachesTheOthersWhole",
       AnAltarRecordReachesTheOthersWhole},
      {"AChaliceTakenOffStartsTheDungeonsOver",
       AChaliceTakenOffStartsTheDungeonsOver},
      {"APingReachesTheTeamAtOnce", APingReachesTheTeamAtOnce},
      {"ATeamTripReachesThePartyAtOnce", ATeamTripReachesThePartyAtOnce},
      {"ADuelIsTheServers", ADuelIsTheServers},
      {"TheFirstMemberOnAnAreaRunsItsEnemies",
       TheFirstMemberOnAnAreaRunsItsEnemies},
      {"GamePacketsGoByMapAndOwner", GamePacketsGoByMapAndOwner},
      {"ARemovedMemberIsOutOfTheTeamsWorld",
       ARemovedMemberIsOutOfTheTeamsWorld},
      {"ARemovedMembersGoodbyeEndsAfterTheGrace",
       ARemovedMembersGoodbyeEndsAfterTheGrace},
      {"PublicLobbyIdentityComesFromTheAuthenticatedAccount",
       PublicLobbyIdentityComesFromTheAuthenticatedAccount},
      {"HostedSearchListsThePlayersOnline", HostedSearchListsThePlayersOnline},
      {"ACutMembersGoodbyeIsForTheHostOnly",
       ACutMembersGoodbyeIsForTheHostOnly},
      {"OnlyTheOwnersKillsAndHpCount", OnlyTheOwnersKillsAndHpCount},
      {"ATeamFormsAndStartsThroughTheService",
       ATeamFormsAndStartsThroughTheService},
      {"ATradeGoesThroughTheServerAndOutlivesItsRestart",
       ATradeGoesThroughTheServerAndOutlivesItsRestart},
      {"ATeamsWorldComesFromItsHostAndOutlivesTheServer",
       ATeamsWorldComesFromItsHostAndOutlivesTheServer},
      {"ATeamsWorldKeepsBackups", ATeamsWorldKeepsBackups},
      {"ATeamsWorldsAreKeptApart", ATeamsWorldsAreKeptApart},
      {"AnInvaderJoinsATeamWorldAfterANotice",
       AnInvaderJoinsATeamWorldAfterANotice},
      {"AnInvasionNeedsTheServerAndTheTeamToAllowIt",
       AnInvasionNeedsTheServerAndTheTeamToAllowIt},
      {"AnInvadersPacketsGoByTheHostileTable",
       AnInvadersPacketsGoByTheHostileTable},
      {"AnInvaderNeverChangesTheTeamsWorld",
       AnInvaderNeverChangesTheTeamsWorld},
      {"EachEndTellsEveryoneAndRestoresTheRouting",
       EachEndTellsEveryoneAndRestoresTheRouting},
      {"VictoryRewardsUseSoloMailboxesAndSurviveRestart",
       VictoryRewardsUseSoloMailboxesAndSurviveRestart},
      {"CovenantRewardsFollowFrozenNativeOaths", CovenantRewardsFollowFrozenNativeOaths},
      {"ExtendedCovenantGrantsWaitForACapableReconnect", ExtendedCovenantGrantsWaitForACapableReconnect},
      {"RewardPublicationAndAckAreDurableWithoutDrain", RewardPublicationAndAckAreDurableWithoutDrain},
      {"FailedRewardWritesPublishNothingAndPreserveAckState", FailedRewardWritesPublishNothingAndPreserveAckState},
      {"CorruptRewardLedgerNeverResetsItsCounter", CorruptRewardLedgerNeverResetsItsCounter},
      {"AnInvasionLivesWithTheCapsAndTheCollector",
       AnInvasionLivesWithTheCapsAndTheCollector},
      {"AForgottenHostedWorldIsRestoredToItsPlayer",
       AForgottenHostedWorldIsRestoredToItsPlayer},
      {"WhatCollectedWorldsKeepIsBounded", WhatCollectedWorldsKeepIsBounded},
  };
  int failed = 0;
  for (const auto &[name, test] : tests) {
    const int result = test();
    std::cout << (result == 0 ? "PASS " : "FAIL ") << name << '\n';
    failed += result != 0 ? 1 : 0;
  }
  return failed == 0 ? 0 : 1;
}
