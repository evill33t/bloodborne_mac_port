// SPDX-FileCopyrightText: Copyright 2026 shadNet Project
// SPDX-License-Identifier: GPL-2.0-or-later
#include <algorithm>
#include <memory>
#include <optional>

#include <QCryptographicHash>
#include <QFile>
#include <QRandomGenerator>
#include <QRegularExpression>
#include <QStringDecoder>

#ifndef MONOCYPHER_CPP_NAMESPACE
#define MONOCYPHER_CPP_NAMESPACE LinkMonocypher // compiled once, coop/link_crypto.cpp
#endif
#include "coop/monocypher/monocypher.h"
#include "password_accounts.h"
#include "signed_accounts.h"

namespace PasswordAccounts {

namespace {

using namespace LinkMonocypher;

constexpr int SaltBytes = 16;
constexpr int HashBytes = 32;
constexpr quint32 MaxPasses = 16;
const auto Base64 = QByteArray::Base64Encoding | QByteArray::OmitTrailingEquals;

struct Stored {
    quint32 memory_kib = 0;
    quint32 passes = 0;
    QByteArray salt;
    QByteArray hash;
};

std::optional<Stored> Read(const QString& encoded) {
    static const QRegularExpression shape{QStringLiteral(
        R"(^\$argon2id\$v=19\$m=(\d{1,8}),t=(\d{1,2}),p=1\$([A-Za-z0-9+/]{11,88})\$([A-Za-z0-9+/]{22,88})$)")};
    const QRegularExpressionMatch match = shape.match(encoded);
    if (!match.hasMatch()) {
        return std::nullopt;
    }
    Stored stored;
    stored.memory_kib = match.captured(1).toUInt();
    stored.passes = match.captured(2).toUInt();
    stored.salt = QByteArray::fromBase64(match.captured(3).toLatin1(), Base64);
    stored.hash = QByteArray::fromBase64(match.captured(4).toLatin1(), Base64);
    if (stored.memory_kib < 8 || stored.memory_kib > MaxMemoryKib || stored.passes < 1 ||
        stored.passes > MaxPasses || stored.salt.size() < 8 || stored.hash.size() < 16) {
        return std::nullopt;
    }
    return stored;
}

QByteArray Argon2id(const QByteArray& password, const QByteArray& salt, quint32 memory_kib,
                    quint32 passes, int size) {
    QByteArray out(size, '\0');
    const auto work = std::make_unique_for_overwrite<quint8[]>(std::size_t{memory_kib} * 1024);
    const crypto_argon2_config config{CRYPTO_ARGON2_ID, memory_kib, passes, 1};
    const crypto_argon2_inputs inputs{reinterpret_cast<const quint8*>(password.constData()),
                                      reinterpret_cast<const quint8*>(salt.constData()),
                                      static_cast<quint32>(password.size()),
                                      static_cast<quint32>(salt.size())};
    crypto_argon2(reinterpret_cast<quint8*>(out.data()), static_cast<quint32>(size), work.get(),
                  config, inputs, crypto_argon2_no_extras);
    crypto_wipe(work.get(), std::size_t{memory_kib} * 1024);
    return out;
}

// The password's bytes, wiped when done with.
struct Bytes {
    explicit Bytes(const QString& text) : bytes{text.toUtf8()} {}
    ~Bytes() {
        crypto_wipe(bytes.data(), static_cast<std::size_t>(bytes.size()));
    }
    QByteArray bytes;
};

bool Same(const QByteArray& a, const QByteArray& b) {
    if (a.size() != b.size()) {
        return false;
    }
    quint8 differs = 0;
    for (qsizetype i = 0; i < a.size(); ++i) {
        differs |= static_cast<quint8>(a[i] ^ b[i]);
    }
    return differs == 0;
}

bool IsHex(QStringView text) {
    return std::all_of(text.begin(), text.end(), [](QChar c) {
        return (c >= u'0' && c <= u'9') || (c >= u'a' && c <= u'f');
    });
}

// Common passwords of 10 or more characters (the shorter are refused as short anyway), for a
// server without the shipped list.
constexpr const char* BuiltIn[] = {
    "password123",   "password1234",  "password12",    "password!1",   "password1!",
    "passw0rd123",   "1234567890",    "12345678910",   "0987654321",   "9876543210",
    "1111111111",    "0000000000",    "1212121212",    "1234512345",   "5555555555",
    "123123123123",  "123456789a",    "a123456789",    "abc1234567",   "qwertyuiop",
    "qwertyuiop123", "qwerty1234",    "qwerty123456",  "1q2w3e4r5t",   "1qaz2wsx3edc",
    "zaq12wsxcde3",  "123qweasdzxc",  "asdfghjkl1",    "zxcvbnm123",   "iloveyou123",
    "princess123",   "football123",   "baseball123",   "sunshine123",  "welcome123",
    "letmein123",    "monkey1234",    "dragon1234",    "trustno1234",  "superman123",
    "starwars123",   "administrator", "bloodborne",    "bloodborne1",  "bloodborne123",
    "bloodborne!",   "yharnam123",    "yharnamhunter", "fromsoftware", "darksouls123",
};

} // namespace

QString Hash(const QString& password, const HashParams& params) {
    QByteArray salt(SaltBytes, '\0');
    QRandomGenerator::system()->fillRange(reinterpret_cast<quint32*>(salt.data()), SaltBytes / 4);
    const Bytes plain{password};
    const QByteArray hash =
        Argon2id(plain.bytes, salt, params.memory_kib, params.passes, HashBytes);
    return QStringLiteral("$argon2id$v=19$m=%1,t=%2,p=1$%3$%4")
        .arg(params.memory_kib)
        .arg(params.passes)
        .arg(QString::fromLatin1(salt.toBase64(Base64)),
             QString::fromLatin1(hash.toBase64(Base64)));
}

bool Verify(const QString& password, const QString& encoded) {
    const auto stored = Read(encoded);
    if (!stored) {
        return false;
    }
    const Bytes plain{password};
    return Same(Argon2id(plain.bytes, stored->salt, stored->memory_kib, stored->passes,
                         static_cast<int>(stored->hash.size())),
                stored->hash);
}

bool NeedsRehash(const QString& encoded, const HashParams& params) {
    const auto stored = Read(encoded);
    return !stored || stored->memory_kib != params.memory_kib || stored->passes != params.passes ||
           stored->hash.size() != HashBytes;
}

NameProblem CheckName(const QString& name) {
    if (name.size() < NameMin || name.size() > NameMax) {
        return NameProblem::Length;
    }
    const bool fits = std::all_of(name.begin(), name.end(), [](QChar c) {
        return (c >= u'a' && c <= u'z') || (c >= u'A' && c <= u'Z') || (c >= u'0' && c <= u'9') ||
               c == u'_' || c == u'-' || c == u'.';
    });
    return fits ? NameProblem::None : NameProblem::Characters;
}

QString NameKey(const QString& name) {
    return name.toLower();
}

CommonPasswords::CommonPasswords() {
    for (const char* word : BuiltIn) {
        m_words.insert(QString::fromLatin1(word));
    }
}

bool CommonPasswords::LoadFile(const QString& path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return false;
    }
    while (!file.atEnd()) {
        const QByteArray line = file.readLine(4096);
        auto decode = QStringDecoder(QStringDecoder::Utf8, QStringDecoder::Flag::Stateless);
        const QString word = QString(decode(line)).trimmed();
        if (decode.hasError() || word.size() < PasswordMin || word.size() > PasswordMax) {
            continue; // the short ones are refused anyway
        }
        m_words.insert(word.toLower());
    }
    return true;
}

bool CommonPasswords::Contains(const QString& password) const {
    return !password.isEmpty() && m_words.contains(password.toLower());
}

PasswordProblem CheckPassword(const QString& password, const QString& name,
                              const CommonPasswords& common) {
    const qsizetype characters = password.toUcs4().size();
    if (characters < PasswordMin) {
        return PasswordProblem::TooShort;
    }
    if (characters > PasswordMax) {
        return PasswordProblem::TooLong;
    }
    if (name.size() >= NameMin && password.contains(name, Qt::CaseInsensitive)) {
        return PasswordProblem::HasName;
    }
    return common.Contains(password) ? PasswordProblem::Common : PasswordProblem::None;
}

QString OnlineId(const QByteArray& key, const QString& name_key, int salt) {
    return SignedAccounts::IdOf('u', key,
                                "name|" + name_key.toUtf8() + "|" + QByteArray::number(salt));
}

QString NewRecoveryCode() {
    static constexpr char Crockford[] = "0123456789ABCDEFGHJKMNPQRSTVWXYZ";
    QString code;
    for (int i = 0; i < 20; ++i) {
        if (i > 0 && i % 4 == 0) {
            code += u'-';
        }
        code += QChar::fromLatin1(Crockford[QRandomGenerator::system()->bounded(32)]);
    }
    return code;
}

QString RecoveryDigest(const QByteArray& key, const QString& typed) {
    QByteArray plain;
    for (const QChar c : typed.toUpper()) {
        if (c == u'-' || c.isSpace()) {
            continue;
        }
        plain += c == u'O' ? '0' : (c == u'I' || c == u'L') ? '1' : c.toLatin1();
    }
    if (plain.isEmpty()) {
        return {};
    }
    return QString::fromLatin1(SignedAccounts::Mac(key, "recovery|" + plain).toHex());
}

qint64 Throttle::WaitMs(const QString& key, qint64 now_ms) const {
    const auto found = m_tries.constFind(key);
    if (found == m_tries.constEnd() || now_ms - found->last_ms > ForgetMs) {
        return 0;
    }
    return std::max<qint64>(0, found->until_ms - now_ms);
}

void Throttle::Failed(const QString& key, qint64 now_ms) {
    if (m_tries.size() >= MaxKeys && !m_tries.contains(key)) {
        Forget(now_ms);
    }
    Tries& tries = m_tries[key];
    if (now_ms - tries.last_ms > ForgetMs) {
        tries = {};
    }
    ++tries.failed;
    tries.last_ms = now_ms;
    if (tries.failed >= m_free) {
        const int doublings = std::min(tries.failed - m_free, 20);
        tries.until_ms = now_ms + std::min(FirstWaitMs << doublings, m_maxWait);
    }
}

void Throttle::Succeeded(const QString& key) {
    m_tries.remove(key);
}

// Full: the forgotten first, then the ones that make nobody wait, then the oldest half.
void Throttle::Forget(qint64 now_ms) {
    m_tries.removeIf(
        [now_ms](const auto& item) { return now_ms - item.value().last_ms > ForgetMs; });
    if (m_tries.size() >= MaxKeys) {
        m_tries.removeIf([this](const auto& item) { return item.value().failed < m_free; });
    }
    if (m_tries.size() >= MaxKeys) {
        QList<qint64> times;
        for (const Tries& tries : std::as_const(m_tries)) {
            times.append(tries.last_ms);
        }
        std::nth_element(times.begin(), times.begin() + times.size() / 2, times.end());
        const qint64 middle = times[times.size() / 2];
        m_tries.removeIf([middle](const auto& item) { return item.value().last_ms <= middle; });
    }
}

namespace {

QByteArray ChallengeSignature(const QByteArray& key, const QByteArray& text) {
    return SignedAccounts::Mac(key, "challenge|" + text).toHex().left(32);
}

} // namespace

QString NewChallenge(const QByteArray& key, qint64 now_s) {
    QByteArray random(16, '\0');
    QRandomGenerator::system()->fillRange(reinterpret_cast<quint32*>(random.data()), 4);
    const QByteArray text =
        QByteArray::number(now_s, 16).rightJustified(8, '0').right(8) + random.toHex();
    return QString::fromLatin1(text + ChallengeSignature(key, text));
}

bool ChallengeFresh(const QByteArray& key, const QString& challenge, qint64 now_s) {
    if (key.isEmpty() || challenge.size() != 72 || !IsHex(challenge)) {
        return false;
    }
    const QByteArray text = challenge.left(40).toLatin1();
    if (!Same(ChallengeSignature(key, text), challenge.mid(40).toLatin1())) {
        return false;
    }
    const qint64 issued = text.left(8).toLongLong(nullptr, 16);
    return issued <= now_s + 30 && now_s - issued <= ChallengeSeconds;
}

bool WorkDone(const QString& challenge, const QString& name_key, quint64 nonce, int bits) {
    if (bits <= 0) {
        return true;
    }
    if (bits > 40) {
        return false;
    }
    QByteArray text = "bb signup v1|" + challenge.toLatin1() + "|" + name_key.toUtf8() + "|";
    for (int i = 0; i < 8; ++i) {
        text += static_cast<char>((nonce >> (8 * i)) & 0xFF);
    }
    const QByteArray digest = QCryptographicHash::hash(text, QCryptographicHash::Sha256);
    int zeros = 0;
    for (const char byte : digest) {
        const auto b = static_cast<quint8>(byte);
        if (b != 0) {
            for (quint8 mask = 0x80; (b & mask) == 0; mask >>= 1) {
                ++zeros;
            }
            break;
        }
        zeros += 8;
    }
    return zeros >= bits;
}

} // namespace PasswordAccounts
