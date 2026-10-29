// SPDX-FileCopyrightText: Copyright 2026 shadNet Project
// SPDX-License-Identifier: GPL-2.0-or-later
// The server link's datagrams as the emulator writes them
// (tests/contracts/server_link_frames.txt, which the emulator's tests read
// too), and the hub that forwards the games' frames between them.
#include <cstring>
#include <fstream>
#include <iostream>
#include <map>
#include <string>
#include <tuple>
#include <vector>

#include <QCryptographicHash>
#include <QFile>
#include <QFileInfo>
#include <QMessageAuthenticationCode>
#include <QTemporaryDir>

#include "coop/link_auth.h"
#include "coop/packet_routes.h"
#include "server_link.h"

using namespace ServerLink;

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

std::map<std::string, QByteArray> ReadContracts(const char *path) {
  std::map<std::string, QByteArray> datagrams;
  std::ifstream file(path);
  std::string line;
  while (std::getline(file, line)) {
    if (!line.empty() && line.back() == '\r') {
      line.pop_back();
    }
    if (line.empty() || line[0] == '#') {
      continue;
    }
    const auto tab = line.find('\t');
    if (tab == std::string::npos) {
      continue;
    }
    datagrams[line.substr(0, tab)] =
        QByteArray::fromHex(QByteArray::fromStdString(line.substr(tab + 1)));
  }
  return datagrams;
}

const Endpoint Ada127{0x7F000001, 0x1234};
const QByteArray GameFrame = QByteArray::fromHex("75307531070000000200000"
                                                 "0abcd");
// The contracts' hello carries the text before the build fingerprint.
const QByteArray ContractBuild = "92829ac4 events 1f protocol 9";
// Builds as the games say them (coop/build_fingerprint.h).
const QByteArray Build =
    "bb-build 1;version=1.2.0;revision=92829ac4;protocol=9;"
    "events=1f;harder=off;fps=60;";
const QByteArray OtherBuild = "bb-build 1;version=1.2.0;revision=13ddd14d;"
                              "protocol=9;events=1f;harder=off;fps=60;";
const QByteArray OlderBuild = "bb-build 1;version=1.1.9;revision=13ddd14d;"
                              "protocol=9;events=1f;harder=off;fps=60;";

const Endpoint Kaniel{0x7F000001, 40001};
const Endpoint Ada{0x7F000001, 40002};
const Endpoint Eileen{0xC0A80105, 40003};

QByteArray HelloFrom(const char *who, const QByteArray &build = Build,
                     const QByteArray &secret = "s3cr3t") {
  return Encode(Hello{LinkVersion, who, build, secret, {}});
}

QByteArray Bytes(const LinkAuth::Trailer &trailer) {
  return QByteArray(reinterpret_cast<const char *>(trailer.data()),
                    static_cast<qsizetype>(trailer.size()));
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

LinkAuth::Key Filled(u8 byte) {
  LinkAuth::Key key;
  key.fill(byte);
  return key;
}

// A new ephemeral key for each handshake a test makes.
LinkAuth::Key NextKey() {
  static quint32 count = 0;
  LinkAuth::Key key = Filled(0x60);
  ++count; // past byte 0, whose low bits X25519 clears
  std::memcpy(key.data() + 1, &count, sizeof(count));
  return key;
}

// The sends of `result` to `to`.
QList<QByteArray> SentTo(const Hub::Result &result, const Endpoint &to) {
  QList<QByteArray> sent;
  for (const auto &send : result.sends) {
    if (send.to == to) {
      sent.append(send.datagram);
    }
  }
  return sent;
}

// A game's hello (`plain`, as Encode writes it) for its link's state: the
// cookie it has, sealed to the server's key (coop/link_auth.h).
QByteArray Sealed(LinkAuth::Client &game, QByteArray plain) {
  if (game.HasCookie()) {
    plain.replace(LinkAuth::HeaderBytes + 1, LinkAuth::CookieBytes,
                  ToQt(game.CookieToEcho()));
  }
  return ToQt(game.SealHello(Span(plain), NextKey()));
}

// The cookie of a cookie answer, taken by `game` (with the server's key).
bool TakeCookie(LinkAuth::Client &game, const QByteArray &answer) {
  LinkAuth::Key key{};
  const auto cookie = DecodeCookie(answer, nullptr, &key);
  return cookie && game.TakeCookie(*cookie, key);
}

// The hub with the games' side of the link around it (coop/link_auth.h, as the
// emulator runs it): a hello goes through the cookie round trip and the
// handshake, a game's datagram goes sealed, and the hub's answers come back
// opened by their game (one that does not open is dropped and counted in
// `unopened`).
class Net {
public:
  template <typename... Args> explicit Net(Args... args) : hub(args...) {}

  Hub::Result Receive(const Endpoint &from, const QByteArray &datagram,
                      qint64 now_ms) {
    const auto hello =
        KindOf(datagram) == Kind::Hello ? DecodeHello(datagram) : std::nullopt;
    auto &game = games[from.Key()];
    if (!hello || hello->version != LinkVersion) {
      QByteArray sent = datagram;
      if (const auto trailer = game.Seal(MutableSpan(sent))) {
        sent += Bytes(*trailer);
      }
      return Opened(hub.Receive(from, sent, now_ms));
    }
    for (int round = 0; round < 3; ++round) {
      Hub::Result result = hub.Receive(from, Sealed(game, datagram), now_ms);
      const auto sent = SentTo(result, from);
      if (sent.size() != 1 || KindOf(sent[0]) != Kind::Cookie) {
        return Opened(std::move(result));
      }
      TakeCookie(game, sent[0]);
    }
    return {};
  }

  Hub::Result Tick(qint64 now_ms) { return Opened(hub.Tick(now_ms)); }
  Hub *operator->() { return &hub; }

  Hub hub;
  std::map<quint64, LinkAuth::Client> games;
  int unopened = 0;

private:
  Hub::Result Opened(Hub::Result result) {
    QList<Hub::Send> opened;
    for (const auto &send : result.sends) {
      if (KindOf(send.datagram) == Kind::Cookie) {
        opened.append(send);
        continue;
      }
      const auto found = games.find(send.to.Key());
      const auto plain = found == games.end()
                             ? std::nullopt
                             : found->second.Open(Span(send.datagram));
      if (!plain) {
        ++unopened;
        continue;
      }
      opened.append({send.to, ToQt(*plain), true});
    }
    result.sends = opened;
    return result;
  }
};

int Contracts() {
  const auto datagrams = ReadContracts(BLOODBORNE_SERVER_LINK_CONTRACTS);
  CHECK(datagrams.size() == 19);
  const auto datagram = [&](const char *name) { return datagrams.at(name); };

  const QByteArray cookie(16, '\x11');
  CHECK(Encode(Hello{LinkVersion, "Kaniel", ContractBuild, "s3cr3t", cookie}) ==
        datagram("hello"));
  const auto hello = DecodeHello(datagram("hello"));
  CHECK(hello && hello->version == 4 && hello->online_id == "Kaniel" &&
        hello->build == ContractBuild && hello->secret == "s3cr3t" &&
        hello->cookie == cookie);
  // The cookie, with the work's bits and the server's key.
  const LinkAuth::ServerKey server_key = LinkAuth::ServerKeyOf(Filled(0x44));
  CHECK(ToQt(server_key.pub) == datagram("server.key"));
  LinkAuth::Cookie cookie_bytes;
  cookie_bytes.fill(0x11);
  CHECK(EncodeCookie(cookie_bytes, 20, server_key.pub) == datagram("cookie"));
  int work_bits = 0;
  LinkAuth::Key said_key{};
  CHECK(DecodeCookie(datagram("cookie"), &work_bits, &said_key) ==
            cookie_bytes &&
        work_bits == 20 && said_key == server_key.pub);
  // Hosted: a new account's work, and the answer that asks for it.
  Hello worked{LinkVersion, "Kaniel", ContractBuild, "s3cr3t", cookie};
  worked.work = 0x0807060504030201;
  CHECK(Encode(worked) == datagram("hello.work"));
  CHECK(DecodeHello(datagram("hello.work"))->work == 0x0807060504030201);
  CHECK(!hello->work);
  CHECK(EncodeWork(20) == datagram("work"));
  CHECK(DecodeWork(datagram("work")) == 20 && !DecodeWork(datagram("rpc")));
  // Link version 4 (coop/link_auth.h): the hello sealed to the server's key
  // (the game's ephemeral key 32 x 0x33), the welcome's answer (the server's
  // 32 x 0x55), the game's first datagram: the bytes loadtest/noise_link.py
  // makes with Python's cryptography, not with our code.
  LinkAuth::Client game;
  CHECK(game.TakeCookie(cookie_bytes, server_key.pub));
  CHECK(ToQt(game.SealHello(Span(datagram("hello")), Filled(0x33))) ==
        datagram("hello.sealed"));
  std::vector<u8> plain;
  const auto responder = LinkAuth::Responder::Open(
      server_key, Span(datagram("hello.sealed")), plain);
  CHECK(responder && ToQt(plain) == datagram("hello"));
  auto answered = responder->Answer(Span(datagram("welcome.ok")), Filled(0x55));
  CHECK(ToQt(answered.datagram) == datagram("welcome.sealed"));
  const auto welcomed = game.Open(Span(datagram("welcome.sealed")));
  CHECK(welcomed && ToQt(*welcomed) == datagram("welcome.ok") && game.Linked());
  QByteArray ping_bytes = Encode(Kind::Ping, Ping{7, 1000});
  ping_bytes += Bytes(*game.Seal(MutableSpan(ping_bytes)));
  CHECK(ping_bytes == datagram("ping.sealed"));
  CHECK(answered.session.Open(MutableSpan(ping_bytes)) ==
            LinkAuth::Session::Verdict::Ok &&
        ping_bytes.left(ping_bytes.size() - 24) == datagram("ping"));
  // Phase 11: the old calls (friends, scores) in the link's second stream.
  CHECK(EncodeRpc(QByteArray::fromHex("0102")) == datagram("rpc"));
  CHECK(DecodeRpc(datagram("rpc")) == QByteArray::fromHex("0102"));
  CHECK(!DecodeRpc(datagram("ping")) && !DecodeCoop(datagram("rpc")));

  CHECK(Encode(Welcome{true, {}}) == datagram("welcome.ok"));
  CHECK(Encode(Welcome{false, "another build: 13ddd14d"}) ==
        datagram("welcome.refused"));
  const auto refused = DecodeWelcome(datagram("welcome.refused"));
  CHECK(refused && !refused->accepted &&
        refused->text == "another build: 13ddd14d");

  CHECK(Encode(Data{Ada127, GameFrame}) == datagram("data"));
  const auto data = DecodeData(datagram("data"));
  CHECK(data && data->peer == Ada127 && data->frame == GameFrame);

  CHECK(Encode(Kind::Ping, Ping{7, 1000}) == datagram("ping"));
  CHECK(Encode(Kind::Pong, Ping{7, 1000}) == datagram("pong"));
  const auto ping = DecodePing(Kind::Ping, datagram("ping"));
  CHECK(ping && ping->seq == 7 && ping->sent_ms == 1000);
  CHECK(!DecodePing(Kind::Ping, datagram("pong")));

  CHECK(Encode(Member{MemberState::Quiet, 3500, Ada127, "Ada"}) ==
        datagram("member.quiet"));
  CHECK(Encode(Member{MemberState::Back, 4200, Ada127, "Ada"}) ==
        datagram("member.back"));
  CHECK(Encode(Member{MemberState::Gone, 60000, Ada127, "Ada"}) ==
        datagram("member.gone"));
  const auto gone = DecodeMember(datagram("member.gone"));
  CHECK(gone && gone->state == MemberState::Gone && gone->gap_ms == 60000 &&
        gone->endpoint == Ada127 && gone->online_id == "Ada");

  for (const auto &[name, bytes] : datagrams) {
    if (name == "server.key") {
      continue; // a key, not a datagram
    }
    CHECK(KindOf(bytes).has_value());
    if (name.ends_with(".sealed")) {
      continue; // read only once opened
    }
    for (int size = 0; size < bytes.size(); ++size) {
      const QByteArray cut = bytes.left(size);
      if (name == "data" && size >= 15) {
        continue; // a shorter frame is still a frame
      }
      if ((name == "hello.work" || name == "hello.ticket") &&
          size >= datagram("hello").size()) {
        continue; // without the work, or the ticket
      }
      CHECK(!DecodeHello(cut) && !DecodeWelcome(cut) && !DecodeData(cut) &&
            !DecodeCookie(cut) && !DecodePing(Kind::Ping, cut) &&
            !DecodePing(Kind::Pong, cut) && !DecodeMember(cut) &&
            !DecodeWork(cut) && !DecodeRedirect(cut));
    }
  }
  // The STUN ping shares the header.
  CHECK(!KindOf(QByteArray::fromHex("ffffffff014b616e69656c")));
  std::cout << "server link contracts: ok\n";
  return 0;
}

int HelloAndBuilds() {
  Net hub;
  auto result = hub.Receive(Kaniel, HelloFrom("Kaniel"), 0);
  CHECK(SentTo(result, Kaniel).size() == 1);
  CHECK(DecodeWelcome(SentTo(result, Kaniel)[0])->accepted);
  CHECK(hub->Members() == 1 && hub->Build() == Build);

  result = hub.Receive(Ada, HelloFrom("Ada"), 10);
  CHECK(DecodeWelcome(SentTo(result, Ada).value(0))->accepted);
  CHECK(hub->Members() == 2);

  // A build or id with control characters is no hello: its text goes into other
  // games' refusals and logs, where a line break could pass for a line of the
  // game's own (the launcher reads them).
  result =
      hub.Receive(Eileen, HelloFrom("Eileen", "x\nServer link: forged"), 15);
  CHECK(SentTo(result, Eileen).isEmpty() && hub->Members() == 2);
  // (The short build of before the fingerprints: a hello as long as a cookie
  // would get one back, the hub cannot read it before.)
  result = hub.Receive(Eileen,
                       HelloFrom("Eil\x01"
                                 "een",
                                 ContractBuild),
                       16);
  CHECK(SentTo(result, Eileen).isEmpty() && hub->Members() == 2);

  // One release for everyone (Kaniel 2026-10-25: only another Seamless
  // Co-op version is refused).
  result = hub.Receive(Eileen, HelloFrom("Eileen", OlderBuild), 20);
  const auto refused = DecodeWelcome(SentTo(result, Eileen).value(0));
  CHECK(refused && !refused->accepted);
  // The team screen's line first, then the whole reason (as logged).
  CHECK(refused->text ==
        "Co-op refused: your game is version 1.1.9, the games here run 1.2.0: "
        "update in the launcher\n"
        "another build: your game is version 1.1.9, the games here run "
        "1.2.0: update in the launcher");
  CHECK(hub->Members() == 2);
  CHECK(result.log.join('\n').contains(
      "refused Eileen (192.168.1.5:40003): another build: your game is "
      "version 1.1.9"));

  // Another link version cannot read a sealed answer, and a plain one would
  // go to any source: it hears nothing (version 3's games, the last before
  // encryption, neither).
  for (const quint8 version : {3, 5}) {
    Hello other{version, "Eileen", Build, "s3cr3t", {}, {}};
    result = hub.Receive(Eileen, Encode(other).leftJustified(80, '\0'), 30);
    CHECK(result.sends.isEmpty() && hub->Members() == 2);
  }

  // A hello again (the game restarted on the same port) is welcomed again.
  result = hub.Receive(Kaniel, HelloFrom("Kaniel"), 40);
  CHECK(DecodeWelcome(SentTo(result, Kaniel).value(0))->accepted);
  CHECK(hub->Members() == 2);
  // Phase 11: the link starts with the sign-in, before the co-op code says the
  // build. A hello with none is welcomed and holds nothing against the others;
  // the game says hello again once it knows its build, and that one counts.
  Net fresh;
  result = fresh.Receive(Kaniel, HelloFrom("Kaniel", ""), 45);
  CHECK(DecodeWelcome(SentTo(result, Kaniel).value(0))->accepted);
  CHECK(fresh->Build().isEmpty());
  result = fresh.Receive(Ada, HelloFrom("Ada"), 46);
  CHECK(DecodeWelcome(SentTo(result, Ada).value(0))->accepted);
  CHECK(fresh->Build() == Build);
  result = fresh.Receive(Kaniel, HelloFrom("Kaniel", OlderBuild), 47);
  CHECK(!DecodeWelcome(SentTo(result, Kaniel).value(0))->accepted);
  result = fresh.Receive(Kaniel, HelloFrom("Kaniel"), 48);
  CHECK(DecodeWelcome(SentTo(result, Kaniel).value(0))->accepted);
  CHECK(fresh->Members() == 2);
  // A game of before phase 11 (link version 1, no secret) neither.
  QByteArray old = QByteArray::fromHex("ffffffff") + "SHLK";
  old += char(1);
  old += char(1);
  old += char(3);
  old += "Old";
  old += char(Build.size());
  old += Build;
  result = hub.Receive(Eileen, old, 50);
  CHECK(result.sends.isEmpty() && hub.unopened == 0);
  std::cout << "server link hello: ok\n";
  return 0;
}

// Build check on join: another release is refused, against the games here
// and the server's own build; any other difference links with a warning.
int BuildChecks() {
  // A setting that changes the game, against the games here: linked, and
  // the welcome says the installation differs (the team screen's line first,
  // then every field).
  Net hub;
  auto result = hub.Receive(Kaniel, HelloFrom("Kaniel"), 0);
  CHECK(DecodeWelcome(SentTo(result, Kaniel).value(0))->text.isEmpty());
  QByteArray harder = Build;
  harder.replace("harder=off", "harder=3,1.5,3");
  result = hub.Receive(Ada, HelloFrom("Ada", harder), 10);
  auto welcome = DecodeWelcome(SentTo(result, Ada).value(0));
  CHECK(welcome && welcome->accepted);
  CHECK(welcome->text ==
        "Your installation differs from the others (harder enemies): co-op "
        "may misbehave\n"
        "your installation differs from the games here: harder-enemies "
        "setting HP x3 damage x1.5 poise x3, theirs off");
  CHECK(hub->Members() == 2);
  CHECK(result.log.join('\n').contains(
      "Ada (127.0.0.1:40002) runs other settings than the games here "
      "(harder-enemies setting HP x3 damage x1.5 poise x3, theirs off)"));
  // What is only logged (the frame rate) is no warning; the server says it.
  QByteArray faster = Build;
  faster.replace("fps=60", "fps=90 unlocked");
  result = hub.Receive(Ada, HelloFrom("Ada", faster), 15);
  welcome = DecodeWelcome(SentTo(result, Ada).value(0));
  CHECK(welcome && welcome->accepted && welcome->text.isEmpty());
  CHECK(result.log.join('\n').contains(
      "Ada (127.0.0.1:40002) runs other settings than the games here (not "
      "checked: frame rate 90 unlocked, theirs 60)"));

  // Another build of the same release links too, against each game here.
  QByteArray other_harder = OtherBuild;
  other_harder.replace("harder=off", "harder=2,1,1");
  result = hub.Receive(Eileen, HelloFrom("Eileen", other_harder), 20);
  welcome = DecodeWelcome(SentTo(result, Eileen).value(0));
  CHECK(welcome && welcome->accepted &&
        welcome->text.startsWith("Your installation differs from the others "
                                 "(build, harder enemies): co-op may "
                                 "misbehave\n"));
  CHECK(hub->Members() == 3);

  // A hello in the text before the fingerprint: an older release.
  result = hub.Receive(Eileen, HelloFrom("Eileen", ContractBuild), 30);
  auto refused = DecodeWelcome(SentTo(result, Eileen).value(0));
  CHECK(refused && !refused->accepted);
  CHECK(refused->text.startsWith("Co-op refused: your game is another "
                                 "release: update in the launcher\n"));
  CHECK(refused->text.contains(ContractBuild));

  // The server's own release: refused alone on the server, with nobody here.
  Net server;
  server->SetServerBuild(
      "bb-build 1;version=1.2.0;revision=92829ac4;protocol=9;");
  QByteArray older = Build;
  older.replace("version=1.2.0", "version=1.1.9");
  result = server.Receive(Kaniel, HelloFrom("Kaniel", older), 0);
  refused = DecodeWelcome(SentTo(result, Kaniel).value(0));
  CHECK(refused && !refused->accepted);
  CHECK(refused->text ==
        "Co-op refused: your game is version 1.1.9, the server runs 1.2.0: "
        "update in the launcher\n"
        "another build: your game is version 1.1.9, the server runs 1.2.0: "
        "update in the launcher");
  CHECK(server->Members() == 0);
  result = server.Receive(Kaniel, HelloFrom("Kaniel"), 10);
  CHECK(DecodeWelcome(SentTo(result, Kaniel).value(0))->accepted);
  // The server does not know a game's settings or files: the games here
  // warn of them.
  result = server.Receive(Ada, HelloFrom("Ada", harder), 20);
  welcome = DecodeWelcome(SentTo(result, Ada).value(0));
  CHECK(welcome && welcome->accepted && !welcome->text.isEmpty());
  Net alone;
  alone->SetServerBuild(
      "bb-build 1;version=1.2.0;revision=92829ac4;protocol=9;");
  result = alone.Receive(Ada, HelloFrom("Ada", harder), 30);
  welcome = DecodeWelcome(SentTo(result, Ada).value(0));
  CHECK(welcome && welcome->accepted && welcome->text.isEmpty());
  // Another build of the server's release links.
  result = alone.Receive(Eileen, HelloFrom("Eileen", OtherBuild), 31);
  CHECK(DecodeWelcome(SentTo(result, Eileen).value(0))->accepted);
  // A server that does not check its own (a rig's, built apart): the games
  // here only.
  Net apart;
  apart->SetServerBuild({});
  result = apart.Receive(Kaniel, HelloFrom("Kaniel", older), 0);
  CHECK(DecodeWelcome(SentTo(result, Kaniel).value(0))->accepted);
  std::cout << "server link build checks: ok\n";
  return 0;
}

int CompatibleVersions() {
  const QByteArray dev = QByteArray(Build).replace("version=1.2.0", "version=dev");
  const QByteArray windows = QByteArray(Build).replace("version=1.2.0", "version=1.3.0");
  // Exercise the encrypted handshake in each join order, including a pre-build sign-in.
  for (bool windows_first : {false, true}) {
    Net hub;
    hub->SetServerBuild("bb-build 1;version=dev;protocol=9;");
    hub->SetCompatibleVersions({"dev", "1.3.0"});
    auto result = hub.Receive(Kaniel, HelloFrom("Kaniel", {}), 0);
    CHECK(DecodeWelcome(SentTo(result, Kaniel).value(0))->accepted);
    result = hub.Receive(Kaniel, HelloFrom("Kaniel", windows_first ? windows : dev), 1);
    CHECK(DecodeWelcome(SentTo(result, Kaniel).value(0))->accepted);
    auto second = windows_first ? dev : windows;
    second.replace("events=1f", "events=ff");
    result = hub.Receive(Ada, HelloFrom("Ada", second), 2);
    auto welcome = DecodeWelcome(SentTo(result, Ada).value(0));
    CHECK(welcome && welcome->accepted && welcome->text.contains("game files"));
    CHECK(hub->Members() == 2);
    // An unlisted release and a listed release with a different protocol stay out.
    const QByteArray unlisted = QByteArray(windows).replace("version=1.3.0", "version=1.4.0");
    result = hub.Receive(Eileen, HelloFrom("Eileen", unlisted), 3);
    welcome = DecodeWelcome(SentTo(result, Eileen).value(0));
    CHECK(welcome && !welcome->accepted && welcome->text.contains("version 1.4.0"));
    const QByteArray wrong_protocol = QByteArray(windows).replace("protocol=9", "protocol=10");
    result = hub.Receive(Eileen, HelloFrom("Eileen", wrong_protocol), 4);
    welcome = DecodeWelcome(SentTo(result, Eileen).value(0));
    CHECK(welcome && !welcome->accepted && welcome->text.contains("protocol 10"));
    CHECK(hub->Members() == 2);
  }
  // A legacy exact HostedBuild remains exact even with compatible labels configured.
  Net exact;
  exact->SetRequiredBuild("specific-build");
  exact->SetCompatibleVersions({"dev", "1.3.0"});
  auto result = exact.Receive(Kaniel, HelloFrom("Kaniel", windows), 0);
  CHECK(!DecodeWelcome(SentTo(result, Kaniel).value(0))->accepted);
  std::cout << "server link compatible release labels: ok\n";
  return 0;
}

// Phase 11: the hello is the sign-in. The player's id and its secret (the
// launcher made them) sign in, nothing else; the old calls go in the link's
// second stream (Rpc), from signed-in games only.
int SignsIn() {
  Net hub;
  QList<QByteArray> asked;
  hub->SetSignIn(
      [&](const QByteArray &online_id, const QByteArray &secret, bool) {
        asked.append(online_id);
        if (online_id == "Squatter") {
          return SignInVerdict{false, "address taken: make a new id"};
        }
        return SignInVerdict{online_id == "Kaniel" && secret == "right", {}};
      });
  auto result = hub.Receive(Kaniel, HelloFrom("Kaniel", Build, "wrong"), 0);
  const auto refused = DecodeWelcome(SentTo(result, Kaniel).value(0));
  CHECK(refused && !refused->accepted && refused->text.contains("sign-in"));
  CHECK(hub->Members() == 0 && asked == QList<QByteArray>{"Kaniel"});
  result = hub.Receive(Kaniel, EncodeRpc("dropped"), 5);
  CHECK(result.rpc.isEmpty() && result.sends.isEmpty());

  result = hub.Receive(Kaniel, HelloFrom("Kaniel", Build, "right"), 10);
  CHECK(DecodeWelcome(SentTo(result, Kaniel).value(0))->accepted);
  CHECK(hub->Members() == 1);
  result = hub.Receive(Kaniel, EncodeRpc("body"), 20);
  CHECK(result.rpc.size() == 1 && result.rpc[0].from == Kaniel &&
        result.rpc[0].online_id == "Kaniel" && result.rpc[0].body == "body");
  CHECK(result.coop.isEmpty());
  // A refusal with its reason (a new account on another's address): the
  // game's player reads it.
  result = hub.Receive(Ada, HelloFrom("Squatter", Build, "any"), 30);
  const auto squatted = DecodeWelcome(SentTo(result, Ada).value(0));
  CHECK(squatted && !squatted->accepted &&
        squatted->text == "address taken: make a new id");
  // The address the games compute too (emulator net_link.h): never changes.
  CHECK(VirtualEndpointOf("p000000000048cf").ipv4 == 0x0ae08f32 &&
        VirtualEndpointOf("p00000000005b99").ipv4 == 0x0ae08f32 &&
        VirtualEndpointOf("Kaniel").port == VirtualPort);
  std::cout << "server link sign-in: ok\n";
  return 0;
}

int Relays() {
  Net hub;
  hub.Receive(Kaniel, HelloFrom("Kaniel"), 0);
  hub.Receive(Ada, HelloFrom("Ada"), 0);

  auto result = hub.Receive(
      Kaniel, Encode(Data{VirtualEndpointOf("Ada"), GameFrame}), 100);
  const auto forwarded = SentTo(result, Ada);
  CHECK(forwarded.size() == 1 && result.sends.size() == 1);
  const auto data = DecodeData(forwarded[0]);
  CHECK(data && data->peer == VirtualEndpointOf("Kaniel") &&
        data->frame == GameFrame);
  CHECK(!result.log.isEmpty() && result.log[0].contains("Kaniel") &&
        result.log[0].contains("Ada"));

  // Nobody at the destination: dropped, and said.
  result = hub.Receive(
      Kaniel, Encode(Data{VirtualEndpointOf("Eileen"), GameFrame}), 110);
  CHECK(result.sends.isEmpty());
  CHECK(!result.log.isEmpty() && result.log[0].contains("no game"));

  // A game that never said hello is not relayed.
  result = hub.Receive(
      Eileen, Encode(Data{VirtualEndpointOf("Kaniel"), GameFrame}), 120);
  CHECK(result.sends.isEmpty());

  // Only the first frames of a pair are logged.
  Net quiet_log(3000, 60000, 10000, 2);
  quiet_log.Receive(Kaniel, HelloFrom("Kaniel"), 0);
  quiet_log.Receive(Ada, HelloFrom("Ada"), 0);
  int lines = 0;
  for (int index = 0; index < 5; ++index) {
    lines += static_cast<int>(
        quiet_log
            .Receive(Kaniel, Encode(Data{VirtualEndpointOf("Ada"), GameFrame}),
                     index)
            .log.size());
  }
  CHECK(lines == 2);

  // A ping comes back as a pong with the same body.
  result = hub.Receive(Ada, Encode(Kind::Ping, Ping{3, 777}), 130);
  const auto pong = DecodePing(Kind::Pong, SentTo(result, Ada).value(0));
  CHECK(pong && pong->seq == 3 && pong->sent_ms == 777);
  CHECK(hub.unopened == 0);
  std::cout << "server link relay: ok\n";
  return 0;
}

// Phase 11: a game silent for 12 s (its game died, or its connection is down
// for longer than a blip) loses its sign-in session (`lost`, once a silence):
// what the TCP connection's end did at once when its game died. A blip under
// that rides out.
int ALongSilenceEndsTheSignIn() {
  Net hub;
  hub.Receive(Kaniel, HelloFrom("Kaniel"), 0);
  hub.Receive(Ada, HelloFrom("Ada"), 0);
  for (qint64 at = 1000; at <= 30000; at += 1000) {
    hub.Receive(Ada, Encode(Kind::Ping, Ping{1, 1}), at);
    const auto result = hub.Tick(at + 1);
    if (at == 12000) {
      CHECK(result.lost.size() == 1 && result.lost[0] == Kaniel);
    } else {
      CHECK(result.lost.isEmpty());
    }
    if (at == 20000) {
      hub.Receive(Kaniel, Encode(Kind::Ping, Ping{1, 1}), at); // back
    }
  }
  CHECK(hub.Tick(32002).lost.size() == 1); // silent again since 20 s
  std::cout << "server link: a long silence ends the sign-in\n";
  return 0;
}

int Liveness() {
  Net hub;
  hub.Receive(Kaniel, HelloFrom("Kaniel"), 0);
  hub.Receive(Ada, HelloFrom("Ada"), 0);
  hub.Receive(Kaniel, Encode(Kind::Ping, Ping{1, 1000}), 1000);

  auto result = hub.Tick(3000);
  CHECK(result.sends.isEmpty());

  // Ada's link quiet for longer than 3 s: Kaniel is told, once.
  hub.Receive(Kaniel, Encode(Kind::Ping, Ping{2, 3000}), 3000);
  result = hub.Tick(3500);
  const auto told = SentTo(result, Kaniel);
  CHECK(told.size() == 1 && SentTo(result, Ada).isEmpty());
  const auto quiet = DecodeMember(told[0]);
  CHECK(quiet && quiet->state == MemberState::Quiet &&
        quiet->endpoint == VirtualEndpointOf("Ada") &&
        quiet->online_id == "Ada" && quiet->gap_ms == 3500);
  CHECK(!result.log.isEmpty() && result.log[0].contains("Ada"));
  hub.Receive(Kaniel, Encode(Kind::Ping, Ping{3, 4000}), 4000);
  CHECK(SentTo(hub.Tick(4500), Kaniel).isEmpty());

  // Heard again.
  result = hub.Receive(Ada, Encode(Kind::Ping, Ping{1, 6000}), 6000);
  const auto back = DecodeMember(SentTo(result, Kaniel).value(0));
  CHECK(back && back->state == MemberState::Back && back->gap_ms == 6000);

  // Kaniel's game is gone after a minute without a word; the build is free
  // again once nobody is linked.
  hub.Receive(Ada, Encode(Kind::Ping, Ping{2, 64000}), 64000);
  result = hub.Tick(64001);
  CHECK(SentTo(result, Ada).size() == 1); // gone, not quiet first
  const auto gone = DecodeMember(SentTo(result, Ada).value(0));
  CHECK(gone && gone->state == MemberState::Gone &&
        gone->online_id == "Kaniel");
  CHECK(hub->Members() == 1);
  hub.Tick(200000);
  CHECK(hub->Members() == 0 && hub->Build().isEmpty());
  result = hub.Receive(Eileen, HelloFrom("Eileen", OlderBuild), 200001);
  CHECK(DecodeWelcome(SentTo(result, Eileen).value(0))->accepted);

  // The counters every 10 s.
  Net counted;
  counted.Receive(Kaniel, HelloFrom("Kaniel"), 0);
  counted.Receive(Ada, HelloFrom("Ada"), 0);
  counted.Receive(Kaniel, Encode(Data{VirtualEndpointOf("Ada"), GameFrame}), 1);
  counted.Receive(Kaniel, Encode(Kind::Ping, Ping{1, 1}), 9000);
  counted.Receive(Ada, Encode(Kind::Ping, Ping{1, 1}), 9000);
  CHECK(counted.Tick(9999).log.isEmpty());
  const auto counters = counted.Tick(10000).log;
  CHECK(counters.size() == 2);
  CHECK(counters.join('\n').contains("Kaniel") &&
        counters.join('\n').contains("Ada"));
  CHECK(hub.unopened == 0);
  std::cout << "server link liveness: ok\n";
  return 0;
}

// Phase 7: the relay and the member notices stay inside the sender's team.
int TeamOnly() {
  Net hub;
  const Endpoint Lone{0x7F000001, 40004};
  hub->SetPartyOf([&](const Endpoint &link) -> QString {
    if (link == Kaniel || link == Ada) {
      return QStringLiteral("team-a");
    }
    return link == Eileen ? QStringLiteral("team-b") : QString{};
  });
  hub.Receive(Kaniel, HelloFrom("Kaniel"), 0);
  hub.Receive(Ada, HelloFrom("Ada"), 0);
  hub.Receive(Eileen, HelloFrom("Eileen"), 0);
  hub.Receive(Lone, HelloFrom("Lone"), 0);

  auto result = hub.Receive(
      Kaniel, Encode(Data{VirtualEndpointOf("Ada"), GameFrame}), 10);
  CHECK(SentTo(result, Ada).size() == 1);
  // Another team's game, or one in no team yet: dropped, and said.
  result = hub.Receive(
      Kaniel, Encode(Data{VirtualEndpointOf("Eileen"), GameFrame}), 20);
  CHECK(result.sends.isEmpty());
  CHECK(!result.log.isEmpty() && result.log[0].contains("another team"));
  result =
      hub.Receive(Lone, Encode(Data{VirtualEndpointOf("Ada"), GameFrame}), 30);
  CHECK(result.sends.isEmpty());
  result =
      hub.Receive(Ada, Encode(Data{VirtualEndpointOf("Lone"), GameFrame}), 40);
  CHECK(result.sends.isEmpty());

  // Eileen going quiet is told to nobody in team a.
  for (const Endpoint &game : {Kaniel, Ada, Lone}) {
    hub.Receive(game, Encode(Kind::Ping, Ping{1, 3000}), 3000);
  }
  result = hub.Tick(3500);
  CHECK(result.sends.isEmpty());
  // Ada going quiet is told to Kaniel only.
  for (const Endpoint &game : {Kaniel, Eileen, Lone}) {
    hub.Receive(game, Encode(Kind::Ping, Ping{2, 6000}), 6000);
  }
  result = hub.Tick(7000);
  CHECK(SentTo(result, Kaniel).size() == 1 && result.sends.size() == 1);
  CHECK(hub.unopened == 0);
  std::cout << "server link team only: ok\n";
  return 0;
}

// Phase 7: no game learns another's address. Each game is known by a virtual
// address made from its account (10.x.y.z, one port), the same from any port
// it links from; the hub forwards between the virtual addresses.
int VirtualMembers() {
  Net hub;
  const Endpoint ada_virtual = VirtualEndpointOf("Ada");
  const Endpoint kaniel_virtual = VirtualEndpointOf("Kaniel");
  CHECK((ada_virtual.ipv4 >> 24) == 10 && ada_virtual.port == VirtualPort);
  CHECK(!(ada_virtual == kaniel_virtual));
  CHECK(VirtualEndpointOf("Ada") == ada_virtual); // the same every time
  // The games compute the same (emulator/tests/network/test_net_link.cpp).
  CHECK(VirtualEndpointOf("Kaniel").ipv4 == 0x0A89C805u);
  CHECK(ada_virtual.ipv4 == 0x0AC3A55Bu);
  CHECK(!hub->VirtualOf(Ada)); // not linked yet
  hub.Receive(Kaniel, HelloFrom("Kaniel"), 0);
  // A game welcomed is said with its virtual address: the rooms and signaling
  // hand that one out (no STUN needed).
  const auto linked = hub.Receive(Ada, HelloFrom("Ada"), 0).linked;
  CHECK(linked.size() == 1 && linked[0].online_id == "Ada" &&
        linked[0].virtual_endpoint == ada_virtual);
  CHECK(hub->VirtualOf(Ada) == ada_virtual);

  // Sent to Ada's virtual address: Ada gets it from Kaniel's virtual address.
  auto result = hub.Receive(Kaniel, Encode(Data{ada_virtual, GameFrame}), 10);
  const auto forwarded = DecodeData(SentTo(result, Ada).value(0));
  CHECK(forwarded && forwarded->peer == kaniel_virtual &&
        forwarded->frame == GameFrame);
  // Phase 9: a real address finds nobody, even a linked game's.
  result = hub.Receive(Kaniel, Encode(Data{Ada, GameFrame}), 15);
  CHECK(result.sends.isEmpty());
  CHECK(!result.log.isEmpty() && result.log[0].contains("no game"));

  // Ada's game links again from another port: still the same address.
  const Endpoint ada_moved{0x7F000001, 40099};
  // Its old link is forgotten (the world's member there goes), told to nobody:
  // Ada is still here.
  result = hub.Receive(ada_moved, HelloFrom("Ada"), 20);
  CHECK(result.gone.size() == 1 && result.gone[0] == Ada);
  CHECK(SentTo(result, Kaniel).isEmpty() && hub->Members() == 2);
  CHECK(hub->VirtualOf(ada_moved) == ada_virtual && !hub->VirtualOf(Ada));
  result = hub.Receive(Kaniel, Encode(Data{ada_virtual, GameFrame}), 30);
  CHECK(SentTo(result, ada_moved).size() == 1 && result.sends.size() == 1);

  // The notices name the virtual address too.
  hub.Receive(Kaniel, Encode(Kind::Ping, Ping{1, 3000}), 3000);
  result = hub.Tick(3500);
  const auto quiet = DecodeMember(SentTo(result, Kaniel).value(0));
  CHECK(quiet && quiet->state == MemberState::Quiet &&
        quiet->endpoint == ada_virtual);
  CHECK(hub.unopened == 0);
  std::cout << "server link virtual members: ok\n";
  return 0;
}

// Phase 9: the hub reads each game packet in a frame (SHAD GamePacketsType)
// and forwards only what the sender's routes let reach that member: a refused
// unreliable packet leaves the frame, a refused reliable one keeps its
// sequence number (coop/packet_routes.h). Other frames go as they are.
QByteArray GamePacketsFrame(const BloodborneCoop::GameFrame &frame) {
  QByteArray bytes = QByteArray::fromHex("ffffffff") + "SHAD" +
                     char(BloodborneCoop::GamePacketsType);
  const auto payload = BloodborneCoop::EncodeGameFrame(frame);
  bytes.append(reinterpret_cast<const char *>(payload.data()),
               static_cast<qsizetype>(payload.size()));
  return bytes;
}

std::vector<u8> PacketOn(u32 channel) {
  std::vector<u8> packet;
  BloodborneCoop::GameLinkDetail::Put(packet, channel);
  BloodborneCoop::GameLinkDetail::Put(packet, u32{2});
  packet.push_back(0xAB);
  packet.push_back(0xCD);
  return packet;
}

int RoutesGamePackets() {
  using BloodborneCoop::PacketClass;
  Net hub;
  std::vector<std::tuple<Endpoint, Endpoint, u32>> asked;
  hub->SetPacketRoutes(
      [&](const Endpoint &from, const Endpoint &to, u32 channel) {
        asked.emplace_back(from, to, channel);
        return channel != 0x06 && channel != 0x31;
      });
  hub.Receive(Kaniel, HelloFrom("Kaniel"), 0);
  hub.Receive(Ada, HelloFrom("Ada"), 0);

  BloodborneCoop::GameFrame frame{"Kaniel", 5, 6, 2, {}};
  frame.entries.push_back({PacketClass::Unreliable, 0, PacketOn(0x06)});
  frame.entries.push_back({PacketClass::Unreliable, 0, PacketOn(0x04)});
  frame.entries.push_back({PacketClass::Reliable, 4, PacketOn(0x31)});
  frame.entries.push_back({PacketClass::Reliable, 5, PacketOn(0x10)});
  auto result = hub.Receive(
      Kaniel, Encode(Data{VirtualEndpointOf("Ada"), GamePacketsFrame(frame)}),
      10);
  CHECK(asked.size() == 4 && std::get<0>(asked[0]) == Kaniel &&
        std::get<1>(asked[0]) == Ada && std::get<2>(asked[0]) == 0x06);
  const auto data = DecodeData(SentTo(result, Ada).value(0));
  CHECK(data &&
        data->frame.startsWith(QByteArray::fromHex("ffffffff") + "SHAD" +
                               char(BloodborneCoop::GamePacketsType)));
  const auto routed = BloodborneCoop::DecodeGameFrame(
      {reinterpret_cast<const u8 *>(data->frame.constData()) + 9,
       static_cast<size_t>(data->frame.size() - 9)});
  CHECK(routed && routed->sender == "Kaniel" && routed->ack == 2 &&
        routed->entries.size() == 3);
  CHECK(routed->entries[0].packet == PacketOn(0x04));
  CHECK(routed->entries[1].seq == 4 &&
        routed->entries[1].packet == BloodborneCoop::DroppedNativePacket());
  CHECK(routed->entries[2].packet == PacketOn(0x10));

  // Nothing left but its ack: the frame still goes (the stream needs the ack).
  BloodborneCoop::GameFrame only_refused{"Kaniel", 5, 6, 3, {}};
  only_refused.entries.push_back({PacketClass::Unreliable, 0, PacketOn(0x06)});
  result = hub.Receive(
      Kaniel,
      Encode(Data{VirtualEndpointOf("Ada"), GamePacketsFrame(only_refused)}),
      20);
  CHECK(SentTo(result, Ada).size() == 1);

  // A frame that is not the game's packets is not read.
  asked.clear();
  result = hub.Receive(Kaniel,
                       Encode(Data{VirtualEndpointOf("Ada"), ::GameFrame}), 30);
  CHECK(asked.empty() &&
        DecodeData(SentTo(result, Ada).value(0))->frame == ::GameFrame);
  // A game frame that does not read is dropped, and said.
  result = hub.Receive(
      Kaniel,
      Encode(Data{VirtualEndpointOf("Ada"),
                  QByteArray::fromHex("ffffffff") + "SHAD" +
                      char(BloodborneCoop::GamePacketsType) + "\x01"}),
      40);
  CHECK(result.sends.isEmpty());
  CHECK(!result.log.isEmpty() &&
        result.log.join('\n').contains("does not read"));

  // The counters say how much each sender had refused, per channel.
  hub.Receive(Kaniel, Encode(Kind::Ping, Ping{1, 1}), 9000);
  hub.Receive(Ada, Encode(Kind::Ping, Ping{1, 1}), 9000);
  const QString counters = hub.Tick(10000).log.join('\n');
  CHECK(counters.contains("refused") && counters.contains("0x6: 2") &&
        counters.contains("0x31: 1"));
  CHECK(hub.unopened == 0);
  std::cout << "server link routes game packets: ok\n";
  return 0;
}
// The cookies and the work (coop/link_auth.h): SHA-256 and HMAC as the
// standards say, and as Qt computes them.
int Crypto() {
  const auto hex = [](const LinkAuth::Digest &digest) {
    return QByteArray(reinterpret_cast<const char *>(digest.data()), 32)
        .toHex();
  };
  CHECK(hex(LinkAuth::Sha256::Of(LinkAuth::Bytes("abc"))) ==
        "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
  // RFC 4231, test case 2.
  CHECK(hex(LinkAuth::HmacKey{LinkAuth::Bytes("Jefe")}.Mac(
            LinkAuth::Bytes("what do ya want for nothing?"))) ==
        "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843");
  QByteArray key, message;
  for (int size = 0; size < 300; ++size) {
    key.append(char(size * 7));
    message.append(char(size * 13 + 1));
    const auto ours = LinkAuth::HmacKey{Span(key.left(size % 90))}.Mac(
        Span(message.left(size / 2)), Span(message.mid(size / 2)));
    CHECK(hex(ours) ==
          QMessageAuthenticationCode::hash(message, key.left(size % 90),
                                           QCryptographicHash::Sha256)
              .toHex());
  }
  std::cout << "server link crypto: ok\n";
  return 0;
}

// Link version 4 (coop/link_auth.h): Monocypher's X25519, ChaCha20-Poly1305
// and BLAKE2b as their standards' examples say.
int Noise() {
  namespace Mono = MONOCYPHER_CPP_NAMESPACE;
  const auto key = [](const char *hex) {
    LinkAuth::Key out;
    const QByteArray bytes = QByteArray::fromHex(hex);
    std::memcpy(out.data(), bytes.constData(), out.size());
    return out;
  };
  // RFC 7748, section 6.1.
  const auto alice =
      key("77076d0a7318a57d3c16c17251b26645df4c2f87ebc0992ab177fba51db92c2a");
  const auto bob =
      key("5dab087e624a8a4b79e17f8b83800ee66f3bb1292618b6fd1c2f8b27ff88e0eb");
  CHECK(ToQt(LinkAuth::Noise::PublicOf(alice)).toHex() ==
        "8520f0098930a754748b7ddcb43ef75a0dbf3a0d26381af4eba4a98eaa9b4e6a");
  CHECK(ToQt(*LinkAuth::Noise::Dh(bob, LinkAuth::Noise::PublicOf(alice)))
            .toHex() ==
        "4a5d9d5ba4ce2de1728e3bf480350f25e07e21c947d19e3376f09b3c1e161742");
  // A small-order point shares no secret.
  CHECK(!LinkAuth::Noise::Dh(alice, LinkAuth::Key{}));
  // RFC 8439, section 2.8.2.
  QByteArray text = "Ladies and Gentlemen of the class of '99: If I could "
                    "offer you only one tip for the future, sunscreen would "
                    "be it.";
  const QByteArray ad = QByteArray::fromHex("50515253c0c1c2c3c4c5c6c7");
  const QByteArray nonce = QByteArray::fromHex("070000004041424344454647");
  LinkAuth::Key aead_key;
  for (size_t index = 0; index < aead_key.size(); ++index) {
    aead_key[index] = static_cast<u8>(0x80 + index);
  }
  Mono::crypto_aead_ctx ctx;
  Mono::crypto_aead_init_ietf(&ctx, aead_key.data(),
                              reinterpret_cast<const u8 *>(nonce.constData()));
  u8 tag[16];
  Mono::crypto_aead_write(&ctx, MutableSpan(text).data(), tag,
                          reinterpret_cast<const u8 *>(ad.constData()),
                          ad.size(), MutableSpan(text).data(), text.size());
  CHECK(text.left(16).toHex() == "d31a8d34648e60db7b86afbc53ef7ec2");
  CHECK(QByteArray(reinterpret_cast<const char *>(tag), 16).toHex() ==
        "1ae10b594f09e26a7e902ecbd0600691");
  CHECK(ToQt(LinkAuth::Noise::Blake2b(LinkAuth::Bytes("abc"))).toHex() ==
        "ba80a53f981c4d0d6a2797b69f12f6e94c212f14685ac4b74b12bb6fdbffa2d17d87"
        "c5392aab792dc252d5de4533cc9518d38aa8dbf1925ab92386edd4009923");
  // Key text both ways.
  CHECK(LinkAuth::KeyFromHex(LinkAuth::HexOf(alice)) == alice);
  CHECK(!LinkAuth::KeyFromHex("abc") &&
        !LinkAuth::KeyFromHex(std::string(64, 'g')));
  std::cout << "server link noise: ok\n";
  return 0;
}

// Link version 4: a hello needs a cookie of its source address and port (no
// state kept, the answer smaller than the hello); the hello that echoes it is
// the handshake, which opens only with the server's key; after it every
// datagram is encrypted with its link's keys, and one that does not open is
// dropped without a word, counted.
int Handshake() {
  Hub hub;
  int checks = 0;
  hub.SetSignIn([&](const QByteArray &, const QByteArray &secret, bool) {
    ++checks;
    return SignInVerdict{secret == "s3cr3t", {}};
  });
  int admitted = 0;
  hub.SetAdmit([&](const Endpoint &, qsizetype) {
    ++admitted;
    return true;
  });
  LinkAuth::Client game;
  game.Pin(hub.PublicKey());
  const QByteArray plain = HelloFrom("Kaniel");

  // The first hello: a cookie back with the server's key, nothing else (a
  // spoofed source never sees it: no link, no sign-in, nothing kept). It says
  // nothing secret.
  const QByteArray first = Sealed(game, plain);
  CHECK(first.size() == static_cast<qsizetype>(LinkAuth::FirstHelloBytes) &&
        !first.contains("s3cr3t") && !first.contains("Kaniel"));
  auto result = hub.Receive(Kaniel, first, 1000);
  CHECK(result.sends.size() == 1 && result.log.isEmpty());
  CHECK(result.sends[0].datagram.size() < first.size());
  CHECK(hub.Members() == 0 && checks == 0 && hub.CookiesSent() == 1);
  // Shorter than that: not even a cookie (the answer would be larger).
  CHECK(
      hub.Receive(Kaniel, first.left(first.size() - 1), 1000).sends.isEmpty());
  CHECK(TakeCookie(game, result.sends[0].datagram));

  // The cookie holds for its source only, and for 30-60 s.
  const QByteArray second = Sealed(game, plain);
  CHECK(Sealed(game, plain) == second); // said again: the same datagram
  CHECK(!second.contains("s3cr3t") && !second.contains("Kaniel") &&
        !second.contains(Build));
  CHECK(hub.CookieValid(Kaniel, second, 1000));
  CHECK(hub.CookieValid(Kaniel, second, 59'999));
  CHECK(!hub.CookieValid(Kaniel, second, 60'000));
  CHECK(!hub.CookieValid(Ada, second, 1000));
  CHECK(!hub.CookieValid(Endpoint{Kaniel.ipv4 + 1, Kaniel.port}, second, 1000));
  CHECK(!hub.CookieValid(Kaniel, first, 1000));
  result = hub.Receive(Ada, second, 1000); // someone else's cookie
  CHECK(KindOf(SentTo(result, Ada).value(0)) == Kind::Cookie &&
        hub.Members() == 0 && checks == 0);
  Hub restarted; // another key: the cookies of before hold nothing
  CHECK(!restarted.CookieValid(Kaniel, second, 1000));

  // The handshake changed on the way, or sealed to another server's key: it
  // does not open; no sign-in, no answer, counted.
  QByteArray changed = second;
  changed[40] = char(changed[40] ^ 1);
  CHECK(hub.Receive(Kaniel, changed, 1500).sends.isEmpty());
  changed = second;
  changed[changed.size() - 30] = char(changed[changed.size() - 30] ^ 1);
  CHECK(hub.Receive(Kaniel, changed, 1500).sends.isEmpty());
  LinkAuth::Client other_server;
  other_server.Pin(restarted.PublicKey());
  CHECK(other_server.TakeCookie(game.CookieToEcho(), restarted.PublicKey()));
  CHECK(hub.Receive(Kaniel, Sealed(other_server, plain), 1500).sends.isEmpty());
  CHECK(checks == 0 && hub.Members() == 0 && hub.BadHellos() == 3);

  // The cookie echoed: signed in, and the welcome sealed for this game.
  result = hub.Receive(Kaniel, second, 2000);
  CHECK(checks == 1 && hub.Members() == 1);
  const QByteArray welcome = SentTo(result, Kaniel).value(0);
  // Another game's answer, a changed one, or one made with another key: not
  // this game's welcome.
  LinkAuth::Client bystander;
  bystander.Pin(hub.PublicKey());
  bystander.TakeCookie(game.CookieToEcho(), hub.PublicKey());
  Sealed(bystander, plain);
  CHECK(!bystander.Open(Span(welcome)) && !bystander.Linked());
  QByteArray forged_welcome = welcome;
  forged_welcome[20] = char(forged_welcome[20] ^ 1);
  CHECK(!game.Open(Span(forged_welcome)) && !game.Linked());
  std::vector<u8> unused;
  // (the server's answer under another static key, to this game's hello)
  LinkAuth::ServerKey impostor = LinkAuth::ServerKeyOf(Filled(0x77));
  impostor.pub = hub.PublicKey(); // it says the real key, but holds another
  CHECK(!LinkAuth::Responder::Open(impostor, Span(second), unused));
  const auto opened = game.Open(Span(welcome));
  CHECK(opened && DecodeWelcome(ToQt(*opened))->accepted && game.Linked());
  CHECK(!game.Open(Span(welcome))); // its handshake is done

  const auto sealed = [&](QByteArray datagram) {
    datagram += Bytes(*game.Seal(MutableSpan(datagram)));
    return datagram;
  };
  const QByteArray ping = sealed(Encode(Kind::Ping, Ping{1, 1}));
  result = hub.Receive(Kaniel, ping, 2100);
  CHECK(SentTo(result, Kaniel).size() == 1 && admitted == 1);
  const auto pong = game.Open(Span(SentTo(result, Kaniel)[0]));
  CHECK(pong && DecodePing(Kind::Pong, ToQt(*pong))->seq == 1);

  // The same datagram again (a replay): dropped, counted.
  result = hub.Receive(Kaniel, ping, 2200);
  CHECK(result.sends.isEmpty() && result.log.isEmpty() && hub.Replays() == 1 &&
        admitted == 1);
  // Its body changed, its tag, or none (someone sending from the game's
  // address): dropped before its rates count it.
  const QByteArray coop = sealed(EncodeCoop("restart"));
  CHECK(!coop.contains("restart")); // encrypted
  QByteArray forged = coop;
  forged[10] = char(forged[10] ^ 1);
  result = hub.Receive(Kaniel, forged, 2300);
  CHECK(result.coop.isEmpty() && result.sends.isEmpty() && hub.BadTags() == 1);
  forged = coop;
  forged[forged.size() - 1] = char(forged[forged.size() - 1] ^ 1);
  CHECK(hub.Receive(Kaniel, forged, 2300).coop.isEmpty() && hub.BadTags() == 2);
  forged = coop;
  forged[8] = char(Kind::Rpc); // its kind, in clear, is authenticated too
  CHECK(hub.Receive(Kaniel, forged, 2300).rpc.isEmpty() && hub.BadTags() == 3);
  result =
      hub.Receive(Kaniel, EncodeRpc("injected" + QByteArray(30, 'x')), 2400);
  CHECK(result.rpc.isEmpty() && hub.BadTags() == 4 && admitted == 1);
  QByteArray moved_bytes = sealed(EncodeCoop("body"));
  result = hub.Receive(Ada, moved_bytes, 2450); // from another source
  CHECK(result.coop.isEmpty());
  // One that opens: the stream goes on.
  result = hub.Receive(Kaniel, coop, 2500);
  CHECK(result.coop.size() == 1 && result.coop[0].body == "restart");

  // Late datagrams within the window are taken, once; older ones are not.
  QList<QByteArray> burst;
  for (int index = 0; index < 1100; ++index) {
    burst.append(sealed(EncodeCoop(QByteArray::number(index))));
  }
  CHECK(hub.Receive(Kaniel, burst[1099], 3000).coop.size() == 1);
  CHECK(hub.Receive(Kaniel, burst[500], 3000).coop.size() == 1);
  CHECK(hub.Receive(Kaniel, burst[500], 3000).coop.isEmpty());
  CHECK(hub.Receive(Kaniel, burst[10], 3000).coop.isEmpty());
  CHECK(hub.Replays() == 3);
  CHECK(hub.Receive(Kaniel, burst[1098], 3000).coop.size() == 1);

  // The hello repeated (its welcome lost): the same welcome, one session, its
  // counters kept, so a datagram of before is still a replay.
  result = hub.Receive(Kaniel, second, 3100);
  CHECK(SentTo(result, Kaniel).value(0) == welcome);
  CHECK(hub.Receive(Kaniel, burst[1099], 3200).coop.isEmpty());
  CHECK(game.Open(Span(*hub.Seal(Kaniel, EncodeCoop("x")))).has_value());

  // A refusal is sealed for the game that asked, a hello without a cookie
  // gets none.
  CHECK(!hub.Refusal(Kaniel, first, "too many", 3300));
  LinkAuth::Client fresh;
  fresh.Pin(hub.PublicKey());
  fresh.TakeCookie(game.CookieToEcho(), hub.PublicKey());
  const QByteArray asked = Sealed(fresh, plain);
  const auto refusal = hub.Refusal(Kaniel, asked, "too many", 3300);
  const auto heard = refusal ? fresh.Open(Span(*refusal)) : std::nullopt;
  CHECK(heard && DecodeWelcome(ToQt(*heard))->text == "too many" &&
        !fresh.Linked() && !game.Open(Span(*refusal)));
  // What the hub sends a game outside its answers (the worlds, Rpc): sealed
  // for a linked game, nothing for another.
  CHECK(!hub.Seal(Ada, EncodeCoop("x")));
  const auto rpc = game.Open(Span(*hub.Seal(Kaniel, EncodeRpc("secret rpc"))));
  CHECK(rpc && DecodeRpc(ToQt(*rpc)) == "secret rpc");
  CHECK(!hub.Seal(Kaniel, EncodeRpc("secret rpc"))->contains("secret"));

  // A game with no key from its launcher takes the first cookie's, and keeps
  // it once a welcome proves the server holds it (trust on first use); then a
  // cookie with another key is not taken.
  LinkAuth::Client home;
  result = hub.Receive(Eileen, Sealed(home, HelloFrom("Eileen")), 4000);
  CHECK(TakeCookie(home, SentTo(result, Eileen).value(0)) && !home.Pinned());
  result = hub.Receive(Eileen, Sealed(home, HelloFrom("Eileen")), 4000);
  CHECK(home.Open(Span(SentTo(result, Eileen).value(0))) && home.Pinned());
  CHECK(home.TakeLearned() == hub.PublicKey() && !home.TakeLearned());
  CHECK(!home.TakeCookie(game.CookieToEcho(), restarted.PublicKey()));
  // A game pinned to another key takes no cookie of this server.
  LinkAuth::Client pinned;
  pinned.Pin(restarted.PublicKey());
  result = hub.Receive(Eileen, Sealed(pinned, HelloFrom("Eileen")), 4100);
  CHECK(!TakeCookie(pinned, SentTo(result, Eileen).value(0)) &&
        !pinned.HasCookie());
  std::cout << "server link handshake: ok\n";
  return 0;
}

// LinkKeyFile: the server's key made the first time, only its user may read
// it, and the same key every start after.
int KeyFile() {
  QTemporaryDir folder;
  const QString path = folder.filePath("link_key.bin");
  bool made = false;
  QString why;
  const auto key = KeyFromFile(path, &made, &why);
  CHECK(key && made && QFileInfo{path}.size() == 32);
#ifndef Q_OS_WIN
  // (Windows: Qt's permission bits are no ACL; a hosted server runs on Linux.)
  const auto permissions = QFile::permissions(path);
  CHECK(!(permissions & (QFileDevice::ReadGroup | QFileDevice::ReadOther |
                         QFileDevice::WriteGroup | QFileDevice::WriteOther)));
#endif
  CHECK(KeyFromFile(path, &made, &why) == key && !made);
  QFile file{path};
  CHECK(file.open(QIODevice::Append) && file.write("x") == 1);
  file.close();
  CHECK(!KeyFromFile(path, &made, &why) && why.contains("not a key"));
  std::cout << "server link key file: ok\n";
  return 0;
}

// Hosted: a hello for an id with no account must carry work (coop/link_auth.h)
// for its cookie and id; the hub checks it with one hash before the sign-in,
// an account already made never pays, and a hello without it hears how much.
int SignupWork() {
  const auto hex = [](const LinkAuth::Digest &digest) {
    return QByteArray(reinterpret_cast<const char *>(digest.data()), 32)
        .toHex();
  };
  LinkAuth::Digest digest{};
  CHECK(LinkAuth::LeadingZeroBits(digest) == 256);
  digest[0] = 0x01;
  CHECK(LinkAuth::LeadingZeroBits(digest) == 7);
  digest[0] = 0;
  digest[1] = 0x20;
  CHECK(LinkAuth::LeadingZeroBits(digest) == 10);
  LinkAuth::Cookie some{};
  some.fill(0x11);
  // SHA-256 of the cookie, the id and the nonce (8 bytes, little-endian).
  CHECK(hex(LinkAuth::WorkDigest(LinkAuth::WorkPrefix(some, "Kaniel"),
                                 0x0807060504030201)) ==
        QCryptographicHash::hash(QByteArray(16, 0x11) + "Kaniel" +
                                     QByteArray::fromHex("0102030405060708"),
                                 QCryptographicHash::Sha256)
            .toHex());
  const auto solved = LinkAuth::SolveWork(some, "Kaniel", 12, 0, 1 << 22);
  CHECK(solved && LinkAuth::WorkDone(some, "Kaniel", *solved, 12));
  CHECK(!LinkAuth::WorkDone(some, "Ada", *solved, 12) ||
        !LinkAuth::WorkDone(some, "Ada", *solved + 1, 12));
  CHECK(!LinkAuth::SolveWork(some, "Kaniel", 41, 0, 1 << 20));
  CHECK(LinkAuth::WorkDone(some, "Kaniel", 123, 0));

  Hub hub;
  hub.SetSignupWork(10);
  QList<std::tuple<QByteArray, bool>> asked;
  hub.SetSignIn([&](const QByteArray &online_id, const QByteArray &,
                    bool paid) -> std::optional<SignInVerdict> {
    asked.append({online_id, paid});
    if (online_id == "Kaniel") {
      return SignInVerdict{true, {}}; // an account already
    }
    if (!paid) {
      return SignInVerdict{false, {}, true};
    }
    return SignInVerdict{true, {}};
  });
  LinkAuth::Client game;
  const auto hello = [&](const char *who, std::optional<quint64> work) {
    Hello said{LinkVersion, who, Build, "s3cr3t", {}};
    said.work = work;
    return Sealed(game, Encode(said));
  };

  // The cookie says the work a new account needs.
  auto result = hub.Receive(Ada, hello("Ada", {}), 1000);
  int bits = -1;
  const auto cookie = DecodeCookie(SentTo(result, Ada).value(0), &bits);
  CHECK(cookie && bits == 10 && asked.isEmpty());
  CHECK(TakeCookie(game, SentTo(result, Ada).value(0)));

  // No work: the game hears how much, sealed for its hello (a spoofed one
  // cannot make a game spend its CPU); no account, no link.
  result = hub.Receive(Ada, hello("Ada", {}), 1100);
  CHECK(
      (asked.size() == 1 && asked[0] == std::tuple{QByteArray{"Ada"}, false}));
  QByteArray answer = SentTo(result, Ada).value(0);
  QByteArray spoofed = EncodeWork(30);
  CHECK(!game.Open(Span(spoofed)));
  auto opened = game.Open(Span(answer));
  CHECK(opened && DecodeWork(ToQt(*opened)) == 10 && !game.Linked());
  CHECK(hub.Members() == 0 && hub.WorkAsked() == 1);
  CHECK(game.WorkAsked(10) && game.Working());
  CHECK(!game.WorkAsked(10)); // asked again while under way

  // Work not done (a wrong nonce, or one for another id): not paid.
  const auto right = *LinkAuth::SolveWork(*cookie, "Ada", 10, 0, 1 << 22);
  quint64 wrong = 0;
  while (LinkAuth::WorkDone(*cookie, "Ada", wrong, 10)) {
    ++wrong;
  }
  const auto other_id = *LinkAuth::SolveWork(*cookie, "Eileen", 10, 0, 1 << 22);
  result = hub.Receive(Ada, hello("Ada", wrong), 1200);
  CHECK(std::get<1>(asked.last()) == false && hub.WorkAsked() == 2);
  if (!LinkAuth::WorkDone(*cookie, "Ada", other_id, 10)) {
    result = hub.Receive(Ada, hello("Ada", other_id), 1250);
    CHECK(std::get<1>(asked.last()) == false && hub.Members() == 0);
  }
  // Done for another source's cookie: that cookie is not this source's.
  game.TakeWork(*cookie, 10, right);
  CHECK(!game.Working() && game.WorkToSend() == right);
  result = hub.Receive(Eileen, hello("Ada", right), 1300);
  CHECK(KindOf(SentTo(result, Eileen).value(0)) == Kind::Cookie &&
        hub.Members() == 0);

  // The work done: paid, signed in, welcomed (the hello with it is a new
  // handshake: its bytes changed).
  const int before = static_cast<int>(asked.size());
  result = hub.Receive(Ada, hello("Ada", game.WorkToSend()), 1400);
  CHECK((asked.size() == before + 1 &&
         asked.last() == std::tuple{QByteArray{"Ada"}, true}));
  opened = game.Open(Span(SentTo(result, Ada).value(0)));
  CHECK(opened && DecodeWelcome(ToQt(*opened))->accepted && game.Linked());
  CHECK(hub.Members() == 1);

  // An account already made: no work asked, welcomed at once.
  LinkAuth::Client kaniel;
  result = hub.Receive(Kaniel, Sealed(kaniel, HelloFrom("Kaniel")), 2000);
  CHECK(TakeCookie(kaniel, SentTo(result, Kaniel).value(0)));
  result = hub.Receive(Kaniel, Sealed(kaniel, HelloFrom("Kaniel")), 2100);
  opened = kaniel.Open(Span(SentTo(result, Kaniel).value(0)));
  CHECK(opened && DecodeWelcome(ToQt(*opened))->accepted);
  CHECK((asked.last() == std::tuple{QByteArray{"Kaniel"}, false}));
  CHECK(hub.WorkAsked() == 2 || hub.WorkAsked() == 3);

  // No work asked (a host's own server): every hello may make its account.
  Hub open;
  bool paid = false;
  open.SetSignIn([&](const QByteArray &, const QByteArray &, bool p) {
    paid = p;
    return SignInVerdict{true, {}};
  });
  LinkAuth::Client home;
  result = open.Receive(Kaniel, Sealed(home, HelloFrom("Kaniel")), 0);
  bits = -1;
  CHECK(DecodeCookie(SentTo(result, Kaniel).value(0), &bits) && bits == 0);
  CHECK(TakeCookie(home, SentTo(result, Kaniel)[0]));
  open.Receive(Kaniel, Sealed(home, HelloFrom("Kaniel")), 10);
  CHECK(paid && open.Members() == 1);
  std::cout << "server link signup work: ok\n";
  return 0;
}

// Invasions (docs/INVASIONS.md): an invader visiting team a shares its relay
// (and its quiet notices), but only the game's packets go between them, never
// our co-op frames; it still reaches no other team.
int AnInvaderSharesTheVisitedTeamsRelay() {
  Net hub;
  const Endpoint Red{0x7F000001, 40005};
  hub->SetPartyOf([&](const Endpoint &link) -> QString {
    if (link == Kaniel || link == Ada) {
      return QStringLiteral("team-a");
    }
    return link == Eileen ? QStringLiteral("team-b") : QString{};
  });
  hub->SetVisiting([&](const Endpoint &link) -> QString {
    return link == Red ? QStringLiteral("team-a") : QString{};
  });
  hub->SetPacketRoutes(
      [](const Endpoint &, const Endpoint &, u32) { return true; });
  for (const auto &[game, name] :
       {std::pair{Kaniel, "Kaniel"}, std::pair{Ada, "Ada"},
        std::pair{Eileen, "Eileen"}, std::pair{Red, "Red"}}) {
    hub.Receive(game, HelloFrom(name), 0);
  }
  BloodborneCoop::GameFrame frame{"Red", 5, 6, 2, {}};
  frame.entries.push_back(
      {BloodborneCoop::PacketClass::Unreliable, 0, PacketOn(0x06)});
  auto result = hub.Receive(
      Red, Encode(Data{VirtualEndpointOf("Kaniel"), GamePacketsFrame(frame)}),
      10);
  CHECK(SentTo(result, Kaniel).size() == 1);
  result = hub.Receive(
      Ada, Encode(Data{VirtualEndpointOf("Red"), GamePacketsFrame(frame)}), 20);
  CHECK(SentTo(result, Red).size() == 1);
  // Our own frames (another SHAD type) never, either way.
  const QByteArray coop =
      QByteArray::fromHex("ffffffff") + "SHAD" + char(0x41) + "vitals";
  result =
      hub.Receive(Red, Encode(Data{VirtualEndpointOf("Kaniel"), coop}), 30);
  CHECK(result.sends.isEmpty());
  result =
      hub.Receive(Kaniel, Encode(Data{VirtualEndpointOf("Red"), coop}), 40);
  CHECK(result.sends.isEmpty());
  // Team a among itself still carries them; team b never hears the invader.
  result =
      hub.Receive(Kaniel, Encode(Data{VirtualEndpointOf("Ada"), coop}), 50);
  CHECK(SentTo(result, Ada).size() == 1);
  result = hub.Receive(
      Red, Encode(Data{VirtualEndpointOf("Eileen"), GamePacketsFrame(frame)}),
      60);
  CHECK(result.sends.isEmpty());
  std::cout << "server link invader relay: ok\n";
  return 0;
}

// Two PCs with one player id (a copied install, player report 2026-10-24): the
// second one's hello no longer takes the link from the first while that one is
// still heard.
int TwoPCsOneId() {
  Net hub;
  const Endpoint home{0x1A0BABBA, 59881};
  const Endpoint friends_pc{0x1AFE174E, 50084};
  auto result = hub.Receive(home, HelloFrom("Eloise"), 0);
  CHECK(DecodeWelcome(SentTo(result, home).value(0))->accepted);
  result = hub.Receive(friends_pc, HelloFrom("Eloise"), 1000);
  const auto refused = DecodeWelcome(SentTo(result, friends_pc).value(0));
  CHECK(refused && !refused->accepted && refused->text.contains("another PC") &&
        refused->text.contains("coop.json"));
  CHECK(result.gone.isEmpty() && hub->Members() == 1 && hub->VirtualOf(home) &&
        !hub->VirtualOf(friends_pc));
  // Its link still works.
  hub.Receive(home, Encode(Kind::Ping, Ping{1, 2500}), 2500);
  result = hub.Receive(friends_pc, HelloFrom("Eloise"), 5000);
  CHECK(!DecodeWelcome(SentTo(result, friends_pc).value(0))->accepted);
  // The first PC gone quiet (its game closed, or its connection moved): the id
  // links from there.
  result = hub.Receive(friends_pc, HelloFrom("Eloise"), 5600);
  CHECK(DecodeWelcome(SentTo(result, friends_pc).value(0))->accepted);
  CHECK(result.gone.size() == 1 && result.gone[0] == home &&
        hub->Members() == 1);
  std::cout << "server link one id on two PCs: ok\n";
  return 0;
}
} // namespace

int main() {
  if (Contracts() != 0 || HelloAndBuilds() != 0 || BuildChecks() != 0 ||
      CompatibleVersions() != 0 ||
      Relays() != 0 || Liveness() != 0 || TeamOnly() != 0 ||
      VirtualMembers() != 0 || RoutesGamePackets() != 0 || SignsIn() != 0 ||
      ALongSilenceEndsTheSignIn() != 0 || TwoPCsOneId() != 0 || Crypto() != 0 ||
      Noise() != 0 || Handshake() != 0 || KeyFile() != 0 || SignupWork() != 0 ||
      AnInvaderSharesTheVisitedTeamsRelay() != 0) {
    return 1;
  }
  return 0;
}
