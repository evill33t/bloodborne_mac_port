// SPDX-FileCopyrightText: Copyright 2026 shadNet Project
// SPDX-License-Identifier: GPL-2.0-or-later
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QJsonArray>
#include <QJsonDocument>
#include <QPromise>
#include <QRandomGenerator>
#include <QThreadPool>
#include <QtEndian>
#include "admin.h"
#include "bbcoop_revision.h"
#include "config.h"
#include "coop/build_fingerprint.h"
#include "link_server.h"

namespace {

// A new id while the region's directory does not answer (it cannot be claimed).
const QByteArray DirectoryDown = "the region's directory does not answer: try again in a minute";

// What a refused hello's game hears when its id got no account for a reason the player can act on.
QByteArray RefusalOf(DbError why) {
    if (why == DbError::ExistingAddress)
        return "address taken: another account's game has this id's virtual address; the launcher "
               "needs a new id";
    return {};
}

// This server's release: BB_RELEASE_VERSION, else the release's VERSION.txt (beside the server's
// folder, as the launcher reads it), else "" (a dev build).
QString ReleaseVersion() {
    const QString given = qEnvironmentVariable("BB_RELEASE_VERSION");
    if (!given.isEmpty()) {
        return given;
    }
    const QDir folder{QCoreApplication::applicationDirPath()};
    for (const QString& path : {folder.filePath(QStringLiteral("../VERSION.txt")),
                                folder.filePath(QStringLiteral("VERSION.txt"))}) {
        QFile file{path};
        if (file.open(QIODevice::ReadOnly)) {
            return QString::fromStdString(
                BloodborneCoop::ReleaseVersionOf(file.read(4096).toStdString()));
        }
    }
    return {};
}

} // namespace

LinkServer::LinkServer(SharedState* shared, const QString& db_path, quint16 tcp_port,
                       QObject* parent)
    : QObject(parent), m_socket(new QUdpSocket(this)), m_shared(shared),
      m_linkTimer(new QTimer(this)),
      m_world(shared->config ? shared->config->GetBloodborneWorldPath() : QString{}),
      m_worldTimer(new QTimer(this)), m_db(QStringLiteral("link_sign_in")), m_tcpPort(tcp_port) {
    if (!m_db.Open(db_path))
        qCritical() << "Link: the accounts did not open:" << m_db.lastError();
    // Link version 4: hellos are encrypted to the server's static key, made at the first start and
    // kept (the launcher pins its public half): one that does not read stops the link from
    // starting.
    if (const QString path =
            shared && shared->config ? shared->config->GetLinkKeyFile() : QString{};
        !path.isEmpty()) {
        bool made = false;
        if (const auto key = ServerLink::KeyFromFile(path, &made, &m_keyError)) {
            m_link.SetStaticKey(*key);
            if (made)
                qInfo().noquote() << "Link: a new key made in" << path
                                  << "(only this user reads it)";
        } else {
            m_keyError = path + ": " + m_keyError;
        }
    }
    qInfo().noquote() << "Link: the server's public key (the launcher's server key):"
                      << QString::fromStdString(LinkAuth::HexOf(m_link.PublicKey()));
    // Phase 11: the hello signs the player in (its id's account is made the first time).
    m_link.SetSignIn([this](const QByteArray& online_id, const QByteArray& secret, bool paid) {
        return SignIn(online_id, secret, paid);
    });
    // Build check on join: a game runs this server's release, or it is refused.
    const auto own = BloodborneCoop::EncodeBuildFingerprint(
        BloodborneCoop::ServerBuildFingerprint(ReleaseVersion().toStdString(), BB_SCM_REV));
    if (!shared->config || shared->config->IsServerBuildChecked()) {
        m_link.SetServerBuild(QByteArray::fromStdString(own));
        qInfo().noquote() << "Link: games must run this server's build:"
                          << QString::fromStdString(own);
    } else {
        qInfo().noquote() << "Link: this server's build (" << QString::fromStdString(own)
                          << ") is not checked (CheckServerBuild=false): the games here only";
    }
    connect(m_socket, &QUdpSocket::readyRead, this, &LinkServer::OnReadyRead);
    connect(m_linkTimer, &QTimer::timeout, this, &LinkServer::OnLinkTick);
    connect(m_worldTimer, &QTimer::timeout, this, &LinkServer::OnWorldTick);
    // Phase 7: the link relays only between games of one team (the party of their hello).
    m_link.SetPartyOf([this](const ServerLink::Endpoint& link) { return m_world.PartyOf(link); });
    // Invasions (docs/INVASIONS.md): and between an invader and the team whose world it visits.
    m_link.SetVisiting(
        [this](const ServerLink::Endpoint& link) { return m_world.VisitingOf(link); });
    // Phase 9: and forwards a game packet only to a teammate that needs it.
    m_link.SetPacketRoutes(
        [this](const ServerLink::Endpoint& from, const ServerLink::Endpoint& to, quint32 channel) {
            return m_world.RoutesPacket(from, to, channel);
        });
    // Hosted: a link's rates count its datagrams once their tag is good (a spoofed flood from a
    // player's address must not get that address banned).
    m_link.SetAdmit([this](const ServerLink::Endpoint& from, qsizetype bytes) {
        return !m_hosted.enabled ||
               m_guard.AllowDatagram(from.Key(), from.ipv4, static_cast<int>(bytes),
                                     m_clock.elapsed());
    });
    m_clock.start();
    // Hosted (docs/HOSTED_SERVER.md): limits on who comes in, a fixed build, passwords checked on
    // their own thread (a flood of new ids must not stop the games' relay).
    if (shared && shared->config)
        m_hosted = shared->config->GetHosted();
    std::vector<std::string> compatible_versions;
    for (const QString& version : m_hosted.compatibleVersions)
        compatible_versions.push_back(version.toStdString());
    m_link.SetCompatibleVersions(std::move(compatible_versions));
    if (!m_hosted.compatibleVersions.isEmpty())
        qInfo().noquote() << "Link: compatible release labels (co-op protocol still checked):"
                          << m_hosted.compatibleVersions.join(", ");
    if (m_hosted.enabled) {
        Admission::Limits limits;
        limits.links_per_address = m_hosted.linksPerAddress;
        limits.hellos_per_second = m_hosted.hellosPerSecond;
        limits.new_accounts_per_hour = m_hosted.newAccountsPerHour;
        limits.datagrams_per_second = m_hosted.datagramsPerSecond;
        limits.datagram_burst = m_hosted.datagramsPerSecond * 2;
        limits.bytes_per_second = m_hosted.bytesPerSecond;
        limits.byte_burst = m_hosted.bytesPerSecond * 2.5;
        m_guard = Admission::Guard{limits};
        m_link.SetRequiredBuild(m_hosted.build.toUtf8());
        // A new account's work (coop/link_auth.h): a botnet's many addresses each stay within
        // their accounts an hour, but each account costs it a CPU's while.
        m_signupWork = Admission::SignupWork{m_hosted.signupWorkBits, m_hosted.signupWorkMaxBits,
                                             m_hosted.signupsPerMinute};
        m_link.SetSignupWork(m_signupWork.Bits(m_clock.elapsed()));
        BloodborneWorld::HostedRules rules;
        rules.enabled = true;
        rules.lobby_players = static_cast<size_t>(std::max(0, m_hosted.lobbyPlayers));
        rules.lobby_interval_ms = m_hosted.lobbyIntervalMs;
        rules.worlds_per_player = static_cast<size_t>(std::max(1, m_hosted.worldsPerPlayer));
        m_world.SetHosted(rules);
        BloodborneWorld::Limits world_limits;
        world_limits.party_worlds = static_cast<size_t>(std::max(1, m_hosted.partyWorlds));
        m_world.SetLimits(world_limits);
        // The password hashes on every core but the link's.
        m_signInPool.setMaxThreadCount(std::max(1, QThread::idealThreadCount() - 1));
        m_signInPool.setExpiryTimeout(-1);
        LoadBanList();
        m_banTimer = new QTimer(this);
        connect(m_banTimer, &QTimer::timeout, this, &LinkServer::LoadBanList);
        m_banTimer->start(30'000);
        if (!m_hosted.adminSocket.isEmpty()) {
            // Never a network port: a socket file only the server's own user may open. A path, so
            // Qt keeps it where it is said (a bare name would go to the temp folder).
            const QString path = QFileInfo{m_hosted.adminSocket}.absoluteFilePath();
            m_admin = new QLocalServer(this);
            m_admin->setSocketOptions(QLocalServer::UserAccessOption);
            if (QString why; Admin::ListenAdmin(*m_admin, path, why)) {
                connect(m_admin, &QLocalServer::newConnection, this,
                        &LinkServer::OnAdminConnection);
                qInfo().noquote() << "Hosted: the operator's commands on" << path;
            } else {
                qWarning().noquote() << "Hosted: no admin socket at" << path << ":" << why;
            }
        }
        if (m_hosted.retireDays > 0) {
            // Daily, the first a minute after the start (the list is read by then).
            m_retireTimer = new QTimer(this);
            connect(m_retireTimer, &QTimer::timeout, this, &LinkServer::RetireWorlds);
            m_retireTimer->start(24 * 3600 * 1000);
            QTimer::singleShot(60'000, this, &LinkServer::RetireWorlds);
        }
        qInfo().noquote() << QStringLiteral(
                                 "Hosted: %1 links an address, %2 hellos/s, %3 new accounts/h, "
                                 "%4 datagrams/s and %5 bytes/s a link, build %6, new accounts' "
                                 "work %7 bits")
                                 .arg(limits.links_per_address)
                                 .arg(limits.hellos_per_second)
                                 .arg(limits.new_accounts_per_hour)
                                 .arg(limits.datagrams_per_second)
                                 .arg(limits.bytes_per_second)
                                 .arg(m_hosted.build.isEmpty() ? QStringLiteral("any")
                                                               : m_hosted.build)
                                 .arg(m_link.SignupWork());
    }
    if (shared && shared->config) {
        const InvasionConfig invasions = shared->config->GetInvasions();
        BloodborneWorld::InvasionRules rules;
        rules.enabled = invasions.enabled;
        rules.kind = invasions.kind;
        rules.sign = invasions.sign;
        m_world.SetInvasions(rules);
        if (rules.enabled)
            qInfo().noquote() << QStringLiteral("Invasions on: kind 0x%1, sign %2")
                                     .arg(rules.kind, 0, 16)
                                     .arg(rules.sign);
    }
    m_dbPath = db_path;
    if (shared && shared->config)
        m_shardConfig = shared->config->GetShard();
    if (m_shardConfig.Enabled())
        SetupRegion();
    m_linkTimer->start(500);
    // The worlds' clock is the wall clock: their journals keep it across restarts.
    Apply(m_world.Load(QDateTime::currentMSecsSinceEpoch()));
    m_worldTimer->start(50);
    // The worlds' writes are on their own thread: on disk before anything else goes down.
    if (QCoreApplication::instance())
        connect(QCoreApplication::instance(), &QCoreApplication::aboutToQuit, this, [this] {
            m_world.Drain();
            qInfo() << "Link: the worlds are on disk";
        });
}

LinkServer::~LinkServer() {
    m_signInPool.clear();
    m_signInPool.waitForDone();
}

void LinkServer::LoadBanList() {
    Admin::LoadBans(m_hosted.banList, QDateTime::currentSecsSinceEpoch(), m_guard);
    // A linked game banned now goes now (the guard checks ids at the hello only).
    Apply(Admin::KickBanned(m_link, m_guard, m_clock.elapsed()));
}

void LinkServer::OnAdminConnection() {
    while (QLocalSocket* socket = m_admin->nextPendingConnection()) {
        connect(socket, &QLocalSocket::disconnected, socket, &QObject::deleteLater);
        connect(socket, &QLocalSocket::readyRead, this, [this, socket] {
            // One command a connection, one line.
            if (!socket->canReadLine()) {
                if (socket->bytesAvailable() > 4096)
                    socket->abort();
                return;
            }
            const QString line = QString::fromUtf8(socket->readLine(4096)).trimmed();
            RunAdmin(socket, line);
        });
    }
}

void LinkServer::RunAdmin(QLocalSocket* socket, const QString& line) {
    Admin::Context context{m_link, m_world, m_guard};
    context.ban_list = m_hosted.banList;
    context.link_ms = m_clock.elapsed();
    context.wall_ms = QDateTime::currentMSecsSinceEpoch();
    context.retire_ms = qint64{std::max(0, m_hosted.retireDays)} * 24 * 3600 * 1000;
    context.link_load = m_linkLoad;
    const Admin::Answer answer = Admin::Run(line, context);
    qInfo().noquote() << "Admin:" << line << (answer.json["ok"].toBool() ? "done" : "refused");
    Apply(answer.link);
    Apply(answer.world);
    const auto reply = [socket](const QJsonObject& json) {
        socket->write(QJsonDocument{json}.toJson(QJsonDocument::Compact) + '\n');
        socket->disconnectFromServer();
    };
    if (!answer.finish) {
        reply(answer.json);
        return;
    }
    // What reads the disk runs on the pool, the answer comes back here (none if the operator's
    // connection went: the watcher goes with it).
    auto promise = std::make_shared<QPromise<QJsonObject>>();
    auto* watcher = new QFutureWatcher<QJsonObject>(socket);
    connect(watcher, &QFutureWatcherBase::finished, socket,
            [watcher, reply] { reply(watcher->result()); });
    watcher->setFuture(promise->future());
    QThreadPool::globalInstance()->start(
        [promise, finish = answer.finish, json = answer.json]() mutable {
            promise->start();
            finish(json);
            promise->addResult(json);
            promise->finish();
        });
}

void LinkServer::RetireWorlds() {
    BloodborneWorld::Service::Result result;
    const auto retired = m_world.Retire(QDateTime::currentMSecsSinceEpoch(),
                                        qint64{m_hosted.retireDays} * 24 * 3600 * 1000, result);
    Apply(result);
    qInfo().noquote() << QStringLiteral("Hosted: %1 worlds not played for %2 days put aside")
                             .arg(retired.size())
                             .arg(m_hosted.retireDays);
}

void LinkServer::Busy(const QElapsedTimer& since) {
    m_busyNs += since.nsecsElapsed();
    if (!m_loadClock.isValid()) {
        m_loadClock.start();
    } else if (const qint64 window = m_loadClock.nsecsElapsed(); window >= 10'000'000'000) {
        m_linkLoad = static_cast<double>(m_busyNs) / static_cast<double>(window);
        m_busyNs = 0;
        m_loadClock.restart();
    }
}

bool LinkServer::Start(const QHostAddress& addr, uint16_t port) {
    if (!m_keyError.isEmpty()) {
        qCritical().noquote() << "Link: no key, so no link:" << m_keyError;
        return false;
    }
    if (!m_socket->bind(addr, port)) {
        qCritical() << "Link UDP bind failed on" << addr.toString() << ":" << port
                    << m_socket->errorString();
        return false;
    }
    // Every game's datagrams come to this one socket: room for a burst while the thread is busy
    // (the system's cap, net.core.rmem_max, may hold it lower).
    m_socket->setSocketOption(QAbstractSocket::ReceiveBufferSizeSocketOption, 16 << 20);
    m_socket->setSocketOption(QAbstractSocket::SendBufferSizeSocketOption, 16 << 20);
    qInfo().nospace().noquote() << "Link UDP listener on: " << addr.toString() << ":" << port;
    return true;
}

void LinkServer::OnReadyRead() {
    QElapsedTimer busy;
    busy.start();
    while (m_socket->hasPendingDatagrams()) {
        QByteArray raw;
        raw.resize(static_cast<int>(m_socket->pendingDatagramSize()));
        QHostAddress sender;
        uint16_t senderPort = 0;
        m_socket->readDatagram(raw.data(), raw.size(), &sender, &senderPort);

        if (raw.isEmpty())
            continue;

        // The games' link only (phase 9: no STUN, nothing else on this port).
        if (ServerLink::KindOf(raw)) {
            HandleLink(raw, sender, senderPort);
        }
    }
    Busy(busy);
}

void LinkServer::HandleLink(const QByteArray& datagram, const QHostAddress& sender,
                            uint16_t senderPort) {
    bool ipv4 = false;
    const quint32 address = sender.toIPv4Address(&ipv4);
    if (!ipv4)
        return;
    const ServerLink::Endpoint from{address, senderPort};
    if (m_hosted.enabled) {
        const auto kind = ServerLink::KindOf(datagram);
        if (!kind || !Admit(datagram, *kind, address, from))
            return;
    } else if (ServerLink::KindOf(datagram) == ServerLink::Kind::Hello &&
               m_link.CookieValid(from, datagram, m_clock.elapsed()) &&
               !m_helloBudget.Allow(address, m_clock.elapsed())) {
        return; // the hosted guard limits hellos tighter
    }
    m_helloAddress = address;
    Apply(m_link.Receive(from, datagram, m_clock.elapsed()));
}

bool LinkServer::Admit(const QByteArray& datagram, ServerLink::Kind kind, quint32 address,
                       const ServerLink::Endpoint& from) {
    const qint64 now = m_clock.elapsed();
    // Game frames share 1200 bytes (coop/game_link.h); a large game packet goes alone, never this
    // big.
    if (datagram.size() > 16384 || m_guard.Banned(address, now))
        return false;
    if (kind == ServerLink::Kind::Hello) {
        // Without a cookie of its source the hub only sends one back (no larger, nothing kept):
        // a spoofed flood of hellos must not use up a player's address's hellos.
        if (!m_link.CookieValid(from, datagram, now))
            return true;
        if (!m_guard.AllowHello(address, now))
            return false;
        const bool known = m_link.VirtualOf(from).has_value();
        if (!known && !m_guard.AllowLink(address, m_addressLinks.value(address))) {
            if (const auto refusal =
                    m_link.Refusal(from, datagram, "too many games from your address", now))
                m_socket->writeDatagram(*refusal, QHostAddress(from.ipv4), from.port);
            return false;
        }
        return true; // a banned id is known once its hello opens (SignIn)
    }
    // A link's datagrams: their rates once their tag is checked (the hub's admit).
    return true;
}

void LinkServer::OnLinkTick() {
    if (m_hosted.enabled)
        m_link.SetSignupWork(m_signupWork.Bits(m_clock.elapsed()));
    QElapsedTimer busy;
    busy.start();
    if (!m_redirecting.isEmpty())
        ResendRedirects();
    Apply(m_link.Tick(m_clock.elapsed()));
    Busy(busy);
}

void LinkServer::OnWorldTick() {
    QElapsedTimer busy;
    busy.start();
    Apply(m_world.Tick(QDateTime::currentMSecsSinceEpoch()));
    // The Rpc tunnels' resends and keepalives.
    for (auto& [id, end] : m_rpc)
        FlushRpc(*end);
    Busy(busy);
}

std::optional<ServerLink::SignInVerdict> LinkServer::SignIn(const QByteArray& online_id,
                                                            const QByteArray& secret, bool paid) {
    // A hello comes again every second until it is welcomed, and after every outage: the
    // password's hash (100k rounds) only the first time.
    // Hosted: a banned id hears nothing (its id is read only once its hello opened).
    if (m_hosted.enabled && m_guard.BannedId(online_id))
        return std::nullopt;
    const QByteArray hash = QCryptographicHash::hash(secret, QCryptographicHash::Sha256);
    // A region: another shard's ticket for this one stands for the account and its password (the
    // shard that sent the game checked them); its requests run at the player's lobby hello.
    bool ticketed = false;
    if (m_directory && !m_link.HelloTicket().isEmpty()) {
        const auto ticket = Region::OpenTicket(m_ticketKey, m_link.HelloTicket(), online_id, secret,
                                               m_shardConfig.name.toStdString(),
                                               QDateTime::currentMSecsSinceEpoch());
        ticketed = ticket.has_value();
        if (ticket && !m_ticketsTaken.contains(m_link.HelloTicket())) {
            if (m_ticketsTaken.size() > 100'000)
                m_ticketsTaken.clear(); // each holds 2 minutes
            m_ticketsTaken.insert(m_link.HelloTicket());
            m_world.Expect(online_id.toHex().toStdString(),
                           {ticket->requests().begin(), ticket->requests().end()});
        }
    }
    const auto known = m_signedIn.constFind(online_id);
    if (known != m_signedIn.constEnd()) {
        if (*known != hash)
            return ServerLink::SignInVerdict{false, {}};
        return Placed(online_id, hash, ticketed);
    }
    bool make = !m_shared->config || m_shared->config->IsMakingAccountsAtSignIn();
    if (!m_hosted.enabled) {
        DbError why = DbError::None;
        const auto user = m_db.SignIn(QString::fromUtf8(online_id), QString::fromUtf8(secret), {},
                                      false, make, &why);
        if (!user)
            return ServerLink::SignInVerdict{false, RefusalOf(why)};
        m_signedIn.insert(online_id, hash);
        return Placed(online_id, hash, ticketed);
    }
    // Hosted: on the sign-in thread; the game's next hello hears the verdict.
    const qint64 now = m_clock.elapsed();
    if (const auto refused = m_signInRefused.constFind(online_id);
        refused != m_signInRefused.constEnd() && refused->hash == hash && refused->until_ms > now) {
        // A region: the account is another shard's: the game goes there (it checks the secret).
        if (const Region::Shard* home = refused->elsewhere.isEmpty()
                                            ? nullptr
                                            : m_region.Find(refused->elsewhere.toStdString())) {
            ServerLink::SignInVerdict verdict{true, {}};
            verdict.redirect = ServerLink::Redirect{
                home->address, home->key, {}, "this id's account is on another shard"};
            return verdict;
        }
        return ServerLink::SignInVerdict{false, refused->refusal};
    }
    if (m_signInPending.contains(online_id))
        return std::nullopt;
    // A hello without the work (hub.SetSignupWork) makes no account: one for an id that has one
    // goes on, the others hear how much work, before any password is hashed. A ticket's account is
    // made here as it is elsewhere.
    if (!paid && make && !ticketed) {
        if (!m_db.GetUserId(QString::fromUtf8(online_id)))
            return ServerLink::SignInVerdict{false, {}, true};
        make = false;
    }
    // A flood of new ids from many addresses: no more checks queued than the pool clears in a few
    // seconds; the others' games say hello again in a second.
    if (m_signInPending.size() >= 512 && !ticketed)
        return std::nullopt;
    const quint32 address = m_helloAddress;
    const bool counted = make && !ticketed && m_guard.MayMakeAccount(address, now);
    make = ticketed ? make : counted;
    if (counted)
        m_guard.Made(address, now); // counted now (checks run at once), given back if none is
    m_signInPending.insert(online_id);
    const QString db_path = m_dbPath;
    // A region: a new id (no ticket) is claimed in the directory before its account is made.
    const bool claim = m_directory && !ticketed;
    const QString directory = m_shardConfig.directoryUrl;
    const QByteArray token = m_shardConfig.token;
    const QString shard = m_shardConfig.name;
    // A game another shard sent here goes before the new players queued (a join rush): its team
    // is waiting for it.
    m_signInPool.start(
        [this, online_id, secret, hash, make, address, counted, db_path, claim, directory, token,
         shard] {
            // Each sign-in thread's own connection to the accounts.
            static thread_local std::unique_ptr<Database> db;
            if (!db) {
                db = std::make_unique<Database>(
                    QStringLiteral("link_sign_in_%1")
                        .arg(reinterpret_cast<quintptr>(QThread::currentThreadId())));
                if (!db->Open(db_path))
                    qCritical() << "Link: the sign-in thread's accounts did not open";
            }
            const QString id = QString::fromUtf8(online_id);
            const bool existed = db->GetUserId(id).has_value();
            QString elsewhere;
            QByteArray refusal;
            bool may_make = make;
            if (!existed && make && claim) {
                const auto holder = DirectoryClient::ClaimAccount(directory, token, id, shard);
                if (!holder) {
                    may_make = false;
                    refusal = DirectoryDown;
                } else if (*holder != shard) {
                    may_make = false;
                    elsewhere = *holder;
                }
            }
            DbError why = DbError::None;
            const bool accepted =
                (existed || may_make) &&
                db->SignIn(id, QString::fromUtf8(secret), {}, false, may_make, &why).has_value();
            const bool made = accepted && !existed;
            if (!accepted && refusal.isEmpty())
                refusal = RefusalOf(why);
            QMetaObject::invokeMethod(
                this,
                [this, online_id, hash, accepted, made, address, counted, refusal, elsewhere] {
                    SignedIn(online_id, hash, accepted, made, address, counted, refusal, elsewhere);
                },
                Qt::QueuedConnection);
        },
        ticketed ? 1 : 0);
    return std::nullopt;
}

void LinkServer::SignedIn(const QByteArray& online_id, const QByteArray& hash, bool accepted,
                          bool made, quint32 address, bool counted, const QByteArray& refusal,
                          const QString& elsewhere) {
    m_signInPending.remove(online_id);
    const qint64 now = m_clock.elapsed();
    if (accepted) {
        m_signedIn.insert(online_id, hash);
        m_signInRefused.remove(online_id);
        if (counted && !made)
            m_guard.Unmade(address);
        if (made) {
            m_signupWork.Made(now);
            m_link.SetSignupWork(m_signupWork.Bits(now));
        }
    } else {
        if (counted)
            m_guard.Unmade(address);
        // A wrong password (or no account allowed) is not hashed again for 30 s.
        if (m_signInRefused.size() > 100'000)
            m_signInRefused.clear();
        // The directory not answering is held 5 s only: the game's next hellos try again.
        const qint64 hold = refusal == DirectoryDown ? 5'000 : 30'000;
        m_signInRefused.insert(online_id, Refused{hash, now + hold, refusal, elsewhere});
    }
}

void LinkServer::SetupRegion() {
    if (!m_hosted.enabled) {
        qCritical() << "Region: a shard needs Hosted=true; this server runs alone";
        return;
    }
    const auto address = Region::EndpointOf(m_shardConfig.publicAddress);
    if (!address || m_shardConfig.token.isEmpty()) {
        qCritical().noquote() << "Region: ShardPublicAddress (a.b.c.d:port) and DirectoryToken are"
                                 " needed; this server runs alone";
        return;
    }
    m_ticketKey = Region::TicketKey(m_shardConfig.regionSecret, m_shardConfig.token);
    m_directory = new DirectoryClient(m_shardConfig.directoryUrl, m_shardConfig.token, this);
    BloodborneCoop::TeamLobby::Region lookups;
    lookups.shard_of_code = [this](const std::string& code) {
        const auto found = m_region.codes.find(code);
        return found == m_region.codes.end() ? std::string{} : found->second;
    };
    lookups.named = [this](const std::string& folded) {
        std::vector<BloodborneCoop::TeamLobby::Elsewhere> found;
        for (const Region::Player& player : m_region.players) {
            if (BloodborneCoop::TeamLobby::Fold(player.name) == folded)
                found.push_back({player.shard, player.member});
        }
        return found;
    };
    lookups.world_holder = [this](const std::string& world) {
        const auto found = m_region.held.find(world);
        return found == m_region.held.end() ? std::string{} : found->second;
    };
    m_world.SetRegion(std::move(lookups), m_shardConfig.name.toStdString());
    m_beatTimer = new QTimer(this);
    connect(m_beatTimer, &QTimer::timeout, this, &LinkServer::Heartbeat);
    m_beatTimer->start(m_shardConfig.heartbeatMs);
    Heartbeat();
    qInfo().noquote() << QStringLiteral("Region: shard %1 at %2 (capacity %3%4), directory %5")
                             .arg(m_shardConfig.name, m_shardConfig.publicAddress)
                             .arg(m_shardConfig.capacity)
                             .arg(m_shardConfig.draining ? QStringLiteral(", draining") : QString{})
                             .arg(m_shardConfig.directoryUrl);
}

void LinkServer::Heartbeat() {
    if (m_beating)
        return; // the last one is still under way
    m_beating = true;
    QJsonArray players;
    for (const auto& [member, name] : m_world.Lobby().Players()) {
        const QByteArray id = QByteArray::fromHex(QByteArray::fromStdString(member));
        players.append(QJsonObject{{"id", QString::fromUtf8(id)},
                                   {"member", QString::fromStdString(member)},
                                   {"name", QString::fromStdString(name)}});
    }
    QJsonArray codes;
    for (const std::string& code : m_world.Lobby().Codes())
        codes.append(QString::fromStdString(code));
    QJsonArray worlds;
    for (const bbcoop::TeamWorldInfo& world : m_world.Lobby().Worlds())
        worlds.append(QString::fromStdString(world.id()));
    QJsonArray started;
    for (const QByteArray& id : std::exchange(m_started, {}))
        started.append(QString::fromUtf8(id));
    const QJsonObject beat{
        {"shard", m_shardConfig.name},
        {"address", m_shardConfig.publicAddress},
        {"key", QString::fromStdString(LinkAuth::HexOf(m_link.PublicKey()))},
        {"capacity", m_shardConfig.capacity},
        {"draining", m_shardConfig.draining},
        {"count", m_link.Members()},
        {"load", m_linkLoad},
        {"players", players},
        {"codes", codes},
        {"worlds", worlds},
        {"started", started},
    };
    m_directory->Post("/v1/heartbeat", beat, [this](std::optional<QJsonObject> answer) {
        m_beating = false;
        if (answer) {
            TakeView(*answer);
        } else if (m_beatsLost++ % 30 == 0) {
            qWarning() << "Region: the directory did not answer a heartbeat (" << m_beatsLost
                       << "so far); the last view stays";
        }
    });
}

void LinkServer::TakeView(const QJsonObject& answer) {
    QElapsedTimer busy;
    busy.start();
    std::vector<Region::Invite> inbox;
    m_region = Region::ViewOf(answer, &inbox);
    if (!inbox.empty()) {
        std::vector<std::pair<std::string, bbcoop::TeamInvite>> invites;
        for (Region::Invite& invite : inbox)
            invites.emplace_back(std::move(invite.member), std::move(invite.invite));
        Apply(m_world.TakeInvites(invites, QDateTime::currentMSecsSinceEpoch()));
    }
    Busy(busy);
}

ServerLink::SignInVerdict LinkServer::Placed(const QByteArray& online_id, const QByteArray& hash,
                                             bool ticketed) {
    ServerLink::SignInVerdict verdict{true, {}};
    // Rule (c): a full or draining shard sends a new arrival on (never a game linked here already,
    // nor one another shard just sent here).
    if (!m_directory || ticketed || m_link.HasLink(online_id))
        return verdict;
    const Region::Shard* to =
        Region::PlaceHello(m_region, m_shardConfig.name.toStdString(), m_link.Members(),
                           m_shardConfig.capacity, m_shardConfig.draining);
    if (to)
        verdict.redirect =
            RedirectTo(*to, online_id, hash, {},
                       m_shardConfig.draining ? "this shard is draining" : "this shard is full");
    return verdict;
}

ServerLink::Redirect LinkServer::RedirectTo(const Region::Shard& shard, const QByteArray& online_id,
                                            const QByteArray& hash,
                                            const std::vector<bbcoop::TeamRequest>& requests,
                                            const QByteArray& why) const {
    const QByteArray ticket = Region::SignTicket(
        m_ticketKey, Region::TicketFor(online_id, hash, shard.name,
                                       QDateTime::currentMSecsSinceEpoch(), requests));
    return ServerLink::Redirect{shard.address, shard.key, ticket, why};
}

void LinkServer::SendOn(const BloodborneWorld::Service::Move& move) {
    const Region::Shard* shard = m_region.Find(move.shard);
    const QByteArray hash = m_signedIn.value(move.online_id);
    if (!shard || hash.isEmpty()) {
        qWarning().noquote() << QStringLiteral("Region: %1 stays: shard %2 is not in the view")
                                    .arg(QString::fromUtf8(move.online_id),
                                         QString::fromStdString(move.shard));
        return;
    }
    const QByteArray plain = ServerLink::Encode(
        RedirectTo(*shard, move.online_id, hash, move.requests, "to your team's shard"));
    if (const auto sealed = m_link.Seal(move.link, plain))
        m_socket->writeDatagram(*sealed, QHostAddress(move.link.ipv4), move.link.port);
    // Said again until the game goes (a datagram may be lost); then its link here is forgotten.
    m_redirecting.insert(move.link.Key(),
                         Redirecting{move.link, move.online_id, plain, m_clock.elapsed() + 3000});
    qInfo().noquote() << QStringLiteral("Region: %1 sent on to shard %2 (%3)")
                             .arg(QString::fromUtf8(move.online_id),
                                  QString::fromStdString(move.shard), shard->address.Text());
}

void LinkServer::ResendRedirects() {
    const qint64 now = m_clock.elapsed();
    QList<Redirecting> over;
    for (auto it = m_redirecting.begin(); it != m_redirecting.end();) {
        if (now >= it->until_ms) {
            over.append(*it);
            it = m_redirecting.erase(it);
            continue;
        }
        if (const auto sealed = m_link.Seal(it->to, it->datagram))
            m_socket->writeDatagram(*sealed, QHostAddress(it->to.ipv4), it->to.port);
        ++it;
    }
    for (const Redirecting& gone : over)
        Apply(m_link.Kick(gone.online_id, 0, now));
}

void LinkServer::OnRpc(const ServerLink::Endpoint& from, const QByteArray& online_id,
                       const QByteArray& body) {
    auto& slot = m_rpc[online_id];
    if (!slot) {
        // The game's calls: none past a read of its bridge (16 KiB), its chunks a Send's. Each end
        // its own instance: a game whose end was dropped sees a new one and connects again.
        BloodborneCoop::CoopStream::Limits limits;
        limits.message_bytes = MaxRpcMessageBytes;
        limits.chunk_bytes = BloodborneCoop::CoopStream::ChunkBytes + 1;
        limits.held_bytes = MaxRpcHeldBytes;
        limits.unreliable_channels = 16;
        slot = std::make_unique<RpcEnd>();
        slot->tunnel = std::make_unique<BloodborneCoop::RpcTunnel>(
            "shadnet", QRandomGenerator::global()->generate() | 1u, limits);
    }
    RpcEnd& end = *slot;
    end.to = from;
    const auto bytes =
        std::span{reinterpret_cast<const u8*>(body.constData()), static_cast<size_t>(body.size())};
    const auto events = end.tunnel->Receive(bytes, m_clock.elapsed());
    if (end.tunnel->Broken()) {
        qInfo().noquote() << QStringLiteral("Link: %1's Rpc tunnel dropped: a message past %2 "
                                            "bytes")
                                 .arg(QString::fromLatin1(online_id.toHex()))
                                 .arg(MaxRpcMessageBytes);
        CloseRpc(end);
        m_rpc.erase(online_id);
        return;
    }
    for (auto& event : events) {
        using Kind = BloodborneCoop::RpcTunnel::Event::Kind;
        switch (event.kind) {
        case Kind::Opened:
            OpenRpc(online_id, end);
            break;
        case Kind::Data: {
            const QByteArray data(reinterpret_cast<const char*>(event.bytes.data()),
                                  static_cast<qsizetype>(event.bytes.size()));
            if (end.socket && end.socket->state() == QAbstractSocket::ConnectedState) {
                end.socket->write(data);
            } else if (end.pending.size() + data.size() <= MaxRpcPendingBytes) {
                end.pending.append(data);
            } else {
                // The connection to the TCP port is not up and the game keeps sending: over.
                CloseRpc(end);
                end.tunnel->Close(m_clock.elapsed());
            }
            break;
        }
        case Kind::Closed:
            CloseRpc(end);
            break;
        }
    }
    FlushRpc(end);
}

void LinkServer::OpenRpc(const QByteArray& online_id, RpcEnd& end) {
    CloseRpc(end);
    QTcpSocket* socket = new QTcpSocket(this);
    end.socket = socket;
    // The game's end, while this socket is still its connection (it may be gone by then).
    const auto mine = [this, online_id, socket]() -> RpcEnd* {
        const auto found = m_rpc.find(online_id);
        return found != m_rpc.end() && found->second->socket == socket ? found->second.get()
                                                                       : nullptr;
    };
    connect(socket, &QTcpSocket::connected, this, [socket, mine] {
        if (RpcEnd* at = mine(); at && !at->pending.isEmpty())
            socket->write(std::exchange(at->pending, {}));
    });
    connect(socket, &QTcpSocket::readyRead, this, [this, socket, mine] {
        const QByteArray data = socket->readAll();
        RpcEnd* at = mine();
        if (!at)
            return;
        at->tunnel->Write(std::span{reinterpret_cast<const u8*>(data.constData()),
                                    static_cast<size_t>(data.size())},
                          m_clock.elapsed());
        // A game that does not ack what shadNet answers: its connection ends, not the server's
        // memory.
        if (at->tunnel->UnackedBytes() > MaxRpcUnackedBytes) {
            CloseRpc(*at);
            at->tunnel->Close(m_clock.elapsed());
        }
        FlushRpc(*at);
    });
    connect(socket, &QTcpSocket::disconnected, this, [this, socket, mine] {
        RpcEnd* at = mine();
        socket->deleteLater();
        if (!at)
            return;
        at->socket = nullptr;
        at->tunnel->Close(m_clock.elapsed());
        FlushRpc(*at);
    });
    socket->connectToHost(QHostAddress::LocalHost, m_tcpPort);
}

void LinkServer::CloseRpc(RpcEnd& end) {
    end.pending.clear();
    if (QTcpSocket* socket = std::exchange(end.socket, nullptr)) {
        // Its disconnected signal finds it no longer the game's connection, and deletes it.
        socket->disconnectFromHost();
        if (socket->state() == QAbstractSocket::UnconnectedState)
            socket->deleteLater();
    }
}

void LinkServer::FlushRpc(RpcEnd& end) {
    for (const auto& body : end.tunnel->Flush(m_clock.elapsed())) {
        if (const auto sealed = m_link.Seal(
                end.to, ServerLink::EncodeRpc(QByteArray(reinterpret_cast<const char*>(body.data()),
                                                         static_cast<qsizetype>(body.size())))))
            m_socket->writeDatagram(*sealed, QHostAddress(end.to.ipv4), end.to.port);
    }
}

void LinkServer::Apply(const ServerLink::Hub::Result& result) {
    for (const auto& send : result.sends)
        m_socket->writeDatagram(send.datagram, QHostAddress(send.to.ipv4), send.to.port);
    for (const auto& line : result.log)
        qInfo().noquote() << line;
    for (const auto& gone : result.gone) {
        Apply(m_world.Forget(gone));
        if (const auto found = m_linkAddress.constFind(gone.Key());
            found != m_linkAddress.constEnd()) {
            const quint32 address = *found;
            m_linkAddress.erase(found);
            if (--m_addressLinks[address] <= 0)
                m_addressLinks.remove(address);
        }
        m_guard.Forget(gone.Key());
        m_shared->httpGate.Gone(gone.Key());
        // A game gone (or moved to another address): its connection to the TCP port is over.
        for (auto it = m_rpc.begin(); it != m_rpc.end();) {
            if (it->second->to == gone) {
                CloseRpc(*it->second);
                it = m_rpc.erase(it);
            } else {
                ++it;
            }
        }
    }
    // Phase 7: a linked game's address for the rooms and signaling is its virtual one, known from
    // its hello.
    for (const auto& linked : result.linked) {
        m_redirecting.remove(linked.endpoint.Key()); // back here: it stays
        if (!m_linkAddress.contains(linked.endpoint.Key())) {
            m_linkAddress.insert(linked.endpoint.Key(), linked.endpoint.ipv4);
            ++m_addressLinks[linked.endpoint.ipv4];
        }
        m_shared->httpGate.Linked(linked.endpoint.Key(), linked.endpoint.ipv4,
                                  QString::fromUtf8(linked.online_id));
        QWriteLocker lk(&m_shared->matching.udpLock);
        m_shared->matching.udpExt[QString::fromUtf8(linked.online_id)] = {
            QHostAddress(linked.virtual_endpoint.ipv4).toString(), linked.virtual_endpoint.port};
    }
    // Phase 11: a game silent too long (died, or down for longer than a blip): its TCP side ends,
    // and the game is told when it is back (its client signs in again by itself).
    for (const auto& lost : result.lost) {
        Apply(m_world.LinkLost(lost));
        for (auto& [id, end] : m_rpc) {
            if (end->to == lost && end->socket) {
                CloseRpc(*end);
                end->tunnel->Close(m_clock.elapsed());
            }
        }
    }
    for (const auto& coop : result.coop)
        Apply(m_world.Receive(coop.from, coop.online_id, coop.body,
                              QDateTime::currentMSecsSinceEpoch()));
    for (const auto& rpc : result.rpc)
        OnRpc(rpc.from, rpc.online_id, rpc.body);
}

void LinkServer::Apply(const BloodborneWorld::Service::Result& result) {
    for (const auto& send : result.sends)
        if (const auto sealed = m_link.Seal(send.to, ServerLink::EncodeCoop(send.body)))
            m_socket->writeDatagram(*sealed, QHostAddress(send.to.ipv4), send.to.port);
    for (const auto& line : result.log)
        qInfo().noquote() << line;
    for (const auto& move : result.moves)
        SendOn(move);
    for (const auto& invite : result.remote_invites) {
        if (m_directory)
            m_directory->Post(
                "/v1/invite",
                QJsonObject{{"shard", QString::fromStdString(invite.shard)},
                            {"member", QString::fromStdString(invite.member)},
                            {"team", QString::fromStdString(invite.invite.team())},
                            {"from", QString::fromStdString(invite.invite.from())},
                            {"from_name", QString::fromStdString(invite.invite.from_name())}});
    }
    if (m_directory)
        m_started.append(result.started);
}
