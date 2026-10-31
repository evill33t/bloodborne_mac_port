// SPDX-FileCopyrightText: Copyright 2026 CLS_AMG (Amanises)
// SPDX-License-Identifier: GPL-2.0-or-later
#include "lobby_friends.h"

#include <algorithm>
#include <map>

namespace BloodborneWorld {

void AddFriendsToLobby(bbcoop::LobbyState& state, const std::string& member,
                       const FriendRelations& relations, const std::string& search_query,
                       const std::vector<FriendEntry>& search) {
    // The signed-in players as the lobby has them (everyone but `member`).
    std::map<std::string, const bbcoop::LobbyPlayer*> online;
    for (const bbcoop::LobbyPlayer& player : state.players()) {
        online[player.member()] = &player;
    }
    std::map<std::string, bbcoop::LobbyPlayer::Relation> relation;
    for (const FriendEntry& entry : relations.friends) {
        relation[entry.member] = bbcoop::LobbyPlayer::FRIEND;
    }
    for (const FriendEntry& entry : relations.sent) {
        relation[entry.member] = bbcoop::LobbyPlayer::REQUEST_SENT;
    }
    for (const FriendEntry& entry : relations.received) {
        relation[entry.member] = bbcoop::LobbyPlayer::REQUEST_RECEIVED;
    }
    const auto fill = [&](bbcoop::LobbyPlayer& out, const FriendEntry& entry) {
        out.set_member(entry.member);
        out.set_name(entry.name);
        if (const auto found = online.find(entry.member); found != online.end()) {
            out.set_name(found->second->name());
            out.set_state(found->second->state());
            out.set_team(found->second->team());
        }
        if (entry.member == member) {
            out.set_relation(bbcoop::LobbyPlayer::SELF);
        } else if (const auto r = relation.find(entry.member); r != relation.end()) {
            out.set_relation(r->second);
        }
    };

    std::vector<bbcoop::LobbyPlayer> friends;
    for (const FriendEntry& entry : relations.friends) {
        fill(friends.emplace_back(), entry);
    }
    std::stable_sort(friends.begin(), friends.end(),
                     [](const bbcoop::LobbyPlayer& a, const bbcoop::LobbyPlayer& b) {
                         const bool a_on = a.state() != bbcoop::LobbyPlayer::OFFLINE;
                         const bool b_on = b.state() != bbcoop::LobbyPlayer::OFFLINE;
                         if (a_on != b_on) {
                             return a_on;
                         }
                         return a.name() < b.name();
                     });
    for (bbcoop::LobbyPlayer& f : friends) {
        *state.add_friends() = std::move(f);
    }
    for (const FriendEntry& entry : relations.received) {
        fill(*state.add_friend_requests(), entry);
    }
    for (const FriendEntry& entry : relations.sent) {
        fill(*state.add_friend_requests_sent(), entry);
    }
    state.set_search_query(search_query);
    for (const FriendEntry& entry : search) {
        fill(*state.add_search(), entry);
    }
    // The players online: their relation, friends first (the list shows the first few).
    for (bbcoop::LobbyPlayer& player : *state.mutable_players()) {
        if (const auto r = relation.find(player.member()); r != relation.end()) {
            player.set_relation(r->second);
        }
    }
    std::stable_partition(
        state.mutable_players()->begin(), state.mutable_players()->end(),
        [](const bbcoop::LobbyPlayer& p) { return p.relation() == bbcoop::LobbyPlayer::FRIEND; });
}

} // namespace BloodborneWorld
