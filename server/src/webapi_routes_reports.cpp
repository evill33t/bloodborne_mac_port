// SPDX-FileCopyrightText: Copyright 2026 shadNet Project
// SPDX-License-Identifier: GPL-2.0-or-later
#include <QDateTime>
#include <QDebug>
#include <QHttpServer>
#include <QHttpServerRequest>
#include <QHttpServerResponse>

#include "bloodborne_reports.h"
#include "database.h"
#include "http_gate.h"

void RegisterReportRoutes(QHttpServer& http, Database& db, const QString& folder,
                          qint64 max_bytes) {
    auto store = std::make_shared<ReportStore>(folder, max_bytes);
    auto limiter = std::make_shared<ReportLimiter>();
    http.route("/bb/report/<arg>", QHttpServerRequest::Method::Post,
               [&db, store, limiter](const QString& file,
                                     const QHttpServerRequest& request) -> QHttpServerResponse {
                   const auto address = HttpGate::Ipv4Of(request.remoteAddress());
                   if (!limiter->Allow(address.value_or(0), QDateTime::currentMSecsSinceEpoch())) {
                       return QHttpServerResponse{QHttpServerResponse::StatusCode::TooManyRequests};
                   }
                   const QString player = QString::fromUtf8(request.value("X-Player"));
                   const QString secret = QString::fromUtf8(request.value("X-Secret"));
                   if (player.isEmpty() || !db.CheckUser(player, secret, {}, false)) {
                       return QHttpServerResponse{QHttpServerResponse::StatusCode::Forbidden};
                   }
                   const auto saved = store->Save(player, file, request.body());
                   if (!saved) {
                       return QHttpServerResponse{QHttpServerResponse::StatusCode::BadRequest};
                   }
                   qInfo().noquote() << "Report from" << player << "kept:" << *saved << "("
                                     << request.body().size() << "bytes)";
                   return QHttpServerResponse{QHttpServerResponse::StatusCode::Ok};
               });
}
