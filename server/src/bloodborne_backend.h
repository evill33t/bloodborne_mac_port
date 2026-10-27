// SPDX-FileCopyrightText: Copyright 2026 shadNet Project
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <memory>
#include <optional>
#include <set>
#include <utility>
#include <vector>

#include <QByteArray>
#include <QHash>
#include <QList>
#include <QSet>
#include <QString>

#include "bloodborne_backend_json.h"

namespace Bloodborne::Backend {

// The game's own online services (login, the online check, messages, bloodstains, wandering
// ghosts, lantern counts, chalice dungeons), which retail sends to Sony's ss4.scej-network.jp as
// JSON over HTTP. backend/bb_dummy_server.py answered them first; this answers them the same way
// from inside shadNet, so a host runs one program. backend/tests/test_parity.py sends both the
// same requests and checks the answers match.

// How often (seconds) the game re-posts its summon sign and polls for signs, and the co-op guard
// timers after a session, in the ss.info the game reads (retail 30/35/60).
inline constexpr int DefaultSummonPoll = 5;

// What players may see of their own content: "deaths", "ghosts", "lanterns" (messages always).
inline const QStringList ShowOwnChoices = {QStringLiteral("deaths"), QStringLiteral("ghosts"),
                                           QStringLiteral("lanterns")};
// "deaths,ghosts", "all" or "": the set, or nullopt with an unknown name.
std::optional<QSet<QString>> ParseShowOwn(const QString& value);

struct BackendOptions {
    // The SQLite file that keeps players' content; empty: none, the world stays empty.
    QString storePath;
    QSet<QString> showOwn;
    qint64 ghostTtlSeconds = 30 * 60;
    qint64 lanternTtlSeconds = 10 * 60;
    int tombCap = 100;
    // Text files read on each request (missing: empty).
    QString noticePath;       // the sign-in notice: first line the title, the rest the body
    QString broadcastPath;    // a popup each player gets once per distinct text
    QString loginMessagePath; // the login warning message
    int summonPoll = DefaultSummonPoll;
    QByteArray ssInfoTemplate; // backend/ss_info_template.xml
};

// ss.info: the template with {SERVICES} and {SUMMON} filled in and the summon timers set, base64.
// An error text when the template lacks a placeholder or still names the community server.
struct SsInfo {
    QByteArray body;
    QString error;
};
SsInfo BuildSsInfo(const QByteArray& templ, const QString& servicesUrl, const QString& summonUrl,
                   int summonPoll);

struct BackendRequest {
    QByteArray method; // "GET", "POST"
    QString path;      // without the query
    QByteArray body;
    QString servicesUrl; // "http://<address>:<port>", this server as the game reaches it
    QString summonUrl;   // shadNet's WebAPI, "http://<address>:31315"
};

struct BackendReply {
    int status = 200;
    QByteArray contentType;
    QByteArray body;
    QList<std::pair<QByteArray, QByteArray>> headers;
};

// Who a request acts as (http_gate.h): a Login the account it names, every other service the
// UserId in its body; neither for a GET (ss.info, status) or a body that names none.
struct BackendIdentity {
    std::optional<QString> account;
    std::optional<qint64> userId;
};
BackendIdentity IdentityOf(const BackendRequest& request);

class ContentStore;

class BloodborneBackend {
public:
    BloodborneBackend();
    ~BloodborneBackend();

    // Checks the template and opens the content store. False with `error` set when either fails.
    bool Open(const BackendOptions& options, QString* error);
    BackendReply Handle(const BackendRequest& request);

    bool HasStore() const {
        return m_store != nullptr;
    }
    // users, blood_messages, tombstones, ghosts, chalice_dungeons: rows in each.
    Json Counts() const;
    // The UserId Login gives `account`; nothing while the store has none for it.
    std::optional<qint64> UserIdOf(const QString& account);

private:
    Json Service(const QString& path, const Json& request);
    Json Login(const Json& request);

    BackendOptions m_options;
    std::unique_ptr<ContentStore> m_store;
    // Without a store, chalice dungeons are kept in memory; m_chaliceStore is the one in use.
    std::unique_ptr<ContentStore> m_memoryStore;
    ContentStore* m_chaliceStore = nullptr;
    // Standard chalice dungeons a ritual asked for (keys), oldest first: Chalice Dungeon Search
    // lists them.
    std::vector<qint64> m_standardUsed;
    qint64 m_nextCreatedId = 1000;
    QSet<QString> m_unknownSeen;
    // (UserId, broadcast text's CRC-32) already delivered; in memory on purpose, so a restart
    // sends a waiting broadcast again.
    std::set<std::pair<qint64, quint32>> m_broadcastSent;
    QHash<QString, qint64> m_userIds; // lower-cased account -> its UserId, as Login gave it
};

} // namespace Bloodborne::Backend
