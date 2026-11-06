// SPDX-FileCopyrightText: Copyright 2026 shadNet Project
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <functional>

#include <QByteArray>
#include <QString>

#include "config.h"

class Database;
class QHttpServer;
class QObject;

namespace WebApiRoutes {

// Global servers' own accounts (password_accounts.h), from the launcher over HTTPS only (the TLS
// proxy in front, docs/HOSTED_SERVER.md). Each takes and answers JSON; an error is
// {"error": <what>} (and "problem" or "seconds" with it):
// - GET  /bb/auth/config: {sign_in: ["password"], work_bits, name_min, name_max,
//   password_min, password_max}
// - POST /bb/auth/challenge: {challenge, bits}, the signup's work to do
// - POST /bb/auth/sign-up {name, password, challenge, nonce}: {id, secret, name, recovery_code}
// - POST /bb/auth/sign-in {name, password}: {id, secret, name}
// - POST /bb/auth/change {name, password, new_password}: {id, secret, name}, older sign-ins dead
// - POST /bb/auth/recover {name, recovery_code, new_password}: {id, secret, name, recovery_code},
//   older sign-ins and the old code dead
// - POST /bb/auth/sign-out {id, secret}: {signed_out: true}, older sign-ins dead
// A wrong password and a name nobody has get the same answer, after the same work. `revoked` hears
// each kill (the region's other shards hear it from the directory). The account routes only on the
// accounts' home (SignInConfig::home); `owner` lives as long as the server.
struct PasswordRoutes {
    QByteArray key; // SignedAccounts::KeyFrom
    SignInConfig config;
    int workBits = 20;      // HostedConfig::signupWorkBits
    int signupsPerHour = 5; // an address's, HostedConfig::newAccountsPerHour
    std::function<void(const QString& online_id, qint64 not_before_s)> revoked;
};
void RegisterPasswordRoutes(QHttpServer& http, Database& db, const PasswordRoutes& routes,
                            QObject* owner);

} // namespace WebApiRoutes
