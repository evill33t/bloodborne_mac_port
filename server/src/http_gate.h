// SPDX-FileCopyrightText: Copyright 2026 shadNet Project
// SPDX-License-Identifier: GPL-2.0-or-later
// Who may use the HTTP ports (the game's services on BackendPort 20443, the summons and WebAPI on
// WebApiPort 31315). Their requests say who they act as (a UserId or PlatformAccountId in the
// body), so anyone who reaches the ports could act as any player. The games call them directly at
// the server's address (host_overrides.json, BB_SHADNET_WEBAPI), the address their link's UDP goes
// to, so a game's HTTP comes from its link's address. On a hosted server (or HttpLinkedOnly) a
// request is answered only from an address with a signed-in link, and only as one of the players
// linked from there (a household behind one router shares an address).
#pragma once

#include <functional>
#include <optional>

#include <QHash>
#include <QHostAddress>
#include <QMutex>
#include <QString>
#include <QStringList>

namespace HttpGate {

enum class Verdict {
    Allowed,
    NoLink,        // no signed-in link from the request's address
    NotLinkedHere, // the player it acts as is not linked from there
};

// The rule. `linked`: the players signed in on links from the request's address; `as`: the
// player the request acts as, nothing when it names none.
Verdict Decide(const QStringList& linked, const std::optional<QString>& as);

// "10.0.0.1" (or "::ffff:10.0.0.1") as the link keeps it; nothing for an IPv6 address.
std::optional<quint32> Ipv4Of(const QHostAddress& address);

// The links' players by address, kept by the link server, asked by the HTTP servers (all on the
// main thread today; locked anyway).
class Gate {
public:
    void SetEnforced(bool on);
    bool Enforced() const;

    // A link (ServerLink::Endpoint::Key) signed in as `online_id`, or that said hello again.
    void Linked(quint64 link, quint32 address, const QString& online_id);
    void Gone(quint64 link);
    QStringList At(quint32 address) const;

    // The backend's UserId of an account (the game's requests name that number), nothing when it
    // has none yet. Unset (the backend off) a UserId is not checked, only the address.
    using UserIdOf = std::function<std::optional<qint64>(const QString& online_id)>;
    void SetUserIdOf(UserIdOf user_id_of);

    // Allowed for everyone when not enforced.
    Verdict ForPlayer(quint32 address, const std::optional<QString>& online_id) const;
    Verdict ForUserId(quint32 address, std::optional<qint64> user_id) const;

private:
    struct Link {
        quint32 address = 0;
        QString online_id;
    };
    mutable QMutex mutex;
    bool enforced = false;
    QHash<quint64, Link> links;
    UserIdOf user_id_of;
};

} // namespace HttpGate
