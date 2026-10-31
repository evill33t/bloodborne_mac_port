// SPDX-FileCopyrightText: Copyright 2026 CLS_AMG (Amanises)
// SPDX-License-Identifier: GPL-2.0-or-later
#include "lobby_friends_db.h"

#include <QDebug>
#include <QReadLocker>
#include <QWriteLocker>

#include "client_session.h"
#include "proto_utils.h"
#include "shadnet.pb.h"

namespace BloodborneWorld {

namespace {

constexpr uint8_t F = static_cast<uint8_t>(FriendStatus::Friend);
constexpr uint8_t B = static_cast<uint8_t>(FriendStatus::Blocked);

template <typename T>
QByteArray Payload(const T& message) {
    QByteArray buffer;
    appendProto(buffer, message);
    return buffer;
}

std::vector<FriendEntry> Entries(const QList<QPair<int64_t, QString>>& list) {
    std::vector<FriendEntry> out;
    out.reserve(static_cast<size_t>(list.size()));
    for (const auto& [id, npid] : list) {
        out.push_back({DatabaseFriendBook::MemberOf(npid), npid.toStdString()});
    }
    return out;
}

} // namespace

QString DatabaseFriendBook::NpidOf(const std::string& member) {
    return QString::fromUtf8(QByteArray::fromHex(QByteArray::fromStdString(member)));
}

std::string DatabaseFriendBook::MemberOf(const QString& npid) {
    return npid.toUtf8().toHex().toStdString();
}

std::optional<DatabaseFriendBook::Account> DatabaseFriendBook::Find(const std::string& member) {
    const QString npid = NpidOf(member);
    if (npid.isEmpty()) {
        return std::nullopt;
    }
    const auto id = db.GetUserId(npid);
    if (!id) {
        return std::nullopt;
    }
    // The name as the account has it (the lookup ignores case).
    return Account{*id, db.GetUsername(*id).value_or(npid)};
}

FriendRelations DatabaseFriendBook::Of(const std::string& member) {
    FriendRelations out;
    const auto me = Find(member);
    if (!me) {
        return out;
    }
    const UserRelationships rel = db.GetRelationships(me->id);
    out.friends = Entries(rel.friends);
    out.received = Entries(rel.friendRequestsReceived);
    out.sent = Entries(rel.friendRequestsSent);
    return out;
}

void DatabaseFriendBook::Notify(int64_t to, NotificationType type, const QByteArray& payload) {
    if (!shared) {
        return;
    }
    std::function<void(QByteArray)> send;
    {
        QReadLocker lock(&shared->clientsLock);
        const auto it = shared->clients.find(to);
        if (it == shared->clients.end()) {
            return;
        }
        send = it->send;
    }
    if (send) {
        send(ClientSession::BuildNotification(type, payload));
    }
}

void DatabaseFriendBook::Befriended(const Account& a, const Account& b) {
    bool a_online = false, b_online = false;
    if (shared) {
        QWriteLocker lock(&shared->clientsLock);
        if (const auto it = shared->clients.find(a.id); it != shared->clients.end()) {
            it->friends.insert(b.id, b.npid);
            a_online = true;
        }
        if (const auto it = shared->clients.find(b.id); it != shared->clients.end()) {
            it->friends.insert(a.id, a.npid);
            b_online = true;
        }
    }
    shadnet::NotifyFriendNew to_a;
    to_a.set_npid(b.npid.toStdString());
    to_a.set_online(b_online);
    Notify(a.id, NotificationType::FriendNew, Payload(to_a));
    shadnet::NotifyFriendNew to_b;
    to_b.set_npid(a.npid.toStdString());
    to_b.set_online(a_online);
    Notify(b.id, NotificationType::FriendNew, Payload(to_b));
}

void DatabaseFriendBook::Unfriended(const Account& a, const Account& b) {
    if (shared) {
        QWriteLocker lock(&shared->clientsLock);
        if (const auto it = shared->clients.find(a.id); it != shared->clients.end()) {
            it->friends.remove(b.id);
        }
        if (const auto it = shared->clients.find(b.id); it != shared->clients.end()) {
            it->friends.remove(a.id);
        }
    }
    shadnet::NotifyFriendLost to_a;
    to_a.set_npid(b.npid.toStdString());
    Notify(a.id, NotificationType::FriendLost, Payload(to_a));
    shadnet::NotifyFriendLost to_b;
    to_b.set_npid(a.npid.toStdString());
    Notify(b.id, NotificationType::FriendLost, Payload(to_b));
}

std::string DatabaseFriendBook::Add(const std::string& member, const std::string& other) {
    const auto me = Find(member);
    const auto them = Find(other);
    if (!me || !them) {
        return "no such player";
    }
    if (me->id == them->id) {
        return "that is you";
    }
    const auto [res, rel] = db.GetRelStatus(me->id, them->id);
    if (res == Database::RelResult::Error) {
        return "the server could not read its friends";
    }
    const uint8_t mine = res == Database::RelResult::Ok ? rel.caller : 0;
    const uint8_t theirs = res == Database::RelResult::Ok ? rel.other : 0;
    if ((mine & B) || (theirs & B)) {
        return "this player cannot be added";
    }
    if (mine & F) {
        return theirs & F ? "already friends" : "request already sent";
    }
    if (!db.SetRelStatus(me->id, them->id, mine | F, theirs)) {
        return "the server could not write its friends";
    }
    if (theirs & F) {
        Befriended(*me, *them);
        qInfo().noquote() << "Lobby friends: friendship formed:" << me->npid << "<->" << them->npid;
    } else {
        shadnet::NotifyFriendQuery query;
        query.set_from_npid(me->npid.toStdString());
        Notify(them->id, NotificationType::FriendQuery, Payload(query));
        qInfo().noquote() << "Lobby friends: request sent:" << me->npid << "->" << them->npid;
    }
    return {};
}

std::string DatabaseFriendBook::Remove(const std::string& member, const std::string& other) {
    const auto me = Find(member);
    const auto them = Find(other);
    if (!me || !them || me->id == them->id) {
        return "no such player";
    }
    const auto [res, rel] = db.GetRelStatus(me->id, them->id);
    if (res != Database::RelResult::Ok || !(rel.caller & F)) {
        return "not a friend";
    }
    // Both Friend bits go (cmd_friend.cpp's RemoveFriend): taking a request back clears only ours.
    const bool were_friends = (rel.other & F) != 0;
    const uint8_t mine = rel.caller & static_cast<uint8_t>(~F);
    const uint8_t theirs = rel.other & static_cast<uint8_t>(~F);
    const bool ok = mine == 0 && theirs == 0 ? db.DeleteRel(me->id, them->id)
                                             : db.SetRelStatus(me->id, them->id, mine, theirs);
    if (!ok) {
        return "the server could not write its friends";
    }
    if (were_friends) {
        Unfriended(*me, *them);
    }
    qInfo().noquote() << "Lobby friends:" << me->npid
                      << (were_friends ? "removed" : "took back the request to") << them->npid;
    return {};
}

std::string DatabaseFriendBook::Decline(const std::string& member, const std::string& other) {
    const auto me = Find(member);
    const auto them = Find(other);
    if (!me || !them || me->id == them->id) {
        return "no such player";
    }
    const auto [res, rel] = db.GetRelStatus(me->id, them->id);
    if (res != Database::RelResult::Ok || !(rel.other & F) || (rel.caller & F)) {
        return "no request from this player";
    }
    const uint8_t theirs = rel.other & static_cast<uint8_t>(~F);
    const bool ok = rel.caller == 0 && theirs == 0
                        ? db.DeleteRel(me->id, them->id)
                        : db.SetRelStatus(me->id, them->id, rel.caller, theirs);
    if (!ok) {
        return "the server could not write its friends";
    }
    // The asker's game drops the request it sent, as after a RemoveFriend.
    shadnet::NotifyFriendLost lost;
    lost.set_npid(me->npid.toStdString());
    Notify(them->id, NotificationType::FriendLost, Payload(lost));
    qInfo().noquote() << "Lobby friends:" << me->npid << "declined" << them->npid;
    return {};
}

std::vector<FriendEntry> DatabaseFriendBook::Search(const std::string& member,
                                                    const std::string& query, size_t limit) {
    Q_UNUSED(member);
    return Entries(
        db.SearchUsernames(QString::fromStdString(query).trimmed(), static_cast<int>(limit)));
}

} // namespace BloodborneWorld
