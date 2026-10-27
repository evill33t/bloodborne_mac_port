// SPDX-FileCopyrightText: Copyright 2019-2026 rpcsn Project
// SPDX-FileCopyrightText: Copyright 2026 shadNet Project
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <QReadLocker>
#include <QReadWriteLock>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QWriteLocker>

// The game's own online services inside shadNet (bloodborne_backend_server.h).
struct BackendConfig {
    bool enabled = true;
    quint16 port = 20443;
    QString address;                               // empty: the address each game connects to
    QString folder = QStringLiteral("../backend"); // notice.txt, broadcast.txt, login_message.txt
    QString store;                                 // the content store's SQLite file; empty: none
    QString showOwn;                               // deaths, ghosts, lanterns or all
    int ghostTtlMinutes = 30;
    int lanternTtlMinutes = 10;
    int tombCap = 100;
    int summonPoll = 5;
};

// A hosted server (docs/HOSTED_SERVER.md): shadNet on the internet for players who do not know each
// other. Off (the default) it is a host's server for its friends, as before.
struct HostedConfig {
    bool enabled = false;
    QString build; // the build every game must say (empty: the first linked game's, as before)
    QString banList = QStringLiteral("banned.txt");
    int linksPerAddress = 8;
    double hellosPerSecond = 4.0;
    int newAccountsPerHour = 5;
    int signupWorkBits = 20;    // a new account's work (coop/link_auth.h): ~0.3 s of one core
    int signupWorkMaxBits = 20; // 2 bits more per doubling of the accounts a minute over:
    int signupsPerMinute = 60;  // this many
    double datagramsPerSecond = 1500.0;
    double bytesPerSecond = 400000.0;
    int lobbyPlayers = 32;     // players listed to each player besides its team and invites
    int lobbyIntervalMs = 250; // lobby states go out at most this often
    int worldsPerPlayer = 16;  // worlds listed to a player (the ones it played in)
    int partyWorlds = 2048;    // party worlds in memory at once (a team's each; idle ones go)
    bool stats = false;        // the stats page (it lists player ids; StatsEnabled as well)
    bool reports = false;      // crash reports (/bb/report), at most 8 MB each when on
    // The operator's commands (admin.h): a UNIX-domain socket only the server's user opens (empty:
    // none). Worlds nobody played for this many days are put aside, daily (0: never).
    QString adminSocket = QStringLiteral("admin.sock");
    int retireDays = 0;
};

// A region of several shards (docs/SCALING.md): this shard's place in it. Each setting from
// shadnet.cfg, or its environment variable (DIRECTORY_URL, ...), which wins: the deploy's.
struct ShardConfig {
    QString directoryUrl; // empty: no region (a lone server, as before)
    QString name;
    QString publicAddress;   // "a.b.c.d:port": the games reach this shard's link there
    QByteArray token;        // the shards' and the directory's shared secret
    QByteArray regionSecret; // the tickets' key (empty: derived from the token)
    int capacity = 800;
    bool draining = false;
    int heartbeatMs = 2000;
    bool Enabled() const {
        return !directoryUrl.isEmpty() && !name.isEmpty();
    }
};

// Invasions (docs/INVASIONS.md): a solo player invades a team's world that allows it. Off unless
// the operator turns it on (a hosted server or one at home alike).
struct InvasionConfig {
    bool enabled = false; // Invasions
    // InvasionKind / InvasionSign: what the invader's game joins the target's session with
    // (GameMan +0x16F0 / +0x16F4). Placeholders until the retail Sinister Resonant Bell capture
    // (docs/INVASIONS.md, phase 0) says the real values.
    quint32 kind = 0xFFFFFFFF;
    quint32 sign = 4;
};

class ConfigManager {
public:
    bool Load(const QString& path = "shadnet.cfg");
    void Reload(const QString& path = "shadnet.cfg");

    QString GetHost() const {
        QReadLocker lk(&m_lock);
        return m_host;
    }
    QString GetUnsecuredPort() const {
        QReadLocker lk(&m_lock);
        return m_unsecured_port;
    }
    QString GetMatchingUdpPort() const {
        QReadLocker lk(&m_lock);
        return m_matchingUdpPort;
    }
    QString GetWebApiPort() const {
        QReadLocker lk(&m_lock);
        return m_webapiPort;
    }
    bool IsStatsEnabled() const {
        QReadLocker lk(&m_lock);
        return m_statsEnabled;
    }
    QString GetStatsPort() const {
        QReadLocker lk(&m_lock);
        return m_statsPort;
    }
    // The link's static key (link version 4, coop/link_auth.h), made at the first start.
    QString GetLinkKeyFile() const {
        QReadLocker lk(&m_lock);
        return m_linkKeyFile;
    }
    // Where the parties' worlds are kept (bloodborne_world.h): one folder per party.
    QString GetBloodborneWorldPath() const {
        QReadLocker lk(&m_lock);
        return m_bloodborneWorldPath;
    }
    QString GetStatsPath() const {
        QReadLocker lk(&m_lock);
        return m_statsPath;
    }
    int GetStatsCacheLife() const {
        QReadLocker lk(&m_lock);
        return m_statsCacheLife;
    }
    bool IsMatching2Enabled() const {
        QReadLocker lk(&m_lock);
        return m_matching2Enabled;
    }
    bool IsBloodborneSeamlessCoopEnabled() const {
        QReadLocker lk(&m_lock);
        return m_bloodborneSeamlessCoop;
    }
    // Build check on join: whether a game must run this server's own release (CheckServerBuild,
    // on by default; off for a dev or rig server built apart from the games).
    bool IsServerBuildChecked() const {
        QReadLocker lk(&m_lock);
        return m_checkServerBuild;
    }

    ShardConfig GetShard() const {
        QReadLocker lk(&m_lock);
        return m_shard;
    }
    InvasionConfig GetInvasions() const {
        QReadLocker lk(&m_lock);
        return m_invasions;
    }
    HostedConfig GetHosted() const {
        QReadLocker lk(&m_lock);
        return m_hosted;
    }

    BackendConfig GetBackend() const {
        QReadLocker lk(&m_lock);
        return m_backend;
    }

    bool IsEmailValidated() const {
        QReadLocker lk(&m_lock);
        return m_emailValidated;
    }
    // Phase 11: a player id never seen before gets its account at its first sign-in (no
    // registering; the launcher makes the id).
    bool IsMakingAccountsAtSignIn() const {
        QReadLocker lk(&m_lock);
        return m_makeAccountsAtSignIn;
    }
    // Phase 11, one port: the TCP port listens on 127.0.0.1 only (the games' calls come through
    // the UDP link's Rpc tunnel). Off: on Host, for games that connect to it themselves.
    bool IsTcpLocalOnly() const {
        QReadLocker lk(&m_lock);
        return m_tcpLocalOnly;
    }
    // The HTTP ports answer only addresses with a signed-in link, as one of its players
    // (http_gate.h). Always on for a hosted server; this turns it on for a host's own.
    bool IsHttpLinkedOnly() const {
        QReadLocker lk(&m_lock);
        return m_httpLinkedOnly || m_hosted.enabled;
    }
    bool IsBannedDomain(const QString& d) const {
        QReadLocker lk(&m_lock);
        return m_bannedDomains.contains(d.toLower());
    }
    bool IsAdmin(const QString& npid) const {
        QReadLocker lk(&m_lock);
        return m_adminsList.contains(npid);
    }

    // Returns true if registration is allowed for the given secret_key.
    // When RegistrationSecretKey is empty, all registrations are allowed.
    // When set, only requests carrying the matching key are accepted.
    bool IsRegistrationAllowed(const QString& key) const {
        QReadLocker lk(&m_lock);
        return m_registrationSecretKey.isEmpty() ||
               (!key.isEmpty() && key == m_registrationSecretKey);
    }

    void SetHost(const QString& v) {
        QWriteLocker lk(&m_lock);
        m_host = v;
    }
    void SetUnsecuredPort(const QString& v) {
        QWriteLocker lk(&m_lock);
        m_unsecured_port = v;
    }
    void SetEmailValidated(bool v) {
        QWriteLocker lk(&m_lock);
        m_emailValidated = v;
    }
    void SetAdminsList(const QStringList& v) {
        QWriteLocker lk(&m_lock);
        m_adminsList = v;
    }
    void LoadBannedDomains();

private:
    void Parse(const QString& path);

    mutable QReadWriteLock m_lock;
    QString m_path;

    // config values
    QString m_host = "0.0.0.0";
    QString m_unsecured_port = "31313";
    QString m_matchingUdpPort = "31314";
    QString m_webapiPort = "31315";
    bool m_statsEnabled = true;
    bool m_matching2Enabled = true;
    bool m_bloodborneSeamlessCoop = false;
    bool m_checkServerBuild = true;
    BackendConfig m_backend;
    HostedConfig m_hosted;
    ShardConfig m_shard;
    InvasionConfig m_invasions;
    bool m_httpLinkedOnly = false;
    QString m_statsPort = "31320";
    QString m_statsPath = "stats";
    QString m_bloodborneWorldPath = "bloodborne_worlds";
    QString m_linkKeyFile = "link_key.bin";
    int m_statsCacheLife = 30; // seconds the stats JSON is cached before recompute
    bool m_emailValidated = false;
    bool m_makeAccountsAtSignIn = true;
    bool m_tcpLocalOnly = true;
    QStringList m_adminsList;
    QSet<QString> m_bannedDomains;
    // When non-empty, registrations must supply this key or they are rejected.
    QString m_registrationSecretKey;
};
