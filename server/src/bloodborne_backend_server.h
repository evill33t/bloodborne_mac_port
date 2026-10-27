// SPDX-FileCopyrightText: Copyright 2026 shadNet Project
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <memory>

#include <QHttpServer>
#include <QObject>
#include <QTcpServer>

#include "bloodborne_backend.h"
#include "config.h"
#include "http_gate.h"

// The game's own online services on their own port (BackendPort, 20443), where the game reaches
// them through host_overrides.json. On unless BloodborneBackend=false in shadnet.cfg; then (or
// when it cannot start) the kits run backend/bb_dummy_server.py on that port instead.
class BloodborneBackendServer : public QObject {
    Q_OBJECT
public:
    explicit BloodborneBackendServer(QObject* parent = nullptr);
    ~BloodborneBackendServer();

    // `gate`: who may ask (http_gate.h); null answers everyone.
    bool Start(ConfigManager* config, HttpGate::Gate* gate = nullptr);

private:
    // "http://<address>:<port>" as the game that sent `request` reaches this server.
    QString AddressFor(const QHttpServerRequest& request) const;

    ConfigManager* m_config = nullptr;
    HttpGate::Gate* m_gate = nullptr;
    QString m_fixedAddress;
    quint16 m_port = 0;
    QString m_webApiPort;
    Bloodborne::Backend::BloodborneBackend m_backend;
    std::unique_ptr<QHttpServer> m_http;
    std::unique_ptr<QTcpServer> m_tcp;
};
