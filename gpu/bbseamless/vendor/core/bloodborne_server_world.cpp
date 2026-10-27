// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// The party's world on the server (docs/SERVER_COOP_PLAN.md phase 3): in the port (always, over
// the server link; never in shadPS4) shadNet keeps the party's world log, not the host's game.
// Every member, the host too, proposes to it and follows its log as a WorldLogMember
// (server/coop/world_log.h): the kills (3a; the kill board is not used then), the host's
// characters, map state and return point, every member's shared flags and HP reports (3b; the
// host's own log is not run then), and what the HTTP boards carried: maps, rests, trophies, kill
// echoes, vitals and portraits (3c; the boards are not used then).
//
// The messages (server/coop/coop.proto) go in the server link's Coop datagrams, in their reliable
// stream (server/coop/coop_stream.h). A thread of its own runs the stream (receives, resends,
// acks) every StreamTickMs; the game's thread only queues messages and takes what came (the kill
// step). No hook on the game's per-frame network update (phase 2: a hook there stalled joins).

#include "core/bloodborne_re_internal.h"
#include "core/bloodborne_trade.h"

#include <deque>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <optional>
#include <random>
#include <set>
#include <thread>

#include <fmt/format.h>
#include "common/logging/log.h"
#include "common/path_util.h"
#include "coop/coop_messages.h"
#include "coop/coop_stream.h"
#include "core/bloodborne_invasion.h"
#include "core/bloodborne_team.h"
#include "core/libraries/network/server_link.h"
#include "core/libraries/np/np_matching2/np_matching2.h"

namespace Core::Bloodborne {
namespace {

constexpr s64 StreamTickMs = 20;
// A member's standing kills and the host's reports go again this often while they stay the same (a
// restart of the server drops what was in flight; the same again changes nothing there).
constexpr s64 KillsResendMs = 2'000;
constexpr s64 ReportResendMs = 5'000;

// The server's world, phase 4's enemy owners (see EnemyRole) and phase 5's teams: the port's only
// way since phase 6 (no switches); shadPS4 keeps the old path (docs/SERVER_COOP_PLAN.md).
#ifdef BLOODBORNE_NATIVE_PORT
constexpr bool server_world_switch = true;
#else
constexpr bool server_world_switch = false;
#endif
constexpr bool enemy_owner_switch = server_world_switch;

// The server's enemy owners as last heard (the stream's thread writes, any thread reads).
std::mutex owners_mutex;
std::vector<EnemyOwnerEntry> server_owners;
s64 owners_heard_ms = 0;

const u32 run = [] {
    std::random_device random;
    return static_cast<u32>(random()) | 1u;
}();

// The stream and its queues: the world thread and the game thread.
std::mutex stream_mutex;
std::optional<CoopStream> stream;
std::deque<bbcoop::Envelope> inbox;
std::deque<bbcoop::Envelope> outbox;
// Something for the kill step came (see TakeServerWorldNews).
std::atomic<bool> inbox_news{false};
// The others' hits (bbcoop::EnemyHit) as they came, for the next enemy step.
std::mutex enemy_hits_mutex;
std::vector<EnemyHitNews> enemy_hits;
std::mutex altar_records_mutex;
std::vector<AltarRecordNews> altar_records;
// Kaniel 2026-10-24: the team's pings (bbcoop::Ping) as they came, for the next pings tick.
std::mutex team_pings_mutex;
std::vector<TeamPing> team_pings;
// Kaniel 2026-10-25: the party's duel states as they came, for the next duel tick.
std::mutex duel_states_mutex;
std::vector<bbcoop::DuelState> duel_states;
std::string hello_party;
std::string hello_member;
bool hello_due = true;
std::once_flag thread_started;

// Phase 5: the team lobby (bloodborne_team.h). The lobby's messages go on the same stream, before
// any party hello; the party is the team's from its Start.
constexpr bool team_switch = server_world_switch;
std::mutex team_mutex;
TeamGate team_gate;
std::string team_party;   // the team's party key once its Start let this game's load go
std::string lobby_member; // this player in the lobby (its board name), once known
std::string lobby_name;   // its character's name (phase 11), "" before one is known
s64 lobby_name_read_ms = 0;
s64 title_load_ms = 0; // the title's last save load (CharacterNameReadable)
std::atomic<s64> save_opened_ms{0};
bool lobby_playing = false;  // in a world (the load went)
bool lobby_hello_due = true; // stream restarts and changes of lobby_playing
std::deque<bbcoop::Envelope> lobby_outbox;
// Phase 5b: the team's world as the server last sent it (none before), and what this game does
// with it: a save playing in a team takes it as its loads bring each flag block (team_world_due;
// a block once, team_world_blocks: the area's own changes stay), the team's host reports its own
// once its save took the team's (team_host, team_world_taken), a save loaded alone takes its own
// world back (personal_world_due).
std::optional<bbcoop::TeamWorld> team_world;
std::optional<ResumePoint> server_resume_point; // ResumeAt, until the resume takes it
bool server_resume_refused = false;             // RefuseServerResumePoint: this run's is not ours
bool team_world_due = false;
bool team_world_taken = false;
std::set<u32> team_world_blocks;
bool team_host = false;
bool personal_world_due = false;
WorldFlags team_world_reported;
s64 team_world_reported_ms = 0;
// A guest: the team world its game last took or followed (TeamStoryToFollow's `before`).
WorldFlags team_story_followed;
constexpr s64 TeamWorldReportMs = 10'000;
// A load held for the team's world after the Start gives up waiting after this long.
constexpr s64 TeamWorldWaitMs = 10'000;
s64 team_world_wait_ms = 0;
// Phase 6: the team join without signs or a bell (see TeamJoin). The host's open session as the
// server last sent it (a guest joins it), the game thread's step, and the sessions this host
// opened (each one's number).
std::optional<bbcoop::TeamSession> team_session;
TeamJoin team_join;
TeamLoad team_load;           // team_mutex
ReadySettle team_load_settle; // team_mutex
// The team loading screen as the game's thread last made it (the present thread draws it).
std::mutex team_load_view_mutex;
TeamLoad::View team_load_view;
// The screen is up: the port's sound holds (its audio thread reads it every buffer).
std::atomic<bool> team_load_covers{false};
// A guest's team load still to be put beside its host (TeamResumeDue).
std::atomic<bool> team_resume_due{false};
// Kaniel 2026-10-15: a cutscene under that screen is skipped. The cutscene player's (SprjRemoMan)
// step as its step methods last ran (NoteRemoStep), and Options held for the skip (the pad reads
// it).
std::atomic<int> remo_step{-1};
std::atomic<s64> remo_step_ms{0};
CutsceneSkip cutscene_skip; // CutsceneSkipPressed (the pad's reads)
std::atomic<bool> skip_pressed{false};
// Kaniel 2026-10-17: Options under a party's cutscene is a vote (the pad's reads; the overlay shows
// the count).
std::mutex cutscene_vote_mutex;
CutsceneVote cutscene_vote;
std::atomic<u64> cutscene_vote_shown{0}; // want << 32 | watchers, 0: none
// The others' votes on the vitals board count while it is this fresh.
constexpr auto CutsceneVoteFresh = std::chrono::seconds{3};
// The rig's press under a cutscene (ArmCutsceneOptions): after this long, -1 none; and since when
// this game's cutscene plays.
std::atomic<s64> cutscene_options_after{-1};
s64 cutscene_watch_since_ms = 0; // cutscene_vote_mutex
u64 team_sessions_opened = 0;
// Kaniel 2026-10-16: the team's worlds. The world this save last played in its team (its slot's
// "team_world" file), whether the title starts it new, and whether the leader's screen picked its
// default world for this hold yet.
std::string team_world_hint;
bool title_fresh = false;
bool team_world_default_sent = false;
// Kaniel 2026-10-16: this save left its team while playing (removed, or left from the menu): it
// still clears the leaving flag and its phantom (TickTeamJoin) until the title or a team again.
bool left_team_in_play = false; // team_mutex
// Kaniel 2026-10-16: the team screen opened over the game from its System menu (TeamMenu), and
// when it closed: the game's own page under it gets a Circle a moment later (TeamMenuBackPressed).
std::atomic<bool> team_menu_open{false};
// The rig's team-worlds: the team screen opens its world list (TeamWorldListAsked).
std::atomic<bool> team_worlds_asked{false};
bool team_menu_from_page = false; // team_back_mutex
std::mutex team_back_mutex;
TeamMenuBack team_menu_back;
// What the summon packet carries before the join data (0x01873754): GameMan +0x16F0 and +0x16F4
// on the guest, the same for every co-operator (rig 2026-10-14: 0xFFFFFFFF and 7).
constexpr u32 TeamSessionKind = 0xFFFFFFFFu;
constexpr u32 TeamSessionSign = 7;
// Invasions (bloodborne_invasion.h): this game invading (the stream's thread hands it the
// server's messages, the game thread ticks it) or its team's world invaded; both team_mutex. A
// game in no team has no party hello, so its outbox never goes: while it invades, its map and
// vitals go without one (the server takes nothing else from an invader).
InvaderTrip invader_trip;
InvadedTeam invaded_team;
std::atomic<bool> invader_link{false};
// invader_trip.AwayFromHome(), for the hooks that ask many times a frame (Invading): only the game
// thread's tick changes it.
std::atomic<bool> invader_away{false};

std::string PartyKey() {
    if (team_switch) {
        std::scoped_lock lock{team_mutex};
        return team_party;
    }
    const char* setting = std::getenv("SHADPS4_BLOODBORNE_PARTY");
    return setting != nullptr ? ParsePartySetting(setting).value_or("") : "";
}

void QueueLobby(bbcoop::Envelope envelope) {
    std::scoped_lock lock{team_mutex};
    lobby_outbox.push_back(std::move(envelope));
}

// On the stream's thread, as they come.
std::filesystem::path TeamWorldHintFile(int slot) {
    return Common::FS::GetUserPath(Common::FS::PathType::UserDir) /
           SlotFileName("team_world", slot);
}

std::string ReadTeamWorldHint(int slot) {
    std::ifstream file{TeamWorldHintFile(slot), std::ios::binary};
    std::string id;
    std::getline(file, id);
    return id;
}

void WriteTeamWorldHint(int slot, const std::string& id) {
    std::ofstream file{TeamWorldHintFile(slot), std::ios::binary | std::ios::trunc};
    file << id << '\n';
}

// Kaniel 2026-10-19: each slot's character name, for the title's team screen
// (TitleLoadCharacterName), and the last one played, for the title before a save is picked.
std::filesystem::path CharacterNameFile(int slot) {
    return Common::FS::GetUserPath(Common::FS::PathType::UserDir) /
           (slot < 0 ? std::string{"bloodborne_party_character_last.txt"}
                     : SlotFileName("character", slot));
}

std::string ReadCharacterName(int slot) {
    std::ifstream file{CharacterNameFile(slot), std::ios::binary};
    std::string name;
    std::getline(file, name);
    return TitleLoadCharacterName(false, name);
}

void WriteCharacterName(int slot, const std::string& name) {
    std::ofstream file{CharacterNameFile(slot), std::ios::binary | std::ios::trunc};
    file << name << '\n';
}

// Kaniel 2026-10-25: the host rules this game plays by (the team's while in one, else its own),
// for the game thread's every frame without the team's lock.
std::atomic<u8> team_rules_bits{0};
std::atomic<bool> in_team{false};
// The team's rot for this player, as the server last had it (a game that came back takes it).
std::mutex kept_rot_mutex;
std::optional<std::vector<u32>> kept_rot;

u8 RulesBits(const HostRules& rules) {
    return static_cast<u8>((rules.rot ? 1 : 0) | (rules.travel_together ? 2 : 0) |
                           (rules.travel_vote && rules.travel_together ? 4 : 0) |
                           (rules.enemy_scaling ? 8 : 0));
}

HostRules RulesOf(u8 bits) {
    return {.rot = (bits & 1) != 0,
            .travel_together = (bits & 2) != 0,
            .travel_vote = (bits & 4) != 0,
            .enemy_scaling = (bits & 8) != 0};
}

void NoteTeamRules(const bbcoop::LobbyState& state, const std::string& me) {
    const bool team = state.has_team();
    const bbcoop::TeamRules& r = state.team().rules();
    const HostRules rules{.rot = r.rot(),
                          .travel_together = r.travel_together(),
                          .travel_vote = r.travel_vote(),
                          .enemy_scaling = r.enemy_scaling()};
    const u8 bits = RulesBits(rules);
    const bool was_in = in_team.exchange(team);
    if (team && (team_rules_bits.exchange(bits) != bits || !was_in)) {
        LOG_INFO(Debug, "Co-op host rules: the team's rules are {}", HostRulesText(rules));
    }
    if (team) {
        for (const bbcoop::TeamMember& m : state.team().members()) {
            if (m.member() == me) {
                std::scoped_lock lock{kept_rot_mutex};
                kept_rot.emplace(m.rot().begin(), m.rot().end());
            }
        }
    }
}

bool TakeLobbyMessage(const bbcoop::Envelope& envelope) {
    if (envelope.has_lobby_state()) {
        std::scoped_lock lock{team_mutex};
        team_gate.TakeLobby(envelope.lobby_state());
        NoteTeamRules(envelope.lobby_state(), lobby_member);
        if (!envelope.lobby_state().notice().empty()) {
            LOG_WARNING(Debug, "Co-op team lobby: the server refused: {}",
                        envelope.lobby_state().notice());
        }
        return true;
    }
    if (envelope.has_team_world()) {
        std::scoped_lock lock{team_mutex};
        team_world = envelope.team_world();
        if (!team_world->exists() && team_world_due) {
            team_world_taken = true; // nothing to take: the host's save makes it
        }
        LOG_INFO(Debug, "Co-op team world: the server's has {} ({} flags on, version {})",
                 team_world->exists() ? "a world" : "no world yet", team_world->on_size(),
                 team_world->version());
        return true;
    }
    if (envelope.has_load_progress()) {
        std::scoped_lock lock{team_mutex};
        team_load.Take(envelope.load_progress(), SteadyMilliseconds());
        return true;
    }
    if (envelope.has_resume_at()) {
        const bbcoop::ResumeAt& at = envelope.resume_at();
        LOG_INFO(Debug,
                 "Co-op team world: the server has this member last on map {:#x} at ({}, "
                 "{}, {})",
                 at.map(), at.at().x(), at.at().y(), at.at().z());
        std::scoped_lock lock{team_mutex};
        server_resume_point = ResumePoint{at.map(), {at.at().x(), at.at().y(), at.at().z()}, 0};
        return true;
    }
    if (envelope.has_team_session()) {
        const bbcoop::TeamSession& session = envelope.team_session();
        LOG_INFO(Debug, "Co-op team join: {}'s session {} {}", session.host(), session.session(),
                 session.join().empty() ? "closed"
                                        : fmt::format("is open ({} bytes)", session.join().size()));
        std::scoped_lock lock{team_mutex};
        if (team_session && !team_session->host().empty() && !session.host().empty() &&
            team_session->host() != session.host() && !session.join().empty()) {
            LOG_INFO(Debug, "Co-op team join: the session host moved to {}", session.host());
        }
        team_session = session;
        return true;
    }
    if (envelope.has_invasion_notice()) {
        const bbcoop::InvasionNotice& notice = envelope.invasion_notice();
        std::scoped_lock lock{team_mutex};
        const bool mine = notice.invader() == lobby_member || invader_trip.Active();
        LOG_INFO(Debug, "Co-op invasion {}: {} approaches {} in {} ms ({})", notice.id(),
                 notice.invader(), notice.target(), notice.in_ms(),
                 mine ? "this game invades" : "this team's world");
        if (mine) {
            invader_trip.TakeNotice(notice, SteadyMilliseconds());
        } else {
            invaded_team.TakeNotice(notice, SteadyMilliseconds());
        }
        return true;
    }
    if (envelope.has_invasion_start()) {
        const bbcoop::InvasionStart& start = envelope.invasion_start();
        std::scoped_lock lock{team_mutex};
        const bool taken = invader_trip.TakeStart(start, SteadyMilliseconds());
        LOG_INFO(Debug,
                 "Co-op invasion {}: start: {}'s session (kind {:#x}, sign {}), map {:#x} at ({}, "
                 "{}, {}){}",
                 start.id(), start.session().host(), start.session().kind(), start.session().sign(),
                 start.map(), start.at().x(), start.at().y(), start.at().z(),
                 taken ? "" : " (no invasion of this game waits: ignored)");
        if (taken && start.has_owners()) {
            // The target world's enemy owners: this game follows them there (EnemyRoleOn).
            std::vector<EnemyOwnerEntry> owners;
            for (const bbcoop::EnemyOwner& owner : start.owners().owners()) {
                owners.push_back({owner.map(), owner.member(), owner.epoch()});
            }
            std::scoped_lock owners_lock{owners_mutex};
            server_owners = std::move(owners);
            owners_heard_ms = SteadyMilliseconds();
        }
        return true;
    }
    if (envelope.has_invader_joined()) {
        const bbcoop::InvaderJoined& joined = envelope.invader_joined();
        LOG_INFO(Debug, "Co-op invasion {}: {} (\"{}\") joined, hunting {}", joined.id(),
                 joined.member(), joined.name(), joined.target());
        std::scoped_lock lock{team_mutex};
        invaded_team.TakeJoined(joined, SteadyMilliseconds());
        return true;
    }
    if (envelope.has_invasion_end()) {
        const bbcoop::InvasionEnd& end = envelope.invasion_end();
        LOG_INFO(Debug, "Co-op invasion {}: over ({}, {}){}", end.id(),
                 bbcoop::InvasionEnd::Reason_Name(end.reason()),
                 InvasionEndReasonName(end.reason()),
                 end.text().empty() ? "" : fmt::format(": {}", end.text()));
        std::scoped_lock lock{team_mutex};
        if (invader_trip.Active()) {
            invader_trip.TakeEnd(end, SteadyMilliseconds());
        } else {
            invaded_team.TakeEnd(end, SteadyMilliseconds());
        }
        return true;
    }
    if (envelope.has_team_start()) {
        const bbcoop::TeamStart& start = envelope.team_start();
        LOG_INFO(Debug, "Co-op team lobby: team {} starts (party {}, host {}, slot {}, world {})",
                 start.team(), start.party(), start.host(), start.slot(), start.world());
        if (!start.world().empty()) {
            WriteTeamWorldHint(static_cast<int>(start.slot()), start.world());
        }
        std::scoped_lock lock{team_mutex};
        team_gate.TakeStart(start);
        return true;
    }
    return false;
}

// The lobby's hello when due, and its requests: on the stream's thread, stream_mutex held.
void SendLobby(s64 now_ms) {
    std::scoped_lock lock{team_mutex};
    if (!team_switch || lobby_member.empty()) {
        return;
    }
    if (lobby_hello_due) {
        lobby_hello_due = false;
        bbcoop::Envelope hello;
        hello.mutable_lobby_hello()->set_protocol(CoopProtocol);
        hello.mutable_lobby_hello()->set_member(lobby_member);
        hello.mutable_lobby_hello()->set_name(lobby_name);
        hello.mutable_lobby_hello()->set_playing(lobby_playing);
        hello.mutable_lobby_hello()->set_invasion_rewards(true);
        hello.mutable_lobby_hello()->set_covenant_rewards(true);
        stream->Send(EncodeEnvelope(hello), now_ms);
        // A reconnect or character-name update must not let a member overwrite its host's rules.
        const auto& state = team_gate.Lobby();
        if (!state || !state->has_team() || state->team().leader() == lobby_member) {
            // Kaniel 2026-10-25: this launcher's host rules, the team's if this player leads it.
            const HostRules& own = OwnHostRules();
            bbcoop::Envelope rules;
            rules.mutable_team_request()->set_action(bbcoop::TeamRequest::RULES);
            bbcoop::TeamRules& r = *rules.mutable_team_request()->mutable_rules();
            r.set_rot(own.rot);
            r.set_travel_together(own.travel_together);
            r.set_travel_vote(own.travel_vote);
            r.set_enemy_scaling(own.enemy_scaling);
            stream->Send(EncodeEnvelope(rules), now_ms);
        }
    }
    for (const bbcoop::Envelope& envelope : lobby_outbox) {
        stream->Send(EncodeEnvelope(envelope), now_ms);
    }
    lobby_outbox.clear();
}

void Queue(bbcoop::Envelope envelope) {
    std::scoped_lock lock{stream_mutex};
    outbox.push_back(std::move(envelope));
}

// The boards (3c): taken on the stream's thread as they come (not on the kill step's).
bool IsBoard(const bbcoop::Envelope& envelope) {
    switch (envelope.body_case()) {
    case bbcoop::Envelope::kMapBoard:
    case bbcoop::Envelope::kRestBoard:
    case bbcoop::Envelope::kTrophies:
    case bbcoop::Envelope::kEchoes:
    case bbcoop::Envelope::kVitalsBoard:
    case bbcoop::Envelope::kPortrait:
    case bbcoop::Envelope::kEnemyOwners:
        return true;
    default:
        return false;
    }
}

LampPosition PositionOf(const bbcoop::Position& at) {
    return {at.x(), at.y(), at.z()};
}

void PutPosition(const LampPosition& at, bbcoop::Position* out) {
    out->set_x(at[0]);
    out->set_y(at[1]);
    out->set_z(at[2]);
}

void TakeBoard(const bbcoop::Envelope& envelope) {
    switch (envelope.body_case()) {
    case bbcoop::Envelope::kMapBoard: {
        std::vector<MemberMap> maps;
        for (const auto& entry : envelope.map_board().members()) {
            maps.push_back({entry.member(), entry.map()});
        }
        TakeServerMapBoard(std::move(maps));
        return;
    }
    case bbcoop::Envelope::kRestBoard: {
        std::vector<LampRest> rests;
        for (const auto& entry : envelope.rest_board().rests()) {
            rests.push_back(
                {entry.member(), entry.lamp(),
                 entry.has_at() ? std::optional{PositionOf(entry.at())} : std::nullopt});
        }
        TakeServerRestBoard(std::move(rests));
        return;
    }
    case bbcoop::Envelope::kTrophies: {
        const std::vector<s32> ids(envelope.trophies().ids().begin(),
                                   envelope.trophies().ids().end());
        ReadTrophyBoard(ids);
        return;
    }
    case bbcoop::Envelope::kEchoes: {
        std::vector<KillEchoes> echoes;
        for (const auto& kill : envelope.echoes().kills()) {
            echoes.push_back({kill.echoes(), kill.map()});
        }
        TakeServerEchoes(std::move(echoes));
        return;
    }
    case bbcoop::Envelope::kVitalsBoard: {
        VitalsBoard board;
        for (const auto& entry : envelope.vitals_board().members()) {
            const bbcoop::Vitals& vitals = entry.vitals();
            board.members.push_back(
                MemberVitals{.member = entry.member(),
                             .map = vitals.map(),
                             .vital = static_cast<Vital>(std::min<u32>(vitals.vital(), 4)),
                             .hp = vitals.hp(),
                             .max = vitals.max(),
                             .down = vitals.down(),
                             .landed = vitals.landed(),
                             .needed = vitals.needed(),
                             .at = PositionOf(vitals.at()),
                             .revive_damage = entry.revive_damage(),
                             .cutscene = vitals.cutscene(),
                             .vials = vitals.vials(),
                             .bullets = vitals.bullets(),
                             .level = vitals.level(),
                             .boss = vitals.boss(),
                             .killed_by_invader = vitals.killed_by_invader(),
                             .hostile = entry.hostile(),
                             .lamp_uses = vitals.lamp_uses(),
                             .bosses = {vitals.bosses().begin(), vitals.bosses().end()}});
        }
        for (const auto& wipe : envelope.vitals_board().wipes()) {
            board.wipes.push_back(
                {wipe.id(), wipe.map(), {wipe.members().begin(), wipe.members().end()}});
        }
        TakeServerVitals(std::move(board));
        return;
    }
    case bbcoop::Envelope::kPortrait: {
        const std::string& pixels = envelope.portrait().pixels();
        TakeServerPortrait(envelope.portrait().member(), envelope.portrait().id(),
                           std::vector<u8>(pixels.begin(), pixels.end()));
        return;
    }
    case bbcoop::Envelope::kEnemyOwners: {
        std::vector<EnemyOwnerEntry> owners;
        for (const bbcoop::EnemyOwner& owner : envelope.enemy_owners().owners()) {
            owners.push_back({owner.map(), owner.member(), owner.epoch()});
        }
        std::scoped_lock lock{owners_mutex};
        server_owners = std::move(owners);
        owners_heard_ms = SteadyMilliseconds();
        return;
    }
    default:
        return;
    }
}

void StreamTick(s64 now_ms) {
    std::vector<std::vector<u8>> received;
    std::vector<bbcoop::Envelope> boards;
    std::array<u8, 4096> buffer{};
    for (int count = 0; count < 512; ++count) {
        const int size =
            Libraries::Net::ServerCoopReceive(buffer.data(), static_cast<u32>(buffer.size()));
        if (size < 0) {
            break;
        }
        received.emplace_back(buffer.begin(), buffer.begin() + size);
    }
    std::vector<std::vector<u8>> out;
    bool restarted = false;
    {
        std::scoped_lock lock{stream_mutex};
        for (const std::vector<u8>& body : received) {
            for (const std::vector<u8>& message : stream->Receive(body, now_ms)) {
                if (auto envelope = DecodeEnvelope(message)) {
                    if (TakeLobbyMessage(*envelope)) {
                        continue;
                    }
                    if (envelope->has_enemy_hit()) {
                        const bbcoop::EnemyHit& hit = envelope->enemy_hit();
                        std::scoped_lock hits_lock{enemy_hits_mutex};
                        if (enemy_hits.size() < 256) {
                            enemy_hits.push_back(
                                {hit.map(), hit.handle(), hit.hp(), hit.max(), hit.member()});
                        }
                        inbox_news.store(true);
                        continue;
                    }
                    if (envelope->has_altar_record()) {
                        const bbcoop::AltarRecord& news = envelope->altar_record();
                        if (news.bits().size() == AltarRecordBytes) {
                            AltarRecordNews record{.altar = static_cast<s32>(news.altar()),
                                                   .member = news.member()};
                            std::copy(news.bits().begin(), news.bits().end(),
                                      record.record.begin());
                            std::scoped_lock records_lock{altar_records_mutex};
                            if (altar_records.size() < 16) {
                                altar_records.push_back(std::move(record));
                            }
                        }
                        continue;
                    }
                    if (envelope->has_team_travel()) {
                        TakeTeamTravelNews(envelope->team_travel());
                        continue;
                    }
                    if (envelope->has_ping()) {
                        std::scoped_lock pings_lock{team_pings_mutex};
                        if (team_pings.size() < 64) {
                            team_pings.push_back(PingOf(envelope->ping()));
                        }
                        continue;
                    }
                    if (envelope->has_duel_state()) {
                        std::scoped_lock duels_lock{duel_states_mutex};
                        if (duel_states.size() < 64) {
                            duel_states.push_back(envelope->duel_state());
                        }
                        continue;
                    }
                    if (IsTradeMessage(*envelope)) {
                        TakeServerTrade(std::move(*envelope));
                    } else if (IsBoard(*envelope)) {
                        boards.push_back(std::move(*envelope));
                    } else {
                        inbox.push_back(std::move(*envelope));
                        inbox_news.store(true);
                    }
                }
            }
        }
        if (stream->TakeRestarted()) {
            restarted = true;
            hello_due = true;
            std::scoped_lock team_lock{team_mutex};
            lobby_hello_due = true;
        }
        SendLobby(now_ms);
        if (hello_due && !hello_party.empty() && !hello_member.empty()) {
            hello_due = false;
            bbcoop::Envelope hello;
            hello.mutable_hello()->set_protocol(CoopProtocol);
            hello.mutable_hello()->set_party(hello_party);
            hello.mutable_hello()->set_member(hello_member);
            hello.mutable_hello()->set_run(run);
            hello.mutable_hello()->set_host(!IsPartyGuestRole());
            hello.mutable_hello()->set_enemy_owner(enemy_owner_switch);
            stream->Send(EncodeEnvelope(hello), now_ms);
        }
        if (!hello_due) {
            for (const bbcoop::Envelope& envelope : outbox) {
                stream->Send(EncodeEnvelope(envelope), now_ms);
            }
            outbox.clear();
        } else if (invader_link.load()) {
            // Only the newest of each: a game in no team queued them unsent until now.
            const auto newest = [&](auto has) {
                const auto found = std::find_if(outbox.rbegin(), outbox.rend(), has);
                if (found != outbox.rend()) {
                    stream->Send(EncodeEnvelope(*found), now_ms);
                }
            };
            newest([](const bbcoop::Envelope& envelope) { return envelope.has_map(); });
            newest([](const bbcoop::Envelope& envelope) { return envelope.has_vitals(); });
            outbox.clear();
        }
        out = stream->Flush(now_ms);
    }
    for (const bbcoop::Envelope& board : boards) {
        TakeBoard(board);
    }
    if (restarted) {
        LOG_WARNING(Debug, "Co-op server world: shadNet started over (or lost this game): "
                           "saying hello again");
        Trace("srvworld", {{"event", "restarted"}});
    }
    for (const std::vector<u8>& body : out) {
        Libraries::Net::ServerCoopSend(body.data(), static_cast<u32>(body.size()));
    }
}

void StartThread() {
    std::call_once(thread_started, [] {
        {
            std::scoped_lock lock{stream_mutex};
            stream.emplace(LocalOnlineId(), run);
        }
        std::thread{[] {
            while (true) {
                StreamTick(SteadyMilliseconds());
                std::this_thread::sleep_for(std::chrono::milliseconds{StreamTickMs});
            }
        }}.detach();
        LOG_INFO(Debug, "Co-op server world: on, shadNet keeps the party's world (run {:#x})", run);
    });
}

// This member's copy of the party's log (the game thread, and the copies others read).
std::mutex member_mutex;
WorldLogMember member;
std::atomic<bool> member_reset{false};
WorldLogKills last_proposed;
u32 proposals = 0;
s64 proposed_ms = 0;

void SendLifecycle(bbcoop::Lifecycle::Event event, u32 map = 0) {
    if (!server_world_switch) {
        return;
    }
    bbcoop::Envelope envelope;
    envelope.mutable_lifecycle()->set_event(event);
    envelope.mutable_lifecycle()->set_host(!IsPartyGuestRole());
    envelope.mutable_lifecycle()->set_map(map);
    Queue(std::move(envelope));
    LOG_INFO(Debug, "Co-op server world: told shadNet this member's {} ({}{})",
             bbcoop::Lifecycle::Event_Name(event), IsPartyGuestRole() ? "a guest" : "the host",
             map != 0 ? fmt::format(", its map {:#x} alone", map) : "");
}

// This member's session events, for the party's world: a wipe starts its log over once for the
// party; the host's join, leave or new character starts a new one, a guest's takes its share out.
// A new character out of a session is no event of the party's.
[[maybe_unused]] const bool wipe_registered = RegisterLifecycleReset(
    "server world (wipe)", {Lifecycle::Wipe}, [] { SendLifecycle(bbcoop::Lifecycle::WIPE); });
[[maybe_unused]] const bool join_registered =
    RegisterLifecycleReset("server world (join)", {Lifecycle::Join}, [] {
        member_reset.store(true);
        SendLifecycle(bbcoop::Lifecycle::JOIN);
    });
// Kaniel's play test 2026-10-19: a guest's join reloads its world, and the reload gives its game
// its own saved world back in every block its title load had taken the team's in. krolebord's
// own save had the Cathedral Ward gate closed (2010/2020 off), so the gate the team had opened
// was shut again. The join's reload takes the team's world again in every block.
[[maybe_unused]] const bool team_world_join_registered =
    RegisterLifecycleReset("team world (join)", {Lifecycle::Join}, [] {
        if (!IsPartyGuestRole()) {
            return;
        }
        std::scoped_lock lock{team_mutex};
        if (team_host || team_world_blocks.empty()) {
            return;
        }
        LOG_INFO(Debug,
                 "Co-op team world: joined; the join's reload takes the team's world again in "
                 "{} blocks",
                 team_world_blocks.size());
        team_world_blocks.clear();
    });
[[maybe_unused]] const bool leave_registered =
    RegisterLifecycleReset("server world (leave)", {Lifecycle::Leave}, [] {
        member_reset.store(true);
        SendLifecycle(bbcoop::Lifecycle::LEAVE);
    });
[[maybe_unused]] const bool new_character_registered =
    RegisterLifecycleReset("server world (new character)", {Lifecycle::NewCharacter}, [] {
        member_reset.store(true);
        if (InPartySession()) {
            SendLifecycle(bbcoop::Lifecycle::NEW_CHARACTER);
        }
    });

std::string EntriesText(std::span<const WorldLogEntry> entries) {
    std::string text;
    for (size_t index = 0; index < entries.size() && index < 8; ++index) {
        text += (text.empty() ? "" : ", ") + FormatWorldLogEntry(entries[index]);
    }
    return entries.size() > 8 ? fmt::format("{}, ... ({} in all)", text, entries.size()) : text;
}

void LogStep(const WorldLogMember::Step& step, std::string_view what) {
    if (step.new_epoch) {
        LOG_INFO(Debug, "Co-op server world: following the party's log {:#x}", member.Epoch());
    }
    if (step.snapshot) {
        LOG_INFO(Debug, "Co-op server world: took the party's world to seq {} ({} kills)",
                 member.Applied(), WorldLogKillCount(member.Kills()));
    }
    if (!step.applied.empty()) {
        LOG_INFO(Debug, "Co-op server world: applied {}", EntriesText(step.applied));
    }
    if (step.stale || step.new_epoch || step.snapshot || step.gap || !step.applied.empty()) {
        Trace("srvworld", {{"event", std::string{what}},
                           {"epoch", fmt::format("{:#x}", member.Epoch())},
                           {"seq", std::to_string(member.Applied())},
                           {"applied", std::to_string(step.applied.size())},
                           {"gap", step.gap ? "1" : "0"},
                           {"stale", step.stale ? "1" : "0"}});
    }
}

struct Taken {
    bool after_wipe = false; // the party's log started over after a wipe in it
    std::vector<WorldLogEntry> applied;
};

// What came from the server, on the game thread (member_mutex held).
Taken TakeInbox(s64 now_ms) {
    std::deque<bbcoop::Envelope> taken;
    {
        std::scoped_lock lock{stream_mutex};
        taken.swap(inbox);
    }
    Taken result;
    const auto took = [&](const WorldLogMember::Step& step, std::string_view what) {
        result.after_wipe = result.after_wipe || step.after_wipe || StartsAfterAWipe(step.applied);
        result.applied.insert(result.applied.end(), step.applied.begin(), step.applied.end());
        LogStep(step, what);
    };
    for (const bbcoop::Envelope& envelope : taken) {
        if (envelope.has_welcome()) {
            const bbcoop::Welcome& welcome = envelope.welcome();
            TakeServerTradeWelcome(welcome.accepted() && welcome.trades());
            if (welcome.accepted()) {
                LOG_INFO(Debug,
                         "Co-op server world: shadNet took this member (log {:#x} at seq {})",
                         welcome.epoch(), welcome.last());
            } else {
                LOG_ERROR(Debug, "Co-op server world: shadNet refused this member: {}",
                          welcome.text());
            }
            Trace("srvworld", {{"event", welcome.accepted() ? "welcome" : "refused"},
                               {"text", welcome.text()}});
        } else if (envelope.has_entries()) {
            took(member.Take(BatchOf(envelope.entries()), now_ms), "entries");
        } else if (envelope.has_snapshot()) {
            const SnapshotParts parts = PartsOf(envelope.snapshot());
            member.Take(parts.kills, now_ms);
            member.Take(parts.shown, now_ms);
            member.Take(parts.map_state, now_ms);
            member.Take(parts.hp, now_ms);
            took(member.Take(parts.flags, now_ms), "snapshot");
        }
    }
    return result;
}

// The host's last reports, sent again on a change or after ReportResendMs.
template <typename T>
struct LastReport {
    std::optional<T> value;
    s64 sent_ms = 0;
    bool Due(const T& now, s64 now_ms) {
        if (value == now && now_ms - sent_ms < ReportResendMs) {
            return false;
        }
        value = now;
        sent_ms = now_ms;
        return true;
    }
};
std::mutex reports_mutex;
LastReport<std::pair<u32, std::vector<CharacterShown>>> last_shown;
LastReport<std::pair<u32, std::vector<u8>>> last_map_state;
LastReport<WorldLogReturnPoint> last_return_point;
WorldLogFlagOutbox flag_outbox;
s64 flags_sent_ms = 0;
u32 hp_proposals = 0;
s64 hp_sent_ms = 0;

// The member's flag events (any thread queues them, the kill step sends them; member_mutex held).
void SendFlags(s64 now_ms) {
    std::vector<WorldLogFlagEvent> events;
    {
        std::scoped_lock lock{reports_mutex};
        if (!flag_outbox.TakeNew() && now_ms - flags_sent_ms < WorldLogResendMs) {
            return;
        }
        events = flag_outbox.Pending(now_ms);
        if (events.empty()) {
            return;
        }
        flags_sent_ms = now_ms;
    }
    bbcoop::Envelope envelope;
    *envelope.mutable_flags() =
        ToMessage(WorldLogFlagProposal{{}, member.Epoch(), run, std::move(events)});
    Queue(std::move(envelope));
}

} // namespace

namespace Coop {

bool ServerWorldOn() {
    return server_world_switch;
}

bool EnemyOwnerOn() {
    return enemy_owner_switch;
}

void TellServerMapWipe(u32 map) {
    SendLifecycle(bbcoop::Lifecycle::WIPE, map);
}

bool TakeServerWorldNews() {
    return enemy_owner_switch && inbox_news.exchange(false);
}

std::vector<EnemyOwnerEntry> ServerEnemyOwners(s64 now_ms) {
    // Invasions: an invader hears the target world's owners only as they change (a member hears
    // them with every answer to its map): its last list stands for the visit.
    const bool invading = Invading();
    std::scoped_lock lock{owners_mutex};
    if (owners_heard_ms == 0 || (now_ms - owners_heard_ms > EnemyOwnersFreshMs && !invading)) {
        return {};
    }
    return server_owners;
}

void SendEnemyHit(u32 map, u32 handle, s32 hp, s32 max) {
    if (!ServerWorldOn() || !enemy_owner_switch) {
        return;
    }
    bbcoop::Envelope envelope;
    bbcoop::EnemyHit& hit = *envelope.mutable_enemy_hit();
    hit.set_map(map);
    hit.set_handle(handle);
    hit.set_hp(hp);
    hit.set_max(max);
    Queue(std::move(envelope));
}

void SendTeamPing(const TeamPing& ping) {
    if (!ServerWorldOn()) {
        return;
    }
    bbcoop::Envelope envelope;
    *envelope.mutable_ping() = ToMessage(ping);
    Queue(std::move(envelope));
}

void SendTeamTravel(const bbcoop::TeamTravel& travel) {
    if (!ServerWorldOn()) {
        return;
    }
    bbcoop::Envelope envelope;
    *envelope.mutable_team_travel() = travel;
    Queue(std::move(envelope));
}

std::vector<TeamPing> TakeTeamPings() {
    std::scoped_lock lock{team_pings_mutex};
    return std::exchange(team_pings, {});
}

void SendDuelRequest(bbcoop::DuelRequest::Action action, u32 duel, u32 map) {
    if (!ServerWorldOn()) {
        return;
    }
    bbcoop::Envelope envelope;
    envelope.mutable_duel_request()->set_action(action);
    envelope.mutable_duel_request()->set_duel(duel);
    envelope.mutable_duel_request()->set_map(map);
    Queue(std::move(envelope));
}

std::vector<bbcoop::DuelState> TakeDuelStates() {
    std::scoped_lock lock{duel_states_mutex};
    return std::exchange(duel_states, {});
}

std::vector<EnemyHitNews> TakeEnemyHits() {
    std::scoped_lock lock{enemy_hits_mutex};
    return std::exchange(enemy_hits, {});
}

void SendAltarRecord(s32 altar, const AltarRecord& record) {
    if (!ServerWorldOn()) {
        return;
    }
    bbcoop::Envelope envelope;
    bbcoop::AltarRecord& message = *envelope.mutable_altar_record();
    message.set_altar(static_cast<u32>(altar));
    message.set_bits(std::string(record.begin(), record.end()));
    Queue(std::move(envelope));
}

std::vector<AltarRecordNews> TakeAltarRecords() {
    std::scoped_lock lock{altar_records_mutex};
    return std::exchange(altar_records, {});
}

void RequestServerHello() {
    std::scoped_lock lock{stream_mutex};
    hello_due = true;
}

// Phase 11: the character's name, as the others see this player: to the lobby when it changes.
void NoteCharacterName(s64 now_ms) {
    {
        std::scoped_lock lock{team_mutex};
        if (!team_switch || now_ms - lobby_name_read_ms < 2'000) {
            return;
        }
        lobby_name_read_ms = now_ms;
        if (!CharacterNameReadable(title_load_ms, save_opened_ms.load(), now_ms)) {
            return; // the game still holds the last character's name
        }
    }
    std::string name = OwnCharacterName();
    if (name.empty()) {
        return;
    }
    std::scoped_lock lock{team_mutex};
    if (name != lobby_name) {
        LOG_INFO(Debug, "Co-op team lobby: this character is \"{}\"", name);
        if (const int slot = character_slot.load(); slot >= 0) {
            WriteCharacterName(slot, name);
        }
        WriteCharacterName(-1, name);
        lobby_name = std::move(name);
        lobby_hello_due = true;
    }
}

ServerWorldKills TickServerWorld(const WorldLogKills& mine, bool propose, s64 now_ms) {
    StartThread();
    std::scoped_lock member_lock{member_mutex};
    {
        std::scoped_lock lock{stream_mutex};
        const std::string party = PartyKey();
        if (party != hello_party && !hello_party.empty()) {
            hello_due = true; // another team's world (phase 5): a new hello
        }
        hello_party = party;
        std::scoped_lock vitals{vitals_mutex};
        if (!vitals_member.empty()) {
            hello_member = vitals_member;
        }
    }
    if (member_reset.exchange(false)) {
        member.Reset();
        last_proposed.clear();
        proposed_ms = 0;
    }
    TickTeamWorld(now_ms);
    FollowTeamTimeOfDay(now_ms);
    FollowTeamStory(now_ms);
    NoteCharacterName(now_ms);
    ServerWorldKills world;
    Taken taken = TakeInbox(now_ms);
    world.host_wiped = taken.after_wipe;
    world.applied = std::move(taken.applied);
    if (propose) {
        SendFlags(now_ms);
    }
    if (propose && (mine != last_proposed || now_ms - proposed_ms >= KillsResendMs)) {
        last_proposed = mine;
        proposed_ms = now_ms;
        bbcoop::Envelope envelope;
        *envelope.mutable_kills() =
            ToMessage(WorldLogProposal{{}, member.Epoch(), run, ++proposals, mine});
        Queue(std::move(envelope));
    }
    if (member.SnapshotDue(now_ms)) {
        bbcoop::Envelope envelope;
        envelope.mutable_snapshot_request()->set_epoch(member.Epoch());
        envelope.mutable_snapshot_request()->set_seq(member.Applied());
        Queue(std::move(envelope));
        LOG_INFO(Debug,
                 "Co-op server world: asked shadNet for the party's world (log {:#x}, "
                 "seq {})",
                 member.Epoch(), member.Applied());
    }
    world.kills = WorldLogHandles(member.Kills());
    world.fresh = member.Fresh(now_ms);
    world.return_point = member.HostReturnPoint();
    return world;
}

void ShowToServer(u32 map, std::span<const CharacterShown> characters) {
    const s64 now_ms = SteadyMilliseconds();
    {
        std::scoped_lock lock{reports_mutex};
        if (!last_shown.Due({map, {characters.begin(), characters.end()}}, now_ms)) {
            return;
        }
    }
    bbcoop::Envelope envelope;
    *envelope.mutable_shown() = ToMessage(map, characters);
    Queue(std::move(envelope));
}

void MapStateToServer(u32 map, std::span<const u8> bits) {
    const s64 now_ms = SteadyMilliseconds();
    {
        std::scoped_lock lock{reports_mutex};
        if (!last_map_state.Due({map, {bits.begin(), bits.end()}}, now_ms)) {
            return;
        }
    }
    bbcoop::Envelope envelope;
    envelope.mutable_map_state()->set_map(map);
    envelope.mutable_map_state()->set_bits(std::string(bits.begin(), bits.end()));
    Queue(std::move(envelope));
}

void ReturnPointToServer(s32 point, u32 map) {
    if (point <= 0) {
        return; // a load: the last one stands
    }
    {
        std::scoped_lock lock{reports_mutex};
        if (!last_return_point.Due({point, map}, SteadyMilliseconds())) {
            return;
        }
    }
    bbcoop::Envelope envelope;
    envelope.mutable_return_point()->set_point(point);
    envelope.mutable_return_point()->set_map(map);
    Queue(std::move(envelope));
}

void FlagsToServer(std::span<const std::pair<s32, bool>> shared) {
    std::scoped_lock lock{reports_mutex};
    flag_outbox.Add(shared, SteadyMilliseconds());
}

void HpToServer(u32 map, const EnemyHpReport& report, bool changed) {
    const s64 now_ms = SteadyMilliseconds();
    u64 epoch = 0;
    {
        std::scoped_lock lock{member_mutex};
        epoch = member.Epoch();
    }
    {
        std::scoped_lock lock{reports_mutex};
        if (!changed && now_ms - hp_sent_ms < WorldLogResendMs) {
            return;
        }
        hp_sent_ms = now_ms;
    }
    bbcoop::Envelope envelope;
    *envelope.mutable_hp() =
        ToMessage(WorldLogHpProposal{{}, epoch, run, ++hp_proposals, map, report});
    Queue(std::move(envelope));
}

std::optional<WorldLogShown> ServerShown() {
    std::scoped_lock lock{member_mutex};
    if (!member.ShownKnown()) {
        return std::nullopt;
    }
    return member.Shown();
}

std::optional<WorldLogMapState> ServerMapState() {
    std::scoped_lock lock{member_mutex};
    if (!member.MapStateKnown()) {
        return std::nullopt;
    }
    return member.MapState();
}

WorldLogHp ServerHp() {
    std::scoped_lock lock{member_mutex};
    return member.Hp();
}

void QueueServerWorld(bbcoop::Envelope envelope) {
    Queue(std::move(envelope));
}

void StartServerWorld() {
    if (server_world_switch) {
        StartThread();
    }
}

void SendServerMap(u32 map) {
    bbcoop::Envelope envelope;
    envelope.mutable_map()->set_map(map);
    Queue(std::move(envelope));
}

void SendServerRest(s32 lamp, const std::optional<LampPosition>& at) {
    bbcoop::Envelope envelope;
    envelope.mutable_rest()->set_lamp(lamp);
    if (at) {
        PutPosition(*at, envelope.mutable_rest()->mutable_at());
    }
    Queue(std::move(envelope));
}

void SendServerTrophies(std::span<const s32> trophies) {
    bbcoop::Envelope envelope;
    for (const s32 trophy : trophies) {
        envelope.mutable_trophies()->add_ids(trophy);
    }
    Queue(std::move(envelope));
}

void SendServerEchoes(std::span<const KillEchoes> echoes) {
    bbcoop::Envelope envelope;
    for (const KillEchoes& kill : echoes) {
        bbcoop::EchoKill* out = envelope.mutable_echoes()->add_kills();
        out->set_echoes(kill.amount);
        out->set_map(kill.map);
    }
    Queue(std::move(envelope));
}

void SendServerVitals(const MemberVitals& mine, std::span<const ReviveHitTotal> hits) {
    bbcoop::Envelope envelope;
    bbcoop::Vitals& vitals = *envelope.mutable_vitals();
    vitals.set_map(mine.map);
    vitals.set_vital(static_cast<u32>(mine.vital));
    vitals.set_hp(mine.hp);
    vitals.set_max(mine.max);
    vitals.set_down(mine.down);
    vitals.set_landed(mine.landed);
    vitals.set_needed(mine.needed);
    vitals.set_cutscene(mine.cutscene);
    vitals.set_vials(mine.vials);
    vitals.set_bullets(mine.bullets);
    vitals.set_level(mine.level);
    vitals.set_boss(mine.boss);
    vitals.set_killed_by_invader(mine.killed_by_invader);
    vitals.set_covenant_effect(OwnCovenantEffect());
    vitals.set_lamp_uses(mine.lamp_uses);
    for (const u32 boss : mine.bosses) {
        vitals.add_bosses(boss);
    }
    PutPosition(mine.at, vitals.mutable_at());
    for (const ReviveHitTotal& hit : hits) {
        bbcoop::ReviveHit* out = vitals.add_hits();
        out->set_target(hit.target);
        out->set_down(hit.down);
        out->set_damage(hit.damage);
    }
    Queue(std::move(envelope));
}

void SendServerPortrait(u32 id, std::span<const u8> pixels) {
    bbcoop::Envelope envelope;
    envelope.mutable_portrait()->set_id(id);
    envelope.mutable_portrait()->set_pixels(std::string(pixels.begin(), pixels.end()));
    Queue(std::move(envelope));
}

bool TeamModeOn() {
    return team_switch;
}

void StartTeamLobby() {
    if (!team_switch) {
        return;
    }
    std::thread{[] {
        // The online id comes once the game signed in (a few seconds into the start).
        std::string online_id;
        for (int attempt = 0; attempt < 600 && online_id.empty(); ++attempt) {
            online_id = LocalOnlineId();
            if (online_id.empty()) {
                std::this_thread::sleep_for(std::chrono::milliseconds{500});
            }
        }
        if (online_id.empty()) {
            LOG_ERROR(Debug, "Co-op team lobby: no online id: not signed in to the lobby");
            return;
        }
        const std::string member = BoardMemberOfOnlineId(online_id);
        const std::string last_name = ReadCharacterName(-1);
        {
            std::scoped_lock lock{team_mutex};
            lobby_member = member;
            if (lobby_name.empty()) {
                lobby_name = last_name; // the title's lobby names the last character played
            }
            lobby_hello_due = true;
        }
        StartThread();
        LOG_INFO(Debug, "Co-op team lobby: signing in as {} ({})", online_id, member);
    }}.detach();
}

void NoteSaveOpenedForName() {
    save_opened_ms.store(SteadyMilliseconds());
}

bool HoldTitleLoadForTeam(u32 slot, bool fresh) {
    if (!team_switch) {
        return false;
    }
    TeamGate::Decision decision;
    std::string me;
    bool new_party = false;
    bool hold_for_world = false;
    {
        std::scoped_lock lock{team_mutex};
        me = lobby_member;
        const bool first = !team_gate.Holding();
        decision = team_gate.Tick(me, slot, SteadyMilliseconds());
        if (first) {
            ForgetStoryRemovals();
            title_fresh = fresh;
            team_world_hint = fresh ? std::string{} : ReadTeamWorldHint(static_cast<int>(slot));
            team_world_default_sent = false;
            // The team screen names this save's character, not the last one played (never a
            // stale name in the world's hunters list either).
            lobby_name = fresh ? std::string{} : ReadCharacterName(static_cast<int>(slot));
            lobby_name_read_ms = 0;
            title_load_ms = SteadyMilliseconds();
            lobby_hello_due = true;
            LOG_INFO(Debug,
                     "Co-op team lobby: the title loads slot {}{}: the team screen decides "
                     "({}; its team world {})",
                     slot, fresh ? " (a new character)" : "",
                     !team_gate.Lobby()              ? "no word from the lobby yet"
                     : team_gate.Lobby()->has_team() ? "in a team"
                                                     : "not in a team",
                     team_world_hint.empty() ? "none" : team_world_hint);
        }
        // Kaniel 2026-10-16: the leader's screen starts on this save's own world (a new one for
        // a new character); the leader may pick another. Kaniel 2026-10-17: so does the screen of
        // a player in no team yet (the team it makes takes the pick).
        if (decision.step == TeamGate::Step::Hold && !team_world_default_sent) {
            const auto& lobby = team_gate.Lobby();
            if (lobby && (!lobby->has_team() || lobby->team().leader() == me)) {
                team_world_default_sent = true;
                const auto pick = TeamScreenWorldPick(*lobby, me, team_world_hint, title_fresh);
                if (pick) {
                    bbcoop::Envelope envelope;
                    envelope.mutable_team_request()->set_action(bbcoop::TeamRequest::WORLD);
                    envelope.mutable_team_request()->set_world(*pick);
                    lobby_outbox.push_back(std::move(envelope));
                    LOG_INFO(Debug, "Co-op team lobby: the team screen starts on {}",
                             pick->empty() ? "a new world" : "world " + *pick);
                }
            }
        }
        if (decision.ready) {
            LOG_INFO(Debug, "Co-op team lobby: the team screen says slot {} is ready", slot);
            for (bbcoop::TeamRequest& request : team_gate.Choose(TeamGate::Choice::Ready, me)) {
                bbcoop::Envelope envelope;
                *envelope.mutable_team_request() = std::move(request);
                lobby_outbox.push_back(std::move(envelope));
            }
        }
        if (decision.step == TeamGate::Step::Hold) {
            return true;
        }
        if (decision.start && team_party != decision.start->party()) {
            // The team's party from its Start: the party's hello goes now, and the load waits
            // for the team's world (TeamWorldWaitMs at most) so the save takes it as it loads.
            team_party = decision.start->party();
            team_world.reset();
            team_session.reset();
            team_world_wait_ms = SteadyMilliseconds();
            new_party = true;
        }
        hold_for_world = decision.start && !team_world &&
                         SteadyMilliseconds() - team_world_wait_ms < TeamWorldWaitMs;
    }
    if (new_party) {
        const bool host = decision.start->host() == me;
        if (party_controller) {
            party_controller->Assign(host ? PartyRole::Host : PartyRole::Guest); // before the hello
        }
        std::scoped_lock lock{stream_mutex};
        if (hello_party != decision.start->party()) {
            hello_party = decision.start->party();
            hello_due = true;
        }
        if (hello_member.empty()) {
            hello_member = me; // the board name (the kill step sets it in the world)
        }
    }
    if (hold_for_world) {
        return true;
    }
    {
        std::scoped_lock lock{team_mutex};
        if (!team_world && decision.start) {
            LOG_WARNING(Debug,
                        "Co-op team world: no word of the team's world in {} ms: the save "
                        "loads without it",
                        TeamWorldWaitMs);
        }
        lobby_playing = true;
        lobby_hello_due = true;
        // A save alone after a team's plays no team's world.
        if (!decision.start) {
            team_party.clear();
            team_world.reset();
        }
        team_world_due = decision.start.has_value();
        team_world_taken = false;
        team_world_blocks.clear();
        team_host = decision.start && decision.start->host() == me;
        personal_world_due = !decision.start.has_value();
        team_world_reported = {};
        team_world_reported_ms = 0;
    }
    if (decision.start && party_controller) {
        const bool host = decision.start->host() == me;
        party_controller->Assign(host ? PartyRole::Host : PartyRole::Guest);
        {
            std::scoped_lock lock{team_mutex};
            team_load.Start(SteadyMilliseconds());
        }
        team_resume_due.store(!host && !fresh);
        LOG_INFO(Debug, "Co-op team lobby: the save plays in team {} as its {}",
                 decision.start->team(), host ? "host" : "guest");
    } else {
        team_resume_due.store(false);
        if (party_controller) {
            party_controller->Wait();
        }
        LOG_INFO(Debug, "Co-op team lobby: the save plays alone");
    }
    return false;
}

TeamLoad::View TeamLoadView() {
    std::scoped_lock lock{team_load_view_mutex};
    return team_load_view;
}

bool TeamLoadCovers() {
    return team_load_covers.load(std::memory_order_relaxed);
}

bool CutsceneSkipPressed() {
    static std::mutex skip_mutex;
    std::scoped_lock lock{skip_mutex};
    const bool press = cutscene_skip.Press(
        team_load_covers.load(std::memory_order_relaxed), remo_step.load(std::memory_order_relaxed),
        remo_step_ms.load(std::memory_order_relaxed), SteadyMilliseconds());
    if (press && !skip_pressed.exchange(true, std::memory_order_relaxed)) {
        LOG_INFO(Debug,
                 "Co-op team load: a cutscene plays under the screen: Options pressed to skip it "
                 "(press {})",
                 cutscene_skip.Presses());
    } else if (!press) {
        skip_pressed.store(false, std::memory_order_relaxed);
    }
    return press;
}

CutsceneVote::Pad CutsceneVotePad(bool options) {
    const s64 now_ms = SteadyMilliseconds();
    const int step = remo_step.load(std::memory_order_relaxed);
    const bool playing =
        (step == CutsceneSkip::PrologueStep || step == CutsceneSkip::PlayStep) &&
        now_ms - remo_step_ms.load(std::memory_order_relaxed) <= CutsceneSkip::LongestMs;
    // Under the team's loading screen CutsceneSkip skips it; out of a session it is the player's.
    const bool watching =
        playing && !team_load_covers.load(std::memory_order_relaxed) && InPartySession();
    CutsceneVote::Frame frame{.watching = watching, .options = options, .now_ms = now_ms};
    const u32 map = party_map_now.load();
    if (watching && map != 0) {
        std::scoped_lock lock{vitals_mutex};
        if (vitals_heard_at &&
            std::chrono::steady_clock::now() - *vitals_heard_at < CutsceneVoteFresh) {
            for (const MemberVitals& other : vitals_board.members) {
                if (other.member != vitals_member && other.map == map &&
                    other.cutscene != CutsceneVote::NotWatching) {
                    ++frame.others_watching;
                    frame.others_skip += other.cutscene == CutsceneVote::WantsSkip ? 1 : 0;
                }
            }
        }
    }
    std::scoped_lock lock{cutscene_vote_mutex};
    if (!watching) {
        cutscene_watch_since_ms = 0;
    } else if (cutscene_watch_since_ms == 0) {
        cutscene_watch_since_ms = now_ms;
    }
    if (const s64 after = cutscene_options_after.load();
        after >= 0 && watching && now_ms - cutscene_watch_since_ms >= after) {
        frame.options = true;
        cutscene_options_after.store(-1);
        LOG_INFO(Debug, "Co-op cutscene vote: the script presses Options");
    }
    const u32 was = cutscene_vote.Own();
    const u32 watchers_before = cutscene_vote.Watchers();
    const u64 shown_before = cutscene_vote_shown.load();
    const CutsceneVote::Pad pad = cutscene_vote.Tick(frame);
    NoteCutsceneVote(cutscene_vote.Own());
    const u64 shown =
        cutscene_vote.Shown() ? (u64{cutscene_vote.Want()} << 32) | cutscene_vote.Watchers() : 0;
    cutscene_vote_shown.store(shown);
    if (cutscene_vote.Own() != was || shown != shown_before ||
        cutscene_vote.Watchers() != watchers_before) {
        LOG_INFO(Debug, "Co-op cutscene vote: this member {}, {} of {} watching want to skip",
                 cutscene_vote.Own() == CutsceneVote::WantsSkip  ? "wants to skip"
                 : cutscene_vote.Own() == CutsceneVote::Watching ? "watches"
                                                                 : "is out of a cutscene",
                 cutscene_vote.Want(), cutscene_vote.Watchers());
    }
    return pad;
}

void ArmCutsceneOptions(s64 after_ms) {
    cutscene_options_after.store(std::max<s64>(after_ms, 0));
}

bool CutsceneVoteView(u32& want, u32& watchers) {
    const u64 shown = cutscene_vote_shown.load();
    want = static_cast<u32>(shown >> 32);
    watchers = static_cast<u32>(shown);
    return shown != 0;
}

void PS4_SYSV_ABI NoteRemoStep(u64 step, const GuestRegisterSnapshot*) {
    static constexpr std::array<const char*, 7> Steps{
        "Init", "RegistWait", "Prologue", "PrologueWait", "Update", "Epilog", "EpilogWait"};
    const int now_step = static_cast<int>(step);
    if (remo_step.load(std::memory_order_relaxed) != now_step) {
        remo_step_ms.store(SteadyMilliseconds(), std::memory_order_relaxed);
        remo_step.store(now_step, std::memory_order_relaxed);
        LOG_INFO(Debug, "Co-op cutscene: the cutscene player's step is {}",
                 Steps[step % Steps.size()]);
    }
}

// The game's thread (TickTeamJoin): this game's stage of the team's load, said to the others, and
// the screen as it stands. The present thread only reads the view (it read the game's memory
// itself once and crashed: rig 2026-10-14).
void UpdateTeamLoad() {
    const s64 now_ms = SteadyMilliseconds();
    const u64 global = GetGlobalState();
    const bool loading = global < 0x10000 || TryRead<u8>(global, 0x08).value_or(1) != 0;
    const u64 manager = GetMatchingState();
    const s32 matching = manager >= 0x10000 && HasMemoryAccess(manager, 0x128, MemoryProt::CpuRead)
                             ? ReadValue<s32>(manager, 0x124)
                             : 0;
    // The host is in once its session is open, a guest once it is in the session and beside its
    // host (play test 2026-10-18: the screen opened on a guest back in the Dream, its host in
    // Central Yharnam).
    const bool in = IsPartyGuestRole() ? InPartySession() && !TeamResumeDue()
                                       : matching == MatchingStateHosting;
    // Kaniel 2026-10-15: ready once every other hunter's character stands in this world: spawned
    // (a body somewhere) and shown (one on another map is hidden, ReconcileMemberPresence). A team
    // guest joins in place, with no reload to wait for (InSessionWorld waited its 30 s for one).
    size_t seen = 0;
    if (!loading && in) {
        for (const OtherPlayer& other : OtherPlayers()) {
            seen += other.at.has_value() && !IsCharacterDisabled(other.character) ? 1 : 0;
        }
    }
    // Release thread 2026-10-16: members the server's map board has on another map are not waited
    // for (a host off to the Dream before it saw the guest kept the guest covered until it came
    // back).
    std::vector<std::pair<std::string, u32>> maps;
    {
        std::scoped_lock lock{map_board_mutex};
        if (std::chrono::steady_clock::now() - map_board_time < std::chrono::seconds{10}) {
            for (const MemberMap& entry : map_board) {
                maps.emplace_back(entry.member, entry.map);
            }
        }
    }
    const u32 map = CurrentPlace().map;
    std::optional<bbcoop::LoadProgress> said;
    TeamLoad::View view;
    size_t needed = 0;
    TeamLoad::Stage stage = bbcoop::LoadProgress::LOADING;
    {
        std::scoped_lock lock{team_mutex};
        const auto& lobby = team_gate.Lobby();
        const auto members = lobby ? TeamLoadMembers(*lobby, lobby_member)
                                   : std::vector<std::pair<std::string, std::string>>{};
        needed = TeamLoad::OthersHere(members, lobby_member, maps, map);
        // A cutscene playing (SprjRemoMan's RegistWait .. Update): not ready, so it is skipped
        // under the screen.
        const int step = remo_step.load(std::memory_order_relaxed);
        const bool cutscene =
            step >= 1 && step <= CutsceneSkip::PlayStep &&
            now_ms - remo_step_ms.load(std::memory_order_relaxed) < CutsceneSkip::LongestMs;
        const TeamLoad::Stage own_stage = TeamLoad::OwnStage(loading, in, seen, needed);
        stage = team_load_settle.Settle(own_stage, needed, now_ms, cutscene);
        static TeamLoad::Stage logged_own = bbcoop::LoadProgress::WAITING;
        if (own_stage != logged_own) {
            logged_own = own_stage;
            LOG_INFO(Debug,
                     "Co-op team load: in the session {}, {} of {} others seen, cutscene {}: {}",
                     in, seen, needed, cutscene,
                     own_stage == bbcoop::LoadProgress::READY ? "ready (settling)" : "not ready");
        }
        team_load.SetStage(stage, now_ms);
        said = team_load.ToSend(now_ms);
        view = team_load.Tick(members, lobby_member, now_ms);
    }
    if (said) {
        bbcoop::Envelope envelope;
        *envelope.mutable_load_progress() = *said;
        Queue(std::move(envelope));
    }
    // Said while the screen is up, and the stage that opens it (in the tick it goes).
    static bool shown = false;
    static TeamLoad::Stage said_stage = bbcoop::LoadProgress::WAITING;
    if ((view.up || shown) && stage != said_stage) {
        said_stage = stage;
        static constexpr std::array<const char*, 4> Stages{"waiting", "loading", "joining",
                                                           "ready"};
        LOG_INFO(Debug,
                 "Co-op team load: this game is {}, {} of {} other hunters' characters in its "
                 "world",
                 Stages[static_cast<size_t>(stage) % Stages.size()], seen, needed);
    }
    // When the team's load screen comes and goes: the rig checks it is gone after a rejoin
    // (Lisara 2026-10-14: after Continue the team bars stayed on top). Kaniel 2026-10-15: it
    // covers the whole screen, the game's input and its sound until every hunter is in.
    if (view.up != shown) {
        shown = view.up;
        team_load_covers.store(shown, std::memory_order_relaxed);
        LOG_INFO(Debug, "Co-op team load: the team's load view is {} ({} rows)",
                 shown ? "up" : "gone", view.rows.size());
        LOG_INFO(Debug, "Co-op team load: {}",
                 shown ? "the screen covers the game, its sound held"
                       : "the screen opens, the sound back");
        if (!shown) {
            said_stage = bbcoop::LoadProgress::WAITING;
        }
    }
    std::scoped_lock lock{team_load_view_mutex};
    team_load_view = std::move(view);
}

bool TeamScreenUp() {
    if (!team_switch) {
        return false;
    }
    if (team_menu_open.load(std::memory_order_relaxed)) {
        return true;
    }
    std::scoped_lock lock{team_mutex};
    return team_gate.Holding() && !lobby_playing;
}

void TeamMenu(bool open, bool from_page) {
    if (!team_switch || team_menu_open.exchange(open) == open) {
        return;
    }
    bool page = from_page;
    {
        std::scoped_lock lock{team_back_mutex};
        if (open) {
            team_menu_from_page = from_page;
        } else {
            // The game's own page under the screen (bbport_game_menu.cpp) closes with it: Circle,
            // pressed for the game once the player's buttons are up (TeamMenuBackPressed). The
            // Insert menu's has no page of the game under it.
            page = team_menu_from_page;
            team_menu_back.Closed(SteadyMilliseconds(), page);
        }
    }
    LOG_INFO(Debug, "Co-op team menu: the team screen is {} the {}",
             open ? "open from" : "closed back to",
             page ? "game's System menu" : "port's Insert menu");
}

bool TeamMenuOpen() {
    return team_menu_open.load(std::memory_order_relaxed);
}

bool TeamWorldListAsked() {
    return team_worlds_asked.exchange(false, std::memory_order_relaxed);
}

bool TeamMenuBackPressed() {
    std::scoped_lock lock{team_back_mutex};
    return team_menu_back.Pressed(SteadyMilliseconds());
}

DeathMode TeamDeathModeNow() {
    std::scoped_lock lock{team_mutex};
    const std::optional<bbcoop::LobbyState>& lobby = team_gate.Lobby();
    return lobby ? TeamDeathMode(*lobby) : DeathMode::GoDown;
}

bool TeamPlaying(bool* in_hunt) {
    std::scoped_lock lock{team_mutex};
    if (in_hunt != nullptr) {
        *in_hunt = lobby_playing && !team_party.empty();
    }
    return lobby_playing;
}

void TeamJoinHunt(const std::string& invite_team) {
    std::scoped_lock lock{team_mutex};
    if (!lobby_playing || !team_party.empty() || !team_gate.Lobby()) {
        return;
    }
    const int slot = character_slot.load();
    const auto requests = TeamJoinHuntRequests(*team_gate.Lobby(), lobby_member, invite_team,
                                               slot < 0 ? 0u : static_cast<u32>(slot), lobby_name,
                                               slot < 0 ? std::string{} : ReadTeamWorldHint(slot));
    LOG_INFO(Debug, "Co-op team menu: this save joins the hunt ({} requests, slot {}{})",
             requests.size(), slot, invite_team.empty() ? "" : ", the invitation accepted");
    for (const bbcoop::TeamRequest& request : requests) {
        bbcoop::Envelope envelope;
        *envelope.mutable_team_request() = request;
        lobby_outbox.push_back(std::move(envelope));
    }
}

namespace {

// Kaniel 2026-10-16: out of the team this save played in (removed by its leader, or it left from
// the menu): the team's session is left where the player stands, as a guest alone in it leaves
// (TeamJoin::LeaveAlone), and the save plays on alone: no party, its own world back from the next
// load. The white phantom of a joined guest is a player again a little later (PhantomHome).
void LeaveTeamInPlay(u64 manager) {
    std::string party;
    {
        std::scoped_lock lock{team_mutex};
        party = std::exchange(team_party, {});
        team_world.reset();
        team_session.reset();
        team_world_due = false;
        team_world_taken = false;
        team_world_blocks.clear();
        team_host = false;
        personal_world_due = true;
        team_join = {};
        team_load = {};
        team_gate.DropStart();
        left_team_in_play = true;
    }
    {
        std::scoped_lock lock{stream_mutex};
        hello_party.clear();
    }
    if (party_controller) {
        party_controller->Wait();
    }
    const s32 matching = ReadValue<s32>(manager, 0x124);
    if (matching != 0) {
        reinterpret_cast<void PS4_SYSV_ABI (*)(u64)>(image_base +
                                                     MultiPlayStopRequestOffset)(manager);
    }
    LOG_INFO(Debug,
             "Co-op team menu: out of the team ({}): {}; the save plays on alone where it "
             "stands",
             party, matching != 0 ? "left its session" : "no session to leave");
}

// Kaniel 2026-10-16: the team's Start for a save playing alone (it joined the hunt from the
// menu): the team's party and role as at the title's Start, and its loading screen; the session
// is joined in place (TeamJoin), the team's world taken as the join's reload brings the blocks.
void AdoptTeamInPlay(const bbcoop::TeamStart& start) {
    std::string me;
    {
        std::scoped_lock lock{team_mutex};
        me = lobby_member;
        team_party = start.party();
        team_world.reset();
        team_session.reset();
        team_world_due = true;
        team_world_taken = false;
        team_world_blocks.clear();
        team_host = start.host() == me;
        personal_world_due = false;
        team_world_reported = {};
        team_world_reported_ms = 0;
        team_join = {};
        team_load.Start(SteadyMilliseconds());
        left_team_in_play = false;
    }
    const bool host = start.host() == me;
    if (party_controller) {
        party_controller->Assign(host ? PartyRole::Host : PartyRole::Guest);
    }
    {
        std::scoped_lock lock{stream_mutex};
        hello_party = start.party();
        hello_due = true;
        if (hello_member.empty()) {
            hello_member = me;
        }
    }
    LOG_INFO(Debug, "Co-op team menu: this save joins team {} where it stands, as its {}",
             start.team(), host ? "host" : "guest");
    // The team's loading screen takes over from the menu (back in the game when it opens).
    TeamMenu(false, false);
}

void TickTeamPlay(u64 manager) {
    TeamPlayStep step = TeamPlayStep::None;
    std::optional<bbcoop::TeamStart> start;
    {
        std::scoped_lock lock{team_mutex};
        if (!lobby_playing) {
            return;
        }
        const auto& lobby = team_gate.Lobby();
        start = team_gate.Start();
        step = TeamPlayStepOf(lobby ? &*lobby : nullptr, team_party, start ? &*start : nullptr,
                              lobby_member);
    }
    if (step == TeamPlayStep::Leave) {
        LeaveTeamInPlay(manager);
    } else if (step == TeamPlayStep::Adopt) {
        AdoptTeamInPlay(*start);
    }
}

} // namespace

void TeamChoose(TeamGate::Choice choice) {
    std::scoped_lock lock{team_mutex};
    if (!team_gate.Holding() || lobby_playing) {
        return;
    }
    static constexpr std::array<const char*, 3> Names{"ready", "start", "alone"};
    LOG_INFO(Debug, "Co-op team lobby: the team screen chose {}",
             Names[static_cast<size_t>(choice)]);
    for (bbcoop::TeamRequest& request : team_gate.Choose(choice, lobby_member)) {
        bbcoop::Envelope envelope;
        *envelope.mutable_team_request() = std::move(request);
        lobby_outbox.push_back(std::move(envelope));
    }
}

void TeamBackAtTitle() {
    if (!team_switch) {
        return;
    }
    std::scoped_lock lock{team_mutex};
    if (lobby_playing || team_gate.Holding()) {
        team_gate.Reset();
        lobby_playing = false;
        lobby_hello_due = true;
    }
    team_session.reset();
    team_load = {};
    team_load_settle = {};
    left_team_in_play = false;
    team_world_default_sent = false;
    team_load_covers.store(false, std::memory_order_relaxed);
    team_menu_open.store(false, std::memory_order_relaxed);
    {
        std::scoped_lock view_lock{team_load_view_mutex};
        team_load_view = {};
    }
}

bool TickTeamJoin(bool ready) {
    if (!team_switch) {
        return false;
    }
    UpdateTeamLoad();
    const u64 manager = GetMatchingState();
    if (manager < 0x10000 || !HasMemoryAccess(manager, 0x128, MemoryProt::CpuRead)) {
        return true;
    }
    // Kaniel 2026-10-16: the team menu in game: out of the team, or into it, where it stands.
    TickTeamPlay(manager);
    std::optional<bbcoop::TeamSession> session;
    bool in_team = false;
    bool left_in_play = false;
    {
        std::scoped_lock lock{team_mutex};
        in_team = !team_party.empty() && lobby_playing;
        left_in_play = left_team_in_play && lobby_playing;
        session = team_session;
    }
    // A save playing alone from its start reads none of this: the game's flag read walks the flag
    // tree the loading thread rebuilds (rig 2026-10-16: a solo guest's load hung 3 times in 7 when
    // every save read flag 4047 here). A save out of its team since still needs it to be a player.
    if (!in_team && !left_in_play) {
        return true;
    }
    static LeavingFlag leaving;
    if (leaving.Clear(EventFlagOn(LeavingFlag::Flag), InPartySession(), SteadyMilliseconds())) {
        using SetFlag = void PS4_SYSV_ABI (*)(u64, s32, bool);
        reinterpret_cast<SetFlag>(image_base + PartyGhostSetEventFlagOffset)(0, LeavingFlag::Flag,
                                                                             false);
        LOG_INFO(Debug,
                 "Co-op team join: flag {} (a player on the way out) cleared: this game "
                 "stands alone in its world",
                 LeavingFlag::Flag);
    }
    // A team's guest is in its own world in a session too (joined in place): once that session is
    // over it is a player again, not a white phantom (the summon's way home, a reload, did that;
    // rig 2026-10-14: a save loaded as a phantom and never joined again). Only well after the
    // session (matching idle for PhantomHomeMs): while it joins, the game makes it a phantom first.
    static PhantomHome home;
    const u64 player = GetLocalPlayer();
    const bool phantom = player >= 0x10000 && HasMemoryAccess(player, 0x7C, MemoryProt::CpuWrite) &&
                         ReadValue<s32>(player, 0x78) == WhiteGhostCharacterType;
    if (home.Due(phantom, !InPartySession() && ReadValue<s32>(manager, 0x124) == 0,
                 SteadyMilliseconds())) {
        WriteValue(player, 0x78, s32{0});
        LOG_INFO(Debug, "Co-op team join: a white phantom out of a session in its own world: a "
                        "player again");
    }
    if (!in_team) {
        return true; // alone (from the title, or out of its team since): no session to open or join
    }
    const TeamJoin::Frame frame{
        .host = !IsPartyGuestRole(),
        .matching_state = ReadValue<s32>(manager, 0x124),
        .ready = ready,
        .session_known = session.has_value() && !session->join().empty(),
        .room_owner = Libraries::Np::NpMatching2::OwnsItsRoom(),
        .room_has_others = Libraries::Np::NpMatching2::OwnedRoomHasOthers(),
        .session_members = TeamSessionMembers(SessionMembers(), !InvaderInWorld().empty()),
        .standing = party_death_run.downed.Current() == Vital::Up ||
                    party_death_run.downed.Current() == Vital::Off,
        .now_ms = SteadyMilliseconds(),
    };
    const auto say = [](std::string join, u64 number) {
        bbcoop::Envelope envelope;
        envelope.mutable_team_session()->set_join(std::move(join));
        envelope.mutable_team_session()->set_kind(TeamSessionKind);
        envelope.mutable_team_session()->set_sign(TeamSessionSign);
        envelope.mutable_team_session()->set_session(number);
        Queue(std::move(envelope));
    };
    switch (team_join.Tick(frame)) {
    case TeamJoin::Action::None:
        break;
    case TeamJoin::Action::CreateSession:
        OpenTeamSession();
        break;
    case TeamJoin::Action::Publish:
        if (const auto join = TeamSessionJoinData()) {
            ++team_sessions_opened;
            say(std::string{join->begin(), join->end()}, team_sessions_opened);
            LOG_INFO(Debug, "Co-op team join: this host's session {} to the server ({} bytes)",
                     team_sessions_opened, join->size());
        } else {
            LOG_WARNING(Debug, "Co-op team join: this host's session has no join data");
        }
        break;
    case TeamJoin::Action::PublishClosed:
        say({}, team_sessions_opened);
        LOG_INFO(Debug, "Co-op team join: this host's session {} closed", team_sessions_opened);
        break;
    case TeamJoin::Action::TakeHost:
        // Phase 7, host migration: the host's game went and the server passed the room to this
        // guest. The game's room layer moved the owner flag (every game's session host follows,
        // SprjSessionManager +0xD0); this game hosts the session now: +0x124 6 -> 3 (hosting) and
        // +0x120 = 4 as a created session has, the host's role, and a hello saying so. Its next
        // tick says the session to the server (Publish) for anyone joining later.
        WriteValue(manager, 0x124, s32{MatchingStateHosting});
        WriteValue(manager, 0x120, s32{4});
        if (party_controller) {
            party_controller->Assign(PartyRole::Host);
        }
        RequestServerHello();
        LOG_INFO(Debug, "Co-op team join: the session host moved to this game (the room is ours)");
        break;
    case TeamJoin::Action::LeaveAlone:
        // Phase 7: the others left the room (only the server ends a session, so it stays with this
        // guest alone in it): it leaves, and joins the session its host opens again.
        reinterpret_cast<void PS4_SYSV_ABI (*)(u64)>(image_base +
                                                     MultiPlayStopRequestOffset)(manager);
        LOG_INFO(Debug,
                 "Co-op team join: alone in the session; leaving it to join the host's next");
        break;
    case TeamJoin::Action::Join: {
        const std::string& join = session->join();
        // Each guest's own player slot (two guests on one slot took each other's moves as their
        // own: Kaniel 2026-10-15).
        u32 sign = session->sign();
        {
            std::scoped_lock lock{team_mutex};
            if (const auto lobby = team_gate.Lobby()) {
                sign = TeamGuestSign(*lobby, lobby_member, session->host());
            }
        }
        LOG_INFO(Debug, "Co-op team join: joining with player slot {}", sign);
        JoinTeamSession({reinterpret_cast<const u8*>(join.data()), join.size()}, session->kind(),
                        sign);
        break;
    }
    }
    return true;
}

std::optional<ResumePoint> TakeServerResumePoint() {
    std::scoped_lock lock{team_mutex};
    if (server_resume_refused) {
        server_resume_point.reset();
    }
    return std::exchange(server_resume_point, std::nullopt);
}

bool TeamResumeDue() {
    return team_resume_due.load();
}

void EndTeamResume() {
    if (team_resume_due.exchange(false)) {
        LOG_INFO(Debug, "Co-op team load: the guest's resume is done");
    }
}

std::string TeamStartHost() {
    std::scoped_lock lock{team_mutex};
    return team_gate.Start() ? team_gate.Start()->host() : std::string{};
}

void RefuseServerResumePoint() {
    std::scoped_lock lock{team_mutex};
    server_resume_refused = true;
    server_resume_point.reset();
}

std::optional<bbcoop::LobbyState> TeamLobbyState() {
    std::scoped_lock lock{team_mutex};
    return team_gate.Lobby();
}

std::string TeamLobbyMember() {
    std::scoped_lock lock{team_mutex};
    return lobby_member;
}

std::string CharacterNameOf(std::string_view member) {
    std::scoped_lock lock{team_mutex};
    if (member == lobby_member) {
        return lobby_name;
    }
    const auto& lobby = team_gate.Lobby();
    if (!lobby) {
        return {};
    }
    for (const bbcoop::TeamMember& m : lobby->team().members()) {
        if (m.member() == member) {
            return m.name();
        }
    }
    for (const bbcoop::LobbyPlayer& player : lobby->players()) {
        if (player.member() == member) {
            return player.name();
        }
    }
    return {};
}

const HostRules& OwnHostRules() {
    static const HostRules own = [] {
        const char* setting = std::getenv("SHADPS4_BLOODBORNE_HOST_RULES");
        const HostRules rules = ParseHostRules(setting != nullptr ? setting : "");
        LOG_INFO(Debug, "Co-op host rules: this launcher's are {}", HostRulesText(rules));
        return rules;
    }();
    return own;
}

HostRules ActiveHostRules() {
    return in_team.load() ? RulesOf(team_rules_bits.load()) : OwnHostRules();
}

std::optional<std::vector<u32>> TakeKeptRot() {
    std::scoped_lock lock{kept_rot_mutex};
    return std::exchange(kept_rot, std::nullopt);
}

void ReportRot(std::span<const u32> rot) {
    if (!team_switch || !in_team.load()) {
        return;
    }
    bbcoop::Envelope envelope;
    envelope.mutable_team_request()->set_action(bbcoop::TeamRequest::ROT);
    for (const u32 kind : rot) {
        envelope.mutable_team_request()->add_rot(kind);
    }
    QueueLobby(std::move(envelope));
}

void TeamLobbyRequest(bbcoop::TeamRequest::Action action, const std::string& id) {
    bbcoop::Envelope envelope;
    *envelope.mutable_team_request() = TeamMenuRequest(action, id);
    // A typed code joins a team: not in the log.
    LOG_INFO(Debug, "Co-op team lobby: the menu asks {} {}",
             bbcoop::TeamRequest::Action_Name(action),
             action == bbcoop::TeamRequest::JOIN_CODE ? std::string{"(a code)"} : id);
    QueueLobby(std::move(envelope));
}

namespace {

std::filesystem::path PersonalWorldFile(int slot) {
    return Common::FS::GetUserPath(Common::FS::PathType::UserDir) /
           SlotFileName("personal_world", slot);
}

std::string ReadWholeFile(const std::filesystem::path& path) {
    std::ifstream file{path, std::ios::binary};
    return std::string{std::istreambuf_iterator<char>{file}, {}};
}

// Written whole beside it, then moved over it: a crash leaves the old file or the new one.
void WriteWholeFile(const std::filesystem::path& path, const std::string& text) {
    std::filesystem::path partial = path;
    partial += ".part";
    {
        std::ofstream file{partial, std::ios::binary | std::ios::trunc};
        file << text;
    }
    std::error_code error;
    std::filesystem::rename(partial, path, error);
    if (error) {
        LOG_ERROR(Debug, "Co-op team world: could not write {}: {}", path.string(),
                  error.message());
    }
}

struct WorldStore {
    FlagStore store;
    std::vector<FlagBlockBits> blocks;
};
std::optional<WorldStore> ReadWorldStore() {
    WorldStore world{ReadFlagStore(), {}};
    if (!world.store.whole || world.store.flags_per_block == 0 || world.store.blocks.empty()) {
        return std::nullopt;
    }
    for (const FlagBlock& block : world.store.blocks) {
        world.blocks.push_back({block.id, block.bytes});
    }
    return world;
}

// Sets the store's world flags to `target`'s: how many changed.
size_t SetWorldFlags(const WorldStore& world, const WorldFlags& target) {
    using SetFlag = void PS4_SYSV_ABI (*)(u64, s32, bool);
    const auto changes = WorldFlagChanges(world.store.flags_per_block, world.blocks, target);
    std::string which;
    for (const auto& [flag, on] : changes) {
        reinterpret_cast<SetFlag>(image_base + PartyGhostSetEventFlagOffset)(0, flag, on);
        if (which.size() < 600) {
            which += fmt::format(" {}{}", on ? "" : "-", flag);
        }
    }
    if (!which.empty()) {
        LOG_INFO(Debug, "Co-op team world: flags set to the world's (-off):{}", which);
    }
    return changes.size();
}

} // namespace

void ApplyTeamWorldOnLoad(bool trip) {
    if (!team_switch) {
        return;
    }
    // Invasions: an invasion's own world kept aside and never put back (the game stopped
    // mid-visit) comes back first; an invader takes no team's world into its save.
    RestoreInvasionHomeOnLoad();
    if (Invading()) {
        return;
    }
    std::optional<bbcoop::TeamWorld> world;
    bool due = false;
    bool personal = false;
    {
        std::scoped_lock lock{team_mutex};
        due = team_world_due && team_world.has_value();
        personal = personal_world_due;
        if (due) {
            world = team_world;
        }
        if (team_world_due && !team_world) {
            static s64 warned_ms = 0;
            if (SteadyMilliseconds() - warned_ms > 5'000) {
                warned_ms = SteadyMilliseconds();
                LOG_WARNING(Debug, "Co-op team world: the map loads before the server's world "
                                   "came; it goes in at the next load");
            }
        }
    }
    const int slot = character_slot.load();
    if (personal && slot >= 0) {
        // Alone again: this save's own world back where a team's was in it, block by block as
        // the loads bring them; the file keeps the blocks still to come.
        const std::string text = ReadWholeFile(PersonalWorldFile(slot));
        std::scoped_lock lock{team_mutex};
        if (text.empty()) {
            personal_world_due = false;
            return;
        }
        auto own = ParseWorldFlags(text);
        std::optional<WorldStore> store = ReadWorldStore();
        if (!own || !store) {
            LOG_WARNING(Debug, "Co-op team world: slot {}'s own world {}", slot,
                        own ? "waits (the flags are not readable now)" : "does not read: left");
            personal_world_due = own.has_value();
            return;
        }
        std::erase_if(store->blocks, [&](const FlagBlockBits& block) {
            return !std::binary_search(own->blocks.begin(), own->blocks.end(), block.id);
        });
        if (store->blocks.empty()) {
            return;
        }
        const size_t changed = SetWorldFlags(*store, *own);
        const u32 per_block = store->store.flags_per_block;
        for (const FlagBlockBits& block : store->blocks) {
            std::erase(own->blocks, block.id);
            std::erase_if(own->on, [&](s32 flag) { return u32(flag) / per_block == block.id; });
        }
        if (own->blocks.empty()) {
            std::error_code error;
            std::filesystem::remove(PersonalWorldFile(slot), error);
            personal_world_due = false;
        } else {
            WriteWholeFile(PersonalWorldFile(slot), FormatWorldFlags(*own));
        }
        LOG_INFO(Debug,
                 "Co-op team world: slot {} plays alone: its own world is back in {} "
                 "blocks ({} flags changed, {} blocks to come)",
                 slot, store->blocks.size(), changed, own->blocks.size());
        return;
    }
    if (!due) {
        return;
    }
    std::optional<WorldStore> store = ReadWorldStore();
    if (!store || slot < 0) {
        return; // the next call (the flags readable, the slot known)
    }
    std::scoped_lock lock{team_mutex};
    if (!world->exists()) {
        if (!team_world_taken) {
            LOG_INFO(Debug, "Co-op team world: the team has no world yet: {}",
                     team_host ? "this save's makes it" : "the host's save makes it");
        }
        // The host whose save makes it has the team's world already: its later loads never take
        // the server's copy back over it, a report up to 10 s old (rig 2026-10-20, boss-refight:
        // the host's next load put back Father Gascoigne's kill it had cleared after its report).
        if (team_host) {
            for (const FlagBlockBits& block : store->blocks) {
                team_world_blocks.insert(block.id);
            }
        }
        team_world_taken = true;
        return;
    }
    const WorldFlags target = WorldFlagsOf(*world);
    if (!team_host) {
        // Every load of a guest (its join's reload and its trips give it its own saved world
        // back): the team's lamps lit again, and on this character's lamp list, or the game puts
        // them out again (Kaniel's play test 2026-10-18: krolebord's lamps listed only Cathedral
        // Ward, his own save's).
        using SetFlag = void PS4_SYSV_ABI (*)(u64, s32, bool);
        const std::vector<s32> lamp_flags = TeamLampFlagsToSet(target, EventFlagOn);
        for (const s32 flag : lamp_flags) {
            reinterpret_cast<SetFlag>(image_base + PartyGhostSetEventFlagOffset)(0, flag, true);
        }
        if (!lamp_flags.empty()) {
            ListLamps(TeamWorldLamps(store->store.flags_per_block, target.blocks, target));
            LOG_INFO(Debug, "Co-op team world: {} of the team's lamp flags lit again in this load",
                     lamp_flags.size());
        }
    }
    // The blocks this load brings that the team's world covers and no earlier load took; on a
    // guest's trip every one again, the global flags (NPCs' steps) too: the trip's load gave its
    // game its own saved world back (the host's save is the team's world, never retaken). The
    // session's changes go on top after (SetGuestWorldChanges).
    const bool retake = trip && !team_host;
    std::erase_if(store->blocks, [&](const FlagBlockBits& block) {
        return !TakesTeamWorldBlock(block.id, team_world_blocks, target.blocks, retake);
    });
    if (store->blocks.empty()) {
        return;
    }
    // This save's own world aside first, block by block as the loads bring them: a block the file
    // has already (a save that held a team's before) keeps what the file says.
    {
        WorldFlags own =
            ParseWorldFlags(ReadWholeFile(PersonalWorldFile(slot))).value_or(WorldFlags{});
        std::vector<FlagBlockBits> new_blocks;
        for (const FlagBlockBits& block : store->blocks) {
            if (!std::binary_search(own.blocks.begin(), own.blocks.end(), block.id)) {
                new_blocks.push_back(block);
            }
        }
        if (!new_blocks.empty()) {
            const WorldFlags added = CaptureWorldFlags(store->store.flags_per_block, new_blocks);
            own.blocks.insert(own.blocks.end(), added.blocks.begin(), added.blocks.end());
            own.on.insert(own.on.end(), added.on.begin(), added.on.end());
            std::sort(own.blocks.begin(), own.blocks.end());
            std::sort(own.on.begin(), own.on.end());
            WriteWholeFile(PersonalWorldFile(slot), FormatWorldFlags(own));
            LOG_INFO(Debug,
                     "Co-op team world: slot {}'s own world kept aside ({} more blocks, {} "
                     "flags on in all)",
                     slot, added.blocks.size(), own.on.size());
        }
    }
    const size_t changed = SetWorldFlags(*store, target);
    for (const FlagBlockBits& block : store->blocks) {
        team_world_blocks.insert(block.id);
    }
    if (!team_host) {
        team_story_followed = target;
    }
    team_world_taken = true;
    LOG_INFO(Debug,
             "Co-op team world: the team's world is in this save{}: {} blocks, {} flags "
             "changed (version {})",
             retake ? fmt::format(" again after a trip's load (map {:#x})", GetCurrentPackedMap())
                    : "",
             store->blocks.size(), changed, world->version());
}

std::vector<u32> TeamWorldBlocksForGuest() {
    std::scoped_lock lock{team_mutex};
    if (!team_switch || team_host || !team_world || !team_world->exists()) {
        return {};
    }
    return WorldFlagsOf(*team_world).blocks;
}

std::vector<s32> TeamWorldFlagsOnForGuest() {
    std::scoped_lock lock{team_mutex};
    if (!team_switch || team_host || !team_world || !team_world->exists()) {
        return {};
    }
    return WorldFlagsOf(*team_world).on;
}

void TickTeamWorld(s64 now_ms) {
    if (!team_switch) {
        return;
    }
    {
        std::scoped_lock lock{team_mutex};
        if (!team_host || team_party.empty() || !team_world_taken || !team_world ||
            now_ms - team_world_reported_ms < TeamWorldReportMs) {
            return;
        }
        team_world_reported_ms = now_ms;
    }
    const auto store = ReadWorldStore();
    if (!store) {
        return;
    }
    WorldFlags world = CaptureWorldFlags(store->store.flags_per_block, store->blocks);
    {
        std::scoped_lock lock{team_mutex};
        if (world == team_world_reported) {
            return;
        }
        team_world_reported = world;
    }
    bbcoop::Envelope envelope;
    envelope.mutable_team_world()->mutable_on()->Add(world.on.begin(), world.on.end());
    envelope.mutable_team_world()->mutable_blocks()->Add(world.blocks.begin(), world.blocks.end());
    {
        std::scoped_lock lock{team_mutex};
        envelope.mutable_team_world()->set_character(lobby_name);
    }
    Queue(std::move(envelope));
    LOG_INFO(Debug, "Co-op team world: this host's world to the server ({} flags on)",
             world.on.size());
}

// Kaniel's playtest 2026-10-17: the night (Vicar Amelia's skull) fell only in the games that
// played its cutscene. A guest away from it takes the team world's time of day as the host's
// report brings it; its next load shows it. Read between loads only, every 2 s.
void FollowTeamTimeOfDay(s64 now_ms) {
    static s64 checked_ms = 0;
    if (!team_switch || !IsPartyGuestRole() || !InPartySession() || now_ms - checked_ms < 2'000) {
        return;
    }
    checked_ms = now_ms;
    WorldFlags team;
    {
        std::scoped_lock lock{team_mutex};
        if (team_host || !team_world_taken || !team_world || !team_world->exists()) {
            return;
        }
        for (const s32 flag : team_world->on()) {
            if (std::ranges::find(TimeOfDayFlags, flag) != TimeOfDayFlags.end()) {
                team.on.push_back(flag);
            }
        }
    }
    std::sort(team.on.begin(), team.on.end());
    const u64 global = GetGlobalState();
    if (team.on.empty() || global < 0x10000 || TryRead<u8>(global, 0x08).value_or(1) != 0) {
        return;
    }
    using SetFlag = void PS4_SYSV_ABI (*)(u64, s32, bool);
    for (const s32 flag : TimeOfDayToFollow(team, EventFlagOn)) {
        reinterpret_cast<SetFlag>(image_base + PartyGhostSetEventFlagOffset)(0, flag, true);
        LOG_INFO(Debug, "Co-op team world: the team's time of day is this guest's too (flag {})",
                 flag);
    }
}

// Kaniel's playtest 2026-10-22: a guest's load takes the team world's time of day exactly, after
// the session's changes went in (its own game's time of day can be among them: the rig's
// team-time-of-day-trip), so the map's events pick the team's characters. The load's hooks only.
void ApplyTeamTimeOfDayOnLoad() {
    static u32 flags_per_block = 0;
    if (flags_per_block == 0) {
        flags_per_block = ReadFlagStore().flags_per_block;
    }
    std::vector<std::pair<s32, bool>> changes;
    {
        std::scoped_lock lock{team_mutex};
        if (!team_switch || team_host || !team_world_taken || !team_world ||
            !team_world->exists()) {
            return;
        }
        changes = TeamTimeOfDayAtLoad(WorldFlagsOf(*team_world), flags_per_block, EventFlagOn);
    }
    using SetFlag = void PS4_SYSV_ABI (*)(u64, s32, bool);
    std::string which;
    for (const auto& [flag, on] : changes) {
        reinterpret_cast<SetFlag>(image_base + PartyGhostSetEventFlagOffset)(0, flag, on);
        which += fmt::format(" {}{}", on ? "" : "-", flag);
    }
    if (!which.empty()) {
        LOG_INFO(Debug, "Co-op team world: the team's time of day in this load (-off):{}", which);
    }
}

// Kaniel 2026-10-20: NPCs showed up for guests on maps without the host. The host's story steps
// (TeamStoryToFollow) reached a guest only at its next trip's load; now as each report of the
// host's world comes, read between loads only, every 2 s.
void FollowTeamStory(s64 now_ms) {
    static s64 checked_ms = 0;
    if (!team_switch || !IsPartyGuestRole() || now_ms - checked_ms < 2'000) {
        return;
    }
    checked_ms = now_ms;
    std::vector<std::pair<s32, bool>> follow;
    {
        std::scoped_lock lock{team_mutex};
        if (team_host || !team_world_taken || !team_world || !team_world->exists() ||
            !InPartySession()) {
            team_story_followed = {};
            return;
        }
        const u64 global = GetGlobalState();
        if (global < 0x10000 || TryRead<u8>(global, 0x08).value_or(1) != 0) {
            return; // loading: the load takes it, or the next check
        }
        WorldFlags target = WorldFlagsOf(*team_world);
        if (team_story_followed.blocks.empty()) {
            team_story_followed = std::move(target);
            return;
        }
        follow = TeamStoryToFollow(team_story_followed, target);
        team_story_followed = std::move(target);
    }
    using SetFlag = void PS4_SYSV_ABI (*)(u64, s32, bool);
    std::string which;
    for (const auto& [flag, on] : follow) {
        if (EventFlagOn(flag) == on) {
            continue;
        }
        reinterpret_cast<SetFlag>(image_base + PartyGhostSetEventFlagOffset)(0, flag, on);
        if (which.size() < 300) {
            which += fmt::format(" {}{}", on ? "" : "-", flag);
        }
    }
    if (!which.empty()) {
        LOG_INFO(Debug, "Co-op team world: the host's story steps followed here (-off):{}", which);
    }
}

bool IsTeamScreenVerb(std::string_view verb) {
    return verb == "team-screen" || verb == "team-ready" || verb == "team-start" ||
           verb == "team-alone" || verb == "team-worlds";
}

bool IsTeamTitleVerb(std::string_view verb) {
    return verb.starts_with("team-");
}

bool RunTeamTitleVerb(const AutoplayCommand& command, bool begun, double elapsed,
                      std::string& failure) {
    constexpr double TimeoutSeconds = 180.0;
    std::optional<bbcoop::LobbyState> lobby;
    std::string me;
    {
        std::scoped_lock lock{team_mutex};
        lobby = team_gate.Lobby();
        me = lobby_member;
    }
    const auto request = [](bbcoop::TeamRequest::Action action, std::string member = {},
                            std::string team = {}) {
        bbcoop::Envelope envelope;
        envelope.mutable_team_request()->set_action(action);
        envelope.mutable_team_request()->set_member(std::move(member));
        envelope.mutable_team_request()->set_team(std::move(team));
        QueueLobby(std::move(envelope));
    };
    bool done = false;
    if (command.verb == "team-create") {
        // Once the lobby answered: create, then wait for the team in the state.
        done = lobby && lobby->has_team();
        if (lobby && !lobby->has_team() && (!begun || elapsed > 5.0)) {
            request(bbcoop::TeamRequest::CREATE);
        }
    } else if (command.verb == "team-invite" && command.args.size() == 1) {
        // A hosted lobby lists 32 players: one not listed is invited by its id every 5 s (the
        // server refuses it while it is not signed in); once invited the lobby lists it.
        static double invited_at = -100.0;
        if (!begun) {
            invited_at = -100.0;
        }
        const std::string member = BoardMemberOfOnlineId(command.args[0]);
        if (lobby && lobby->has_team()) {
            const bool listed =
                std::ranges::any_of(lobby->players(), [&](const bbcoop::LobbyPlayer& player) {
                    return player.member() == member;
                });
            if (listed || elapsed - invited_at >= 5.0) {
                invited_at = elapsed;
                request(bbcoop::TeamRequest::INVITE, member);
                done = listed;
            }
        }
    } else if (command.verb == "team-accept") {
        done = lobby && lobby->has_team();
        if (!done && lobby && lobby->invites_size() > 0) {
            request(bbcoop::TeamRequest::ACCEPT, {}, lobby->invites(0).team());
        }
    } else if (IsTeamScreenVerb(command.verb)) {
        if (!TeamScreenUp()) {
            return false; // the title's Cross first (TickAutoplayTitle presses it)
        }
        // Kaniel 2026-10-20: a member's screen is ready by itself; team-ready waits for that,
        // and the rig's leader begins once every member is (the lobby would start without one
        // not ready yet, which then goes in by itself).
        if (command.verb == "team-ready" || command.verb == "team-start") {
            const bool ready = command.verb == "team-ready" ? lobby && TeamMemberReady(*lobby, me)
                                                            : lobby && TeamCanBegin(*lobby, me);
            if (!ready) {
                if (elapsed > TimeoutSeconds) {
                    failure = command.verb == "team-ready" ? "the team screen never said ready"
                                                           : "not every member got ready";
                }
                return false;
            }
        }
        if (command.verb == "team-worlds") {
            // Kaniel 2026-10-17: the world list (the port's team screen opens it once asked).
            team_worlds_asked.store(true, std::memory_order_relaxed);
        } else if (command.verb == "team-start" || command.verb == "team-alone") {
            TeamChoose(command.verb == "team-start" ? TeamGate::Choice::Start
                                                    : TeamGate::Choice::Alone);
        }
        done = true;
    } else if (command.verb == "team-leave") {
        done = lobby && !lobby->has_team();
        if (lobby && lobby->has_team() && (!begun || elapsed > 5.0)) {
            request(bbcoop::TeamRequest::LEAVE);
        }
    } else if (command.verb == "team-wait" && command.args.size() == 1) {
        done = lobby && lobby->has_team() &&
               lobby->team().members_size() >= std::atoi(command.args[0].c_str());
    } else {
        failure = "unknown team verb";
        return false;
    }
    if (!done && elapsed > TimeoutSeconds) {
        failure = "timed out in the lobby";
    }
    return done;
}

bool RunTeamMenuVerb(const AutoplayCommand& command, bool begun, double elapsed,
                     std::string& failure) {
    // menu-invite <online id> [timeout], menu-kick <online id> [timeout], menu-join [timeout],
    // menu-leave [timeout]. Invasions: menu-invade [timeout] (in the target's world),
    // menu-invade-leave [timeout] (home), menu-allow-invaders <0|1> [timeout] (the leader's
    // toggle), wait-invader <0|1> [timeout] (an invader in this team's world or not).
    const size_t timeout_arg = command.verb == "menu-invite" || command.verb == "menu-kick" ||
                                       command.verb == "menu-allow-invaders" ||
                                       command.verb == "wait-invader"
                                   ? 1
                                   : 0;
    const double timeout =
        command.args.size() > timeout_arg ? std::atof(command.args[timeout_arg].c_str()) : 180.0;
    std::optional<bbcoop::LobbyState> lobby;
    {
        std::scoped_lock lock{team_mutex};
        lobby = team_gate.Lobby();
    }
    // What the screen's buttons send, at most every 5 s while the lobby does not show it done.
    static double asked_at = -100.0;
    if (!begun) {
        asked_at = -100.0;
    }
    const bool ask = elapsed - asked_at >= 5.0;
    const auto member_in_team = [&](const std::string& member) {
        return lobby && lobby->has_team() &&
               std::ranges::any_of(lobby->team().members(), [&](const bbcoop::TeamMember& m) {
                   return m.member() == member;
               });
    };
    bool in_hunt = false;
    TeamPlaying(&in_hunt);
    bool done = false;
    if (command.verb == "menu-invite" && !command.args.empty()) {
        // Listed or not (a hosted lobby lists 32 players): the server refuses one not signed in.
        const std::string member = BoardMemberOfOnlineId(command.args[0]);
        done = member_in_team(member);
        if (!done && lobby && lobby->has_team() && ask) {
            asked_at = elapsed;
            TeamLobbyRequest(bbcoop::TeamRequest::INVITE, member);
        }
    } else if (command.verb == "menu-kick" && !command.args.empty()) {
        const std::string member = BoardMemberOfOnlineId(command.args[0]);
        done = lobby && !member_in_team(member);
        if (!done && ask) {
            asked_at = elapsed;
            TeamLobbyRequest(bbcoop::TeamRequest::REMOVE, member);
        }
    } else if (command.verb == "menu-join") {
        done = in_hunt;
        if (!done && lobby && ask && (lobby->has_team() || lobby->invites_size() > 0)) {
            asked_at = elapsed;
            TeamJoinHunt(lobby->has_team() ? std::string{} : lobby->invites(0).team());
        }
    } else if (command.verb == "menu-leave") {
        done = lobby && !lobby->has_team() && !in_hunt;
        if (!done && lobby && lobby->has_team() && ask) {
            asked_at = elapsed;
            TeamLobbyRequest(bbcoop::TeamRequest::LEAVE, {});
        }
    } else if (command.verb == "menu-invade" || command.verb == "menu-invade-leave") {
        const bool leave = command.verb == "menu-invade-leave";
        bool active = false;
        {
            std::scoped_lock lock{team_mutex};
            active = invader_trip.Active();
            done = leave ? !active : invader_trip.Current() == InvaderTrip::Phase::Visiting;
        }
        if (!done && ask && active == leave) {
            asked_at = elapsed;
            InvasionRequest(leave);
        }
    } else if (command.verb == "menu-allow-invaders" && !command.args.empty()) {
        const bool allow = command.args[0] != "0";
        done = TeamAllowsInvaders() == allow;
        if (!done && lobby && lobby->has_team() && ask) {
            asked_at = elapsed;
            TeamAllowInvaders(allow);
        }
    } else if (command.verb == "wait-invader" && !command.args.empty()) {
        done = InvaderInWorld().empty() == (command.args[0] == "0");
    } else {
        failure = "unknown team menu verb";
        return false;
    }
    if (!done && elapsed > timeout) {
        failure = "timed out in the team menu";
    }
    return done;
}

} // namespace Coop

// Invasions (bloodborne_invasion.h, docs/invasions/GAME_SIDE_PLAN.md): the invader's trip on the
// game thread, its own world kept aside for the visit, and what the menu and the notice need.
namespace {

std::filesystem::path InvasionHomeFile(int slot) {
    return Common::FS::GetUserPath(Common::FS::PathType::UserDir) /
           SlotFileName("invasion_home", slot);
}

// The invader's own world before the visit (its respawn lamp, every flag), to its slot's file
// before anything changes: a crash mid-visit puts it back at the next load. A file still there
// from an earlier visit is that world already (never written over).
bool SaveInvasionHome() {
    const int slot = character_slot.load();
    const u64 global = GetGlobalState();
    if (slot < 0 || global < 0x10000) {
        return false;
    }
    const std::filesystem::path path = InvasionHomeFile(slot);
    std::error_code error;
    if (std::filesystem::exists(path, error)) {
        LOG_INFO(Debug, "Co-op invasion: slot {}'s own world is kept aside already", slot);
        return true;
    }
    const std::optional<WorldStore> store = ReadWorldStore();
    if (!store) {
        return false;
    }
    const InvasionHome home{ReadValue<s32>(global, 0x1528),
                            CaptureWorldFlags(store->store.flags_per_block, store->blocks, true)};
    WriteWholeFile(path, FormatInvasionHome(home));
    LOG_INFO(Debug,
             "Co-op invasion: slot {}'s own world kept aside for the visit (respawn {}, {} "
             "flags on in {} blocks)",
             slot, home.respawn, home.flags.on.size(), home.flags.blocks.size());
    return true;
}

// Puts the invader's own world back (its flags; its respawn lamp with `respawn`) and forgets the
// file. False while the flags cannot be read (a load).
bool RestoreInvasionHome(bool respawn) {
    const int slot = character_slot.load();
    if (slot < 0) {
        return false;
    }
    const std::filesystem::path path = InvasionHomeFile(slot);
    const std::string text = ReadWholeFile(path);
    if (text.empty()) {
        return true;
    }
    std::error_code error;
    const std::optional<InvasionHome> home = ParseInvasionHome(text);
    if (!home) {
        LOG_WARNING(Debug, "Co-op invasion: slot {}'s kept world does not read: left", slot);
        std::filesystem::remove(path, error);
        return true;
    }
    const std::optional<WorldStore> store = ReadWorldStore();
    const u64 global = GetGlobalState();
    if (!store || global < 0x10000) {
        return false;
    }
    using SetFlag = void PS4_SYSV_ABI (*)(u64, s32, bool);
    const auto changes =
        WorldFlagChanges(store->store.flags_per_block, store->blocks, home->flags, true);
    for (const auto& [flag, on] : changes) {
        reinterpret_cast<SetFlag>(image_base + PartyGhostSetEventFlagOffset)(0, flag, on);
    }
    if (respawn) {
        WriteValue(global, 0x1528, home->respawn);
    }
    std::filesystem::remove(path, error);
    LOG_INFO(Debug, "Co-op invasion: slot {}'s own world is back ({} flags changed, respawn {})",
             slot, changes.size(), home->respawn);
    return true;
}

void SendInvadeRequest(bool cancel) {
    bbcoop::Envelope envelope;
    envelope.mutable_invade_request()->set_cancel(cancel);
    if (!cancel) {
        const u64 data = OwnGameData();
        const s32 level = data >= 0x10000 && HasMemoryAccess(data + 0x90, 4, MemoryProt::CpuRead)
                              ? ReadValue<s32>(data, 0x90)
                              : 0;
        envelope.mutable_invade_request()->set_level(level > 0 && level < 1000 ? level : 0);
        envelope.mutable_invade_request()->set_covenant_effect(OwnCovenantEffect());
    }
    QueueLobby(std::move(envelope));
}

// The invader goes home (GAME_SIDE_PLAN.md section 7): out of the target's session, a live player
// again, to its own respawn lamp (the kept world's); its flags come back once there. A dead
// invader first gets up where it fell (its death script's hook, ReviveInvaderInPlace).
void InvaderGoHome(bool died, bool tell_server) {
    SetInvasionTint(GetLocalPlayer(),false);
    {
        // The target world's enemy owners are not its own world's.
        std::scoped_lock lock{owners_mutex};
        server_owners.clear();
        owners_heard_ms = 0;
    }
    const u64 manager = GetMatchingState();
    if (InPartySession() && manager >= 0x10000) {
        reinterpret_cast<void PS4_SYSV_ABI (*)(u64)>(image_base +
                                                     MultiPlayStopRequestOffset)(manager);
    }
    const u64 player = GetLocalPlayer();
    if (player >= 0x10000 && HasMemoryAccess(player, 0x7C, MemoryProt::CpuRead) &&
        ReadValue<s32>(player, 0x78) != 0) {
        reinterpret_cast<void PS4_SYSV_ABI (*)(u64, s32, s32)>(image_base + SetChrTypeOffset)(
            0, LocalPlayerEntity, 0);
    }
    const int slot = character_slot.load();
    const auto home =
        slot >= 0 ? ParseInvasionHome(ReadWholeFile(InvasionHomeFile(slot))) : std::nullopt;
    if (home && home->respawn != 0) {
        // A moment later: the session's stop goes first (as party death's respawn trip).
        guest_trip_point = home->respawn;
        guest_lamp_trip_at = std::chrono::steady_clock::now() + std::chrono::milliseconds{1'500};
    }
    if (tell_server) {
        SendInvadeRequest(true);
    }
    LOG_INFO(Debug, "Co-op invasion: going home ({}{}), to lamp {}", died ? "died" : "ended",
             tell_server ? ", the server told" : "", home ? home->respawn : 0);
}

// How long a dead invader waits for its death script's hook before it is revived by us.
constexpr s64 InvaderReviveWaitMs = 8'000;

} // namespace

namespace Coop {

void QueueInvasionRewardAck(bbcoop::Envelope envelope) {
    // Solo games have no party Hello; that outbox only forwards map/vitals during a visit.
    // Reward mailboxes belong to the authenticated lobby and must be acknowledged there.
    if(envelope.has_trade_granted() && (envelope.trade_granted().id() & BloodborneCoop::InvasionRewardGrantBit))
        QueueLobby(std::move(envelope));
}

void InvasionRequest(bool cancel) {
    if (!team_switch) {
        return;
    }
    bool send = false;
    {
        std::scoped_lock lock{team_mutex};
        if (!cancel && team_gate.Lobby() && team_gate.Lobby()->has_team()) {
            LOG_INFO(Debug, "Co-op invasion: a player in a team does not invade");
            return;
        }
        send = cancel ? invader_trip.Cancel(SteadyMilliseconds())
                      : invader_trip.Ask(SteadyMilliseconds());
        if (cancel && send && invader_trip.Current() == InvaderTrip::Phase::GoingHome) {
            send = false; // InvaderGoHome tells the server
        }
    }
    LOG_INFO(Debug, "Co-op invasion: the menu {} ({})", cancel ? "leaves" : "asks to invade",
             send ? "to the server" : "nothing to send");
    if (send) {
        SendInvadeRequest(cancel);
    }
}

void TeamAllowInvaders(bool allow) {
    bbcoop::Envelope envelope;
    envelope.mutable_team_request()->set_action(bbcoop::TeamRequest::INVADERS);
    envelope.mutable_team_request()->set_allow(allow);
    LOG_INFO(Debug, "Co-op team lobby: the leader {} invaders", allow ? "allows" : "turns off");
    QueueLobby(std::move(envelope));
}

bool TeamAllowsInvaders() {
    std::scoped_lock lock{team_mutex};
    const auto lobby = team_gate.Lobby();
    return lobby && lobby->has_team() && lobby->team().allow_invaders();
}

InvasionView InvasionViewNow() {
    std::scoped_lock lock{team_mutex};
    return InvasionViewOf(invader_trip, invaded_team, SteadyMilliseconds());
}

bool Invading() {
    return team_switch && invader_away.load(std::memory_order_relaxed);
}

std::string InvaderInWorld() {
    if (!team_switch) {
        return {};
    }
    std::scoped_lock lock{team_mutex};
    return invaded_team.Invader();
}

void RestoreInvasionHomeOnLoad() {
    if (!team_switch) {
        return;
    }
    {
        std::scoped_lock lock{team_mutex};
        if (invader_trip.Active()) {
            return; // its own trip puts it back once home
        }
    }
    const int slot = character_slot.load();
    std::error_code error;
    if (slot >= 0 && std::filesystem::exists(InvasionHomeFile(slot), error) &&
        RestoreInvasionHome(true)) {
        LOG_WARNING(Debug,
                    "Co-op invasion: slot {} loaded with its world still kept aside (the "
                    "game stopped mid-visit): put back",
                    slot);
    }
}

void TickInvasion() {
    if (!team_switch) {
        return;
    }
    static s64 stable_since = -1;
    static u32 stable_map = 0;
    static bool home_kept = false; // its own world is in the file (SaveInvasionHome)
    static s64 going_home_ms = -1; // InvaderGoHome ran
    static s64 revive_due_ms = -1; // dead: going home once revived (or at this time)
    static bool home_tells_server = false;
    static bool late_cancel_sent = false; // its death: the server never said the end
    const s64 now_ms = SteadyMilliseconds();
    {
        std::scoped_lock lock{team_mutex};
        invaded_team.Tick(now_ms);
        if (!invader_trip.Active() && !home_kept) {
            invader_link.store(false);
            invader_away.store(false);
            return;
        }
    }
    // Settled: loaded, on a usable map, not on a trip, for 3 s (as the guest's resume).
    const u64 global = GetGlobalState();
    const bool loading = global < 0x10000 || ReadValue<u8>(global, 0x08) != 0;
    const u32 map = GetCurrentPackedMap();
    const auto at = LocalPlayerPosition();
    const bool ready = !loading && IsUsablePackedMap(map) && at && !PartyTripActive() &&
                       !guest_lamp_trip_at.has_value();
    if (!ready || map != stable_map) {
        stable_since = ready ? now_ms : -1;
        stable_map = ready ? map : 0;
    }
    const bool alive = party_death_run.downed.Current() != Vital::Dead;
    const bool revived = TakeInvaderDeath();
    const InvaderTrip::Frame frame{
        .now_ms = now_ms,
        .map = ready ? map : 0,
        .settled = ready && stable_since >= 0 && now_ms - stable_since >= 3'000,
        .in_session = InPartySession(),
        .dead = revived || !alive,
        .boss_fog = alive && EventFlagOn(4000),
    };
    InvaderTrip::Action action = InvaderTrip::Action::None;
    InvaderTrip::Phase phase = InvaderTrip::Phase::None;
    std::optional<bbcoop::InvasionStart> start;
    InvasionJoin join;
    bool died = false;
    bool tell = false;
    bbcoop::InvasionEnd::Reason reason = bbcoop::InvasionEnd::NONE;
    {
        std::scoped_lock lock{team_mutex};
        // The invader's own world kept aside before its trip changes anything.
        if (invader_trip.Current() == InvaderTrip::Phase::Travelling && !home_kept) {
            home_kept = SaveInvasionHome();
            if (!home_kept) {
                return; // the flags are not readable yet: no trip before the file is written
            }
        }
        action = invader_trip.Tick(frame);
        invader_away.store(invader_trip.AwayFromHome());
        phase = invader_trip.Current();
        start = invader_trip.Start();
        join = invader_trip.Join(invasion_fallback_switch, TeamSessionKind);
        died = invader_trip.Died();
        tell = invader_trip.TellsServer();
        reason = invader_trip.EndReason();
    }
    invader_link.store(phase == InvaderTrip::Phase::Joining ||
                       phase == InvaderTrip::Phase::Visiting ||
                       phase == InvaderTrip::Phase::GoingHome);
    if (phase == InvaderTrip::Phase::None) {
        // Over before it left home (a refusal, nothing found, an end during the notice or its
        // trip there, its death on the way): its own world was not touched but by the lamp trip.
        if (home_kept) {
            RestoreInvasionHome(true);
        }
        if (tell) {
            SendInvadeRequest(true);
        }
        home_kept = false;
        going_home_ms = -1;
        revive_due_ms = -1;
        LOG_INFO(Debug, "Co-op invasion: over before the join ({})", InvasionEndReasonName(reason));
        return;
    }
    switch (action) {
    case InvaderTrip::Action::None:
        break;
    case InvaderTrip::Action::Travel: {
        const ResumePoint point{start->map(), PositionOf(start->at()), 0};
        const ResumeStep step = ChooseResumeStep(point, map, *at, 0);
        if (step.kind == ResumeStep::Kind::Travel) {
            guest_trip_point = step.warp_point;
            guest_lamp_trip_at = std::chrono::steady_clock::now();
        }
        LOG_INFO(Debug, "Co-op invasion: travelling to the target's map {:#x} (lamp {})",
                 start->map(), step.kind == ResumeStep::Kind::Travel ? step.warp_point : 0);
        break;
    }
    case InvaderTrip::Action::Join: {
        const std::string& data = start->session().join();
        const bool joined = JoinTeamSession({reinterpret_cast<const u8*>(data.data()), data.size()},
                                            join.kind, join.sign);
        LOG_INFO(Debug, "Co-op invasion: joining {}'s session (kind {:#x}, sign {}{}): {}",
                 start->session().host(), join.kind, join.sign,
                 join.make_hostile ? ", the fallback" : "", joined ? "joined" : "not joined");
        if (joined && join.make_hostile) {
            reinterpret_cast<void PS4_SYSV_ABI (*)(u64, s32, s32)>(image_base + SetChrTypeOffset)(
                0, LocalPlayerEntity, InvaderCharacterType);
        }
        break;
    }
    case InvaderTrip::Action::Place:
        if (start->has_at()) {
            TeleportLocalPlayer(PositionOf(start->at()));
        }
        LOG_INFO(Debug, "Co-op invasion: in {}'s world, hunting {}", start->session().host(),
                 start->target());
        break;
    case InvaderTrip::Action::GoHome:
        home_tells_server = tell;
        if (died && !revived) {
            // Dead: its death script's hook revives it in place (ReviveInvaderInPlace); then home.
            revive_due_ms = now_ms + InvaderReviveWaitMs;
        } else {
            InvaderGoHome(died, tell);
            going_home_ms = now_ms;
        }
        break;
    }
    if (revive_due_ms >= 0 && going_home_ms < 0) {
        const bool hooked = revived && action != InvaderTrip::Action::GoHome;
        if (hooked || now_ms >= revive_due_ms) {
            if (!hooked && !alive) {
                ReviveInvaderInPlace(); // no death script's hook came
                TakeInvaderDeath();
            }
            revive_due_ms = -1;
            InvaderGoHome(true, home_tells_server);
            going_home_ms = now_ms;
        }
    }
    if (phase == InvaderTrip::Phase::Visiting && invasion_fallback_switch && frame.settled) {
        // The game sets the type back on its loads (PhantomHome): kept for the whole visit.
        const u64 player = GetLocalPlayer();
        SetInvasionTint(player,true);
        if (player >= 0x10000 && HasMemoryAccess(player, 0x7C, MemoryProt::CpuRead) &&
            ReadValue<s32>(player, 0x78) != InvaderCharacterType) {
            reinterpret_cast<void PS4_SYSV_ABI (*)(u64, s32, s32)>(image_base + SetChrTypeOffset)(
                0, LocalPlayerEntity, InvaderCharacterType);
        }
    }
    // Its death ends the invasion on the server (its vitals): if no end came after a while (the
    // vitals lost), it says it left.
    if (phase == InvaderTrip::Phase::GoingHome && going_home_ms >= 0 && !home_tells_server &&
        !late_cancel_sent && reason == bbcoop::InvasionEnd::NONE &&
        now_ms - going_home_ms > 10'000) {
        late_cancel_sent = true;
        SendInvadeRequest(true);
        LOG_INFO(Debug, "Co-op invasion: no end from the server after the death; told it");
    }
    // Home: out of the session and settled after the trip (or the trip never came): its own world
    // back, and the trip is over.
    if (phase == InvaderTrip::Phase::GoingHome && going_home_ms >= 0 && !frame.in_session &&
        ((frame.settled && now_ms - going_home_ms >= 5'000) || now_ms - going_home_ms > 120'000) &&
        RestoreInvasionHome(false)) {
        if (!home_tells_server && !late_cancel_sent && reason == bbcoop::InvasionEnd::NONE) {
            SendInvadeRequest(true); // home before the server's end came (see above)
        }
        std::scoped_lock lock{team_mutex};
        invader_trip.Home();
        invader_away.store(invader_trip.AwayFromHome());
        home_kept = false;
        going_home_ms = -1;
        late_cancel_sent = false;
        LOG_INFO(Debug, "Co-op invasion: home");
    }
}

} // namespace Coop

} // namespace Core::Bloodborne
