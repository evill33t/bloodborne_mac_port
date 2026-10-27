// SPDX-FileCopyrightText: Copyright 2026 shadNet Project
// SPDX-License-Identifier: GPL-2.0-or-later
#include "region.h"

#include <QCryptographicHash>
#include <QHostAddress>
#include <QJsonArray>

namespace Region {

namespace {

std::span<const u8> Span(const QByteArray& bytes) {
    return {reinterpret_cast<const u8*>(bytes.constData()), static_cast<size_t>(bytes.size())};
}

std::string Text(const QJsonValue& value) {
    return value.toString().toStdString();
}

} // namespace

const Shard* View::Find(const std::string& name) const {
    for (const Shard& shard : shards) {
        if (shard.name == name) {
            return &shard;
        }
    }
    return nullptr;
}

std::optional<ServerLink::Endpoint> EndpointOf(const QString& text) {
    const qsizetype colon = text.lastIndexOf(':');
    if (colon <= 0) {
        return std::nullopt;
    }
    bool ipv4 = false;
    const quint32 address = QHostAddress(text.left(colon)).toIPv4Address(&ipv4);
    bool number = false;
    const uint port = text.mid(colon + 1).toUInt(&number);
    if (!ipv4 || !number || port == 0 || port > 0xFFFF) {
        return std::nullopt;
    }
    return ServerLink::Endpoint{address, static_cast<quint16>(port)};
}

View ViewOf(const QJsonObject& answer, std::vector<Invite>* inbox) {
    View view;
    for (const QJsonValue& value : answer.value("shards").toArray()) {
        const QJsonObject row = value.toObject();
        const auto address = EndpointOf(row.value("address").toString());
        const auto key = LinkAuth::KeyFromHex(Text(row.value("key")));
        if (!address || !key || Text(row.value("name")).empty()) {
            continue;
        }
        view.shards.push_back({Text(row.value("name")), *address, *key,
                               row.value("players").toInt(), row.value("capacity").toInt(),
                               row.value("draining").toBool()});
    }
    for (const QJsonValue& value : answer.value("players").toArray()) {
        const QJsonObject row = value.toObject();
        view.players.push_back(
            {Text(row.value("shard")), Text(row.value("member")), Text(row.value("name"))});
    }
    const QJsonObject codes = answer.value("codes").toObject();
    for (auto it = codes.begin(); it != codes.end(); ++it) {
        view.codes[it.key().toStdString()] = Text(it.value());
    }
    const QJsonObject held = answer.value("held").toObject();
    for (auto it = held.begin(); it != held.end(); ++it) {
        view.held[it.key().toStdString()] = Text(it.value());
    }
    if (inbox) {
        for (const QJsonValue& value : answer.value("inbox").toArray()) {
            const QJsonObject row = value.toObject();
            Invite invite;
            invite.member = Text(row.value("member"));
            invite.invite.set_team(Text(row.value("team")));
            invite.invite.set_from(Text(row.value("from")));
            invite.invite.set_from_name(Text(row.value("from_name")));
            if (!invite.member.empty() && invite.invite.team().starts_with('@')) {
                inbox->push_back(std::move(invite));
            }
        }
    }
    return view;
}

LinkAuth::HmacKey TicketKey(const QByteArray& region_secret, const QByteArray& directory_token) {
    if (!region_secret.isEmpty()) {
        return LinkAuth::HmacKey{Span(region_secret)};
    }
    // Another key than the token itself: a ticket is never a valid token, nor the other way.
    const QByteArray derived = QCryptographicHash::hash("shadnet region ticket\n" + directory_token,
                                                        QCryptographicHash::Sha256);
    return LinkAuth::HmacKey{Span(derived)};
}

bbcoop::RegionTicket TicketFor(const QByteArray& online_id, const QByteArray& secret_sha256,
                               const std::string& shard, qint64 now_ms,
                               const std::vector<bbcoop::TeamRequest>& requests) {
    bbcoop::RegionTicket ticket;
    ticket.set_online_id(online_id.toStdString());
    ticket.set_secret_sha256(secret_sha256.toStdString());
    ticket.set_shard(shard);
    ticket.set_expires_ms(now_ms + TicketMs);
    for (const bbcoop::TeamRequest& request : requests) {
        *ticket.add_requests() = request;
    }
    return ticket;
}

QByteArray SignTicket(const LinkAuth::HmacKey& key, const bbcoop::RegionTicket& ticket) {
    const QByteArray bytes = QByteArray::fromStdString(ticket.SerializeAsString());
    const LinkAuth::Digest mac = key.Mac(Span(bytes));
    return bytes + QByteArray(reinterpret_cast<const char*>(mac.data()), mac.size());
}

std::optional<bbcoop::RegionTicket> OpenTicket(const LinkAuth::HmacKey& key,
                                               const QByteArray& ticket,
                                               const QByteArray& online_id,
                                               const QByteArray& secret, const std::string& shard,
                                               qint64 now_ms) {
    constexpr qsizetype MacBytes = 32;
    if (ticket.size() <= MacBytes || ticket.size() > MaxTicketBytes) {
        return std::nullopt;
    }
    const QByteArray bytes = ticket.left(ticket.size() - MacBytes);
    const LinkAuth::Digest mac = key.Mac(Span(bytes));
    if (!LinkAuth::SameBytes(mac, Span(ticket.right(MacBytes)))) {
        return std::nullopt;
    }
    bbcoop::RegionTicket read;
    if (!read.ParseFromArray(bytes.constData(), static_cast<int>(bytes.size()))) {
        return std::nullopt;
    }
    const QByteArray hash = QCryptographicHash::hash(secret, QCryptographicHash::Sha256);
    if (read.online_id() != online_id.toStdString() || read.shard() != shard ||
        read.expires_ms() < now_ms ||
        !LinkAuth::SameBytes(Span(hash), Span(QByteArray::fromStdString(read.secret_sha256())))) {
        return std::nullopt;
    }
    return read;
}

const Shard* PlaceHello(const View& view, const std::string& self, int players, int capacity,
                        bool draining) {
    if (!draining && (capacity <= 0 || players < capacity)) {
        return nullptr;
    }
    const Shard* best = nullptr;
    double best_share = RoomShare;
    for (const Shard& shard : view.shards) {
        if (shard.name == self || shard.draining || shard.capacity <= 0) {
            continue;
        }
        const double share = static_cast<double>(shard.players) / shard.capacity;
        if (share < best_share) {
            best = &shard;
            best_share = share;
        }
    }
    return best;
}

} // namespace Region
