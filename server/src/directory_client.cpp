// SPDX-FileCopyrightText: Copyright 2026 shadNet Project
// SPDX-License-Identifier: GPL-2.0-or-later
#include "directory_client.h"

#include <QEventLoop>
#include <QJsonDocument>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QTimer>

namespace {

QNetworkRequest RequestFor(const QString& url, const QByteArray& token, int timeout_ms) {
    QNetworkRequest request{QUrl{url}};
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    request.setRawHeader("Authorization", "Bearer " + token);
    request.setTransferTimeout(timeout_ms);
    return request;
}

std::optional<QJsonObject> AnswerOf(QNetworkReply* reply) {
    if (reply->error() != QNetworkReply::NoError) {
        return std::nullopt;
    }
    const QJsonDocument document = QJsonDocument::fromJson(reply->readAll());
    if (!document.isObject()) {
        return std::nullopt;
    }
    return document.object();
}

} // namespace

DirectoryClient::DirectoryClient(QString url, QByteArray token, QObject* parent)
    : QObject(parent), url{std::move(url)}, token{std::move(token)},
      net{new QNetworkAccessManager(this)} {}

void DirectoryClient::Post(const QString& path, const QJsonObject& body,
                           std::function<void(std::optional<QJsonObject>)> done, int timeout_ms) {
    QNetworkReply* reply = net->post(RequestFor(url + path, token, timeout_ms),
                                     QJsonDocument{body}.toJson(QJsonDocument::Compact));
    connect(reply, &QNetworkReply::finished, this, [reply, done = std::move(done)] {
        reply->deleteLater();
        if (done) {
            done(AnswerOf(reply));
        }
    });
}

std::optional<QString> DirectoryClient::ClaimAccount(const QString& url, const QByteArray& token,
                                                     const QString& online_id, const QString& shard,
                                                     int timeout_ms) {
    // A sign-in thread has no event loop of its own: one for this call.
    QNetworkAccessManager net;
    QEventLoop loop;
    QNetworkReply* reply =
        net.post(RequestFor(url + "/v1/account", token, timeout_ms),
                 QJsonDocument{QJsonObject{{"id", online_id}, {"shard", shard}}}.toJson(
                     QJsonDocument::Compact));
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    QTimer::singleShot(timeout_ms + 500, &loop, &QEventLoop::quit);
    loop.exec();
    std::optional<QString> holder;
    if (reply->isFinished()) {
        if (const auto answer = AnswerOf(reply)) {
            if (const QString said = answer->value("shard").toString(); !said.isEmpty()) {
                holder = said;
            }
        }
    }
    reply->abort();
    delete reply;
    return holder;
}
