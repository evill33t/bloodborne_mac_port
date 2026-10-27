// SPDX-FileCopyrightText: Copyright 2026 shadNet Project
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <map>
#include <memory>
#include <QElapsedTimer>
#include <QHostAddress>
#include <QLocalServer>
#include <QLocalSocket>
#include <QObject>
#include <QTcpSocket>
#include <QThread>
#include <QThreadPool>
#include <QTimer>
#include <QUdpSocket>
#include "admission.h"
#include "bloodborne_world.h"
#include "client_session.h"
#include "coop/rpc_tunnel.h"
#include "database.h"
#include "directory_client.h"
#include "region.h"
#include "server_link.h"

// shadNet's UDP port: the games' link (server_link.h), which relays their frames and carries
// their messages with the parties' worlds (bloodborne_world.h). Phase 11: its hello signs the
// player in (the accounts in `db_path`), and each game's calls for shadNet's TCP port come through
// it (coop/rpc_tunnel.h) and go on to that port on 127.0.0.1, so a host opens this port only.
class LinkServer : public QObject {
    Q_OBJECT
public:
    explicit LinkServer(SharedState* shared, const QString& db_path, quint16 tcp_port,
                        QObject* parent = nullptr);
    ~LinkServer() override;
    bool Start(const QHostAddress& addr, uint16_t port);

private slots:
    void OnReadyRead();
    void OnLinkTick();
    void OnWorldTick();

private:
    // The games' link (co-op phase 2, server_link.h): relayed frames, hellos, pings.
    void HandleLink(const QByteArray& datagram, const QHostAddress& sender, uint16_t senderPort);
    void Apply(const ServerLink::Hub::Result& result);
    void Apply(const BloodborneWorld::Service::Result& result);
    std::optional<ServerLink::SignInVerdict> SignIn(const QByteArray& online_id,
                                                    const QByteArray& secret, bool paid);
    // Hosted: a password checked on the sign-in thread (its account made if it may be).
    // `counted`: it counted as an account made when it was queued (Admission::Guard::Made).
    // `elsewhere`: a region's shard that holds the id's account (it was not made here).
    void SignedIn(const QByteArray& online_id, const QByteArray& hash, bool accepted, bool made,
                  quint32 address, bool counted, const QByteArray& refusal,
                  const QString& elsewhere = {});
    void LoadBanList();
    // Hosted: the operator's commands (admin.h) on the admin socket, and the worlds' retirement.
    void OnAdminConnection();
    void RunAdmin(QLocalSocket* socket, const QString& line);
    void RetireWorlds();
    // The link thread's busy share (the stats command): the time in its handlers, by 10 s.
    void Busy(const QElapsedTimer& since);
    // Hosted (docs/HOSTED_SERVER.md): whether a datagram from `sender` goes on to the hub.
    bool Admit(const QByteArray& datagram, ServerLink::Kind kind, quint32 address,
               const ServerLink::Endpoint& from);

    // A region of shards (docs/SCALING.md): the directory's heartbeat and its view; a signed-in
    // hello's verdict, sent on when this shard is full or draining (rule (c)); a game the lobby
    // sends to another shard (rules (a), (b)), its Redirect said again until its game goes.
    void SetupRegion();
    void Heartbeat();
    void TakeView(const QJsonObject& answer);
    ServerLink::SignInVerdict Placed(const QByteArray& online_id, const QByteArray& hash,
                                     bool ticketed);
    ServerLink::Redirect RedirectTo(const Region::Shard& shard, const QByteArray& online_id,
                                    const QByteArray& hash,
                                    const std::vector<bbcoop::TeamRequest>& requests,
                                    const QByteArray& why) const;
    void SendOn(const BloodborneWorld::Service::Move& move);
    void ResendRedirects();

    // A game's end of its Rpc tunnel here: the connection to shadNet's TCP port it stands for.
    // What a game can make it hold: its messages (the bridge reads 16 KiB at a time) and those
    // held out of order; what came before the TCP connection was up (it is up within a moment on
    // 127.0.0.1); shadNet's answers it has not acked (one packet is at most 8 MiB, protocol.h).
    // Past the first the tunnel goes, past the others its connection ends.
    static constexpr size_t MaxRpcMessageBytes = 64 << 10;
    static constexpr size_t MaxRpcHeldBytes = 256 << 10;
    static constexpr qsizetype MaxRpcPendingBytes = 1 << 20;
    static constexpr size_t MaxRpcUnackedBytes = 9 << 20;
    struct RpcEnd {
        std::unique_ptr<BloodborneCoop::RpcTunnel> tunnel;
        ServerLink::Endpoint to;
        QTcpSocket* socket = nullptr;
        QByteArray pending; // what came before the connection to the TCP port was up
    };
    void OnRpc(const ServerLink::Endpoint& from, const QByteArray& online_id,
               const QByteArray& body);
    void OpenRpc(const QByteArray& online_id, RpcEnd& end);
    void CloseRpc(RpcEnd& end);
    void FlushRpc(RpcEnd& end);

    QUdpSocket* m_socket;
    SharedState* m_shared;
    ServerLink::Hub m_link;
    QTimer* m_linkTimer;
    // The parties' worlds (co-op phase 3), on the link's Coop datagrams; its timer runs its
    // resends and heartbeats.
    BloodborneWorld::Service m_world;
    QTimer* m_worldTimer;
    QElapsedTimer m_clock;
    Database m_db;
    quint16 m_tcpPort;
    QHash<QByteArray, QByteArray> m_signedIn; // player id -> its secret's hash, once signed in
    // Hosted: who may come in (admission.h), the sign-in thread and its checks under way, the
    // passwords refused lately (not hashed again for a while), the links of each address.
    HostedConfig m_hosted;
    Admission::Guard m_guard;
    Admission::HelloBudget m_helloBudget;
    Admission::SignupWork m_signupWork{0};
    QThreadPool m_signInPool;
    QSet<QByteArray> m_signInPending;
    struct Refused {
        QByteArray hash;
        qint64 until_ms = 0;
        QByteArray refusal;
        QString elsewhere; // a region: the shard that holds the account
    };
    QHash<QByteArray, Refused> m_signInRefused;
    quint32 m_helloAddress = 0;            // the address of the hello the hub is reading
    QHash<quint64, quint32> m_linkAddress; // link (Endpoint::Key) -> its address
    QHash<quint32, int> m_addressLinks;    // address -> its links
    QTimer* m_banTimer = nullptr;
    QLocalServer* m_admin = nullptr;
    QTimer* m_retireTimer = nullptr;
    QElapsedTimer m_loadClock;
    qint64 m_busyNs = 0;
    double m_linkLoad = 0;
    QString m_dbPath;
    QString m_keyError; // the link's key did not read (LinkKeyFile): the link does not start
    std::map<QByteArray, std::unique_ptr<RpcEnd>> m_rpc; // by player id

    ShardConfig m_shardConfig;
    DirectoryClient* m_directory = nullptr; // a region's only
    Region::View m_region;
    LinkAuth::HmacKey m_ticketKey;
    QTimer* m_beatTimer = nullptr;
    bool m_beating = false;
    quint64 m_beatsLost = 0;
    QList<QByteArray> m_started; // since the last heartbeat
    struct Redirecting {
        ServerLink::Endpoint to;
        QByteArray online_id;
        QByteArray datagram; // plain
        qint64 until_ms = 0;
    };
    QHash<quint64, Redirecting> m_redirecting; // by link
    QSet<QByteArray> m_ticketsTaken;           // whose requests the world expects already
};
