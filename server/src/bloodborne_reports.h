// SPDX-FileCopyrightText: Copyright 2026 shadNet Project
// SPDX-License-Identifier: GPL-2.0-or-later
// Phase 11: crash dumps and logs the players' launchers send the server on their own after a crash,
// kept per player: reports/<player id>/<when>-<file>. Only dumps and text, under 64 MB each, and a
// player's newest 50.
#pragma once

#include <optional>
#include <QByteArray>
#include <QDateTime>
#include <QHash>
#include <QString>

class ReportStore {
public:
    static constexpr qint64 MaxBytes = 64ll * 1024 * 1024;
    static constexpr int KeptPerPlayer = 50;

    explicit ReportStore(QString folder, qint64 max_bytes = MaxBytes)
        : folder{std::move(folder)}, max_bytes{max_bytes} {}

    // The file's path once kept, nothing when the player, its name, kind or size is refused.
    std::optional<QString> Save(const QString& player, const QString& file_name,
                                const QByteArray& bytes,
                                const QDateTime& now = QDateTime::currentDateTimeUtc());

private:
    QString folder;
    qint64 max_bytes;
};

// Reports one address may send: `burst`, then one more every `every_ms`. Asked before the
// password is checked (100k hashing rounds a report), so a flood costs nothing.
class ReportLimiter {
public:
    explicit ReportLimiter(int burst = 20, qint64 every_ms = 60'000)
        : burst{burst}, every_ms{every_ms} {}
    bool Allow(quint32 address, qint64 now_ms);

private:
    struct Bucket {
        double tokens = 0;
        qint64 at_ms = 0;
    };
    int burst;
    qint64 every_ms;
    QHash<quint32, Bucket> buckets;
};

class Database;
class QHttpServer;
// POST /bb/report/<file>, the body the file, X-Player and X-Secret its player id: kept in
// `folder` (ReportStore), at most `max_bytes` each, an address's limited (ReportLimiter).
void RegisterReportRoutes(QHttpServer& http, Database& db, const QString& folder,
                          qint64 max_bytes = ReportStore::MaxBytes);
