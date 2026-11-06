// SPDX-FileCopyrightText: Copyright 2026 shadNet Project
// SPDX-License-Identifier: GPL-2.0-or-later
// Global servers' sign-ins (docs/HOSTED_SERVER.md, "Global servers: accounts"): an account made
// by the launcher's sign-in, a name and a password (password_accounts.h, ids "u"). Its secret is
// signed with a key only the servers hold, itself made from the link key every shard of a region
// shares, and says when it was issued, so any shard can tell it at a hello with no store shared
// between them. A secret holds SecretDays; each sign-in gives a new one, and a sign out, a password
// change or a recovery kills the older ones (Database::SignInNotBefore, sent to every shard of a
// region by the directory).
#pragma once

#include <span>

#include <QByteArray>
#include <QByteArrayList>
#include <QCryptographicHash>
#include <QMessageAuthenticationCode>
#include <QString>
#include <QtGlobal>

namespace SignedAccounts {

constexpr qsizetype IdHexChars = 14; // the kind's letter and these: a valid id, as the launcher's
constexpr qsizetype IssuedHexChars = 8;
constexpr qsizetype SecretHexChars = 32; // the issue time, then the signature
constexpr qint64 SecretDays = 30;
// Tries for a free virtual address (two ids can share one, which the accounts refuse).
constexpr int SaltTries = 16;

inline QByteArray Mac(const QByteArray& key, const QByteArray& text) {
    return QMessageAuthenticationCode::hash(text, key, QCryptographicHash::Sha256);
}

// The accounts' key from the link's secret key.
inline QByteArray KeyFrom(std::span<const quint8> link_secret) {
    return Mac(QByteArray(reinterpret_cast<const char*>(link_secret.data()),
                          static_cast<qsizetype>(link_secret.size())),
               "bloodborne accounts v1");
}

// `kind`'s id ('u': a name's) from `text`, under the accounts' key.
inline QString IdOf(char kind, const QByteArray& key, const QByteArray& text) {
    return QChar::fromLatin1(kind) + QString::fromLatin1(Mac(key, text).toHex().left(IdHexChars));
}

// The secret a sign-in at `issued_s` (unix seconds) gives `online_id`.
inline QString Secret(const QByteArray& key, const QString& online_id, qint64 issued_s) {
    const QByteArray issued =
        QByteArray::number(issued_s, 16).rightJustified(IssuedHexChars, '0').right(IssuedHexChars);
    const QByteArray signature = Mac(key, "secret|" + online_id.toUtf8() + "|" + issued).toHex();
    return QString::fromLatin1(issued + signature.left(SecretHexChars - IssuedHexChars));
}

// Whether `online_id` has the shape of a sign-in's id (no other account's secret is told here).
inline bool IsSignedId(const QByteArray& online_id) {
    if (online_id.size() != IdHexChars + 1 || online_id[0] != 'u') {
        return false;
    }
    for (qsizetype i = 1; i < online_id.size(); ++i) {
        const char c = online_id[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) {
            return false;
        }
    }
    return true;
}

enum class Verdict { Good, Forged, Expired, SignedOut };

// `secret` at a hello: signed for this id, not older than SecretDays, and not from before the
// account's last sign out (`not_before_s`).
inline Verdict Check(const QByteArray& key, const QByteArray& online_id, const QByteArray& secret,
                     qint64 now_s, qint64 not_before_s) {
    if (key.isEmpty() || !IsSignedId(online_id) || secret.size() != SecretHexChars) {
        return Verdict::Forged;
    }
    bool hex = false;
    const qint64 issued = secret.left(IssuedHexChars).toLongLong(&hex, 16);
    if (!hex) {
        return Verdict::Forged;
    }
    const QByteArray want = Secret(key, QString::fromLatin1(online_id), issued).toLatin1();
    quint8 differs = 0;
    for (qsizetype i = 0; i < want.size(); ++i) {
        differs |= static_cast<quint8>(want[i] ^ secret[i]);
    }
    if (differs != 0) {
        return Verdict::Forged;
    }
    if (issued > now_s + 300 || now_s - issued > SecretDays * 24 * 3600) {
        return Verdict::Expired;
    }
    return issued < not_before_s ? Verdict::SignedOut : Verdict::Good;
}

// The address a sign-in counts against: the connection's, or behind our own TLS proxy
// (SignInProxied) the one the proxy added last: anything before it the client typed itself.
inline QString ClientAddress(const QString& connection, const QByteArray& forwarded_for,
                             bool proxied) {
    if (!proxied || forwarded_for.trimmed().isEmpty()) {
        return connection;
    }
    return QString::fromLatin1(forwarded_for.split(',').constLast().trimmed());
}

} // namespace SignedAccounts
