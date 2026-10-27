// SPDX-FileCopyrightText: Copyright 2026 shadNet Project
// SPDX-License-Identifier: GPL-2.0-or-later
// The party's world on the server (docs/SERVER_COOP_PLAN.md phase 3): shadNet keeps each party's
// world log (coop/world_log.h, WorldLogHost) instead of the host's game, orders what the members
// propose, stores it on disk and sends every member the entries, and a snapshot when it asks.
//
// The messages (coop/coop.proto) come in the server link's Coop datagrams, in each link's
// reliable stream (coop/coop_stream.h). A party is its key (SHADPS4_BLOODBORNE_PARTY, from the
// member's hello); on disk it goes by a hash of the key, never the key itself.
//
// The epochs are the server's: the world's id (random, kept with the world) and its count of logs
// started. A wipe starts a new log once for the party; the session's host joining, leaving or
// becoming another character starts one too (a new session); a guest's own events take only its
// share out of the log.
#pragma once

#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>

#include <QByteArray>
#include <QDate>
#include <QList>
#include <QString>
#include <QStringList>

#include "bloodborne_party_boards.h"
#include "coop.pb.h"
#include "coop/coop_stream.h"
#include "coop/duel.h"
#include "coop/invasions.h"
#include "coop/team_lobby.h"
#include "coop/trade.h"
#include "coop/world_log.h"
#include "server_link.h"

namespace BloodborneWorld {

// One party's world on disk, in its folder: world.pb (the world as of its journal_from), the
// journal of changes since (journal.pb: each record its size, u32 little-endian, then the record),
// and the last three worlds before (world.pb.1-3). A world is written whole to a new file and
// renamed over the old one (QSaveFile), so a crash leaves the old one or the new one.
class WorldStore {
public:
    explicit WorldStore(QString folder);

    struct Loaded {
        bbcoop::StoredWorld world;
        QList<bbcoop::JournalRecord> journal; // the records from world.journal_from on
        bool journal_cut = false; // its journal ends in bytes that do not read (a crash)
    };
    // The world and its journal, or nothing (no world here yet). A world file that does not read
    // falls back to the backups; a journal cut short ends at its last whole record.
    std::optional<Loaded> Load(QStringList& log) const;
    bool Append(const bbcoop::JournalRecord& record);
    // The world written whole (the old one to the backups) and the journal emptied. Phase 11:
    // the first write of each day is also kept as backups/world-<yyyyMMdd>.pb, the last 7 days.
    bool Compact(const bbcoop::StoredWorld& world, const QDate& today = QDate::currentDate());
    // The party's trades (trades.pb, coop/trade.h), written whole on every change.
    std::optional<bbcoop::StoredTrades> LoadTrades() const;
    // Fault checkpoints let focused storage tests stop before/after the commit boundary.
    enum class TradesWritePoint { BeforeFileSync, BeforeRename, AfterRename, AfterDirectorySync };
    using TradesWriteCheck = std::function<bool(TradesWritePoint)>;
    bool SaveTrades(const bbcoop::StoredTrades& trades, const TradesWriteCheck& check = {});
    const QString& Folder() const {
        return folder;
    }

private:
    QString folder;
};

// The worlds' disk writes, on a thread of their own (security review P1 12): the link thread
// that relays every game's packets only queues them. One queue for every world, written in the
// order given, so each world's records reach its journal in order and a Compact comes after the
// records it holds. Group commit: the journals written to are fsynced every commit_ms, after
// commit_records records, before a world is written whole (journal before world.pb, as the
// backups read them) and before a folder moves. A crash loses at most the records not fsynced
// yet (a tail: the order holds); the journal still reads (WorldStore::Load stops at a cut record).
class WorldWriter {
public:
    explicit WorldWriter(qint64 commit_ms = 100, int commit_records = 256);
    // Drains: everything queued is on disk.
    ~WorldWriter();
    WorldWriter(const WorldWriter&) = delete;
    WorldWriter& operator=(const WorldWriter&) = delete;

    void Append(const QString& folder, bbcoop::JournalRecord record);
    void Compact(const QString& folder, bbcoop::StoredWorld world,
                 const QDate& today = QDate::currentDate());
    void SaveTrades(const QString& folder, bbcoop::StoredTrades trades);
    // Queue behind all older snapshots, then wait for checked file+directory durability.
    bool SaveTradesDurable(const QString& folder, bbcoop::StoredTrades trades,
                          WorldStore::TradesWriteCheck check = {});
    // A whole file (team_world.pb, worlds.pb), replaced at once (QSaveFile).
    void Save(const QString& path, QByteArray bytes);
    // team_world.pb in `dir`, with the backups before it and the day's (save-restore).
    void SaveTeamWorld(const QString& dir, bbcoop::TeamWorld world,
                       const QDate& today = QDate::currentDate());
    // `name` in `parent` moves to `to` (in `parent`): a world's folder put aside.
    void Move(const QString& parent, const QString& name, const QString& to);
    // Whether a Move of the folder `parent`/`name` is queued (what is in it is not known yet).
    bool Moving(const QString& parent, const QString& name) const;
    // Whether writes in `folder` (a world's) are queued.
    bool Pending(const QString& folder) const;
    // Returns when everything queued before it is on disk.
    void Drain();
    // Stops at once, writing nothing more and fsyncing nothing (a crash, for the tests).
    void Abandon();
    // What went wrong since the last call, for the server's log.
    QStringList TakeLog();
    // Records fsynced, and fsync rounds, so far (the tests).
    quint64 Synced() const;
    quint64 Commits() const;

private:
    struct State;
    std::unique_ptr<State> state;
};

// The world key of a party: a hash of its key, safe as a folder name.
QString PartyFolderName(const QByteArray& party);

// Hosted (docs/HOSTED_SERVER.md): shadNet for players who do not know each other. A player's
// names in the lobby and the party are its online id's (hex, as the games say them), a party hello
// must be for the world its team plays, and the lobby goes out at most every lobby_interval_ms,
// only to the players whose own state changed, listing few players (TeamLobby::SetHosted).
struct HostedRules {
    bool enabled = false;
    size_t lobby_players = 32;
    qint64 lobby_interval_ms = 250;
    size_t worlds_per_player = 16;
};

// Invasions (docs/INVASIONS.md): off unless the server's operator turns them on, here or at home.
// The invader's kind and sign (GameMan +0x16F0 / +0x16F4) are what its game joins the target's
// session with: placeholders until the retail capture (phase 0) says the real ones.
struct InvasionRules {
    bool enabled = false;
    quint32 kind = 0xFFFFFFFF; // InvasionKind: the co-operator's kind until the capture
    quint32 sign = 4;          // InvasionSign: the slot after the guests' 7, 6, 5 (a guess)
};

// What one game can make the service hold (docs/HOSTED_SERVER.md, "Caps"), for every server: far
// above what a game sends, so only a broken or hostile one meets them.
struct Limits {
    // A message from a game, and the chunks it may have held out of order. The largest a game
    // sends is its TeamWorld report: every world flag on in its save as a varint (~4 bytes each;
    // tens of thousands at most) and a portrait (128x128 RGB, 48 KiB). Past it the member's stream
    // is dropped (the game starts it over and says hello again).
    size_t message_bytes = 1 << 20;
    size_t held_bytes = 1 << 20;
    // What the server queued to a game that has not acked it: a game acks every frame, so only
    // one that never does nears it (the hub keeps a link alive on its pings alone). Past it the
    // member is dropped.
    size_t unacked_bytes = 8 << 20;
    // The parties' worlds in memory at once, and how long one nobody is in stays (then only on
    // disk; the next hello reads it again). A world kept in memory only (no folder) stays.
    size_t party_worlds = 1024;
    qint64 idle_world_ms = 5 * 60 * 1000;
    // What a collected world keeps for its next read (where its guests stood, its boards): for so
    // long, and for so many worlds at most (the oldest go first).
    qint64 kept_ms = 24 * 60 * 60 * 1000;
    size_t kept_worlds = 4096;
};

// The fields of a game's message (coop.proto), each far above the game's own: a message past one
// is dropped whole (and logged).
constexpr size_t MaxNameBytes = 64;                // board names: an online id (16) in hex
constexpr size_t MaxCharacterBytes = 96;           // a character's name: 16 UTF-16 units, as UTF-8
constexpr size_t MaxPartyKeyBytes = 256;           // "world:<id>", "<name>:<secret>"
constexpr size_t MaxPortraitBytes = 128 * 128 * 3; // bloodborne_portrait.h: 128x128 RGB888
constexpr size_t MaxJoinBytes = 4096;         // the game's join data: 0x200 (bloodborne_join.cpp)
constexpr size_t MaxListEntries = 4096;       // shown characters, HP enemies, flag events, echoes
constexpr size_t MaxKillHandles = 32768;      // a member's standing kills (a map: a few hundred)
constexpr size_t MaxTeamWorldFlags = 250'000; // TeamWorld.on, within message_bytes
constexpr size_t MaxSmallList = 64; // trophy ids (the game has 34), trade items, revive hits

class Service {
public:
    // `folder`: where the worlds are kept (one folder per party); empty keeps them in memory only.
    explicit Service(QString folder, qint64 heartbeat_ms = 1000, int compact_records = 200,
                     qint64 compact_ms = 30'000);

    struct Send {
        ServerLink::Endpoint to;
        QByteArray body; // a Coop datagram's body
    };
    // A region of shards (docs/SCALING.md): a linked game this shard sends to another one (its
    // redirect carries the requests in a ticket), and an invite for another shard's player.
    struct Move {
        ServerLink::Endpoint link;
        QByteArray online_id;
        std::string shard;
        std::vector<bbcoop::TeamRequest> requests;
    };
    struct Result {
        QList<Send> sends;
        QStringList log;
        std::vector<Move> moves;
        std::vector<BloodborneCoop::TeamLobby::RemoteInvite> remote_invites;
        QList<QByteArray> started; // players whose team started here (the directory's homes)
    };

    void SetHosted(const HostedRules& rules);
    // A region: the directory's view for the lobby's lookups, and this shard's name.
    void SetRegion(BloodborneCoop::TeamLobby::Region region, std::string shard);
    // Invites from other shards' teams for players here (the heartbeat's inbox).
    Result TakeInvites(const std::vector<std::pair<std::string, bbcoop::TeamInvite>>& invites,
                       qint64 now_ms);
    // A player that came with a ticket: its requests run at its lobby hello (once), as its own.
    void Expect(const std::string& member, std::vector<bbcoop::TeamRequest> requests);
    void SetInvasions(const InvasionRules& rules) {
        invasion_rules = rules;
    }
    // For the members that link from now on (and the worlds).
    void SetLimits(const Limits& limits) {
        this->limits = limits;
    }

    // Every party's world in the folder (shadNet's start).
    Result Load(qint64 now_ms);
    // A Coop datagram's body from a linked game.
    Result Receive(const ServerLink::Endpoint& from, const QByteArray& online_id,
                   const QByteArray& body, qint64 now_ms);
    // Resends, acks and heartbeats; worlds written whole when their journal grew.
    Result Tick(qint64 now_ms);
    // A link went (its game is gone).
    Result Forget(const ServerLink::Endpoint& link);
    // A link silent for a while (Hub::Result::lost): an invasion it is in ends.
    Result LinkLost(const ServerLink::Endpoint& link);
    // Invasions: the party a hostile visitor's link visits, empty when it visits none (the hub's
    // teams: it shares the visited party's routing).
    QString VisitingOf(const ServerLink::Endpoint& link) const;
    // The party a link's game said in its hello, empty before one (phase 7: the hub's teams).
    QString PartyOf(const ServerLink::Endpoint& link) const;
    // Phase 9: whether a game packet on `channel` goes from one link's game to another's, by the
    // channel table, their maps and their parts in their maps' enemies (coop/packet_routes.h). A
    // link without a member here gets only the game's own channels.
    bool RoutesPacket(const ServerLink::Endpoint& from, const ServerLink::Endpoint& to,
                      std::uint32_t channel) const;

    // The team lobby (phase 5), for the tests and the log.
    const BloodborneCoop::TeamLobby& Lobby() const {
        return lobby;
    }

    // Returns when the worlds' writes so far are on disk (the tests; the service drains when it
    // goes).
    void Drain();

    // The operator's (admin.h). Where a linked game is: its lobby name, its team, the world it
    // plays (its team's pick, or its party's), its map; nothing before its first message.
    struct Place {
        std::string lobby;
        std::string team;
        std::string world;
        std::uint32_t map = 0;
    };
    std::optional<Place> PlaceOf(const ServerLink::Endpoint& link) const;
    // The listed worlds: each one's line, folder and games in it now. Their bytes on disk are not
    // read here (every file of every world): BytesOn(path), off the link's thread (admin.cpp).
    struct WorldRow {
        bbcoop::TeamWorldInfo info;
        QString folder;
        QString path; // the folder on disk (none: in memory only)
        qint64 bytes = 0;
        int games = 0;
    };
    std::vector<WorldRow> WorldRows() const;
    static qint64 BytesOn(const QString& path);
    const QString& Folder() const {
        return folder;
    }
    // The worlds put aside (forgotten/<folder>-<when>): the line each had, when it was kept.
    struct AsideRow {
        QString folder;
        qint64 bytes = 0;
        std::optional<bbcoop::TeamWorldInfo> info;
    };
    std::vector<AsideRow> AsideRows() const;
    // The same from the disk alone (any thread).
    static std::vector<AsideRow> AsideRowsIn(const QString& folder);
    // Worlds nobody played for `idle_ms` (and no team picked, no game is in) off the list, their
    // folders put aside as a forgotten world's (never deleted): their ids.
    std::vector<std::string> Retire(qint64 now_ms, qint64 idle_ms, Result& result);
    // A world put aside (its folder in forgotten/) back on the list, to the players it had, as
    // played now (the next retirement keeps it). `said`: its id, or why it could not.
    bool Restore(const QString& aside, qint64 now_ms, QString& said, Result& result);

    // For the tests and the log.
    const BloodborneCoop::WorldLogHost* World(const QByteArray& party) const;
    int Parties() const {
        return static_cast<int>(worlds.size());
    }
    int KeptWorlds() const {
        return static_cast<int>(kept.size());
    }

private:
    struct PartyWorld {
        QString name; // PartyFolderName
        BloodborneCoop::WorldLogHost host;
        quint32 world = 0;
        quint32 logs = 0;
        qint64 last_wipe_ms = 0;
        // When each map last started over on its own (WorldLogHost::ResetMap), by map.
        std::map<std::uint32_t, qint64> map_reset_ms;
        quint64 records = 0;      // journal records written (the next one's number)
        quint64 journal_from = 0; // the first record not in the stored world
        qint64 compacted_ms = 0;
        std::unique_ptr<WorldStore> store;
        qint64 heartbeat_ms = 0;
        // Each member's last kills taken into the log: a proposal that changes them is journaled.
        std::map<std::string, BloodborneCoop::WorldLogKills> proposed;
        // Phase 3c: the boards (in memory only: they are about the games running now) and each
        // member's last portrait.
        PartyBoards boards;
        std::map<std::string, bbcoop::Portrait> portraits;
        // Phase 6: the host's open game session, for its guests to join (none: closed).
        std::optional<bbcoop::TeamSession> session;
        // Each member's load (the team loading screen), by member name.
        std::map<std::string, bbcoop::LoadProgress> loads;
        // Phase 7: where each member last stood (its last vitals up, by member name), for its
        // next run to resume there.
        struct Place {
            bbcoop::ResumeAt at;
            std::string character; // whose (CharacterOf): another character resumes nowhere
        };
        std::map<std::string, Place> places;
        u32 session_run = 0; // the host game's run that opened it
        // Phase 4: who runs each area's enemies, and what was logged of it.
        EnemyOwners owners;
        QString owners_logged;
        QString vitals_logged; // the members' states and the wipes, as last logged
        // Its trades (coop/trade.h): the offers held and the grants owed.
        BloodborneCoop::TradeDesk trades;
        bool reward_mailbox = false; // trades.pb alone is authoritative; no world.pb dependency
        // Kaniel 2026-10-25: its duels (coop/duel.h), in memory only (about the games running).
        BloodborneCoop::DuelReferee duels;
        // Phase 5b: a team's world (its party key is a team's), from its host's reports; read
        // from team_world.pb when the party first comes.
        bool team = false;
        std::optional<bbcoop::TeamWorld> team_world;
        bool team_world_read = false;
        // Kaniel 2026-10-17: which of the server's worlds (TeamWorldInfo.id) this party is, from
        // its key ("world:<id>").
        std::string team_world_id;
        std::optional<qint64> empty_since_ms; // nobody in it since (Collect)
    };
    // What a world collected while nobody was in it keeps until it is read again.
    struct Kept {
        std::map<std::string, PartyWorld::Place> places;
        PartyBoards boards;
        qint64 since_ms = 0; // collected then (Limits::kept_ms)
    };
    // The worlds Collect takes out at once for room (each is written whole).
    static constexpr size_t CollectAtOnce = 4;
    struct Member {
        Member(quint32 instance, BloodborneCoop::CoopStream::Limits limits)
            : stream{"shadnet", instance, limits} {}
        BloodborneCoop::CoopStream stream;
        bool to_flush = false; // something to send since the last flush (Service::flush_now)
        quint64 key = 0;       // its link's (Endpoint::Key)
        QByteArray online_id;
        QString party; // PartyFolderName, empty before its hello
        std::string name;
        quint32 run = 0;
        bool host = false; // hosts the session (its hello says): its characters and map state count
        // Runs phase 4's enemy owners (its hello says): on an area another member owns its new
        // kills and its HP reports do not count.
        bool enemy_owner = false;
        std::string lobby;        // its name in the team lobby, empty before its lobby hello
        bool lobby_again = false; // its game started over: its next lobby hello is a first one
        std::uint32_t map = 0;    // the map it last said (0: loading, or none yet)
        // Kaniel 2026-10-16: the team world it was taken out of (removed from the team, or left
        // it): no game packet goes between it and that party until its next hello.
        QString cut_from;
        // Invasions: the party it visits as an invader (hostile): it shares that party's routing
        // by the hostile table, and is no member of its world.
        QString visiting;
        bool hostile = false;
        // Its last vitals' level and boss fight (the matcher's).
        std::uint32_t level = 0;
        bool boss = false;
        std::string display; // its lobby hello's name (as players see it)
        bool invasion_rewards = false; // LobbyHello's explicit extension capability
        bool covenant_rewards = false; // can consume the extra Blood Dreg reward item
        qint64 reward_retry_ms = 0; // disk failure backoff; never spin on dead Vitals
        std::uint32_t covenant_effect = 0; // normalized native oath marker, last vitals
    };
    // Invasions: one invader on its way to (or in) a team world.
    struct Invasion {
        quint64 id = 0;
        quint64 invader = 0;      // its link (Endpoint::Key)
        std::string invader_name; // its lobby name
        std::string target;       // the member it hunts: the session's host
        std::uint32_t invader_level = 0; // frozen at match; never the death report's level
        std::uint32_t target_level = 0;  // frozen at join
        std::uint32_t invader_covenant = 0; // frozen at match, from the invasion request
        std::uint32_t target_covenant = 0;  // frozen at join, never the death report's oath
        qint64 matched_ms = 0;
        bool joined = false;
        qint64 joined_ms = 0;
        std::set<int> dropped; // the kinds of its messages dropped (each logged once)
        // The invader's last vitals: a hostile line on the team's vitals board (its body stays
        // shown on the team's screens).
        std::optional<bbcoop::Vitals> vitals;
    };
    struct Search {
        std::uint32_t level = 0;
        qint64 since_ms = 0;
        std::uint32_t covenant_effect = 0;
    };

    PartyWorld& WorldFor(const QString& name, qint64 now_ms, Result& result);
    // A world from its folder (Load, and a world collected while nobody was in it): whether one
    // was there.
    bool LoadWorld(const QString& name, qint64 now_ms, Result& result);
    // Whether a world for `name` is in memory or may be (read again, or made).
    bool RoomFor(const QString& name, qint64 now_ms, Result& result);
    // Worlds nobody is in (for idle_world_ms, or `all` of them) out of memory, written whole.
    void Collect(qint64 now_ms, bool all, Result& result);
    // A member past the limits: forgotten as when its link goes (its game's next datagram meets
    // a new stream and says hello again).
    void Drop(quint64 key, const QString& why, Result& result);
    // Why a game's message is past the field limits, or nothing.
    static std::optional<QString> Oversized(const bbcoop::Envelope& envelope);
    void Take(Member& member, const bbcoop::Envelope& envelope, qint64 now_ms, Result& result);
    void OnHello(Member& member, const bbcoop::Hello& hello, qint64 now_ms, Result& result);
    void OnLifecycle(Member& member, const bbcoop::Lifecycle& lifecycle, qint64 now_ms,
                     Result& result);
    void StartLog(PartyWorld& world, bool after_wipe, qint64 now_ms, Result& result);
    // `map` (and the maps loaded with it) starts over on its own (WorldLogHost::ResetMap).
    void ResetMap(PartyWorld& world, std::uint32_t map, qint64 now_ms);
    void Journal(PartyWorld& world, bbcoop::JournalRecord record, qint64 now_ms);
    void Compact(PartyWorld& world, qint64 now_ms);
    void Replay(PartyWorld& world, const bbcoop::JournalRecord& record);
    // The host's reports and every member's flags and HP (phase 3b): taken into the log, journaled
    // and the entries sent.
    void TakeWorld(Member& member, PartyWorld& world, const bbcoop::Envelope& envelope,
                   qint64 now_ms, Result& result);
    // The boards' messages (phase 3c), and the changed boards to the party.
    void TakeBoard(Member& member, PartyWorld& world, const bbcoop::Envelope& envelope,
                   qint64 now_ms, Result& result);
    void SendBoards(PartyWorld& world, unsigned changed, qint64 now_ms, Member* only = nullptr);
    // The vitals board, with a joined invader's hostile line.
    bbcoop::VitalsBoard VitalsBoardOf(const PartyWorld& world) const;
    // The enemy owners to the party (or `only`), logged when they changed.
    // The host's session closed (its game gone or started again): the others hear it.
    // Whether the member hosting the party's open session is linked (phase 7).
    bool SessionHostLinked(const PartyWorld& world) const;
    // The character a member readied in its team (save slot and name), empty when none.
    std::string CharacterOf(const std::string& member) const;
    void CloseSession(PartyWorld& world, qint64 now_ms, Result& result);
    void SendOwners(PartyWorld& world, qint64 now_ms, Result& result, Member* only = nullptr);
    // Whether `member` may decide the enemies of `map` (phase 4): a game without the owners, or
    // on an area nobody or itself runs.
    bool Decides(const PartyWorld& world, const Member& member, u32 map) const;
    // A member's trade messages, and what the desk says to send and store.
    void TakeTrade(Member& member, PartyWorld& world, const bbcoop::Envelope& envelope,
                   qint64 now_ms, Result& result);
    void Deliver(PartyWorld& world, BloodborneCoop::TradeDesk::Result trades, qint64 now_ms,
                 Result& result);
    // A member's duel messages; the duel's state to every member when it changed.
    void TakeDuel(Member& member, PartyWorld& world, const bbcoop::Envelope& envelope,
                  qint64 now_ms, Result& result);
    std::vector<BloodborneCoop::DuelMember> DuelMembers(const PartyWorld& world) const;
    void SettleDuel(PartyWorld& world, const BloodborneCoop::DuelReferee::Result& duel,
                    qint64 now_ms, Result& result);
    void SendToOthers(PartyWorld& world, const Member& from, const bbcoop::Envelope& envelope,
                      qint64 now_ms);
    void SendTo(Member& member, const bbcoop::Envelope& envelope, qint64 now_ms);
    void SendEntries(PartyWorld& world, const std::vector<BloodborneCoop::WorldLogEntry>& entries,
                     qint64 now_ms);
    void Flush(qint64 now_ms, Result& result);
    // Only the members sent something (or heard from) since the last flush: a datagram from one
    // game must not walk every member's stream (O(n) a datagram was the limit past ~600 games).
    void FlushSome(qint64 now_ms, Result& result);
    std::string Who(const Member& member) const;
    // The team lobby (phase 5): a player's sign-in and requests; every signed-in player's state
    // after a change, a TeamStart to those the outcome starts, the world list to disk.
    void OnLobbyHello(Member& member, const bbcoop::LobbyHello& hello, qint64 now_ms,
                      Result& result);
    // `ticket`: a request a region ticket carried (a game's CREATE names no code).
    void OnTeamRequest(Member& member, const bbcoop::TeamRequest& request, qint64 now_ms,
                       Result& result, bool ticket = false);
    void Settle(const BloodborneCoop::TeamLobby::Outcome& outcome, qint64 now_ms, Result& result);
    // A member out of its team: out of that team's world at once (its share, boards, owners, load
    // and session go, as when its game leaves) and cut off from the team's games.
    void TakeOutOfTeam(const BloodborneCoop::TeamLobby::Departure& out, qint64 now_ms,
                       Result& result);
    // What goes when a guest leaves its party's world (its game said so, or its team let it go).
    void ForgetMember(PartyWorld& world, const std::string& name, qint64 now_ms, Result& result);
    void SendLobby(qint64 now_ms);
    // Phase 5b: the team's world, read from its folder once; a host's report taken.
    const std::optional<bbcoop::TeamWorld>& TeamWorldOf(PartyWorld& world);
    void TakeTeamWorld(Member& member, PartyWorld& world, const bbcoop::TeamWorld& report,
                       qint64 now_ms, Result& result);
    void SendTeamWorld(PartyWorld& world, Member& member, qint64 now_ms);
    // Kaniel 2026-10-17: the server's list of worlds (worlds.pb). Read at the start, made once
    // from the teams file of the builds that kept teams and from the world folders not on it
    // (worlds kept before the list); written when it changes.
    void LoadWorldList(Result& result);
    void StoreWorldList(Result& result);
    // The folder of a party: its world's on the list ("world:<id>"), else a hash of its key.
    QString FolderOf(const QByteArray& party) const;
    // Worlds off the list (`why`: forgotten, retired): their folders go aside
    // (forgotten/<folder>-<when>) with their lines (kept_world.pb, for Restore), nothing deleted.
    void PutAside(const std::vector<bbcoop::TeamWorldInfo>& lines, const QString& why,
                  Result& result);

    QString folder;
    std::unique_ptr<WorldWriter> writer; // with a folder
    qint64 heartbeat_ms;
    int compact_records;
    qint64 compact_ms;
    std::map<QString, PartyWorld> worlds;
    std::map<QString, Kept> kept;      // by world (Collect)
    std::map<quint64, Member> members; // by link (Endpoint::Key)
    std::map<quint64, ServerLink::Endpoint> endpoints;
    std::vector<Member*> flush_now; // the members FlushSome flushes
    // Invasions (docs/INVASIONS.md).
    void OnInvadeRequest(Member& member, const bbcoop::InvadeRequest& request, qint64 now_ms,
                         Result& result);
    // An invader's messages: its map and vitals; the rest dropped (it is in no world here).
    void TakeHostile(Member& member, const bbcoop::Envelope& envelope, qint64 now_ms,
                     Result& result);
    void Match(qint64 now_ms, Result& result);
    BloodborneCoop::InvasionTarget TargetOf(const PartyWorld& world) const;
    void JoinInvasion(PartyWorld& world, Invasion& invasion, qint64 now_ms, Result& result);
    // The invasion of a world over: InvasionEnd to everyone in it, the invader out of its
    // routing, the cooldowns started.
    void EndInvasion(const QString& world, bbcoop::InvasionEnd::Reason reason, qint64 now_ms,
                     Result& result, bool killed_by_invader = false);
    // What a team member's vitals end (its target killed, a boss, a wipe).
    void CheckInvasion(const PartyWorld& world, const Member& member, const bbcoop::Vitals& vitals,
                       std::uint32_t wipe_before, qint64 now_ms, Result& result);
    // The world whose invasion `link` is the invader of, empty when none.
    // A deterministic mailbox of the authenticated link identity, independent of party/team:
    // solo invaders have no party Hello. Its grants use InvasionRewardGrantBit for safe ACKs.
    QString RewardFolder(const Member& member) const;
    void ResumeRewards(Member& member, qint64 now_ms, Result& result);
    PartyWorld* RewardMailbox(Member& member, bool create, qint64 now_ms, Result& result);
    bool DeliverRewards(Member& member, PartyWorld& mailbox, BloodborneCoop::TradeDesk staged,
                        BloodborneCoop::TradeDesk::Result grants, qint64 now_ms, Result& result);
    bool RewardInvasion(Member& winner, quint64 invasion, const std::string& defeated,
                        std::uint32_t echoes, std::uint32_t covenant_effect,
                        qint64 now_ms, Result& result);
    QString InvadedBy(quint64 link) const;
    // An invasion's link gone: its own (the invader's) or its target's host's.
    void InvasionLinkGone(quint64 link, qint64 now_ms, Result& result);
    void Refuse(Member& member, bbcoop::InvasionEnd::Reason reason, const std::string& text,
                qint64 now_ms);

    // Hosted: whether a name a game says is its own (its online id in hex).
    bool OwnName(const Member& member, const std::string& name) const;

    BloodborneCoop::TeamLobby lobby;
    HostedRules hosted;
    Limits limits;
    qint64 collected_ms = 0;
    qint64 lobby_sent_ms = 0;
    std::map<std::string, std::string> lobby_sent; // hosted: each player's last state, as sent
    bool lobby_changed = false; // a player's game went: states to send at the next tick
    std::map<std::string, std::vector<bbcoop::TeamRequest>> expected; // Expect, by member
    // The listed worlds' folders by world id, for those not in PartyFolderName("world:<id>").
    std::map<std::string, QString> world_folders;

    InvasionRules invasion_rules;
    std::map<QString, Invasion> invasions; // by the invaded world
    std::map<quint64, Search> searches;    // by the invader's link
    BloodborneCoop::InvasionCooldowns cooldowns;
    quint64 invasions_made = 0;
    qint64 matched_ms = 0; // the searches' last look
    qint64 clock_ms = 0;   // the last time a call said (for the calls that say none)
};

} // namespace BloodborneWorld
