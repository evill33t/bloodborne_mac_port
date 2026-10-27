// SPDX-FileCopyrightText: Copyright 2026 shadNet Project
// SPDX-License-Identifier: GPL-2.0-or-later
// The operator's commands on a hosted server (docs/HOSTED_SERVER.md, server/hosting/README.md):
// one line in, one JSON object out. LinkServer takes them on a UNIX-domain socket that only the
// server's own user can open, never on a network port, and runs them on the link's thread, where
// the link, the worlds and the bans live.
//
//   players                          the linked games: id, address, team, world, map
//   kick <id|address>                unlinked now (its game links again unless banned)
//   ban <id|address> [90s|30m|12h|7d] [note]   into banned.txt (for ever without a duration)
//   unban <id|address>               out of banned.txt
//   bans                             banned.txt's entries
//   worlds                           the listed worlds, and those put aside (forgotten/)
//   restore <folder>                 a world put aside back on the list
//   retire                           worlds idle past HostedRetireDays put aside now
//   stats                            players, worlds, the link thread's load, refusals
//   key                              the link's public key (what the launcher pins)
#pragma once

#include <functional>
#include <optional>

#include <QJsonObject>
#include <QLocalServer>
#include <QString>

#include "admission.h"
#include "bloodborne_world.h"
#include "server_link.h"

namespace Admin {

struct Context {
    ServerLink::Hub& hub;
    BloodborneWorld::Service& world;
    Admission::Guard& guard;
    QString ban_list;     // banned.txt: bans are written into it, the operator's lines kept
    qint64 link_ms = 0;   // the hub's clock
    qint64 wall_ms = 0;   // the wall clock: bans' ends, the worlds' last played
    qint64 retire_ms = 0; // a world idle this long retires (0: never)
    double link_load = 0; // the link thread's busy share lately (0-1)
};

struct Answer {
    QJsonObject json; // "ok", and "error" when not
    // To apply as the link's and the worlds' own results (a kicked game's link gone, a world's
    // log lines).
    ServerLink::Hub::Result link;
    BloodborneWorld::Service::Result world;
    // What reads the disk (the worlds' sizes), to run on another thread before the answer goes:
    // it fills `json` in. It touches nothing of the link's or the worlds'.
    std::function<void(QJsonObject& json)> finish;
};

Answer Run(const QString& line, Context& context);

// "90s", "30m", "12h", "7d": seconds.
std::optional<qint64> ParseDuration(const QString& text);

// banned.txt read again into the guard, the bans past their end left out.
void LoadBans(const QString& path, qint64 now_s, Admission::Guard& guard);
// The linked games the guard bans (by id or address), unlinked: a ban holds at once, not only at
// the game's next hello.
ServerLink::Hub::Result KickBanned(ServerLink::Hub& hub, const Admission::Guard& guard,
                                   qint64 link_ms);

// The admin socket at `path`: refused while another server answers on it (a second one started by
// mistake must not unlink the running one's), else one left by a crash is replaced. `why`: why not.
bool ListenAdmin(QLocalServer& server, const QString& path, QString& why);

} // namespace Admin
