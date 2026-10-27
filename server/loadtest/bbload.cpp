// SPDX-FileCopyrightText: Copyright 2026 shadNet Project
// SPDX-License-Identifier: GPL-2.0-or-later
// bbload: simulated players for a load test of shadNet (docs/HOSTED_SERVER.md).
// Each simulated game speaks the real link (server_link.h) from its own UDP
// port: the hello that signs it in, a ping a second, the team lobby (create,
// invite, accept, ready, start) and the party hello over its Coop stream
// (coop/coop_stream.h), then plays: vitals, kill and flag proposals, a map
// change now and then, and game packets to its teammates at the rates the rig
// measured (2026-10-15: the enemy owner ~87 packets a second per teammate, a
// follower ~8).
//
// Every game packet carries the time it was sent and a sequence number, so the
// receiving game measures the relay's delay and the packets lost. A summary
// line every few seconds, a JSON line at the end (--json).
//
// A region of shards (docs/SCALING.md, --shards): each team's games start on
// different shards; the members join their leader by its team's code or its
// invite by name, and follow the Redirect to the leader's shard as the game
// does (a new link there, the ticket in its hellos).
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <map>
#include <memory>
#include <random>
#include <string>
#include <tuple>
#include <vector>

#include <QCommandLineParser>
#include <QCoreApplication>
#include <QHostAddress>
#include <QJsonDocument>
#include <QJsonObject>
#include <QThreadPool>
#include <QTimer>
#include <QUdpSocket>

#include "coop/coop_messages.h"
#include "coop/coop_stream.h"
#include "coop/game_link.h"
#include "server_link.h"

using namespace BloodborneCoop;

namespace {

s64 NowUs() {
  return std::chrono::duration_cast<std::chrono::microseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

// A new ephemeral key for a hello's handshake.
LinkAuth::Key FreshKey() {
  thread_local std::random_device device;
  LinkAuth::Key key;
  for (u8 &byte : key) {
    byte = static_cast<u8>(device());
  }
  return key;
}

struct Settings {
  QHostAddress server{QHostAddress::LocalHost};
  quint16 port = 31314;
  int teams = 10;
  int team_size = 3;
  int seconds = 60;
  int ramp_ms = 50;     // between two games' first hellos
  int owner_fps = 30;   // the owner's frames a second to each teammate
  int follower_fps = 8; // a follower's
  QByteArray tag =
      "lt"; // the start of every online id (one per bbload process)
  QByteArray build = "bbload";
  double map_change_per_min = 0.5; // a game's map changes a minute
  bool json = false;
  bool spread = true; // each game from its own loopback address (127.0.x.y), as
                      // from its home
  int same_address =
      0; // >0: this many games share each address (an attacker's flood)
  // A region: the shards' link addresses; each team's games start spread over
  // them. `cross`: how a member on another shard joins its leader (code,
  // invite: by name, mixed: every other team each).
  std::vector<std::pair<QHostAddress, quint16>> shards;
  std::string cross = "mixed";
};

constexpr u32 MapA = 0x18010000; // Central Yharnam
constexpr u32 MapB = 0x18000000; // Cathedral Ward (loaded with it)
constexpr u32 MapC = 0x15010000; // elsewhere

// One sample set: kept whole (a run has at most a few million).
struct Samples {
  std::vector<u32> values;
  void Add(s64 value) {
    values.push_back(static_cast<u32>(std::clamp<s64>(value, 0, 0xFFFFFFFF)));
  }
  u32 Percentile(double p) {
    if (values.empty()) {
      return 0;
    }
    const size_t at =
        std::min(values.size() - 1, static_cast<size_t>(p * values.size()));
    std::nth_element(values.begin(),
                     values.begin() + static_cast<std::ptrdiff_t>(at),
                     values.end());
    return values[at];
  }
  void Clear() { values.clear(); }
};

struct Totals {
  quint64 packets_sent = 0;
  quint64 packets_received = 0;
  quint64 packets_lost = 0; // gaps in the sequence numbers
  quint64 bytes_sent = 0;
  quint64 bytes_received = 0;
  quint64 datagrams_sent = 0;
  quint64 datagrams_received = 0;
  quint64 refused = 0;
  quint64 untagged = 0; // datagrams from the server that did not open
  Samples relay_us;     // a game packet's time from its game to its teammate's
  Samples ping_us;      // ping -> pong
  Samples welcome_ms;   // first hello -> welcomed
  Samples work_ms;      // hosted: a new account's work asked -> done
  Samples solve_ms;     // its solving alone (what one core of a game spends)
  Samples
      playing_ms; // first hello -> the party's welcome (team made and started)
  quint64 redirects = 0;
  Samples redirect_ms; // a Redirect -> the welcome of the shard it named
};

class Load;

class Game {
public:
  Game(Load &load, int index, int team, int seat);
  void Start();
  void Tick(s64 now_us);
  bool Playing() const { return state == State::Playing; }
  bool Linked() const { return state != State::Linking; }
  std::string name; // its online id
  std::string
      member; // its board name: the online id in hex, as the games say it

private:
  enum class State { Linking, Lobby, Started, Playing };
  void OnReadyRead();
  void Send(const QByteArray &datagram);
  void SendHello();
  void SendCoop(const bbcoop::Envelope &envelope, s64 now_ms);
  void FlushCoop(s64 now_ms);
  void Take(const bbcoop::Envelope &envelope, s64 now_us);
  void SendGamePackets(s64 now_us);
  void TakeData(const QByteArray &frame, s64 now_us);
  void StartWork(int bits, s64 now_us);
  void Redirected(const ServerLink::Redirect &redirect, s64 now_us);
  bool Leader() const { return seat == 0; }
  // A region: whether this member joins its leader on another shard, and how.
  bool Elsewhere() const;
  bool ByCode() const;

  Load &load;
  int index;
  int team;
  int seat;
  QUdpSocket socket;
  CoopStream stream;
  State state = State::Linking;
  u32 run;
  s64 started_us = 0;
  s64 next_hello_us = 0;
  s64 next_ping_us = 0;
  s64 next_vitals_us = 0;
  s64 next_kills_us = 0;
  s64 next_frame_us = 0;
  s64 next_lobby_us = 0;
  s64 next_map_us = 0;
  s64 next_map_report_us =
      0; // the game says its map every second (EnemyOwners::SilentMs)
  u32 ping_seq = 0;
  u32 map = MapA;
  u32 kills_number = 0;
  u64 epoch = 0;
  std::string team_id;
  bool invited = false;
  int team_members = 0; // in its team, as its last lobby state said
  bool readied = false;
  bool started = false;
  std::string party;
  std::map<std::string, u32> sent_seq;  // per teammate
  std::map<std::string, u32> heard_seq; // per teammate: the last packet's
  u32 frame_count = 0;
  std::mt19937 random;
  // Link version 4 (coop/link_auth.h): the cookie round trip, the handshake
  // and the encryption (the server's key taken on first use).
  LinkAuth::Client auth;
  // The shard its link is on, the ticket of the Redirect that sent it there.
  QHostAddress server;
  quint16 port = 0;
  int first_shard = 0;
  QByteArray ticket;
  int redirects = 0;
  s64 redirected_us = 0;
  s64 next_join_us = 0;
};

class Load {
public:
  explicit Load(Settings settings) : settings{std::move(settings)} {}

  void Run() {
    for (int team = 0; team < settings.teams; ++team) {
      for (int seat = 0; seat < settings.team_size; ++seat) {
        games.push_back(std::make_unique<Game>(
            *this, static_cast<int>(games.size()), team, seat));
      }
    }
    started_us = NowUs();
    for (size_t at = 0; at < games.size(); ++at) {
      QTimer::singleShot(static_cast<int>(at) * settings.ramp_ms,
                         [game = games[at].get()] { game->Start(); });
    }
    connect_timer();
    QTimer::singleShot((settings.seconds) * 1000 +
                           static_cast<int>(games.size()) * settings.ramp_ms,
                       [this] { Finish(); });
  }

  std::string NameOf(int team, int seat) const {
    return settings.tag.toStdString() + "-t" + std::to_string(team) + "s" +
           std::to_string(seat);
  }
  ServerLink::Endpoint VirtualOf(const std::string &name) const {
    return ServerLink::VirtualEndpointOf(QByteArray::fromStdString(name));
  }
  // A region: the shard a team's seat starts on (the leader's spread over the
  // shards, its members on the others).
  int ShardOf(int team, int seat) const {
    const int count = static_cast<int>(settings.shards.size());
    return count == 0 ? 0 : (team + seat) % count;
  }
  // The leaders' team codes, as their lobby said them, and since when.
  std::map<int, std::pair<std::string, s64>> codes;

  Settings settings;
  Totals totals;
  s64 started_us = 0;
  int max_work_bits = 0;

private:
  void connect_timer() {
    auto *timer = new QTimer();
    timer->setTimerType(Qt::PreciseTimer);
    QObject::connect(timer, &QTimer::timeout, [this] {
      const s64 now = NowUs();
      for (auto &game : games) {
        game->Tick(now);
      }
      if (now - last_report_us >= 5'000'000) {
        Report(now);
      }
    });
    timer->start(5);
  }

  void Report(s64 now_us) {
    int linked = 0;
    int playing = 0;
    for (auto &game : games) {
      linked += game->Linked();
      playing += game->Playing();
    }
    const double seconds = (now_us - last_report_us) / 1e6;
    const quint64 sent = totals.packets_sent - last_sent;
    const quint64 received = totals.packets_received - last_received;
    std::fprintf(stderr,
                 "[%5.0fs] games %zu linked %d playing %d | packets out %.0f/s "
                 "in %.0f/s "
                 "lost %llu | relay p50 %.2f ms p99 %.2f ms max %.2f ms | ping "
                 "p50 %.2f ms "
                 "p99 %.2f ms\n",
                 (now_us - started_us) / 1e6, games.size(), linked, playing,
                 sent / seconds, received / seconds,
                 static_cast<unsigned long long>(totals.packets_lost),
                 window_relay.Percentile(0.5) / 1000.0,
                 window_relay.Percentile(0.99) / 1000.0,
                 window_relay.Percentile(1.0) / 1000.0,
                 window_ping.Percentile(0.5) / 1000.0,
                 window_ping.Percentile(0.99) / 1000.0);
    window_relay.Clear();
    window_ping.Clear();
    last_sent = totals.packets_sent;
    last_received = totals.packets_received;
    last_report_us = now_us;
  }

  void Finish() {
    Report(NowUs());
    int linked = 0;
    int playing = 0;
    for (auto &game : games) {
      linked += game->Linked();
      playing += game->Playing();
    }
    const double seconds = (NowUs() - started_us) / 1e6;
    QJsonObject out{
        {"games", static_cast<int>(games.size())},
        {"linked", linked},
        {"playing", playing},
        {"seconds", seconds},
        {"packets_sent", static_cast<double>(totals.packets_sent)},
        {"packets_received", static_cast<double>(totals.packets_received)},
        {"packets_lost", static_cast<double>(totals.packets_lost)},
        {"datagrams_sent", static_cast<double>(totals.datagrams_sent)},
        {"datagrams_received", static_cast<double>(totals.datagrams_received)},
        {"bytes_sent", static_cast<double>(totals.bytes_sent)},
        {"bytes_received", static_cast<double>(totals.bytes_received)},
        {"refused", static_cast<double>(totals.refused)},
        {"untagged", static_cast<double>(totals.untagged)},
        {"relay_ms_p50", totals.relay_us.Percentile(0.5) / 1000.0},
        {"relay_ms_p95", totals.relay_us.Percentile(0.95) / 1000.0},
        {"relay_ms_p99", totals.relay_us.Percentile(0.99) / 1000.0},
        {"relay_ms_max", totals.relay_us.Percentile(1.0) / 1000.0},
        {"ping_ms_p50", totals.ping_us.Percentile(0.5) / 1000.0},
        {"ping_ms_p99", totals.ping_us.Percentile(0.99) / 1000.0},
        {"welcome_ms_p50",
         static_cast<double>(totals.welcome_ms.Percentile(0.5))},
        {"welcome_ms_p99",
         static_cast<double>(totals.welcome_ms.Percentile(0.99))},
        {"work_ms_p50", static_cast<double>(totals.work_ms.Percentile(0.5))},
        {"work_ms_p99", static_cast<double>(totals.work_ms.Percentile(0.99))},
        {"accounts_worked", static_cast<double>(totals.work_ms.values.size())},
        {"solve_ms_p50", static_cast<double>(totals.solve_ms.Percentile(0.5))},
        {"solve_ms_p99", static_cast<double>(totals.solve_ms.Percentile(0.99))},
        {"work_bits_max", max_work_bits},
        {"playing_ms_p50",
         static_cast<double>(totals.playing_ms.Percentile(0.5))},
        {"playing_ms_p99",
         static_cast<double>(totals.playing_ms.Percentile(0.99))},
        {"redirects", static_cast<double>(totals.redirects)},
        {"redirect_ms_p50",
         static_cast<double>(totals.redirect_ms.Percentile(0.5))},
        {"redirect_ms_p99",
         static_cast<double>(totals.redirect_ms.Percentile(0.99))},
    };
    if (settings.json) {
      std::printf(
          "%s\n",
          QJsonDocument(out).toJson(QJsonDocument::Compact).constData());
    }
    std::fflush(stdout);
    QCoreApplication::exit(0);
  }

public:
  Samples window_relay;
  Samples window_ping;

private:
  std::vector<std::unique_ptr<Game>> games;
  s64 last_report_us = NowUs();
  quint64 last_sent = 0;
  quint64 last_received = 0;

public:
  // Hosted: the new accounts' work, off the games' thread (each real game does
  // its own on its own PC). Destroyed before the games: it waits for its work.
  QThreadPool work_pool;
};

Game::Game(Load &load, int index, int team, int seat)
    : name{load.NameOf(team, seat)},
      member{QByteArray::fromStdString(load.NameOf(team, seat))
                 .toHex()
                 .toStdString()},
      load{load}, index{index}, team{team}, seat{seat},
      stream{"bbload", static_cast<u32>(0x10000 + index) | 1u},
      run{static_cast<u32>(0x2000 + index)},
      random{static_cast<u32>(index * 7919 + 1)} {
  QObject::connect(&socket, &QUdpSocket::readyRead, [this] { OnReadyRead(); });
  server = load.settings.server;
  port = load.settings.port;
  if (!load.settings.shards.empty()) {
    first_shard = load.ShardOf(team, seat);
    std::tie(server, port) = load.settings.shards[first_shard];
  }
}

bool Game::Elsewhere() const {
  return !Leader() && load.settings.shards.size() > 1 && redirects == 0 &&
         first_shard != load.ShardOf(team, 0);
}

bool Game::ByCode() const {
  return load.settings.cross == "code" ||
         (load.settings.cross == "mixed" && team % 2 == 0);
}

void Game::Start() {
  if (load.settings.spread && load.settings.server.isLoopback()) {
    const int group = load.settings.same_address > 0
                          ? index / load.settings.same_address
                          : index;
    const quint32 tag = static_cast<quint32>(qHash(load.settings.tag) & 0x3F);
    socket.bind(QHostAddress(0x7F000000u | (tag << 16) |
                             static_cast<quint32>(group + 2)),
                0);
  } else {
    socket.bind(QHostAddress::AnyIPv4, 0);
  }
  socket.setSocketOption(QAbstractSocket::ReceiveBufferSizeSocketOption,
                         1 << 20);
  started_us = NowUs();
  next_hello_us = started_us;
}

void Game::Send(const QByteArray &plain) {
  QByteArray datagram = plain;
  if (ServerLink::KindOf(plain) != ServerLink::Kind::Hello) {
    const auto trailer = auth.Seal({reinterpret_cast<u8 *>(datagram.data()),
                                    static_cast<size_t>(datagram.size())});
    if (!trailer) {
      return; // no session yet: the server would drop it
    }
    datagram.append(reinterpret_cast<const char *>(trailer->data()),
                    static_cast<qsizetype>(trailer->size()));
  }
  socket.writeDatagram(datagram, server, port);
  ++load.totals.datagrams_sent;
  load.totals.bytes_sent += static_cast<quint64>(datagram.size());
}

void Game::SendCoop(const bbcoop::Envelope &envelope, s64 now_ms) {
  stream.Send(EncodeEnvelope(envelope), now_ms);
  FlushCoop(now_ms);
}

void Game::FlushCoop(s64 now_ms) {
  for (const std::vector<u8> &body : stream.Flush(now_ms)) {
    Send(ServerLink::EncodeCoop(
        QByteArray(reinterpret_cast<const char *>(body.data()),
                   static_cast<qsizetype>(body.size()))));
  }
}

void Game::Tick(s64 now_us) {
  if (started_us == 0) {
    return;
  }
  const s64 now_ms = now_us / 1000;
  if (state == State::Linking) {
    if (now_us >= next_hello_us && !auth.Working()) {
      next_hello_us = now_us + 1'000'000;
      SendHello();
    }
    return;
  }
  if (now_us >= next_ping_us) {
    next_ping_us = now_us + 1'000'000;
    Send(ServerLink::Encode(
        ServerLink::Kind::Ping,
        ServerLink::Ping{++ping_seq, static_cast<quint64>(now_us)}));
  }
  if (state == State::Lobby && now_us >= next_lobby_us) {
    // The leader makes its team and invites the others; everyone readies; the
    // leader starts once every member is ready. Requests go again every two
    // seconds until they stand.
    next_lobby_us = now_us + 2'000'000;
    bbcoop::Envelope envelope;
    bbcoop::TeamRequest &request = *envelope.mutable_team_request();
    if (Leader() && team_id.empty()) {
      request.set_action(bbcoop::TeamRequest::CREATE);
      SendCoop(envelope, now_ms);
    } else if (!Leader() && team_id.empty() && Elsewhere() && ByCode()) {
      // A region: the leader's code once the directory knows it (a heartbeat
      // or two after the leader's lobby said it).
      const auto code = load.codes.find(team);
      if (code != load.codes.end() &&
          now_us - code->second.second > 5'000'000 && now_us >= next_join_us) {
        next_join_us = now_us + 4'000'000;
        request.set_action(bbcoop::TeamRequest::JOIN_CODE);
        request.set_code(code->second.first);
        SendCoop(envelope, now_ms);
      }
    } else if (Leader() && !invited) {
      for (int other = 1; other < load.settings.team_size; ++other) {
        const bool far = load.settings.shards.size() > 1 &&
                         load.ShardOf(team, other) != load.ShardOf(team, 0);
        if (far && ByCode()) {
          continue; // it joins by the code
        }
        bbcoop::Envelope invite;
        if (far) {
          // Another shard's: by its name, as the lobby shows it.
          invite.mutable_team_request()->set_action(
              bbcoop::TeamRequest::INVITE_NAME);
          invite.mutable_team_request()->set_name(load.NameOf(team, other));
        } else {
          invite.mutable_team_request()->set_action(
              bbcoop::TeamRequest::INVITE);
          invite.mutable_team_request()->set_member(
              QByteArray::fromStdString(load.NameOf(team, other))
                  .toHex()
                  .toStdString());
        }
        SendCoop(invite, now_ms);
      }
      invited = true;
      next_lobby_us = now_us + 500'000;
    } else if (!team_id.empty() && !readied) {
      request.set_action(bbcoop::TeamRequest::READY);
      request.set_slot(0);
      request.set_character("Hunter " + std::to_string(index));
      SendCoop(envelope, now_ms);
      readied = true;
      next_lobby_us = now_us + 500'000;
    } else if (Leader() && invited && team_members < load.settings.team_size &&
               now_us - started_us >
                   (load.settings.shards.size() > 1 ? 8'000'000 : 5'000'000) &&
               random() % 3 == 0) {
      invited = false; // an invite sent before its player signed in: again
    } else if (Leader() && readied && started) {
      request.set_action(bbcoop::TeamRequest::START);
      SendCoop(envelope, now_ms);
    }
  }
  if (state == State::Playing) {
    if (now_us >= next_vitals_us) {
      next_vitals_us = now_us + 500'000;
      bbcoop::Envelope envelope;
      bbcoop::Vitals &vitals = *envelope.mutable_vitals();
      vitals.set_map(map);
      vitals.set_vital(1);
      vitals.set_hp(1000 - static_cast<s32>(random() % 400));
      vitals.set_max(1000);
      vitals.set_vials(10);
      vitals.set_bullets(20);
      vitals.mutable_at()->set_x(static_cast<float>(random() % 1000));
      vitals.mutable_at()->set_y(0);
      vitals.mutable_at()->set_z(static_cast<float>(random() % 1000));
      SendCoop(envelope, now_ms);
    }
    if (now_us >= next_kills_us) {
      next_kills_us =
          now_us + 10'000'000 + static_cast<s64>(random() % 5'000'000);
      bbcoop::Envelope envelope;
      bbcoop::KillProposal &kills = *envelope.mutable_kills();
      kills.set_epoch(epoch);
      kills.set_run(run);
      kills.set_number(++kills_number);
      auto &on_map = *kills.add_kills();
      on_map.set_map(map);
      for (u32 kill = 0; kill < std::min<u32>(kills_number, 40); ++kill) {
        on_map.add_handles(0x10014000 + kill * 3 + static_cast<u32>(seat));
      }
      SendCoop(envelope, now_ms);
    }
    if (next_map_us == 0) {
      next_map_us =
          now_us + 20'000'000 + static_cast<s64>(random() % 20'000'000);
    }
    if (now_us >= next_map_report_us) {
      next_map_report_us = now_us + 1'000'000;
      bbcoop::Envelope envelope;
      envelope.mutable_map()->set_map(map);
      SendCoop(envelope, now_ms);
    }
    if (now_us >= next_map_us) {
      next_map_us =
          now_us +
          static_cast<s64>(60e6 /
                           std::max(0.01, load.settings.map_change_per_min)) +
          static_cast<s64>(random() % 10'000'000);
      if (!Leader() && load.settings.map_change_per_min > 0 &&
          random() % 2 == 0) {
        map = map == MapA ? (random() % 2 ? MapB : MapC) : MapA;
        next_map_report_us = 0;
      }
    }
    if (now_us >= next_frame_us) {
      const int fps =
          Leader() ? load.settings.owner_fps : load.settings.follower_fps;
      next_frame_us =
          (next_frame_us == 0 ? now_us : next_frame_us) + 1'000'000 / fps;
      if (next_frame_us < now_us) {
        next_frame_us = now_us + 1'000'000 / fps; // behind: no burst
      }
      SendGamePackets(now_us);
    }
  }
  FlushCoop(now_ms);
}

// A game packet: {u32 channel, u32 size, payload}; the payload starts with the
// time it was sent and its number to this teammate, then filler to its size.
std::vector<u8> GamePacket(u32 channel, size_t size, s64 now_us, u32 seq) {
  std::vector<u8> packet;
  GameLinkDetail::Put(packet, channel);
  GameLinkDetail::Put(packet, static_cast<u32>(size));
  GameLinkDetail::Put(packet, static_cast<u64>(now_us));
  GameLinkDetail::Put(packet, seq);
  packet.resize(8 + size, 0x5A);
  return packet;
}

void Game::SendGamePackets(s64 now_us) {
  ++frame_count;
  for (int other = 0; other < load.settings.team_size; ++other) {
    if (other == seat) {
      continue;
    }
    const std::string to = load.NameOf(team, other);
    GameFrame frame;
    frame.sender = name;
    frame.instance = run;
    auto add = [&](u32 channel, size_t size) {
      frame.entries.push_back(
          {PacketClass::Unreliable, 0,
           GamePacket(channel, size, now_us, ++sent_seq[to])});
    };
    if (Leader()) {
      // The enemy owner (rig 2026-10-15): 0x31 twice a frame, 0x04 every third,
      // 0x05 every fourth, 0x06 every fifth.
      add(0x31, 48);
      add(0x31, 48);
      if (frame_count % 3 == 0) {
        add(0x04, 80);
      }
      if (frame_count % 4 == 0) {
        add(0x05, 32);
      }
      if (frame_count % 5 == 0) {
        add(0x06, 96);
      }
    } else {
      add(0x06, 96);
    }
    load.totals.packets_sent += frame.entries.size();
    const std::vector<u8> payload = EncodeGameFrame(frame);
    QByteArray body = QByteArray::fromHex("0e4a0e4a") + "SHAD";
    body.append(static_cast<char>(GamePacketsType));
    body.append(reinterpret_cast<const char *>(payload.data()),
                static_cast<qsizetype>(payload.size()));
    Send(ServerLink::Encode(ServerLink::Data{load.VirtualOf(to), body}));
  }
}

void Game::TakeData(const QByteArray &frame, s64 now_us) {
  constexpr qsizetype GameFrameAt = 9;
  if (frame.size() < GameFrameAt || frame.mid(4, 4) != "SHAD") {
    return;
  }
  const auto game_frame = DecodeGameFrame(
      {reinterpret_cast<const u8 *>(frame.constData()) + GameFrameAt,
       static_cast<size_t>(frame.size() - GameFrameAt)});
  if (!game_frame) {
    return;
  }
  for (const GameFrameEntry &entry : game_frame->entries) {
    if (entry.packet.size() < 8 + 12) {
      continue;
    }
    size_t at = 8;
    u64 sent = 0;
    u32 seq = 0;
    GameLinkDetail::Get(std::span<const u8>{entry.packet}, at, sent);
    GameLinkDetail::Get(std::span<const u8>{entry.packet}, at, seq);
    ++load.totals.packets_received;
    load.totals.relay_us.Add(now_us - static_cast<s64>(sent));
    load.window_relay.Add(now_us - static_cast<s64>(sent));
    u32 &last = heard_seq[game_frame->sender];
    if (seq > last + 1 && last != 0) {
      load.totals.packets_lost += seq - last - 1;
    }
    last = std::max(last, seq);
  }
}

void Game::SendHello() {
  const QByteArray secret = "secret-" + QByteArray::fromStdString(name);
  ServerLink::Hello hello{ServerLink::LinkVersion,
                          QByteArray::fromStdString(name),
                          load.settings.build,
                          secret,
                          {}};
  if (auth.HasCookie()) {
    hello.cookie =
        QByteArray(reinterpret_cast<const char *>(auth.CookieToEcho().data()),
                   LinkAuth::CookieBytes);
    hello.work = auth.WorkToSend();
    hello.ticket = ticket;
  }
  const QByteArray plain = ServerLink::Encode(hello);
  const auto sealed =
      auth.SealHello({reinterpret_cast<const u8 *>(plain.constData()),
                      static_cast<size_t>(plain.size())},
                     FreshKey());
  Send(QByteArray(reinterpret_cast<const char *>(sealed.data()),
                  static_cast<qsizetype>(sealed.size())));
}

// Hosted: a new account (net_link.h, as the game does it): the work on the
// pool, then the hello with it at once, under a new nonce.
void Game::StartWork(int bits, s64 now_us) {
  if (!auth.WorkAsked(bits)) {
    return;
  }
  const LinkAuth::Cookie cookie = auth.CookieToEcho();
  load.work_pool.start([this, cookie, bits, now_us, id = name] {
    const s64 began_us = NowUs();
    const auto found = LinkAuth::SolveWork(cookie, id, bits, 0, ~u64{0});
    const s64 solve_ms = (NowUs() - began_us) / 1000;
    QMetaObject::invokeMethod(
        &socket,
        [this, cookie, bits, now_us, found, solve_ms] {
          auth.TakeWork(cookie, bits, found);
          load.totals.work_ms.Add((NowUs() - now_us) / 1000);
          load.totals.solve_ms.Add(solve_ms);
          load.max_work_bits = std::max(load.max_work_bits, bits);
          next_hello_us = NowUs() + 1'000'000;
          SendHello();
        },
        Qt::QueuedConnection);
  });
}

void Game::OnReadyRead() {
  while (socket.hasPendingDatagrams()) {
    QByteArray datagram;
    datagram.resize(static_cast<qsizetype>(socket.pendingDatagramSize()));
    QHostAddress from;
    quint16 from_port = 0;
    socket.readDatagram(datagram.data(), datagram.size(), &from, &from_port);
    if (from_port != port || from != server) {
      continue; // the shard it left, saying its Redirect again
    }
    ++load.totals.datagrams_received;
    load.totals.bytes_received += static_cast<quint64>(datagram.size());
    const s64 now_us = NowUs();
    const auto kind = ServerLink::KindOf(datagram);
    if (!kind) {
      continue;
    }
    if (*kind == ServerLink::Kind::Cookie) {
      LinkAuth::Key key{};
      if (const auto cookie = ServerLink::DecodeCookie(datagram, nullptr, &key);
          cookie && state == State::Linking && auth.TakeCookie(*cookie, key)) {
        SendHello();
      }
      continue;
    }
    const auto opened =
        auth.Open({reinterpret_cast<const u8 *>(datagram.constData()),
                   static_cast<size_t>(datagram.size())});
    if (!opened) {
      ++load.totals.untagged;
      continue;
    }
    datagram = QByteArray(reinterpret_cast<const char *>(opened->data()),
                          static_cast<qsizetype>(opened->size()));
    switch (*kind) {
    case ServerLink::Kind::Work:
      if (const auto bits = ServerLink::DecodeWork(datagram);
          bits && state == State::Linking) {
        StartWork(*bits, now_us);
      }
      break;
    case ServerLink::Kind::Welcome:
      if (const auto welcome = ServerLink::DecodeWelcome(datagram)) {
        if (!welcome->accepted) {
          ++load.totals.refused;
          std::fprintf(stderr, "%s refused: %s\n", name.c_str(),
                       welcome->text.constData());
          next_hello_us = now_us + 5'000'000;
        } else if (state == State::Linking) {
          state = State::Lobby;
          if (redirects == 0) {
            load.totals.welcome_ms.Add((now_us - started_us) / 1000);
          } else {
            load.totals.redirect_ms.Add((now_us - redirected_us) / 1000);
          }
          bbcoop::Envelope envelope;
          envelope.mutable_lobby_hello()->set_protocol(CoopProtocol);
          envelope.mutable_lobby_hello()->set_member(member);
          envelope.mutable_lobby_hello()->set_name(name);
          envelope.mutable_lobby_hello()->set_playing(false);
          SendCoop(envelope, now_us / 1000);
          next_lobby_us = now_us + 300'000;
        }
      }
      break;
    case ServerLink::Kind::Redirect:
      if (const auto redirect = ServerLink::DecodeRedirect(datagram)) {
        Redirected(*redirect, now_us); // the rest is the old shard's: dropped
      }
      break;
    case ServerLink::Kind::Pong:
      if (const auto pong =
              ServerLink::DecodePing(ServerLink::Kind::Pong, datagram)) {
        load.totals.ping_us.Add(now_us - static_cast<s64>(pong->sent_ms));
        load.window_ping.Add(now_us - static_cast<s64>(pong->sent_ms));
      }
      break;
    case ServerLink::Kind::Data:
      if (const auto data = ServerLink::DecodeData(datagram)) {
        TakeData(data->frame, now_us);
      }
      break;
    case ServerLink::Kind::Coop:
      if (const auto body = ServerLink::DecodeCoop(datagram)) {
        const auto bytes =
            std::span{reinterpret_cast<const u8 *>(body->constData()),
                      static_cast<size_t>(body->size())};
        for (const std::vector<u8> &message :
             stream.Receive(bytes, now_us / 1000)) {
          if (const auto envelope = DecodeEnvelope(message)) {
            Take(*envelope, now_us);
          }
        }
        FlushCoop(now_us / 1000);
      }
      break;
    default:
      break;
    }
  }
}

// A region: the link moves to the shard the Redirect names, as the game's does
// (net_link.h): its key pinned, a new cookie and handshake, the ticket in its
// hellos, a new stream; its lobby hello again once welcomed.
void Game::Redirected(const ServerLink::Redirect &redirect, s64 now_us) {
  server = QHostAddress(redirect.to.ipv4);
  port = redirect.to.port;
  ticket = redirect.ticket;
  auth = LinkAuth::Client{};
  auth.Pin(redirect.key);
  ++redirects;
  ++load.totals.redirects;
  stream = CoopStream{
      "bbload", static_cast<u32>(0x10000 + index + 0x1000000 * redirects) | 1u};
  state = State::Linking;
  team_id.clear();
  team_members = 0;
  readied = false;
  started = false;
  invited = false;
  redirected_us = now_us;
  next_hello_us = now_us;
}

void Game::Take(const bbcoop::Envelope &envelope, s64 now_us) {
  const s64 now_ms = now_us / 1000;
  if (envelope.has_lobby_state()) {
    const bbcoop::LobbyState &lobby = envelope.lobby_state();
    if (lobby.has_team()) {
      team_id = lobby.team().id();
      team_members = lobby.team().members_size();
      if (Leader() && !lobby.team().code().empty() &&
          load.codes[team].first != lobby.team().code()) {
        load.codes[team] = {lobby.team().code(), now_us};
      }
      if (Leader()) {
        int ready = 0;
        for (const auto &member : lobby.team().members()) {
          ready += member.ready();
        }
        started = ready == load.settings.team_size;
        if (started && state == State::Lobby) {
          next_lobby_us = 0; // start now
        }
      }
    } else if (!Leader()) {
      for (const auto &invite : lobby.invites()) {
        bbcoop::Envelope accept;
        accept.mutable_team_request()->set_action(bbcoop::TeamRequest::ACCEPT);
        accept.mutable_team_request()->set_team(invite.team());
        SendCoop(accept, now_ms);
        break;
      }
    }
  } else if (envelope.has_team_start() && state == State::Lobby) {
    party = envelope.team_start().party();
    state = State::Started;
    bbcoop::Envelope hello;
    hello.mutable_hello()->set_protocol(CoopProtocol);
    hello.mutable_hello()->set_party(party);
    hello.mutable_hello()->set_member(member);
    hello.mutable_hello()->set_run(run);
    hello.mutable_hello()->set_host(Leader());
    hello.mutable_hello()->set_enemy_owner(true);
    SendCoop(hello, now_ms);
    bbcoop::Envelope playing;
    playing.mutable_lobby_hello()->set_protocol(CoopProtocol);
    playing.mutable_lobby_hello()->set_member(member);
    playing.mutable_lobby_hello()->set_name(name);
    playing.mutable_lobby_hello()->set_playing(true);
    SendCoop(playing, now_ms);
  } else if (envelope.has_welcome() && state == State::Started) {
    if (envelope.welcome().accepted()) {
      state = State::Playing;
      epoch = envelope.welcome().epoch();
      load.totals.playing_ms.Add((now_us - started_us) / 1000);
      bbcoop::Envelope where;
      where.mutable_map()->set_map(map);
      SendCoop(where, now_ms);
    } else {
      ++load.totals.refused;
      std::fprintf(stderr, "%s party refused: %s\n", name.c_str(),
                   envelope.welcome().text().c_str());
    }
  } else if (envelope.has_entries() || envelope.has_snapshot()) {
    // The log follows: only its epoch matters here.
    if (envelope.has_entries() && envelope.entries().epoch() != 0) {
      epoch = envelope.entries().epoch();
    }
  }
}

} // namespace

int main(int argc, char **argv) {
  QCoreApplication app(argc, argv);
  QCommandLineParser parser;
  parser.setApplicationDescription("Simulated players for a shadNet load test");
  parser.addHelpOption();
  parser.addOptions({
      {"server", "server address", "address", "127.0.0.1"},
      {"port", "link UDP port", "port", "31314"},
      {"teams", "teams", "n", "10"},
      {"team-size", "players a team (2-4)", "n", "3"},
      {"seconds", "seconds to play after the last game starts", "n", "60"},
      {"ramp-ms", "ms between two games' starts", "n", "50"},
      {"owner-fps", "the enemy owner's frames a second per teammate", "n",
       "30"},
      {"follower-fps", "a follower's frames a second per teammate", "n", "8"},
      {"map-changes", "map changes a minute a game", "n", "0.5"},
      {"tag", "the start of every online id", "tag", "lt"},
      {"build", "the build the hello says", "build", "bbload"},
      {"json", "a JSON summary on stdout at the end"},
      {"one-address",
       "every game from the server's loopback address (no spread)"},
      {"same-address", "games sharing each loopback address", "n", "0"},
      {"work-threads",
       "threads for the new accounts' work (hosted; 0: the cores)", "n", "0"},
      {"shards",
       "a region: the shards' link addresses (a.b.c.d:port,...); each team's "
       "games start spread over them",
       "list", ""},
      {"cross",
       "how a member on another shard joins its leader: code, invite, mixed",
       "how", "mixed"},
  });
  parser.process(app);
  Settings settings;
  settings.server = QHostAddress(parser.value("server"));
  settings.port = static_cast<quint16>(parser.value("port").toUInt());
  settings.teams = parser.value("teams").toInt();
  settings.team_size = std::clamp(parser.value("team-size").toInt(), 2, 4);
  settings.seconds = parser.value("seconds").toInt();
  settings.ramp_ms = parser.value("ramp-ms").toInt();
  settings.owner_fps = std::max(1, parser.value("owner-fps").toInt());
  settings.follower_fps = std::max(1, parser.value("follower-fps").toInt());
  settings.map_change_per_min = parser.value("map-changes").toDouble();
  settings.tag = parser.value("tag").toUtf8();
  settings.build = parser.value("build").toUtf8();
  settings.json = parser.isSet("json");
  settings.spread = !parser.isSet("one-address");
  settings.same_address = parser.value("same-address").toInt();
  for (const QString &shard :
       parser.value("shards").split(',', Qt::SkipEmptyParts)) {
    const qsizetype colon = shard.lastIndexOf(':');
    settings.shards.emplace_back(
        QHostAddress(shard.left(colon)),
        static_cast<quint16>(shard.mid(colon + 1).toUInt()));
  }
  settings.cross = parser.value("cross").toStdString();
  Load load{settings};
  if (const int threads = parser.value("work-threads").toInt(); threads > 0) {
    load.work_pool.setMaxThreadCount(threads);
  }
  load.Run();
  return app.exec();
}
