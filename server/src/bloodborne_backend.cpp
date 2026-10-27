// SPDX-FileCopyrightText: Copyright 2026 shadNet Project
// SPDX-License-Identifier: GPL-2.0-or-later
#include "bloodborne_backend.h"

#include <algorithm>
#include <functional>
#include <map>
#include <optional>
#include <random>
#include <set>

#include <QDateTime>
#include <QDebug>
#include <QFile>
#include <QRegularExpression>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QSqlRecord>
#include <QStringDecoder>
#include <QUuid>
#include <QVariant>

#include "ss_info_path.h"

namespace Bloodborne::Backend {

namespace {

// The game sends these when it means "any area" (first ghost query at boot).
bool IsWildcardArea(qint64 area) {
    return area == -1 || area == 4294967295LL;
}

// Messenger shell shown on a player's messages/bloodstains until they upload their own via
// MessengerShellUpload. Value observed as the common default.
const QString DefaultShellData = QStringLiteral("AQAAAAAAAAA=");
constexpr qint64 DefaultShellVersion = 2;

// Chalice glyph alphabet: digits 2-9 and lowercase minus l/o, as in the community's glyphs.
constexpr char GlyphAlphabet[] = "23456789abcdefghijkmnpqrstuvwxyz";

// ss.info has every element once per region number (the game reads its own: US 1, EU 2, JP and the
// Unlock Game Region patch 3, Asia 4), so these names get each number appended.
const QStringList SummonPollTags = {QStringLiteral("SummonDataCreateInterval"),
                                    QStringLiteral("SummonDataGetListInterval")};
// After a co-op session ends the host waits out these guard timers (retail 60 s) before it takes
// a new sign, which stalls every rejoin after a death. Same value as the poll interval.
const QStringList CoopGuardTags = {QStringLiteral("SummonDataGetListGuardTimer"),
                                   QStringLiteral("SummonDataGetListCoopGuardTimerClampTime")};
constexpr int RegionNumbers[] = {1, 2, 3, 4};

qint64 NowEpoch() {
    return QDateTime::currentSecsSinceEpoch();
}

// The community server's CreateDate/LastPlayDate format, local time.
QString NowIso() {
    return QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-ddTHH:mm:ss"));
}

Json Str(const QString& text) {
    return Json::String(text);
}

Json Int(qint64 value) {
    return Json::Int(value);
}

QString B64Text(const QString& text) {
    return QString::fromLatin1(text.toUtf8().toBase64());
}

// Python's str.strip(chars) for one character.
QString StripChar(const QString& text, QChar c) {
    qsizetype start = 0;
    qsizetype end = text.size();
    while (start < end && text[start] == c) {
        ++start;
    }
    while (end > start && text[end - 1] == c) {
        --end;
    }
    return text.mid(start, end - start);
}

// Contents of a UTF-8 text file with normalised newlines, or "" (read_text).
QString ReadText(const QString& path) {
    QFile file(path);
    if (path.isEmpty() || !file.open(QIODevice::ReadOnly)) {
        return {};
    }
    QStringDecoder decoder(QStringConverter::Utf8);
    QString text = decoder(file.readAll());
    text.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
    text.replace(u'\r', u'\n');
    return text;
}

struct Notice {
    Json entry;
    quint32 digest = 0;
};

// notice.txt or broadcast.txt: the first line is the title, the rest the body. The Id is a hash
// of the text, so editing it shows it again (the game drops an Id it already showed).
std::optional<Notice> LoadNotice(const QString& path) {
    const QString text = StripChar(ReadText(path), u'\n');
    if (text.trimmed().isEmpty()) {
        return std::nullopt;
    }
    const qsizetype newline = text.indexOf(u'\n');
    const QString title = newline < 0 ? text : text.left(newline);
    const QString body = newline < 0 ? QString() : text.mid(newline + 1);
    const quint32 digest = Crc32(text.toUtf8());
    const qint64 id = digest % 1'000'000'000;
    Json entry = Json::Object();
    entry.Set(QStringLiteral("Title"), Str(B64Text(title.trimmed())));
    entry.Set(QStringLiteral("Notice"), Str(B64Text(StripChar(body, u'\n'))));
    entry.Set(QStringLiteral("Id"), Int(id ? id : 1));
    return Notice{entry, digest};
}

// Deterministic per-account id for the no-store mode.
qint64 StableUserId(const QString& account) {
    return 1000 + Crc32(account.toUtf8()) % 9'000'000;
}

// [{'BloodMessId': 4833}, ...] -> [4833, ...], tolerating bare ints (id_list).
std::vector<qint64> IdList(const Json& items, const QString& key) {
    std::vector<qint64> out;
    for (const Json& item : Iterate(Truthy(items) ? items : Json::Array())) {
        const Json value = item.IsObject() ? item.Get(key) : item;
        if (!value.IsNull()) {
            out.push_back(AsInt(value));
        }
    }
    return out;
}

// req.get(key) or []
Json OrEmptyList(const Json& value) {
    return Truthy(value) ? value : Json::Array();
}

QVariant Bind(const Json& value) {
    switch (value.kind()) {
    case Json::Kind::Null:
        return QVariant(QMetaType::fromType<qlonglong>());
    case Json::Kind::Bool:
        return QVariant::fromValue<qlonglong>(value.BoolValue() ? 1 : 0);
    case Json::Kind::Int:
        return QVariant::fromValue<qlonglong>(value.IntValue());
    case Json::Kind::Float:
        return QVariant::fromValue(value.FloatValue());
    case Json::Kind::String:
        return QVariant(value.StringValue());
    default:
        throw PythonError("Error binding parameter: type not supported");
    }
}

QVariant BindInt(qint64 value) {
    return QVariant::fromValue<qlonglong>(value);
}

Json FromSql(const QVariant& value) {
    if (value.isNull()) {
        return Json();
    }
    switch (value.typeId()) {
    case QMetaType::LongLong:
    case QMetaType::Int:
    case QMetaType::UInt:
    case QMetaType::ULongLong:
    case QMetaType::Long:
    case QMetaType::ULong:
        return Int(value.toLongLong());
    case QMetaType::Double:
    case QMetaType::Float:
        return Json::Float(value.toDouble());
    default:
        return Str(value.toString());
    }
}

using Row = QSqlRecord;

Json Col(const Row& row, const QString& name) {
    return FromSql(row.value(name));
}

qint64 ColInt(const Row& row, const QString& name) {
    return row.value(name).toLongLong();
}

} // namespace

std::optional<QSet<QString>> ParseShowOwn(const QString& value) {
    QSet<QString> chosen;
    for (const QString& part : value.split(u',')) {
        const QString name = part.trimmed().toLower();
        if (!name.isEmpty()) {
            chosen.insert(name);
        }
    }
    if (chosen.contains(QStringLiteral("all"))) {
        return QSet<QString>(ShowOwnChoices.begin(), ShowOwnChoices.end());
    }
    for (const QString& name : chosen) {
        if (!ShowOwnChoices.contains(name)) {
            return std::nullopt;
        }
    }
    return chosen;
}

SsInfo BuildSsInfo(const QByteArray& templ, const QString& servicesUrl, const QString& summonUrl,
                   int summonPoll) {
    // Python's text mode reads every newline as "\n".
    QString xml = QString::fromUtf8(templ);
    xml.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
    xml.replace(u'\r', u'\n');
    QStringList missing;
    for (const QString placeholder : {QStringLiteral("{SERVICES}"), QStringLiteral("{SUMMON}")}) {
        if (!xml.contains(placeholder)) {
            missing.append(placeholder);
        }
    }
    if (!missing.isEmpty()) {
        return {{},
                QStringLiteral("the ss.info template has no %1 placeholder")
                    .arg(missing.join(QStringLiteral(" or ")))};
    }
    xml.replace(QStringLiteral("{SERVICES}"), servicesUrl);
    xml.replace(QStringLiteral("{SUMMON}"), summonUrl);
    for (const QString& base : SummonPollTags + CoopGuardTags) {
        for (const int number : RegionNumbers) {
            const QString tag = base + QString::number(number);
            const QString name = QRegularExpression::escape(tag);
            const QRegularExpression pattern(QStringLiteral("<%1>[^<]*</%1>").arg(name));
            xml.replace(pattern, QStringLiteral("<%1>%2</%1>").arg(tag).arg(summonPoll));
        }
    }
    if (xml.contains(QStringLiteral("thehuntersdream"))) {
        return {{}, QStringLiteral("the ss.info template still references thehuntersdream.com")};
    }
    return {xml.toUtf8().toBase64(), {}};
}

// --------------------------------------------------------------------------------------------
// Content store (only with a store file). Same tables as bb_dummy_server.py, so a store it
// wrote reads here unchanged.
// --------------------------------------------------------------------------------------------

namespace {

const char* const Schema[] = {
    R"(CREATE TABLE IF NOT EXISTS users (
    user_id       INTEGER PRIMARY KEY AUTOINCREMENT,
    account       TEXT NOT NULL UNIQUE,
    display       TEXT NOT NULL,
    shell_data    TEXT,
    shell_version INTEGER,
    created_at    INTEGER NOT NULL,
    last_login    INTEGER NOT NULL
))",
    R"(CREATE TABLE IF NOT EXISTS blood_messages (
    id                 INTEGER PRIMARY KEY AUTOINCREMENT,
    user_id            INTEGER NOT NULL,
    chara_id           TEXT,
    area_id            INTEGER NOT NULL,
    area_region_id     INTEGER NOT NULL,
    channel_id         INTEGER NOT NULL,
    blood_data         TEXT NOT NULL,
    blood_data_version INTEGER,
    chara_data         INTEGER,
    chara_data_version INTEGER,
    evaluate_plus      INTEGER NOT NULL DEFAULT 0,
    evaluate_minus     INTEGER NOT NULL DEFAULT 0,
    created_at         INTEGER NOT NULL
))",
    "CREATE INDEX IF NOT EXISTS ix_blood_area ON blood_messages(area_id, area_region_id, "
    "channel_id, created_at)",
    R"(CREATE TABLE IF NOT EXISTS ratings (
    blood_mess_id INTEGER NOT NULL,
    user_id       INTEGER NOT NULL,
    kind          INTEGER NOT NULL,
    created_at    INTEGER NOT NULL,
    PRIMARY KEY (blood_mess_id, user_id)
))",
    R"(CREATE TABLE IF NOT EXISTS tombstones (
    id                        INTEGER PRIMARY KEY AUTOINCREMENT,
    user_id                   INTEGER NOT NULL,
    chara_id                  TEXT,
    area_id                   INTEGER NOT NULL,
    area_region_id            INTEGER NOT NULL,
    channel_id                INTEGER NOT NULL,
    tomb_data                 TEXT NOT NULL,
    tomb_data_version         INTEGER,
    death_vision_data         TEXT,
    death_vision_data_version INTEGER,
    created_at                INTEGER NOT NULL
))",
    "CREATE INDEX IF NOT EXISTS ix_tomb_area ON tombstones(area_id, area_region_id, channel_id, "
    "created_at)",
    R"(CREATE TABLE IF NOT EXISTS ghosts (
    id                 INTEGER PRIMARY KEY AUTOINCREMENT,
    user_id            INTEGER NOT NULL,
    chara_id           TEXT,
    area_id            INTEGER NOT NULL,
    area_region_id     INTEGER NOT NULL,
    channel_id         INTEGER NOT NULL,
    matching_level     INTEGER,
    ghost_data         TEXT NOT NULL,
    ghost_data_version INTEGER,
    created_at         INTEGER NOT NULL
))",
    "CREATE INDEX IF NOT EXISTS ix_ghost_area ON ghosts(area_id, area_region_id, channel_id, "
    "created_at)",
    R"(CREATE TABLE IF NOT EXISTS lanterns (
    warp_info_id INTEGER NOT NULL,
    user_id      INTEGER NOT NULL,
    channel_id   INTEGER NOT NULL,
    seen_at      INTEGER NOT NULL,
    PRIMARY KEY (warp_info_id, user_id)
))",
    R"(CREATE TABLE IF NOT EXISTS chalice_dungeons (
    channel_id         INTEGER PRIMARY KEY AUTOINCREMENT,
    user_id            INTEGER NOT NULL,
    chara_id           TEXT,
    form_data          TEXT NOT NULL,
    form_data_version  INTEGER,
    discernment_word   TEXT NOT NULL UNIQUE,
    holy_grail_type_id INTEGER,
    ritual_level       INTEGER,
    fixed_or_general   INTEGER,
    share_level        INTEGER NOT NULL DEFAULT 0,
    status             INTEGER,
    sub_feature_flag   INTEGER,
    turnout_level      INTEGER NOT NULL DEFAULT 0,
    unlock_flags       TEXT NOT NULL DEFAULT '[]',
    wish_materials     TEXT NOT NULL DEFAULT '[]',
    create_date        TEXT NOT NULL,
    last_play_date     TEXT NOT NULL
))",
};

const QString ListSql = QStringLiteral("SELECT t.*, u.shell_data, u.shell_version FROM %1 t "
                                       "LEFT JOIN users u ON u.user_id = t.user_id WHERE %2 "
                                       "ORDER BY t.created_at DESC, t.id DESC LIMIT ?");

QString Marks(size_t count) {
    QStringList marks;
    for (size_t i = 0; i < count; ++i) {
        marks.append(QStringLiteral("?"));
    }
    return marks.join(u',');
}

std::pair<Json, Json> ShellOf(const Row& row) {
    const Json data = Col(row, QStringLiteral("shell_data"));
    if (Truthy(data)) {
        return {data, Col(row, QStringLiteral("shell_version"))};
    }
    return {Str(DefaultShellData), Int(DefaultShellVersion)};
}

} // namespace

class ContentStore {
public:
    ContentStore(QSet<QString> showOwn, qint64 ghostTtl, qint64 lanternTtl, int tombCap)
        : m_showOwn(std::move(showOwn)), m_ghostTtl(ghostTtl), m_lanternTtl(lanternTtl),
          m_tombCap(tombCap), m_connection(QStringLiteral("bloodborne_backend_%1")
                                               .arg(reinterpret_cast<quintptr>(this), 0, 16)) {}

    ~ContentStore() {
        {
            QSqlDatabase db = QSqlDatabase::database(m_connection, false);
            if (db.isOpen()) {
                db.close();
            }
        }
        QSqlDatabase::removeDatabase(m_connection);
    }

    bool Open(const QString& path, QString* error) {
        QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), m_connection);
        db.setDatabaseName(path);
        if (!db.open()) {
            *error = db.lastError().text();
            return false;
        }
        try {
            Exec(QStringLiteral("PRAGMA journal_mode=WAL"));
            Exec(QStringLiteral("PRAGMA synchronous=NORMAL"));
            Transaction tx(Db());
            for (const char* statement : Schema) {
                Exec(QString::fromLatin1(statement));
            }
            tx.Commit();
        } catch (const PythonError& e) {
            *error = QString::fromUtf8(e.what());
            return false;
        }
        return true;
    }

    bool HideOwn(const QString& kind) const {
        return !m_showOwn.contains(kind);
    }

    Json Counts() {
        Json counts = Json::Object();
        for (const QString table : {QStringLiteral("users"), QStringLiteral("blood_messages"),
                                    QStringLiteral("tombstones"), QStringLiteral("ghosts"),
                                    QStringLiteral("chalice_dungeons")}) {
            const auto rows = Query(QStringLiteral("SELECT COUNT(*) AS c FROM ") + table);
            counts.Set(table, Int(ColInt(rows.front(), QStringLiteral("c"))));
        }
        return counts;
    }

    // -- users ----------------------------------------------------------------------------

    std::optional<qint64> UserIdOf(const QString& account) {
        const auto rows =
            Query(QStringLiteral("SELECT user_id FROM users WHERE account=?"), {account.toLower()});
        if (rows.empty()) {
            return std::nullopt;
        }
        return ColInt(rows.front(), QStringLiteral("user_id"));
    }

    qint64 Login(const QString& account) {
        const qint64 now = NowEpoch();
        Transaction tx(Db());
        Exec(QStringLiteral(
                 "INSERT INTO users (account, display, created_at, last_login) VALUES (?,?,?,?) "
                 "ON CONFLICT(account) DO UPDATE SET display=excluded.display, "
                 "last_login=excluded.last_login"),
             {account.toLower(), account, BindInt(now), BindInt(now)});
        const auto rows =
            Query(QStringLiteral("SELECT user_id FROM users WHERE account=?"), {account.toLower()});
        tx.Commit();
        return ColInt(rows.front(), QStringLiteral("user_id"));
    }

    void SetShell(qint64 userId, const Json& data, const Json& version) {
        Transaction tx(Db());
        Exec(QStringLiteral("UPDATE users SET shell_data=?, shell_version=? WHERE user_id=?"),
             {Bind(data), Bind(version), BindInt(userId)});
        tx.Commit();
    }

    // -- blood messages -------------------------------------------------------------------

    static Json MessageEntry(const Row& row) {
        const auto [shell, version] = ShellOf(row);
        Json entry = Json::Object();
        entry.Set(QStringLiteral("BloodMessId"), Col(row, QStringLiteral("id")));
        entry.Set(QStringLiteral("BloodData"), Col(row, QStringLiteral("blood_data")));
        entry.Set(QStringLiteral("BloodDataVersion"),
                  Col(row, QStringLiteral("blood_data_version")));
        entry.Set(QStringLiteral("EvaluatePlus"), Col(row, QStringLiteral("evaluate_plus")));
        entry.Set(QStringLiteral("EvaluateMinus"), Col(row, QStringLiteral("evaluate_minus")));
        entry.Set(QStringLiteral("MessShellInfo"), shell);
        entry.Set(QStringLiteral("MessShellInfoVersion"), version);
        return entry;
    }

    std::vector<std::pair<qint64, Json>> CreateMessages(qint64 userId, const QString& chara,
                                                        const Json& entries) {
        const qint64 now = NowEpoch();
        std::vector<std::pair<qint64, Json>> pairs;
        Transaction tx(Db());
        for (const Json& e : Iterate(entries)) {
            if (!e.IsObject()) {
                continue;
            }
            QSqlQuery query = Exec(
                QStringLiteral(
                    "INSERT INTO blood_messages (user_id, chara_id, area_id, area_region_id, "
                    "channel_id, blood_data, blood_data_version, chara_data, chara_data_version, "
                    "created_at) VALUES (?,?,?,?,?,?,?,?,?,?)"),
                {BindInt(userId), chara, BindInt(AsInt(e.Get(QStringLiteral("AreaId")), -1)),
                 BindInt(AsInt(e.Get(QStringLiteral("AreaRegionId")), -1)),
                 BindInt(AsInt(e.Get(QStringLiteral("ChannelId")))),
                 Bind(e.Get(QStringLiteral("BloodData"), Str(QString()))),
                 Bind(e.Get(QStringLiteral("BloodDataVersion"))),
                 BindInt(Truthy(e.Get(QStringLiteral("CharaData"))) ? 1 : 0),
                 Bind(e.Get(QStringLiteral("CharaDataVersion"))), BindInt(now)});
            pairs.emplace_back(query.lastInsertId().toLongLong(),
                               e.Get(QStringLiteral("PrevBloodMessId")));
        }
        tx.Commit();
        return pairs;
    }

    Json ListMessages(qint64 userId, const Json& areas, const Json& getMax) {
        // Players see their own notes in the world, so these are never hidden.
        Json list = Json::Array();
        for (const Row& row :
             ListByAreas(QStringLiteral("blood_messages"), areas, userId, false, getMax)) {
            list.Append(MessageEntry(row));
        }
        return list;
    }

    // EvaluateKind 1 = Fine (+), -1 = Foul (-). One vote per player.
    std::optional<Json> Evaluate(qint64 userId, qint64 messId, const Json& kind) {
        const qint64 how = AsInt(kind);
        const QString column = how == 1    ? QStringLiteral("evaluate_plus")
                               : how == -1 ? QStringLiteral("evaluate_minus")
                                           : QString();
        Transaction tx(Db());
        if (!column.isEmpty()) {
            QSqlQuery insert =
                Exec(QStringLiteral("INSERT OR IGNORE INTO ratings (blood_mess_id, user_id, kind, "
                                    "created_at) VALUES (?,?,?,?)"),
                     {BindInt(messId), BindInt(userId), BindInt(how), BindInt(NowEpoch())});
            if (insert.numRowsAffected() > 0) {
                Exec(QStringLiteral("UPDATE blood_messages SET %1 = %1 + 1 WHERE id=?").arg(column),
                     {BindInt(messId)});
            }
        }
        const auto rows =
            Query(ListSql.arg(QStringLiteral("blood_messages"), QStringLiteral("t.id = ?")),
                  {BindInt(messId), BindInt(1)});
        tx.Commit();
        if (rows.empty()) {
            return std::nullopt;
        }
        return MessageEntry(rows.front());
    }

    void DeleteMessage(qint64 userId, qint64 messId) {
        Transaction tx(Db());
        Exec(QStringLiteral("DELETE FROM blood_messages WHERE id=? AND user_id=?"),
             {BindInt(messId), BindInt(userId)});
        Exec(QStringLiteral("DELETE FROM ratings WHERE blood_mess_id=?"), {BindInt(messId)});
        tx.Commit();
    }

    std::vector<qint64> LostMessages(const std::vector<qint64>& ids) {
        if (ids.empty()) {
            return {};
        }
        QSet<qint64> present;
        for (const Row& row : Query(QStringLiteral("SELECT id FROM blood_messages WHERE id IN (%1)")
                                        .arg(Marks(ids.size())),
                                    BindAll(ids))) {
            present.insert(ColInt(row, QStringLiteral("id")));
        }
        std::vector<qint64> lost;
        for (const qint64 id : ids) {
            if (!present.contains(id)) {
                lost.push_back(id);
            }
        }
        return lost;
    }

    // INFERRED in the Python server: never observed with content. The message entry's rating
    // fields, for messages that have any rating at all.
    Json EvaluationsFor(const std::vector<qint64>& ids) {
        Json list = Json::Array();
        if (ids.empty()) {
            return list;
        }
        for (const Row& row :
             Query(QStringLiteral("SELECT id, evaluate_plus, evaluate_minus FROM blood_messages "
                                  "WHERE id IN (%1) AND evaluate_plus + evaluate_minus > 0")
                       .arg(Marks(ids.size())),
                   BindAll(ids))) {
            Json entry = Json::Object();
            entry.Set(QStringLiteral("BloodMessId"), Col(row, QStringLiteral("id")));
            entry.Set(QStringLiteral("EvaluatePlus"), Col(row, QStringLiteral("evaluate_plus")));
            entry.Set(QStringLiteral("EvaluateMinus"), Col(row, QStringLiteral("evaluate_minus")));
            list.Append(entry);
        }
        return list;
    }

    // -- tombstones -----------------------------------------------------------------------

    qint64 CreateTomb(qint64 userId, const QString& chara, const Json& req) {
        const QVariantList key = AreaKey(req);
        Transaction tx(Db());
        QSqlQuery insert = Exec(
            QStringLiteral("INSERT INTO tombstones (user_id, chara_id, area_id, area_region_id, "
                           "channel_id, tomb_data, tomb_data_version, death_vision_data, "
                           "death_vision_data_version, created_at) VALUES (?,?,?,?,?,?,?,?,?,?)"),
            QVariantList{BindInt(userId), chara} + key +
                QVariantList{Bind(req.Get(QStringLiteral("TombData"), Str(QString()))),
                             Bind(req.Get(QStringLiteral("TombDataVersion"))),
                             Bind(req.Get(QStringLiteral("DeathVisionData"), Str(QString()))),
                             Bind(req.Get(QStringLiteral("DeathVisionDataVersion"))),
                             BindInt(NowEpoch())});
        const qint64 id = insert.lastInsertId().toLongLong();
        // Keep only the newest N bloodstains per area, like the game's own cap.
        Exec(QStringLiteral(
                 "DELETE FROM tombstones WHERE area_id=? AND area_region_id=? AND channel_id=? "
                 "AND id NOT IN (SELECT id FROM tombstones WHERE area_id=? AND area_region_id=? "
                 "AND channel_id=? ORDER BY created_at DESC, id DESC LIMIT ?)"),
             key + key + QVariantList{BindInt(m_tombCap)});
        tx.Commit();
        return id;
    }

    Json ListTombs(qint64 userId, const Json& areas, const Json& getMax) {
        Json list = Json::Array();
        for (const Row& row : ListByAreas(QStringLiteral("tombstones"), areas, userId,
                                          HideOwn(QStringLiteral("deaths")), getMax)) {
            Json entry = Json::Object();
            entry.Set(QStringLiteral("TombMessId"), Col(row, QStringLiteral("id")));
            entry.Set(QStringLiteral("TombData"), Col(row, QStringLiteral("tomb_data")));
            entry.Set(QStringLiteral("MessShellInfo"), ShellOf(row).first);
            list.Append(entry);
        }
        return list;
    }

    std::optional<std::pair<Json, Json>> DeathVision(qint64 tombId) {
        const auto rows = Query(QStringLiteral("SELECT death_vision_data, "
                                               "death_vision_data_version FROM tombstones "
                                               "WHERE id=?"),
                                {BindInt(tombId)});
        if (rows.empty()) {
            return std::nullopt;
        }
        return std::pair{Col(rows.front(), QStringLiteral("death_vision_data")),
                         Col(rows.front(), QStringLiteral("death_vision_data_version"))};
    }

    // -- wandering ghosts -----------------------------------------------------------------

    qint64 CreateGhost(qint64 userId, const QString& chara, const Json& req) {
        const QVariantList key = AreaKey(req);
        const qint64 now = NowEpoch();
        Transaction tx(Db());
        Exec(QStringLiteral("DELETE FROM ghosts WHERE created_at < ?"),
             {BindInt(now - m_ghostTtl)});
        // One ghost per player per area: the game uploads a new snippet every ~60s, and a
        // friend should appear once, not 20 times.
        Exec(QStringLiteral("DELETE FROM ghosts WHERE user_id=? AND area_id=? AND "
                            "area_region_id=? AND channel_id=?"),
             QVariantList{BindInt(userId)} + key);
        QSqlQuery insert =
            Exec(QStringLiteral("INSERT INTO ghosts (user_id, chara_id, area_id, area_region_id, "
                                "channel_id, matching_level, ghost_data, ghost_data_version, "
                                "created_at) VALUES (?,?,?,?,?,?,?,?,?)"),
                 QVariantList{BindInt(userId), chara} + key +
                     QVariantList{
                         Bind(req.Get(QStringLiteral("MatchingLevel"))),
                         Bind(req.Get(QStringLiteral("WanderingGhostData"), Str(QString()))),
                         Bind(req.Get(QStringLiteral("WanderingGhostDataVersion"))), BindInt(now)});
        const qint64 id = insert.lastInsertId().toLongLong();
        tx.Commit();
        return id;
    }

    Json ListGhosts(qint64 userId, const Json& areas, const Json& getMax) {
        const auto rows = ListByAreas(QStringLiteral("ghosts"), areas, userId,
                                      HideOwn(QStringLiteral("ghosts")), getMax);
        const qint64 cutoff = NowEpoch() - m_ghostTtl;
        Json list = Json::Array();
        for (const Row& row : rows) {
            if (ColInt(row, QStringLiteral("created_at")) < cutoff) {
                continue;
            }
            Json entry = Json::Object();
            entry.Set(QStringLiteral("WanderingGhostId"), Col(row, QStringLiteral("id")));
            entry.Set(QStringLiteral("WanderingGhostData"), Col(row, QStringLiteral("ghost_data")));
            entry.Set(QStringLiteral("WanderingGhostDataVersion"),
                      Col(row, QStringLiteral("ghost_data_version")));
            list.Append(entry);
        }
        return list;
    }

    // -- lanterns -------------------------------------------------------------------------

    void TouchLantern(qint64 userId, qint64 warpId, qint64 channelId) {
        Transaction tx(Db());
        Exec(QStringLiteral("INSERT INTO lanterns (warp_info_id, user_id, channel_id, seen_at) "
                            "VALUES (?,?,?,?) ON CONFLICT(warp_info_id, user_id) DO UPDATE SET "
                            "seen_at=excluded.seen_at, channel_id=excluded.channel_id"),
             {BindInt(warpId), BindInt(userId), BindInt(channelId), BindInt(NowEpoch())});
        tx.Commit();
    }

    // The community server answered with every lantern that had players, not just the ones
    // asked about, so this does the same.
    Json LanternCounts(qint64 userId) {
        QString where = QStringLiteral("seen_at >= ?");
        QVariantList params{BindInt(NowEpoch() - m_lanternTtl)};
        if (HideOwn(QStringLiteral("lanterns"))) {
            where += QStringLiteral(" AND user_id != ?");
            params.append(BindInt(userId));
        }
        Json list = Json::Array();
        for (const Row& row :
             Query(QStringLiteral("SELECT warp_info_id, COUNT(*) AS c FROM lanterns WHERE %1 "
                                  "GROUP BY warp_info_id ORDER BY warp_info_id")
                       .arg(where),
                   params)) {
            Json entry = Json::Object();
            entry.Set(QStringLiteral("WarpInfoId"), Col(row, QStringLiteral("warp_info_id")));
            entry.Set(QStringLiteral("ChairMessCount"), Col(row, QStringLiteral("c")));
            list.Append(entry);
        }
        return list;
    }

    // -- chalice dungeons -----------------------------------------------------------------

    static Json DungeonRecord(const Row& row) {
        const Json chara = Col(row, QStringLiteral("chara_id"));
        qint64 charaId = 0;
        if (chara.IsString() && !chara.StringValue().isEmpty()) {
            bool digits = true;
            for (const QChar c : chara.StringValue()) {
                digits = digits && c.isDigit();
            }
            if (digits) {
                charaId = AsInt(chara);
            }
        }
        const auto parsed = [&row](const QString& name) {
            return ParseJson(row.value(name).toString().toUtf8()).value_or(Json::Array());
        };
        Json record = Json::Object();
        record.Set(QStringLiteral("ChannelId"), Col(row, QStringLiteral("channel_id")));
        record.Set(QStringLiteral("ShareLevel"), Col(row, QStringLiteral("share_level")));
        record.Set(QStringLiteral("Status"), Col(row, QStringLiteral("status")));
        record.Set(QStringLiteral("TurnoutLevel"), Col(row, QStringLiteral("turnout_level")));
        record.Set(QStringLiteral("FormDataVersion"),
                   Col(row, QStringLiteral("form_data_version")));
        record.Set(QStringLiteral("FormData"), Col(row, QStringLiteral("form_data")));
        record.Set(QStringLiteral("DiscernmentWord"), Col(row, QStringLiteral("discernment_word")));
        record.Set(QStringLiteral("RitualLevel"), Col(row, QStringLiteral("ritual_level")));
        record.Set(QStringLiteral("HolyGrailTypeId"),
                   Col(row, QStringLiteral("holy_grail_type_id")));
        record.Set(QStringLiteral("FixedOrGeneral"), Col(row, QStringLiteral("fixed_or_general")));
        record.Set(QStringLiteral("SubFeatureFlag"), Col(row, QStringLiteral("sub_feature_flag")));
        record.Set(QStringLiteral("CreateUserId"), Col(row, QStringLiteral("user_id")));
        record.Set(QStringLiteral("CreateCharaId"), Int(charaId));
        record.Set(QStringLiteral("CreateDate"), Col(row, QStringLiteral("create_date")));
        record.Set(QStringLiteral("LastPlayDate"), Col(row, QStringLiteral("last_play_date")));
        record.Set(QStringLiteral("WishMaterialList"), parsed(QStringLiteral("wish_materials")));
        record.Set(QStringLiteral("UnlockFlagList"), parsed(QStringLiteral("unlock_flags")));
        return record;
    }

    std::pair<qint64, QString> UploadDungeon(qint64 userId, const QString& chara, const Json& req) {
        const QString stamp = NowIso();
        Transaction tx(Db());
        QString glyph;
        for (int attempt = 0; attempt < 50; ++attempt) {
            glyph = RandomGlyph();
            if (Query(QStringLiteral("SELECT 1 FROM chalice_dungeons WHERE discernment_word=?"),
                      {glyph})
                    .empty()) {
                break;
            }
        }
        QSqlQuery insert = Exec(
            QStringLiteral(
                "INSERT INTO chalice_dungeons (user_id, chara_id, form_data, form_data_version, "
                "discernment_word, holy_grail_type_id, ritual_level, fixed_or_general, "
                "share_level, status, sub_feature_flag, unlock_flags, wish_materials, "
                "create_date, last_play_date) VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)"),
            {BindInt(userId), chara, Bind(req.Get(QStringLiteral("FormData"), Str(QString()))),
             Bind(req.Get(QStringLiteral("FormDataVersion"))), glyph,
             Bind(req.Get(QStringLiteral("HolyGrailTypeId"))),
             Bind(req.Get(QStringLiteral("RitualLevel"))),
             Bind(req.Get(QStringLiteral("FixedOrGeneral"))),
             BindInt(AsInt(req.Get(QStringLiteral("ShareLevel")))),
             Bind(req.Get(QStringLiteral("Status"))),
             Bind(req.Get(QStringLiteral("SubFeatureFlag"))),
             QString::fromUtf8(
                 DumpJsonSpaced(OrEmptyList(req.Get(QStringLiteral("UnlockFlagList"))))),
             QString::fromUtf8(
                 DumpJsonSpaced(OrEmptyList(req.Get(QStringLiteral("WishMaterialList"))))),
             stamp, stamp});
        const qint64 id = insert.lastInsertId().toLongLong();
        tx.Commit();
        return {id, glyph};
    }

    void ShareDungeon(qint64 userId, qint64 channelId, const Json& level) {
        Transaction tx(Db());
        Exec(QStringLiteral(
                 "UPDATE chalice_dungeons SET share_level=? WHERE channel_id=? AND user_id=?"),
             {BindInt(AsInt(level)), BindInt(channelId), BindInt(userId)});
        tx.Commit();
    }

    // Shared dungeons matching the game's filters. FixedOrGeneralList is an "in" filter;
    // HolyGrailTypeId, RitualLevel, SubFeatureFlag and Status match exactly when given and mean
    // "any" when null (0 is a real value). UnlockedFlagList is deliberately ignored: its bitmask
    // rule isn't fully known, and over-returning beats hiding a friend's dungeon.
    Json SearchDungeons(const Json& req) {
        QStringList where{QStringLiteral("share_level >= 1")};
        QVariantList params;
        const auto kinds =
            IdList(req.Get(QStringLiteral("FixedOrGeneralList")), QStringLiteral("FixedOrGeneral"));
        if (!kinds.empty()) {
            where.append(QStringLiteral("fixed_or_general IN (%1)").arg(Marks(kinds.size())));
            params += BindAll(kinds);
        }
        const std::pair<const char*, const char*> filters[] = {
            {"HolyGrailTypeId", "holy_grail_type_id"},
            {"RitualLevel", "ritual_level"},
            {"SubFeatureFlag", "sub_feature_flag"},
            {"Status", "status"}};
        for (const auto& [key, column] : filters) {
            const Json value = req.Get(QString::fromLatin1(key));
            if (!value.IsNull()) {
                where.append(QString::fromLatin1(column) + QStringLiteral(" = ?"));
                params.append(BindInt(AsInt(value)));
            }
        }
        qint64 limit = AsInt(req.Get(QStringLiteral("GetCount")), 100);
        if (!limit) {
            limit = 100;
        }
        params.append(BindInt(limit));
        Json list = Json::Array();
        for (const Row& row : Query(QStringLiteral("SELECT * FROM chalice_dungeons WHERE %1 "
                                                   "ORDER BY channel_id DESC LIMIT ?")
                                        .arg(where.join(QStringLiteral(" AND "))),
                                    params)) {
            list.Append(DungeonRecord(row));
        }
        return list;
    }

    // Every dungeon uploaded, newest first (of that depth and area when given).
    std::vector<Json> AllDungeons(const Json& level, const Json& area) {
        QStringList where{QStringLiteral("1")};
        QVariantList params;
        const std::pair<const Json*, const char*> filters[] = {{&level, "ritual_level"},
                                                               {&area, "holy_grail_type_id"}};
        for (const auto& [value, column] : filters) {
            if (!value->IsNull()) {
                where.append(QString::fromLatin1(column) + QStringLiteral(" = ?"));
                params.append(BindInt(AsInt(*value)));
            }
        }
        std::vector<Json> list;
        for (const Row& row : Query(QStringLiteral("SELECT * FROM chalice_dungeons WHERE %1 "
                                                   "ORDER BY channel_id DESC")
                                        .arg(where.join(QStringLiteral(" AND "))),
                                    params)) {
            list.push_back(DungeonRecord(row));
        }
        return list;
    }

    std::optional<Json> WordSearch(const Json& word) {
        if (!Truthy(word)) {
            return std::nullopt;
        }
        const auto rows = Query(QStringLiteral("SELECT * FROM chalice_dungeons WHERE "
                                               "lower(discernment_word)=lower(?)"),
                                {PyStr(word).trimmed()});
        if (rows.empty()) {
            return std::nullopt;
        }
        return DungeonRecord(rows.front());
    }

    std::pair<Json, std::vector<qint64>> DungeonsByIds(const std::vector<qint64>& ids) {
        Json found = Json::Array();
        if (ids.empty()) {
            return {found, {}};
        }
        std::map<qint64, Json> byId;
        for (const Row& row :
             Query(QStringLiteral("SELECT * FROM chalice_dungeons WHERE channel_id IN (%1)")
                       .arg(Marks(ids.size())),
                   BindAll(ids))) {
            byId[ColInt(row, QStringLiteral("channel_id"))] = DungeonRecord(row);
        }
        std::vector<qint64> lost;
        for (const qint64 id : ids) {
            const auto it = byId.find(id);
            if (it != byId.end()) {
                found.Append(it->second);
            } else {
                lost.push_back(id);
            }
        }
        return {found, lost};
    }

private:
    // `with self.conn:` in the Python server: commits when the block finishes, rolls back when
    // it raises.
    class Transaction {
    public:
        explicit Transaction(QSqlDatabase db) : m_db(std::move(db)) {
            m_db.transaction();
        }
        ~Transaction() {
            if (!m_done) {
                m_db.rollback();
            }
        }
        void Commit() {
            m_db.commit();
            m_done = true;
        }

    private:
        QSqlDatabase m_db;
        bool m_done = false;
    };

    QSqlDatabase Db() const {
        return QSqlDatabase::database(m_connection, false);
    }

    QSqlQuery Exec(const QString& sql, const QVariantList& params = {}) {
        QSqlQuery query(Db());
        if (!query.prepare(sql)) {
            throw PythonError(query.lastError().text().toStdString());
        }
        for (const QVariant& param : params) {
            query.addBindValue(param);
        }
        if (!query.exec()) {
            throw PythonError(query.lastError().text().toStdString());
        }
        return query;
    }

    std::vector<Row> Query(const QString& sql, const QVariantList& params = {}) {
        QSqlQuery query = Exec(sql, params);
        std::vector<Row> rows;
        while (query.next()) {
            rows.push_back(query.record());
        }
        return rows;
    }

    static QVariantList BindAll(const std::vector<qint64>& ids) {
        QVariantList params;
        for (const qint64 id : ids) {
            params.append(BindInt(id));
        }
        return params;
    }

    static QVariantList AreaKey(const Json& req) {
        return {BindInt(AsInt(req.Get(QStringLiteral("AreaId")), -1)),
                BindInt(AsInt(req.Get(QStringLiteral("AreaRegionId")), -1)),
                BindInt(AsInt(req.Get(QStringLiteral("ChannelId"))))};
    }

    static QString RandomGlyph() {
        static std::random_device device;
        constexpr int Size = sizeof(GlyphAlphabet) - 1;
        std::uniform_int_distribution<int> pick(0, Size - 1);
        QString glyph;
        for (int i = 0; i < 8; ++i) {
            glyph.append(QChar::fromLatin1(GlyphAlphabet[pick(device)]));
        }
        return glyph;
    }

    // One AreaInfoList/AreaList entry as a WHERE fragment (wildcards honoured).
    static std::pair<QString, QVariantList> AreaClause(const Json& area) {
        QStringList clauses;
        QVariantList params;
        const qint64 areaId = AsInt(area.Get(QStringLiteral("AreaId")), -1);
        if (!IsWildcardArea(areaId)) {
            clauses.append(QStringLiteral("t.area_id = ?"));
            params.append(BindInt(areaId));
        }
        const qint64 region = AsInt(area.Get(QStringLiteral("AreaRegionId")), -1);
        if (region != -1) {
            clauses.append(QStringLiteral("t.area_region_id = ?"));
            params.append(BindInt(region));
        }
        const qint64 channel = AsInt(area.Get(QStringLiteral("ChannelId")), -1);
        if (channel != -1) {
            clauses.append(QStringLiteral("t.channel_id = ?"));
            params.append(BindInt(channel));
        }
        return {clauses.isEmpty() ? QStringLiteral("1=1") : clauses.join(QStringLiteral(" AND ")),
                params};
    }

    std::vector<Row> ListByAreas(const QString& table, const Json& areas, qint64 userId,
                                 bool hideOwn, const Json& getMaxValue) {
        std::vector<Row> out;
        QSet<qint64> seen;
        const qint64 getMax = AsInt(getMaxValue, 0);
        for (const Json& area : Iterate(areas)) {
            if (!area.IsObject()) {
                continue;
            }
            if (getMax && static_cast<qint64>(out.size()) >= getMax) {
                break;
            }
            auto [where, params] = AreaClause(area);
            if (hideOwn) {
                where += QStringLiteral(" AND t.user_id != ?");
                params.append(BindInt(userId));
            }
            qint64 limit = AsInt(area.Get(QStringLiteral("GetCount")), 30);
            if (!limit) {
                limit = 30;
            }
            if (getMax) {
                limit = std::min(limit, getMax - static_cast<qint64>(out.size()));
            }
            params.append(BindInt(limit));
            for (const Row& row : Query(ListSql.arg(table, where), params)) {
                const qint64 id = ColInt(row, QStringLiteral("id"));
                if (!seen.contains(id)) { // overlapping/wildcard entries
                    seen.insert(id);
                    out.push_back(row);
                }
            }
        }
        return out;
    }

    QSet<QString> m_showOwn;
    qint64 m_ghostTtl;
    qint64 m_lanternTtl;
    int m_tombCap;
    QString m_connection;
};

// --------------------------------------------------------------------------------------------
// Services. Names match the game's api_<Name> entries; each adds its fields next to
// ResKind/MessageId.
// --------------------------------------------------------------------------------------------

namespace {

struct Context {
    ContentStore& store;
    const Json& req;
    qint64 uid;
    QString chara;
};

using StoreHandler = std::function<void(Context&, Json&)>;

Json AreasOf(const Json& req, const QString& key) {
    return OrEmptyList(req.Get(key));
}

Json IdObjects(const std::vector<qint64>& ids, const QString& key) {
    Json list = Json::Array();
    for (const qint64 id : ids) {
        Json entry = Json::Object();
        entry.Set(key, Int(id));
        list.Append(entry);
    }
    return list;
}

// Standard chalice dungeons. A standard (not root) chalice's ritual asks for the one dungeon the
// server keeps for that chalice: ChannelSearch with FixedOrGeneral 2 and GetCount 1, then its
// details by ChannelId. Without one the game says "Failed to contribute ritual materials" and
// returns to the title. The server makes them itself. FormData is the dungeon as the game uploads
// one (ChannelUpload after a root chalice's ritual, format 5, little endian): u32 5, u32 140;
// layout, rites, depth*10 + area (0 Pthumeru, 1 Hintertomb, 2 Loran, 3 Isz), 0x1D; u32 the
// chalice's goods id; 9 feature slots of {u32 id, s32 code} (-1 for none); the conductor's name,
// 16 bytes ASCII, and 32 bytes UTF-16; zeros to 140. A ritual takes the chalice from the player's
// hands but its layout from the record's rites byte: 90 is a standard chalice's story layout,
// m29_<key>_90_00 (0 gave the story Pthumeru Chalice the root layout m29_10_00_00 and its boss the
// root loot, rig BB_FILE_TRACE 2026-10-21); a join through Chalice Dungeon Search builds it from
// FormData alone. The slots are the game's own for the chalice: HolygrailExParam row <goods> names
// eight slot ids, DungeonSubFeatLotParam gives each id its one code (a DungeonFeatureParam row).
// The ritual sets the altar's flag 5300 + each code's feature flag, and the dungeon's last bosses
// give the next chalice only with 5378 or 5385 on (m29 event 12901693): the root slots these had
// before (9900000 + key*1000 + 30, 35, 50, ...) gave none (Kaniel's playtest 2026-10-25). Keyed by
// RitualLevel * 10 + HolyGrailTypeId: (rites, goods id, feature slots).
struct StandardSlot {
    quint32 id;
    qint32 code;
};
struct StandardChalice {
    int key;
    quint8 rites;
    quint32 goods;
    std::vector<StandardSlot> features;
};
constexpr StandardSlot NoSlot{0xFFFFFFFFu, -1};

const std::vector<StandardChalice>& StandardChalices() {
    constexpr StandardSlot S90{995010110, 90};
    constexpr StandardSlot S91{995010120, 91};
    constexpr StandardSlot S93{995010140, 93};
    constexpr StandardSlot S94{995010220, 94};
    constexpr StandardSlot S45{994500000, 45};
    static const std::vector<StandardChalice> chalices = {
        {10, 90, 6109, {NoSlot, NoSlot, S90, NoSlot, NoSlot, S45}},
        {20, 90, 6209, {NoSlot, NoSlot, S90, S91, NoSlot, S45}},
        {21, 90, 6219, {NoSlot, NoSlot, S94, {991000000, 10}, NoSlot, S45}},
        {30, 90, 6309, {{996100000, 61}, {998500000, 85}, S90, S91, NoSlot, S45}},
        {31, 90, 6319, {NoSlot, {998600000, 86}, S94, {995010230, 95}, {991000000, 10}, S45}},
        {40, 90, 6409, {{996200000, 62}, {997000000, 70}, S90, S91, S93, S45}},
        {42, 90, 6429, {NoSlot, NoSlot, {995010340, 96}, NoSlot, NoSlot, S45}},
        {50, 90, 6509, {{996200000, 62}, {998700000, 87}, S90, S91, S93, S45}},
        {52, 90, 6529, {NoSlot, {998800000, 88}, {995010340, 96}, {995010350, 97}, NoSlot, S45}},
        {53, 90, 6539, {S90, {998900000, 89}, {995010450, 98}, S91, S94, S45}},
    };
    return chalices;
}

// Their ChannelIds: this base plus the key, far above any stored dungeon's.
constexpr qint64 StandardChannelBase = 2'000'000'000;
constexpr char StandardConductor[] = "Hunter";

void AppendLe32(QByteArray& out, quint32 value) {
    for (int shift = 0; shift < 32; shift += 8) {
        out.append(char((value >> shift) & 0xFF));
    }
}

std::optional<Json> StandardDungeon(qint64 key) {
    const auto& chalices = StandardChalices();
    const auto it = std::find_if(chalices.begin(), chalices.end(),
                                 [key](const StandardChalice& c) { return c.key == key; });
    if (it == chalices.end()) {
        return std::nullopt;
    }
    QByteArray form;
    AppendLe32(form, 5);
    AppendLe32(form, 140);
    form.append('\0').append(char(it->rites)).append(char(it->key)).append(char(0x1D));
    AppendLe32(form, it->goods);
    for (size_t slot = 0; slot < 9; ++slot) {
        const StandardSlot& feature = slot < it->features.size() ? it->features[slot] : NoSlot;
        AppendLe32(form, feature.id);
        AppendLe32(form, quint32(feature.code));
    }
    const QByteArray conductor(StandardConductor);
    form.append(conductor.leftJustified(16, '\0'));
    QByteArray wide;
    for (const char c : conductor) {
        wide.append(c).append('\0');
    }
    form.append(wide.leftJustified(32, '\0'));
    form = form.leftJustified(140, '\0');
    const QString glyph = QStringLiteral("chs") + QChar::fromLatin1(GlyphAlphabet[key / 32]) +
                          QChar::fromLatin1(GlyphAlphabet[key % 32]);
    const QString stamp = QStringLiteral("2015-03-24T00:00:00");
    Json record = Json::Object();
    record.Set(QStringLiteral("ChannelId"), Int(StandardChannelBase + key));
    record.Set(QStringLiteral("ShareLevel"), Int(2));
    record.Set(QStringLiteral("Status"), Int(1));
    record.Set(QStringLiteral("TurnoutLevel"), Int(0));
    record.Set(QStringLiteral("FormDataVersion"), Int(0));
    record.Set(QStringLiteral("FormData"), Str(QString::fromLatin1(form.toBase64())));
    record.Set(QStringLiteral("DiscernmentWord"), Str(glyph));
    record.Set(QStringLiteral("RitualLevel"), Int(key / 10));
    record.Set(QStringLiteral("HolyGrailTypeId"), Int(key % 10));
    record.Set(QStringLiteral("FixedOrGeneral"), Int(2));
    record.Set(QStringLiteral("SubFeatureFlag"), Int(256));
    record.Set(QStringLiteral("CreateUserId"), Int(0));
    record.Set(QStringLiteral("CreateCharaId"), Int(0));
    record.Set(QStringLiteral("CreateDate"), Str(stamp));
    record.Set(QStringLiteral("LastPlayDate"), Str(stamp));
    record.Set(QStringLiteral("WishMaterialList"), Json::Array());
    record.Set(QStringLiteral("UnlockFlagList"), Json::Array());
    return record;
}

// The standard chalice dungeon a ritual's ChannelSearch asks for, or nothing.
std::optional<Json> StandardSearch(const Json& req) {
    if (IdList(req.Get(QStringLiteral("FixedOrGeneralList")), QStringLiteral("FixedOrGeneral")) !=
        std::vector<qint64>{2}) {
        return std::nullopt;
    }
    const Json level = req.Get(QStringLiteral("RitualLevel"));
    const Json area = req.Get(QStringLiteral("HolyGrailTypeId"));
    if (level.kind() != Json::Kind::Int || area.kind() != Json::Kind::Int || area.IntValue() < 0 ||
        area.IntValue() > 9) {
        return std::nullopt;
    }
    return StandardDungeon(level.IntValue() * 10 + area.IntValue());
}

// Chalice Dungeon Search (FixedOrGeneral 0 among those asked): what dungeons are in use.
bool IsDungeonSearch(const Json& req) {
    const auto kinds =
        IdList(req.Get(QStringLiteral("FixedOrGeneralList")), QStringLiteral("FixedOrGeneral"));
    return std::find(kinds.begin(), kinds.end(), 0) != kinds.end();
}

// Adds the standard dungeons among a request's ChannelIds to an answer's list.
void StandardDetails(const Json& req, Json& payload, const QString& listKey) {
    std::vector<Json> found;
    std::set<qint64> taken;
    for (const qint64 id :
         IdList(req.Get(QStringLiteral("ChannelIdList")), QStringLiteral("ChannelId"))) {
        if (auto dungeon = StandardDungeon(id - StandardChannelBase)) {
            taken.insert(id);
            found.push_back(std::move(*dungeon));
        }
    }
    if (found.empty()) {
        return;
    }
    const auto kept = [&taken](const Json& list) {
        Json out = Json::Array();
        for (const Json& item : list.IsArray() ? list.Items() : std::vector<Json>{}) {
            if (!item.IsObject() || !taken.contains(AsInt(item.Get(QStringLiteral("ChannelId"))))) {
                out.Append(item);
            }
        }
        return out;
    };
    Json list = kept(payload.Get(listKey));
    for (Json& dungeon : found) {
        list.Append(std::move(dungeon));
    }
    payload.Set(listKey, std::move(list));
    payload.Set(QStringLiteral("LostChannelIdList"),
                kept(payload.Get(QStringLiteral("LostChannelIdList"))));
}

const std::map<QString, StoreHandler>& StoreHandlers() {
    static const std::map<QString, StoreHandler> handlers = {
        {QStringLiteral("MessengerShellUpload"),
         [](Context& c, Json&) {
             c.store.SetShell(c.uid, c.req.Get(QStringLiteral("ShellData")),
                              c.req.Get(QStringLiteral("ShellDataVersion")));
         }},
        {QStringLiteral("BloodMessCreate"),
         [](Context& c, Json& out) {
             Json list = Json::Array();
             for (const auto& [id, prev] : c.store.CreateMessages(
                      c.uid, c.chara, OrEmptyList(c.req.Get(QStringLiteral("BloodMessList"))))) {
                 Json entry = Json::Object();
                 entry.Set(QStringLiteral("BloodMessId"), Int(id));
                 entry.Set(QStringLiteral("PrevBloodMessId"), prev);
                 list.Append(entry);
             }
             out.Set(QStringLiteral("BloodMessIdList"), list);
         }},
        {QStringLiteral("BloodMessGetList"),
         [](Context& c, Json& out) {
             out.Set(QStringLiteral("BloodMessList"),
                     c.store.ListMessages(c.uid, AreasOf(c.req, QStringLiteral("AreaInfoList")),
                                          c.req.Get(QStringLiteral("GetMaxCount"))));
         }},
        {QStringLiteral("BloodMessEvaluate"),
         [](Context& c, Json& out) {
             const auto entry =
                 c.store.Evaluate(c.uid, AsInt(c.req.Get(QStringLiteral("BloodMessId")), -1),
                                  c.req.Get(QStringLiteral("EvaluateKind")));
             out.Set(QStringLiteral("BloodMessList"),
                     entry ? Json::Array({*entry}) : Json::Array());
         }},
        {QStringLiteral("BloodMessRemove"),
         [](Context& c, Json&) {
             c.store.DeleteMessage(c.uid, AsInt(c.req.Get(QStringLiteral("BloodMessId")), -1));
         }},
        {QStringLiteral("BloodMessSearchAdd"),
         [](Context& c, Json& out) {
             const auto ids = IdList(c.req.Get(QStringLiteral("BloodMessIdList")),
                                     QStringLiteral("BloodMessId"));
             // INFERRED: LostBloodMessIdList element shape mirrors the request's list.
             out.Set(QStringLiteral("BloodMessEvaluationList"), c.store.EvaluationsFor(ids));
             out.Set(QStringLiteral("LostBloodMessIdList"),
                     IdObjects(c.store.LostMessages(ids), QStringLiteral("BloodMessId")));
         }},
        {QStringLiteral("BloodMessGetEvaluate"),
         [](Context& c, Json& out) {
             out.Set(QStringLiteral("BloodMessEvaluationList"),
                     c.store.EvaluationsFor(IdList(c.req.Get(QStringLiteral("BloodMessIdList")),
                                                   QStringLiteral("BloodMessId"))));
         }},
        {QStringLiteral("TombMessCreate"),
         [](Context& c, Json& out) {
             out.Set(QStringLiteral("TombMessId"), Int(c.store.CreateTomb(c.uid, c.chara, c.req)));
         }},
        {QStringLiteral("TombMessGetList"),
         [](Context& c, Json& out) {
             out.Set(QStringLiteral("TombMessList"),
                     c.store.ListTombs(c.uid, AreasOf(c.req, QStringLiteral("AreaInfoList")),
                                       c.req.Get(QStringLiteral("GetMaxCount"))));
         }},
        {QStringLiteral("DeathVisionGet"),
         [](Context& c, Json& out) {
             const auto vision =
                 c.store.DeathVision(AsInt(c.req.Get(QStringLiteral("TombMessId")), -1));
             if (vision) {
                 out.Set(QStringLiteral("DeathVisionData"), vision->first);
                 out.Set(QStringLiteral("DeathVisionDataVersion"), vision->second);
             } else {
                 out.Set(QStringLiteral("DeathVisionData"), Str(QString()));
                 out.Set(QStringLiteral("DeathVisionDataVersion"),
                         c.req.Get(QStringLiteral("DeathVisionDataVersion"), Int(4)));
             }
         }},
        {QStringLiteral("WanderingGhostCreate"),
         [](Context& c, Json& out) {
             out.Set(QStringLiteral("WanderingGhostId"),
                     Int(c.store.CreateGhost(c.uid, c.chara, c.req)));
         }},
        {QStringLiteral("WanderingGhostGet"),
         [](Context& c, Json& out) {
             out.Set(QStringLiteral("WanderingGhostList"),
                     c.store.ListGhosts(c.uid, AreasOf(c.req, QStringLiteral("AreaList")),
                                        c.req.Get(QStringLiteral("GetMaxCount"))));
         }},
        {QStringLiteral("ChairMessRespawnPointNotice"),
         [](Context& c, Json&) {
             c.store.TouchLantern(c.uid, AsInt(c.req.Get(QStringLiteral("WarpInfoId")), -1),
                                  AsInt(c.req.Get(QStringLiteral("ChannelId"))));
         }},
        {QStringLiteral("ChairMessGetList"),
         [](Context& c, Json& out) {
             out.Set(QStringLiteral("ChairMessList"), c.store.LanternCounts(c.uid));
         }},
        {QStringLiteral("ChannelUpload"),
         [](Context& c, Json& out) {
             const auto [id, glyph] = c.store.UploadDungeon(c.uid, c.chara, c.req);
             out.Set(QStringLiteral("ChannelId"), Int(id));
             out.Set(QStringLiteral("DiscernmentWord"), Str(glyph));
         }},
        {QStringLiteral("ChannelShare"),
         [](Context& c, Json&) {
             c.store.ShareDungeon(c.uid, AsInt(c.req.Get(QStringLiteral("ChannelId")), -1),
                                  c.req.Get(QStringLiteral("ShareLevel")));
         }},
        {QStringLiteral("ChannelSearch"),
         [](Context& c, Json& out) {
             out.Set(QStringLiteral("ChannelList"), c.store.SearchDungeons(c.req));
         }},
        {QStringLiteral("ChannelWordSearch"),
         [](Context& c, Json& out) {
             // The community server omitted ChannelList when nothing matched.
             const auto record = c.store.WordSearch(c.req.Get(QStringLiteral("SearchWord")));
             if (record) {
                 out.Set(QStringLiteral("ChannelList"), Json::Array({*record}));
             }
         }},
        {QStringLiteral("ChannelGetInfo"),
         [](Context& c, Json& out) {
             // INFERRED: ChannelInfoList entries never observed with content; full records.
             auto [found, lost] = c.store.DungeonsByIds(
                 IdList(c.req.Get(QStringLiteral("ChannelIdList")), QStringLiteral("ChannelId")));
             out.Set(QStringLiteral("ChannelInfoList"), found);
             out.Set(QStringLiteral("LostChannelIdList"),
                     IdObjects(lost, QStringLiteral("ChannelId")));
         }},
        {QStringLiteral("ChannelGetDetailsInfo"),
         [](Context& c, Json& out) {
             auto [found, lost] = c.store.DungeonsByIds(
                 IdList(c.req.Get(QStringLiteral("ChannelIdList")), QStringLiteral("ChannelId")));
             out.Set(QStringLiteral("ChannelList"), found);
             out.Set(QStringLiteral("LostChannelIdList"),
                     IdObjects(lost, QStringLiteral("ChannelId")));
         }},
    };
    return handlers;
}

// Empty-content answers without a store. Names and shapes verbatim from the community server.
Json EmptyResponseFields(const QString& responseId) {
    const auto list = [] { return Json::Array(); };
    Json fields = Json::Object();
    if (responseId == QStringLiteral("UserPropertiesMoveCountCheckResponse") ||
        responseId == QStringLiteral("MessengerShellUploadResponse")) {
        return fields;
    }
    if (responseId == QStringLiteral("ChannelGetInfoResponse")) {
        fields.Set(QStringLiteral("ChannelInfoList"), list());
        fields.Set(QStringLiteral("LostChannelIdList"), list());
    } else if (responseId == QStringLiteral("BloodMessGetListResponse")) {
        fields.Set(QStringLiteral("BloodMessList"), list());
    } else if (responseId == QStringLiteral("BloodMessSearchAddResponse")) {
        fields.Set(QStringLiteral("BloodMessEvaluationList"), list());
        fields.Set(QStringLiteral("LostBloodMessIdList"), list());
    } else if (responseId == QStringLiteral("BloodMessGetEvaluateResponse")) {
        fields.Set(QStringLiteral("BloodMessEvaluationList"), list());
    } else if (responseId == QStringLiteral("TombMessGetListResponse")) {
        fields.Set(QStringLiteral("TombMessList"), list());
    } else if (responseId == QStringLiteral("DeathVisionGetResponse")) {
        fields.Set(QStringLiteral("DeathVisionData"), Str(QString()));
    } else if (responseId == QStringLiteral("ChairMessGetListResponse")) {
        fields.Set(QStringLiteral("ChairMessList"), list());
    } else if (responseId == QStringLiteral("WanderingGhostGetResponse")) {
        fields.Set(QStringLiteral("WanderingGhostList"), list());
    } else {
        return Json(); // not one of these
    }
    return fields;
}

// Create-type services without a store still hand back an id so the game's own bookkeeping
// stays consistent. Field names from the captures.
QString CreateIdField(const QString& responseId) {
    if (responseId == QStringLiteral("BloodMessCreateResponse")) {
        return QStringLiteral("BloodMessId");
    }
    if (responseId == QStringLiteral("TombMessCreateResponse")) {
        return QStringLiteral("TombMessId");
    }
    if (responseId == QStringLiteral("WanderingGhostCreateResponse")) {
        return QStringLiteral("WanderingGhostId");
    }
    return {};
}

BackendReply JsonReply(const Json& payload) {
    // The retail service sets sp-res-kind alongside the in-body ResKind.
    return {200, "application/json", DumpJson(payload), {{"sp-res-kind", "0"}}};
}

void Merge(Json& payload, const Json& fields) {
    for (size_t i = 0; i < fields.Keys().size(); ++i) {
        payload.Set(fields.Keys()[i], fields.Items()[i]);
    }
}

} // namespace

BloodborneBackend::BloodborneBackend() = default;
BloodborneBackend::~BloodborneBackend() = default;

bool BloodborneBackend::Open(const BackendOptions& options, QString* error) {
    m_options = options;
    m_options.summonPoll = std::max(1, options.summonPoll);
    const SsInfo check = BuildSsInfo(m_options.ssInfoTemplate, QStringLiteral("http://check"),
                                     QStringLiteral("http://check"), m_options.summonPoll);
    if (!check.error.isEmpty()) {
        *error = check.error;
        return false;
    }
    m_store.reset();
    if (!options.storePath.isEmpty()) {
        auto store = std::make_unique<ContentStore>(options.showOwn, options.ghostTtlSeconds,
                                                    options.lanternTtlSeconds, options.tombCap);
        QString storeError;
        if (!store->Open(options.storePath, &storeError)) {
            *error = QStringLiteral("cannot open content store %1: %2")
                         .arg(options.storePath, storeError);
            return false;
        }
        m_store = std::move(store);
    }
    m_memoryStore.reset();
    m_chaliceStore = m_store.get();
    if (!m_store) {
        // Chalice dungeons only, so teammates can join each other's without a store.
        m_memoryStore = std::make_unique<ContentStore>(QSet<QString>(), 0, 0, 0);
        QString storeError;
        if (!m_memoryStore->Open(QStringLiteral(":memory:"), &storeError)) {
            *error = QStringLiteral("cannot open the chalice store in memory: %1").arg(storeError);
            return false;
        }
        m_chaliceStore = m_memoryStore.get();
    }
    m_standardUsed.clear();
    return true;
}

BackendIdentity IdentityOf(const BackendRequest& request) {
    if (request.method != "POST" || request.body.isEmpty()) {
        return {};
    }
    const auto json = ParseJson(request.body);
    if (!json || !json->IsObject()) {
        return {};
    }
    const Json messageId = json->Get(QStringLiteral("MessageId"));
    if (messageId.IsString() && messageId.StringValue() == QStringLiteral("LoginRequest")) {
        // As Login reads it (no account: "Hunter").
        const Json given = json->Get(QStringLiteral("PlatformAccountId"));
        return {Truthy(given) ? PyStr(given) : QStringLiteral("Hunter"), std::nullopt};
    }
    const Json userId = json->Get(QStringLiteral("UserId"));
    if (userId.IsNull()) {
        return {};
    }
    return {std::nullopt, AsInt(userId, 0)};
}

std::optional<qint64> BloodborneBackend::UserIdOf(const QString& account) {
    const QString key = account.toLower();
    if (const auto known = m_userIds.constFind(key); known != m_userIds.constEnd()) {
        return *known;
    }
    std::optional<qint64> userId;
    if (!m_store) {
        userId = StableUserId(account);
    } else {
        try {
            userId = m_store->UserIdOf(account);
        } catch (const PythonError& e) {
            qWarning().noquote() << "backend: no UserId for" << account << ":" << e.what();
        }
    }
    if (userId) {
        if (m_userIds.size() > 100'000) {
            m_userIds.clear();
        }
        m_userIds.insert(key, *userId);
    }
    return userId;
}

Json BloodborneBackend::Counts() const {
    return m_store ? m_store->Counts() : Json();
}

BackendReply BloodborneBackend::Handle(const BackendRequest& request) {
    if (request.method == "GET") {
        if (IsSsInfoPath(request.path.toStdString())) {
            const SsInfo info = BuildSsInfo(m_options.ssInfoTemplate, request.servicesUrl,
                                            request.summonUrl, m_options.summonPoll);
            qDebug().noquote() << "backend: ss.info ->" << info.body.size() << "bytes";
            return {200, "text/plain; charset=utf-8", info.body, {}};
        }
        if (request.path == QStringLiteral("/status")) {
            Json status = Json::Object();
            status.Set(QStringLiteral("ok"), Json::Bool(true));
            status.Set(QStringLiteral("service"), Str(QStringLiteral("bb-dummy")));
            status.Set(QStringLiteral("content"),
                       m_store ? m_store->Counts() : Str(QStringLiteral("off")));
            return JsonReply(status);
        }
        qInfo().noquote() << "backend: unhandled GET" << request.path;
        return {404, "text/plain", "Not Found", {}};
    }
    if (request.method != "POST") {
        return {501, "text/plain", "Unsupported method", {}};
    }
    Json parsed = Json::Object();
    if (!request.body.isEmpty()) {
        const auto json = ParseJson(request.body);
        if (json && json->IsObject()) {
            parsed = *json;
        }
    }
    return JsonReply(Service(request.path, parsed));
}

Json BloodborneBackend::Service(const QString& path, const Json& request) {
    // Every service follows <Name>Request -> <Name>Response, so this also covers the endpoints
    // that were never exercised during capture.
    const Json messageValue = request.Get(QStringLiteral("MessageId"), Str(QString()));
    const QString messageId = messageValue.IsString() ? messageValue.StringValue() : QString();
    const QString name =
        messageId.endsWith(QStringLiteral("Request")) ? messageId.chopped(7) : QString();
    const QString responseId = name.isEmpty() ? QString() : name + QStringLiteral("Response");

    if (name == QStringLiteral("Login")) {
        return Login(request);
    }

    Json payload = Json::Object();
    payload.Set(QStringLiteral("ResKind"), Int(0));
    payload.Set(QStringLiteral("MessageId"), Str(responseId));
    const qint64 uid = AsInt(request.Get(QStringLiteral("UserId")), 0);
    const QString chara = PyStr(request.Get(QStringLiteral("CharaId"), Str(QString())));

    const auto& handlers = StoreHandlers();
    if (name == QStringLiteral("NoticeNormalGet")) {
        const auto notice = LoadNotice(m_options.noticePath);
        payload.Set(QStringLiteral("NoticeList"),
                    notice ? Json::Array({notice->entry}) : Json::Array());
    } else if (name == QStringLiteral("NoticeEmergencyGet")) {
        std::optional<Json> entry;
        if (const auto broadcast = LoadNotice(m_options.broadcastPath)) {
            if (m_broadcastSent.insert({uid, broadcast->digest}).second) {
                entry = broadcast->entry;
            }
        }
        payload.Set(QStringLiteral("NoticeList"), entry ? Json::Array({*entry}) : Json::Array());
        // The community server echoed the client's own CheckTime back untouched (CheckTime is
        // in the client's local time, so the host's clock means nothing to it).
        const Json checkTime = request.Get(QStringLiteral("CheckTime"));
        payload.Set(QStringLiteral("CheckTime"), Truthy(checkTime) ? checkTime : Str(NowIso()));
        if (entry) {
            qInfo().noquote() << "backend: broadcast delivered to UserId" << uid;
        }
    } else if (name == QStringLiteral("SyncCharaId")) {
        // A save that meets a new account asks for CharaIdNum fresh character ids; without them
        // the game loops on "Failed to reset online character data". Ids derive from the UserId,
        // stable across restarts and never colliding between users.
        const qint64 count = std::max<qint64>(
            1, std::min<qint64>(AsInt(request.Get(QStringLiteral("CharaIdNum")), 1), 16));
        Json ids = Json::Array();
        for (qint64 i = 0; i < count; ++i) {
            Json entry = Json::Object();
            entry.Set(QStringLiteral("PublishCharaId"), Int(uid * 16 + i + 1));
            ids.Append(entry);
        }
        payload.Set(QStringLiteral("PublishCharacterIdList"), ids);
    } else if (const auto standard = name == QStringLiteral("ChannelSearch")
                                         ? StandardSearch(request)
                                         : std::nullopt) {
        const qint64 key =
            standard->Get(QStringLiteral("ChannelId")).IntValue() - StandardChannelBase;
        m_standardUsed.erase(std::remove(m_standardUsed.begin(), m_standardUsed.end(), key),
                             m_standardUsed.end());
        m_standardUsed.push_back(key);
        payload.Set(QStringLiteral("ChannelList"), Json::Array({*standard}));
    } else if (name == QStringLiteral("ChannelSearch") && IsDungeonSearch(request)) {
        // The dungeons in use on this server, newest first: the standard ones a ritual asked for
        // and every one uploaded (shared or not: a team's dungeons are for the team), of the
        // depth and area asked for when given. A teammate joins one there, free.
        const Json level = request.Get(QStringLiteral("RitualLevel"));
        const Json area = request.Get(QStringLiteral("HolyGrailTypeId"));
        std::vector<Json> found;
        for (auto it = m_standardUsed.rbegin(); it != m_standardUsed.rend(); ++it) {
            if ((level.IsNull() || *it / 10 == AsInt(level)) &&
                (area.IsNull() || *it % 10 == AsInt(area))) {
                found.push_back(*StandardDungeon(*it));
            }
        }
        try {
            for (Json& dungeon : m_chaliceStore->AllDungeons(level, area)) {
                found.push_back(std::move(dungeon));
            }
        } catch (const PythonError& e) {
            qWarning().noquote() << "backend: ERROR in" << name << ":" << e.what();
        }
        qint64 limit = AsInt(request.Get(QStringLiteral("GetCount")), 100);
        if (!limit) {
            limit = 100;
        }
        Json list = Json::Array();
        for (size_t i = 0; i < found.size() && qint64(i) < limit; ++i) {
            list.Append(found[i]);
        }
        payload.Set(QStringLiteral("ChannelList"), list);
    } else if ((m_store || name.startsWith(QStringLiteral("Channel"))) && handlers.count(name)) {
        // Chalice dungeons are kept even without a store (in memory then): teammates join them.
        const bool chalice = name.startsWith(QStringLiteral("Channel"));
        Json fields = Json::Object();
        try {
            Context context{chalice ? *m_chaliceStore : *m_store, request, uid, chara};
            handlers.at(name)(context, fields);
            Merge(payload, fields);
        } catch (const PythonError& e) {
            // A store error must never knock the game offline.
            qWarning().noquote() << "backend: ERROR in" << name << ":" << e.what()
                                 << "-- answered without content";
        }
    } else if (const Json empty = EmptyResponseFields(responseId); empty.IsObject()) {
        Merge(payload, empty);
    } else if (const QString field = CreateIdField(responseId); !field.isEmpty()) {
        payload.Set(field, Int(++m_nextCreatedId));
    } else if (!responseId.isEmpty()) {
        // Answered generically rather than 404, so an unexercised service can never knock the
        // game offline. Flagged once so it can be added.
        if (!m_unknownSeen.contains(responseId)) {
            m_unknownSeen.insert(responseId);
            qInfo().noquote() << "backend: NOTE: unmapped service" << responseId << "at" << path
                              << "-- answered generically";
        }
    } else {
        qInfo().noquote() << "backend: POST" << path << "with no MessageId; answering ResKind 0";
    }

    if (name == QStringLiteral("ChannelGetDetailsInfo")) {
        StandardDetails(request, payload, QStringLiteral("ChannelList"));
    } else if (name == QStringLiteral("ChannelGetInfo")) {
        StandardDetails(request, payload, QStringLiteral("ChannelInfoList"));
    }

    if (qEnvironmentVariableIsSet("BB_TRACE_BODIES")) {
        qInfo().noquote() << "backend: TRACE" << path
                          << "req=" << QString::fromUtf8(DumpJsonSpaced(request).left(600))
                          << "resp=" << QString::fromUtf8(DumpJsonSpaced(payload).left(300));
    }
    qDebug().noquote() << "backend:" << path << "->"
                       << (responseId.isEmpty() ? QStringLiteral("(generic)") : responseId);
    return payload;
}

Json BloodborneBackend::Login(const Json& request) {
    const Json given = request.Get(QStringLiteral("PlatformAccountId"));
    const QString account = Truthy(given) ? PyStr(given) : QStringLiteral("Hunter");
    qint64 userId = 0;
    if (m_store) {
        try {
            userId = m_store->Login(account);
        } catch (const PythonError& e) {
            qWarning().noquote() << "backend: login failed for" << account << ":" << e.what();
            userId = StableUserId(account);
        }
    } else {
        userId = StableUserId(account);
    }
    Json payload = Json::Object();
    payload.Set(QStringLiteral("ServerVersion"), Int(1));
    payload.Set(QStringLiteral("UserStatus"), Int(0));
    payload.Set(QStringLiteral("SessionId"),
                Str(QUuid::createUuid().toString(QUuid::WithoutBraces)));
    payload.Set(QStringLiteral("PlatformAccountId"), Str(account));
    payload.Set(
        QStringLiteral("LanguageId"),
        Json::Float(static_cast<double>(AsInt(request.Get(QStringLiteral("LanguageId")), 19))));
    payload.Set(QStringLiteral("UserId"), Int(userId));
    payload.Set(QStringLiteral("MessageId"), Str(QStringLiteral("LoginResponse")));
    payload.Set(QStringLiteral("WarningMessage"),
                Str(B64Text(ReadText(m_options.loginMessagePath).trimmed())));
    payload.Set(
        QStringLiteral("IssuerId"),
        Json::Float(static_cast<double>(AsInt(request.Get(QStringLiteral("IssuerId")), 100))));
    payload.Set(QStringLiteral("ResKind"), Int(0));
    qInfo().noquote() << "backend: login:" << account << "-> UserId" << userId;
    return payload;
}

} // namespace Bloodborne::Backend
