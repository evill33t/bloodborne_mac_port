// SPDX-FileCopyrightText: Copyright 2026 CLS_AMG (Amanises)
// SPDX-License-Identifier: GPL-2.0-or-later

// Amanises 2026-10-25: friends in the team lobby. The friendships are shadNet's own (the
// friendship table, the same the game's friend list and the WebAPI's friendList read); the lobby
// only finds players by name, sends and answers requests, and shows each player its friends with
// their lobby state. The book is what the lobby (bloodborne_world.h) asks: shadNet's database in
// the server (link_server.cpp), a plain map in the tests.
#pragma once

#include <string>
#include <utility>
#include <vector>

#include "coop.pb.h"

namespace BloodborneWorld {

// A player as the book knows it: its lobby member (the online id in hex) and its name.
struct FriendEntry {
    std::string member;
    std::string name;
    bool operator==(const FriendEntry&) const = default;
};

struct FriendRelations {
    std::vector<FriendEntry> friends;  // both sides said yes
    std::vector<FriendEntry> received; // they asked, we did not answer yet
    std::vector<FriendEntry> sent;     // we asked, they did not answer yet
};

class FriendBook {
public:
    virtual ~FriendBook() = default;
    virtual FriendRelations Of(const std::string& member) = 0;
    // Each returns why it was refused, empty when done. Add sends a request, or accepts the one
    // `other` sent (they are friends then).
    virtual std::string Add(const std::string& member, const std::string& other) = 0;
    // Off the friends, or the request sent to `other` taken back.
    virtual std::string Remove(const std::string& member, const std::string& other) = 0;
    // The request `other` sent refused.
    virtual std::string Decline(const std::string& member, const std::string& other) = 0;
    // Accounts whose name holds `query` (any case), at most `limit`, `member` itself included.
    virtual std::vector<FriendEntry> Search(const std::string& member, const std::string& query,
                                            size_t limit) = 0;
};

// How many a search lists.
constexpr size_t FriendSearchLimit = 20;
// Shortest search text the server answers (one letter would list half the server).
constexpr size_t FriendSearchMinLength = 2;

// `state` (TeamLobby::StateFor(member)) with the friends added: each friend, request and search
// result with its lobby state and team from the signed-in players, friends online first, and
// every listed player's relation to `member`.
void AddFriendsToLobby(bbcoop::LobbyState& state, const std::string& member,
                       const FriendRelations& relations, const std::string& search_query,
                       const std::vector<FriendEntry>& search);

} // namespace BloodborneWorld
