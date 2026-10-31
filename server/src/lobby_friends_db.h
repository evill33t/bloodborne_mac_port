// SPDX-FileCopyrightText: Copyright 2026 CLS_AMG (Amanises)
// SPDX-License-Identifier: GPL-2.0-or-later

// Amanises 2026-10-25: the lobby's friends in shadNet's database (lobby_friends.h). The rules are
// cmd_friend.cpp's AddFriend/RemoveFriend: a request is the asker's Friend bit, a friendship both
// bits, a block on either side refuses. A change also reaches the players' shadNet connections
// (their friend lists and FriendQuery/FriendNew/FriendLost), as the game's own requests do.
#pragma once

#include <optional>

#include "database.h"
#include "lobby_friends.h"

struct SharedState;

namespace BloodborneWorld {

class DatabaseFriendBook final : public FriendBook {
public:
    // `db` is open; `shared` may be null (no live connections told).
    DatabaseFriendBook(Database& db, SharedState* shared) : db{db}, shared{shared} {}

    FriendRelations Of(const std::string& member) override;
    std::string Add(const std::string& member, const std::string& other) override;
    std::string Remove(const std::string& member, const std::string& other) override;
    std::string Decline(const std::string& member, const std::string& other) override;
    std::vector<FriendEntry> Search(const std::string& member, const std::string& query,
                                    size_t limit) override;

    // A lobby member is the online id in hex; the account's name is the online id.
    static QString NpidOf(const std::string& member);
    static std::string MemberOf(const QString& npid);

private:
    struct Account {
        int64_t id = 0;
        QString npid;
    };
    std::optional<Account> Find(const std::string& member);
    void Notify(int64_t to, NotificationType type, const QByteArray& payload);
    void Befriended(const Account& a, const Account& b);
    void Unfriended(const Account& a, const Account& b);

    Database& db;
    SharedState* shared;
};

} // namespace BloodborneWorld
