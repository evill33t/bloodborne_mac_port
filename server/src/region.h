// SPDX-FileCopyrightText: Copyright 2026 shadNet Project
// SPDX-License-Identifier: GPL-2.0-or-later
// A region of several shadNet processes, the shards (docs/SCALING.md): what a shard knows of the
// others (the directory's answer to its heartbeat), the region tickets it gives a player it sends
// to another shard, and where a hello goes when this shard is full or draining.
#pragma once

#include <map>
#include <optional>
#include <string>
#include <vector>

#include <QByteArray>
#include <QJsonObject>

#include "coop.pb.h"
#include "coop/link_auth.h"
#include "server_link.h"

namespace Region {

// A ticket holds this long: enough for a game to say hello to its new shard, a few times over.
constexpr qint64 TicketMs = 2 * 60 * 1000;
// A shard past this share of its capacity takes no hello another shard sends on.
constexpr double RoomShare = 0.9;
constexpr qsizetype MaxTicketBytes = 4096;

struct Shard {
    std::string name;
    ServerLink::Endpoint address; // its link's, as the games reach it
    LinkAuth::Key key{};
    int players = 0;
    int capacity = 0;
    bool draining = false;
};
// A player signed in on another shard: its lobby member name (the online id in hex) and its name.
struct Player {
    std::string shard;
    std::string member;
    std::string name;
};
// An invite another shard's player sent to one here (TeamLobby::InviteFromElsewhere).
struct Invite {
    std::string member;
    bbcoop::TeamInvite invite;
};

// The directory's last answer: a few seconds old.
struct View {
    std::vector<Shard> shards;                // the healthy ones, this one too
    std::vector<Player> players;              // on the other shards
    std::map<std::string, std::string> codes; // another shard's team code -> that shard
    std::map<std::string, std::string> held;  // a world this shard lists -> the shard holding it
    const Shard* Find(const std::string& name) const;
};

// The directory's answer to a heartbeat (server/directory/directory.py); its inbox into `inbox`.
View ViewOf(const QJsonObject& answer, std::vector<Invite>* inbox = nullptr);
// "a.b.c.d:port", as the directory and SHARD_PUBLIC_ADDRESS write it.
std::optional<ServerLink::Endpoint> EndpointOf(const QString& text);

// The tickets' key: the region's secret, or one derived from the directory's token (every shard
// holds it) when the secret is empty.
LinkAuth::HmacKey TicketKey(const QByteArray& region_secret, const QByteArray& directory_token);
bbcoop::RegionTicket TicketFor(const QByteArray& online_id, const QByteArray& secret_sha256,
                               const std::string& shard, qint64 now_ms,
                               const std::vector<bbcoop::TeamRequest>& requests);
// The ticket's bytes and their HMAC (32 bytes) after them.
QByteArray SignTicket(const LinkAuth::HmacKey& key, const bbcoop::RegionTicket& ticket);
// A ticket a hello carries, when it is this region's, for this shard, this player and its secret,
// and not expired.
std::optional<bbcoop::RegionTicket> OpenTicket(const LinkAuth::HmacKey& key,
                                               const QByteArray& ticket,
                                               const QByteArray& online_id,
                                               const QByteArray& secret, const std::string& shard,
                                               qint64 now_ms);

// Rule (c): the shard a hello goes to instead of this one (`self`, with `players` of `capacity`),
// or nothing. A full or draining shard sends it to the healthy shard least loaded by its share of
// its own capacity, among those under RoomShare and not draining.
const Shard* PlaceHello(const View& view, const std::string& self, int players, int capacity,
                        bool draining);

} // namespace Region
