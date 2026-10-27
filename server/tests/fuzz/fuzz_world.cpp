// SPDX-FileCopyrightText: Copyright 2026 shadNet Project
// SPDX-License-Identifier: GPL-2.0-or-later
#include <array>
#include <memory>
#include <string>

#include <QByteArray>

#include "bloodborne_world.h"
#include "coop/coop_messages.h"
#include "coop/coop_stream.h"
#include "fuzz_targets.h"

namespace ShadNetFuzz {

namespace {

using namespace BloodborneCoop;

const QByteArray Ids[] = {"Ada", "Kaniel", "Eileen", "Gehrman"};

ServerLink::Endpoint EndpointOf(int member) {
  return {0x7F000001u, static_cast<quint16>(41000 + member)};
}

std::string Hex(int member) { return Ids[member].toHex().toStdString(); }

} // namespace

// A chunk's selector: bits 0-1 the member, bit 2 the bytes are a Coop
// datagram's body as is (else one message, sent in the member's reliable
// stream), bit 3 a Tick first, bit 4 the member's link forgotten first, bit 5
// its game restarted first (a new stream). The first chunk's bit 6: hosted.
void WorldMessages(std::span<const std::uint8_t> input) {
  BloodborneWorld::Service service{{}};
  if (!input.empty() && (input[0] & 0x40)) {
    BloodborneWorld::HostedRules rules;
    rules.enabled = true;
    service.SetHosted(rules);
  }
  std::array<std::unique_ptr<CoopStream>, 4> games;
  std::array<u32, 4> instances{};
  qint64 now = 1'000'000;
  service.Load(now);
  Chunks chunks{input};
  std::uint8_t selector = 0;
  std::span<const std::uint8_t> bytes;
  while (chunks.Next(selector, bytes)) {
    const int member = selector & 3;
    const ServerLink::Endpoint from = EndpointOf(member);
    if (selector & 0x08) {
      now += 400 + static_cast<qint64>(bytes.size()) * 31;
      service.Tick(now);
    }
    if (selector & 0x10) {
      service.Forget(from);
    }
    if (!games[member] || (selector & 0x20)) {
      games[member] = std::make_unique<CoopStream>(Hex(member),
                                                   ++instances[member] * 4 + 1);
    }
    now += 3;
    if (selector & 0x04) {
      service.Receive(from, Ids[member],
                      QByteArray(reinterpret_cast<const char *>(bytes.data()),
                                 static_cast<qsizetype>(bytes.size())),
                      now);
      continue;
    }
    CoopStream &game = *games[member];
    game.Send(bytes, now);
    for (const std::vector<u8> &body : game.Flush(now)) {
      const auto result = service.Receive(
          from, Ids[member],
          QByteArray(reinterpret_cast<const char *>(body.data()),
                     static_cast<qsizetype>(body.size())),
          now);
      // The server's answers back to the game: its acks keep the stream moving.
      for (const auto &send : result.sends) {
        for (int other = 0; other < 4; ++other) {
          if (games[other] && send.to == EndpointOf(other)) {
            games[other]->Receive(
                std::span{reinterpret_cast<const u8 *>(send.body.constData()),
                          static_cast<size_t>(send.body.size())},
                now);
          }
        }
      }
    }
  }
  service.Tick(now + 60'000);
  for (int member = 0; member < 4; ++member) {
    service.RoutesPacket(EndpointOf(member), EndpointOf((member + 1) % 4), 3);
    service.PartyOf(EndpointOf(member));
  }
}

std::vector<Bytes> WorldSeeds() {
  const auto put = [](Bytes &input, std::uint8_t selector,
                      const bbcoop::Envelope &envelope) {
    PutChunk(input, selector, EncodeEnvelope(envelope));
  };
  const auto lobby_hello = [](int member) {
    bbcoop::Envelope envelope;
    auto &hello = *envelope.mutable_lobby_hello();
    hello.set_protocol(CoopProtocol);
    hello.set_member(Hex(member));
    hello.set_name(Ids[member].toStdString());
    hello.set_playing(true);
    return envelope;
  };
  const auto request = [](bbcoop::TeamRequest::Action action, int member = 0) {
    bbcoop::Envelope envelope;
    envelope.mutable_team_request()->set_action(action);
    envelope.mutable_team_request()->set_member(Hex(member));
    envelope.mutable_team_request()->set_character("Hunter");
    return envelope;
  };
  const auto hello = [](int member, const std::string &party) {
    bbcoop::Envelope envelope;
    auto &hello = *envelope.mutable_hello();
    hello.set_protocol(CoopProtocol);
    hello.set_party(party);
    hello.set_member(Hex(member));
    hello.set_run(0x1001 + member);
    hello.set_host(member == 0);
    hello.set_enemy_owner(true);
    return envelope;
  };
  std::vector<Bytes> seeds;
  for (const std::uint8_t hosted : {0, 0x40}) {
    Bytes input;
    put(input, hosted | 0, lobby_hello(0));
    put(input, 1, lobby_hello(1));
    put(input, 0, request(bbcoop::TeamRequest::CREATE));
    put(input, 0, request(bbcoop::TeamRequest::INVITE, 1));
    put(input, 0, request(bbcoop::TeamRequest::READY));
    put(input, 0, request(bbcoop::TeamRequest::START));
    put(input, 0, hello(0, "fuzz:party"));
    put(input, 1, hello(1, "fuzz:party"));
    bbcoop::Envelope kills;
    auto *map = kills.mutable_kills()->add_kills();
    map->set_map(0x18000000);
    map->add_handles(0x10014066);
    put(input, 0, kills);
    bbcoop::Envelope vitals;
    vitals.mutable_vitals()->set_map(0x18000000);
    put(input, 1, vitals);
    bbcoop::Envelope request_snapshot;
    request_snapshot.mutable_snapshot_request();
    put(input, 1, request_snapshot);
    bbcoop::Envelope lifecycle;
    lifecycle.mutable_lifecycle();
    put(input, 0x08 | 1, lifecycle);
    bbcoop::Envelope trade;
    trade.mutable_trade_offer();
    put(input, 1, trade);
    put(input, 0x30 | 1, hello(1, "fuzz:party"));
    seeds.push_back(input);
  }
  return seeds;
}

} // namespace ShadNetFuzz
