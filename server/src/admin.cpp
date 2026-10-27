// SPDX-FileCopyrightText: Copyright 2026 shadNet Project
// SPDX-License-Identifier: GPL-2.0-or-later
#include "admin.h"

#include <QDateTime>
#include <QFile>
#include <QHostAddress>
#include <QJsonArray>
#include <QLocalSocket>
#include <QRegularExpression>
#include <QSaveFile>
#include <QTimeZone>

namespace Admin {
namespace {

QString Time(qint64 ms) {
    if (ms <= 0) {
        return {};
    }
    return QDateTime::fromMSecsSinceEpoch(ms, QTimeZone::utc()).toString(Qt::ISODate);
}

QJsonArray Strings(const auto& list) {
    QJsonArray array;
    for (const std::string& item : list) {
        array.append(QString::fromStdString(item));
    }
    return array;
}

QJsonObject Line(const bbcoop::TeamWorldInfo& info) {
    return {{"id", QString::fromStdString(info.id())},
            {"name", QString::fromStdString(info.name())},
            {"character", QString::fromStdString(info.character())},
            {"members", Strings(info.members())},
            {"lamps", static_cast<int>(info.lamps())},
            {"played", Time(info.played_ms())},
            {"created", Time(info.created_ms())}};
}

Answer Fail(const QString& why) {
    return {QJsonObject{{"ok", false}, {"error", why}}, {}, {}};
}

// An address (a.b.c.d) or a player id.
struct Who {
    QString text;
    quint32 ipv4 = 0;
};
Who WhoOf(const QString& text) {
    bool ipv4 = false;
    const quint32 address = QHostAddress(text).toIPv4Address(&ipv4);
    return {text, ipv4 ? address : 0};
}

QString ReadText(const QString& path) {
    QFile file{path};
    return file.open(QIODevice::ReadOnly | QIODevice::Text) ? QString::fromUtf8(file.readAll())
                                                            : QString{};
}

bool WriteText(const QString& path, const QString& text) {
    QSaveFile file{path};
    const QByteArray bytes = text.toUtf8();
    return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size() && file.commit();
}

Answer Players(Context& context) {
    QJsonArray players;
    for (const ServerLink::Hub::LinkRow& link : context.hub.Links()) {
        QJsonObject row{{"id", QString::fromUtf8(link.online_id)},
                        {"address", link.endpoint.Text()},
                        {"build", QString::fromUtf8(link.build)},
                        {"silent_ms", context.link_ms - link.last_heard_ms}};
        if (const auto place = context.world.PlaceOf(link.endpoint)) {
            row["team"] = QString::fromStdString(place->team);
            row["world"] = QString::fromStdString(place->world);
            row["map"] = static_cast<int>(place->map);
        }
        players.append(row);
    }
    return {QJsonObject{{"ok", true}, {"players", players}}, {}, {}};
}

Answer Kick(Context& context, const QString& target) {
    const Who who = WhoOf(target);
    Answer answer;
    answer.link = context.hub.Kick(who.ipv4 != 0 ? QByteArray{} : who.text.toUtf8(), who.ipv4,
                                   context.link_ms);
    const auto kicked = answer.link.gone.size();
    answer.json = {{"ok", kicked > 0}, {"kicked", static_cast<int>(kicked)}};
    if (kicked == 0) {
        answer.json["error"] = QStringLiteral("nobody linked as %1").arg(target);
    }
    return answer;
}

Answer Ban(Context& context, const QStringList& words) {
    if (words.size() < 2) {
        return Fail(QStringLiteral("ban <id|address> [90s|30m|12h|7d] [note]"));
    }
    Admission::BanLine ban{words[1], 0, {}};
    qsizetype note_from = 2;
    if (words.size() > 2 && words[2].front().isDigit()) {
        const auto seconds = ParseDuration(words[2]);
        if (!seconds) {
            return Fail(QStringLiteral("%1 is no duration (90s, 30m, 12h, 7d)").arg(words[2]));
        }
        ban.until_s = context.wall_ms / 1000 + *seconds;
        note_from = 3;
    }
    ban.note = words.mid(note_from).join(' ');
    const qint64 now_s = context.wall_ms / 1000;
    if (!WriteText(context.ban_list, Admission::WithBan(ReadText(context.ban_list), ban, now_s))) {
        return Fail(QStringLiteral("could not write %1").arg(context.ban_list));
    }
    LoadBans(context.ban_list, now_s, context.guard);
    Answer answer = Kick(context, ban.who);
    answer.json = {{"ok", true},
                   {"banned", ban.who},
                   {"until", Time(ban.until_s * 1000)},
                   {"kicked", answer.json["kicked"]}};
    answer.world.log.append(
        QStringLiteral("admin: %1 banned%2")
            .arg(ban.who, ban.until_s != 0 ? QStringLiteral(" until ") + Time(ban.until_s * 1000)
                                           : QString{}));
    return answer;
}

Answer Unban(Context& context, const QString& target) {
    bool found = false;
    const QString text = Admission::WithoutBan(ReadText(context.ban_list), target, found);
    if (!found) {
        return Fail(QStringLiteral("%1 is not in %2").arg(target, context.ban_list));
    }
    if (!WriteText(context.ban_list, text)) {
        return Fail(QStringLiteral("could not write %1").arg(context.ban_list));
    }
    LoadBans(context.ban_list, context.wall_ms / 1000, context.guard);
    Answer answer{QJsonObject{{"ok", true}, {"unbanned", target}}, {}, {}};
    answer.world.log.append(QStringLiteral("admin: %1 unbanned").arg(target));
    return answer;
}

Answer Bans(Context& context) {
    const qint64 now_s = context.wall_ms / 1000;
    LoadBans(context.ban_list, now_s, context.guard);
    QJsonArray bans;
    for (const Admission::BanLine& ban : Admission::BanLines(ReadText(context.ban_list))) {
        bans.append(QJsonObject{{"who", ban.who},
                                {"until", Time(ban.until_s * 1000)},
                                {"ended", ban.until_s != 0 && ban.until_s <= now_s},
                                {"note", ban.note}});
    }
    return {QJsonObject{{"ok", true}, {"bans", bans}}, {}, {}};
}

Answer Worlds(Context& context) {
    QJsonArray worlds;
    QStringList paths;
    for (const auto& row : context.world.WorldRows()) {
        QJsonObject line = Line(row.info);
        line["folder"] = row.folder;
        line["games"] = row.games;
        worlds.append(line);
        paths.append(row.path);
    }
    Answer answer{QJsonObject{{"ok", true}, {"worlds", worlds}, {"aside", QJsonArray{}}}, {}, {}};
    // Every file of every world, and the folders put aside: read off the link's thread.
    answer.finish = [paths, folder = context.world.Folder()](QJsonObject& json) {
        QJsonArray worlds = json["worlds"].toArray();
        for (qsizetype at = 0; at < worlds.size() && at < paths.size(); ++at) {
            QJsonObject line = worlds[at].toObject();
            line["bytes"] = paths[at].isEmpty() ? 0 : BloodborneWorld::Service::BytesOn(paths[at]);
            worlds[at] = line;
        }
        json["worlds"] = worlds;
        QJsonArray aside;
        for (const auto& row : BloodborneWorld::Service::AsideRowsIn(folder)) {
            QJsonObject line = row.info ? Line(*row.info) : QJsonObject{};
            line["folder"] = row.folder;
            line["bytes"] = row.bytes;
            aside.append(line);
        }
        json["aside"] = aside;
    };
    return answer;
}

Answer Restore(Context& context, const QString& aside) {
    Answer answer;
    QString said;
    if (!context.world.Restore(aside, context.wall_ms, said, answer.world)) {
        return Fail(said);
    }
    answer.json = {{"ok", true}, {"id", said}};
    return answer;
}

Answer Retire(Context& context) {
    Answer answer;
    QJsonArray retired;
    for (const std::string& id :
         context.world.Retire(context.wall_ms, context.retire_ms, answer.world)) {
        retired.append(QString::fromStdString(id));
    }
    answer.json = {{"ok", true}, {"retired", retired}};
    return answer;
}

QString Key(const Context& context) {
    return QString::fromStdString(LinkAuth::HexOf(context.hub.PublicKey()));
}

Answer Stats(Context& context) {
    const Admission::Guard::Counters& count = context.guard.Count();
    return {QJsonObject{{"ok", true},
                        {"players", context.hub.Members()},
                        {"worlds", static_cast<int>(context.world.Lobby().Worlds().size())},
                        {"worlds_in_memory", context.world.Parties()},
                        {"link_load", context.link_load},
                        {"link_key", Key(context)},
                        {"bad_hellos", static_cast<qint64>(context.hub.BadHellos())},
                        {"bad_tags", static_cast<qint64>(context.hub.BadTags())},
                        {"hellos_refused", static_cast<qint64>(count.hellos_refused)},
                        {"datagrams_dropped", static_cast<qint64>(count.datagrams_dropped)},
                        {"accounts_refused", static_cast<qint64>(count.accounts_refused)},
                        {"bans", static_cast<qint64>(count.bans)}},
            {},
            {}};
}

} // namespace

std::optional<qint64> ParseDuration(const QString& text) {
    static const QRegularExpression pattern{QStringLiteral("^(\\d{1,9})([smhd])$")};
    const auto match = pattern.match(text.trimmed());
    if (!match.hasMatch()) {
        return std::nullopt;
    }
    const qint64 count = match.captured(1).toLongLong();
    switch (match.captured(2).front().toLatin1()) {
    case 's':
        return count;
    case 'm':
        return count * 60;
    case 'h':
        return count * 3600;
    default:
        return count * 86400;
    }
}

void LoadBans(const QString& path, qint64 now_s, Admission::Guard& guard) {
    QSet<quint32> addresses;
    QSet<QByteArray> ids;
    Admission::ParseBanList(ReadText(path), addresses, ids, now_s);
    guard.SetBanned(addresses, ids);
}

ServerLink::Hub::Result KickBanned(ServerLink::Hub& hub, const Admission::Guard& guard,
                                   qint64 link_ms) {
    ServerLink::Hub::Result result;
    for (const ServerLink::Hub::LinkRow& link : hub.Links()) {
        // banned.txt's alone: a rate ban (Guard::Ban) holds the address's next hellos only, or
        // every game behind one address (a CGNAT) would go with the one flooding.
        if (guard.BannedId(link.online_id) || guard.ListBanned(link.endpoint.ipv4)) {
            const auto kicked = hub.Kick(link.online_id, 0, link_ms);
            result.sends.append(kicked.sends);
            result.log.append(kicked.log);
            result.gone.append(kicked.gone);
        }
    }
    return result;
}

bool ListenAdmin(QLocalServer& server, const QString& path, QString& why) {
    QLocalSocket probe;
    probe.connectToServer(path);
    if (probe.waitForConnected(500)) {
        probe.abort();
        why = QStringLiteral("another server answers on it");
        return false;
    }
    QLocalServer::removeServer(path); // one left by a crash: nothing answers on it
    if (!server.listen(path)) {
        why = server.errorString();
        return false;
    }
    return true;
}

Answer Run(const QString& line, Context& context) {
    const QStringList words =
        line.trimmed().split(QRegularExpression(QStringLiteral("\\s+")), Qt::SkipEmptyParts);
    const QString verb = words.value(0).toLower();
    if (verb == QStringLiteral("players")) {
        return Players(context);
    }
    if (verb == QStringLiteral("kick") && words.size() == 2) {
        return Kick(context, words[1]);
    }
    if (verb == QStringLiteral("ban")) {
        return Ban(context, words);
    }
    if (verb == QStringLiteral("unban") && words.size() == 2) {
        return Unban(context, words[1]);
    }
    if (verb == QStringLiteral("bans")) {
        return Bans(context);
    }
    if (verb == QStringLiteral("worlds")) {
        return Worlds(context);
    }
    if (verb == QStringLiteral("restore") && words.size() == 2) {
        return Restore(context, words[1]);
    }
    if (verb == QStringLiteral("retire")) {
        return Retire(context);
    }
    if (verb == QStringLiteral("stats")) {
        return Stats(context);
    }
    if (verb == QStringLiteral("key")) {
        return {QJsonObject{{"ok", true}, {"key", Key(context)}}, {}, {}};
    }
    return Fail(QStringLiteral("commands: players, kick <id|address>, ban <id|address> "
                               "[90s|30m|12h|7d] [note], unban <id|address>, bans, worlds, "
                               "restore <folder>, retire, stats, key"));
}

} // namespace Admin
