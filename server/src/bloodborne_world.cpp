// SPDX-FileCopyrightText: Copyright 2026 shadNet Project
// SPDX-License-Identifier: GPL-2.0-or-later
#include "bloodborne_world.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <future>
#include <mutex>
#include <set>
#include <thread>
#include <utility>

#include <QCryptographicHash>
#include <QDate>
#include <QDateTime>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QRandomGenerator>
#include <QRegularExpression>
#include <QSaveFile>
#include <QtEndian>
#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <io.h>
#include <windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

#include "coop/coop_messages.h"
#include "coop/packet_routes.h"

namespace BloodborneWorld {

using namespace BloodborneCoop;

namespace {
// The team world written with its backups (save-restore), defined further down.
bool StoreTeamWorld(const QDir& world_dir, const bbcoop::TeamWorld& world, const QDate& today);
} // namespace

namespace {

constexpr quint32 StoreVersion = 1;
constexpr int Backups = 3;
constexpr int DatedBackups = 7; // phase 11: a week of daily copies
const QString WorldFile = QStringLiteral("world.pb");
const QString JournalFile = QStringLiteral("journal.pb");
const QString TradesFile = QStringLiteral("trades.pb");

std::optional<bbcoop::StoredWorld> ReadWorld(const QString& path) {
    QFile file{path};
    if (!file.open(QIODevice::ReadOnly)) {
        return std::nullopt;
    }
    const QByteArray bytes = file.readAll();
    bbcoop::StoredWorld world;
    if (!world.ParseFromArray(bytes.constData(), static_cast<int>(bytes.size())) ||
        world.version() != StoreVersion || world.world() == 0) {
        return std::nullopt;
    }
    return world;
}

QByteArray Serialized(const google::protobuf::MessageLite& message) {
    QByteArray bytes(static_cast<qsizetype>(message.ByteSizeLong()), Qt::Uninitialized);
    if (!bytes.isEmpty()) {
        message.SerializeToArray(bytes.data(), static_cast<int>(bytes.size()));
    }
    return bytes;
}

// A journal record as the journal keeps it: its size (u32 little-endian), then the record.
QByteArray Framed(const bbcoop::JournalRecord& record) {
    const QByteArray bytes = Serialized(record);
    QByteArray framed(4, Qt::Uninitialized);
    qToLittleEndian<quint32>(static_cast<quint32>(bytes.size()), framed.data());
    return framed + bytes;
}

QString Hex(quint64 value) {
    return QStringLiteral("%1").arg(value, 0, 16);
}

// Phase 11: what was written reaches the disk, not only the system's cache (a power cut or a
// crash of the PC keeps it).
bool SyncToDisk(QFileDevice& file) {
    if (!file.flush()) {
        return false;
    }
#ifdef Q_OS_WIN
    const auto handle = reinterpret_cast<HANDLE>(_get_osfhandle(file.handle()));
    return handle != INVALID_HANDLE_VALUE && FlushFileBuffers(handle) != 0;
#else
    return fsync(file.handle()) == 0;
#endif
}


// A successful rename is not yet a durable directory entry. Sync both the mailbox directory
// and its parent: the first reward may have just created the mailbox directory itself.
bool SyncDirectory(const QString& path) {
#ifdef Q_OS_WIN
    const HANDLE handle = CreateFileW(reinterpret_cast<const wchar_t*>(path.utf16()),
        GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
        OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (handle == INVALID_HANDLE_VALUE) return false;
    const bool ok = FlushFileBuffers(handle) != 0;
    CloseHandle(handle);
    return ok;
#else
    const int fd = open(QFile::encodeName(path).constData(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0) return false;
    const bool ok = fsync(fd) == 0;
    close(fd);
    return ok;
#endif
}

} // namespace

QString PartyFolderName(const QByteArray& party) {
    return QString::fromLatin1(
        QCryptographicHash::hash(party, QCryptographicHash::Sha256).toHex().left(32));
}

WorldStore::WorldStore(QString folder) : folder{std::move(folder)} {}

std::optional<WorldStore::Loaded> WorldStore::Load(QStringList& log) const {
    const QDir dir{folder};
    std::optional<bbcoop::StoredWorld> world = ReadWorld(dir.filePath(WorldFile));
    for (int backup = 1; !world && backup <= Backups; ++backup) {
        const QString path = dir.filePath(QStringLiteral("%1.%2").arg(WorldFile).arg(backup));
        if (!QFileInfo::exists(path)) {
            continue;
        }
        world = ReadWorld(path);
        if (world) {
            log.append(QStringLiteral("world store %1: world.pb does not read, took backup %2")
                           .arg(folder)
                           .arg(backup));
        }
    }
    if (!world) {
        return std::nullopt;
    }
    Loaded loaded{*world, {}};
    QFile journal{dir.filePath(JournalFile)};
    if (!journal.open(QIODevice::ReadOnly)) {
        return loaded;
    }
    const QByteArray bytes = journal.readAll();
    qsizetype at = 0;
    while (bytes.size() - at >= 4) {
        const quint32 size = qFromLittleEndian<quint32>(bytes.constData() + at);
        if (bytes.size() - at - 4 < static_cast<qsizetype>(size)) {
            loaded.journal_cut = true;
            log.append(QStringLiteral("world store %1: the journal ends in a cut record (%2 "
                                      "bytes left), read up to it")
                           .arg(folder)
                           .arg(bytes.size() - at));
            break;
        }
        bbcoop::JournalRecord record;
        if (!record.ParseFromArray(bytes.constData() + at + 4, static_cast<int>(size))) {
            loaded.journal_cut = true;
            log.append(QStringLiteral("world store %1: a journal record does not read, stopped "
                                      "there")
                           .arg(folder));
            break;
        }
        at += 4 + static_cast<qsizetype>(size);
        if (record.number() >= world->journal_from()) {
            loaded.journal.append(std::move(record));
        }
    }
    if (at < bytes.size()) {
        loaded.journal_cut = true;
    }
    return loaded;
}

bool WorldStore::Append(const bbcoop::JournalRecord& record) {
    QDir{}.mkpath(folder);
    QFile journal{QDir{folder}.filePath(JournalFile)};
    if (!journal.open(QIODevice::WriteOnly | QIODevice::Append)) {
        return false;
    }
    const QByteArray bytes = Framed(record);
    return journal.write(bytes) == bytes.size() && SyncToDisk(journal);
}

bool WorldStore::Compact(const bbcoop::StoredWorld& world, const QDate& today) {
    QDir{}.mkpath(folder);
    const QDir dir{folder};
    const QString path = dir.filePath(WorldFile);
    if (QFileInfo::exists(path)) {
        const auto backup = [&](int number) {
            return dir.filePath(QStringLiteral("%1.%2").arg(WorldFile).arg(number));
        };
        QFile::remove(backup(Backups));
        for (int number = Backups - 1; number >= 1; --number) {
            QFile::rename(backup(number), backup(number + 1));
        }
        QFile::copy(path, backup(1));
    }
    QSaveFile file{path};
    if (!file.open(QIODevice::WriteOnly)) {
        return false;
    }
    file.write(Serialized(world));
    if (!file.commit()) {
        return false;
    }
    // The day's copy, once a day; the oldest beyond a week go.
    const QDir backups{dir.filePath(QStringLiteral("backups"))};
    const QString dated = backups.filePath(
        QStringLiteral("world-%1.pb").arg(today.toString(QStringLiteral("yyyyMMdd"))));
    if (!QFileInfo::exists(dated)) {
        QDir{}.mkpath(backups.path());
        QFile::copy(path, dated);
        QStringList days =
            backups.entryList({QStringLiteral("world-*.pb")}, QDir::Files, QDir::Name);
        while (days.size() > DatedBackups) {
            QFile::remove(backups.filePath(days.takeFirst()));
        }
    }
    // The journal's records are in the world now (journal_from); a crash before this leaves them
    // to be skipped on the next load.
    QFile journal{dir.filePath(JournalFile)};
    return !journal.exists() || journal.resize(0);
}

std::optional<bbcoop::StoredTrades> WorldStore::LoadTrades() const {
    QFile file{QDir{folder}.filePath(TradesFile)};
    if (!QFileInfo{file}.isFile() || !file.open(QIODevice::ReadOnly)) {
        return std::nullopt;
    }
    const QByteArray bytes = file.readAll();
    bbcoop::StoredTrades trades;
    if (file.error() != QFileDevice::NoError ||
        !trades.ParseFromArray(bytes.constData(), static_cast<int>(bytes.size())) ||
        trades.next_id() == 0 || trades.next_id() >= InvasionRewardGrantBit) {
        return std::nullopt;
    }
    return trades;
}

bool WorldStore::SaveTrades(const bbcoop::StoredTrades& trades, const TradesWriteCheck& check) {
    const auto proceed = [&](TradesWritePoint point) { return !check || check(point); };
    if (!QDir{}.mkpath(folder)) return false;
    QSaveFile file{QDir{folder}.filePath(TradesFile)};
    file.setDirectWriteFallback(false);
    const QByteArray bytes = Serialized(trades);
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() ||
        !proceed(TradesWritePoint::BeforeFileSync) || !SyncToDisk(file) ||
        !proceed(TradesWritePoint::BeforeRename) || !file.commit()) {
        return false;
    }
    if (!proceed(TradesWritePoint::AfterRename)) return false;
    // Include ancestors that mkpath (or an older queued world snapshot) just created.
    QString directory = QDir{folder}.absolutePath();
    for (;;) {
        if (!SyncDirectory(directory)) return false;
        const QString parent = QFileInfo{directory}.dir().absolutePath();
        if (parent == directory) break;
        directory = parent;
    }
    return proceed(TradesWritePoint::AfterDirectorySync);
}

struct WorldWriter::State {
    using Task = std::function<void(State&)>;
    using Clock = std::chrono::steady_clock;
    static constexpr int LogLines = 100; // between two TakeLog: a full disk says it once a record

    qint64 commit_ms;
    int commit_records;

    mutable std::mutex mutex;
    std::condition_variable wake;    // the thread: tasks, a drain or the end
    std::condition_variable drained; // Drain
    std::deque<Task> tasks;
    std::multiset<QString> moving;
    std::multiset<QString> pending; // the folders with tasks queued (Pending)
    bool running = false;           // the thread has tasks out of the queue
    bool stop = false;
    quint64 drains_wanted = 0;
    quint64 drains_done = 0;
    QStringList log;
    int log_dropped = 0;
    std::atomic<bool> abandon{false};
    std::atomic<quint64> synced{0};
    std::atomic<quint64> commits{0};

    // The thread's own: the journals written to since the last commit.
    std::map<QString, std::unique_ptr<QFile>> journals;
    int unsynced = 0;
    Clock::time_point first_unsynced;

    std::thread thread;

    void Log(QString line) {
        const std::lock_guard lock{mutex};
        if (log.size() < LogLines) {
            log.append(std::move(line));
        } else {
            ++log_dropped;
        }
    }

    // A task that writes in `folder`: Pending until it ran.
    void Push(const QString& folder, Task task) {
        {
            const std::lock_guard lock{mutex};
            pending.insert(folder);
            tasks.push_back([folder, task = std::move(task)](State& s) {
                task(s);
                const std::lock_guard lock{s.mutex};
                s.pending.erase(s.pending.find(folder));
            });
        }
        wake.notify_one();
    }

    // The journals written to reach the disk.
    void Commit() {
        if (unsynced == 0 || abandon) {
            return;
        }
        for (auto& [folder, file] : journals) {
            if (!SyncToDisk(*file)) {
                Log(QStringLiteral("world store %1: the journal could not be synced").arg(folder));
            }
        }
        journals.clear();
        synced += static_cast<quint64>(unsynced);
        ++commits;
        unsynced = 0;
    }

    void Append(const QString& folder, const bbcoop::JournalRecord& record) {
        std::unique_ptr<QFile>& file = journals[folder];
        if (!file) {
            QDir{}.mkpath(folder);
            file = std::make_unique<QFile>(QDir{folder}.filePath(JournalFile));
            if (!file->open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Unbuffered)) {
                Log(QStringLiteral("world store %1: the journal does not open (%2)")
                        .arg(folder, file->errorString()));
                journals.erase(folder);
                return;
            }
        }
        // One write for the record: a crash cuts at most the last one. A write that fails is
        // taken back, so the records after it are not behind a cut one.
        const QByteArray bytes = Framed(record);
        const qint64 size = file->size();
        if (file->write(bytes) != bytes.size()) {
            Log(QStringLiteral("world store %1: a journal record could not be written (%2)")
                    .arg(folder, file->errorString()));
            file->resize(size);
            return;
        }
        if (unsynced++ == 0) {
            first_unsynced = Clock::now();
        }
        if (unsynced >= commit_records) {
            Commit();
        }
    }

    void Run() {
        std::unique_lock lock{mutex};
        while (!abandon) {
            if (!tasks.empty()) {
                std::deque<Task> batch;
                batch.swap(tasks);
                running = true;
                lock.unlock();
                for (Task& task : batch) {
                    if (abandon) {
                        return;
                    }
                    task(*this);
                }
                lock.lock();
                running = false;
                continue;
            }
            const auto deadline = first_unsynced + std::chrono::milliseconds{commit_ms};
            if (unsynced > 0 && (stop || drains_wanted > drains_done || Clock::now() >= deadline)) {
                lock.unlock();
                Commit();
                lock.lock();
                continue;
            }
            if (unsynced == 0) {
                drains_done = drains_wanted;
                drained.notify_all();
                if (stop) {
                    return;
                }
                wake.wait(lock);
            } else {
                wake.wait_until(lock, deadline);
            }
        }
    }
};

WorldWriter::WorldWriter(qint64 commit_ms, int commit_records) : state{std::make_unique<State>()} {
    state->commit_ms = commit_ms;
    state->commit_records = std::max(commit_records, 1);
    state->thread = std::thread{[this] { state->Run(); }};
}

WorldWriter::~WorldWriter() {
    {
        const std::lock_guard lock{state->mutex};
        state->stop = true;
    }
    state->wake.notify_one();
    state->thread.join();
}

void WorldWriter::Append(const QString& folder, bbcoop::JournalRecord record) {
    state->Push(folder,
                [folder, record = std::move(record)](State& s) { s.Append(folder, record); });
}

void WorldWriter::Compact(const QString& folder, bbcoop::StoredWorld world, const QDate& today) {
    state->Push(folder, [folder, world = std::move(world), today](State& s) {
        // Its records on disk before the world that holds them: the journal is cut only after
        // world.pb is written (WorldStore::Compact), and a world that does not write keeps them.
        s.Commit();
        if (!WorldStore{folder}.Compact(world, today)) {
            s.Log(QStringLiteral("world store %1: the world could not write, its journal kept")
                      .arg(folder));
        }
    });
}

void WorldWriter::SaveTrades(const QString& folder, bbcoop::StoredTrades trades) {
    state->Push(folder, [folder, trades = std::move(trades)](State& s) {
        if (!WorldStore{folder}.SaveTrades(trades)) {
            s.Log(QStringLiteral("world store %1: the trades could not write").arg(folder));
        }
    });
}

bool WorldWriter::SaveTradesDurable(const QString& folder, bbcoop::StoredTrades trades,
                                    WorldStore::TradesWriteCheck check) {
    if (state->abandon) return false;
    auto completion = std::make_shared<std::promise<bool>>();
    auto done = completion->get_future();
    state->Push(folder, [folder, trades = std::move(trades), check = std::move(check),
                         completion](State& s) {
        const bool ok = WorldStore{folder}.SaveTrades(trades, check);
        if (!ok) s.Log(QStringLiteral("world store %1: durable trades write failed").arg(folder));
        completion->set_value(ok);
    });
    // The same FIFO queue owns every trades.pb write, including ordinary asynchronous saves.
    // No older queued snapshot can overwrite this acknowledged completion afterwards.
    while (done.wait_for(std::chrono::milliseconds{10}) != std::future_status::ready) {
        if (state->abandon) return false;
    }
    return done.get();
}

void WorldWriter::SaveTeamWorld(const QString& dir, bbcoop::TeamWorld world, const QDate& today) {
    state->Push(dir, [dir, world = std::move(world), today](State& s) {
        QDir{}.mkpath(dir);
        if (!StoreTeamWorld(QDir{dir}, world, today)) {
            s.Log(QStringLiteral("could not write %1")
                      .arg(QDir{dir}.filePath(QStringLiteral("team_world.pb"))));
        }
    });
}

void WorldWriter::Save(const QString& path, QByteArray bytes) {
    state->Push(QFileInfo{path}.path(), [path, bytes = std::move(bytes)](State& s) {
        QDir{}.mkpath(QFileInfo{path}.path());
        QSaveFile file{path};
        if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() ||
            !file.commit()) {
            s.Log(QStringLiteral("could not write %1").arg(path));
        }
    });
}

void WorldWriter::Move(const QString& parent, const QString& name, const QString& to) {
    const QString key = QDir{parent}.filePath(name);
    {
        const std::lock_guard lock{state->mutex};
        state->moving.insert(key);
    }
    state->Push(key, [parent, name, to, key](State& s) {
        s.Commit(); // no journal open in it
        QDir dir{parent};
        if (dir.exists(name)) {
            dir.mkpath(QFileInfo{dir.filePath(to)}.path());
            if (!dir.rename(name, to)) {
                s.Log(QStringLiteral("could not move %1 to %2").arg(key, dir.filePath(to)));
            }
        }
        const std::lock_guard lock{s.mutex};
        s.moving.erase(s.moving.find(key));
    });
}

bool WorldWriter::Moving(const QString& parent, const QString& name) const {
    const std::lock_guard lock{state->mutex};
    return state->moving.contains(QDir{parent}.filePath(name));
}

bool WorldWriter::Pending(const QString& folder) const {
    const std::lock_guard lock{state->mutex};
    return state->pending.contains(folder);
}

void WorldWriter::Drain() {
    std::unique_lock lock{state->mutex};
    const quint64 wanted = ++state->drains_wanted;
    state->wake.notify_one();
    state->drained.wait(lock, [&] { return state->drains_done >= wanted || state->abandon; });
}

void WorldWriter::Abandon() {
    state->abandon = true;
    state->wake.notify_one();
    state->drained.notify_all();
}

QStringList WorldWriter::TakeLog() {
    const std::lock_guard lock{state->mutex};
    QStringList log;
    log.swap(state->log);
    if (state->log_dropped > 0) {
        log.append(QStringLiteral("world store: %1 more lines like these").arg(state->log_dropped));
        state->log_dropped = 0;
    }
    return log;
}

quint64 WorldWriter::Synced() const {
    return state->synced;
}

quint64 WorldWriter::Commits() const {
    return state->commits;
}

Service::Service(QString folder, qint64 heartbeat_ms, int compact_records, qint64 compact_ms)
    : folder{std::move(folder)},
      writer{this->folder.isEmpty() ? nullptr : std::make_unique<WorldWriter>()},
      heartbeat_ms{heartbeat_ms}, compact_records{compact_records}, compact_ms{compact_ms},
      lobby{[] {
          return QStringLiteral("%1")
              .arg(QRandomGenerator::global()->generate64(), 16, 16, QLatin1Char('0'))
              .toStdString();
      }} {}

void Service::Drain() {
    if (writer) {
        writer->Drain();
    }
}

const WorldLogHost* Service::World(const QByteArray& party) const {
    const auto found = worlds.find(FolderOf(party));
    return found != worlds.end() ? &found->second.host : nullptr;
}

std::string Service::Who(const Member& member) const {
    return member.name.empty() ? member.online_id.toStdString() : member.name;
}

Service::Result Service::Load(qint64 now_ms) {
    Result result;
    const auto loaded = [&] {
        result.log.append(QStringLiteral("party worlds: %1, %2 loaded")
                              .arg(folder.isEmpty() ? QStringLiteral("kept in memory only")
                                                    : QStringLiteral("kept in ") + folder)
                              .arg(worlds.size()));
    };
    if (folder.isEmpty()) {
        loaded();
        return result;
    }
    LoadWorldList(result);
    const QDir dir{folder};
    for (const QString& name : dir.entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
        LoadWorld(name, now_ms, result);
    }
    loaded();
    return result;
}

bool Service::LoadWorld(const QString& name, qint64 now_ms, Result& result) {
    // A world put out of memory may still have writes queued (and a new party's folder has none:
    // no wait for the other worlds' writes on the link's thread).
    const QString path = QDir{folder}.filePath(name);
    if (writer && writer->Pending(path)) {
        writer->Drain();
    }
    auto store = std::make_unique<WorldStore>(path);
    const auto loaded = store->Load(result.log);
    if (!loaded) {
        return false;
    }
    PartyWorld& world = worlds[name];
    world.name = name;
    world.world = loaded->world.world();
    world.logs = loaded->world.logs();
    world.last_wipe_ms = loaded->world.last_wipe_ms();
    world.journal_from = loaded->world.journal_from();
    world.records = world.journal_from;
    world.host.Restore(StateOf(loaded->world.host()));
    for (const auto& [member, state] : world.host.Save().members) {
        world.proposed[member] = state.kills;
    }
    for (const bbcoop::JournalRecord& record : loaded->journal) {
        Replay(world, record);
        world.records = std::max<quint64>(world.records, record.number() + 1);
    }
    if (const auto trades = store->LoadTrades()) {
        world.trades.Restore(*trades, now_ms);
        result.log.append(QStringLiteral("party world %1: %2 open trades, %3 grants owed")
                              .arg(name)
                              .arg(world.trades.OpenTrades())
                              .arg(world.trades.PendingGrants()));
    }
    world.store = std::move(store);
    world.compacted_ms = now_ms;
    if (const auto found = kept.find(name); found != kept.end()) {
        world.places = std::move(found->second.places);
        world.boards = std::move(found->second.boards);
        kept.erase(found);
    }
    result.log.append(QStringLiteral("party world %1 loaded: log %2 at seq %3, %4 kills "
                                     "(%5 journal records)")
                          .arg(name, Hex(world.host.Epoch()))
                          .arg(world.host.Last())
                          .arg(WorldLogKillCount(world.host.Kills()))
                          .arg(loaded->journal.size()));
    if (!loaded->journal.isEmpty() || loaded->journal_cut) { // and no record behind a cut one
        Compact(world, now_ms);
    }
    return true;
}

bool Service::RoomFor(const QString& name, qint64 now_ms, Result& result) {
    if (worlds.contains(name) || worlds.size() < limits.party_worlds) {
        return true;
    }
    Collect(now_ms, true, result);
    return worlds.size() < limits.party_worlds;
}

void Service::Collect(qint64 now_ms, bool all, Result& result) {
    if (folder.isEmpty()) {
        return; // its worlds are nowhere else
    }
    std::set<QString> in_use;
    for (const auto& [key, member] : members) {
        if (!member.party.isEmpty()) {
            in_use.insert(member.party);
        }
    }
    // Invasions: the invaded world stays while its invasion runs (the invader itself keeps none).
    for (const auto& [name, invasion] : invasions) {
        in_use.insert(name);
    }
    std::vector<std::pair<qint64, QString>> idle; // since when, which
    for (auto& [name, world] : worlds) {
        if (in_use.contains(name)) {
            world.empty_since_ms.reset();
            continue;
        }
        if (!world.empty_since_ms) {
            world.empty_since_ms = now_ms;
        }
        if (all || now_ms - *world.empty_since_ms >= limits.idle_world_ms) {
            idle.emplace_back(*world.empty_since_ms, name);
        }
    }
    // For room (`all`) only the oldest few: each is written whole on the way out.
    std::sort(idle.begin(), idle.end());
    if (all && idle.size() > CollectAtOnce) {
        idle.resize(CollectAtOnce);
    }
    for (const auto& [since, name] : idle) {
        const auto it = worlds.find(name);
        PartyWorld& world = it->second;
        if (world.records > world.journal_from) {
            Compact(world, now_ms);
        }
        // Where its members stood and its boards are not in the stored world: kept until it is
        // read again, so a guest still resumes where it was.
        if (!world.places.empty()) {
            kept[name] = Kept{std::move(world.places), std::move(world.boards), now_ms};
        }
        result.log.append(QStringLiteral("party world %1: nobody in it, out of memory").arg(name));
        worlds.erase(it);
    }
    // Not for ever, for every server: a world that never comes back would keep it.
    std::erase_if(
        kept, [&](const auto& entry) { return now_ms - entry.second.since_ms >= limits.kept_ms; });
    while (kept.size() > limits.kept_worlds) {
        kept.erase(std::ranges::min_element(
            kept, {}, [](const auto& entry) { return entry.second.since_ms; }));
    }
}

void Service::Drop(quint64 key, const QString& why, Result& result) {
    const auto found = members.find(key);
    if (found == members.end()) {
        return;
    }
    result.log.append(QStringLiteral("party world: dropped %1's stream: %2")
                          .arg(QString::fromStdString(Who(found->second)), why));
    result.log.append(Forget(endpoints.at(key)).log);
}

void Service::Replay(PartyWorld& world, const bbcoop::JournalRecord& record) {
    switch (record.change_case()) {
    case bbcoop::JournalRecord::kStart:
        world.host.Start(record.start().epoch(), record.start().after_wipe());
        world.logs = std::max(world.logs, record.start().logs());
        if (record.start().after_wipe()) {
            world.last_wipe_ms = record.at_ms();
        }
        world.proposed.clear();
        break;
    case bbcoop::JournalRecord::kPropose: {
        const WorldLogProposal proposal =
            ProposalOf(record.propose().kills(), record.propose().member());
        if (!world.host.Propose(proposal).stale) {
            world.proposed[proposal.member] = proposal.kills;
        }
        break;
    }
    case bbcoop::JournalRecord::kDrop:
        world.host.Drop(record.drop().member());
        world.proposed.erase(record.drop().member());
        break;
    case bbcoop::JournalRecord::kShown:
        world.host.Show(record.shown().map(), CharactersOf(record.shown()));
        break;
    case bbcoop::JournalRecord::kFlags:
        world.host.ProposeFlags(FlagsOf(record.flags().flags(), record.flags().member()),
                                record.at_ms());
        break;
    case bbcoop::JournalRecord::kMapState: {
        const std::string& bits = record.map_state().bits();
        world.host.MapState(record.map_state().map(), std::vector<u8>(bits.begin(), bits.end()));
        break;
    }
    case bbcoop::JournalRecord::kReturnPoint:
        world.host.ReturnPoint(record.return_point().point(), record.return_point().map());
        break;
    case bbcoop::JournalRecord::kHp:
        world.host.ProposeHp(HpOf(record.hp().hp(), record.hp().member()));
        break;
    case bbcoop::JournalRecord::kResetMap:
        world.host.ResetMap(record.reset_map().map());
        world.map_reset_ms[record.reset_map().map()] = record.at_ms();
        for (auto& [name, kills] : world.proposed) {
            for (const u32 on : MapsLoadedWith(record.reset_map().map())) {
                kills.erase(on);
            }
        }
        break;
    default:
        break;
    }
}

void Service::Journal(PartyWorld& world, bbcoop::JournalRecord record, qint64 now_ms) {
    record.set_number(world.records++);
    record.set_at_ms(now_ms);
    if (world.store) {
        writer->Append(world.store->Folder(), std::move(record));
    }
    if (world.records - world.journal_from >= static_cast<quint64>(compact_records)) {
        Compact(world, now_ms);
    }
}

void Service::Compact(PartyWorld& world, qint64 now_ms) {
    world.compacted_ms = now_ms;
    world.journal_from = world.records;
    if (!world.store) {
        return;
    }
    bbcoop::StoredWorld stored;
    stored.set_version(StoreVersion);
    stored.set_world(world.world);
    stored.set_logs(world.logs);
    stored.set_last_wipe_ms(world.last_wipe_ms);
    stored.set_journal_from(world.journal_from);
    *stored.mutable_host() = ToMessage(world.host.Save());
    writer->Compact(world.store->Folder(), std::move(stored));
}

Service::PartyWorld& Service::WorldFor(const QString& name, qint64 now_ms, Result& result) {
    const auto found = worlds.find(name);
    if (found != worlds.end()) {
        return found->second;
    }
    // Collected while nobody was in it, or made before this run: as it was.
    if (!folder.isEmpty() && LoadWorld(name, now_ms, result)) {
        return worlds.at(name);
    }
    PartyWorld& world = worlds[name];
    world.name = name;
    world.world = QRandomGenerator::global()->generate() | 1u;
    world.compacted_ms = now_ms;
    if (!folder.isEmpty()) {
        world.store = std::make_unique<WorldStore>(QDir{folder}.filePath(name));
    }
    result.log.append(QStringLiteral("party world %1: a new world %2").arg(name, Hex(world.world)));
    StartLog(world, false, now_ms, result);
    Compact(world, now_ms); // on disk at once: a journal alone is no world
    return world;
}

void Service::StartLog(PartyWorld& world, bool after_wipe, qint64 now_ms, Result& result) {
    const u64 epoch = WorldLogEpoch(world.world, ++world.logs);
    const std::vector<WorldLogEntry> entries = world.host.Start(epoch, after_wipe);
    world.proposed.clear();
    if (after_wipe) {
        world.last_wipe_ms = now_ms;
    }
    bbcoop::JournalRecord record;
    record.mutable_start()->set_epoch(epoch);
    record.mutable_start()->set_after_wipe(after_wipe);
    record.mutable_start()->set_logs(world.logs);
    Journal(world, std::move(record), now_ms);
    result.log.append(QStringLiteral("party world %1: log %2 starts (%3, no kills stand)")
                          .arg(world.name, Hex(epoch), after_wipe ? "a wipe" : "a new session"));
    SendEntries(world, entries, now_ms);
}

void Service::ResetMap(PartyWorld& world, std::uint32_t map, qint64 now_ms) {
    const std::vector<WorldLogEntry> entries = world.host.ResetMap(map);
    for (const std::uint32_t on : MapsLoadedWith(map)) {
        world.map_reset_ms[on] = now_ms;
        for (auto& [name, kills] : world.proposed) {
            kills.erase(on);
        }
    }
    bbcoop::JournalRecord record;
    record.mutable_reset_map()->set_map(map);
    Journal(world, std::move(record), now_ms);
    SendEntries(world, entries, now_ms);
}

void Service::SendTo(Member& member, const bbcoop::Envelope& envelope, qint64 now_ms) {
    member.stream.Send(EncodeEnvelope(envelope), now_ms);
    if (!member.to_flush) {
        member.to_flush = true;
        flush_now.push_back(&member);
    }
}

void Service::SendEntries(PartyWorld& world, const std::vector<WorldLogEntry>& entries,
                          qint64 now_ms) {
    bbcoop::Envelope envelope;
    *envelope.mutable_entries() = ToMessage(world.host.Epoch(), world.host.Last(), entries);
    for (auto& [key, member] : members) {
        if (member.party == world.name) {
            SendTo(member, envelope, now_ms);
        }
    }
    world.heartbeat_ms = now_ms;
}

void Service::OnHello(Member& member, const bbcoop::Hello& hello, qint64 now_ms, Result& result) {
    bbcoop::Envelope answer;
    bbcoop::Welcome& welcome = *answer.mutable_welcome();
    const QByteArray party = QByteArray::fromStdString(hello.party()).trimmed();
    if (hello.protocol() != CoopProtocol) {
        welcome.set_text("co-op protocol " + std::to_string(hello.protocol()) +
                         ", this server speaks " + std::to_string(CoopProtocol));
    } else if (party.indexOf(':') <= 0 || hello.member().empty()) {
        welcome.set_text("no party key or member name");
    } else if (hosted.enabled && !OwnName(member, hello.member())) {
        welcome.set_text("the member name is not this player's");
    } else if (hosted.enabled) {
        // Hosted: only the world the player's team plays.
        const bbcoop::Team* team = lobby.TeamOf(hello.member());
        if (!party.startsWith("world:") || !team ||
            QByteArray::fromStdString(team->party()) != party) {
            welcome.set_text("not in the team that plays this world");
        }
    }
    if (welcome.text().empty() && !RoomFor(FolderOf(party), now_ms, result)) {
        welcome.set_text("the server has no room for another world now");
    }
    if (!welcome.text().empty()) {
        SendTo(member, answer, now_ms);
        result.log.append(
            QStringLiteral("party world: refused %1: %2")
                .arg(QString::fromStdString(Who(member)), QString::fromStdString(welcome.text())));
        return;
    }
    // The same run saying hello again (a guest whose game took the session host's role: phase 7).
    const bool same_run =
        !member.name.empty() && member.name == hello.member() && member.run == hello.run();
    member.party = FolderOf(party);
    member.cut_from.clear();
    const bool team = party.startsWith("world:");
    member.name = hello.member();
    member.run = hello.run();
    member.host = hello.host();
    member.enemy_owner = hello.enemy_owner();
    PartyWorld& world = WorldFor(member.party, now_ms, result);
    if (team && world.team_world_id.empty()) {
        world.team_world_id = party.mid(6).toStdString();
    }
    // Phase 7, host migration: the session's host is whoever hosts its open session. The room's
    // owner passes to a guest when the host's game goes, and that guest's game says hello again,
    // hosting now; a new run saying it hosts (its team's leader, back) is a guest of that one.
    if (member.host && !same_run && world.session && world.session->host() != member.name &&
        SessionHostLinked(world)) {
        member.host = false;
        result.log.append(QStringLiteral("party world %1: %2 says it hosts, but %3 hosts the "
                                         "session: it joins as a guest")
                              .arg(world.name, QString::fromStdString(member.name),
                                   QString::fromStdString(world.session->host())));
    }
    // The host's game started again: the session its last run opened is gone with it.
    if (world.session && world.session->host() == member.name && world.session_run != member.run) {
        CloseSession(world, now_ms, result);
    }
    welcome.set_accepted(true);
    welcome.set_trades(true);
    welcome.set_epoch(world.host.Epoch());
    welcome.set_last(world.host.Last());
    SendTo(member, answer, now_ms);
    bbcoop::Envelope snapshot;
    *snapshot.mutable_snapshot() = SnapshotOf(world.host);
    SendTo(member, snapshot, now_ms);
    // The boards as they are, and the others' portraits.
    SendBoards(world,
               PartyBoards::Maps | PartyBoards::Rests | PartyBoards::TrophyBoard |
                   PartyBoards::VitalsBoard,
               now_ms, &member);
    for (const auto& [name, portrait] : world.portraits) {
        if (name != member.name) {
            bbcoop::Envelope envelope;
            *envelope.mutable_portrait() = portrait;
            SendTo(member, envelope, now_ms);
        }
    }
    SendOwners(world, now_ms, result, &member);
    // The others' loads (the team loading screen).
    for (const auto& [name, load] : world.loads) {
        if (name != member.name) {
            bbcoop::Envelope envelope;
            *envelope.mutable_load_progress() = load;
            SendTo(member, envelope, now_ms);
        }
    }
    // Phase 7: where a guest last stood, for its run to come back there (the host's save is its
    // own place).
    if (const auto place = world.places.find(member.name);
        !member.host && place != world.places.end() &&
        place->second.character == CharacterOf(member.name)) {
        bbcoop::Envelope envelope;
        *envelope.mutable_resume_at() = place->second.at;
        SendTo(member, envelope, now_ms);
    }
    // The host's open session: a guest joins it once it stands in its world.
    if (world.session && world.session->host() != member.name) {
        bbcoop::Envelope envelope;
        *envelope.mutable_team_session() = *world.session;
        SendTo(member, envelope, now_ms);
    }
    // What its trades owe it: grants not yet added, offers waiting for its answer.
    Deliver(world, world.trades.Resume(member.name, now_ms), now_ms, result);
    // The party's last duel (a game coming back mid-duel follows it).
    if (const bbcoop::DuelState duel = world.duels.State(now_ms); duel.duel() != 0) {
        bbcoop::Envelope envelope;
        *envelope.mutable_duel_state() = duel;
        SendTo(member, envelope, now_ms);
    }
    world.team = world.team || team;
    if (world.team) {
        SendTeamWorld(world, member, now_ms);
    }
    result.log.append(QStringLiteral("party world %1: %2 (run %3) follows log %4 at seq %5")
                          .arg(world.name, QString::fromStdString(member.name))
                          .arg(Hex(member.run), Hex(world.host.Epoch()))
                          .arg(world.host.Last()));
}

void Service::OnLifecycle(Member& member, const bbcoop::Lifecycle& lifecycle, qint64 now_ms,
                          Result& result) {
    PartyWorld& world = worlds.at(member.party);
    const QString who = QString::fromStdString(member.name);
    switch (lifecycle.event()) {
    case bbcoop::Lifecycle::WIPE:
        // Invasions: the team wiped with an invader in its world: the invader wins.
        if (const auto invasion = invasions.find(world.name);
            invasion != invasions.end() && invasion->second.joined) {
            EndInvasion(world.name, bbcoop::InvasionEnd::PARTY_WIPED, now_ms, result);
        }
        if (world.last_wipe_ms != 0 && now_ms - world.last_wipe_ms < WorldLogWipeGraceMs) {
            result.log.append(QStringLiteral("party world %1: %2's wipe: the party's wipe log "
                                             "already started")
                                  .arg(world.name, who));
            return;
        }
        // Kaniel 2026-10-16: kills stay while anyone is on their map. A member down alone (its
        // map wiped) while another stands anywhere dies on its own: the party's log stands, but
        // its map starts over when nobody stands there (Kaniel's play test 2026-10-19: the two
        // on Cathedral Ward wiped while krolebord was in the Dream, and the Ward came back half
        // respawned, different for each of them).
        if (world.boards.SomeoneStandsBesides(member.name)) {
            const std::uint32_t map = lifecycle.map();
            if (map == 0 || world.boards.SomeoneStandsOn(MapsLoadedWith(map), member.name)) {
                result.log.append(QStringLiteral("party world %1: %2's wipe is its own (another "
                                                 "member stands%3): the party's log stands")
                                      .arg(world.name, who,
                                           map == 0 ? QString{} : QStringLiteral(" on its map")));
                return;
            }
            if (const auto reset = world.map_reset_ms.find(map);
                reset != world.map_reset_ms.end() && now_ms - reset->second < WorldLogWipeGraceMs) {
                result.log.append(QStringLiteral("party world %1: %2's wipe on %3: that map "
                                                 "already started over")
                                      .arg(world.name, who, Hex(map)));
                return;
            }
            ResetMap(world, map, now_ms);
            result.log.append(QStringLiteral("party world %1: %2's wipe on %3 (another member "
                                             "stands elsewhere): that map starts over")
                                  .arg(world.name, who, Hex(map)));
            return;
        }
        result.log.append(QStringLiteral("party world %1: %2 wiped").arg(world.name, who));
        StartLog(world, true, now_ms, result);
        return;
    case bbcoop::Lifecycle::JOIN:
    case bbcoop::Lifecycle::LEAVE:
    case bbcoop::Lifecycle::NEW_CHARACTER: {
        const QString event =
            QString::fromStdString(bbcoop::Lifecycle::Event_Name(lifecycle.event()));
        if (lifecycle.host()) {
            result.log.append(QStringLiteral("party world %1: the host %2: %3")
                                  .arg(world.name, who, event.toLower()));
            StartLog(world, false, now_ms, result);
            return;
        }
        const std::vector<WorldLogEntry> entries = world.host.Drop(member.name);
        world.proposed.erase(member.name);
        if (lifecycle.event() == bbcoop::Lifecycle::LEAVE) {
            SendBoards(world, world.boards.Forget(member.name), now_ms);
            if (world.owners.Forget(member.name)) {
                SendOwners(world, now_ms, result);
            }
        }
        bbcoop::JournalRecord record;
        record.mutable_drop()->set_member(member.name);
        Journal(world, std::move(record), now_ms);
        result.log.append(QStringLiteral("party world %1: %2: %3, its share goes (%4 entries)")
                              .arg(world.name, who, event.toLower())
                              .arg(entries.size()));
        if (!entries.empty()) {
            SendEntries(world, entries, now_ms);
        }
        return;
    }
    default:
        return;
    }
}

void Service::Take(Member& member, const bbcoop::Envelope& envelope, qint64 now_ms,
                   Result& result) {
    if (envelope.has_hello()) {
        OnHello(member, envelope.hello(), now_ms, result);
        return;
    }
    if (envelope.has_lobby_hello()) {
        OnLobbyHello(member, envelope.lobby_hello(), now_ms, result);
        return;
    }
    if (envelope.has_team_request()) {
        OnTeamRequest(member, envelope.team_request(), now_ms, result);
        return;
    }
    if (envelope.has_invade_request()) {
        OnInvadeRequest(member, envelope.invade_request(), now_ms, result);
        return;
    }
    // Reward mailboxes are keyed by the authenticated link, never a party or client key.
    // Handle their disjoint grant IDs even for a solo invader with no party Hello.
    if (envelope.has_trade_granted() &&
        (envelope.trade_granted().id() & InvasionRewardGrantBit) != 0) {
        if (member.invasion_rewards && !member.lobby.empty()) {
            if (PartyWorld* mailbox = RewardMailbox(member, false, now_ms, result)) {
                TradeDesk staged = mailbox->trades;
                auto ack = staged.Granted(member.lobby, envelope.trade_granted().id());
                DeliverRewards(member, *mailbox, std::move(staged), std::move(ack), now_ms, result);
            }
        }
        return;
    }
    // An invader in a world, or one on its way with no world of its own: in no world here.
    if (member.hostile || (member.party.isEmpty() &&
                           (searches.contains(member.key) || !InvadedBy(member.key).isEmpty()))) {
        TakeHostile(member, envelope, now_ms, result);
        return;
    }
    if (member.party.isEmpty()) {
        return; // nothing before its hello
    }
    PartyWorld& world = worlds.at(member.party);
    switch (envelope.body_case()) {
    case bbcoop::Envelope::kLifecycle:
        OnLifecycle(member, envelope.lifecycle(), now_ms, result);
        break;
    case bbcoop::Envelope::kKills: {
        WorldLogProposal proposal = ProposalOf(envelope.kills(), member.name);
        // Phase 4: a new kill on an area another member runs is not this member's to make (the
        // kills it made there before stand).
        const auto before = world.proposed.find(member.name);
        size_t refused = 0;
        for (auto& [map, handles] : proposal.kills) {
            if (Decides(world, member, map)) {
                continue;
            }
            refused += std::erase_if(handles, [&](u32 handle) {
                return before == world.proposed.end() || !before->second.contains(map) ||
                       !before->second.at(map).contains(handle);
            });
        }
        std::erase_if(proposal.kills, [](const auto& entry) { return entry.second.empty(); });
        if (refused > 0) {
            result.log.append(QStringLiteral("party world %1: %2's %3 new kills are not its to "
                                             "make (another member runs their area)")
                                  .arg(world.name, QString::fromStdString(member.name))
                                  .arg(refused));
        }
        bbcoop::Envelope taken_envelope = envelope;
        *taken_envelope.mutable_kills() = ToMessage(proposal);
        const WorldLogHost::Taken taken = world.host.Propose(proposal);
        if (taken.stale) {
            break;
        }
        const auto proposed = world.proposed.find(member.name);
        if (proposed == world.proposed.end() || proposed->second != proposal.kills) {
            world.proposed[member.name] = proposal.kills;
            bbcoop::JournalRecord record;
            record.mutable_propose()->set_member(member.name);
            *record.mutable_propose()->mutable_kills() = taken_envelope.kills();
            Journal(world, std::move(record), now_ms);
        }
        if (!taken.entries.empty()) {
            QStringList kills;
            for (const WorldLogEntry& entry : taken.entries) {
                kills.append(QStringLiteral("%1 %2 on %3")
                                 .arg(entry.kind == WorldLogKind::Kill ? "kill" : "unkill",
                                      Hex(entry.subject), Hex(entry.map)));
            }
            result.log.append(QStringLiteral("party world %1: %2's kills: %3")
                                  .arg(world.name, QString::fromStdString(member.name),
                                       kills.join(QStringLiteral(", "))));
            SendEntries(world, taken.entries, now_ms);
        }
        break;
    }
    case bbcoop::Envelope::kTeamWorld:
        TakeTeamWorld(member, world, envelope.team_world(), now_ms, result);
        break;
    case bbcoop::Envelope::kEnemyHit: {
        bbcoop::Envelope relayed = envelope;
        relayed.mutable_enemy_hit()->set_member(member.name);
        SendToOthers(world, member, relayed, now_ms);
        break;
    }
    case bbcoop::Envelope::kAltarRecord: {
        bbcoop::Envelope relayed = envelope;
        relayed.mutable_altar_record()->set_member(member.name);
        result.log.append(QStringLiteral("party world %1: %2's record of altar %3 to the others")
                              .arg(world.name, QString::fromStdString(member.name))
                              .arg(envelope.altar_record().altar()));
        SendToOthers(world, member, relayed, now_ms);
        break;
    }
    case bbcoop::Envelope::kPing: {
        // Kaniel 2026-10-24: a team ping, at once to the others, under the sender's name; nothing
        // is kept.
        bbcoop::Envelope relayed = envelope;
        relayed.mutable_ping()->set_member(member.name);
        SendToOthers(world, member, relayed, now_ms);
        break;
    }
    case bbcoop::Envelope::kTeamTravel: {
        // Kaniel 2026-10-25 (travel together): a team trip's ask, answers and go, at once to the
        // others under the sender's name; nothing is kept.
        bbcoop::Envelope relayed = envelope;
        relayed.mutable_team_travel()->set_member(member.name);
        result.log.append(QStringLiteral("party world %1: %2's team trip %3 (%4) to the others")
                              .arg(world.name, QString::fromStdString(member.name))
                              .arg(envelope.team_travel().id())
                              .arg(QString::fromStdString(
                                  bbcoop::TeamTravel::Step_Name(envelope.team_travel().step()))));
        SendToOthers(world, member, relayed, now_ms);
        break;
    }
    case bbcoop::Envelope::kLoadProgress: {
        bbcoop::Envelope relayed = envelope;
        relayed.mutable_load_progress()->set_member(member.name);
        world.loads[member.name] = relayed.load_progress();
        SendToOthers(world, member, relayed, now_ms);
        break;
    }
    case bbcoop::Envelope::kTeamSession: {
        // Only the host's session is the team's to join (a guest's says nothing).
        if (!member.host) {
            break;
        }
        // A former host's close says nothing about the session another member hosts now.
        if (envelope.team_session().join().empty() && world.session &&
            world.session->host() != member.name) {
            break;
        }
        bbcoop::Envelope relayed = envelope;
        relayed.mutable_team_session()->set_host(member.name);
        if (relayed.team_session().join().empty()) {
            world.session.reset();
        } else {
            world.session = relayed.team_session();
            world.session_run = member.run;
        }
        result.log.append(QStringLiteral("party world %1: %2's session %3 %4")
                              .arg(world.name, QString::fromStdString(member.name))
                              .arg(relayed.team_session().session())
                              .arg(world.session ? QStringLiteral("open (%1 bytes)")
                                                       .arg(relayed.team_session().join().size())
                                                 : QStringLiteral("closed")));
        SendToOthers(world, member, relayed, now_ms);
        if (!world.session) {
            EndInvasion(world.name, bbcoop::InvasionEnd::HOST_LEFT, now_ms, result);
        }
        break;
    }
    case bbcoop::Envelope::kShown:
    case bbcoop::Envelope::kFlags:
    case bbcoop::Envelope::kMapState:
    case bbcoop::Envelope::kReturnPoint:
    case bbcoop::Envelope::kHp:
        TakeWorld(member, world, envelope, now_ms, result);
        break;
    case bbcoop::Envelope::kMap:
    case bbcoop::Envelope::kRest:
    case bbcoop::Envelope::kTrophies:
    case bbcoop::Envelope::kEchoes:
    case bbcoop::Envelope::kVitals:
    case bbcoop::Envelope::kPortrait:
        TakeBoard(member, world, envelope, now_ms, result);
        break;
    case bbcoop::Envelope::kTradeOffer:
    case bbcoop::Envelope::kTradeAnswer:
    case bbcoop::Envelope::kTradeCancel:
    case bbcoop::Envelope::kTradeGranted:
        TakeTrade(member, world, envelope, now_ms, result);
        break;
    case bbcoop::Envelope::kDuelRequest:
        TakeDuel(member, world, envelope, now_ms, result);
        break;
    case bbcoop::Envelope::kSnapshotRequest: {
        bbcoop::Envelope snapshot;
        *snapshot.mutable_snapshot() = SnapshotOf(world.host);
        SendTo(member, snapshot, now_ms);
        break;
    }
    default:
        break; // the games' messages only
    }
}

void Service::TakeWorld(Member& member, PartyWorld& world, const bbcoop::Envelope& envelope,
                        qint64 now_ms, Result& result) {
    bbcoop::JournalRecord record;
    std::vector<WorldLogEntry> entries;
    bool changed = false;
    switch (envelope.body_case()) {
    case bbcoop::Envelope::kShown:
        if (!member.host) {
            return; // only the host's world is the party's (a guest's follows it)
        }
        entries = world.host.Show(envelope.shown().map(), CharactersOf(envelope.shown()));
        *record.mutable_shown() = envelope.shown();
        changed = !entries.empty();
        break;
    case bbcoop::Envelope::kMapState: {
        if (!member.host) {
            return;
        }
        const std::string& bits = envelope.map_state().bits();
        entries = world.host.MapState(envelope.map_state().map(),
                                      std::vector<u8>(bits.begin(), bits.end()));
        *record.mutable_map_state() = envelope.map_state();
        changed = !entries.empty();
        break;
    }
    case bbcoop::Envelope::kReturnPoint:
        if (!member.host) {
            return;
        }
        entries =
            world.host.ReturnPoint(envelope.return_point().point(), envelope.return_point().map());
        *record.mutable_return_point() = envelope.return_point();
        changed = !entries.empty();
        break;
    case bbcoop::Envelope::kFlags: {
        const WorldLogHost::Taken taken =
            world.host.ProposeFlags(FlagsOf(envelope.flags(), member.name), now_ms);
        if (taken.stale) {
            return;
        }
        entries = taken.entries;
        record.mutable_flags()->set_member(member.name);
        *record.mutable_flags()->mutable_flags() = envelope.flags();
        // Resends of events taken already change nothing (a restart that loses a repeat's number
        // takes the repeat again: a flag set twice).
        changed = !entries.empty();
        if (!entries.empty()) {
            QStringList flags;
            for (const WorldLogEntry& entry : entries) {
                flags.append(QStringLiteral("%1 %2")
                                 .arg(static_cast<qint32>(entry.subject))
                                 .arg(entry.kind == WorldLogKind::FlagSet ? "on" : "off"));
            }
            result.log.append(QStringLiteral("party world %1: %2's flags: %3")
                                  .arg(world.name, QString::fromStdString(member.name),
                                       flags.join(QStringLiteral(", "))));
        }
        break;
    }
    case bbcoop::Envelope::kHp: {
        // Another member runs that area: its HP is the owner's to report (phase 8: no guest runs
        // an enemy on a map that has an owner).
        const bbcoop::HpProposal& hp = envelope.hp();
        if (!Decides(world, member, hp.map())) {
            return;
        }
        const WorldLogHost::Taken taken = world.host.ProposeHp(HpOf(hp, member.name));
        if (taken.stale) {
            return;
        }
        entries = taken.entries;
        record.mutable_hp()->set_member(member.name);
        *record.mutable_hp()->mutable_hp() = envelope.hp();
        changed = !entries.empty();
        break;
    }
    default:
        return;
    }
    if (changed) {
        Journal(world, std::move(record), now_ms);
    }
    if (!entries.empty()) {
        SendEntries(world, entries, now_ms);
    }
}

void Service::TakeBoard(Member& member, PartyWorld& world, const bbcoop::Envelope& envelope,
                        qint64 now_ms, Result& result) {
    // A member's part changes its board for everyone; else the board goes back to the member
    // alone (as the HTTP boards answered every post): its copy stays fresh.
    const auto answer = [&](unsigned changed, unsigned board) {
        if (changed != PartyBoards::None) {
            SendBoards(world, changed, now_ms);
        } else {
            SendBoards(world, board, now_ms, &member);
        }
    };
    switch (envelope.body_case()) {
    case bbcoop::Envelope::kMap: {
        member.map = envelope.map().map();
        // A load (map 0) is no place on the boards, only for the enemy owners.
        if (envelope.map().map() != 0) {
            answer(world.boards.Map(member.name, envelope.map().map(), now_ms), PartyBoards::Maps);
        }
        // The owners go with every answer to a member's map (to everyone when they changed).
        const bool changed =
            world.owners.Place(member.name, envelope.map().map(), now_ms, member.host);
        SendOwners(world, now_ms, result, changed ? nullptr : &member);
        return;
    }
    case bbcoop::Envelope::kRest:
        answer(world.boards.Rest(member.name, envelope.rest(), now_ms), PartyBoards::Rests);
        return;
    case bbcoop::Envelope::kTrophies:
        answer(world.boards.Trophies(envelope.trophies()), PartyBoards::TrophyBoard);
        return;
    case bbcoop::Envelope::kVitals: {
        member.level = envelope.vitals().level();
        member.covenant_effect = NormalizeCovenantEffect(envelope.vitals().covenant_effect());
        member.boss = envelope.vitals().boss();
        std::uint32_t wipe_before = 0;
        if (invasions.contains(world.name)) {
            const bbcoop::VitalsBoard board = world.boards.VitalsMessage();
            for (const auto& wipe : board.wipes()) {
                wipe_before = std::max(wipe_before, wipe.id());
            }
        }
        if (const bbcoop::Vitals& vitals = envelope.vitals();
            vitals.map() != 0 && (vitals.vital() == 1 || vitals.vital() == 2) &&
            vitals.has_at()) { // up or down where it stands
            PartyWorld::Place& place = world.places[member.name];
            place.at.set_map(vitals.map());
            *place.at.mutable_at() = vitals.at();
            place.character = CharacterOf(member.name);
        }
        answer(world.boards.Vitals(member.name, envelope.vitals(), now_ms),
               PartyBoards::VitalsBoard);
        CheckInvasion(world, member, envelope.vitals(), wipe_before, now_ms, result);
        // Logged when someone's state or the wipes change, not for every HP change.
        static constexpr const char* Vitals[] = {"off", "up", "down", "dead", "spectate"};
        const bbcoop::VitalsBoard board = world.boards.VitalsMessage();
        QStringList parts;
        for (const auto& entry : board.members()) {
            parts.append(QStringLiteral("%1 %2 %3")
                             .arg(QString::fromStdString(entry.member()))
                             .arg(Vitals[std::min<u32>(entry.vitals().vital(), 4)])
                             .arg(Hex(entry.vitals().map())));
        }
        for (const auto& wipe : board.wipes()) {
            QStringList members;
            for (const std::string& name : wipe.members()) {
                members.append(QString::fromStdString(name));
            }
            parts.append(QStringLiteral("wipe %1 of %2 (%3)")
                             .arg(wipe.id())
                             .arg(Hex(wipe.map()), members.join(' ')));
        }
        const QString logged = parts.join(QStringLiteral(", "));
        if (logged != world.vitals_logged) {
            world.vitals_logged = logged;
            result.log.append(QStringLiteral("party world %1: vitals: %2").arg(world.name, logged));
        }
        return;
    }
    case bbcoop::Envelope::kEchoes:
        // Each kill's echoes to every other member once (the stream delivers them once).
        if (envelope.echoes().kills_size() > 0) {
            SendToOthers(world, member, envelope, now_ms);
        }
        return;
    case bbcoop::Envelope::kPortrait: {
        bbcoop::Envelope relayed = envelope;
        relayed.mutable_portrait()->set_member(member.name);
        world.portraits[member.name] = relayed.portrait();
        SendToOthers(world, member, relayed, now_ms);
        return;
    }
    default:
        return;
    }
}

void Service::SendBoards(PartyWorld& world, unsigned changed, qint64 now_ms, Member* only) {
    std::vector<bbcoop::Envelope> boards;
    if ((changed & PartyBoards::Maps) != 0) {
        *boards.emplace_back().mutable_map_board() = world.boards.MapMessage();
    }
    if ((changed & PartyBoards::Rests) != 0) {
        *boards.emplace_back().mutable_rest_board() = world.boards.RestMessage();
    }
    if ((changed & PartyBoards::TrophyBoard) != 0) {
        *boards.emplace_back().mutable_trophies() = world.boards.TrophyMessage();
    }
    if ((changed & PartyBoards::VitalsBoard) != 0) {
        *boards.emplace_back().mutable_vitals_board() = VitalsBoardOf(world);
    }
    for (const bbcoop::Envelope& board : boards) {
        if (only != nullptr) {
            SendTo(*only, board, now_ms);
            continue;
        }
        for (auto& [key, member] : members) {
            if (member.party == world.name) {
                SendTo(member, board, now_ms);
            }
        }
    }
}

bbcoop::VitalsBoard Service::VitalsBoardOf(const PartyWorld& world) const {
    bbcoop::VitalsBoard board = world.boards.VitalsMessage();
    if (const auto invasion = invasions.find(world.name);
        invasion != invasions.end() && invasion->second.joined && invasion->second.vitals) {
        bbcoop::VitalsBoard::Member& line = *board.add_members();
        line.set_member(invasion->second.invader_name);
        *line.mutable_vitals() = *invasion->second.vitals;
        line.set_hostile(true);
    }
    return board;
}

void Service::SendOwners(PartyWorld& world, qint64 now_ms, Result& result, Member* only) {
    bbcoop::Envelope envelope;
    *envelope.mutable_enemy_owners() = world.owners.Message();
    QStringList owners;
    for (const bbcoop::EnemyOwner& owner : envelope.enemy_owners().owners()) {
        owners.append(QStringLiteral("%1 %2 (epoch %3)")
                          .arg(Hex(owner.map()), QString::fromStdString(owner.member()))
                          .arg(owner.epoch()));
    }
    const QString logged = owners.isEmpty() ? QStringLiteral("none") : owners.join(", ");
    if (logged != world.owners_logged) {
        world.owners_logged = logged;
        result.log.append(
            QStringLiteral("party world %1: enemy owners: %2").arg(world.name, logged));
    }
    if (only != nullptr) {
        SendTo(*only, envelope, now_ms);
        return;
    }
    for (auto& [key, member] : members) {
        if (member.party == world.name) {
            SendTo(member, envelope, now_ms);
        }
    }
    // Invasions: the invader follows the owners too (it travels and sees the enemies by them).
    if (const auto invasion = invasions.find(world.name);
        invasion != invasions.end() && invasion->second.joined) {
        if (const auto invader = members.find(invasion->second.invader); invader != members.end()) {
            SendTo(invader->second, envelope, now_ms);
        }
    }
}

bool Service::Decides(const PartyWorld& world, const Member& member, u32 map) const {
    if (!member.enemy_owner) {
        return true;
    }
    const std::string owner = world.owners.OwnerOf(map);
    return owner.empty() || owner == member.name;
}

void Service::TakeTrade(Member& member, PartyWorld& world, const bbcoop::Envelope& envelope,
                        qint64 now_ms, Result& result) {
    switch (envelope.body_case()) {
    case bbcoop::Envelope::kTradeOffer: {
        // Hosted: only between members of one started team (a stranger in the party by some other
        // way is nobody to trade with).
        const bbcoop::Team* team = hosted.enabled ? lobby.TeamOf(member.name) : nullptr;
        std::set<std::string> online;
        for (const auto& [key, other] : members) {
            if (other.party != world.name) {
                continue;
            }
            if (hosted.enabled && (!team || !team->started() || lobby.TeamOf(other.name) != team)) {
                continue;
            }
            online.insert(other.name);
        }
        Deliver(world, world.trades.Offer(member.name, envelope.trade_offer(), online, now_ms),
                now_ms, result);
        return;
    }
    case bbcoop::Envelope::kTradeAnswer:
        Deliver(world, world.trades.Answer(member.name, envelope.trade_answer()), now_ms, result);
        return;
    case bbcoop::Envelope::kTradeCancel:
        Deliver(world, world.trades.Cancel(member.name, envelope.trade_cancel().id()), now_ms,
                result);
        return;
    case bbcoop::Envelope::kTradeGranted:
        Deliver(world, world.trades.Granted(member.name, envelope.trade_granted().id()), now_ms,
                result);
        return;
    default:
        return;
    }
}

void Service::Deliver(PartyWorld& world, TradeDesk::Result trades, qint64 now_ms, Result& result) {
    for (const TradeDesk::Send& send : trades.sends) {
        for (auto& [key, member] : members) {
            if (member.party == world.name && member.name == send.to) {
                SendTo(member, send.envelope, now_ms);
            }
        }
    }
    for (const std::string& line : trades.log) {
        result.log.append(
            QStringLiteral("party world %1: %2").arg(world.name, QString::fromStdString(line)));
    }
    if (trades.changed && world.store) {
        writer->SaveTrades(world.store->Folder(), world.trades.Stored());
    }
}

std::vector<DuelMember> Service::DuelMembers(const PartyWorld& world) const {
    std::vector<DuelMember> online;
    for (const auto& [key, member] : members) {
        if (member.party == world.name) {
            online.push_back(DuelMember{member.name, member.map});
        }
    }
    return online;
}

void Service::TakeDuel(Member& member, PartyWorld& world, const bbcoop::Envelope& envelope,
                       qint64 now_ms, Result& result) {
    const DuelReferee::Result duel =
        world.duels.Request(member.name, envelope.duel_request(), DuelMembers(world), now_ms);
    if (!duel.refused.empty()) {
        bbcoop::Envelope refused;
        *refused.mutable_duel_state() = world.duels.State(now_ms);
        refused.mutable_duel_state()->set_refused(duel.refused);
        SendTo(member, refused, now_ms);
        result.log.append(QStringLiteral("party world %1: %2's duel request refused: %3")
                              .arg(world.name, QString::fromStdString(member.name),
                                   QString::fromStdString(duel.refused)));
    }
    SettleDuel(world, duel, now_ms, result);
}

void Service::SettleDuel(PartyWorld& world, const DuelReferee::Result& duel, qint64 now_ms,
                         Result& result) {
    for (const std::string& line : duel.log) {
        result.log.append(
            QStringLiteral("party world %1: %2").arg(world.name, QString::fromStdString(line)));
    }
    if (!duel.changed) {
        return;
    }
    bbcoop::Envelope state;
    *state.mutable_duel_state() = world.duels.State(now_ms);
    for (auto& [key, member] : members) {
        if (member.party == world.name) {
            SendTo(member, state, now_ms);
        }
    }
}

void Service::SendToOthers(PartyWorld& world, const Member& from, const bbcoop::Envelope& envelope,
                           qint64 now_ms) {
    for (auto& [key, member] : members) {
        if (member.party == world.name && &member != &from) {
            SendTo(member, envelope, now_ms);
        }
    }
}

Service::Result Service::Receive(const ServerLink::Endpoint& from, const QByteArray& online_id,
                                 const QByteArray& body, qint64 now_ms) {
    Result result;
    clock_ms = now_ms;
    CoopStream::Limits stream_limits;
    stream_limits.message_bytes = limits.message_bytes;
    stream_limits.chunk_bytes = CoopStream::ChunkBytes + 1;
    stream_limits.held_bytes = limits.held_bytes;
    stream_limits.unreliable_channels = 16; // the games send none on this stream
    auto [found, added] =
        members.try_emplace(from.Key(), QRandomGenerator::global()->generate() | 1u, stream_limits);
    Member& member = found->second;
    member.key = from.Key();
    endpoints[from.Key()] = from;
    member.online_id = online_id;
    const auto bytes =
        std::span{reinterpret_cast<const u8*>(body.constData()), static_cast<size_t>(body.size())};
    const std::vector<std::vector<u8>> messages = member.stream.Receive(bytes, now_ms);
    if (member.stream.Broken()) {
        Drop(member.key,
             QStringLiteral("a message past %1 bytes, or a chunk larger than a "
                            "game sends")
                 .arg(limits.message_bytes),
             result);
        FlushSome(now_ms, result);
        return result;
    }
    if (member.stream.TakeRestarted()) {
        result.log.append(QStringLiteral("party world: %1's game started over: waiting for its "
                                         "hello")
                              .arg(QString::fromStdString(Who(member))));
        member.party.clear();
        member.invasion_rewards = false; // renegotiate before sending an extension to a new game
        member.covenant_rewards = false;
        // An invader's game that started over is home: its invasion is over.
        searches.erase(member.key);
        if (const QString invaded = InvadedBy(member.key); !invaded.isEmpty()) {
            EndInvasion(invaded, bbcoop::InvasionEnd::INVADER_LEFT, now_ms, result);
        }
        // Still in the lobby (its team keeps it), but the new game has heard nothing of it.
        if (!member.lobby.empty()) {
            lobby_sent.erase(member.lobby);
            member.lobby_again = true;
        }
    }
    for (const std::vector<u8>& message : messages) {
        if (const auto envelope = DecodeEnvelope(message)) {
            if (const auto why = Oversized(*envelope)) {
                result.log.append(QStringLiteral("party world: %1's message dropped: %2")
                                      .arg(QString::fromStdString(Who(member)), *why));
                continue;
            }
            Take(member, *envelope, now_ms, result);
        }
    }
    // Its acks, and whatever its messages made the server send.
    if (!member.to_flush) {
        member.to_flush = true;
        flush_now.push_back(&member);
    }
    FlushSome(now_ms, result);
    return result;
}

std::optional<QString> Service::Oversized(const bbcoop::Envelope& envelope) {
    const auto text = [](const std::string& value, size_t limit) { return value.size() > limit; };
    const auto many = [](int count, size_t limit) { return static_cast<size_t>(count) > limit; };
    const auto items = [&](const auto& list) {
        return many(list.size(), MaxSmallList) ||
               std::ranges::any_of(list, [&](const bbcoop::TradeItem& item) {
                   return text(item.name(), MaxTradeItemName) ||
                          item.data().size() > MaxTradeItemData;
               });
    };
    switch (envelope.body_case()) {
    case bbcoop::Envelope::kHello:
        if (text(envelope.hello().party(), MaxPartyKeyBytes) ||
            text(envelope.hello().member(), MaxNameBytes)) {
            return QStringLiteral("a party key or member name too long");
        }
        break;
    case bbcoop::Envelope::kLobbyHello:
        if (text(envelope.lobby_hello().member(), MaxNameBytes) ||
            text(envelope.lobby_hello().name(), MaxNameBytes)) {
            return QStringLiteral("a lobby name too long");
        }
        break;
    case bbcoop::Envelope::kTeamRequest: {
        const bbcoop::TeamRequest& request = envelope.team_request();
        if (text(request.member(), MaxNameBytes) || text(request.team(), MaxNameBytes) ||
            text(request.world(), MaxNameBytes) || text(request.character(), MaxCharacterBytes) ||
            text(request.code(), MaxNameBytes) || text(request.name(), MaxCharacterBytes)) {
            return QStringLiteral("a team request's field too long");
        }
        break;
    }
    case bbcoop::Envelope::kKills: {
        size_t handles = 0;
        for (const bbcoop::MapKills& map : envelope.kills().kills()) {
            handles += static_cast<size_t>(map.handles_size()) + 1;
        }
        if (handles > MaxKillHandles) {
            return QStringLiteral("%1 kills").arg(handles);
        }
        break;
    }
    case bbcoop::Envelope::kShown:
        if (many(envelope.shown().characters_size(), MaxListEntries)) {
            return QStringLiteral("too many shown characters");
        }
        break;
    case bbcoop::Envelope::kFlags:
        if (many(envelope.flags().events_size(), MaxListEntries)) {
            return QStringLiteral("too many flag events");
        }
        break;
    case bbcoop::Envelope::kMapState:
        // The map's MapFlagCount flags, one bit each (fewer: WorldLogHost::MapState ignores it).
        if (envelope.map_state().bits().size() > static_cast<size_t>((MapFlagCount + 7) / 8)) {
            return QStringLiteral("a map state of %1 bytes")
                .arg(envelope.map_state().bits().size());
        }
        break;
    case bbcoop::Envelope::kHp:
        if (many(envelope.hp().enemies_size(), MaxListEntries)) {
            return QStringLiteral("too many enemies' HP");
        }
        break;
    case bbcoop::Envelope::kEchoes:
        if (many(envelope.echoes().kills_size(), MaxListEntries)) {
            return QStringLiteral("too many echoes");
        }
        break;
    case bbcoop::Envelope::kTrophies:
        if (many(envelope.trophies().ids_size(), MaxSmallList)) {
            return QStringLiteral("too many trophies");
        }
        break;
    case bbcoop::Envelope::kVitals:
        if (many(envelope.vitals().hits_size(), MaxSmallList) ||
            std::ranges::any_of(envelope.vitals().hits(), [&](const bbcoop::ReviveHit& hit) {
                return text(hit.target(), MaxNameBytes);
            })) {
            return QStringLiteral("too many revive hits");
        }
        break;
    case bbcoop::Envelope::kPortrait:
        if (envelope.portrait().pixels().size() > MaxPortraitBytes) {
            return QStringLiteral("a portrait of %1 bytes")
                .arg(envelope.portrait().pixels().size());
        }
        break;
    case bbcoop::Envelope::kTeamSession:
        if (envelope.team_session().join().size() > MaxJoinBytes) {
            return QStringLiteral("join data of %1 bytes")
                .arg(envelope.team_session().join().size());
        }
        break;
    case bbcoop::Envelope::kTeamWorld:
        if (many(envelope.team_world().on_size(), MaxTeamWorldFlags) ||
            many(envelope.team_world().blocks_size(), MaxTeamWorldFlags) ||
            text(envelope.team_world().character(), MaxCharacterBytes)) {
            return QStringLiteral("a team world too large");
        }
        break;
    case bbcoop::Envelope::kTradeOffer: {
        const bbcoop::TradeOffer& offer = envelope.trade_offer();
        if (text(offer.to(), MaxNameBytes) || text(offer.from_name(), MaxCharacterBytes) ||
            text(offer.to_name(), MaxCharacterBytes) || items(offer.give()) ||
            items(offer.want())) {
            return QStringLiteral("a trade offer too large");
        }
        break;
    }
    case bbcoop::Envelope::kTradeAnswer:
        if (items(envelope.trade_answer().taken())) {
            return QStringLiteral("a trade answer too large");
        }
        break;
    default:
        break;
    }
    return std::nullopt;
}

void Service::FlushSome(qint64 now_ms, Result& result) {
    std::vector<quint64> over;
    for (Member* member : std::exchange(flush_now, {})) {
        member->to_flush = false;
        for (const std::vector<u8>& body : member->stream.Flush(now_ms)) {
            result.sends.append(
                {endpoints.at(member->key), QByteArray(reinterpret_cast<const char*>(body.data()),
                                                       static_cast<qsizetype>(body.size()))});
        }
        if (member->stream.UnackedBytes() > limits.unacked_bytes) {
            over.push_back(member->key);
        }
    }
    for (const quint64 key : over) {
        Drop(key, QStringLiteral("over %1 bytes not acked").arg(limits.unacked_bytes), result);
    }
}

void Service::Flush(qint64 now_ms, Result& result) {
    for (Member* member : std::exchange(flush_now, {})) {
        member->to_flush = false;
    }
    std::vector<quint64> over;
    for (auto& [key, member] : members) {
        for (const std::vector<u8>& body : member.stream.Flush(now_ms)) {
            result.sends.append(
                {endpoints.at(key), QByteArray(reinterpret_cast<const char*>(body.data()),
                                               static_cast<qsizetype>(body.size()))});
        }
        if (member.stream.UnackedBytes() > limits.unacked_bytes) {
            over.push_back(key);
        }
    }
    for (const quint64 key : over) {
        Drop(key, QStringLiteral("over %1 bytes not acked").arg(limits.unacked_bytes), result);
    }
}

Service::Result Service::Tick(qint64 now_ms) {
    Result result;
    clock_ms = now_ms;
    if (!searches.empty() && now_ms - matched_ms >= InvasionMatchMs) {
        Match(now_ms, result);
    }
    std::vector<QString> due;
    std::vector<QString> timed_out;
    for (const auto& [name, invasion] : invasions) {
        if (!invasion.joined && now_ms - invasion.matched_ms >= InvasionNoticeMs) {
            due.push_back(name);
        } else if (invasion.joined && now_ms - invasion.joined_ms >= InvasionMaxMs) {
            timed_out.push_back(name);
        }
    }
    for (const QString& name : due) {
        const auto world = worlds.find(name);
        if (world == worlds.end()) {
            EndInvasion(name, bbcoop::InvasionEnd::HOST_LEFT, now_ms, result);
        } else {
            JoinInvasion(world->second, invasions.at(name), now_ms, result);
        }
    }
    for (const QString& name : timed_out) {
        EndInvasion(name, bbcoop::InvasionEnd::TIMEOUT, now_ms, result);
    }
    if (const TeamLobby::Outcome expired = lobby.Expire(now_ms); expired.changed) {
        result.log.append(QStringLiteral("team lobby: members gone for %1 min left their teams")
                              .arg(OfflineGraceMs / 60000));
        Settle(expired, now_ms, result);
    }
    if (lobby_changed && (!hosted.enabled || now_ms - lobby_sent_ms >= hosted.lobby_interval_ms)) {
        SendLobby(now_ms);
    }
    if (now_ms - collected_ms >= 1000) {
        collected_ms = now_ms;
        Collect(now_ms, false, result);
    }
    for (auto& [name, world] : worlds) {
        if (world.records > world.journal_from && now_ms - world.compacted_ms >= compact_ms) {
            Compact(world, now_ms);
        }
        if (now_ms - world.heartbeat_ms >= heartbeat_ms) {
            SendEntries(world, {}, now_ms);
        }
        SendBoards(world, world.boards.Expire(now_ms), now_ms);
        if (world.owners.Expire(now_ms)) {
            SendOwners(world, now_ms, result);
        }
        Deliver(world, world.trades.Expire(now_ms), now_ms, result);
        SettleDuel(world, world.duels.Tick(DuelMembers(world), now_ms), now_ms, result);
    }
    Flush(now_ms, result);
    if (writer) {
        result.log.append(writer->TakeLog());
    }
    return result;
}

std::string Service::CharacterOf(const std::string& member) const {
    if (const bbcoop::Team* team = lobby.TeamOf(member)) {
        for (const bbcoop::TeamMember& m : team->members()) {
            if (m.member() == member) {
                return std::to_string(m.slot()) + ":" + m.character();
            }
        }
    }
    return {};
}

bool Service::SessionHostLinked(const PartyWorld& world) const {
    return world.session && std::any_of(members.begin(), members.end(), [&](const auto& entry) {
               return entry.second.party == world.name &&
                      entry.second.name == world.session->host();
           });
}

void Service::CloseSession(PartyWorld& world, qint64 now_ms, Result& result) {
    bbcoop::Envelope closed;
    closed.mutable_team_session()->set_host(world.session->host());
    closed.mutable_team_session()->set_session(world.session->session());
    result.log.append(QStringLiteral("party world %1: %2's session %3 closed with its game")
                          .arg(world.name, QString::fromStdString(world.session->host()))
                          .arg(world.session->session()));
    world.session.reset();
    for (auto& [key, member] : members) {
        if (member.party == world.name && member.name != closed.team_session().host()) {
            SendTo(member, closed, now_ms);
        }
    }
    EndInvasion(world.name, bbcoop::InvasionEnd::HOST_LEFT, now_ms, result);
}

Service::Result Service::Forget(const ServerLink::Endpoint& link) {
    Result result;
    searches.erase(link.Key());
    InvasionLinkGone(link.Key(), clock_ms, result);
    const auto found = members.find(link.Key());
    if (found != members.end() && !found->second.lobby.empty()) {
        const std::string name = found->second.lobby;
        const bool elsewhere = std::any_of(members.begin(), members.end(), [&](const auto& m) {
            return m.first != link.Key() && m.second.lobby == name;
        });
        if (!elsewhere) {
            lobby.Leave(name);
            lobby_sent.erase(name);
            lobby_changed = true;
        }
    }
    if (found != members.end()) {
        std::erase(flush_now, &found->second);
    }
    members.erase(link.Key());
    endpoints.erase(link.Key());
    return result;
}

Service::Result Service::LinkLost(const ServerLink::Endpoint& link) {
    Result result;
    if (searches.erase(link.Key()) > 0) {
        result.log.append(QStringLiteral("invasions: a searching invader's link is lost"));
    }
    InvasionLinkGone(link.Key(), clock_ms, result);
    return result;
}

QString Service::VisitingOf(const ServerLink::Endpoint& link) const {
    const auto found = members.find(link.Key());
    return found != members.end() && found->second.hostile ? found->second.visiting : QString{};
}

QString Service::PartyOf(const ServerLink::Endpoint& link) const {
    const auto found = members.find(link.Key());
    return found == members.end() ? QString{} : found->second.party;
}

bool Service::RoutesPacket(const ServerLink::Endpoint& from, const ServerLink::Endpoint& to,
                           std::uint32_t channel) const {
    using BloodborneCoop::EnemyPart;
    const auto sender = members.find(from.Key());
    const auto target = members.find(to.Key());
    if (sender == members.end() || target == members.end()) {
        return BloodborneCoop::NativeChannelPolicy(channel) ==
                   BloodborneCoop::ChannelPolicy::Pass &&
               channel < BloodborneCoop::NativeChannelCount;
    }
    // Kaniel 2026-10-16: a member out of its team reaches none of the team's games, nor they it.
    if ((!sender->second.cut_from.isEmpty() && sender->second.cut_from == target->second.party) ||
        (!target->second.cut_from.isEmpty() && target->second.cut_from == sender->second.party)) {
        return false;
    }
    // Invasions: a hostile visitor and the team it visits go by the hostile table; the team
    // member's part in the enemies is the visited world's.
    const auto visits = [](const Member& visitor, const Member& other) {
        return visitor.hostile && !visitor.visiting.isEmpty() && visitor.visiting == other.party;
    };
    const bool from_visitor = visits(sender->second, target->second);
    const bool to_visitor = visits(target->second, sender->second);
    const auto world = worlds.find(from_visitor ? target->second.party : sender->second.party);
    const auto part = [&](const Member& member) {
        if (member.map == 0 || world == worlds.end()) {
            return EnemyPart::None;
        }
        const std::string owner = world->second.owners.OwnerOf(member.map);
        return owner.empty()          ? EnemyPart::None
               : owner == member.name ? EnemyPart::Owner
                                      : EnemyPart::Follower;
    };
    if (from_visitor || to_visitor) {
        return BloodborneCoop::RoutesHostilePacket(
            channel,
            {sender->second.map, target->second.map,
             from_visitor ? EnemyPart::None : part(sender->second),
             to_visitor ? EnemyPart::None : part(target->second)},
            from_visitor);
    }
    // A hostile visitor reaches nobody else (nor its own party while away), and games of two
    // parties never meet (the hub's teams say so too).
    if (sender->second.hostile || target->second.hostile ||
        sender->second.party != target->second.party) {
        return false;
    }
    return BloodborneCoop::RoutesNativePacket(
        channel,
        {sender->second.map, target->second.map, part(sender->second), part(target->second)});
}

namespace {
const QString TeamWorldFile = QStringLiteral("team_world.pb");
const QString WorldListFile = QStringLiteral("worlds.pb");
const QString OldTeamsFile = QStringLiteral("teams.pb");
const QString ForgottenFolder = QStringLiteral("forgotten");
const QString KeptWorldFile = QStringLiteral("kept_world.pb");
constexpr u32 WorldListVersion = 1;

// Kaniel 2026-10-25: a team's world gets the backups a party's world.pb has (the broken chalice
// altar of 2026-10-24 went into the team's world with no copy to take back): the last writes as
// team_world.pb.1 to .3, and each day's first as backups/team_world-<yyyyMMdd>.pb for a week.
std::optional<bbcoop::TeamWorld> ReadTeamWorld(const QString& path) {
    QFile file{path};
    if (!file.open(QIODevice::ReadOnly)) {
        return std::nullopt;
    }
    const QByteArray bytes = file.readAll();
    bbcoop::TeamWorld stored;
    if (bytes.isEmpty() ||
        !stored.ParseFromArray(bytes.constData(), static_cast<int>(bytes.size())) ||
        !stored.exists()) {
        return std::nullopt;
    }
    return stored;
}

// team_world.pb in `world_dir`, else its newest backup that reads.
std::optional<bbcoop::TeamWorld> LoadTeamWorld(const QDir& world_dir) {
    std::optional<bbcoop::TeamWorld> stored = ReadTeamWorld(world_dir.filePath(TeamWorldFile));
    for (int backup = 1; !stored && backup <= Backups; ++backup) {
        stored = ReadTeamWorld(
            world_dir.filePath(QStringLiteral("%1.%2").arg(TeamWorldFile).arg(backup)));
    }
    return stored;
}

bool StoreTeamWorld(const QDir& world_dir, const bbcoop::TeamWorld& world, const QDate& today) {
    const QString path = world_dir.filePath(TeamWorldFile);
    if (QFileInfo::exists(path)) {
        const auto backup = [&](int number) {
            return world_dir.filePath(QStringLiteral("%1.%2").arg(TeamWorldFile).arg(number));
        };
        QFile::remove(backup(Backups));
        for (int number = Backups - 1; number >= 1; --number) {
            QFile::rename(backup(number), backup(number + 1));
        }
        QFile::copy(path, backup(1));
    }
    QSaveFile file{path};
    if (!file.open(QIODevice::WriteOnly) || file.write(Serialized(world)) < 0 || !file.commit()) {
        return false;
    }
    const QDir backups{world_dir.filePath(QStringLiteral("backups"))};
    const QString dated = backups.filePath(
        QStringLiteral("team_world-%1.pb").arg(today.toString(QStringLiteral("yyyyMMdd"))));
    if (!QFileInfo::exists(dated)) {
        QDir{}.mkpath(backups.path());
        QFile::copy(path, dated);
        QStringList days =
            backups.entryList({QStringLiteral("team_world-*.pb")}, QDir::Files, QDir::Name);
        while (days.size() > DatedBackups) {
            QFile::remove(backups.filePath(days.takeFirst()));
        }
    }
    return true;
}
} // namespace

const std::optional<bbcoop::TeamWorld>& Service::TeamWorldOf(PartyWorld& world) {
    if (!world.team_world_read) {
        world.team_world_read = true;
        if (!folder.isEmpty()) {
            if (writer->Moving(folder, world.name)) {
                writer->Drain(); // its folder going aside: what it holds after
            }
            world.team_world = LoadTeamWorld(QDir{QDir{folder}.filePath(world.name)});
        }
    }
    return world.team_world;
}

void Service::SendTeamWorld(PartyWorld& world, Member& member, qint64 now_ms) {
    bbcoop::Envelope envelope;
    if (const auto& team_world = TeamWorldOf(world)) {
        *envelope.mutable_team_world() = *team_world;
        envelope.mutable_team_world()->set_exists(true);
    } else {
        envelope.mutable_team_world()->set_exists(false);
    }
    SendTo(member, envelope, now_ms);
}

void Service::TakeTeamWorld(Member& member, PartyWorld& world, const bbcoop::TeamWorld& report,
                            qint64 now_ms, Result& result) {
    if (!world.team || !member.host) {
        return; // only the team's host's world is the team's
    }
    const auto& before = TeamWorldOf(world);
    if (before &&
        std::equal(before->on().begin(), before->on().end(), report.on().begin(),
                   report.on().end()) &&
        std::equal(before->blocks().begin(), before->blocks().end(), report.blocks().begin(),
                   report.blocks().end())) {
        return; // the same world
    }
    bbcoop::TeamWorld taken;
    *taken.mutable_on() = report.on();
    std::sort(taken.mutable_on()->begin(), taken.mutable_on()->end());
    *taken.mutable_blocks() = report.blocks();
    taken.set_character(report.character());
    taken.set_exists(true);
    taken.set_version((before ? before->version() : 0) + 1);
    world.team_world = taken;
    if (writer) {
        // On the writer's thread, with the team world's backups (StoreTeamWorld).
        writer->SaveTeamWorld(QDir{folder}.filePath(world.name), taken,
                              QDateTime::fromMSecsSinceEpoch(now_ms).date());
    }
    result.log.append(QStringLiteral("team world %1: %2's world taken (%3 flags on, version %4)")
                          .arg(world.name, QString::fromStdString(member.name))
                          .arg(taken.on_size())
                          .arg(taken.version()));
    // Kaniel 2026-10-16: what the team's world list says of it (its host's character, how far).
    bbcoop::TeamWorldInfo note;
    note.set_id(world.team_world_id);
    note.set_character(report.character());
    note.set_flags_on(static_cast<u32>(taken.on_size()));
    note.set_lamps(LampsLit({taken.on().data(), static_cast<size_t>(taken.on_size())}));
    if (lobby.NoteWorld(note)) {
        StoreWorldList(result);
        lobby_changed = true;
    }
    for (auto& [key, other] : members) {
        if (other.party == world.name) {
            SendTeamWorld(world, other, now_ms);
        }
    }
}

void Service::OnLobbyHello(Member& member, const bbcoop::LobbyHello& hello, qint64 now_ms,
                           Result& result) {
    if (hello.protocol() != CoopProtocol || hello.member().empty()) {
        bbcoop::Envelope answer;
        answer.mutable_lobby_state()->set_notice(
            hello.member().empty() ? "no member name"
                                   : "co-op protocol " + std::to_string(hello.protocol()) +
                                         ", this server speaks " + std::to_string(CoopProtocol));
        SendTo(member, answer, now_ms);
        result.log.append(QStringLiteral("team lobby: refused %1: %2")
                              .arg(QString::fromStdString(Who(member)),
                                   QString::fromStdString(answer.lobby_state().notice())));
        return;
    }
    if (hosted.enabled && !OwnName(member, hello.member())) {
        bbcoop::Envelope answer;
        answer.mutable_lobby_state()->set_notice("the member name is not this player's");
        SendTo(member, answer, now_ms);
        result.log.append(
            QStringLiteral("team lobby: refused %1: another player's name %2")
                .arg(QString::fromStdString(Who(member)), QString::fromStdString(hello.member())));
        return;
    }
    const bool arrived = member.lobby.empty() || std::exchange(member.lobby_again, false);
    member.lobby = hello.member();
    member.display = hello.name();
    const bool rewards_before = std::exchange(member.invasion_rewards, hello.invasion_rewards());
    const bool covenants_before = std::exchange(member.covenant_rewards,
                                                hello.invasion_rewards() && hello.covenant_rewards());
    if (member.invasion_rewards &&
        (arrived || !rewards_before || (!covenants_before && member.covenant_rewards))) {
        ResumeRewards(member, now_ms, result);
    }
    if (arrived) {
        result.log.append(
            QStringLiteral("team lobby: %1 (%2) signed in")
                .arg(QString::fromStdString(hello.member()), QString::fromStdString(hello.name())));
    }
    const TeamLobby::Outcome arrived_outcome =
        lobby.Arrive(hello.member(), hello.name(), hello.playing());
    if (arrived_outcome.worlds_changed) {
        StoreWorldList(result);
    }
    Settle(arrived_outcome, now_ms, result);
    if (arrived) {
        // Its state even when nothing changed for the others.
        bbcoop::Envelope envelope;
        *envelope.mutable_lobby_state() = lobby.StateFor(member.lobby);
        SendTo(member, envelope, now_ms);
    }
    // A region: what the shard that sent it here asked (join its team, make its team again).
    if (const auto found = expected.find(member.lobby); found != expected.end()) {
        const std::vector<bbcoop::TeamRequest> requests = std::move(found->second);
        expected.erase(found);
        for (const bbcoop::TeamRequest& request : requests) {
            OnTeamRequest(member, request, now_ms, result, true);
        }
    }
}

void Service::SetRegion(TeamLobby::Region region, std::string shard) {
    lobby.SetRegion(std::move(region));
    lobby.SetShardName(std::move(shard));
}

Service::Result Service::TakeInvites(
    const std::vector<std::pair<std::string, bbcoop::TeamInvite>>& invites, qint64 now_ms) {
    Result result;
    for (const auto& [to, invite] : invites) {
        const TeamLobby::Outcome outcome = lobby.InviteFromElsewhere(to, invite);
        if (outcome.changed) {
            result.log.append(
                QStringLiteral("team lobby: %1 invited from another shard (%2)")
                    .arg(QString::fromStdString(to), QString::fromStdString(invite.team())));
        }
        Settle(outcome, now_ms, result);
    }
    return result;
}

void Service::Expect(const std::string& member, std::vector<bbcoop::TeamRequest> requests) {
    if (expected.size() > 4096) {
        expected.clear(); // tickets nobody used (each holds 2 minutes)
    }
    expected[member] = std::move(requests);
}

void Service::OnTeamRequest(Member& member, const bbcoop::TeamRequest& said, qint64 now_ms,
                            Result& result, bool ticket) {
    if (member.lobby.empty()) {
        return; // nothing before its lobby hello
    }
    bbcoop::TeamRequest request = said;
    if (!ticket && request.action() == bbcoop::TeamRequest::CREATE) {
        request.clear_code(); // a team's code is the server's to make
    }
    const TeamLobby::Outcome outcome = lobby.Request(member.lobby, request, now_ms);
    // Kaniel 2026-10-16: the world a Start plays is the team's last played.
    if (request.action() == bbcoop::TeamRequest::START && !outcome.starts.empty()) {
        if (const bbcoop::Team* team = lobby.TeamOf(member.lobby)) {
            bbcoop::TeamWorldInfo note;
            note.set_id(team->world());
            note.set_played_ms(QDateTime::currentMSecsSinceEpoch());
            note.set_created_ms(note.played_ms()); // a new world's birth (kept from the first)
            lobby.NoteWorld(note);
            result.log.append(QStringLiteral("team lobby: team %1 plays world %2")
                                  .arg(QString::fromStdString(team->id()),
                                       QString::fromStdString(team->world())));
        }
    }
    result.log.append(
        QStringLiteral("team lobby: %1 %2%3")
            .arg(QString::fromStdString(member.lobby),
                 QString::fromStdString(bbcoop::TeamRequest::Action_Name(request.action())))
            .arg(outcome.notice.empty()
                     ? QString{}
                     : QStringLiteral(": refused, ") + QString::fromStdString(outcome.notice)));
    if (!outcome.notice.empty()) {
        bbcoop::Envelope envelope;
        *envelope.mutable_lobby_state() = lobby.StateFor(member.lobby);
        envelope.mutable_lobby_state()->set_notice(outcome.notice);
        SendTo(member, envelope, now_ms);
    }
    PutAside(outcome.forgotten_lines, QStringLiteral("forgotten"), result);
    if (outcome.changed) {
        StoreWorldList(result);
    }
    // Invasions: the leader turned them off: a running one ends.
    if (request.action() == bbcoop::TeamRequest::INVADERS && !request.allow()) {
        if (const bbcoop::Team* team = lobby.TeamOf(member.lobby);
            team && team->leader() == member.lobby && !team->party().empty()) {
            EndInvasion(FolderOf(QByteArray::fromStdString(team->party())),
                        bbcoop::InvasionEnd::INVADERS_OFF, now_ms, result);
        }
    }
    for (const TeamLobby::Departure& out : outcome.departed) {
        TakeOutOfTeam(out, now_ms, result);
    }
    Settle(outcome, now_ms, result);
}

void Service::TakeOutOfTeam(const TeamLobby::Departure& out, qint64 now_ms, Result& result) {
    for (auto& [key, member] : members) {
        if (member.lobby != out.member || member.party.isEmpty()) {
            continue;
        }
        const QString party = member.party;
        const auto world = worlds.find(party);
        if (out.party.empty() || world == worlds.end() ||
            party != FolderOf(QByteArray::fromStdString(out.party))) {
            continue; // not in the world of that team
        }
        result.log.append(
            QStringLiteral("party world %1: %2 is out of team %3: out of its world")
                .arg(party, QString::fromStdString(member.name), QString::fromStdString(out.team)));
        member.party.clear();
        member.cut_from = party;
        member.map = 0;
        if (world->second.session && world->second.session->host() == member.name) {
            CloseSession(world->second, now_ms, result);
        }
        ForgetMember(world->second, member.name, now_ms, result);
    }
}

void Service::ForgetMember(PartyWorld& world, const std::string& name, qint64 now_ms,
                           Result& result) {
    const std::vector<WorldLogEntry> entries = world.host.Drop(name);
    world.proposed.erase(name);
    world.loads.erase(name);
    SendBoards(world, world.boards.Forget(name), now_ms);
    if (world.owners.Forget(name)) {
        SendOwners(world, now_ms, result);
    }
    bbcoop::JournalRecord record;
    record.mutable_drop()->set_member(name);
    Journal(world, std::move(record), now_ms);
    if (!entries.empty()) {
        SendEntries(world, entries, now_ms);
    }
}

void Service::Settle(const TeamLobby::Outcome& outcome, qint64 now_ms, Result& result) {
    for (const TeamLobby::Move& move : outcome.moves) {
        for (const auto& [key, member] : members) {
            if (member.lobby == move.member) {
                result.moves.push_back(
                    {endpoints.at(key), member.online_id, move.shard, move.requests});
                result.log.append(QStringLiteral("team lobby: %1 goes to shard %2")
                                      .arg(QString::fromStdString(move.member),
                                           QString::fromStdString(move.shard)));
            }
        }
    }
    result.remote_invites.insert(result.remote_invites.end(), outcome.remote_invites.begin(),
                                 outcome.remote_invites.end());
    if (outcome.changed) {
        if (hosted.enabled) {
            lobby_changed = true; // hosted: with the others at the next tick past the interval
        } else {
            SendLobby(now_ms);
        }
    }
    for (const std::string& name : outcome.starts) {
        bbcoop::TeamStart start = lobby.StartFor(name);
        // Phase 7: the team's session is hosted by whoever hosts it now (it moves to a guest when
        // the host's game goes), not always by the leader: a leader back joins that one.
        if (const auto world = worlds.find(FolderOf(QByteArray::fromStdString(start.party())));
            world != worlds.end() && world->second.session &&
            world->second.session->host() != start.host() && SessionHostLinked(world->second)) {
            start.set_host(world->second.session->host());
        }
        result.log.append(
            QStringLiteral("team lobby: %1 starts in team %2 (slot %3, host %4)")
                .arg(QString::fromStdString(name), QString::fromStdString(start.team()))
                .arg(start.slot())
                .arg(QString::fromStdString(start.host())));
        for (auto& [key, member] : members) {
            if (member.lobby == name) {
                bbcoop::Envelope envelope;
                *envelope.mutable_team_start() = start;
                SendTo(member, envelope, now_ms);
                result.started.append(member.online_id);
            }
        }
    }
}

void Service::SendLobby(qint64 now_ms) {
    lobby_changed = false;
    lobby_sent_ms = now_ms;
    for (auto& [key, member] : members) {
        if (!member.lobby.empty()) {
            bbcoop::Envelope envelope;
            *envelope.mutable_lobby_state() = lobby.StateFor(member.lobby);
            if (hosted.enabled) {
                // Only to the players whose own state changed.
                std::string bytes = envelope.lobby_state().SerializeAsString();
                std::string& sent = lobby_sent[member.lobby];
                if (sent == bytes) {
                    continue;
                }
                sent = std::move(bytes);
            }
            SendTo(member, envelope, now_ms);
        }
    }
}

void Service::SetHosted(const HostedRules& rules) {
    hosted = rules;
    if (rules.enabled) {
        lobby.SetHosted(rules.lobby_players, rules.worlds_per_player);
    }
}

bool Service::OwnName(const Member& member, const std::string& name) const {
    return name == member.online_id.toHex().toStdString();
}

QString Service::FolderOf(const QByteArray& party) const {
    if (party.startsWith("world:")) {
        if (const auto found = world_folders.find(party.mid(6).toStdString());
            found != world_folders.end()) {
            return found->second;
        }
    }
    return PartyFolderName(party);
}

void Service::StoreWorldList(Result& /*result*/) {
    if (folder.isEmpty()) {
        return;
    }
    bbcoop::StoredWorlds stored;
    stored.set_version(WorldListVersion);
    for (const bbcoop::TeamWorldInfo& info : lobby.Worlds()) {
        bbcoop::KeptWorld& kept = *stored.add_worlds();
        *kept.mutable_info() = info;
        kept.set_folder(
            FolderOf(QByteArray::fromStdString(TeamLobby::PartyOf(info.id()))).toStdString());
    }
    writer->Save(QDir{folder}.filePath(WorldListFile), Serialized(stored));
}

namespace {

// Kaniel 2026-10-17: what a world's line needs that a world from before did not keep: when it was
// made (the earliest time its files know, or its last played) and its host among the characters.
// Whether anything was filled in.
bool FillWorldLine(bbcoop::TeamWorldInfo& info, const QDir& world_dir) {
    bool filled = false;
    if (info.created_ms() == 0) {
        qint64 earliest = info.played_ms();
        const auto note = [&](const QDateTime& at) {
            if (at.isValid()) {
                const qint64 ms = at.toMSecsSinceEpoch();
                earliest = earliest == 0 ? ms : std::min(earliest, ms);
            }
        };
        note(QFileInfo{world_dir.path()}.birthTime());
        for (const QString& sub : {QString{}, QStringLiteral("backups")}) {
            const QDir in{world_dir.filePath(sub)};
            for (const QFileInfo& file : in.entryInfoList(QDir::Files)) {
                note(file.birthTime());
                note(file.lastModified());
            }
        }
        if (earliest != 0) {
            info.set_created_ms(earliest);
            filled = true;
        }
    }
    if (!info.character().empty() &&
        std::ranges::find(info.characters(), info.character()) == info.characters().end()) {
        info.add_characters(info.character());
        filled = true;
    }
    return filled;
}

} // namespace

void Service::LoadWorldList(Result& result) {
    const QDir dir{folder};
    int filled = 0;
    std::vector<bbcoop::TeamWorldInfo> list;
    std::set<std::string> ids;
    std::set<QString> listed;
    {
        QFile file{dir.filePath(WorldListFile)};
        bbcoop::StoredWorlds stored;
        if (file.open(QIODevice::ReadOnly)) {
            const QByteArray bytes = file.readAll();
            if (!stored.ParseFromArray(bytes.constData(), static_cast<int>(bytes.size()))) {
                result.log.append(QStringLiteral("world list: %1 does not read, made again")
                                      .arg(file.fileName()));
                stored.Clear();
            }
        }
        for (const bbcoop::KeptWorld& kept : stored.worlds()) {
            if (kept.info().id().empty() || !ids.insert(kept.info().id()).second) {
                continue;
            }
            const QString name = QString::fromStdString(kept.folder());
            list.push_back(kept.info());
            filled += FillWorldLine(list.back(), QDir{dir.filePath(name)}) ? 1 : 0;
            world_folders[kept.info().id()] = name;
            listed.insert(name);
        }
    }
    // Kaniel 2026-10-17: the worlds kept before the list, under their teams' keys: the teams
    // file says each one's id and line ("team:<team>/<id>"; "team:<team>" for id "0").
    std::map<QString, bbcoop::TeamWorldInfo> old;
    const QString teams_path = dir.filePath(OldTeamsFile);
    const bool had_teams = QFile::exists(teams_path);
    if (QFile file{teams_path}; file.open(QIODevice::ReadOnly)) {
        const QByteArray bytes = file.readAll();
        bbcoop::OldStoredTeams stored;
        if (stored.ParseFromArray(bytes.constData(), static_cast<int>(bytes.size()))) {
            for (const bbcoop::OldTeam& team : stored.teams()) {
                const std::string key = "team:" + team.id();
                old.try_emplace(PartyFolderName(QByteArray::fromStdString(key)))
                    .first->second.set_id("0");
                for (const bbcoop::TeamWorldInfo& world : team.worlds()) {
                    const std::string party = world.id() == "0" ? key : key + "/" + world.id();
                    old[PartyFolderName(QByteArray::fromStdString(party))] = world;
                }
            }
        }
    }
    // A team world's folder not on the list (its team_world.pb, or its line in the teams file).
    int adopted = 0;
    for (const QString& name : dir.entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
        const QDir world_dir{dir.filePath(name)};
        const QString team_world = world_dir.filePath(TeamWorldFile);
        const auto from_teams = old.find(name);
        const bool has_team_world = QFile::exists(team_world);
        const bool has_world = QFile::exists(world_dir.filePath(WorldFile));
        if (listed.contains(name) || !(has_team_world || (from_teams != old.end() && has_world))) {
            continue;
        }
        bbcoop::TeamWorldInfo info;
        if (from_teams != old.end()) {
            info = from_teams->second;
        }
        if (QFile file{team_world}; file.open(QIODevice::ReadOnly)) {
            const QByteArray bytes = file.readAll();
            bbcoop::TeamWorld stored;
            if (stored.ParseFromArray(bytes.constData(), static_cast<int>(bytes.size()))) {
                info.set_flags_on(static_cast<u32>(stored.on_size()));
                info.set_lamps(
                    LampsLit({stored.on().data(), static_cast<size_t>(stored.on_size())}));
                if (!stored.character().empty()) {
                    info.set_character(stored.character());
                }
            }
        }
        if (info.played_ms() == 0) {
            const QString seen = has_team_world ? team_world : world_dir.filePath(WorldFile);
            info.set_played_ms(QFileInfo{seen}.lastModified().toMSecsSinceEpoch());
        }
        // Its old id when no other world has it (a save's "team_world" file names it).
        if (info.id().empty() || info.id() == "0" || ids.contains(info.id())) {
            std::string id = lobby.NewWorldId();
            while (ids.contains(id)) {
                id = lobby.NewWorldId();
            }
            info.set_id(id);
        }
        ids.insert(info.id());
        world_folders[info.id()] = name;
        FillWorldLine(info, world_dir);
        list.push_back(info);
        ++adopted;
        result.log.append(
            QStringLiteral("world list: the world in %1 is world %2 (%3's, %4 lamps)")
                .arg(name, QString::fromStdString(info.id()),
                     QString::fromStdString(info.character().empty() ? std::string{"nobody"}
                                                                     : info.character()))
                .arg(info.lamps()));
    }
    lobby.SetWorlds(std::move(list));
    result.log.append(QStringLiteral("world list: %1 worlds (%2 new on it)")
                          .arg(lobby.Worlds().size())
                          .arg(adopted));
    if (adopted > 0 || filled > 0 || had_teams) {
        StoreWorldList(result);
    }
    if (had_teams) {
        // Teams are not kept any more; the file stays beside as it was.
        const QString kept = teams_path + QStringLiteral(".old");
        QFile::remove(kept);
        QFile::rename(teams_path, kept);
        result.log.append(QStringLiteral("world list: teams are not kept now: %1 is %2")
                              .arg(OldTeamsFile, QFileInfo{kept}.fileName()));
    }
}

void Service::PutAside(const std::vector<bbcoop::TeamWorldInfo>& lines, const QString& why,
                       Result& result) {
    for (const bbcoop::TeamWorldInfo& line : lines) {
        const std::string& id = line.id();
        const QString name = FolderOf(QByteArray::fromStdString(TeamLobby::PartyOf(id)));
        world_folders.erase(id);
        const bool linked = std::ranges::any_of(
            members, [&](const auto& entry) { return entry.second.party == name; });
        if (linked) {
            // A game still in it (it would be made again): its folder stays, off the list.
            result.log.append(
                QStringLiteral("world list: world %1 %2, its folder %3 kept (a game is in it)")
                    .arg(QString::fromStdString(id), why, name));
            continue;
        }
        worlds.erase(name);
        kept.erase(name);
        if (folder.isEmpty()) {
            continue;
        }
        const QString aside =
            QStringLiteral("%1/%2-%3")
                .arg(ForgottenFolder, name,
                     QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd-HHmmss")));
        bbcoop::KeptWorld kept;
        *kept.mutable_info() = line;
        kept.set_folder(name.toStdString());
        writer->Save(QDir{QDir{folder}.filePath(name)}.filePath(KeptWorldFile), Serialized(kept));
        writer->Move(folder, name, aside);
        result.log.append(QStringLiteral("world list: world %1 %2, its folder goes to %3")
                              .arg(QString::fromStdString(id), why, aside));
    }
}

namespace {

qint64 BytesIn(const QString& path) {
    qint64 bytes = 0;
    QDirIterator it{path, QDir::Files | QDir::Hidden, QDirIterator::Subdirectories};
    while (it.hasNext()) {
        it.next();
        bytes += it.fileInfo().size();
    }
    return bytes;
}

std::optional<bbcoop::KeptWorld> ReadKept(const QString& world_dir) {
    QFile file{QDir{world_dir}.filePath(KeptWorldFile)};
    if (!file.open(QIODevice::ReadOnly)) {
        return std::nullopt;
    }
    const QByteArray bytes = file.readAll();
    bbcoop::KeptWorld kept;
    if (!kept.ParseFromArray(bytes.constData(), static_cast<int>(bytes.size())) ||
        kept.info().id().empty()) {
        return std::nullopt;
    }
    return kept;
}

} // namespace

std::optional<Service::Place> Service::PlaceOf(const ServerLink::Endpoint& link) const {
    const auto found = members.find(link.Key());
    if (found == members.end()) {
        return std::nullopt;
    }
    const Member& member = found->second;
    Place place{member.lobby, {}, {}, member.map};
    if (const bbcoop::Team* team = member.lobby.empty() ? nullptr : lobby.TeamOf(member.lobby)) {
        place.team = team->id();
        place.world = team->world();
    }
    if (const auto world = worlds.find(member.party);
        world != worlds.end() && place.world.empty()) {
        place.world = world->second.team_world_id;
    }
    return place;
}

std::vector<Service::WorldRow> Service::WorldRows() const {
    std::vector<WorldRow> rows;
    for (const bbcoop::TeamWorldInfo& info : lobby.Worlds()) {
        WorldRow& row = rows.emplace_back();
        row.info = info;
        row.folder = FolderOf(QByteArray::fromStdString(TeamLobby::PartyOf(info.id())));
        if (!folder.isEmpty()) {
            row.path = QDir{folder}.filePath(row.folder);
        }
        row.games = static_cast<int>(std::ranges::count_if(
            members, [&](const auto& entry) { return entry.second.party == row.folder; }));
    }
    return rows;
}

qint64 Service::BytesOn(const QString& path) {
    return BytesIn(path);
}

std::vector<Service::AsideRow> Service::AsideRows() const {
    return AsideRowsIn(folder);
}

std::vector<Service::AsideRow> Service::AsideRowsIn(const QString& folder) {
    std::vector<AsideRow> rows;
    if (folder.isEmpty()) {
        return rows;
    }
    const QDir aside{QDir{folder}.filePath(ForgottenFolder)};
    for (const QString& name : aside.entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name)) {
        AsideRow& row = rows.emplace_back();
        row.folder = name;
        row.bytes = BytesIn(aside.filePath(name));
        if (const auto kept = ReadKept(aside.filePath(name))) {
            row.info = kept->info();
        }
    }
    return rows;
}

std::vector<std::string> Service::Retire(qint64 now_ms, qint64 idle_ms, Result& result) {
    if (idle_ms <= 0) {
        return {};
    }
    std::vector<bbcoop::TeamWorldInfo> lines = lobby.TakeIdleWorlds(now_ms - idle_ms);
    // A game still in one (its team gone, its game not yet): it stays listed for now.
    std::erase_if(lines, [&](const bbcoop::TeamWorldInfo& line) {
        const QString name = FolderOf(QByteArray::fromStdString(TeamLobby::PartyOf(line.id())));
        if (std::ranges::none_of(members,
                                 [&](const auto& entry) { return entry.second.party == name; })) {
            return false;
        }
        lobby.AddWorld(line);
        return true;
    });
    std::vector<std::string> ids;
    for (const bbcoop::TeamWorldInfo& line : lines) {
        ids.push_back(line.id());
    }
    if (!lines.empty()) {
        PutAside(lines,
                 QStringLiteral("retired (not played for %1 days)").arg(idle_ms / 86'400'000),
                 result);
        StoreWorldList(result);
        lobby_changed = true;
    }
    return ids;
}

bool Service::Restore(const QString& aside, qint64 now_ms, QString& said, Result& result) {
    if (folder.isEmpty()) {
        said = QStringLiteral("the worlds are kept in memory only");
        return false;
    }
    if (aside.isEmpty() || aside.contains('/') || aside.contains('\\') || aside.startsWith('.')) {
        said = QStringLiteral("no such folder in %1").arg(ForgottenFolder);
        return false;
    }
    writer->Drain(); // a folder still going aside: there first
    const QDir dir{folder};
    const QString from = ForgottenFolder + '/' + aside;
    if (!QDir{dir.filePath(from)}.exists()) {
        said = QStringLiteral("no folder %1").arg(from);
        return false;
    }
    bbcoop::KeptWorld kept;
    if (const auto read = ReadKept(dir.filePath(from))) {
        kept = *read;
    } else {
        // Put aside before its line was kept with it: a new line, its players unknown.
        QString name = aside;
        name.remove(QRegularExpression(QStringLiteral("-\\d{8}-\\d{6}$")));
        kept.set_folder(name.toStdString());
        kept.mutable_info()->set_id(lobby.NewWorldId());
        QFile file{QDir{dir.filePath(from)}.filePath(TeamWorldFile)};
        bbcoop::TeamWorld stored;
        if (file.open(QIODevice::ReadOnly)) {
            const QByteArray bytes = file.readAll();
            if (stored.ParseFromArray(bytes.constData(), static_cast<int>(bytes.size()))) {
                kept.mutable_info()->set_character(stored.character());
                kept.mutable_info()->set_flags_on(static_cast<u32>(stored.on_size()));
                kept.mutable_info()->set_lamps(
                    LampsLit({stored.on().data(), static_cast<size_t>(stored.on_size())}));
            }
        }
    }
    const QString name = QString::fromStdString(kept.folder());
    if (name.isEmpty() || name.contains('/') || dir.exists(name) || worlds.contains(name)) {
        said = QStringLiteral("its folder %1 is taken").arg(name);
        return false;
    }
    bbcoop::TeamWorldInfo info = kept.info();
    info.set_played_ms(now_ms);
    if (info.id().empty() || std::ranges::any_of(lobby.Worlds(), [&](const auto& listed) {
            return listed.id() == info.id();
        })) {
        said = QStringLiteral("world %1 is listed").arg(QString::fromStdString(info.id()));
        return false;
    }
    // Here, at once (drained above): a move queued on the writer would leave the folder missing
    // to a hello or a Start that comes first, which would make an empty one in its place.
    if (!QDir{dir}.rename(from, name)) {
        said = QStringLiteral("could not move %1 to %2").arg(from, name);
        return false;
    }
    lobby.AddWorld(info);
    world_folders[info.id()] = name;
    StoreWorldList(result);
    lobby_changed = true;
    said = QString::fromStdString(info.id());
    result.log.append(
        QStringLiteral("world list: world %1 restored from %2 to %3").arg(said, from, name));
    return true;
}

void Service::Refuse(Member& member, bbcoop::InvasionEnd::Reason reason, const std::string& text,
                     qint64 now_ms) {
    bbcoop::Envelope envelope;
    envelope.mutable_invasion_end()->set_reason(reason);
    envelope.mutable_invasion_end()->set_invader(member.lobby);
    envelope.mutable_invasion_end()->set_text(text);
    SendTo(member, envelope, now_ms);
}

void Service::OnInvadeRequest(Member& member, const bbcoop::InvadeRequest& request, qint64 now_ms,
                              Result& result) {
    const QString who = QString::fromStdString(member.lobby.empty() ? Who(member) : member.lobby);
    const QString invaded = InvadedBy(member.key);
    if (request.cancel()) {
        if (searches.erase(member.key) > 0) {
            Refuse(member, bbcoop::InvasionEnd::INVADER_LEFT, "search cancelled", now_ms);
            result.log.append(QStringLiteral("invasions: %1 stops looking").arg(who));
        }
        if (!invaded.isEmpty()) {
            EndInvasion(invaded, bbcoop::InvasionEnd::INVADER_LEFT, now_ms, result);
        }
        return;
    }
    std::string refused;
    if (!invasion_rules.enabled) {
        refused = "invasions are off on this server";
    } else if (member.lobby.empty()) {
        refused = "sign in to the lobby first";
    } else if (lobby.TeamOf(member.lobby)) {
        refused = "leave your team to invade";
    } else if (!invaded.isEmpty()) {
        return; // on its way already
    }
    if (!refused.empty()) {
        Refuse(member, bbcoop::InvasionEnd::REFUSED, refused, now_ms);
        result.log.append(
            QStringLiteral("invasions: %1 refused: %2").arg(who, QString::fromStdString(refused)));
        return;
    }
    searches[member.key] = {request.level(), now_ms,
                            NormalizeCovenantEffect(request.covenant_effect())};
    result.log.append(
        QStringLiteral("invasions: %1 (level %2) looks for a world").arg(who).arg(request.level()));
    Match(now_ms, result);
}

void Service::TakeHostile(Member& member, const bbcoop::Envelope& envelope, qint64 now_ms,
                          Result& result) {
    const QString invaded = InvadedBy(member.key);
    switch (envelope.body_case()) {
    case bbcoop::Envelope::kMap:
        member.map = envelope.map().map();
        return;
    case bbcoop::Envelope::kVitals: {
        member.level = envelope.vitals().level();
        member.covenant_effect = NormalizeCovenantEffect(envelope.vitals().covenant_effect());
        if (envelope.vitals().vital() == 3 && !invaded.isEmpty()) { // Vital: dead
            EndInvasion(invaded, bbcoop::InvasionEnd::INVADER_DIED, now_ms, result);
            return;
        }
        const auto invasion = invasions.find(invaded);
        if (invasion == invasions.end() || !invasion->second.joined) {
            return;
        }
        bbcoop::Vitals line = envelope.vitals();
        line.clear_hits(); // no revives of anyone's
        if (!invasion->second.vitals ||
            invasion->second.vitals->SerializeAsString() != line.SerializeAsString()) {
            invasion->second.vitals = std::move(line);
            if (const auto world = worlds.find(invaded); world != worlds.end()) {
                SendBoards(world->second, PartyBoards::VitalsBoard, now_ms);
            }
        }
        return;
    }
    default:
        break;
    }
    // Its proposals, boards, trades, hellos: it is in no world here, so they go nowhere.
    const auto invasion = invasions.find(invaded);
    if (invasion != invasions.end() &&
        invasion->second.dropped.insert(static_cast<int>(envelope.body_case())).second) {
        result.log.append(QStringLiteral("invasions: %1's message %2 dropped (an invader is in no "
                                         "world)")
                              .arg(QString::fromStdString(member.lobby))
                              .arg(static_cast<int>(envelope.body_case())));
    }
}

BloodborneCoop::InvasionTarget Service::TargetOf(const PartyWorld& world) const {
    InvasionTarget target;
    target.world = world.name.toStdString();
    target.invaded = invasions.contains(world.name);
    if (world.session) {
        const bbcoop::Team* team = lobby.TeamOf(world.session->host());
        target.session_open = team && team->started() && !team->party().empty() &&
                              FolderOf(QByteArray::fromStdString(team->party())) == world.name;
        target.allows_invaders = target.session_open && team->allow_invaders();
    }
    for (const auto& [key, member] : members) {
        if (member.party == world.name && !member.hostile) {
            ++target.players;
            target.members.push_back({member.map, member.level, member.boss});
        }
    }
    return target;
}

void Service::Match(qint64 now_ms, Result& result) {
    matched_ms = now_ms;
    cooldowns.Expire(now_ms);
    std::vector<InvasionTarget> targets;
    for (const auto& [name, world] : worlds) {
        if (world.team && world.session) {
            targets.push_back(TargetOf(world));
        }
    }
    for (auto search = searches.begin(); search != searches.end();) {
        const auto found = members.find(search->first);
        if (found == members.end()) {
            search = searches.erase(search);
            continue;
        }
        Member& invader = found->second;
        if (now_ms - search->second.since_ms >= InvasionSearchMs) {
            Refuse(invader, bbcoop::InvasionEnd::NOT_FOUND, "no world to invade now", now_ms);
            result.log.append(QStringLiteral("invasions: no world found for %1")
                                  .arg(QString::fromStdString(invader.lobby)));
            search = searches.erase(search);
            continue;
        }
        std::vector<InvasionTarget*> open;
        for (InvasionTarget& target : targets) {
            if (target.world != invader.party.toStdString() &&
                InvasionRefusal(target, invader.lobby, search->second.level, cooldowns, now_ms)
                    .empty()) {
                open.push_back(&target);
            }
        }
        if (open.empty()) {
            ++search;
            continue;
        }
        InvasionTarget& target =
            *open[QRandomGenerator::global()->bounded(static_cast<quint32>(open.size()))];
        target.invaded = true;
        const QString name = QString::fromStdString(target.world);
        const PartyWorld& world = worlds.at(name);
        Invasion& invasion = invasions[name];
        invasion.id = ++invasions_made;
        invasion.invader = invader.key;
        invasion.invader_name = invader.lobby;
        invasion.target = world.session->host();
        invasion.invader_level = search->second.level;
        invasion.invader_covenant = search->second.covenant_effect;
        invasion.matched_ms = now_ms;
        bbcoop::Envelope notice;
        notice.mutable_invasion_notice()->set_id(invasion.id);
        notice.mutable_invasion_notice()->set_in_ms(static_cast<quint32>(InvasionNoticeMs));
        notice.mutable_invasion_notice()->set_invader(invasion.invader_name);
        notice.mutable_invasion_notice()->set_target(invasion.target);
        SendTo(invader, notice, now_ms);
        for (auto& [key, member] : members) {
            if (member.party == name && !member.hostile) {
                SendTo(member, notice, now_ms);
            }
        }
        result.log.append(QStringLiteral("invasions: %1 approaches world %2 (invasion %3, hunting "
                                         "%4)")
                              .arg(QString::fromStdString(invasion.invader_name), name)
                              .arg(invasion.id)
                              .arg(QString::fromStdString(invasion.target)));
        search = searches.erase(search);
    }
}

void Service::JoinInvasion(PartyWorld& world, Invasion& invasion, qint64 now_ms, Result& result) {
    const auto found = members.find(invasion.invader);
    if (found == members.end()) {
        EndInvasion(world.name, bbcoop::InvasionEnd::INVADER_LEFT, now_ms, result);
        return;
    }
    if (!world.session) {
        EndInvasion(world.name, bbcoop::InvasionEnd::HOST_LEFT, now_ms, result);
        return;
    }
    Member& invader = found->second;
    invader.visiting = world.name;
    invader.hostile = true;
    invasion.joined = true;
    invasion.joined_ms = now_ms;
    // Its slot byte: the configured one, or the first free below the guests' 7/6/5.
    std::set<std::uint32_t> taken;
    if (const bbcoop::Team* team = lobby.TeamOf(world.session->host())) {
        for (const bbcoop::TeamMember& m : team->members()) {
            taken.insert(m.sign());
        }
    }
    std::uint32_t sign = invasion_rules.sign;
    for (std::uint32_t free = 4; taken.contains(sign) && free > 0; --free) {
        sign = free;
    }
    bbcoop::Envelope start;
    bbcoop::InvasionStart& message = *start.mutable_invasion_start();
    message.set_id(invasion.id);
    *message.mutable_session() = *world.session;
    message.mutable_session()->set_kind(invasion_rules.kind);
    message.mutable_session()->set_sign(sign);
    message.set_target(invasion.target);
    if (const auto place = world.places.find(invasion.target); place != world.places.end()) {
        *message.mutable_at() = place->second.at.at();
    }
    *message.mutable_owners() = world.owners.Message();
    bbcoop::Envelope joined;
    joined.mutable_invader_joined()->set_id(invasion.id);
    joined.mutable_invader_joined()->set_member(invasion.invader_name);
    joined.mutable_invader_joined()->set_target(invasion.target);
    joined.mutable_invader_joined()->set_name(invader.display);
    for (auto& [key, member] : members) {
        if (member.party == world.name && !member.hostile) {
            if (member.name == invasion.target) {
                message.set_map(member.map);
                invasion.target_level = member.level;
                invasion.target_covenant = member.covenant_effect;
            }
            SendTo(member, joined, now_ms);
        }
    }
    SendTo(invader, start, now_ms);
    result.log.append(QStringLiteral("invasions: %1 joins world %2 (invasion %3)")
                          .arg(QString::fromStdString(invasion.invader_name), world.name)
                          .arg(invasion.id));
}

void Service::EndInvasion(const QString& world, bbcoop::InvasionEnd::Reason reason, qint64 now_ms,
                          Result& result, bool killed_by_invader) {
    const auto found = invasions.find(world);
    if (found == invasions.end()) {
        return;
    }
    const Invasion invasion = found->second;
    // A failed persistence attempt publishes nothing and keeps the frozen entitlement alive.
    // A repeated dead Vitals can retry; successful publication retires before another message.

    if (invasion.joined) {
        for (auto& [key, member] : members) {
            if (reason == bbcoop::InvasionEnd::INVADER_DIED && member.party == world &&
                member.name == invasion.target && !member.hostile) {
                if (!RewardInvasion(member, invasion.id, invasion.invader_name,
                                    InvasionVictoryEchoes(invasion.invader_level, false),
                                    invasion.target_covenant, now_ms, result)) return;
                break;
            }
            if (reason == bbcoop::InvasionEnd::TARGET_KILLED && killed_by_invader &&
                member.key == invasion.invader) {
                if (!RewardInvasion(member, invasion.id, invasion.target,
                                    InvasionVictoryEchoes(invasion.target_level, true),
                                    invasion.invader_covenant, now_ms, result)) return;
                break;
            }
        }
    }
    invasions.erase(found); // durable grant/counter first; duplicate end/death cannot pay again
    bbcoop::Envelope envelope;
    envelope.mutable_invasion_end()->set_id(invasion.id);
    envelope.mutable_invasion_end()->set_reason(reason);
    envelope.mutable_invasion_end()->set_invader(invasion.invader_name);
    if (const auto invader = members.find(invasion.invader); invader != members.end()) {
        invader->second.visiting.clear();
        invader->second.hostile = false;
        SendTo(invader->second, envelope, now_ms);
    }
    for (auto& [key, member] : members) {
        if (member.party == world && !member.hostile) {
            SendTo(member, envelope, now_ms);
        }
    }
    cooldowns.Ended(world.toStdString(), invasion.invader_name, now_ms);
    result.log.append(
        QStringLiteral("invasions: invasion %1 of world %2 by %3 ends: %4")
            .arg(invasion.id)
            .arg(world, QString::fromStdString(invasion.invader_name),
                 QString::fromStdString(bbcoop::InvasionEnd::Reason_Name(reason)).toLower()));
}

void Service::CheckInvasion(const PartyWorld& world, const Member& member,
                            const bbcoop::Vitals& vitals, std::uint32_t wipe_before, qint64 now_ms,
                            Result& result) {
    const auto found = invasions.find(world.name);
    if (found == invasions.end()) {
        return;
    }
    if (vitals.boss()) {
        EndInvasion(world.name, bbcoop::InvasionEnd::BOSS, now_ms, result);
        return;
    }
    if (!found->second.joined) {
        return;
    }
    if (member.name == found->second.target && vitals.vital() == 3) { // Vital: dead
        if (const auto winner = members.find(found->second.invader);
            vitals.killed_by_invader() && winner != members.end() &&
            now_ms < winner->second.reward_retry_ms) return;
        result.log.append(
            QStringLiteral("invasions: %1 died%2")
                .arg(QString::fromStdString(member.name),
                     vitals.killed_by_invader() ? " by the invader's hand" : QString{}));
        EndInvasion(world.name, bbcoop::InvasionEnd::TARGET_KILLED, now_ms, result,
                    vitals.killed_by_invader());
        return;
    }
    const bbcoop::VitalsBoard board = world.boards.VitalsMessage();
    for (const auto& wipe : board.wipes()) {
        if (wipe.id() > wipe_before) {
            EndInvasion(world.name, bbcoop::InvasionEnd::PARTY_WIPED, now_ms, result);
            return;
        }
    }
}

QString Service::RewardFolder(const Member& member) const {
    return FolderOf(QByteArray{"invasion-rewards:"} + member.online_id.toHex());
}

Service::PartyWorld* Service::RewardMailbox(Member& member, bool create, qint64 now_ms,
                                                Result& result) {
    if (now_ms < member.reward_retry_ms) return nullptr;
    const QString name = RewardFolder(member);
    if (auto found = worlds.find(name); found != worlds.end() && found->second.reward_mailbox)
        return &found->second;
    const QString path = QDir{folder}.filePath(name);
    if (writer && writer->Pending(path)) writer->Drain();
    auto store = folder.isEmpty() ? nullptr : std::make_unique<WorldStore>(path);
    const auto stored = store ? store->LoadTrades() : std::nullopt;
    // A present unreadable ledger is never an empty ledger: resetting next_id could replay an
    // already committed award under an old id. Keep it intact for operator recovery.
    if (store && QFileInfo::exists(QDir{path}.filePath(TradesFile)) && !stored) {
        member.reward_retry_ms = now_ms + 1000;
        result.log.append(QStringLiteral("invasion rewards: unreadable mailbox, refusing payments"));
        return nullptr;
    }
    if (!stored && !create) return nullptr;
    if (!RoomFor(name, now_ms, result)) return nullptr;
    PartyWorld& mailbox = worlds[name];
    mailbox.name = name;
    mailbox.store = std::move(store);
    mailbox.reward_mailbox = true;
    if (stored) mailbox.trades.Restore(*stored, now_ms);
    return &mailbox;
}

bool Service::DeliverRewards(Member& member, PartyWorld& mailbox, TradeDesk staged,
                             TradeDesk::Result grants, qint64 now_ms, Result& result) {
    // Include unchanged resumes: they must never expose a state whose durability is uncertain.
    // Staging keeps a failed ACK from erasing the last confirmed in-memory pending grant.
    if (mailbox.store && !writer->SaveTradesDurable(mailbox.store->Folder(), staged.Stored())) {
        member.reward_retry_ms = now_ms + 1000;
        result.log.append(QStringLiteral("invasion rewards: persistence failed; no grant published"));
        return false;
    }
    member.reward_retry_ms = 0;
    mailbox.trades = std::move(staged);
    for (const TradeDesk::Send& send : grants.sends) {
        const bool needs_covenant = send.envelope.has_trade_grant() &&
            std::ranges::any_of(send.envelope.trade_grant().items(), [](const auto& item) {
                return item.kind() == static_cast<u32>(TradeItemKind::Goods);
            });
        if (send.to == member.lobby && member.invasion_rewards &&
            (!needs_covenant || member.covenant_rewards)) {
            SendTo(member, send.envelope, now_ms);
        }
    }
    for (const std::string& line : grants.log) {
        result.log.append(QStringLiteral("invasion rewards %1: %2")
                              .arg(QString::fromStdString(member.lobby), QString::fromStdString(line)));
    }
    return true;
}

void Service::ResumeRewards(Member& member, qint64 now_ms, Result& result) {
    if (PartyWorld* mailbox = RewardMailbox(member, false, now_ms, result)) {
        TradeDesk staged = mailbox->trades;
        auto grants = staged.Resume(member.lobby, now_ms);
        DeliverRewards(member, *mailbox, std::move(staged), std::move(grants), now_ms, result);
    }
}

bool Service::RewardInvasion(Member& winner, quint64 invasion, const std::string& defeated,
                             std::uint32_t echoes, std::uint32_t covenant_effect,
                             qint64 now_ms, Result& result) {
    if (!winner.invasion_rewards || winner.lobby.empty()) {
        result.log.append(QStringLiteral("invasions: victory %1 has no reward-capable winner")
                              .arg(invasion));
        return true; // no opted-in payment is owed
    }
    PartyWorld* mailbox = RewardMailbox(winner, true, now_ms, result);
    if (!mailbox) return false;
    TradeDesk staged = mailbox->trades;
    auto grants = staged.InvasionVictory(winner.lobby, invasion, defeated, echoes,
                       winner.covenant_rewards && covenant_effect == CorruptionCovenantEffect);
    return DeliverRewards(winner, *mailbox, std::move(staged), std::move(grants), now_ms, result);
}

QString Service::InvadedBy(quint64 link) const {
    for (const auto& [world, invasion] : invasions) {
        if (invasion.invader == link) {
            return world;
        }
    }
    return {};
}

void Service::InvasionLinkGone(quint64 link, qint64 now_ms, Result& result) {
    if (const QString invaded = InvadedBy(link); !invaded.isEmpty()) {
        EndInvasion(invaded, bbcoop::InvasionEnd::INVADER_LEFT, now_ms, result);
    }
    const auto found = members.find(link);
    if (found == members.end() || found->second.party.isEmpty()) {
        return;
    }
    const auto world = worlds.find(found->second.party);
    if (world != worlds.end() && world->second.session &&
        world->second.session->host() == found->second.name) {
        EndInvasion(world->first, bbcoop::InvasionEnd::HOST_LEFT, now_ms, result);
    }
}

} // namespace BloodborneWorld
