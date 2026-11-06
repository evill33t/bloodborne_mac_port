// SPDX-FileCopyrightText: Copyright 2026 shadNet Project
// SPDX-License-Identifier: GPL-2.0-or-later
// Global servers' own accounts (SignIn=password, the default; docs/HOSTED_SERVER.md, "Global
// servers: accounts"): a name and a password, made and checked by the accounts' home shard over
// HTTPS only (webapi_routes_password.cpp). A sign-in gives the launcher a signed 30-day
// secret (signed_accounts.h, ids "u"), and the password itself is never kept: only an
// Argon2id hash (Monocypher's), never logged, never on any other shard and never on the link.
#pragma once

#include <QByteArray>
#include <QHash>
#include <QSet>
#include <QString>
#include <QtGlobal>

namespace PasswordAccounts {

// Argon2id's cost: about 100-150 ms of one core of a 4-vCPU box (one lane: Monocypher's is one
// thread anyway). A stored hash made with others is made again at its next good sign-in.
struct HashParams {
    quint32 memory_kib = 64 * 1024;
    quint32 passes = 2;
};
constexpr quint32 MaxMemoryKib = 1024 * 1024; // a stored text never asks for more

// "$argon2id$v=19$m=<KiB>,t=<passes>,p=1$<salt>$<hash>" (the PHC string; base64 without padding).
QString Hash(const QString& password, const HashParams& params);
bool Verify(const QString& password, const QString& encoded);
bool NeedsRehash(const QString& encoded, const HashParams& params);

// Names: 3-20 of ASCII letters, digits, '_', '-' and '.', one account a name in any case.
constexpr int NameMin = 3;
constexpr int NameMax = 20;
enum class NameProblem { None, Length, Characters };
NameProblem CheckName(const QString& name);
QString NameKey(const QString& name);

// Passwords: 10-128 characters of anything; not a common one and not holding the name.
constexpr int PasswordMin = 10;
constexpr int PasswordMax = 128;

// The common and breached passwords refused, any case: a few built in, and the list shipped with
// the server (data/common-passwords.txt, one a line), read as plain data.
class CommonPasswords {
public:
    CommonPasswords();
    bool LoadFile(const QString& path);
    bool Contains(const QString& password) const;
    int Size() const {
        return static_cast<int>(m_words.size());
    }

private:
    QSet<QString> m_words;
};

enum class PasswordProblem { None, TooShort, TooLong, Common, HasName };
PasswordProblem CheckPassword(const QString& password, const QString& name,
                              const CommonPasswords& common);

// A name's online id: "u" and 14 hex of an HMAC of its key, a valid id for the game
// ([A-Za-z0-9_-]{3,16}). `salt`: the next try when one's virtual address is another account's.
QString OnlineId(const QByteArray& key, const QString& name_key, int salt = 0);

// The recovery code given at signup and at each recovery: 5 groups of 4 Crockford base32 (100
// bits), kept only as an HMAC under the accounts' key. A typed one is read in any case, without
// its dashes or spaces, and with O for 0 and I or L for 1. Empty for nothing typed.
QString NewRecoveryCode();
QString RecoveryDigest(const QByteArray& key, const QString& typed);

// Wrong passwords slow the next tries down, per account and per address: `free_tries` free, then
// a wait that doubles each time up to `max_wait_ms`. Never a lock: each wait ends, a good sign-in
// starts it over, and a day with no wrong one forgets it. An address is given more (a household, a
// carrier's NAT: AddressFreeTries).
class Throttle {
public:
    static constexpr int FreeTries = 5;
    static constexpr qint64 FirstWaitMs = 1'000;
    static constexpr qint64 MaxWaitMs = 15 * 60'000;
    static constexpr qint64 ForgetMs = 24 * 3600'000;
    static constexpr int MaxKeys = 200'000;
    static constexpr int AddressFreeTries = 30;
    static constexpr qint64 AddressMaxWaitMs = 5 * 60'000;

    explicit Throttle(int free_tries = FreeTries, qint64 max_wait_ms = MaxWaitMs)
        : m_free{free_tries}, m_maxWait{max_wait_ms} {}

    qint64 WaitMs(const QString& key, qint64 now_ms) const;
    void Failed(const QString& key, qint64 now_ms);
    void Succeeded(const QString& key);
    int Size() const {
        return static_cast<int>(m_tries.size());
    }

private:
    struct Tries {
        int failed = 0;
        qint64 last_ms = 0;
        qint64 until_ms = 0;
    };
    void Forget(qint64 now_ms);
    int m_free;
    qint64 m_maxWait;
    QHash<QString, Tries> m_tries;
};

// A signup's work, as the link hello's (coop/link_auth.h): the server's signed challenge (when,
// and a random part), then a nonce such that SHA-256("bb signup v1|" challenge "|" name key "|"
// the nonce as 8 bytes little-endian) starts with the server's number of zero bits.
constexpr qint64 ChallengeSeconds = 120;
QString NewChallenge(const QByteArray& key, qint64 now_s);
bool ChallengeFresh(const QByteArray& key, const QString& challenge, qint64 now_s);
bool WorkDone(const QString& challenge, const QString& name_key, quint64 nonce, int bits);

} // namespace PasswordAccounts
