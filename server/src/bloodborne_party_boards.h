// SPDX-FileCopyrightText: Copyright 2026 shadNet Project
// SPDX-License-Identifier: GPL-2.0-or-later
// The party's boards on the server's world (phase 3c), what the HTTP boards (/bloodborne/party/*)
// were: the map each member is on, who rests at which lamp, the trophies the party unlocked and
// each member's vitals for party death, with the wipes the server declares. Members send their
// own part; every member gets the whole board when it changes (bloodborne_world.h sends it).
#pragma once

#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "coop.pb.h"

namespace BloodborneWorld {

class PartyBoards {
public:
    // A member's map, rest or vitals not heard of for this long is gone (its game went).
    static constexpr std::int64_t FreshMs = 5'000;
    // A wipe stays on the board this long, so every member it takes reads it.
    static constexpr std::int64_t WipeMs = 20'000;
    static constexpr std::int32_t MaxTrophyId = 127;

    enum Changed : unsigned {
        None = 0,
        Maps = 1,
        Rests = 2,
        TrophyBoard = 4,
        VitalsBoard = 8,
    };

    // Each returns the boards that changed (Changed bits).
    unsigned Map(const std::string& member, std::uint32_t map, std::int64_t now_ms);
    unsigned Rest(const std::string& member, const bbcoop::Rest& rest, std::int64_t now_ms);
    unsigned Trophies(const bbcoop::Trophies& trophies);
    // The member's vitals; a map is wiped when nobody there stands and someone there is down that
    // no wipe of the last WipeMs took yet.
    unsigned Vitals(const std::string& member, const bbcoop::Vitals& vitals, std::int64_t now_ms);
    // Parts gone stale and wipes gone old.
    unsigned Expire(std::int64_t now_ms);
    // A member's own events took it out (its leave).
    unsigned Forget(const std::string& member);
    // Kaniel 2026-10-16: whether any member but `member` stands (up, on any map). A wipe comes
    // after the death's animation: only a member that stood since `member` fell counts (Kaniel's
    // play test 2026-10-25: the one that fell first stood at the lamp again by then).
    bool SomeoneStandsBesides(const std::string& member) const;
    // Whether a member stands (up) on one of `maps`, as SomeoneStandsBesides, and but for the
    // members of `member`'s wipes (the first of them to respawn may stand at the lamp again before
    // the last one tells its wipe).
    bool SomeoneStandsOn(const std::vector<std::uint32_t>& maps, const std::string& member) const;

    bbcoop::MapBoard MapMessage() const;
    bbcoop::RestBoard RestMessage() const;
    bbcoop::Trophies TrophyMessage() const;
    // Each member as it sent itself (no hits), with the party's revive damage on its down.
    bbcoop::VitalsBoard VitalsMessage() const;

private:
    struct Heard {
        std::int64_t at_ms = 0;
    };
    struct MapEntry : Heard {
        std::uint32_t map = 0;
    };
    struct RestEntry : Heard {
        bbcoop::Rest rest;
    };
    struct VitalsEntry : Heard {
        bbcoop::Vitals vitals;
        std::int64_t up_since_ms = 0; // when it last stood up (0: never)
        std::int64_t fell_ms = 0;     // when it last went down or died (0: never)
    };
    // Whether `other` stands now and has stood since `member` fell.
    bool StandsSinceFell(const VitalsEntry& other, const std::string& member) const;
    struct Wipe {
        bbcoop::VitalsBoard::Wipe wipe;
        std::int64_t declared_ms = 0;
    };

    std::map<std::string, MapEntry> maps;
    std::map<std::string, RestEntry> rests;
    std::set<std::int32_t> trophies;
    std::map<std::string, VitalsEntry> vitals;
    std::vector<Wipe> wipes;
    std::uint32_t last_wipe = 0;
};

// Phase 4 (docs/SERVER_COOP_PLAN.md): one game runs each occupied map's enemies: the session's
// host while it is there (the game is built around the host running them: a host that followed
// a guest put its own ownership words back every frame, rig 2026-10-14), else the member that
// got there first (its committed map: the game's place tracker holds a border crossing for its
// steady time, MemberMap). It is handed on when that member leaves the area, loads (map 0) or
// goes silent, to the member there longest; every change counts the epoch up.
class EnemyOwners {
public:
    // A member whose map was not heard of for this long is gone (its game sends it every second).
    static constexpr std::int64_t SilentMs = 2'500;
    // The host takes an area another member runs back once it has been there this long: until
    // then its game follows that member's, so its copies take their places and HP (Kaniel's play
    // test 2026-10-18: a guest's hurt enemy went back to its spawn as the host came, the host's
    // fresh copy stood there). A host stepping over a border and back starts again.
    static constexpr std::int64_t HostTakeBackMs = 3'000;
    // What an owner runs: one map (a block). The blocks of an area are loaded together, but the
    // game wakes a block's enemies only near a player in that world (rig 2026-10-14: a guest in
    // Cathedral Ward following the host in Central Yharnam saw them stand still).
    static constexpr std::uint32_t AreaOf(std::uint32_t map) {
        return map;
    }

    // A member's map now (0: loading or not in the world); `host`: it hosts the session. True
    // when an owner changed.
    bool Place(const std::string& member, std::uint32_t map, std::int64_t now_ms,
               bool host = false);
    // Members gone silent leave their areas.
    bool Expire(std::int64_t now_ms);
    // A member's own events took it out (its leave).
    bool Forget(const std::string& member);
    // The owner of the area of `map`, or "" when nobody runs it.
    std::string OwnerOf(std::uint32_t map) const;
    bbcoop::EnemyOwners Message() const;

private:
    struct Presence {
        std::uint32_t area = 0;
        std::int64_t since_ms = 0; // on this area since
        std::int64_t heard_ms = 0;
        bool host = false;
    };
    struct Owner {
        std::string member;
        std::uint64_t epoch = 0;
    };
    bool Settle();

    std::map<std::string, Presence> members;
    std::map<std::uint32_t, Owner> owners;
    std::uint64_t epoch = 0;
    std::int64_t latest_ms = 0; // the latest Place or Expire
};

} // namespace BloodborneWorld
