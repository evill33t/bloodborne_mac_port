// SPDX-FileCopyrightText: Copyright 2026 shadNet Project
// SPDX-License-Identifier: GPL-2.0-or-later
#include "bloodborne_reports.h"

#include <algorithm>

#include <QDir>
#include <QRegularExpression>
#include <QSaveFile>

namespace {

bool PlainName(const QString& name, int longest) {
    static const QRegularExpression plain{QStringLiteral("^[A-Za-z0-9_][A-Za-z0-9._-]*$")};
    return !name.isEmpty() && name.size() <= longest && plain.match(name).hasMatch() &&
           !name.contains(QStringLiteral(".."));
}

bool KeptKind(const QString& name) {
    for (const char* kind : {".dmp", ".txt", ".log", ".json"}) {
        if (name.endsWith(QLatin1String(kind), Qt::CaseInsensitive)) {
            return true;
        }
    }
    return false;
}

} // namespace

std::optional<QString> ReportStore::Save(const QString& player, const QString& file_name,
                                         const QByteArray& bytes, const QDateTime& now) {
    if (!PlainName(player, 32) || !PlainName(file_name, 80) || !KeptKind(file_name) ||
        bytes.isEmpty() || bytes.size() > max_bytes) {
        return std::nullopt;
    }
    const QDir dir{QDir{folder}.filePath(player)};
    if (!QDir{}.mkpath(dir.path())) {
        return std::nullopt;
    }
    const QString path =
        dir.filePath(now.toUTC().toString(QStringLiteral("yyyyMMdd-HHmmss-")) + file_name);
    QSaveFile file{path};
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit()) {
        return std::nullopt;
    }
    QStringList kept = dir.entryList(QDir::Files, QDir::Name);
    while (kept.size() > KeptPerPlayer) {
        QFile::remove(dir.filePath(kept.takeFirst()));
    }
    return path;
}

bool ReportLimiter::Allow(quint32 address, qint64 now_ms) {
    // Addresses whose bucket filled up again are forgotten now and then.
    if (buckets.size() > 4096) {
        for (auto it = buckets.begin(); it != buckets.end();) {
            if (now_ms - it->at_ms >= burst * every_ms)
                it = buckets.erase(it);
            else
                ++it;
        }
    }
    auto it = buckets.find(address);
    if (it == buckets.end())
        it = buckets.insert(address, Bucket{double(burst), now_ms});
    it->tokens = std::min<double>(burst, it->tokens + double(now_ms - it->at_ms) / every_ms);
    it->at_ms = now_ms;
    if (it->tokens < 1)
        return false;
    it->tokens -= 1;
    return true;
}
