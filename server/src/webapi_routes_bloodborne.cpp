// SPDX-FileCopyrightText: Copyright 2026 shadNet Project
// SPDX-License-Identifier: GPL-2.0-or-later
#include "webapi_routes_bloodborne.h"

#include <cstdlib>
#include <memory>
#include <optional>

#include <QDateTime>
#include <QDebug>
#include <QHttpHeaders>
#include <QHttpServer>
#include <QHttpServerRequest>
#include <QHttpServerResponse>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QList>
#include <QString>

#include "bloodborne_summon_broker.h"
#include "http_gate.h"

namespace WebApiRoutes {
namespace {

constexpr auto HostPlacementHeader = "X-ShadPS4-Bloodborne-Host-Placement";
constexpr auto PartyHeader = "X-ShadPS4-Bloodborne-Party";

QByteArray Party(const QHttpServerRequest& request) {
    return Bloodborne::NormalizePartyKey(request.value(PartyHeader));
}

QHttpServerResponse SummonResponse(const QString& messageId, const QByteArray& hostPlacement = {}) {
    QJsonObject body;
    body.insert(QStringLiteral("ResKind"), 0);
    body.insert(QStringLiteral("MessageId"), messageId);
    QHttpServerResponse response{"application/json",
                                 QJsonDocument(body).toJson(QJsonDocument::Compact),
                                 QHttpServerResponse::StatusCode::Ok};
    if (!hostPlacement.isEmpty()) {
        QHttpHeaders headers = response.headers();
        if (headers.append(HostPlacementHeader, hostPlacement)) {
            response.setHeaders(std::move(headers));
        }
    }
    return response;
}

QHttpServerResponse SummonListResponse(const QList<QByteArray>& signs) {
    QByteArray body = "{\"SummonDataList\":[";
    for (qsizetype index = 0; index < signs.size(); ++index) {
        if (index != 0) {
            body.append(',');
        }
        body.append(signs[index]);
    }
    body.append("],\"ResKind\":0,\"MessageId\":\"SummonDataGetListResponse\"}");
    return QHttpServerResponse{"application/json", body, QHttpServerResponse::StatusCode::Ok};
}

QHttpServerResponse RawJsonResponse(const QByteArray& body, const QByteArray& hostPlacement = {}) {
    QHttpServerResponse response{"application/json", body, QHttpServerResponse::StatusCode::Ok};
    if (!hostPlacement.isEmpty()) {
        QHttpHeaders headers = response.headers();
        if (headers.append(HostPlacementHeader, hostPlacement)) {
            response.setHeaders(std::move(headers));
        }
    }
    return response;
}

QHttpServerResponse InvalidRequest(const QString& reason) {
    QJsonObject body;
    body.insert(QStringLiteral("ResKind"), 1);
    body.insert(QStringLiteral("MessageId"), QString());
    body.insert(QStringLiteral("Error"), reason);
    return QHttpServerResponse{"application/json",
                               QJsonDocument(body).toJson(QJsonDocument::Compact),
                               QHttpServerResponse::StatusCode::BadRequest};
}

// Hosted (http_gate.h): a summon request only from a linked game's address, as one of its players
// (the body's UserId). Each must name its player (the game's always do): one naming nobody could
// remove or claim anyone's sign by its SessionId. Nothing when it may go on.
std::optional<QHttpServerResponse> Refused(const HttpGate::Gate* gate,
                                           const QHttpServerRequest& request,
                                           const std::optional<QJsonObject>& body) {
    if (!gate || !gate->Enforced()) {
        return std::nullopt;
    }
    std::optional<qint64> userId;
    if (body && body->contains(QStringLiteral("UserId")) &&
        !body->value(QStringLiteral("UserId")).isNull()) {
        const QJsonValue value = body->value(QStringLiteral("UserId"));
        userId = value.isDouble() ? value.toVariant().toLongLong() : -1;
    } else if (body) {
        userId = -1; // a player not linked here
    }
    const auto from = HttpGate::Ipv4Of(request.remoteAddress());
    if (from && gate->ForUserId(*from, userId) == HttpGate::Verdict::Allowed) {
        return std::nullopt;
    }
    QJsonObject reply;
    reply.insert(QStringLiteral("ResKind"), 1);
    reply.insert(QStringLiteral("MessageId"), QString());
    reply.insert(QStringLiteral("Error"), QStringLiteral("Not a linked game's player"));
    return QHttpServerResponse{"application/json",
                               QJsonDocument(reply).toJson(QJsonDocument::Compact),
                               QHttpServerResponse::StatusCode::Forbidden};
}

std::optional<QJsonObject> ParseRequest(const QHttpServerRequest& request) {
    QJsonParseError error{};
    const QJsonDocument document = QJsonDocument::fromJson(request.body(), &error);
    if (error.error != QJsonParseError::NoError || !document.isObject()) {
        return std::nullopt;
    }
    return document.object();
}

qint64 Integer(const QJsonObject& object, const QString& key, qint64 fallback = 0) {
    const QJsonValue value = object.value(key);
    return value.isDouble() ? value.toVariant().toLongLong() : fallback;
}

bool EnvEnabled(const char* name) {
    const char* value = std::getenv(name);
    return value != nullptr && value[0] != '\0' && QString::fromUtf8(value).trimmed() != "0";
}

void TraceSummonRequest(const char* route, const QHttpServerRequest& request) {
    if (!EnvEnabled("SHADNET_BLOODBORNE_RE_TRACE")) {
        return;
    }
    qInfo().noquote() << "Bloodborne summon RE" << route << request.body();
}

} // namespace

void RegisterBloodborneRoutes(QHttpServer& http, bool seamlessCoop, const HttpGate::Gate* gate) {
    seamlessCoop = seamlessCoop || EnvEnabled("SHADNET_BLOODBORNE_SEAMLESS_COOP");
    Bloodborne::SummonBroker::Options options;
    options.seamlessCoop = seamlessCoop;
    auto broker = std::make_shared<Bloodborne::SummonBroker>(options);

    qInfo() << "Bloodborne summon routes registered; seamless co-op"
            << (broker->IsSeamlessCoopEnabled() ? "enabled" : "disabled") << "anywhere summons"
            << (broker->IsSeamlessAnywhereSummonsEnabled() ? "enabled" : "disabled");

    http.route("/summon_messenger/create", QHttpServerRequest::Method::Post,
               [broker, gate](const QHttpServerRequest& request) -> QHttpServerResponse {
                   TraceSummonRequest("create", request);
                   const auto body = ParseRequest(request);
                   if (auto refused = Refused(gate, request, body)) {
                       return std::move(*refused);
                   }
                   if (!body || !Bloodborne::HasRequiredAdvertisementFields(*body)) {
                       return InvalidRequest(QStringLiteral("Invalid summon advertisement"));
                   }
                   const auto result = broker->Advertise(
                       *body, request.body(), QDateTime::currentMSecsSinceEpoch(), Party(request));
                   if (!result.pendingClaim.isEmpty()) {
                       const QByteArray response =
                           Bloodborne::BuildClaimDeliveryResponse(result.pendingClaim);
                       if (response.isEmpty()) {
                           return InvalidRequest(QStringLiteral("Invalid pending summon claim"));
                       }
                       qInfo() << "Bloodborne summon: delivered claim to user"
                               << Integer(*body, QStringLiteral("UserId")) << "session"
                               << body->value(QStringLiteral("SessionId")).toString()
                               << "host-placement-bytes" << result.pendingHostPlacement.size();
                       return RawJsonResponse(response, result.pendingHostPlacement);
                   }
                   if (!result.pendingHostPlacement.isEmpty()) {
                       qInfo() << "Bloodborne summon: preparing cross-map user"
                               << Integer(*body, QStringLiteral("UserId")) << "session"
                               << body->value(QStringLiteral("SessionId")).toString()
                               << "destination placement bytes"
                               << result.pendingHostPlacement.size();
                       return SummonResponse(QStringLiteral("SummonDataCreateResponse"),
                                             result.pendingHostPlacement);
                   }
                   qInfo() << "Bloodborne summon: advertised user"
                           << Integer(*body, QStringLiteral("UserId")) << "session"
                           << body->value(QStringLiteral("SessionId")).toString();
                   return SummonResponse(QStringLiteral("SummonDataCreateResponse"));
               });

    http.route("/summon_messenger/get", QHttpServerRequest::Method::Post,
               [broker, gate](const QHttpServerRequest& request) -> QHttpServerResponse {
                   TraceSummonRequest("get", request);
                   const auto body = ParseRequest(request);
                   if (auto refused = Refused(gate, request, body)) {
                       return std::move(*refused);
                   }
                   if (!body) {
                       return InvalidRequest(QStringLiteral("Invalid summon search"));
                   }
                   const QList<QByteArray> signs =
                       broker->Search(*body, QDateTime::currentMSecsSinceEpoch(),
                                      request.value(HostPlacementHeader), Party(request));
                   qInfo() << "Bloodborne summon: search for user"
                           << Integer(*body, QStringLiteral("UserId")) << "returned"
                           << signs.size();
                   return SummonListResponse(signs);
               });

    http.route("/summon_messenger/delete", QHttpServerRequest::Method::Post,
               [broker, gate](const QHttpServerRequest& request) -> QHttpServerResponse {
                   TraceSummonRequest("delete", request);
                   const auto body = ParseRequest(request);
                   if (auto refused = Refused(gate, request, body)) {
                       return std::move(*refused);
                   }
                   if (!body) {
                       return InvalidRequest(QStringLiteral("Invalid summon removal"));
                   }
                   const auto result =
                       broker->Consume(*body, QDateTime::currentMSecsSinceEpoch(), Party(request));
                   qInfo() << "Bloodborne summon: consumed" << result.consumed << "retained"
                           << result.retained << "advertisement(s) host-placement-bytes"
                           << result.pendingHostPlacement.size();
                   QHttpServerResponse response =
                       SummonResponse(QStringLiteral("SummonDataRemoveResponse"));
                   if (result.pendingHostPlacement.isEmpty()) {
                       return response;
                   }
                   QHttpHeaders headers = response.headers();
                   if (headers.append(HostPlacementHeader, result.pendingHostPlacement)) {
                       response.setHeaders(std::move(headers));
                   }
                   return response;
               });

    http.route("/summon_messenger/request", QHttpServerRequest::Method::Post,
               [broker, gate](const QHttpServerRequest& request) -> QHttpServerResponse {
                   TraceSummonRequest("request", request);
                   const auto body = ParseRequest(request);
                   if (auto refused = Refused(gate, request, body)) {
                       return std::move(*refused);
                   }
                   if (!body) {
                       return InvalidRequest(QStringLiteral("Invalid summon request"));
                   }
                   const auto result =
                       broker->Claim(*body, request.body(), QDateTime::currentMSecsSinceEpoch(),
                                     request.value(HostPlacementHeader), Party(request));
                   switch (result.status) {
                   case Bloodborne::SummonBroker::ClaimStatus::Claimed:
                       qInfo() << "Bloodborne summon: claimed session" << result.targetSessionId
                               << "user" << result.targetUserId;
                       break;
                   case Bloodborne::SummonBroker::ClaimStatus::AlreadyClaimed:
                       qInfo() << "Bloodborne summon: repeated claim for session"
                               << result.targetSessionId;
                       break;
                   case Bloodborne::SummonBroker::ClaimStatus::NotFound:
                       qWarning() << "Bloodborne summon: claim target not found";
                       break;
                   case Bloodborne::SummonBroker::ClaimStatus::Conflict:
                       qWarning() << "Bloodborne summon: conflicting claim for session"
                                  << result.targetSessionId;
                       break;
                   }
                   return SummonResponse(QStringLiteral("SummonDataSummonResponse"));
               });

    // The party's boards (/bloodborne/party/*) went with phase 3c: they travel over the server
    // link's world stream now (bloodborne_world.h).
}

} // namespace WebApiRoutes
