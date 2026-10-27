// SPDX-FileCopyrightText: Copyright 2026 shadNet Project
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <functional>
#include <optional>

#include <QByteArray>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QObject>
#include <QString>

// A shard's calls to its region's directory (docs/SCALING.md, server/directory/): on the link's
// thread they never wait (the answer comes back as a callback there); the sign-in threads' account
// claims wait, on their own thread.
class DirectoryClient : public QObject {
    Q_OBJECT
public:
    DirectoryClient(QString url, QByteArray token, QObject* parent = nullptr);

    // POST `path` with `body`; `done` gets the answer (nothing: no answer, or not JSON). Calls
    // still under way after `timeout_ms` are given up.
    void Post(const QString& path, const QJsonObject& body,
              std::function<void(std::optional<QJsonObject>)> done = {}, int timeout_ms = 4000);

    // From any thread: claims `online_id` for `shard`'s new account (POST /v1/account), waiting
    // at most `timeout_ms`. The shard holding it (`shard` itself when the claim is this one's), or
    // nothing when the directory does not answer.
    static std::optional<QString> ClaimAccount(const QString& url, const QByteArray& token,
                                               const QString& online_id, const QString& shard,
                                               int timeout_ms = 3000);

private:
    QString url;
    QByteArray token;
    QNetworkAccessManager* net;
};
