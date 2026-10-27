// SPDX-FileCopyrightText: Copyright 2026 shadNet Project
// SPDX-License-Identifier: GPL-2.0-or-later
// The worlds' disk writes off the link thread (bloodborne_world.h WorldWriter):
// each world's records reach the disk in order, a few fsyncs for many records,
// the writer drains when it goes, and a stop without draining (a crash) loses
// only the tail and leaves a journal that reads.
#include <chrono>
#include <iostream>
#include <thread>
#include <vector>

#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QTemporaryDir>

#include "bloodborne_world.h"

using BloodborneWorld::WorldStore;
using BloodborneWorld::WorldWriter;

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

bbcoop::StoredWorld World(quint64 journal_from) {
  bbcoop::StoredWorld world;
  world.set_version(1);
  world.set_world(7);
  world.set_journal_from(journal_from);
  return world;
}

bbcoop::JournalRecord Record(quint64 number) {
  bbcoop::JournalRecord record;
  record.set_number(number);
  record.set_at_ms(static_cast<qint64>(number) * 10);
  record.mutable_reset_map()->set_map(static_cast<quint32>(number));
  return record;
}

// The numbers of the records a load of the folder gives, and whether it read
// without a word about the journal.
struct Read {
  bool world = false;
  quint64 journal_from = 0;
  std::vector<quint64> numbers;
  QStringList log;
};
Read Load(const QString &folder) {
  Read read;
  const auto loaded = WorldStore{folder}.Load(read.log);
  if (loaded) {
    read.world = true;
    read.journal_from = loaded->world.journal_from();
    for (const bbcoop::JournalRecord &record : loaded->journal) {
      read.numbers.push_back(record.number());
    }
  }
  return read;
}

bool Consecutive(const std::vector<quint64> &numbers, quint64 from) {
  for (size_t at = 0; at < numbers.size(); ++at) {
    if (numbers[at] != from + at) {
      return false;
    }
  }
  return true;
}

// Many worlds' records interleaved, a world written whole among them: after
// the drain each world's journal holds its records, in order.
int EachWorldsRecordsAreOnDiskInOrderAfterTheDrain() {
  QTemporaryDir dir;
  CHECK(dir.isValid());
  std::vector<QString> folders;
  for (int at = 0; at < 3; ++at) {
    folders.push_back(QDir{dir.path()}.filePath(QStringLiteral("w%1").arg(at)));
  }
  {
    WorldWriter writer;
    for (const QString &folder : folders) {
      writer.Compact(folder, World(0));
    }
    for (quint64 number = 0; number < 300; ++number) {
      for (const QString &folder : folders) {
        writer.Append(folder, Record(number));
      }
      if (number == 199) {
        writer.Compact(folders[1], World(200)); // records 0-199 in the world
      }
    }
    writer.Drain();
    for (size_t at = 0; at < folders.size(); ++at) {
      const Read read = Load(folders[at]);
      CHECK(read.world && read.log.isEmpty());
      const quint64 from = at == 1 ? 200 : 0;
      CHECK(read.journal_from == from);
      CHECK(read.numbers.size() == 300 - from);
      CHECK(Consecutive(read.numbers, from));
    }
    // Group commit: far fewer fsyncs than records.
    CHECK(writer.Commits() < 900 / 10);
  }
  return 0;
}

// The writer's going (the server's shutdown) writes everything it was given.
int TheWriterDrainsWhenItGoes() {
  QTemporaryDir dir;
  const QString folder = QDir{dir.path()}.filePath("w");
  {
    WorldWriter writer{60'000,
                       1'000'000}; // no commit of its own before it goes
    writer.Compact(folder, World(0));
    for (quint64 number = 0; number < 500; ++number) {
      writer.Append(folder, Record(number));
    }
    writer.Save(QDir{folder}.filePath("team_world.pb"), "team");
  }
  const Read read = Load(folder);
  CHECK(read.numbers.size() == 500 && Consecutive(read.numbers, 0));
  QFile team{QDir{folder}.filePath("team_world.pb")};
  CHECK(team.open(QIODevice::ReadOnly) && team.readAll() == "team");
  return 0;
}

// A stop without draining (the process killed): what was synced stays, the
// rest is a tail of the records in order, and the journal reads to its end.
int AStopWithoutTheDrainLosesOnlyTheTail() {
  QTemporaryDir dir;
  const QString folder = QDir{dir.path()}.filePath("w");
  quint64 synced = 0;
  {
    WorldWriter writer{5, 64};
    writer.Compact(folder, World(0));
    for (quint64 number = 0; number < 2000; ++number) {
      writer.Append(folder, Record(number));
      if (number == 999) {
        while (writer.Synced() < 1000) {
          std::this_thread::sleep_for(std::chrono::milliseconds{1});
        }
      }
    }
    synced = writer.Synced();
    writer.Abandon();
  }
  const Read read = Load(folder);
  CHECK(read.world && read.log.isEmpty());
  CHECK(read.numbers.size() >= synced && read.numbers.size() <= 2000);
  CHECK(Consecutive(read.numbers, 0));

  // A record the crash cut in two, at any byte: the journal reads up to it.
  QFile journal{QDir{folder}.filePath("journal.pb")};
  CHECK(journal.open(QIODevice::ReadOnly));
  const QByteArray whole = journal.readAll();
  journal.close();
  for (const qsizetype cut : {qsizetype{1}, qsizetype{3}, qsizetype{5},
                              qsizetype{9}, qsizetype{17}}) {
    CHECK(journal.open(QIODevice::WriteOnly | QIODevice::Truncate));
    journal.write(whole.left(whole.size() - cut));
    journal.close();
    const Read torn = Load(folder);
    CHECK(torn.world && !torn.numbers.empty());
    CHECK(torn.numbers.size() < read.numbers.size());
    CHECK(Consecutive(torn.numbers, 0));
  }
  return 0;
}

// The journal is never cut before its world is written whole (the backups and
// a crash in a Compact keep the records): a world that does not write leaves
// the journal as it was.
int AWorldThatDoesNotWriteKeepsItsJournal() {
  QTemporaryDir dir;
  const QString folder = QDir{dir.path()}.filePath("w");
  {
    WorldWriter writer;
    writer.Compact(folder, World(0));
    for (quint64 number = 0; number < 10; ++number) {
      writer.Append(folder, Record(number));
    }
    writer.Drain();
    // world.pb cannot be replaced (a folder in its place).
    QFile::remove(QDir{folder}.filePath("world.pb"));
    QDir{folder}.mkpath("world.pb");
    writer.Compact(folder, World(10));
    writer.Drain();
    CHECK(writer.TakeLog().join(' ').contains("could not write"));
  }
  QFile journal{QDir{folder}.filePath("journal.pb")};
  CHECK(journal.size() > 0);
  return 0;
}

// Reward publication uses this completion barrier, rather than Drain after sending. Older
// asynchronous snapshots must finish first; a process stop immediately after it keeps the ACK.
int DurableTradeCompletionOrdersSnapshotsAndSurvivesStop() {
  QTemporaryDir dir;
  CHECK(dir.isValid());
  const QString folder = QDir{dir.path()}.filePath("new-parent/mailbox");
  BloodborneCoop::TradeDesk desk;
  desk.InvasionVictory("red", 9, "host", 48, true);
  const auto pending = desk.Stored();
  CHECK(pending.next_id() == 2 && pending.grants_size() == 1);
  WorldWriter writer;
  for (int at = 0; at < 32; ++at) writer.SaveTrades(folder, pending);
  CHECK(writer.SaveTradesDurable(folder, pending));
  CHECK(WorldStore{folder}.LoadTrades()->grants_size() == 1); // no Drain
  desk.Granted("red", BloodborneCoop::InvasionRewardGrantBit | 1);
  CHECK(writer.SaveTradesDurable(folder, desk.Stored()));
  writer.Abandon(); // no final drain can repair ordering after this point
  const auto acked = WorldStore{folder}.LoadTrades();
  CHECK(acked && acked->next_id() == 2 && acked->grants_size() == 0);
  CHECK(!writer.SaveTradesDurable(folder, pending));
  return 0;
}

int TradeWriteFaultsLeaveACompleteRestartableLedger() {
  using Point = WorldStore::TradesWritePoint;
  for (const auto point : {Point::BeforeFileSync, Point::BeforeRename,
                           Point::AfterRename, Point::AfterDirectorySync}) {
    QTemporaryDir dir;
    CHECK(dir.isValid());
    WorldStore store{QDir{dir.path()}.filePath("mailbox")};
    BloodborneCoop::TradeDesk desk;
    CHECK(store.SaveTrades(desk.Stored()));
    desk.InvasionVictory("red", 9, "host", 48, true);
    WorldWriter writer;
    CHECK(!writer.SaveTradesDurable(store.Folder(), desk.Stored(),
        [point](Point at) { return at != point; }));
    writer.Abandon();
    const auto restored = store.LoadTrades();
    CHECK(restored.has_value());
    const bool replaced = point == Point::AfterRename || point == Point::AfterDirectorySync;
    CHECK(restored->grants_size() == (replaced ? 1 : 0));
    CHECK(restored->next_id() == (replaced ? 2 : 1));
    if (replaced) {
      CHECK(restored->grants(0).grant().id() == (BloodborneCoop::InvasionRewardGrantBit | 1));
      BloodborneCoop::TradeDesk next;
      next.Restore(*restored, 0);
      const auto payment = next.InvasionVictory("red", 10, "host", 48);
      CHECK(payment.sends.front().envelope.trade_grant().id() ==
            (BloodborneCoop::InvasionRewardGrantBit | 2));
    }
  }
  return 0;
}

// The main thread's time per record: fsynced on it (the old way) and handed to
// the writer.
int TheLinkThreadNoLongerWaitsForTheDisk() {
  QTemporaryDir dir{QDir::currentPath() +
                    "/world-writer-XXXXXX"}; // a real disk
  CHECK(dir.isValid());
  const QString before = QDir{dir.path()}.filePath("before");
  const QString after = QDir{dir.path()}.filePath("after");
  constexpr int Records = 300;
  WorldStore store{before};
  QElapsedTimer timer;
  timer.start();
  for (int number = 0; number < Records; ++number) {
    CHECK(store.Append(Record(static_cast<quint64>(number))));
  }
  const qint64 sync_ns = timer.nsecsElapsed() / Records;

  WorldWriter writer;
  writer.Compact(after, World(0));
  writer.Drain();
  timer.restart();
  for (int number = 0; number < Records; ++number) {
    writer.Append(after, Record(static_cast<quint64>(number)));
  }
  const qint64 queued_ns = timer.nsecsElapsed() / Records;
  timer.restart();
  writer.Drain();
  const qint64 drain_ms = timer.elapsed();
  std::cout << "per record on the link thread: fsynced " << sync_ns / 1000.0
            << " us, queued " << queued_ns / 1000.0 << " us (drain " << drain_ms
            << " ms, " << writer.Commits() << " fsync rounds for " << Records
            << " records)\n";
  CHECK(queued_ns < sync_ns);
  const Read read = Load(after);
  CHECK(read.numbers.size() == Records);
  return 0;
}

} // namespace

int main() {
  const std::vector<std::pair<const char *, int (*)()>> tests{
      {"EachWorldsRecordsAreOnDiskInOrderAfterTheDrain",
       EachWorldsRecordsAreOnDiskInOrderAfterTheDrain},
      {"TheWriterDrainsWhenItGoes", TheWriterDrainsWhenItGoes},
      {"AStopWithoutTheDrainLosesOnlyTheTail",
       AStopWithoutTheDrainLosesOnlyTheTail},
      {"AWorldThatDoesNotWriteKeepsItsJournal",
       AWorldThatDoesNotWriteKeepsItsJournal},
      {"DurableTradeCompletionOrdersSnapshotsAndSurvivesStop",
       DurableTradeCompletionOrdersSnapshotsAndSurvivesStop},
      {"TradeWriteFaultsLeaveACompleteRestartableLedger",
       TradeWriteFaultsLeaveACompleteRestartableLedger},
      {"TheLinkThreadNoLongerWaitsForTheDisk",
       TheLinkThreadNoLongerWaitsForTheDisk},
  };
  int failed = 0;
  for (const auto &[name, test] : tests) {
    const int result = test();
    std::cout << (result == 0 ? "PASS " : "FAIL ") << name << '\n';
    failed += result != 0 ? 1 : 0;
  }
  return failed == 0 ? 0 : 1;
}
