// SPDX-FileCopyrightText: Copyright 2026 shadNet Project
// SPDX-License-Identifier: GPL-2.0-or-later
#include "bloodborne_backend_server.h"

#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHostAddress>
#include <QHttpHeaders>
#include <QHttpServerRequest>
#include <QHttpServerResponder>
#include <QHttpServerResponse>

#include "request_address.h"

using namespace Bloodborne::Backend;

namespace {

QByteArray MethodName(QHttpServerRequest::Method method) {
    switch (method) {
    case QHttpServerRequest::Method::Get:
        return "GET";
    case QHttpServerRequest::Method::Post:
        return "POST";
    case QHttpServerRequest::Method::Put:
        return "PUT";
    case QHttpServerRequest::Method::Delete:
        return "DELETE";
    case QHttpServerRequest::Method::Head:
        return "HEAD";
    default:
        return "OTHER";
    }
}

QString InFolder(const QString& folder, const QString& name) {
    return QDir(folder).filePath(name);
}

} // namespace

BloodborneBackendServer::BloodborneBackendServer(QObject* parent) : QObject(parent) {}
BloodborneBackendServer::~BloodborneBackendServer() {
    if (m_gate) {
        m_gate->SetUserIdOf({});
    }
}

bool BloodborneBackendServer::Start(ConfigManager* config, HttpGate::Gate* gate) {
    m_config = config;
    const BackendConfig settings = config->GetBackend();
    m_port = settings.port;
    m_webApiPort = config->GetWebApiPort();
    // The host's address as the players reach it (the kit's HOSTIP); else the address each game
    // connected to, which is the same thing on a VPN or a LAN.
    m_fixedAddress = qEnvironmentVariable("SHADNET_BACKEND_ADDRESS", settings.address).trimmed();

    BackendOptions options;
    QFile templ(QStringLiteral(":/backend/ss_info_template.xml"));
    if (!templ.open(QIODevice::ReadOnly)) {
        qCritical() << "Backend: the ss.info template is missing from this build";
        return false;
    }
    options.ssInfoTemplate = templ.readAll();
    options.storePath = settings.store;
    const auto showOwn = ParseShowOwn(settings.showOwn);
    if (!showOwn) {
        qCritical().noquote() << "Backend: BackendShowOwn: unknown value in" << settings.showOwn
                              << "; choose from" << ShowOwnChoices.join(QStringLiteral(", "))
                              << "or all";
        return false;
    }
    options.showOwn = *showOwn;
    options.ghostTtlSeconds = qint64{settings.ghostTtlMinutes} * 60;
    options.lanternTtlSeconds = qint64{settings.lanternTtlMinutes} * 60;
    options.tombCap = settings.tombCap;
    options.summonPoll = settings.summonPoll;
    options.noticePath = InFolder(settings.folder, QStringLiteral("notice.txt"));
    options.broadcastPath = InFolder(settings.folder, QStringLiteral("broadcast.txt"));
    options.loginMessagePath = InFolder(settings.folder, QStringLiteral("login_message.txt"));

    // Everything that can fail is checked before the port is taken.
    QString error;
    if (!m_backend.Open(options, &error)) {
        qCritical().noquote() << "Backend:" << error;
        return false;
    }

    // The summons on the WebAPI port name the same UserIds.
    if (gate) {
        m_gate = gate;
        m_gate->SetUserIdOf([this](const QString& account) { return m_backend.UserIdOf(account); });
    }

    m_http = std::make_unique<QHttpServer>(this);
    // Every path and method goes to the backend, as in the Python server.
    m_http->setMissingHandler(this, [this](const QHttpServerRequest& request,
                                           QHttpServerResponder& responder) {
        BackendRequest backendRequest;
        backendRequest.method = MethodName(request.method());
        backendRequest.path = request.url().path();
        backendRequest.body = request.body();
        // Hosted (http_gate.h): only a linked game's address, as one of its players. A GET (the
        // ss.info a launcher checks before the game runs, the status page) acts as nobody.
        if (m_gate && m_gate->Enforced() && backendRequest.method != "GET") {
            const BackendIdentity who = IdentityOf(backendRequest);
            const auto from = HttpGate::Ipv4Of(request.remoteAddress());
            const HttpGate::Verdict verdict = !from         ? HttpGate::Verdict::NoLink
                                              : who.account ? m_gate->ForPlayer(*from, who.account)
                                                            : m_gate->ForUserId(*from, who.userId);
            if (verdict != HttpGate::Verdict::Allowed) {
                qDebug().noquote()
                    << "backend: refused" << request.remoteAddress().toString()
                    << backendRequest.path
                    << (verdict == HttpGate::Verdict::NoLink ? "(no link)" : "(not its player)");
                responder.sendResponse(QHttpServerResponse(
                    "text/plain", "Forbidden", QHttpServerResponse::StatusCode::Forbidden));
                return;
            }
        }
        const QString address = AddressFor(request);
        backendRequest.servicesUrl = QStringLiteral("http://%1:%2").arg(address).arg(m_port);
        backendRequest.summonUrl = QStringLiteral("http://%1:%2").arg(address, m_webApiPort);
        const BackendReply reply = m_backend.Handle(backendRequest);
        QHttpServerResponse response(reply.contentType, reply.body,
                                     static_cast<QHttpServerResponse::StatusCode>(reply.status));
        if (!reply.headers.isEmpty()) {
            QHttpHeaders headers = response.headers();
            for (const auto& [name, value] : reply.headers) {
                headers.append(name, value);
            }
            response.setHeaders(std::move(headers));
        }
        responder.sendResponse(response);
    });

    m_tcp = std::make_unique<QTcpServer>(this);
    const QString host = config->GetHost();
    if (!m_tcp->listen(QHostAddress(host), m_port)) {
        qCritical() << "Backend: failed to bind" << host << ":" << m_port << "-"
                    << m_tcp->errorString();
        return false;
    }
    if (!m_http->bind(m_tcp.get())) {
        qCritical() << "Backend: QHttpServer failed to attach to listener";
        return false;
    }

    qInfo().nospace().noquote() << "Backend (the game's online services) listening on: " << host
                                << ":" << m_port;
    qInfo().noquote() << "  ss.info, services and summons at"
                      << (m_fixedAddress.isEmpty()
                              ? QStringLiteral("the address each game connects to")
                              : m_fixedAddress);
    if (m_backend.HasStore()) {
        const Json counts = m_backend.Counts();
        qInfo().noquote() << "  content ->" << QFileInfo(settings.store).absoluteFilePath();
        qInfo().noquote() << QStringLiteral(
                                 "            %1 players, %2 messages, %3 bloodstains, %4 ghosts, "
                                 "%5 chalice dungeons")
                                 .arg(counts.Get(QStringLiteral("users")).IntValue())
                                 .arg(counts.Get(QStringLiteral("blood_messages")).IntValue())
                                 .arg(counts.Get(QStringLiteral("tombstones")).IntValue())
                                 .arg(counts.Get(QStringLiteral("ghosts")).IntValue())
                                 .arg(counts.Get(QStringLiteral("chalice_dungeons")).IntValue());
        QStringList shown(options.showOwn.begin(), options.showOwn.end());
        shown.sort();
        qInfo().noquote() << "            own content shown:"
                          << (shown.isEmpty() ? QStringLiteral("messages only")
                                              : shown.join(QStringLiteral(", ")));
    } else {
        qInfo().noquote() << "  content -> off (world stays empty)";
    }
    qInfo().noquote() << "  notice, broadcast and login message from"
                      << QFileInfo(settings.folder).absoluteFilePath();
    return true;
}

QString BloodborneBackendServer::AddressFor(const QHttpServerRequest& request) const {
    // The address the player used (a guest over the internet reached the host's public address,
    // while the host's own is its LAN one), then the host's given address, then the socket's.
    const QByteArray host =
        request.headers().value(QHttpHeaders::WellKnownHeader::Host).toByteArray();
    const std::string used = AddressFromHostHeader(std::string_view(host.constData(), host.size()));
    if (!used.empty()) {
        return QString::fromStdString(used);
    }
    if (!m_fixedAddress.isEmpty()) {
        return m_fixedAddress;
    }
    QHostAddress local = request.localAddress();
    bool isV4 = false;
    const quint32 v4 = local.toIPv4Address(&isV4);
    if (isV4) {
        return QHostAddress(v4).toString();
    }
    return local.isNull() ? QStringLiteral("127.0.0.1") : local.toString();
}
