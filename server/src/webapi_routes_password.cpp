// SPDX-FileCopyrightText: Copyright 2026 shadNet Project
// SPDX-License-Identifier: GPL-2.0-or-later
// Global servers' own accounts (password_accounts.h, docs/HOSTED_SERVER.md "Global servers:
// accounts"). The launcher reaches these over HTTPS only. A password is read from the request's
// body, hashed or checked on the hash threads, and forgotten: it is never logged, never stored,
// never in an answer. The accounts' rows are read and written on the server's thread only.
#include <algorithm>
#include <memory>

#include <QDateTime>
#include <QDebug>
#include <QHash>
#include <QHostAddress>
#include <QHttpServer>
#include <QHttpServerRequest>
#include <QHttpServerResponder>
#include <QHttpServerResponse>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPointer>
#include <QThreadPool>

#include "bloodborne_reports.h"
#include "database.h"
#include "password_accounts.h"
#include "signed_accounts.h"
#include "virtual_address.h"
#include "webapi_routes_password.h"

namespace {

using Status = QHttpServerResponder::StatusCode;
namespace Accounts = PasswordAccounts;

constexpr qsizetype MaxBodyBytes = 4096;
// The same words for a name nobody has and a wrong password.
const QString Wrong = QStringLiteral("wrong name or password");

QHttpServerResponse Answer(const QJsonObject& body, Status status = Status::Ok) {
    return QHttpServerResponse{"application/json",
                               QJsonDocument(body).toJson(QJsonDocument::Compact), status};
}

QHttpServerResponse Refused(Status status, const QString& why, const QJsonObject& more = {}) {
    QJsonObject body = more;
    body.insert("error", why);
    return Answer(body, status);
}

QString NameProblemText(Accounts::NameProblem problem) {
    return problem == Accounts::NameProblem::Length ? QStringLiteral("length")
                                                    : QStringLiteral("characters");
}

QString PasswordProblemText(Accounts::PasswordProblem problem) {
    switch (problem) {
    case Accounts::PasswordProblem::TooShort:
        return QStringLiteral("short");
    case Accounts::PasswordProblem::TooLong:
        return QStringLiteral("long");
    case Accounts::PasswordProblem::Common:
        return QStringLiteral("common");
    case Accounts::PasswordProblem::HasName:
        return QStringLiteral("name");
    case Accounts::PasswordProblem::None:
        break;
    }
    return {};
}

// What the routes share, on the server's thread (the hash threads get copies only).
struct Service {
    QByteArray key;
    WebApiRoutes::PasswordRoutes routes;
    Accounts::HashParams params;
    Accounts::CommonPasswords common;
    Accounts::Throttle names; // wrong passwords, per account (a name nobody has too)
    Accounts::Throttle addresses{Accounts::Throttle::AddressFreeTries,
                                 Accounts::Throttle::AddressMaxWaitMs}; // and per address
    ReportLimiter requests;
    ReportLimiter signups;
    QHash<QString, qint64> challengesTaken; // a challenge makes one account
    QString dummy;                          // the hash a name nobody has is checked against
    QThreadPool pool;
    int waiting = 0;
    Database* db = nullptr;
    QPointer<QObject> owner;

    Service(const WebApiRoutes::PasswordRoutes& r, Database& database, QObject* o)
        : key{r.key}, routes{r}, params{r.config.hashMemoryKib, r.config.hashPasses},
          requests{r.config.triesPerMinute, 60'000},
          signups{std::max(1, r.signupsPerHour), 3'600'000}, db{&database}, owner{o} {
        pool.setMaxThreadCount(r.config.hashThreads);
        if (!r.config.commonPasswords.isEmpty() && !common.LoadFile(r.config.commonPasswords)) {
            qWarning().noquote() << "Accounts: no" << r.config.commonPasswords
                                 << "(the common passwords refused): the built-in few only";
        }
        qInfo().noquote() << "Accounts:" << common.Size() << "common passwords refused";
        if (!r.config.proxied) {
            qWarning().noquote() << "Accounts: no SignInProxied: put the TLS proxy in front "
                                    "(docs/HOSTED_SERVER.md); the launcher sends passwords over "
                                    "HTTPS only";
        }
        dummy = Accounts::Hash(Accounts::NewRecoveryCode(), params);
    }

    QString Address(const QHttpServerRequest& request) const {
        return SignedAccounts::ClientAddress(request.remoteAddress().toString(),
                                             request.value("X-Forwarded-For"),
                                             routes.config.proxied);
    }

    // Behind our TLS proxy (SignInProxied): only what came through it, over HTTPS. The WebAPI's own
    // plain port is open to players too, and a password never travels in clear.
    bool ThroughProxy(const QHttpServerRequest& request) const {
        if (!routes.config.proxied) {
            return true;
        }
        const QHostAddress from = request.remoteAddress();
        const bool inside = from.isLoopback() || from.isPrivateUse() ||
                            from.isInSubnet(QHostAddress::parseSubnet("10.0.0.0/8")) ||
                            from.isInSubnet(QHostAddress::parseSubnet("172.16.0.0/12")) ||
                            from.isInSubnet(QHostAddress::parseSubnet("192.168.0.0/16"));
        return inside && request.value("X-Forwarded-Proto").toLower() == "https";
    }

    // The checks every request passes before anything else: HTTPS, the address's pace, the body's.
    std::optional<QHttpServerResponse> Gate(const QHttpServerRequest& request, qint64 now_ms) {
        if (!ThroughProxy(request)) {
            return Refused(Status::Forbidden, "https only");
        }
        if (!requests.Allow(qHash(Address(request)), now_ms)) {
            return Refused(Status::TooManyRequests, "slow down");
        }
        if (request.body().size() > MaxBodyBytes) {
            return Refused(Status::PayloadTooLarge, "too large");
        }
        return std::nullopt;
    }

    // The wait a name's or an address's wrong passwords put on the next try (0: none).
    qint64 WaitMs(const QString& name_key, const QString& address, qint64 now_ms) const {
        return std::max(names.WaitMs(name_key, now_ms), addresses.WaitMs(address, now_ms));
    }

    std::optional<QHttpServerResponse> Waiting(const QString& name_key, const QString& address,
                                               qint64 now_ms) const {
        if (const qint64 wait = WaitMs(name_key, address, now_ms); wait > 0) {
            return Refused(Status::TooManyRequests, "wait",
                           QJsonObject{{"seconds", static_cast<qint64>((wait + 999) / 1000)}});
        }
        return std::nullopt;
    }

    void Failed(const QString& name_key, const QString& address, qint64 now_ms) {
        names.Failed(name_key, now_ms);
        addresses.Failed(address, now_ms);
    }

    // A new sign-in's secret for `id`: never from before its last sign out.
    QString SecretFor(const QString& id, qint64 not_before = 0) {
        const qint64 now = QDateTime::currentSecsSinceEpoch();
        return SignedAccounts::Secret(key, id,
                                      std::max({now, db->SignInNotBefore(id), not_before}));
    }

    // Every sign-in of `id` until now dead: here, and through the directory on the region's other
    // shards. The time a sign-in after it is given.
    qint64 Kill(const QString& id) {
        const qint64 at = QDateTime::currentSecsSinceEpoch() + 1;
        db->SetSignInNotBefore(id, at);
        if (routes.revoked) {
            routes.revoked(id, at);
        }
        return at;
    }

    // A free id for a new name: its first salt whose address nobody has.
    std::optional<QString> FreeId(const QString& name_key) {
        for (int salt = 0; salt < SignedAccounts::SaltTries; ++salt) {
            const QString id = Accounts::OnlineId(key, name_key, salt);
            if (!db->PasswordIdTaken(id) && !db->GetUserId(id) &&
                !db->AddressTaken(VirtualAddressOf(id.toUtf8()))) {
                return id;
            }
        }
        return std::nullopt;
    }

    // `work` on a hash thread, then `done` with its result here; a full queue: "busy" at once.
    template <typename Result>
    void Hash(QHttpServerResponder& responder, std::function<Result()> work,
              std::function<QHttpServerResponse(Result)> done) {
        if (waiting >= routes.config.hashQueue) {
            responder.sendResponse(Refused(Status::ServiceUnavailable, "busy"));
            return;
        }
        ++waiting;
        auto answer = std::make_shared<QHttpServerResponder>(std::move(responder));
        pool.start([this, answer, work = std::move(work), done = std::move(done)] {
            auto result = std::make_shared<Result>(work());
            QMetaObject::invokeMethod(
                owner.data(),
                [this, answer, result, done] {
                    --waiting;
                    answer->sendResponse(done(std::move(*result)));
                },
                Qt::QueuedConnection);
        });
    }
};

QJsonObject Body(const QHttpServerRequest& request) {
    return QJsonDocument::fromJson(request.body()).object();
}

} // namespace

namespace WebApiRoutes {

void RegisterPasswordRoutes(QHttpServer& http, Database& db, const PasswordRoutes& routes,
                            QObject* owner) {
    http.route("/bb/auth/config", QHttpServerRequest::Method::Get, [routes] {
        return Answer(QJsonObject{{"sign_in", QJsonArray{"password"}},
                                  {"work_bits", routes.workBits},
                                  {"name_min", Accounts::NameMin},
                                  {"name_max", Accounts::NameMax},
                                  {"password_min", Accounts::PasswordMin},
                                  {"password_max", Accounts::PasswordMax}});
    });
    if (!routes.config.home) {
        return; // the region's accounts' home answers the rest (the proxy sends /bb/auth/* there)
    }
    // Held by the routes, so for as long as the server.
    auto service = std::make_shared<Service>(routes, db, owner);

    http.route("/bb/auth/challenge", QHttpServerRequest::Method::Post,
               [service](const QHttpServerRequest& request) {
                   const qint64 now_ms = QDateTime::currentMSecsSinceEpoch();
                   if (auto refused = service->Gate(request, now_ms)) {
                       return std::move(*refused);
                   }
                   return Answer(QJsonObject{
                       {"challenge", Accounts::NewChallenge(service->key, now_ms / 1000)},
                       {"bits", service->routes.workBits}});
               });

    http.route(
        "/bb/auth/sign-up", QHttpServerRequest::Method::Post,
        [service](const QHttpServerRequest& request, QHttpServerResponder& responder) {
            const qint64 now_ms = QDateTime::currentMSecsSinceEpoch();
            if (auto refused = service->Gate(request, now_ms)) {
                responder.sendResponse(std::move(*refused));
                return;
            }
            const QJsonObject body = Body(request);
            const QString name = body.value("name").toString();
            const QString password = body.value("password").toString();
            const QString challenge = body.value("challenge").toString();
            const auto nonce = static_cast<quint64>(body.value("nonce").toInteger(-1));
            if (const auto problem = Accounts::CheckName(name);
                problem != Accounts::NameProblem::None) {
                responder.sendResponse(Refused(Status::BadRequest, "name",
                                               QJsonObject{{"problem", NameProblemText(problem)}}));
                return;
            }
            if (const auto problem = Accounts::CheckPassword(password, name, service->common);
                problem != Accounts::PasswordProblem::None) {
                responder.sendResponse(
                    Refused(Status::BadRequest, "password",
                            QJsonObject{{"problem", PasswordProblemText(problem)}}));
                return;
            }
            const QString name_key = Accounts::NameKey(name);
            const qint64 now_s = now_ms / 1000;
            if (service->challengesTaken.size() > 100'000) {
                service->challengesTaken.removeIf([now_s](const auto& item) {
                    return now_s - item.value() > Accounts::ChallengeSeconds;
                });
            }
            if (!Accounts::ChallengeFresh(service->key, challenge, now_s) ||
                service->challengesTaken.contains(challenge) ||
                !Accounts::WorkDone(challenge, name_key, nonce, service->routes.workBits)) {
                responder.sendResponse(Refused(Status::BadRequest, "work"));
                return;
            }
            if (service->db->FindPasswordAccount(name_key)) {
                responder.sendResponse(Refused(Status::Conflict, "taken"));
                return;
            }
            const QString address = service->Address(request);
            if (!service->signups.Allow(qHash(address), now_ms)) {
                responder.sendResponse(Refused(Status::TooManyRequests, "signups"));
                return;
            }
            service->challengesTaken.insert(challenge, now_s);
            const Accounts::HashParams params = service->params;
            service->Hash<QString>(
                responder, [password, params] { return Accounts::Hash(password, params); },
                [service, name, name_key](QString hash) {
                    // Another signup for the name may have come first while this one hashed.
                    if (service->db->FindPasswordAccount(name_key)) {
                        return Refused(Status::Conflict, "taken");
                    }
                    const auto id = service->FreeId(name_key);
                    const QString code = Accounts::NewRecoveryCode();
                    if (!id || !service->db->AddPasswordAccount(
                                   {name_key, name, *id, hash,
                                    Accounts::RecoveryDigest(service->key, code)})) {
                        qWarning().noquote()
                            << "Accounts: a signup not stored:" << service->db->lastError();
                        return Refused(Status::ServiceUnavailable, "try again later");
                    }
                    qInfo().noquote() << "Accounts: made" << *id;
                    return Answer(QJsonObject{{"id", *id},
                                              {"secret", service->SecretFor(*id)},
                                              {"name", name},
                                              {"recovery_code", code}});
                });
        });

    // Sign in and Change password: the password checked (a name nobody has against the dummy,
    // the same work), a stored hash of other parameters made again.
    struct Checked {
        bool good = false;
        QString rehash;  // the password's hash with today's parameters, when it needs one
        QString renewed; // Change password: the new one's hash
    };
    const auto check = [service](const QHttpServerRequest& request, QHttpServerResponder& responder,
                                 bool changing) {
        const qint64 now_ms = QDateTime::currentMSecsSinceEpoch();
        if (auto refused = service->Gate(request, now_ms)) {
            responder.sendResponse(std::move(*refused));
            return;
        }
        const QJsonObject body = Body(request);
        const QString name = body.value("name").toString();
        const QString password = body.value("password").toString();
        const QString renewed = body.value("new_password").toString();
        const QString name_key = Accounts::NameKey(name.left(Accounts::NameMax + 1));
        const QString address = service->Address(request);
        if (auto waiting = service->Waiting(name_key, address, now_ms)) {
            responder.sendResponse(std::move(*waiting));
            return;
        }
        if (password.isEmpty() || password.size() > Accounts::PasswordMax * 2) {
            responder.sendResponse(Refused(Status::Unauthorized, Wrong));
            return;
        }
        if (changing) {
            if (const auto problem = Accounts::CheckPassword(renewed, name, service->common);
                problem != Accounts::PasswordProblem::None) {
                responder.sendResponse(
                    Refused(Status::BadRequest, "password",
                            QJsonObject{{"problem", PasswordProblemText(problem)}}));
                return;
            }
        }
        const auto account = service->db->FindPasswordAccount(name_key);
        const QString stored = account ? account->hash : service->dummy;
        const Accounts::HashParams params = service->params;
        service->Hash<Checked>(
            responder,
            [password, renewed, stored, params, changing] {
                Checked checked;
                checked.good = Accounts::Verify(password, stored);
                if (checked.good && changing) {
                    checked.renewed = Accounts::Hash(renewed, params);
                } else if (checked.good && Accounts::NeedsRehash(stored, params)) {
                    checked.rehash = Accounts::Hash(password, params);
                }
                return checked;
            },
            [service, account, name_key, address, changing](Checked checked) {
                const qint64 now_ms = QDateTime::currentMSecsSinceEpoch();
                if (!account || !checked.good) {
                    service->Failed(name_key, address, now_ms);
                    return Refused(Status::Unauthorized, Wrong);
                }
                service->names.Succeeded(name_key);
                qint64 not_before = 0;
                if (changing) {
                    if (!service->db->SetPasswordHash(name_key, checked.renewed)) {
                        return Refused(Status::ServiceUnavailable, "try again later");
                    }
                    not_before = service->Kill(account->onlineId);
                    qInfo().noquote() << "Accounts: a new password for" << account->onlineId;
                } else if (!checked.rehash.isEmpty()) {
                    service->db->SetPasswordHash(name_key, checked.rehash);
                }
                return Answer(
                    QJsonObject{{"id", account->onlineId},
                                {"secret", service->SecretFor(account->onlineId, not_before)},
                                {"name", account->name}});
            });
    };
    http.route("/bb/auth/sign-in", QHttpServerRequest::Method::Post,
               [check](const QHttpServerRequest& request, QHttpServerResponder& responder) {
                   check(request, responder, false);
               });
    http.route("/bb/auth/change", QHttpServerRequest::Method::Post,
               [check](const QHttpServerRequest& request, QHttpServerResponder& responder) {
                   check(request, responder, true);
               });

    // Forgot password: the name and its recovery code give a new password and a new code; every
    // sign-in until now is dead. A wrong code counts as a wrong password.
    http.route(
        "/bb/auth/recover", QHttpServerRequest::Method::Post,
        [service](const QHttpServerRequest& request, QHttpServerResponder& responder) {
            const qint64 now_ms = QDateTime::currentMSecsSinceEpoch();
            if (auto refused = service->Gate(request, now_ms)) {
                responder.sendResponse(std::move(*refused));
                return;
            }
            const QJsonObject body = Body(request);
            const QString name = body.value("name").toString();
            const QString code = body.value("recovery_code").toString();
            const QString renewed = body.value("new_password").toString();
            const QString name_key = Accounts::NameKey(name.left(Accounts::NameMax + 1));
            const QString address = service->Address(request);
            if (auto waiting = service->Waiting(name_key, address, now_ms)) {
                responder.sendResponse(std::move(*waiting));
                return;
            }
            if (const auto problem = Accounts::CheckPassword(renewed, name, service->common);
                problem != Accounts::PasswordProblem::None) {
                responder.sendResponse(
                    Refused(Status::BadRequest, "password",
                            QJsonObject{{"problem", PasswordProblemText(problem)}}));
                return;
            }
            const auto account = service->db->FindPasswordAccount(name_key);
            const QString typed = Accounts::RecoveryDigest(service->key, code.left(64));
            quint8 differs =
                account && !typed.isEmpty() && typed.size() == account->recovery.size() ? 0 : 1;
            for (qsizetype i = 0; differs == 0 && i < typed.size(); ++i) {
                differs |= static_cast<quint8>(typed[i].unicode() ^ account->recovery[i].unicode());
            }
            if (differs != 0) {
                service->Failed(name_key, address, now_ms);
                responder.sendResponse(Refused(Status::Unauthorized, "wrong name or code"));
                return;
            }
            service->names.Succeeded(name_key);
            const Accounts::HashParams params = service->params;
            service->Hash<QString>(
                responder, [renewed, params] { return Accounts::Hash(renewed, params); },
                [service, account, name_key](QString hash) {
                    const QString code = Accounts::NewRecoveryCode();
                    if (!service->db->SetPasswordHash(name_key, hash) ||
                        !service->db->SetPasswordRecovery(
                            name_key, Accounts::RecoveryDigest(service->key, code))) {
                        return Refused(Status::ServiceUnavailable, "try again later");
                    }
                    const qint64 not_before = service->Kill(account->onlineId);
                    qInfo().noquote() << "Accounts: recovered" << account->onlineId;
                    return Answer(
                        QJsonObject{{"id", account->onlineId},
                                    {"secret", service->SecretFor(account->onlineId, not_before)},
                                    {"name", account->name},
                                    {"recovery_code", code}});
                });
        });

    // Sign out: this account's secrets until now are dead, on every shard.
    http.route("/bb/auth/sign-out", QHttpServerRequest::Method::Post,
               [service](const QHttpServerRequest& request) {
                   const qint64 now_ms = QDateTime::currentMSecsSinceEpoch();
                   if (auto refused = service->Gate(request, now_ms)) {
                       return std::move(*refused);
                   }
                   const QJsonObject body = Body(request);
                   const QByteArray id = body.value("id").toString().toUtf8();
                   const QByteArray secret = body.value("secret").toString().toUtf8();
                   if (SignedAccounts::Check(service->key, id, secret, now_ms / 1000,
                                             service->db->SignInNotBefore(QString::fromUtf8(id))) !=
                       SignedAccounts::Verdict::Good) {
                       return Refused(Status::Forbidden, "not this account's");
                   }
                   service->Kill(QString::fromUtf8(id));
                   return Answer(QJsonObject{{"signed_out", true}});
               });
}

} // namespace WebApiRoutes
