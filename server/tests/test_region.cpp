// SPDX-FileCopyrightText: Copyright 2026 shadNet Project
// SPDX-License-Identifier: GPL-2.0-or-later
// A region of several shards (docs/SCALING.md): the link's Redirect and the
// hello's ticket (tests/contracts/server_link_frames.txt, which the emulator's
// tests read too), the region tickets, where a hello goes when its shard is
// full, the team lobby across shards, and the directory's answers read.
#include <cstring>
#include <fstream>
#include <iostream>
#include <map>
#include <string>
#include <vector>

#include <QCryptographicHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include "coop/link_auth.h"
#include "coop/team_lobby.h"
#include "region.h"
#include "server_link.h"

using namespace ServerLink;
using BloodborneCoop::TeamLobby;
using Action = bbcoop::TeamRequest;

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

std::span<const u8> Span(const QByteArray &bytes) {
  return {reinterpret_cast<const u8 *>(bytes.constData()),
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

// The contracts' hello carries the text before the build fingerprint (main's
// #381); a game's hello says its fingerprint, else the build check refuses it.
const QByteArray ContractBuild = "92829ac4 events 1f protocol 9";
const QByteArray Build =
    "bb-build 1;version=1.2.0;revision=92829ac4;protocol=9;"
    "events=1f;harder=off;fps=60;";
const Endpoint Kaniel{0x7F000001, 40001};

// The Redirect (link kind 11) and the hello carrying its ticket, as the game
// writes and reads them.
int RedirectContracts() {
  const auto datagrams = ReadContracts(BLOODBORNE_SERVER_LINK_CONTRACTS);
  const auto datagram = [&](const char *name) { return datagrams.at(name); };
  const LinkAuth::Key key = LinkAuth::ServerKeyOf(Filled(0x44)).pub;
  const Redirect redirect{Endpoint{0x0A000002, 31316}, key,
                          QByteArray::fromHex("0102030405"), "full"};
  CHECK(Encode(redirect) == datagram("redirect"));
  CHECK(KindOf(datagram("redirect")) == Kind::Redirect);
  const auto read = DecodeRedirect(datagram("redirect"));
  CHECK(read && read->to == redirect.to && read->key == key &&
        read->ticket == redirect.ticket && read->text == "full");
  CHECK(!DecodeRedirect(datagram("redirect").chopped(1)));
  CHECK(!DecodeRedirect(datagram("ping")));

  Hello hello{LinkVersion, "Kaniel", ContractBuild, "s3cr3t",
              QByteArray(16, '\x11')};
  hello.ticket = "TICKET";
  CHECK(Encode(hello) == datagram("hello.ticket"));
  const auto decoded = DecodeHello(datagram("hello.ticket"));
  CHECK(decoded && decoded->ticket == "TICKET" && decoded->work == 0u &&
        decoded->secret == "s3cr3t");
  // The work and the ticket both.
  hello.work = 77;
  CHECK(DecodeHello(Encode(hello))->work == 77u &&
        DecodeHello(Encode(hello))->ticket == "TICKET");
  CHECK(DecodeHello(datagram("hello"))->ticket.isEmpty());
  CHECK(DecodeHello(datagram("hello.work"))->ticket.isEmpty());
  std::cout << "region redirect contracts: ok\n";
  return 0;
}

// A shard answers a hello with a Redirect (sealed as the handshake's answer,
// like a welcome) instead of linking it: the game opens it, and has no link.
int AHelloIsAnsweredWithARedirect() {
  Hub hub;
  QByteArray seen_ticket;
  hub.SetSignIn([&](const QByteArray &, const QByteArray &, bool) {
    seen_ticket = hub.HelloTicket();
    SignInVerdict verdict{true, {}};
    verdict.redirect = Redirect{Endpoint{0x0A000003, 31314}, Filled(9),
                                "the ticket", "this shard is full"};
    return std::optional{verdict};
  });
  LinkAuth::Client game;
  game.Pin(hub.PublicKey());
  Hello hello{LinkVersion, "Kaniel", Build, "s3cr3t", {}};
  hello.ticket = "came with this";
  auto plain = Encode(hello);
  auto result =
      hub.Receive(Kaniel, ToQt(game.SealHello(Span(plain), Filled(7))), 0);
  LinkAuth::Key said{};
  const auto cookie =
      DecodeCookie(result.sends.value(0).datagram, nullptr, &said);
  CHECK(cookie && game.TakeCookie(*cookie, said));
  hello.cookie = ToQt(*cookie);
  plain = Encode(hello);
  result =
      hub.Receive(Kaniel, ToQt(game.SealHello(Span(plain), Filled(8))), 10);
  CHECK(seen_ticket == "came with this");
  CHECK(hub.HelloTicket().isEmpty()); // only while the sign-in reads it
  CHECK(result.sends.size() == 1 && result.linked.isEmpty() &&
        hub.Members() == 0);
  const auto opened = game.Open(Span(result.sends[0].datagram));
  CHECK(opened && !game.Linked());
  const auto redirect = DecodeRedirect(ToQt(*opened));
  CHECK(redirect && redirect->to == (Endpoint{0x0A000003, 31314}) &&
        redirect->ticket == "the ticket" && redirect->key == Filled(9));
  std::cout << "region hello redirected: ok\n";
  return 0;
}

// After a welcome, a Redirect goes on the link's session: the game opens it
// with the link's keys.
int ALinkedGameIsRedirectedOnItsSession() {
  Hub hub;
  LinkAuth::Client game;
  game.Pin(hub.PublicKey());
  Hello hello{LinkVersion, "Kaniel", Build, "s3cr3t", {}};
  auto plain = Encode(hello);
  auto result =
      hub.Receive(Kaniel, ToQt(game.SealHello(Span(plain), Filled(7))), 0);
  LinkAuth::Key said{};
  const auto cookie =
      DecodeCookie(result.sends.value(0).datagram, nullptr, &said);
  CHECK(cookie && game.TakeCookie(*cookie, said));
  hello.cookie = ToQt(*cookie);
  plain = Encode(hello);
  result =
      hub.Receive(Kaniel, ToQt(game.SealHello(Span(plain), Filled(8))), 10);
  CHECK(game.Open(Span(result.sends.value(0).datagram)) && game.Linked());
  const auto sealed = hub.Seal(
      Kaniel, Encode(Redirect{Endpoint{0x0A000004, 1}, Filled(3), "t", {}}));
  CHECK(sealed.has_value());
  const auto opened = game.Open(Span(*sealed));
  CHECK(opened && DecodeRedirect(ToQt(*opened))->to.ipv4 == 0x0A000004);
  std::cout << "region linked game redirected: ok\n";
  return 0;
}

QByteArray Sha(const QByteArray &secret) {
  return QCryptographicHash::hash(secret, QCryptographicHash::Sha256);
}

// A region ticket: made by one shard, taken by the shard it names, for the
// player and secret it names, before it expires, and not changed.
int TicketsHoldForTheirPlayerAndShard() {
  const LinkAuth::HmacKey key = Region::TicketKey("region secret", "token");
  Action join;
  join.set_action(Action::JOIN_CODE);
  join.set_code("ABCDEF");
  const QByteArray ticket = Region::SignTicket(
      key, Region::TicketFor("Kaniel", Sha("s3cr3t"), "eu-2", 1000, {join}));
  const auto taken =
      Region::OpenTicket(key, ticket, "Kaniel", "s3cr3t", "eu-2", 2000);
  CHECK(taken && taken->online_id() == "Kaniel" &&
        taken->requests_size() == 1 && taken->requests(0).code() == "ABCDEF");
  CHECK(taken->expires_ms() == 1000 + Region::TicketMs);
  CHECK(!Region::OpenTicket(key, ticket, "Ada", "s3cr3t", "eu-2", 2000));
  CHECK(!Region::OpenTicket(key, ticket, "Kaniel", "wrong", "eu-2", 2000));
  CHECK(!Region::OpenTicket(key, ticket, "Kaniel", "s3cr3t", "eu-3", 2000));
  CHECK(!Region::OpenTicket(key, ticket, "Kaniel", "s3cr3t", "eu-2",
                            1000 + Region::TicketMs + 1));
  QByteArray changed = ticket;
  changed[3] = char(changed[3] ^ 1);
  CHECK(!Region::OpenTicket(key, changed, "Kaniel", "s3cr3t", "eu-2", 2000));
  CHECK(!Region::OpenTicket(key, ticket.left(10), "Kaniel", "s3cr3t", "eu-2",
                            2000));
  CHECK(!Region::OpenTicket(key, {}, "Kaniel", "s3cr3t", "eu-2", 2000));
  // Another region's secret; the token's key when no secret is set.
  CHECK(!Region::OpenTicket(Region::TicketKey("other", "token"), ticket,
                            "Kaniel", "s3cr3t", "eu-2", 2000));
  const LinkAuth::HmacKey from_token = Region::TicketKey({}, "token");
  const QByteArray by_token = Region::SignTicket(
      from_token, Region::TicketFor("Kaniel", Sha("s3cr3t"), "eu-2", 1000, {}));
  CHECK(!!Region::OpenTicket(Region::TicketKey({}, "token"), by_token, "Kaniel",
                             "s3cr3t", "eu-2", 2000));
  CHECK(!Region::OpenTicket(Region::TicketKey({}, "other token"), by_token,
                            "Kaniel", "s3cr3t", "eu-2", 2000));
  std::cout << "region tickets: ok\n";
  return 0;
}

Region::Shard ShardOf(std::string name, int players, int capacity,
                      bool draining = false) {
  Region::Shard shard;
  shard.name = std::move(name);
  shard.address = Endpoint{0x0A000001, 31314};
  shard.players = players;
  shard.capacity = capacity;
  shard.draining = draining;
  return shard;
}

// Rule (c): a hello at a full shard goes to the least loaded shard with room;
// a draining shard sends every hello on. With nowhere to go it stays.
int AFullShardSendsHellosToTheLeastLoaded() {
  Region::View view;
  view.shards = {ShardOf("eu-1", 800, 800), ShardOf("eu-2", 500, 800),
                 ShardOf("eu-3", 300, 800), ShardOf("eu-4", 10, 800, true),
                 ShardOf("eu-5", 100, 0)};
  CHECK(!Region::PlaceHello(view, "eu-1", 799, 800, false));
  const Region::Shard *to = Region::PlaceHello(view, "eu-1", 800, 800, false);
  CHECK(to && to->name == "eu-3");
  // By their share of their own capacity.
  view.shards[1].capacity = 2000;
  CHECK(Region::PlaceHello(view, "eu-1", 800, 800, false)->name == "eu-2");
  // Past 90% of its own capacity a shard takes nobody else's.
  view.shards = {ShardOf("eu-1", 900, 800), ShardOf("eu-2", 721, 800)};
  CHECK(!Region::PlaceHello(view, "eu-1", 900, 800, false));
  // Draining: anywhere with room, even under capacity.
  view.shards = {ShardOf("eu-1", 10, 800, true), ShardOf("eu-2", 600, 800)};
  CHECK(Region::PlaceHello(view, "eu-1", 10, 800, true)->name == "eu-2");
  CHECK(!Region::PlaceHello(Region::View{}, "eu-1", 10, 800, true));
  std::cout << "region hello placement: ok\n";
  return 0;
}

TeamLobby NewLobby() {
  TeamLobby lobby{[n = 0]() mutable { return "t" + std::to_string(++n); }};
  lobby.SetCodeSource([n = 0]() mutable {
    return std::string("KTCDA") + static_cast<char>('A' + n++);
  });
  return lobby;
}

bbcoop::TeamRequest RequestOf(Action::Action action, std::string field = {}) {
  bbcoop::TeamRequest request;
  request.set_action(action);
  if (action == Action::JOIN_CODE) {
    request.set_code(field);
  } else if (action == Action::INVITE_NAME) {
    request.set_name(field);
  } else {
    request.set_team(field);
  }
  return request;
}

// Rule (a): a code or a name this shard does not have is looked up in the
// region; the joiner moves to the leader's shard and joins there.
int AFriendOnAnotherShardIsJoined() {
  TeamLobby lobby = NewLobby();
  TeamLobby::Region region;
  region.shard_of_code = [](const std::string &code) {
    return code == "FARAWY" ? std::string("eu-2") : std::string();
  };
  region.named = [](const std::string &folded) {
    std::vector<TeamLobby::Elsewhere> found;
    if (folded == "eileen") {
      found.push_back({"eu-3", "45696c65656e"});
    }
    if (folded == "twin") {
      found.push_back({"eu-2", "aa"});
      found.push_back({"eu-3", "bb"});
    }
    return found;
  };
  lobby.SetRegion(region);
  lobby.SetShardName("eu-1");
  lobby.Arrive("a", "Kaniel", false);
  lobby.Arrive("b", "Ada", false);

  // Join by a code of another shard: a move there, asking JOIN_CODE.
  auto outcome = lobby.Request("b", RequestOf(Action::JOIN_CODE, "farawy"));
  CHECK(outcome.notice.empty() && outcome.moves.size() == 1);
  CHECK(outcome.moves[0].member == "b" && outcome.moves[0].shard == "eu-2");
  CHECK(outcome.moves[0].requests.size() == 1 &&
        outcome.moves[0].requests[0].action() == Action::JOIN_CODE &&
        outcome.moves[0].requests[0].code() == "FARAWY");
  CHECK(!lobby.TeamOf("b"));
  // A code nowhere is a wrong lookup, as before.
  outcome = lobby.Request("b", RequestOf(Action::JOIN_CODE, "NNWHRE"));
  CHECK(!outcome.notice.empty() && outcome.moves.empty());
  // This shard's own code first.
  CHECK(lobby.Request("a", RequestOf(Action::CREATE)).changed);
  const std::string code = lobby.TeamOf("a")->code();
  outcome = lobby.Request("b", RequestOf(Action::JOIN_CODE, code));
  CHECK(outcome.moves.empty() && lobby.TeamOf("b") == lobby.TeamOf("a"));

  // Invite by a name signed in on another shard: an invite into that shard's
  // inbox, as the team's code.
  outcome = lobby.Request("a", RequestOf(Action::INVITE_NAME, "Eileen"));
  CHECK(outcome.notice.empty() && outcome.remote_invites.size() == 1);
  const auto &sent = outcome.remote_invites[0];
  CHECK(sent.shard == "eu-3" && sent.member == "45696c65656e");
  CHECK(sent.invite.team() == "@eu-1/" + code);
  CHECK(sent.invite.from() == "a" && sent.invite.from_name() == "Kaniel");
  // Several of that name in the region: refused, as on one shard.
  outcome = lobby.Request("a", RequestOf(Action::INVITE_NAME, "twin"));
  CHECK(!outcome.notice.empty() && outcome.remote_invites.empty());
  std::cout << "region lobby lookups: ok\n";
  return 0;
}

// The invitee's side: the invite from another shard is listed as any other;
// its Accept moves the player to the leader's shard with the team's code, its
// Decline only drops it.
int AnInviteFromAnotherShardIsAccepted() {
  TeamLobby lobby = NewLobby();
  lobby.SetShardName("eu-3");
  lobby.Arrive("e", "Eileen", false);
  bbcoop::TeamInvite invite;
  invite.set_team("@eu-1/HJKLMN");
  invite.set_from("a");
  invite.set_from_name("Kaniel");
  CHECK(lobby.InviteFromElsewhere("e", invite).changed);
  CHECK(!lobby.InviteFromElsewhere("nobody", invite).changed);
  bbcoop::LobbyState state = lobby.StateFor("e");
  CHECK(state.invites_size() == 1 && state.invites(0).from_name() == "Kaniel" &&
        state.invites(0).team() == "@eu-1/HJKLMN");
  // Said twice (a heartbeat again): listed once.
  lobby.InviteFromElsewhere("e", invite);
  CHECK(lobby.StateFor("e").invites_size() == 1);
  auto outcome = lobby.Request("e", RequestOf(Action::ACCEPT, "@eu-1/HJKLMN"));
  CHECK(outcome.moves.size() == 1 && outcome.moves[0].shard == "eu-1" &&
        outcome.moves[0].requests.size() == 1 &&
        outcome.moves[0].requests[0].code() == "HJKLMN");
  CHECK(lobby.StateFor("e").invites_size() == 0);
  lobby.InviteFromElsewhere("e", invite);
  outcome = lobby.Request("e", RequestOf(Action::DECLINE, "@eu-1/HJKLMN"));
  CHECK(outcome.moves.empty() && lobby.StateFor("e").invites_size() == 0);
  // In a team already: refused, as a local invite.
  lobby.InviteFromElsewhere("e", invite);
  lobby.Request("e", RequestOf(Action::CREATE));
  outcome = lobby.Request("e", RequestOf(Action::ACCEPT, "@eu-1/HJKLMN"));
  CHECK(!outcome.notice.empty() && outcome.moves.empty());
  // An invite a game makes up with this form is no remote one.
  outcome = lobby.Request("e", RequestOf(Action::ACCEPT, "@eu-9/XXXXXX"));
  CHECK(!outcome.notice.empty() && outcome.moves.empty());
  std::cout << "region remote invites: ok\n";
  return 0;
}

// Rule (b): a Start of a world another shard holds moves the team there: the
// leader makes the team again with its code and pick, the others join it.
int AWorldHeldElsewhereMovesItsTeam() {
  TeamLobby lobby = NewLobby();
  bbcoop::TeamWorldInfo world;
  world.set_id("w1");
  lobby.SetWorlds({world});
  TeamLobby::Region region;
  region.world_holder = [](const std::string &id) {
    return id == "w1" ? std::string("eu-2") : std::string();
  };
  lobby.SetRegion(region);
  lobby.Arrive("a", "Kaniel", false);
  lobby.Arrive("b", "Ada", false);
  lobby.Request("a", RequestOf(Action::CREATE));
  const std::string code = lobby.TeamOf("a")->code();
  lobby.Request("b", RequestOf(Action::JOIN_CODE, code));
  bbcoop::TeamRequest pick;
  pick.set_action(Action::WORLD);
  pick.set_world("w1");
  CHECK(lobby.Request("a", pick).notice.empty());
  bbcoop::TeamRequest ready;
  ready.set_action(Action::READY);
  ready.set_character("c");
  lobby.Request("a", ready);
  lobby.Request("b", ready);
  const auto outcome = lobby.Request("a", RequestOf(Action::START));
  CHECK(outcome.starts.empty() && outcome.moves.size() == 2);
  for (const auto &move : outcome.moves) {
    CHECK(move.shard == "eu-2");
    if (move.member == "a") {
      CHECK(move.requests.size() == 2 &&
            move.requests[0].action() == Action::WORLD &&
            move.requests[0].world() == "w1" &&
            move.requests[1].action() == Action::CREATE &&
            move.requests[1].code() == code);
    } else {
      CHECK(move.member == "b" && move.requests.size() == 1 &&
            move.requests[0].action() == Action::JOIN_CODE &&
            move.requests[0].code() == code);
    }
  }
  CHECK(!lobby.TeamOf("a")->started());
  std::cout << "region world held elsewhere: ok\n";
  return 0;
}

// A CREATE carrying a code (only a ticket's: the service drops a game's) keeps
// it when no team here has it.
int ATeamMadeAgainKeepsItsCode() {
  TeamLobby lobby = NewLobby();
  lobby.Arrive("a", "Kaniel", false);
  lobby.Arrive("b", "Ada", false);
  bbcoop::TeamRequest create = RequestOf(Action::CREATE);
  create.set_code("KEPTCD");
  CHECK(lobby.Request("a", create).changed);
  CHECK(lobby.TeamOf("a")->code() == "KEPTCD");
  CHECK(lobby.Codes() == std::vector<std::string>{"KEPTCD"});
  CHECK(lobby.Request("b", create).changed);
  CHECK(lobby.TeamOf("b")->code() != "KEPTCD");
  CHECK(lobby.Players().size() == 2);
  std::cout << "region team code kept: ok\n";
  return 0;
}

// The directory's answer to a heartbeat, read into the shard's view.
int TheDirectorysAnswerIsRead() {
  const QByteArray key_hex =
      QByteArray::fromStdString(LinkAuth::HexOf(Filled(0x21)));
  const QJsonObject answer{
      {"shards",
       QJsonArray{QJsonObject{{"name", "eu-1"},
                              {"address", "127.0.0.1:31314"},
                              {"key", QString::fromLatin1(key_hex)},
                              {"players", 12},
                              {"capacity", 800},
                              {"draining", false}},
                  QJsonObject{{"name", "eu-2"},
                              {"address", "10.0.0.9:40000"},
                              {"key", QString::fromLatin1(key_hex)},
                              {"players", 3},
                              {"capacity", 800},
                              {"draining", true}},
                  QJsonObject{{"name", "bad"}, {"address", "nowhere"}}}},
      {"players", QJsonArray{QJsonObject{
                      {"shard", "eu-2"}, {"member", "4164"}, {"name", "Ada"}}}},
      {"codes", QJsonObject{{"ABCDEF", "eu-2"}}},
      {"held", QJsonObject{{"w1", "eu-2"}}},
      {"inbox", QJsonArray{QJsonObject{{"member", "45"},
                                       {"team", "@eu-2/ABCDEF"},
                                       {"from", "4164"},
                                       {"from_name", "Ada"}}}}};
  std::vector<Region::Invite> inbox;
  const Region::View view = Region::ViewOf(answer, &inbox);
  CHECK(view.shards.size() == 2);
  CHECK(view.shards[1].name == "eu-2" &&
        view.shards[1].address == (Endpoint{0x0A000009, 40000}) &&
        view.shards[1].draining && view.shards[1].key == Filled(0x21) &&
        view.shards[0].players == 12 && view.shards[0].capacity == 800);
  CHECK(view.Find("eu-2") == &view.shards[1] && !view.Find("bad"));
  CHECK(view.players.size() == 1 && view.players[0].shard == "eu-2" &&
        view.players[0].member == "4164" && view.players[0].name == "Ada");
  CHECK(view.codes.at("ABCDEF") == "eu-2" && view.held.at("w1") == "eu-2");
  CHECK(inbox.size() == 1 && inbox[0].member == "45" &&
        inbox[0].invite.team() == "@eu-2/ABCDEF" &&
        inbox[0].invite.from_name() == "Ada");
  // An address as the directory and the launcher write it.
  CHECK(Region::EndpointOf("1.2.3.4:5") == (Endpoint{0x01020304, 5}));
  CHECK(!Region::EndpointOf("1.2.3.4") && !Region::EndpointOf("host:80") &&
        !Region::EndpointOf("1.2.3.4:0") &&
        !Region::EndpointOf("1.2.3.4:70000"));
  std::cout << "region directory answer: ok\n";
  return 0;
}

} // namespace

int main() {
  int failed = 0;
  failed |= RedirectContracts();
  failed |= AHelloIsAnsweredWithARedirect();
  failed |= ALinkedGameIsRedirectedOnItsSession();
  failed |= TicketsHoldForTheirPlayerAndShard();
  failed |= AFullShardSendsHellosToTheLeastLoaded();
  failed |= AFriendOnAnotherShardIsJoined();
  failed |= AnInviteFromAnotherShardIsAccepted();
  failed |= AWorldHeldElsewhereMovesItsTeam();
  failed |= ATeamMadeAgainKeepsItsCode();
  failed |= TheDirectorysAnswerIsRead();
  if (failed == 0) {
    std::cout << "region: all checks passed\n";
  }
  return failed;
}
