// SPDX-FileCopyrightText: Copyright 2026 shadNet Project
// SPDX-License-Identifier: GPL-2.0-or-later
#include <map>
#include <memory>
#include <string>

#include "absl/log/globals.h"
#include "absl/log/initialize.h"
#include <QByteArray>
#include <QtGlobal>

#include "coop/coop_stream.h"
#include "coop/rpc_tunnel.h"
#include "fuzz_targets.h"
#include "server_link.h"

namespace ShadNetFuzz {

namespace {

using namespace ServerLink;

const QByteArray Ids[] = {"Ada", "Kaniel", "Eileen", "Gehrman"};

Endpoint EndpointOf(std::uint8_t selector) {
  return {0x7F000001u + (selector & 1u),
          static_cast<quint16>(40000 + ((selector >> 1) & 1u))};
}

std::span<const u8> Span(const QByteArray &bytes) {
  return {reinterpret_cast<const u8 *>(bytes.constData()),
          static_cast<size_t>(bytes.size())};
}

std::span<u8> MutableSpan(QByteArray &bytes) {
  return {reinterpret_cast<u8 *>(bytes.data()),
          static_cast<size_t>(bytes.size())};
}

template <typename T> QByteArray ToQt(const T &bytes) {
  return QByteArray(reinterpret_cast<const char *>(bytes.data()),
                    static_cast<qsizetype>(bytes.size()));
}

} // namespace

// A chunk's selector: bits 0-1 the sender, bit 2 a Tick before it, bit 3 the
// header added (bits 4-6 its kind - 1), bit 7 a Data's peer replaced by a
// linked id's virtual address.
void LinkDatagrams(std::span<const std::uint8_t> input) {
  Hub hub;
  hub.SetSignIn([](const QByteArray &online_id, const QByteArray &secret,
                   bool) {
    if (secret.startsWith("later")) {
      return std::optional<SignInVerdict>{};
    }
    return std::optional<SignInVerdict>{SignInVerdict{
        !secret.startsWith("no"), online_id.startsWith("G") ? "taken" : ""}};
  });
  hub.SetPartyOf([](const Endpoint &link) {
    return (link.port & 1) == 0 ? QStringLiteral("party") : QString{};
  });
  hub.SetPacketRoutes([](const Endpoint &, const Endpoint &, quint32 channel) {
    return channel % 3 != 0;
  });
  std::map<QByteArray, std::unique_ptr<BloodborneCoop::RpcTunnel>> tunnels;
  // Link version 4: the games' side (coop/link_auth.h), so the inputs reach
  // past the cookie and the handshake: a hello gets its source's cookie and
  // is sealed to the hub's key, and a game's datagram is sealed (but one in
  // seven, which goes as it is).
  std::map<quint64, LinkAuth::Client> games;
  u8 ephemerals = 0;
  qint64 now = 1000;
  Chunks chunks{input};
  std::uint8_t selector = 0;
  std::span<const std::uint8_t> bytes;
  while (chunks.Next(selector, bytes)) {
    if (selector & 0x04) {
      now += 50 + bytes.size() * 97;
      hub.Tick(now);
    }
    QByteArray datagram = ToQt(bytes);
    if (selector & 0x08) {
      const auto kind = static_cast<quint8>(((selector >> 4) & 7) + 1);
      QByteArray body = datagram;
      if (kind == static_cast<quint8>(Kind::Data) && (selector & 0x80) &&
          body.size() >= 6) {
        const Endpoint peer = VirtualEndpointOf(Ids[body[0] & 3]);
        body = Encode(Data{peer, body.mid(6)});
        datagram = body;
      } else {
        datagram = QByteArray::fromHex("ffffffff") + "SHLK" +
                   QByteArray(1, static_cast<char>(kind)) + body;
      }
    }
    // Every decoder reads every datagram.
    KindOf(datagram);
    DecodeHello(datagram);
    DecodeWelcome(datagram);
    DecodeData(datagram);
    DecodePing(Kind::Ping, datagram);
    DecodePing(Kind::Pong, datagram);
    DecodeMember(datagram);
    DecodeCoop(datagram);
    DecodeRpc(datagram);
    DecodeCookie(datagram);

    now += 1 + (selector & 0x30);
    const Endpoint from = EndpointOf(selector);
    LinkAuth::Client &game = games[from.Key()];
    const auto seal_hello = [&](Hello hello) {
      if (game.HasCookie()) {
        hello.cookie = QByteArray(
            reinterpret_cast<const char *>(game.CookieToEcho().data()),
            LinkAuth::CookieBytes);
      }
      LinkAuth::Key ephemeral{};
      ephemeral[1] = ++ephemerals; // past the bits X25519 clears
      ephemeral[2] = selector;
      return ToQt(game.SealHello(Span(Encode(hello)), ephemeral));
    };
    if (auto hello = KindOf(datagram) == Kind::Hello ? DecodeHello(datagram)
                                                     : std::nullopt;
        hello && hello->version == LinkVersion && (bytes.size() % 7) != 0) {
      if (!game.HasCookie()) {
        const Hub::Result asked = hub.Receive(from, seal_hello(*hello), now);
        for (const auto &send : asked.sends) {
          LinkAuth::Key key{};
          if (const auto cookie = DecodeCookie(send.datagram, nullptr, &key)) {
            game.TakeCookie(*cookie, key);
          }
        }
      }
      datagram = seal_hello(*hello);
    } else if (KindOf(datagram) != Kind::Hello && (bytes.size() % 7) != 0) {
      if (const auto trailer = game.Seal(MutableSpan(datagram))) {
        datagram.append(reinterpret_cast<const char *>(trailer->data()),
                        static_cast<qsizetype>(trailer->size()));
      }
    }
    const Hub::Result result = hub.Receive(from, datagram, now);
    for (const auto &send : result.sends) {
      if (send.to == from) {
        game.Open(Span(send.datagram));
      }
    }
    for (const Hub::Coop &rpc : result.rpc) {
      auto &tunnel = tunnels[rpc.online_id];
      if (!tunnel) {
        tunnel = std::make_unique<BloodborneCoop::RpcTunnel>("shadnet", 7);
      }
      tunnel->Receive(
          std::span{reinterpret_cast<const u8 *>(rpc.body.constData()),
                    static_cast<size_t>(rpc.body.size())},
          now);
    }
  }
  hub.Tick(now + 100'000);
}

// A chunk's selector: bit 0 the bytes are a frame's entries only (a header
// added), bit 1 into the stream too.
void GameFrames(std::span<const std::uint8_t> input) {
  BloodborneCoop::CoopStream stream{"shadnet", 9};
  s64 now = 1000;
  Chunks chunks{input};
  std::uint8_t selector = 0;
  std::span<const std::uint8_t> bytes;
  while (chunks.Next(selector, bytes)) {
    std::vector<u8> body(bytes.begin(), bytes.end());
    if (selector & 1) {
      BloodborneCoop::GameFrame frame{"game", 5, 9, 0, {}};
      std::vector<u8> header = BloodborneCoop::EncodeGameFrame(frame);
      header.resize(header.size() - 2); // the count is the bytes'
      body.insert(body.begin(), header.begin(), header.end());
    }
    if (const auto frame = BloodborneCoop::DecodeGameFrame(body)) {
      // A frame that reads writes back the same bytes.
      if (BloodborneCoop::EncodeGameFrame(*frame) != body) {
        __builtin_trap();
      }
    }
    if (selector & 2) {
      now += 37;
      stream.Receive(body, now);
      stream.Flush(now);
    }
  }
}

void Quiet() {
  qInstallMessageHandler(
      [](QtMsgType, const QMessageLogContext &, const QString &) {});
  // protobuf's, on every string field that is not UTF-8.
  absl::InitializeLog();
  absl::SetMinLogLevel(absl::LogSeverityAtLeast::kInfinity);
  absl::SetStderrThreshold(absl::LogSeverityAtLeast::kInfinity);
}

std::vector<Bytes> LinkSeeds() {
  std::vector<Bytes> seeds;
  const auto put = [](Bytes &input, std::uint8_t selector,
                      const QByteArray &datagram) {
    PutChunk(
        input, selector,
        std::span{reinterpret_cast<const std::uint8_t *>(datagram.constData()),
                  static_cast<size_t>(datagram.size())});
  };
  BloodborneCoop::GameFrame frame{"Ada", 1, 0, 0, {}};
  frame.entries.push_back(
      {BloodborneCoop::PacketClass::Reliable, 1, {0, 0, 0, 2, 1, 2, 3}});
  const std::vector<u8> frame_bytes = BloodborneCoop::EncodeGameFrame(frame);
  const QByteArray game_frame(
      reinterpret_cast<const char *>(frame_bytes.data()),
      static_cast<qsizetype>(frame_bytes.size()));
  BloodborneCoop::RpcTunnel client{"Ada", 3};
  Bytes linked;
  put(linked, 0, Encode(Hello{LinkVersion, "Ada", "build1", "secret"}));
  put(linked, 2, Encode(Hello{LinkVersion, "Kaniel", "build1", "secret"}));
  put(linked, 0, Encode(Data{VirtualEndpointOf("Kaniel"), game_frame}));
  put(linked, 2, Encode(Data{VirtualEndpointOf("Ada"), game_frame}));
  put(linked, 0, Encode(Kind::Ping, Ping{1, 1000}));
  put(linked, 0, EncodeCoop(game_frame));
  put(linked, 0, EncodeRpc(game_frame));
  put(linked, 4, Encode(Kind::Ping, Ping{2, 5000}));
  seeds.push_back(linked);
  Bytes refused;
  put(refused, 1, Encode(Hello{LinkVersion, "Gehrman", "build2", "no"}));
  put(refused, 1, Encode(Hello{1, "Eileen", "", ""}));
  put(refused, 3, Encode(Hello{LinkVersion, "Eileen", "build1", "later"}));
  put(refused, 3, Encode(Member{MemberState::Back, 10, {1, 2}, "x"}));
  put(refused, 3, Encode(Welcome{true, "hi"}));
  seeds.push_back(refused);
  Bytes shortcut;
  put(shortcut, 0, Encode(Hello{LinkVersion, "Ada", "build1", "secret"}));
  put(shortcut, 0x88 | (2 << 4), QByteArray(6, '\1') + game_frame);
  put(shortcut, 0x08 | (6 << 4), game_frame);
  put(shortcut, 0x08 | (7 << 4), game_frame);
  seeds.push_back(shortcut);
  return seeds;
}

std::vector<Bytes> GameFrameSeeds() {
  BloodborneCoop::GameFrame frame{"Kaniel", 5, 9, 3, {}};
  frame.entries.push_back(
      {BloodborneCoop::PacketClass::Reliable, 1, {1, 'a', 'b'}});
  frame.entries.push_back({BloodborneCoop::PacketClass::Unreliable, 7, {9, 9}});
  Bytes input;
  PutChunk(input, 2, BloodborneCoop::EncodeGameFrame(frame));
  frame.entries[0].seq = 2;
  PutChunk(input, 2, BloodborneCoop::EncodeGameFrame(frame));
  const std::vector<u8> entries = {1, 0, 0, 1, 0, 0, 0, 2, 0, 0, 0, 'x', 'y'};
  PutChunk(input, 3, entries);
  return {input};
}

} // namespace ShadNetFuzz
