// SPDX-FileCopyrightText: Copyright 2026 shadNet Project
// SPDX-License-Identifier: GPL-2.0-or-later
// Who may come in on a hosted server (docs/HOSTED_SERVER.md): shadNet on the internet for players
// who do not know each other, not a host's PC for its friends. Without sockets or clocks, like the
// hub: the link server asks it about every datagram and hello, and it says yes or no.
//
// - Hellos from one address: a token bucket (a game says hello once a second until welcomed; a
//   flood of new ids would hash a password each on the sign-in thread).
// - Links from one address: a household behind one router has a few games, never dozens.
// - New accounts from one address an hour (accounts are made at the first sign-in).
// - Each link's datagrams and bytes a second: a token bucket; what goes past it is dropped, and a
//   link past it again and again is reported for the server's log (Kaniel 2026-11-06: logged,
//   never banned: a 15-minute ban froze a player's game after a duel, with nothing in the log).
// - Bans by address or player id, from the operator (banned.txt) only.
#pragma once

#include <algorithm>
#include <QByteArray>
#include <QHash>
#include <QSet>
#include <QString>

namespace Admission {

struct Limits {
    double hellos_per_second = 4.0; // per address
    double hello_burst = 20.0;
    int links_per_address = 8;
    int new_accounts_per_hour = 5;        // per address
    double datagrams_per_second = 1500.0; // per link (a game sends ~60-200)
    double datagram_burst = 3000.0;
    double bytes_per_second = 400'000.0; // per link (a game sends ~10 KB/s)
    double byte_burst = 1'000'000.0;
    int strikes_to_ban = 5; // seconds a link went past its bucket, within strike_window_ms
    qint64 strike_window_ms = 60'000;
};

class Guard {
public:
    explicit Guard(Limits limits = {}) : limits{limits} {}

    // A hello from `address` (a link not known yet, or one saying hello again).
    bool AllowHello(quint32 address, qint64 now_ms);
    // Whether `address`, with `links` games linked already, may link one more.
    bool AllowLink(quint32 address, int links) const {
        return links < limits.links_per_address;
    }
    // Whether `address` may make one more account now; Made counts one.
    bool MayMakeAccount(quint32 address, qint64 now_ms);
    void Made(quint32 address, qint64 now_ms);
    // A sign-in that counted as one (Made when it was queued) made none after all.
    void Unmade(quint32 address);
    // A datagram of `bytes` on the link `link` (Endpoint::Key) from `address`: whether it goes on.
    bool AllowDatagram(quint64 link, quint32 address, int bytes, qint64 now_ms);
    void Forget(quint64 link) {
        links.remove(link);
    }

    bool Banned(quint32 address) const;
    bool BannedId(const QByteArray& online_id) const {
        return banned_ids.contains(online_id);
    }
    // In the operator's list (the only bans there are).
    bool ListBanned(quint32 address) const {
        return banned_addresses.contains(address);
    }
    // A link that went past its rate strikes_to_ban times within strike_window_ms: who, and how
    // many of its datagrams were dropped meanwhile.
    struct Flood {
        quint32 address = 0;
        quint64 link = 0; // Endpoint::Key
        quint64 dropped = 0;
    };
    // The floods since the last call (for the server's log).
    QList<Flood> TakeFloods() {
        QList<Flood> taken;
        taken.swap(floods);
        return taken;
    }
    // The operator's list: one address (a.b.c.d) or player id a line, '#' comments.
    void SetBanned(const QSet<quint32>& addresses, const QSet<QByteArray>& ids);

    struct Counters {
        quint64 hellos_refused = 0;
        quint64 datagrams_dropped = 0;
        quint64 accounts_refused = 0;
        quint64 floods = 0;
    };
    const Counters& Count() const {
        return counters;
    }
    const Limits& Settings() const {
        return limits;
    }

private:
    struct Bucket {
        double tokens = -1; // full at the first use
        qint64 at_ms = 0;
        bool Take(double amount, double rate, double burst, qint64 now_ms);
    };
    struct LinkState {
        Bucket datagrams;
        Bucket bytes;
        qint64 last_strike_ms = 0;
        int strikes = 0;
        qint64 strikes_from_ms = 0;
        quint64 dropped = 0; // in the strikes' window
    };

    Limits limits;
    QHash<quint32, Bucket> hellos;
    QHash<quint32, QList<qint64>> accounts; // when each address made its accounts, the last hour
    QHash<quint64, LinkState> links;
    QList<Flood> floods; // not taken yet (TakeFloods)
    QSet<quint32> banned_addresses;
    QSet<QByteArray> banned_ids;
    Counters counters;
};

// Hellos that open an address may send each second, on every server: each costs the link thread
// Diffie-Hellmans (link version 4), so a flood from one real address would starve the others.
// Over it a hello is dropped (the game says hello again); nothing is banned.
class HelloBudget {
public:
    explicit HelloBudget(int per_second = 20) : per_second{per_second} {}

    bool Allow(quint32 address, qint64 now_ms) {
        const qint64 now = now_ms / 1000;
        if (now != second) {
            second = now;
            counts.clear();
        }
        return ++counts[address] <= per_second;
    }

private:
    int per_second;
    qint64 second = -1;
    QHash<quint32, int> counts;
};

// The zero bits a new account's work needs (coop/link_auth.h): the base, and 2 more for every
// doubling of the accounts made in the last minute over a baseline (a botnet of many addresses
// stays within each address's limit), never more than the cap.
// New accounts a minute at the base work: a launch rush is real players (60 had 300 joins pay
// twice the bits); past it the work still rises, up to its maximum.
constexpr int DefaultSignupsPerMinute = 300;

// The link's socket buffers (link_server.cpp), and whether the system kept them far smaller
// (net.core.rmem_max): a burst past a small one is lost before the link thread sees it.
constexpr int LinkSocketBytes = 16 << 20;
inline bool LinkBufferShort(int got_bytes) {
    return got_bytes < LinkSocketBytes;
}

class SignupWork {
public:
    SignupWork(int base_bits = 20, int max_bits = 20,
               int baseline_per_minute = DefaultSignupsPerMinute)
        : base_bits{base_bits}, max_bits{std::max(base_bits, max_bits)},
          baseline{std::max(1, baseline_per_minute)} {}

    void Made(qint64 now_ms);
    int Bits(qint64 now_ms);

private:
    static constexpr int Slots = 60; // one a second
    void Advance(qint64 now_ms);

    int base_bits;
    int max_bits;
    int baseline;
    int counts[Slots] = {};
    qint64 second = 0; // the newest slot's
    int total = 0;
};

// One address or id a line of an operator's ban list (banned.txt), '#' comments; a line may end
// its ban: "<address|id> until=<unix seconds>", left out from `now_s` on.
void ParseBanList(const QString& text, QSet<quint32>& addresses, QSet<QByteArray>& ids,
                  qint64 now_s = 0);

struct BanLine {
    QString who;        // an address or a player id
    qint64 until_s = 0; // 0: for ever
    QString note;       // its comment
};
QList<BanLine> BanLines(const QString& text);
// The list with `ban` (its earlier lines and the ended bans out, everything else kept as it was).
QString WithBan(const QString& text, const BanLine& ban, qint64 now_s);
// The list without the lines of `who`; `found` whether it had any.
QString WithoutBan(const QString& text, const QString& who, bool& found);

} // namespace Admission
