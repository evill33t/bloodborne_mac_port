// SPDX-FileCopyrightText: Copyright 2026 shadNet Project
// SPDX-License-Identifier: GPL-2.0-or-later
#include "admission.h"

#include <algorithm>
#include <optional>
#include <QDateTime>
#include <QHostAddress>
#include <QRegularExpression>
#include <QStringList>
#include <QTimeZone>

namespace Admission {

bool Guard::Bucket::Take(double amount, double rate, double burst, qint64 now_ms) {
    if (tokens < 0) {
        tokens = burst;
    } else {
        tokens = std::min(burst, tokens + (now_ms - at_ms) * rate / 1000.0);
    }
    at_ms = now_ms;
    if (tokens < amount) {
        return false;
    }
    tokens -= amount;
    return true;
}

bool Guard::AllowHello(quint32 address, qint64 now_ms) {
    if (hellos.size() > 100'000) {
        hellos.clear(); // a spoofed flood of addresses: start over rather than grow
    }
    if (!hellos[address].Take(1.0, limits.hellos_per_second, limits.hello_burst, now_ms)) {
        ++counters.hellos_refused;
        return false;
    }
    return true;
}

bool Guard::MayMakeAccount(quint32 address, qint64 now_ms) {
    QList<qint64>& made = accounts[address];
    made.erase(std::remove_if(made.begin(), made.end(),
                              [&](qint64 at) { return now_ms - at > 3'600'000; }),
               made.end());
    if (made.size() >= limits.new_accounts_per_hour) {
        ++counters.accounts_refused;
        return false;
    }
    return true;
}

void Guard::Made(quint32 address, qint64 now_ms) {
    accounts[address].append(now_ms);
}

void Guard::Unmade(quint32 address) {
    const auto found = accounts.find(address);
    if (found != accounts.end() && !found->isEmpty()) {
        found->removeLast();
    }
}

bool Guard::AllowDatagram(quint64 link, quint32 address, int bytes, qint64 now_ms) {
    LinkState& state = links[link];
    if (state.datagrams.Take(1.0, limits.datagrams_per_second, limits.datagram_burst, now_ms) &&
        state.bytes.Take(bytes, limits.bytes_per_second, limits.byte_burst, now_ms)) {
        return true;
    }
    ++counters.datagrams_dropped;
    // One strike a second at most: a link past its rate for strikes_to_ban seconds within the
    // window is banned.
    if (now_ms - state.last_strike_ms >= 1000) {
        state.last_strike_ms = now_ms;
        if (now_ms - state.strikes_from_ms > limits.strike_window_ms) {
            state.strikes_from_ms = now_ms;
            state.strikes = 0;
        }
        if (++state.strikes >= limits.strikes_to_ban) {
            Ban(address, now_ms + limits.ban_ms);
            ++counters.bans;
            state.strikes = 0;
        }
    }
    return false;
}

bool Guard::Banned(quint32 address, qint64 now_ms) const {
    if (banned_addresses.contains(address)) {
        return true;
    }
    const auto ban = bans.constFind(address);
    return ban != bans.constEnd() && *ban > now_ms;
}

void Guard::SetBanned(const QSet<quint32>& addresses, const QSet<QByteArray>& ids) {
    banned_addresses = addresses;
    banned_ids = ids;
}

namespace {

// A line's entry and its end (0: for ever), without its comment; nothing for a comment or blank.
std::optional<BanLine> ReadBanLine(const QString& raw) {
    const QString line = raw.section('#', 0, 0).trimmed();
    if (line.isEmpty()) {
        return std::nullopt;
    }
    const QStringList words = line.split(QRegularExpression(QStringLiteral("\\s+")));
    BanLine ban{words[0], 0, raw.section('#', 1).trimmed()};
    for (const QString& word : words.mid(1)) {
        if (word.startsWith(QStringLiteral("until="))) {
            ban.until_s = word.mid(6).toLongLong();
        }
    }
    // WithBan's own "until <date> UTC" before the note.
    static const QRegularExpression written{QStringLiteral("^until [0-9-]+ [0-9:]+ UTC(, )?")};
    if (ban.until_s != 0) {
        ban.note.remove(written);
    }
    return ban;
}

} // namespace

void ParseBanList(const QString& text, QSet<quint32>& addresses, QSet<QByteArray>& ids,
                  qint64 now_s) {
    for (const BanLine& ban : BanLines(text)) {
        if (ban.until_s != 0 && ban.until_s <= now_s) {
            continue;
        }
        bool ipv4 = false;
        const quint32 address = QHostAddress(ban.who).toIPv4Address(&ipv4);
        if (ipv4) {
            addresses.insert(address);
        } else {
            ids.insert(ban.who.toUtf8());
        }
    }
}

void SignupWork::Advance(qint64 now_ms) {
    const qint64 now = now_ms / 1000;
    if (now - second >= Slots) {
        std::fill(std::begin(counts), std::end(counts), 0);
        total = 0;
    } else {
        for (qint64 at = second + 1; at <= now; ++at) {
            total -= counts[at % Slots];
            counts[at % Slots] = 0;
        }
    }
    second = std::max(second, now);
}

void SignupWork::Made(qint64 now_ms) {
    Advance(now_ms);
    ++counts[second % Slots];
    ++total;
}

int SignupWork::Bits(qint64 now_ms) {
    Advance(now_ms);
    if (base_bits <= 0) {
        return 0;
    }
    int bits = base_bits;
    for (qint64 over = baseline * 2; total >= over && bits < max_bits; over *= 2) {
        bits += 2;
    }
    return std::min(bits, max_bits);
}

QList<BanLine> BanLines(const QString& text) {
    QList<BanLine> bans;
    for (const QString& line : text.split('\n')) {
        if (auto ban = ReadBanLine(line)) {
            bans.append(*ban);
        }
    }
    return bans;
}

QString WithBan(const QString& text, const BanLine& ban, qint64 now_s) {
    QStringList lines;
    for (const QString& line : text.split('\n')) {
        const auto old = ReadBanLine(line);
        if (old && (old->who == ban.who || (old->until_s != 0 && old->until_s <= now_s))) {
            continue;
        }
        lines.append(line);
    }
    while (!lines.isEmpty() && lines.last().trimmed().isEmpty()) {
        lines.removeLast();
    }
    QString added = ban.who;
    QStringList comment;
    if (ban.until_s != 0) {
        added += QStringLiteral(" until=%1").arg(ban.until_s);
        comment.append(QDateTime::fromSecsSinceEpoch(ban.until_s, QTimeZone::utc())
                           .toString(QStringLiteral("'until' yyyy-MM-dd HH:mm 'UTC'")));
    }
    if (!ban.note.isEmpty()) {
        comment.append(QString{ban.note}.replace('\n', ' '));
    }
    if (!comment.isEmpty()) {
        added += QStringLiteral(" # ") + comment.join(QStringLiteral(", "));
    }
    lines.append(added);
    return lines.join('\n') + '\n';
}

QString WithoutBan(const QString& text, const QString& who, bool& found) {
    found = false;
    QStringList lines;
    for (const QString& line : text.split('\n')) {
        if (const auto old = ReadBanLine(line); old && old->who == who) {
            found = true;
            continue;
        }
        lines.append(line);
    }
    return lines.join('\n');
}

} // namespace Admission
