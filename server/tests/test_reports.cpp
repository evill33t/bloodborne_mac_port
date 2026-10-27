// SPDX-FileCopyrightText: Copyright 2026 shadNet Project
// SPDX-License-Identifier: GPL-2.0-or-later
// Phase 11: crash dumps and logs the games send the server on their own
// (bloodborne_reports.h), kept per player under reports/, so friend sessions
// don't depend on collecting logs by hand.
#include <iostream>

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QTemporaryDir>

#include "bloodborne_reports.h"

namespace {

bool Check(bool condition, const char *expression, int line) {
  if (!condition) {
    std::cerr << "check failed at line " << line << ": " << expression << '\n';
  }
  return condition;
}

#define CHECK(expression)                                                      \
  do {                                                                         \
    if (!Check((expression), #expression, __LINE__))                           \
      return 1;                                                                \
  } while (false)

const QDateTime At{QDate{2026, 10, 15}, QTime{17, 40, 5}, QTimeZone::UTC};

int AReportIsKeptUnderItsPlayer(const QString &folder) {
  ReportStore store{folder};
  const auto saved =
      store.Save("p0123456789abcd", "crash-20261015-174001.dmp", "MDMP...", At);
  CHECK(saved.has_value());
  CHECK(*saved ==
        QDir{folder}.filePath(
            "p0123456789abcd/20261015-174005-crash-20261015-174001.dmp"));
  QFile file{*saved};
  CHECK(file.open(QIODevice::ReadOnly) && file.readAll() == "MDMP...");
  CHECK(store.Save("p0123456789abcd", "shad_log.prev.txt", "log", At)
            .has_value());
  return 0;
}

int ABadNameOrKindIsRefused(const QString &folder) {
  ReportStore store{folder};
  CHECK(!store.Save("p0123456789abcd", "../../shadnet.cfg", "x", At));
  CHECK(!store.Save("p0123456789abcd", "a\\b.txt", "x", At));
  CHECK(!store.Save("p0123456789abcd", "game.exe", "x", At));
  CHECK(!store.Save("..", "log.txt", "x", At));
  CHECK(!store.Save("p0123456789abcd", "log.txt", {}, At)); // nothing to keep
  CHECK(!store.Save(
      "p0123456789abcd", "big.txt",
      QByteArray(static_cast<qsizetype>(ReportStore::MaxBytes) + 1, 'x'), At));
  return 0;
}

int APlayerKeepsItsNewestReports(const QString &folder) {
  ReportStore store{folder};
  for (int i = 0; i < ReportStore::KeptPerPlayer + 5; ++i) {
    CHECK(store
              .Save("pfull",
                    QStringLiteral("log%1.txt").arg(i, 3, 10, QLatin1Char('0')),
                    "x", At.addSecs(i))
              .has_value());
  }
  const QStringList kept =
      QDir{QDir{folder}.filePath("pfull")}.entryList(QDir::Files, QDir::Name);
  CHECK(kept.size() == ReportStore::KeptPerPlayer);
  CHECK(kept.last().endsWith("log054.txt"));
  CHECK(!kept.first().endsWith("log000.txt"));
  return 0;
}

int ASmallerCapIsKept(const QString &folder) {
  // A hosted server keeps reports under 8 MB.
  ReportStore store{folder, 8};
  CHECK(store.Save("pcap", "crash.dmp", "12345678", At).has_value());
  CHECK(!store.Save("pcap", "crash.dmp", "123456789", At).has_value());
  return 0;
}

int AnAddressSendsSoMany(const QString &) {
  // Every report costs a password hash: an address gets a burst, then one a
  // minute; another address has its own.
  ReportLimiter limiter{3, 60'000};
  constexpr quint32 Home = 0x0A000001;
  constexpr quint32 Other = 0x0A000002;
  qint64 now = 1'000;
  CHECK(limiter.Allow(Home, now));
  CHECK(limiter.Allow(Home, now));
  CHECK(limiter.Allow(Home, now));
  CHECK(!limiter.Allow(Home, now));
  CHECK(limiter.Allow(Other, now));
  now += 60'000;
  CHECK(limiter.Allow(Home, now));
  CHECK(!limiter.Allow(Home, now));
  now += 10 * 60'000; // never more than the burst
  for (int i = 0; i < 3; ++i)
    CHECK(limiter.Allow(Home, now));
  CHECK(!limiter.Allow(Home, now));
  return 0;
}

} // namespace

int main() {
  QTemporaryDir folder;
  if (!folder.isValid()) {
    return 1;
  }
  for (const auto test : {AReportIsKeptUnderItsPlayer, ABadNameOrKindIsRefused,
                          APlayerKeepsItsNewestReports, ASmallerCapIsKept,
                          AnAddressSendsSoMany}) {
    if (test(folder.path()) != 0) {
      return 1;
    }
  }
  std::cout << "reports: all passed\n";
  return 0;
}
