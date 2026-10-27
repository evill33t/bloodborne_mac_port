// SPDX-FileCopyrightText: Copyright 2026 shadNet Project
// SPDX-License-Identifier: GPL-2.0-or-later
#include "http_gate.h"

#include <QMutexLocker>

namespace HttpGate {

Verdict Decide(const QStringList& linked, const std::optional<QString>& as) {
    if (linked.isEmpty())
        return Verdict::NoLink;
    if (!as)
        return Verdict::Allowed;
    // Accounts are one whatever the case (the sign-in and the backend's store ignore it).
    if (!as->isEmpty() && linked.contains(*as, Qt::CaseInsensitive))
        return Verdict::Allowed;
    return Verdict::NotLinkedHere;
}

std::optional<quint32> Ipv4Of(const QHostAddress& address) {
    bool ok = false;
    const quint32 v4 = address.toIPv4Address(&ok);
    if (!ok)
        return std::nullopt;
    return v4;
}

void Gate::SetEnforced(bool on) {
    QMutexLocker lock(&mutex);
    enforced = on;
}

bool Gate::Enforced() const {
    QMutexLocker lock(&mutex);
    return enforced;
}

void Gate::Linked(quint64 link, quint32 address, const QString& online_id) {
    QMutexLocker lock(&mutex);
    links.insert(link, Link{address, online_id});
}

void Gate::Gone(quint64 link) {
    QMutexLocker lock(&mutex);
    links.remove(link);
}

QStringList Gate::At(quint32 address) const {
    QMutexLocker lock(&mutex);
    QStringList out;
    for (const Link& link : links) {
        if (link.address == address && !out.contains(link.online_id))
            out.append(link.online_id);
    }
    return out;
}

void Gate::SetUserIdOf(UserIdOf user_id_of) {
    QMutexLocker lock(&mutex);
    this->user_id_of = std::move(user_id_of);
}

Verdict Gate::ForPlayer(quint32 address, const std::optional<QString>& online_id) const {
    if (!Enforced())
        return Verdict::Allowed;
    return Decide(At(address), online_id);
}

Verdict Gate::ForUserId(quint32 address, std::optional<qint64> user_id) const {
    UserIdOf user_id_of_copy;
    {
        QMutexLocker lock(&mutex);
        if (!enforced)
            return Verdict::Allowed;
        user_id_of_copy = user_id_of;
    }
    const QStringList linked = At(address);
    if (!user_id || !user_id_of_copy)
        return Decide(linked, std::nullopt);
    // The account linked here that has this UserId (asked outside the lock: the backend reads its
    // store); none, and the request acts as a player not linked here.
    std::optional<QString> as = QString();
    for (const QString& account : linked) {
        if (user_id_of_copy(account) == *user_id) {
            as = account;
            break;
        }
    }
    return Decide(linked, as);
}

} // namespace HttpGate
