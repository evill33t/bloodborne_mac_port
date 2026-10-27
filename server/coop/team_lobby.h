// SPDX-FileCopyrightText: Copyright 2026 shadNet Project
// SPDX-License-Identifier: GPL-2.0-or-later

// The team lobby's rules (phase 5, docs/SERVER_COOP_PLAN.md "The team lobby"): who is signed in,
// the teams (up to TeamSize members, a leader that hosts), the invites, Ready and Start. shadNet
// keeps one TeamLobby (bloodborne_world.h) and sends every signed-in player its LobbyState after a
// change. Kaniel 2026-10-17: a team is only a session, kept in memory: a member whose game stays
// gone for OfflineGraceMs leaves its team (Expire), and once every member's game is gone the team
// is gone. The worlds are the server's, apart from the teams (any team plays any of them), and the
// server keeps their list on disk (bloodborne_world.h).
#pragma once

#include <functional>
#include <map>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "coop.pb.h"
#include "coop/coop_types.h"

namespace BloodborneCoop {

// The game's remote player slots: the host and three more.
constexpr size_t TeamSize = 4;

// Kaniel 2026-10-16: how long a member's game may stay gone (a crash and a restart) before the
// member leaves its team; a team with nobody left is gone.
constexpr s64 OfflineGraceMs = 10 * 60 * 1000;

// Kaniel 2026-10-17: the server lists this many worlds (the last played); a new one past them puts
// the least lately played aside.
constexpr size_t MaxWorlds = 16;

// Kaniel 2026-10-25 (the Rot rule): a player's rot stacks up to this many penalties.
constexpr size_t MaxRot = 5;

// Kaniel 2026-10-25: the team's death modes (Team.death_mode): 0 down for a revive, 1 spectate.
constexpr u32 MaxDeathMode = 1;

// How far a world is, for the players: its lamps lit, from its world flags on (a lamp's active
// flag, 10007810 + map * 10000 + lamp * 20, maps 210-399, lamps 0-19: the game's FlagTable).
u32 LampsLit(std::span<const s32> on);

class TeamLobby {
public:
    // `make_id` names a new team or world (random on the server, counted in the tests).
    explicit TeamLobby(std::function<std::string()> make_id);

    // Hosted (docs/HOSTED_SERVER.md): players who do not know each other. A world is its players'
    // (TeamWorldInfo.members: those of the teams that played it), listed to them only, a
    // player's last `worlds_per_player`, and forgotten only by them (for themselves; a world
    // nobody keeps goes). A player's lobby lists its team, its invites and at most
    // `lobby_players` others not in a team (the server's whole list for everyone was O(n^2)).
    void SetHosted(size_t lobby_players, size_t worlds_per_player) {
        hosted = true;
        this->lobby_players = lobby_players;
        this->worlds_per_player = worlds_per_player;
    }
    bool Hosted() const {
        return hosted;
    }

    // A friend the lobby does not list joins by its team's code or is invited by its name. Wrong
    // codes and names a player tries are counted: past LookupTries in LookupWindowMs every lookup
    // of it is refused until the oldest is a window old (codes cannot be guessed).
    static constexpr size_t LookupTries = 5;
    static constexpr s64 LookupWindowMs = 60 * 1000;
    // Where team codes come from (random on the server; a test's own).
    void SetCodeSource(std::function<std::string()> source) {
        make_code = std::move(source);
    }

    // Kaniel 2026-10-16 (the team menu in game): a member out of a team, removed by its leader or
    // gone itself; the server takes it out of that team's world too.
    struct Departure {
        std::string member;
        std::string team;
        std::string party; // the party of the world the team plays (Team.party), if any
        bool operator==(const Departure&) const = default;
    };
    // A region of shards (docs/SCALING.md): a player this shard sends to another one, with the
    // requests it runs there (in a region ticket), and an invite for another shard's player.
    struct Move {
        std::string member;
        std::string shard;
        std::vector<bbcoop::TeamRequest> requests;
    };
    struct RemoteInvite {
        std::string shard;
        std::string member;
        bbcoop::TeamInvite invite; // its team "@<this shard>/<code>"
    };
    struct Outcome {
        bool changed = false;            // send everyone their state again
        std::string notice;              // why the request was refused (to the requester)
        std::vector<std::string> starts; // members to send a TeamStart now
        std::vector<Departure> departed; // members out of their team
        // Worlds off the list (forgotten by a leader, or past MaxWorlds): their folders go aside.
        std::vector<std::string> forgotten;
        std::vector<bbcoop::TeamWorldInfo> forgotten_lines; // their lines, as they were
        // Kaniel 2026-10-17: a world's line changed (a character played in it): the list to disk.
        bool worlds_changed = false;
        std::vector<Move> moves;
        std::vector<RemoteInvite> remote_invites;
    };

    // A region: where what this shard does not have is (the directory's view, a few seconds old).
    // A code or name not here is looked up there; the joiner moves to the leader's shard and joins
    // there (JOIN_CODE), an invited player hears its invite from its own shard. A Start of a world
    // another shard holds moves the team there.
    struct Elsewhere {
        std::string shard;
        std::string member;
    };
    struct Region {
        std::function<std::string(const std::string& code)> shard_of_code; // "": none
        std::function<std::vector<Elsewhere>(const std::string& folded_name)> named;
        std::function<std::string(const std::string& world)> world_holder; // "": this one
    };
    void SetRegion(Region region) {
        this->region = std::move(region);
    }
    void SetShardName(std::string name) {
        shard_name = std::move(name);
    }
    // An invite from another shard's team (its team "@<shard>/<code>"), listed as any other;
    // whether `member` is here to see it.
    Outcome InviteFromElsewhere(const std::string& member, const bbcoop::TeamInvite& invite);
    // Who is signed in (member, name) and the teams' codes: what the shard tells the directory.
    std::vector<std::pair<std::string, std::string>> Players() const;
    std::vector<std::string> Codes() const;
    // Names as a lookup compares them.
    static std::string Fold(std::string_view name);

    // A player signed in (or said it now plays / is back at the title).
    Outcome Arrive(const std::string& member, const std::string& name, bool playing);
    // Its game is gone. A team none of whose members is signed in any more is gone.
    Outcome Leave(const std::string& member);
    // Members gone for OfflineGraceMs leave their teams. The clock of a member starts at the first
    // call that finds it gone.
    Outcome Expire(s64 now_ms);
    // `now_ms` counts wrong lookups (JOIN_CODE, INVITE_NAME).
    Outcome Request(const std::string& member, const bbcoop::TeamRequest& request, s64 now_ms = 0);

    // What `member` sees.
    bbcoop::LobbyState StateFor(const std::string& member) const;
    // The TeamStart for `member` (its team must be started).
    bbcoop::TeamStart StartFor(const std::string& member) const;
    // The team `member` is in, or none.
    const bbcoop::Team* TeamOf(const std::string& member) const;
    std::vector<std::string> SignedIn() const;

    // Kaniel 2026-10-17: the server's worlds, the last played first (its store's, at its start).
    void SetWorlds(std::vector<bbcoop::TeamWorldInfo> list);
    const std::vector<bbcoop::TeamWorldInfo>& Worlds() const {
        return worlds;
    }
    // What the server learnt of one of its worlds (its host's character, its flags and lamps, when
    // a Start played it, when it was made: kept from the first note; characters that played in it:
    // added to its own; empty or 0 fields say nothing). A world not listed stays unlisted.
    // Whether anything changed.
    bool NoteWorld(const bbcoop::TeamWorldInfo& note);
    // An id no listed world (nor team) has.
    std::string NewWorldId();
    // The operator's (admin.h): the worlds last played (never played: made) before `before_ms`
    // that no team has picked, off the list (and no player's pick); their lines.
    std::vector<bbcoop::TeamWorldInfo> TakeIdleWorlds(s64 before_ms);
    // A world put aside back on the list; false when a listed world has its id.
    bool AddWorld(bbcoop::TeamWorldInfo world);

    // The party key of a world: "world:<id>".
    static std::string PartyOf(const std::string& world);

private:
    struct Player {
        std::string name;
        bool playing = false;
        // Kaniel 2026-10-17: the world it picked while in no team (the team it makes takes it).
        std::string world;
        // Kaniel 2026-10-25: its launcher's host rules (the team it makes takes them).
        bbcoop::TeamRules rules;
    };

    bbcoop::Team* FindTeam(const std::string& id);
    bbcoop::TeamMember* MemberIn(bbcoop::Team& team, const std::string& member);
    bbcoop::LobbyPlayer::State StateOf(const std::string& member) const;
    void RemoveFrom(bbcoop::Team& team, const std::string& member);
    void DropTeam(const std::string& id);
    bbcoop::TeamWorldInfo* FindWorld(const std::string& id);
    // The started team (not `except`) that plays `world`, if any.
    const bbcoop::Team* PlayedBy(const std::string& world, const bbcoop::Team* except) const;
    static void PickWorld(bbcoop::Team& team, const std::string& world);
    // Past MaxWorlds: the least lately played no team has picked go. Hosted: each member of the
    // `started` team past its worlds_per_player leaves its oldest; a world nobody keeps goes.
    void KeepLastWorlds(const bbcoop::Team& started, std::vector<std::string>& forgotten,
                        std::vector<bbcoop::TeamWorldInfo>& forgotten_lines);
    // Phase 7: the leader's sign is 0 (it hosts); a guest keeps its own, one without takes the
    // first free of 7, 6, 5.
    static void GiveSigns(bbcoop::Team& team);
    // A started team nobody plays in any more is not started.
    void Settle();
    // A code no team has.
    std::string NewCode();
    // Whether `member` may look up a code or a name now; a wrong one is noted.
    bool MayLookUp(const std::string& member, s64 now_ms);
    void WrongLookup(const std::string& member, s64 now_ms);
    void AddMember(bbcoop::Team& team, const std::string& member);

    // Who is in which team (an index for StateFor), made again after any change.
    void Index() const;
    void Changed() const {
        indexed = false;
    }

    bool hosted = false;
    Region region;
    std::string shard_name;
    size_t lobby_players = 0;
    size_t worlds_per_player = 0;
    mutable bool indexed = false;
    mutable std::map<std::string, size_t> team_index; // member -> its team's place in `teams`

    std::function<std::string()> make_id;
    std::function<std::string()> make_code;
    std::map<std::string, std::vector<s64>> wrong_lookups; // by player, the last window's
    std::map<std::string, Player> players;                 // signed in
    std::vector<bbcoop::Team> teams;
    std::vector<bbcoop::TeamWorldInfo> worlds;                      // the last played first
    std::map<std::string, std::vector<bbcoop::TeamInvite>> invites; // by invited member
    std::map<std::string, std::string> names; // every member's last known name
    std::map<std::string, s64> gone_since;    // team members not signed in, since when
    // Kaniel 2026-10-16: the started teams one of whose games played since the Start (a team
    // whose games still load is not over: a member invited in game joins it with its Ready).
    std::set<std::string> played;
};

} // namespace BloodborneCoop
