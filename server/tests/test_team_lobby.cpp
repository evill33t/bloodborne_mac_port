// SPDX-FileCopyrightText: Copyright 2026 shadNet Project
// SPDX-License-Identifier: GPL-2.0-or-later
// The team lobby's rules (coop/team_lobby.h): teams, invites, Ready and Start,
// and the server's worlds.
#include <algorithm>
#include <cctype>
#include <iostream>
#include <string>
#include <vector>

#include "coop/team_code.h"
#include "coop/team_lobby.h"

using namespace BloodborneCoop;
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

TeamLobby NewLobby() {
  return TeamLobby{[n = 0]() mutable { return "t" + std::to_string(++n); }};
}

bbcoop::TeamRequest Request(bbcoop::TeamRequest::Action action,
                            std::string member = {}, std::string team = {},
                            uint32_t slot = 0) {
  bbcoop::TeamRequest request;
  request.set_action(action);
  request.set_member(member);
  request.set_team(team);
  request.set_slot(slot);
  request.set_character("char of slot " + std::to_string(slot));
  return request;
}

int TeamFormsByInvite() {
  TeamLobby lobby = NewLobby();
  lobby.Arrive("a", "Kaniel", false);
  lobby.Arrive("b", "Ada", false);
  // Others see each other signed in.
  bbcoop::LobbyState a = lobby.StateFor("a");
  CHECK(a.players_size() == 1 && a.players(0).name() == "Ada");
  CHECK(a.players(0).state() == bbcoop::LobbyPlayer::TITLE);
  CHECK(!a.has_team());

  CHECK(lobby.Request("a", Request(Action::CREATE)).changed);
  CHECK(lobby.TeamOf("a") && lobby.TeamOf("a")->leader() == "a");
  // Inviting someone not signed in is refused with a notice.
  CHECK(!lobby.Request("a", Request(Action::INVITE, "nobody")).notice.empty());
  CHECK(lobby.Request("a", Request(Action::INVITE, "b")).changed);
  bbcoop::LobbyState b = lobby.StateFor("b");
  CHECK(b.invites_size() == 1 && b.invites(0).from_name() == "Kaniel");
  const std::string team = b.invites(0).team();
  // Only the leader of the team names a team: b cannot invite before joining.
  CHECK(!lobby.Request("b", Request(Action::INVITE, "a")).notice.empty());
  CHECK(lobby.Request("b", Request(Action::ACCEPT, {}, team)).changed);
  CHECK(lobby.TeamOf("b") == lobby.TeamOf("a"));
  CHECK(lobby.StateFor("b").invites_size() == 0);
  CHECK(lobby.StateFor("a").team().members_size() == 2);
  // An accepted invite cannot be used twice; accepting a made-up one is
  // refused.
  CHECK(
      !lobby.Request("b", Request(Action::ACCEPT, {}, "nope")).notice.empty());
  return 0;
}

int TeamHoldsFour() {
  TeamLobby lobby = NewLobby();
  for (const char *m : {"a", "b", "c", "d", "e"}) {
    lobby.Arrive(m, m, false);
  }
  lobby.Request("a", Request(Action::CREATE));
  const std::string team = lobby.TeamOf("a")->id();
  for (const char *m : {"b", "c", "d"}) {
    lobby.Request("a", Request(Action::INVITE, m));
    CHECK(lobby.Request(m, Request(Action::ACCEPT, {}, team)).changed);
  }
  CHECK(lobby.TeamOf("a")->members_size() == 4);
  // A full team takes no more: the invite itself is refused.
  CHECK(!lobby.Request("a", Request(Action::INVITE, "e")).notice.empty());
  return 0;
}

int ReadyAndStart() {
  TeamLobby lobby = NewLobby();
  lobby.Arrive("a", "Kaniel", false);
  lobby.Arrive("b", "Ada", false);
  lobby.Arrive("c", "Lisa", false);
  lobby.Request("a", Request(Action::CREATE));
  const std::string team = lobby.TeamOf("a")->id();
  for (const char *m : {"b", "c"}) {
    lobby.Request("a", Request(Action::INVITE, m));
    lobby.Request(m, Request(Action::ACCEPT, {}, team));
  }
  // Only the leader starts, and only once it is ready itself.
  CHECK(!lobby.Request("b", Request(Action::START)).notice.empty());
  CHECK(!lobby.Request("a", Request(Action::START)).notice.empty());
  lobby.Request("a", Request(Action::READY, {}, {}, 2));
  lobby.Request("b", Request(Action::READY, {}, {}, 0));
  // Kaniel 2026-10-20: the leader begins as soon as the members are in (their
  // games say ready by themselves); one not ready yet (c) goes once it is.
  const TeamLobby::Outcome start = lobby.Request("a", Request(Action::START));
  CHECK(start.notice.empty() && start.changed);
  CHECK(start.starts.size() == 2);
  CHECK(lobby.TeamOf("a")->started());
  const TeamLobby::Outcome joins =
      lobby.Request("c", Request(Action::READY, {}, {}, 1));
  CHECK(joins.starts.size() == 1 && joins.starts[0] == "c");
  const bbcoop::TeamStart go = lobby.StartFor("a");
  CHECK(go.party() == TeamLobby::PartyOf(go.world()) && go.host() == "a" &&
        go.slot() == 2);
  CHECK(lobby.StartFor("b").slot() == 0 && lobby.StartFor("b").host() == "a");
  // c back at the title readies again and goes at once (joins the team as it
  // plays).
  lobby.Request("c", Request(Action::UNREADY));
  const TeamLobby::Outcome late =
      lobby.Request("c", Request(Action::READY, {}, {}, 1));
  CHECK(late.starts.size() == 1 && late.starts[0] == "c");
  // Playing shows; when nobody plays any more, the team is not started.
  lobby.Arrive("a", "Kaniel", true);
  CHECK(lobby.StateFor("b").team().members(0).state() ==
        bbcoop::LobbyPlayer::PLAYING);
  lobby.Arrive("a", "Kaniel", false);
  CHECK(!lobby.TeamOf("a")->started());
  // A member gone from the server: offline in its team, still a member.
  lobby.Leave("c");
  CHECK(lobby.TeamOf("a")->members_size() == 3);
  CHECK(lobby.StateFor("a").team().members(2).state() ==
        bbcoop::LobbyPlayer::OFFLINE);
  return 0;
}

// Kaniel 2026-10-16 (the team menu in game): a team just started stays started
// while its games load (none plays yet; another player's hello comes then), so
// a player invited later joins the hunt from its own game with its Ready.
int AStartedTeamStaysStartedWhileItsGamesLoad() {
  TeamLobby lobby = NewLobby();
  lobby.Arrive("a", "Kaniel", false);
  lobby.Arrive("b", "Ada", false);
  lobby.Request("a", Request(Action::CREATE));
  lobby.Request("a", Request(Action::READY, {}, {}, 0));
  lobby.Request("a", Request(Action::START));
  lobby.Arrive("b", "Ada", true); // b plays alone; a's game still loads
  CHECK(lobby.TeamOf("a")->started());
  lobby.Arrive("a", "Kaniel", true);
  lobby.Request("a", Request(Action::INVITE, "b"));
  lobby.Request("b", Request(Action::ACCEPT, {}, lobby.TeamOf("a")->id()));
  const TeamLobby::Outcome in =
      lobby.Request("b", Request(Action::READY, {}, {}, 1));
  CHECK(in.starts.size() == 1 && in.starts[0] == "b");
  // Once it played, nobody playing any more ends it.
  lobby.Arrive("a", "Kaniel", false);
  lobby.Arrive("b", "Ada", false);
  CHECK(!lobby.TeamOf("a")->started());
  return 0;
}

int StartedTeamKeepsGoingWhileOnePlays() {
  TeamLobby lobby = NewLobby();
  lobby.Arrive("a", "Kaniel", false);
  lobby.Arrive("b", "Ada", false);
  lobby.Request("a", Request(Action::CREATE));
  lobby.Request("a", Request(Action::INVITE, "b"));
  lobby.Request("b", Request(Action::ACCEPT, {}, lobby.TeamOf("a")->id()));
  lobby.Request("a", Request(Action::READY, {}, {}, 0));
  lobby.Request("b", Request(Action::READY, {}, {}, 0));
  lobby.Request("a", Request(Action::START));
  lobby.Arrive("a", "Kaniel", true);
  lobby.Arrive("b", "Ada", true);
  // The host's game crashes: b still plays, the team stays started, and the
  // host's Continue (ready again) goes at once.
  lobby.Leave("a");
  CHECK(lobby.TeamOf("b")->started());
  lobby.Arrive("a", "Kaniel", false);
  const TeamLobby::Outcome again =
      lobby.Request("a", Request(Action::READY, {}, {}, 0));
  CHECK(again.starts.size() == 1 && again.starts[0] == "a");
  return 0;
}

int LeaveAndRemove() {
  TeamLobby lobby = NewLobby();
  for (const char *m : {"a", "b", "c"}) {
    lobby.Arrive(m, m, false);
  }
  lobby.Request("a", Request(Action::CREATE));
  const std::string team = lobby.TeamOf("a")->id();
  for (const char *m : {"b", "c"}) {
    lobby.Request("a", Request(Action::INVITE, m));
    lobby.Request(m, Request(Action::ACCEPT, {}, team));
  }
  // Only the leader removes.
  CHECK(!lobby.Request("b", Request(Action::REMOVE, "c")).notice.empty());
  CHECK(lobby.Request("a", Request(Action::REMOVE, "c")).changed);
  CHECK(lobby.TeamOf("c") == nullptr);
  // A player in a team creates no other one.
  CHECK(!lobby.Request("b", Request(Action::CREATE)).notice.empty());
  // The leader leaves: the next member leads.
  CHECK(lobby.Request("a", Request(Action::LEAVE)).changed);
  CHECK(lobby.TeamOf("b")->leader() == "b");
  // The last one leaves: the team is gone (its world stays on the server).
  lobby.Request("b", Request(Action::LEAVE));
  CHECK(lobby.TeamOf("b") == nullptr);
  // A player in a team accepts no other team's invite before leaving its own.
  lobby.Request("a", Request(Action::CREATE));
  lobby.Request("b", Request(Action::CREATE));
  lobby.Request("a", Request(Action::INVITE, "b"));
  CHECK(
      !lobby.Request("b", Request(Action::ACCEPT, {}, lobby.TeamOf("a")->id()))
           .notice.empty());
  // A declined invite is gone.
  lobby.Request("b", Request(Action::DECLINE, {}, lobby.TeamOf("a")->id()));
  CHECK(lobby.StateFor("b").invites_size() == 0);
  return 0;
}

// Kaniel 2026-10-17: a team is only a session. It lives while one of its
// members is signed in: a member whose game crashed keeps its place (the grace)
// while someone is still there, and once every member's game is gone the team
// and its invites are gone.
int ATeamIsGoneOnceEveryMemberIsGone() {
  TeamLobby lobby = NewLobby();
  lobby.Arrive("a", "Kaniel", false);
  lobby.Arrive("b", "Ada", false);
  lobby.Arrive("c", "Lisa", false);
  lobby.Request("a", Request(Action::CREATE));
  const std::string team = lobby.TeamOf("a")->id();
  lobby.Request("a", Request(Action::INVITE, "b"));
  lobby.Request("b", Request(Action::ACCEPT, {}, team));
  lobby.Request("a", Request(Action::INVITE, "c"));
  // a's game crashes: b is still there, a keeps its place.
  CHECK(lobby.Leave("a").changed);
  CHECK(lobby.TeamOf("a") && lobby.TeamOf("a")->members_size() == 2);
  lobby.Arrive("a", "Kaniel", false);
  CHECK(lobby.TeamOf("a")->leader() == "a");
  // Both games gone: no team, no invite to it.
  lobby.Leave("a");
  CHECK(lobby.TeamOf("b") != nullptr);
  CHECK(lobby.Leave("b").changed);
  CHECK(lobby.TeamOf("a") == nullptr && lobby.TeamOf("b") == nullptr);
  CHECK(lobby.StateFor("c").invites_size() == 0);
  // Back, they are in no team (a new one is made as any other).
  lobby.Arrive("a", "Kaniel", false);
  CHECK(!lobby.StateFor("a").has_team());
  CHECK(lobby.Request("a", Request(Action::CREATE)).changed);
  CHECK(lobby.TeamOf("a")->id() != team);
  return 0;
}

// Phase 7: the server gives each guest its player slot (the sign it joins
// with: 7, 6, 5), and a guest keeps it while others come and go.
uint32_t SignOf(const TeamLobby &lobby, const std::string &member) {
  for (const bbcoop::TeamMember &m : lobby.TeamOf(member)->members()) {
    if (m.member() == member) {
      return m.sign();
    }
  }
  return 99;
}

int GuestsKeepTheirPlayerSlots() {
  TeamLobby lobby = NewLobby();
  for (const char *m : {"a", "b", "c", "d", "e"}) {
    lobby.Arrive(m, m, false);
  }
  lobby.Request("a", Request(Action::CREATE));
  const std::string team = lobby.TeamOf("a")->id();
  for (const char *m : {"b", "c", "d"}) {
    lobby.Request("a", Request(Action::INVITE, m));
    lobby.Request(m, Request(Action::ACCEPT, {}, team));
  }
  CHECK(SignOf(lobby, "a") == 0); // the leader hosts: no sign
  CHECK(SignOf(lobby, "b") == 7 && SignOf(lobby, "c") == 6 &&
        SignOf(lobby, "d") == 5);
  // c leaves: b and d keep theirs, and e takes the free one.
  lobby.Request("c", Request(Action::LEAVE));
  CHECK(SignOf(lobby, "b") == 7 && SignOf(lobby, "d") == 5);
  lobby.Request("a", Request(Action::INVITE, "e"));
  lobby.Request("e", Request(Action::ACCEPT, {}, team));
  CHECK(SignOf(lobby, "e") == 6);
  // The leader leaves: the new one hosts, the others keep theirs.
  lobby.Request("a", Request(Action::LEAVE));
  CHECK(lobby.TeamOf("b")->leader() == "b" && SignOf(lobby, "b") == 0);
  CHECK(SignOf(lobby, "d") == 5 && SignOf(lobby, "e") == 6);
  return 0;
}

// Kaniel 2026-10-16 (the team menu in game): a member out of a team, removed
// by the leader or gone itself, is named with the team it left, so the server
// takes it out of that team's world too.
int AMemberOutOfATeamIsNamed() {
  TeamLobby lobby = NewLobby();
  for (const char *m : {"a", "b", "c"}) {
    lobby.Arrive(m, m, true);
  }
  lobby.Request("a", Request(Action::CREATE));
  const std::string team = lobby.TeamOf("a")->id();
  for (const char *m : {"b", "c"}) {
    lobby.Request("a", Request(Action::INVITE, m));
    lobby.Request(m, Request(Action::ACCEPT, {}, team));
  }
  using Departed = std::vector<TeamLobby::Departure>;
  // A refused removal names nobody.
  CHECK(lobby.Request("b", Request(Action::REMOVE, "c")).departed.empty());
  // Named with the party of the world the team plays (none before its Start).
  CHECK((lobby.Request("a", Request(Action::REMOVE, "c")).departed ==
         Departed{{"c", team, ""}}));
  lobby.Request("a", Request(Action::INVITE, "c"));
  lobby.Request("c", Request(Action::ACCEPT, {}, team));
  for (const char *m : {"a", "b", "c"}) {
    lobby.Request(m, Request(Action::READY, {}, {}, 1));
  }
  lobby.Request("a", Request(Action::START));
  const std::string party = lobby.TeamOf("a")->party();
  CHECK(!party.empty());
  const TeamLobby::Outcome removed =
      lobby.Request("a", Request(Action::REMOVE, "c"));
  CHECK((removed.departed == Departed{{"c", team, party}}));
  const TeamLobby::Outcome left = lobby.Request("b", Request(Action::LEAVE));
  CHECK((left.departed == Departed{{"b", team, party}}));
  // The last one out: its team is gone, and it is named all the same.
  CHECK((lobby.Request("a", Request(Action::LEAVE)).departed ==
         Departed{{"a", team, party}}));
  CHECK(lobby.Request("a", Request(Action::CREATE)).departed.empty());
  return 0;
}

} // namespace

// Phase 11 (Kaniel 2026-10-15): players are seen by their character's name
// only. A game at the title has none to say (""): the last one it said stays,
// and before any it is "Hunter". A new name reaches its team and its invites.
int PlayersAreSeenByTheirCharactersName() {
  TeamLobby lobby = NewLobby();
  lobby.Arrive("a", "", false);
  lobby.Arrive("b", "", false);
  CHECK(lobby.StateFor("b").players(0).name() == "Hunter");
  CHECK(lobby.Request("a", Request(Action::CREATE)).changed);
  CHECK(lobby.Request("a", Request(Action::INVITE, "b")).changed);
  CHECK(lobby.StateFor("b").invites(0).from_name() == "Hunter");

  CHECK(lobby.Arrive("a", "Lady Maria", true).changed);
  bbcoop::LobbyState b = lobby.StateFor("b");
  CHECK(b.players(0).name() == "Lady Maria");
  CHECK(b.invites(0).from_name() == "Lady Maria");
  CHECK(lobby.StateFor("a").team().members(0).name() == "Lady Maria");
  // Back at the title, it says no name: the character's stays.
  lobby.Arrive("a", "", false);
  CHECK(lobby.StateFor("b").players(0).name() == "Lady Maria");
  return 0;
}

int OfflineMemberLeavesAfterGrace() {
  // Kaniel 2026-10-16: a game closed for good drops its player from the team
  // after a grace period; a crash and Continue inside it still rejoin.
  TeamLobby lobby = NewLobby();
  lobby.Arrive("a", "Kaniel", false);
  lobby.Arrive("b", "Ada", false);
  lobby.Request("a", Request(Action::CREATE));
  lobby.Request("a", Request(Action::INVITE, "b"));
  lobby.Request("b", Request(Action::ACCEPT, {}, lobby.TeamOf("a")->id()));
  lobby.Leave("a");
  // The clock starts at the first look after the game is gone.
  CHECK(!lobby.Expire(1000).changed);
  CHECK(!lobby.Expire(1000 + OfflineGraceMs - 1).changed);
  CHECK(lobby.TeamOf("a") != nullptr);
  // Back in time: still a member, and its clock is gone.
  lobby.Arrive("a", "Kaniel", false);
  CHECK(!lobby.Expire(1000 + OfflineGraceMs).changed);
  lobby.Leave("a");
  CHECK(!lobby.Expire(5000).changed);
  CHECK(!lobby.Expire(5000 + OfflineGraceMs - 1).changed);
  // Gone for the whole grace: out of the team, and b leads.
  CHECK(lobby.Expire(5000 + OfflineGraceMs).changed);
  CHECK(lobby.TeamOf("a") == nullptr);
  CHECK(lobby.TeamOf("b")->leader() == "b" &&
        lobby.TeamOf("b")->members_size() == 1);
  return 0;
}

// Kaniel 2026-10-17: worlds are the server's, apart from its teams. Every
// world on the server is listed for every player (the last played first); a
// team's leader picks any of them, or a new one, for the next Start.
struct TwoHunters {
  TeamLobby lobby = NewLobby();
  std::string team;
  TwoHunters() {
    lobby.Arrive("a", "Kaniel", false);
    lobby.Arrive("b", "Ada", false);
    lobby.Request("a", Request(Action::CREATE));
    team = lobby.TeamOf("a")->id();
    lobby.Request("a", Request(Action::INVITE, "b"));
    lobby.Request("b", Request(Action::ACCEPT, {}, team));
  }
  TeamLobby::Outcome Pick(const std::string &member, const std::string &world) {
    bbcoop::TeamRequest request = Request(Action::WORLD);
    request.set_world(world);
    return lobby.Request(member, request);
  }
  TeamLobby::Outcome Forget(const std::string &member,
                            const std::string &world) {
    bbcoop::TeamRequest request = Request(Action::FORGET_WORLD);
    request.set_world(world);
    return lobby.Request(member, request);
  }
  TeamLobby::Outcome Start() {
    lobby.Request("a", Request(Action::READY, {}, {}, 3));
    lobby.Request("b", Request(Action::READY, {}, {}, 1));
    return lobby.Request("a", Request(Action::START));
  }
  void Stop() {
    lobby.Arrive("a", "Kaniel", true);
    lobby.Arrive("a", "Kaniel", false);
    lobby.Arrive("b", "Ada", false);
  }
};

int TheLeaderPicksAnyOfTheServersWorlds() {
  TwoHunters t;
  TeamLobby &lobby = t.lobby;
  // No world yet: the first Start makes one, listed for everyone.
  CHECK(lobby.Worlds().empty() && lobby.TeamOf("a")->world().empty() &&
        lobby.TeamOf("a")->party().empty());
  CHECK(t.Start().starts.size() == 2);
  CHECK(lobby.Worlds().size() == 1);
  const std::string first = lobby.Worlds()[0].id();
  CHECK(!first.empty());
  bbcoop::TeamStart go = lobby.StartFor("b");
  CHECK(go.world() == first && go.party() == "world:" + first);
  CHECK(lobby.TeamOf("a")->party() == go.party());
  CHECK(lobby.StateFor("b").worlds_size() == 1);
  t.Stop();
  // Only the leader picks, only a world of the server's or a new one ("").
  CHECK(!t.Pick("b", first).notice.empty());
  CHECK(!t.Pick("a", "nope").notice.empty());
  CHECK(t.Pick("a", "").changed && lobby.TeamOf("a")->world().empty() &&
        lobby.TeamOf("a")->party().empty());
  // A new world: another one, kept apart, the last played first.
  t.Start();
  t.Stop();
  CHECK(lobby.Worlds().size() == 2);
  const std::string second = lobby.Worlds()[0].id();
  CHECK(second != first && lobby.StartFor("a").party() != go.party());
  // The first one again: the same party as before.
  CHECK(t.Pick("a", first).changed);
  CHECK(lobby.StateFor("b").team().world() == first);
  t.Start();
  CHECK(lobby.Worlds()[0].id() == first);
  CHECK(lobby.StartFor("a").party() == go.party());
  // No pick while the team plays.
  CHECK(!t.Pick("a", second).notice.empty());
  // The server says what each world is (its host's reports) and when it was
  // played.
  bbcoop::TeamWorldInfo note;
  note.set_id(first);
  note.set_character("Kaniel");
  note.set_flags_on(5441);
  note.set_lamps(7);
  note.set_played_ms(1000);
  CHECK(lobby.NoteWorld(note));
  CHECK(!lobby.NoteWorld(note)); // nothing new
  CHECK(lobby.StateFor("b").worlds(0).character() == "Kaniel");
  CHECK(lobby.StateFor("b").worlds(0).lamps() == 7);
  // A world the server does not list is not added by a note.
  note.set_id("gone");
  CHECK(!lobby.NoteWorld(note));
  CHECK(lobby.Worlds().size() == 2);
  return 0;
}

// Another team plays the same worlds (the player ids are each install's own,
// so worlds are nobody's), but not one another team plays now.
int AnyTeamPlaysAnyWorld() {
  TwoHunters t;
  TeamLobby &lobby = t.lobby;
  t.Start();
  const std::string world = lobby.Worlds()[0].id();
  lobby.Arrive("a", "Kaniel", true);
  lobby.Arrive("c", "Lisa", false);
  lobby.Request("c", Request(Action::CREATE));
  bbcoop::TeamRequest pick = Request(Action::WORLD);
  pick.set_world(world);
  CHECK(!lobby.Request("c", pick).notice.empty()); // played now
  CHECK(lobby.StateFor("c").worlds_size() == 1);
  t.Stop();
  CHECK(lobby.Request("c", pick).changed);
  lobby.Request("c", Request(Action::READY, {}, {}, 0));
  const TeamLobby::Outcome go = lobby.Request("c", Request(Action::START));
  CHECK(go.starts.size() == 1);
  CHECK(lobby.StartFor("c").party() == "world:" + world);
  // Now a's team may not start it.
  lobby.Arrive("c", "Lisa", true);
  CHECK(!t.Pick("a", world).notice.empty());
  return 0;
}

// The leader forgets a world (off the list; the server puts its folder
// aside). Not one a team plays now; a team that had it picked has a new one.
int TheLeaderForgetsAWorld() {
  TwoHunters t;
  TeamLobby &lobby = t.lobby;
  t.Start();
  const std::string world = lobby.Worlds()[0].id();
  CHECK(!t.Forget("a", world).notice.empty()); // played now
  t.Stop();
  CHECK(!t.Forget("b", world).notice.empty()); // the leader only
  CHECK(!t.Forget("a", "nope").notice.empty());
  CHECK(lobby.TeamOf("a")->world() == world);
  // Kaniel 2026-10-17: a player in no team picks (and forgets) for the team it
  // makes, as a leader does; here it forgets the world a's team picked.
  lobby.Arrive("c", "Lisa", false);
  const TeamLobby::Outcome forgot = t.Forget("c", world);
  CHECK(forgot.changed && forgot.notice.empty());
  CHECK(forgot.forgotten == std::vector<std::string>{world});
  CHECK(lobby.Worlds().empty());
  CHECK(lobby.TeamOf("a")->world().empty() &&
        lobby.TeamOf("a")->party().empty());
  return 0;
}

// The server keeps its last MaxWorlds worlds: a new one past them puts the
// least lately played aside (never one a team plays or picked).
int TheServerKeepsItsLastWorlds() {
  TwoHunters t;
  TeamLobby &lobby = t.lobby;
  std::vector<bbcoop::TeamWorldInfo> kept;
  for (size_t n = 0; n < MaxWorlds; ++n) {
    bbcoop::TeamWorldInfo &world = kept.emplace_back();
    world.set_id("w" + std::to_string(n));
    world.set_played_ms(static_cast<int64_t>(MaxWorlds - n)); // w0 the last
  }
  lobby.SetWorlds(kept);
  CHECK(lobby.Worlds().size() == MaxWorlds);
  const std::string oldest = "w" + std::to_string(MaxWorlds - 1);
  // The team picked the oldest: it stays; the one before it goes.
  CHECK(t.Pick("a", oldest).changed);
  CHECK(t.Pick("a", "").changed);
  t.Pick("a", oldest);
  lobby.Arrive("c", "Lisa", false);
  lobby.Request("c", Request(Action::CREATE));
  lobby.Request("c", Request(Action::READY, {}, {}, 0));
  const TeamLobby::Outcome go = lobby.Request("c", Request(Action::START));
  CHECK(go.starts.size() == 1);
  CHECK(lobby.Worlds().size() == MaxWorlds);
  CHECK(lobby.Worlds()[0].id() == lobby.TeamOf("c")->world());
  CHECK(go.forgotten ==
        std::vector<std::string>{"w" + std::to_string(MaxWorlds - 2)});
  CHECK(lobby.TeamOf("a")->world() == oldest);
  // New ids are no listed world's.
  CHECK(lobby.NewWorldId() != lobby.NewWorldId());
  return 0;
}

// Kaniel 2026-10-17 (the world list, as the game's Load Game): a world's line
// says the characters that played in it (each member's at a Start, the host's
// reports) and when it was made (the first time the server notes, or the
// earliest known of a world from before).
int AWorldsLineSaysWhoPlayedItAndWhenItWasMade() {
  TwoHunters t;
  TeamLobby &lobby = t.lobby;
  t.Start();
  const std::string world = lobby.Worlds()[0].id();
  const auto has = [&](const std::string &name) {
    const auto &list = lobby.Worlds()[0].characters();
    return std::find(list.begin(), list.end(), name) != list.end();
  };
  CHECK(has("char of slot 3") && has("char of slot 1"));
  CHECK(lobby.Worlds()[0].characters_size() == 2);
  // The character each game plays in it (its hello says it, as the title's
  // Ready names none).
  CHECK(lobby.Arrive("b", "Ada", true).worlds_changed);
  CHECK(has("Ada") && lobby.Worlds()[0].characters_size() == 3);
  CHECK(!lobby.Arrive("b", "Ada", true).worlds_changed);
  bbcoop::TeamWorldInfo note;
  note.set_id(world);
  note.set_created_ms(500);
  note.add_characters("Kaniel");
  note.add_characters("char of slot 1"); // known already
  CHECK(lobby.NoteWorld(note));
  CHECK(lobby.Worlds()[0].created_ms() == 500);
  CHECK(lobby.Worlds()[0].characters_size() == 4 && has("Kaniel"));
  // Made once: a later note keeps the first time.
  note.set_created_ms(900);
  note.clear_characters();
  CHECK(!lobby.NoteWorld(note));
  CHECK(lobby.Worlds()[0].created_ms() == 500);
  // The host's character is one of them too.
  note.set_character("Lady Maria");
  CHECK(lobby.NoteWorld(note) && has("Lady Maria"));
  return 0;
}

// Every player sees which world another team plays now (greyed in its list);
// its own team's world is not "another's".
int AWorldPlayedNowSaysByWhichTeam() {
  TwoHunters t;
  TeamLobby &lobby = t.lobby;
  t.Start();
  lobby.Arrive("c", "Lisa", false);
  bbcoop::LobbyState c = lobby.StateFor("c");
  CHECK(c.worlds_size() == 1 && c.worlds(0).played_by() == t.team);
  CHECK(lobby.StateFor("a").worlds(0).played_by() == t.team);
  // Never stored with the world.
  CHECK(lobby.Worlds()[0].played_by().empty());
  t.Stop();
  CHECK(lobby.StateFor("c").worlds(0).played_by().empty());
  return 0;
}

// Kaniel 2026-10-17: the world is picked before the team is made: a player in
// no team picks one, and the team it makes takes it.
int APickBeforeATeamGoesIntoTheTeam() {
  TwoHunters t;
  TeamLobby &lobby = t.lobby;
  t.Start();
  t.Stop();
  const std::string world = lobby.Worlds()[0].id();
  lobby.Arrive("c", "Lisa", false);
  bbcoop::TeamRequest pick = Request(Action::WORLD);
  pick.set_world("nope");
  CHECK(!lobby.Request("c", pick).notice.empty());
  pick.set_world(world);
  CHECK(lobby.Request("c", pick).changed);
  CHECK(lobby.StateFor("c").world() == world);
  CHECK(!lobby.StateFor("a").has_team() || lobby.StateFor("a").world().empty());
  CHECK(lobby.Request("c", Request(Action::CREATE)).changed);
  CHECK(lobby.TeamOf("c")->world() == world &&
        lobby.TeamOf("c")->party() == "world:" + world);
  // A new world picked ("") makes a team with none.
  lobby.Request("c", Request(Action::LEAVE));
  pick.set_world("");
  lobby.Request("c", pick);
  lobby.Request("c", Request(Action::CREATE));
  CHECK(lobby.TeamOf("c")->world().empty());
  // A forgotten world is no player's pick any more.
  lobby.Request("c", Request(Action::LEAVE));
  pick.set_world(world);
  lobby.Request("c", pick);
  lobby.Request("d", Request(Action::CREATE)); // not signed in: refused
  lobby.Arrive("d", "Dee", false);
  lobby.Request("d", Request(Action::CREATE));
  bbcoop::TeamRequest forget = Request(Action::FORGET_WORLD);
  forget.set_world(world);
  t.Pick("a", "");
  CHECK(lobby.Request("d", forget).changed);
  CHECK(lobby.StateFor("c").world().empty());
  return 0;
}

// A world's line says how far it is: its lamps lit (each lamp's active flag;
// the flag after it, other map flags and other maps' are no lamps).
int AWorldsLampsAreCounted() {
  const std::vector<s32> on{1000,     12107810, 12107811, 12107830,
                            12401800, 12207800, 14007810, 13997810};
  CHECK(LampsLit(on) == 3);
  CHECK(LampsLit({}) == 0);
  return 0;
}

// Hosted (docs/HOSTED_SERVER.md): a world is its players'; a lobby lists few
// others.
static std::string PlayAWorld(TeamLobby &lobby, const std::string &leader,
                              const std::string &guest) {
  lobby.Request(leader, Request(Action::CREATE));
  lobby.Request(leader, Request(Action::INVITE, guest));
  lobby.Request(guest, Request(Action::ACCEPT, {}, lobby.TeamOf(leader)->id()));
  lobby.Request(leader, Request(Action::READY, {}, {}, 0));
  lobby.Request(guest, Request(Action::READY, {}, {}, 1));
  lobby.Request(leader, Request(Action::START));
  return lobby.TeamOf(leader)->world();
}

static int HostedWorldsAreTheirPlayersOnly() {
  TeamLobby lobby = NewLobby();
  lobby.SetHosted(2, 16);
  for (const char *m : {"a", "b", "c", "d", "e"}) {
    lobby.Arrive(m, m, false);
  }
  const std::string world = PlayAWorld(lobby, "a", "b");
  CHECK(!world.empty());
  CHECK(lobby.StateFor("a").worlds_size() == 1);
  CHECK(lobby.StateFor("b").worlds_size() == 1);
  CHECK(lobby.StateFor("c").worlds_size() == 0);            // not c's
  CHECK(lobby.StateFor("a").worlds(0).members_size() == 0); // never sent
  // c can neither pick it nor forget it.
  lobby.Request("c", Request(Action::CREATE));
  bbcoop::TeamRequest pick = Request(Action::WORLD);
  pick.set_world(world);
  CHECK(!lobby.Request("c", pick).notice.empty());
  lobby.Request("c", Request(Action::LEAVE));
  bbcoop::TeamRequest forget = Request(Action::FORGET_WORLD);
  forget.set_world(world);
  CHECK(!lobby.Request("c", forget).notice.empty());
  // Its players forget it each for themselves; the last one's goes.
  lobby.Request("a", Request(Action::LEAVE));
  lobby.Request("b", Request(Action::LEAVE));
  const TeamLobby::Outcome first = lobby.Request("a", forget);
  CHECK(first.notice.empty() && first.forgotten.empty());
  CHECK(lobby.StateFor("a").worlds_size() == 0);
  CHECK(lobby.StateFor("b").worlds_size() == 1);
  const TeamLobby::Outcome last = lobby.Request("b", forget);
  CHECK(last.forgotten == std::vector<std::string>{world});
  CHECK(lobby.Worlds().empty());
  // The line put aside with it names the last one who kept it: restored, it is
  // b's again (a forgot it before, for itself).
  CHECK(last.forgotten_lines.size() == 1);
  CHECK(last.forgotten_lines[0].members_size() == 1 &&
        last.forgotten_lines[0].members(0) == "b");
  return 0;
}

static int HostedServersKeepEveryPlayersWorlds() {
  TeamLobby lobby = NewLobby();
  lobby.SetHosted(32, 16);
  for (int pair = 0; pair < 20; ++pair) {
    const std::string a = "a" + std::to_string(pair);
    const std::string b = "b" + std::to_string(pair);
    lobby.Arrive(a, a, false);
    lobby.Arrive(b, b, false);
    PlayAWorld(lobby, a, b);
  }
  CHECK(lobby.Worlds().size() == 20); // no server-wide cap of MaxWorlds
  return 0;
}

static int AHostedLobbyListsFewOthers() {
  TeamLobby lobby = NewLobby();
  lobby.SetHosted(3, 16);
  for (int n = 0; n < 50; ++n) {
    lobby.Arrive("p" + std::to_string(n), "P" + std::to_string(n), false);
  }
  CHECK(lobby.StateFor("p0").players_size() == 3);
  // Its teammates and the players its team invited are always listed.
  lobby.Request("p0", Request(Action::CREATE));
  lobby.Request("p0", Request(Action::INVITE, "p40"));
  const bbcoop::LobbyState state = lobby.StateFor("p0");
  CHECK(std::ranges::any_of(state.players(),
                            [](const auto &p) { return p.member() == "p40"; }));
  // And the leader who invited a player is listed to it.
  const bbcoop::LobbyState invited = lobby.StateFor("p40");
  CHECK(std::ranges::any_of(invited.players(),
                            [](const auto &p) { return p.member() == "p0"; }));
  return 0;
}

// A friend the lobby does not list joins by the team's code: the leader handed
// it out (its invite), the friend typed it (its accept).
static int AFriendJoinsByTheTeamsCode() {
  TeamLobby lobby = NewLobby();
  lobby.SetHosted(1, 16);
  for (const char *m : {"a", "b", "c", "d", "e", "f"}) {
    lobby.Arrive(m, m, false);
  }
  lobby.Request("a", Request(Action::CREATE));
  const std::string code = lobby.StateFor("a").team().code();
  CHECK(NormalizeTeamCode(code) == code);
  bbcoop::TeamRequest join = Request(Action::JOIN_CODE);
  // Typed in lower case with a dash in the middle.
  std::string typed = code.substr(0, 3) + "-" + code.substr(3);
  std::ranges::transform(typed, typed.begin(), [](char c) {
    return static_cast<char>(std::tolower(c));
  });
  join.set_code(typed);
  const TeamLobby::Outcome joined = lobby.Request("e", join);
  CHECK(joined.changed && joined.notice.empty());
  CHECK(lobby.TeamOf("e") == lobby.TeamOf("a"));
  // Only the leader sees the code.
  CHECK(lobby.StateFor("e").team().code().empty());
  CHECK(lobby.StateFor("a").team().code() == code);
  // In a team already: refused.
  CHECK(!lobby.Request("e", join).notice.empty());
  // A wrong code is refused.
  bbcoop::TeamRequest wrong = Request(Action::JOIN_CODE);
  wrong.set_code(code == "222222" ? "333333" : "222222");
  CHECK(!lobby.Request("b", wrong).notice.empty());
  CHECK(!lobby.TeamOf("b"));
  // A full team is refused.
  join.set_code(code);
  CHECK(lobby.Request("b", join).notice.empty());
  CHECK(lobby.Request("c", join).notice.empty());
  CHECK(!lobby.Request("d", join).notice.empty());
  CHECK(!lobby.TeamOf("d"));
  // Only the leader makes a new code; the old one joins no more.
  CHECK(!lobby.Request("b", Request(Action::NEW_CODE)).notice.empty());
  lobby.Request("c", Request(Action::LEAVE));
  CHECK(lobby.Request("a", Request(Action::NEW_CODE)).changed);
  const std::string fresh = lobby.StateFor("a").team().code();
  CHECK(fresh != code && NormalizeTeamCode(fresh) == fresh);
  CHECK(!lobby.Request("d", join).notice.empty());
  join.set_code(fresh);
  CHECK(lobby.Request("d", join).notice.empty());
  CHECK(lobby.TeamOf("d") == lobby.TeamOf("a"));
  return 0;
}

static int TeamCodesAreUnique() {
  TeamLobby lobby = NewLobby();
  // A source that repeats itself: the second team does not get the first's.
  lobby.SetCodeSource([n = 0]() mutable {
    ++n;
    return n <= 3 ? std::string{"AAAAAA"} : std::string{"BBBBBB"};
  });
  lobby.Arrive("a", "a", false);
  lobby.Arrive("b", "b", false);
  lobby.Request("a", Request(Action::CREATE));
  lobby.Request("b", Request(Action::CREATE));
  CHECK(lobby.StateFor("a").team().code() == "AAAAAA");
  CHECK(lobby.StateFor("b").team().code() == "BBBBBB");
  // A new code is never the team's own again.
  lobby.SetCodeSource([n = 0]() mutable {
    ++n;
    return n <= 2 ? std::string{"BBBBBB"} : std::string{"CCCCCC"};
  });
  lobby.Request("b", Request(Action::NEW_CODE));
  CHECK(lobby.StateFor("b").team().code() == "CCCCCC");
  // The server's own source gives codes in the alphabet.
  TeamLobby real = NewLobby();
  real.Arrive("a", "a", false);
  real.Request("a", Request(Action::CREATE));
  const std::string code = real.StateFor("a").team().code();
  CHECK(code.size() == TeamCodeLength && NormalizeTeamCode(code) == code);
  return 0;
}

// Wrong codes and names are counted per player: past LookupTries a minute,
// even a right one waits (guessing 2^30 codes at 5 a minute takes centuries).
static int WrongLookupsAreLimited() {
  for (const bool hosted : {false, true}) {
    TeamLobby lobby = NewLobby();
    if (hosted) {
      lobby.SetHosted(32, 16);
    }
    lobby.Arrive("a", "Kaniel", false);
    lobby.Arrive("b", "Ada", false);
    lobby.Request("a", Request(Action::CREATE));
    const std::string code = lobby.StateFor("a").team().code();
    bbcoop::TeamRequest wrong = Request(Action::JOIN_CODE);
    wrong.set_code(code == "222222" ? "333333" : "222222");
    for (size_t n = 0; n < TeamLobby::LookupTries; ++n) {
      CHECK(!lobby.Request("b", wrong, 1000 + static_cast<s64>(n))
                 .notice.empty());
    }
    bbcoop::TeamRequest right = Request(Action::JOIN_CODE);
    right.set_code(code);
    const TeamLobby::Outcome held = lobby.Request("b", right, 2000);
    CHECK(held.notice.find("too many") != std::string::npos);
    CHECK(!lobby.TeamOf("b"));
    // Names looked up in vain count too (each player its own).
    bbcoop::TeamRequest by_name = Request(Action::INVITE_NAME);
    by_name.set_name("Gehrman");
    for (size_t n = 0; n < TeamLobby::LookupTries; ++n) {
      CHECK(lobby.Request("a", by_name, 1000).notice.find("too many") ==
            std::string::npos);
    }
    by_name.set_name("Ada");
    CHECK(lobby.Request("a", by_name, 2000).notice.find("too many") !=
          std::string::npos);
    // A minute after the first wrong tries, it is let in.
    CHECK(lobby.Request("b", right, 1000 + TeamLobby::LookupWindowMs)
              .notice.empty());
    CHECK(lobby.TeamOf("b") == lobby.TeamOf("a"));
  }
  return 0;
}

// A friend not listed (hosted: 32 others) is invited by the name the lobby
// shows; the invited player is then listed to its inviter.
static int AFriendIsInvitedByName() {
  TeamLobby lobby = NewLobby();
  lobby.SetHosted(3, 16);
  for (int n = 0; n < 50; ++n) {
    lobby.Arrive("p" + std::to_string(n), "P" + std::to_string(n), false);
  }
  lobby.Arrive("h1", "", false); // "Hunter"
  lobby.Arrive("h2", "", false); // "Hunter"
  bbcoop::TeamRequest invite = Request(Action::INVITE_NAME);
  invite.set_name("p40");
  CHECK(!lobby.Request("p0", invite).notice.empty()); // no team yet
  lobby.Request("p0", Request(Action::CREATE));
  const bool listed_before =
      std::ranges::any_of(lobby.StateFor("p0").players(),
                          [](const auto &p) { return p.member() == "p40"; });
  CHECK(!listed_before);
  invite.set_name(" p40 "); // case and spaces aside
  const TeamLobby::Outcome done = lobby.Request("p0", invite, 10);
  CHECK(done.changed && done.notice.empty());
  CHECK(lobby.StateFor("p40").invites_size() == 1);
  CHECK(std::ranges::any_of(lobby.StateFor("p0").players(),
                            [](const auto &p) { return p.member() == "p40"; }));
  // Two players of that name: refused, nobody invited.
  invite.set_name("Hunter");
  const TeamLobby::Outcome two = lobby.Request("p0", invite, 20);
  CHECK(two.notice.find("several") != std::string::npos);
  CHECK(lobby.StateFor("h1").invites_size() == 0);
  // Nobody of that name, or itself: refused.
  invite.set_name("Gehrman");
  CHECK(!lobby.Request("p0", invite, 30).notice.empty());
  invite.set_name("P0");
  CHECK(!lobby.Request("p0", invite, 40).notice.empty());
  return 0;
}

// A hosted player keeps its newest worlds_per_player worlds: a new one drops
// it from its oldest (one nobody else keeps goes), so CREATE/START/LEAVE again
// and again does not grow the list without end.
static int AHostedPlayersWorldsAreCapped() {
  TeamLobby lobby = NewLobby();
  lobby.SetHosted(32, 3);
  for (const char *m : {"a", "b"}) {
    lobby.Arrive(m, m, false);
  }
  const std::string shared = PlayAWorld(lobby, "a", "b");
  lobby.Request("a", Request(Action::LEAVE));
  lobby.Request("b", Request(Action::LEAVE));
  size_t forgotten = 0;
  for (int round = 0; round < 10; ++round) {
    lobby.Request("a", Request(Action::CREATE));
    lobby.Request("a", Request(Action::READY, {}, {}, 0));
    const TeamLobby::Outcome go = lobby.Request("a", Request(Action::START));
    CHECK(go.notice.empty());
    CHECK(go.forgotten.size() == go.forgotten_lines.size());
    // Each line put aside still names its player: restored, it is a's again.
    for (const bbcoop::TeamWorldInfo &line : go.forgotten_lines) {
      CHECK(std::ranges::count(line.members(), "a") == 1);
    }
    forgotten += go.forgotten.size();
    lobby.Request("a", Request(Action::LEAVE));
  }
  size_t mine = 0;
  for (const bbcoop::TeamWorldInfo &world : lobby.Worlds()) {
    mine += std::ranges::count(world.members(), "a");
  }
  CHECK(mine == 3);
  CHECK(forgotten == 7); // a's own worlds past its 3; the shared one is b's
  CHECK(lobby.Worlds().size() == 4);
  CHECK(lobby.StateFor("b").worlds_size() == 1);
  CHECK(lobby.StateFor("b").worlds(0).id() == shared);
  return 0;
}

// Invasions (docs/INVASIONS.md): only the leader lets invaders in; off by
// default, and the team's members see it.
int TheLeaderAllowsInvaders() {
  TeamLobby lobby = NewLobby();
  lobby.Arrive("a", "Kaniel", false);
  lobby.Arrive("b", "Ada", false);
  lobby.Request("a", Request(Action::CREATE));
  const std::string team = lobby.TeamOf("a")->id();
  lobby.Request("a", Request(Action::INVITE, "b"));
  lobby.Request("b", Request(Action::ACCEPT, {}, team));
  CHECK(!lobby.TeamOf("a")->allow_invaders());
  bbcoop::TeamRequest allow = Request(Action::INVADERS);
  allow.set_allow(true);
  CHECK(!lobby.Request("b", allow).notice.empty());
  CHECK(!lobby.TeamOf("a")->allow_invaders());
  CHECK(lobby.Request("a", allow).changed);
  CHECK(lobby.StateFor("b").team().allow_invaders());
  allow.set_allow(false);
  CHECK(lobby.Request("a", allow).changed);
  CHECK(!lobby.TeamOf("b")->allow_invaders());
  // Not in a team: nothing to allow.
  lobby.Arrive("c", "Lisa", false);
  CHECK(!lobby.Request("c", allow).notice.empty());
  return 0;
}

// Kaniel 2026-10-25 (host rules): the leader's rules are the team's, all off
// by default; a member sees them, only the leader sets them, at any time.
int TheLeaderSetsTheTeamsRules() {
  TwoHunters t;
  TeamLobby &lobby = t.lobby;
  const bbcoop::TeamRules none = lobby.StateFor("b").team().rules();
  CHECK(!none.rot() && !none.travel_together() && !none.travel_vote() &&
        !none.enemy_scaling());
  bbcoop::TeamRequest rules = Request(Action::RULES);
  rules.mutable_rules()->set_rot(true);
  rules.mutable_rules()->set_enemy_scaling(true);
  CHECK(!lobby.Request("b", rules).notice.empty()); // not the leader
  CHECK(!lobby.StateFor("b").team().rules().rot());
  CHECK(lobby.Request("a", rules).changed);
  CHECK(lobby.StateFor("b").team().rules().rot());
  CHECK(lobby.StateFor("b").team().rules().enemy_scaling());
  CHECK(!lobby.Request("a", rules).changed); // the same again: nothing to send
  // While the team plays, too.
  t.Start();
  rules.mutable_rules()->set_rot(false);
  rules.mutable_rules()->set_travel_together(true);
  CHECK(lobby.Request("a", rules).changed);
  const bbcoop::TeamRules now = lobby.StateFor("b").team().rules();
  CHECK(!now.rot() && now.travel_together() && now.enemy_scaling());
  // The rules survive the trip to the game and back (the LobbyState's bytes).
  bbcoop::LobbyState back;
  CHECK(back.ParseFromString(lobby.StateFor("b").SerializeAsString()));
  CHECK(back.team().rules().travel_together() &&
        back.team().rules().enemy_scaling() && !back.team().rules().rot());
  return 0;
}

// A player's games send their rules at the title, before any team: the team
// it makes takes them, and a team it joins keeps its leader's.
int RulesSetBeforeATeamGoIntoTheTeam() {
  TeamLobby lobby = NewLobby();
  lobby.Arrive("a", "Kaniel", false);
  lobby.Arrive("b", "Ada", false);
  bbcoop::TeamRequest rules = Request(Action::RULES);
  rules.mutable_rules()->set_travel_together(true);
  rules.mutable_rules()->set_travel_vote(true);
  CHECK(!lobby.Request("a", rules).changed); // nothing anyone sees yet
  bbcoop::TeamRequest other = Request(Action::RULES);
  other.mutable_rules()->set_rot(true);
  lobby.Request("b", other);
  lobby.Request("a", Request(Action::CREATE));
  const std::string team = lobby.TeamOf("a")->id();
  CHECK(lobby.TeamOf("a")->rules().travel_together());
  CHECK(lobby.TeamOf("a")->rules().travel_vote());
  lobby.Request("a", Request(Action::INVITE, "b"));
  lobby.Request("b", Request(Action::ACCEPT, {}, team));
  CHECK(!lobby.StateFor("b").team().rules().rot()); // the leader's, not b's
  CHECK(lobby.StateFor("b").team().rules().travel_together());
  return 0;
}

// Kaniel 2026-10-25 (the Rot rule): a member's rot is the server's while its
// team lives, so a game that crashes and comes back gets it back. At most
// MaxRot penalties, each a RotKind; a member sets only its own.
int AMembersRotIsKeptWhileItsTeamLives() {
  TwoHunters t;
  TeamLobby &lobby = t.lobby;
  t.Start();
  bbcoop::TeamRequest rot = Request(Action::ROT);
  rot.add_rot(bbcoop::ROT_MAX_HP);
  rot.add_rot(bbcoop::ROT_RALLY);
  CHECK(lobby.Request("b", rot).changed);
  const auto rot_of = [&](const std::string &member) {
    std::vector<uint32_t> out;
    const bbcoop::LobbyState state = lobby.StateFor("a");
    for (const bbcoop::TeamMember &m : state.team().members()) {
      if (m.member() == member) {
        out.assign(m.rot().begin(), m.rot().end());
      }
    }
    return out;
  };
  CHECK((rot_of("b") == std::vector<uint32_t>{1, 4}));
  CHECK(rot_of("a").empty());
  // b's game is gone and back: its rot is still there.
  lobby.Leave("b");
  lobby.Arrive("b", "Ada", true);
  CHECK((rot_of("b") == std::vector<uint32_t>{1, 4}));
  // Too many, or a penalty the game has not: refused, the old rot kept.
  bbcoop::TeamRequest many = Request(Action::ROT);
  for (int i = 0; i < 6; ++i) {
    many.add_rot(bbcoop::ROT_MAX_STAMINA);
  }
  CHECK(!lobby.Request("b", many).notice.empty());
  bbcoop::TeamRequest bad = Request(Action::ROT);
  bad.add_rot(9);
  CHECK(!lobby.Request("b", bad).notice.empty());
  CHECK((rot_of("b") == std::vector<uint32_t>{1, 4}));
  // Cleared (a lamp): empty.
  CHECK(lobby.Request("b", Request(Action::ROT)).changed);
  CHECK(rot_of("b").empty());
  // Out of a team there is no rot to keep.
  lobby.Request("b", Request(Action::LEAVE));
  CHECK(!lobby.Request("b", rot).notice.empty());
  return 0;
}

// Kaniel 2026-10-25: how a member's death plays out is the team's, one for
// all of it: going down for a revive (the default) or spectating the
// teammates. Only the leader picks it, at any time (also while the team
// plays), and every member's state carries the leader's pick.
int TheLeaderPicksTheTeamsDeathMode() {
  TwoHunters t;
  TeamLobby &lobby = t.lobby;
  const auto pick = [&](const std::string &member, uint32_t mode) {
    bbcoop::TeamRequest request = Request(Action::DEATH_MODE);
    request.set_death_mode(mode);
    return lobby.Request(member, request);
  };
  CHECK(lobby.TeamOf("a")->death_mode() == 0);
  CHECK(lobby.StateFor("b").team().death_mode() == 0);
  CHECK(!pick("b", 1).notice.empty()); // the leader only
  CHECK(lobby.TeamOf("a")->death_mode() == 0);
  CHECK(pick("a", 1).changed);
  CHECK(lobby.StateFor("a").team().death_mode() == 1);
  CHECK(lobby.StateFor("b").team().death_mode() == 1);
  CHECK(!pick("a", 1).changed);        // the same again changes nothing
  CHECK(!pick("a", 7).notice.empty()); // no such mode
  CHECK(lobby.TeamOf("a")->death_mode() == 1);
  // While the team plays, too.
  t.Start();
  CHECK(pick("a", 0).changed);
  CHECK(lobby.StateFor("b").team().death_mode() == 0);
  // In no team there is no team's mode to pick.
  lobby.Arrive("c", "Lisa", false);
  CHECK(!pick("c", 1).notice.empty());
  return 0;
}

int main() {
  int failed = 0;
  failed |= TheLeaderSetsTheTeamsRules();
  failed |= RulesSetBeforeATeamGoIntoTheTeam();
  failed |= AMembersRotIsKeptWhileItsTeamLives();
  failed |= TheLeaderPicksTheTeamsDeathMode();
  failed |= PlayersAreSeenByTheirCharactersName();
  failed |= TeamFormsByInvite();
  failed |= TeamHoldsFour();
  failed |= ReadyAndStart();
  failed |= StartedTeamKeepsGoingWhileOnePlays();
  failed |= AStartedTeamStaysStartedWhileItsGamesLoad();
  failed |= LeaveAndRemove();
  failed |= OfflineMemberLeavesAfterGrace();
  failed |= ATeamIsGoneOnceEveryMemberIsGone();
  failed |= GuestsKeepTheirPlayerSlots();
  failed |= AMemberOutOfATeamIsNamed();
  failed |= TheLeaderPicksAnyOfTheServersWorlds();
  failed |= AnyTeamPlaysAnyWorld();
  failed |= TheLeaderForgetsAWorld();
  failed |= TheServerKeepsItsLastWorlds();
  failed |= AWorldsLampsAreCounted();
  failed |= AWorldsLineSaysWhoPlayedItAndWhenItWasMade();
  failed |= AWorldPlayedNowSaysByWhichTeam();
  failed |= APickBeforeATeamGoesIntoTheTeam();
  failed |= HostedWorldsAreTheirPlayersOnly();
  failed |= HostedServersKeepEveryPlayersWorlds();
  failed |= AHostedPlayersWorldsAreCapped();
  failed |= AHostedLobbyListsFewOthers();
  failed |= TheLeaderAllowsInvaders();
  failed |= AFriendJoinsByTheTeamsCode();
  failed |= TeamCodesAreUnique();
  failed |= WrongLookupsAreLimited();
  failed |= AFriendIsInvitedByName();
  if (failed == 0) {
    std::cout << "team lobby: all checks passed\n";
  }
  return failed;
}
