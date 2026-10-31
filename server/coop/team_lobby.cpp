// SPDX-FileCopyrightText: Copyright 2026 shadNet Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "coop/team_lobby.h"

#include <algorithm>
#include <array>
#include <map>
#include <random>
#include <set>
#include <vector>

#include "coop/team_code.h"

namespace BloodborneCoop {

namespace {

// Whether a repeated string field holds `value`.
template <typename Field>
bool Lists(const Field& field, const std::string& value) {
    return std::ranges::find(field, value) != field.end();
}

bool AnyPlays(const bbcoop::Team& team, const std::function<bool(const std::string&)>& plays) {
    return std::any_of(team.members().begin(), team.members().end(),
                       [&](const bbcoop::TeamMember& m) { return plays(m.member()); });
}

bool LaterFirst(const bbcoop::TeamWorldInfo& a, const bbcoop::TeamWorldInfo& b) {
    return a.played_ms() > b.played_ms();
}

// A character that played in `world`, once; whether it is new.
bool AddCharacter(bbcoop::TeamWorldInfo& world, const std::string& name) {
    if (name.empty() || std::ranges::find(world.characters(), name) != world.characters().end()) {
        return false;
    }
    world.add_characters(name);
    return true;
}

// A name as a player types it: ASCII letters in lower case, without the spaces around it.
std::string Folded(std::string_view name) {
    while (!name.empty() && name.front() == ' ') {
        name.remove_prefix(1);
    }
    while (!name.empty() && name.back() == ' ') {
        name.remove_suffix(1);
    }
    std::string out{name};
    for (char& c : out) {
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c - 'A' + 'a');
        }
    }
    return out;
}

std::string RandomCode() {
    // The system's entropy (/dev/urandom, BCryptGenRandom): a code must not be predictable from
    // the ones a player saw. 32 letters divide 2^32 evenly.
    static thread_local std::random_device device;
    std::string code;
    for (size_t at = 0; at < TeamCodeLength; ++at) {
        code += TeamCodeAlphabet[device() % TeamCodeAlphabet.size()];
    }
    return code;
}

} // namespace

u32 LampsLit(std::span<const s32> on) {
    constexpr s32 ActiveFlagBase = 10007810;
    return static_cast<u32>(std::ranges::count_if(on, [](s32 flag) {
        const s32 offset = flag - ActiveFlagBase;
        const s32 map = offset / 10000;
        const s32 within = offset % 10000;
        return offset >= 0 && map >= 210 && map < 400 && within % 20 == 0 && within / 20 < 20;
    }));
}

bool LampResetClears(s32 flag) {
    constexpr s32 ActiveFlagBase = 10007810;
    constexpr s32 LightRequestBase = 70000200;
    const auto lamp_of = [](s32 offset) {
        const s32 map = offset / 10000;
        const s32 within = offset % 10000;
        return offset >= 0 && map >= 211 && map < 400 && within < 400;
    };
    for (const s32 active : {flag, flag - 1}) {
        if (const s32 offset = active - ActiveFlagBase; lamp_of(offset) && offset % 20 == 0) {
            return true;
        }
    }
    const s32 request = flag - LightRequestBase;
    return lamp_of(request) && request % 10000 < 20;
}

TeamLobby::TeamLobby(std::function<std::string()> make_id_)
    : make_id{std::move(make_id_)}, make_code{RandomCode} {}

std::string TeamLobby::NewCode() {
    const auto taken = [this](const std::string& code) {
        return std::ranges::any_of(teams, [&](const bbcoop::Team& t) { return t.code() == code; });
    };
    std::string code = make_code();
    while (taken(code)) {
        code = make_code();
    }
    return code;
}

bool TeamLobby::MayLookUp(const std::string& member, s64 now_ms) {
    const auto found = wrong_lookups.find(member);
    if (found == wrong_lookups.end()) {
        return true;
    }
    std::erase_if(found->second, [&](s64 at) { return now_ms - at >= LookupWindowMs; });
    if (found->second.empty()) {
        wrong_lookups.erase(found);
        return true;
    }
    return found->second.size() < LookupTries;
}

void TeamLobby::WrongLookup(const std::string& member, s64 now_ms) {
    wrong_lookups[member].push_back(now_ms);
}

void TeamLobby::DropAsksBy(const std::string& member) {
    for (auto& [team, askers] : asks) {
        std::erase(askers, member);
    }
    std::erase_if(asks, [](const auto& entry) { return entry.second.empty(); });
}

void TeamLobby::AddMember(bbcoop::Team& team, const std::string& member) {
    bbcoop::TeamMember& m = *team.add_members();
    m.set_member(member);
    m.set_name(names[member]);
    GiveSigns(team);
    DropAsksBy(member); // in a team now: its asks to others are over
    if (const auto list = invites.find(member); list != invites.end()) {
        std::erase_if(list->second,
                      [&](const bbcoop::TeamInvite& i) { return i.team() == team.id(); });
    }
}

std::string TeamLobby::PartyOf(const std::string& world) {
    return "world:" + world;
}

bbcoop::Team* TeamLobby::FindTeam(const std::string& id) {
    for (bbcoop::Team& team : teams) {
        if (team.id() == id) {
            return &team;
        }
    }
    return nullptr;
}

void TeamLobby::Index() const {
    team_index.clear();
    for (size_t at = 0; at < teams.size(); ++at) {
        for (const bbcoop::TeamMember& m : teams[at].members()) {
            team_index.emplace(m.member(), at);
        }
    }
    indexed = true;
}

const bbcoop::Team* TeamLobby::TeamOf(const std::string& member) const {
    if (indexed) {
        const auto found = team_index.find(member);
        return found == team_index.end() ? nullptr : &teams[found->second];
    }
    for (const bbcoop::Team& team : teams) {
        for (const bbcoop::TeamMember& m : team.members()) {
            if (m.member() == member) {
                return &team;
            }
        }
    }
    return nullptr;
}

bbcoop::TeamMember* TeamLobby::MemberIn(bbcoop::Team& team, const std::string& member) {
    for (bbcoop::TeamMember& m : *team.mutable_members()) {
        if (m.member() == member) {
            return &m;
        }
    }
    return nullptr;
}

bbcoop::LobbyPlayer::State TeamLobby::StateOf(const std::string& member) const {
    const auto player = players.find(member);
    if (player == players.end()) {
        return bbcoop::LobbyPlayer::OFFLINE;
    }
    return player->second.playing ? bbcoop::LobbyPlayer::PLAYING : bbcoop::LobbyPlayer::TITLE;
}

bbcoop::TeamWorldInfo* TeamLobby::FindWorld(const std::string& id) {
    const auto found =
        std::ranges::find_if(worlds, [&](const bbcoop::TeamWorldInfo& w) { return w.id() == id; });
    return found != worlds.end() ? &*found : nullptr;
}

const bbcoop::Team* TeamLobby::PlayedBy(const std::string& world,
                                        const bbcoop::Team* except) const {
    for (const bbcoop::Team& team : teams) {
        if (&team != except && team.started() && team.world() == world) {
            return &team;
        }
    }
    return nullptr;
}

void TeamLobby::PickWorld(bbcoop::Team& team, const std::string& world) {
    team.set_world(world);
    team.set_party(world.empty() ? std::string{} : PartyOf(world));
}

void TeamLobby::KeepLastWorlds(const bbcoop::Team& started, std::vector<std::string>& forgotten,
                               std::vector<bbcoop::TeamWorldInfo>& forgotten_lines) {
    std::ranges::stable_sort(worlds, LaterFirst);
    const auto picked = [this](const std::string& id) {
        return std::ranges::any_of(teams, [&](const bbcoop::Team& t) { return t.world() == id; });
    };
    if (hosted) {
        // Each player's list is its own worlds (StateFor), none crowds out another's: a player
        // past its newest worlds_per_player leaves its oldest (one nobody keeps then goes), or
        // starting again and again would grow the list without end.
        // The worlds a member left here, as they were: one that goes aside keeps its players in
        // its line, so the operator's restore gives it back to them.
        std::map<std::string, bbcoop::TeamWorldInfo> left;
        for (const bbcoop::TeamMember& m : started.members()) {
            size_t kept = 0;
            for (bbcoop::TeamWorldInfo& world : worlds) {
                if (!Lists(world.members(), m.member()) || ++kept <= worlds_per_player ||
                    picked(world.id())) {
                    continue;
                }
                left.try_emplace(world.id(), world);
                auto& keepers = *world.mutable_members();
                keepers.erase(std::ranges::find(keepers, m.member()));
            }
        }
        for (size_t at = worlds.size(); at-- > 0;) {
            if (const auto line = left.find(worlds[at].id());
                worlds[at].members().empty() && line != left.end()) {
                forgotten.push_back(worlds[at].id());
                forgotten_lines.push_back(line->second);
                worlds.erase(worlds.begin() + static_cast<std::ptrdiff_t>(at));
            }
        }
        return;
    }
    for (size_t at = worlds.size(); worlds.size() > MaxWorlds && at-- > 0;) {
        if (!picked(worlds[at].id())) {
            forgotten.push_back(worlds[at].id());
            forgotten_lines.push_back(worlds[at]);
            worlds.erase(worlds.begin() + static_cast<std::ptrdiff_t>(at));
        }
    }
}

std::string TeamLobby::NewWorldId() {
    std::string id = make_id();
    while (FindWorld(id) || FindTeam(id)) {
        id = make_id();
    }
    return id;
}

std::vector<bbcoop::TeamWorldInfo> TeamLobby::TakeIdleWorlds(s64 before_ms) {
    std::vector<bbcoop::TeamWorldInfo> taken;
    std::erase_if(worlds, [&](const bbcoop::TeamWorldInfo& world) {
        const s64 last = world.played_ms() != 0 ? world.played_ms() : world.created_ms();
        if (last >= before_ms || std::ranges::any_of(teams, [&](const bbcoop::Team& team) {
                return team.world() == world.id();
            })) {
            return false;
        }
        taken.push_back(world);
        return true;
    });
    for (auto& [id, player] : players) {
        if (std::ranges::any_of(taken, [&](const auto& w) { return w.id() == player.world; })) {
            player.world.clear();
        }
    }
    return taken;
}

bool TeamLobby::AddWorld(bbcoop::TeamWorldInfo world) {
    if (world.id().empty() || FindWorld(world.id())) {
        return false;
    }
    worlds.push_back(std::move(world));
    std::ranges::stable_sort(worlds, LaterFirst);
    return true;
}

void TeamLobby::SetWorlds(std::vector<bbcoop::TeamWorldInfo> list) {
    worlds = std::move(list);
    std::ranges::stable_sort(worlds, LaterFirst);
}

bool TeamLobby::Plays(const std::string& member) const {
    return StateOf(member) == bbcoop::LobbyPlayer::PLAYING && !quiet_since.contains(member);
}

bool TeamLobby::OthersPlay(const bbcoop::Team& team, const std::string& member) const {
    return std::ranges::any_of(team.members(), [&](const bbcoop::TeamMember& m) {
        return m.member() != member && Plays(m.member());
    });
}

void TeamLobby::RemoveFrom(bbcoop::Team& team, const std::string& member) {
    auto* members = team.mutable_members();
    members->erase(
        std::remove_if(members->begin(), members->end(),
                       [&](const bbcoop::TeamMember& m) { return m.member() == member; }),
        members->end());
    if (team.leader() == member && !members->empty()) {
        const auto plays = std::ranges::find_if(
            *members, [this](const bbcoop::TeamMember& m) { return Plays(m.member()); });
        team.set_leader(plays != members->end() ? plays->member() : members->begin()->member());
        InvitesFromLeader(team);
    }
    GiveSigns(team);
    if (members->empty()) {
        DropTeam(team.id());
    }
}

TeamLobby::Departure TeamLobby::Depart(bbcoop::Team& team, const std::string& member) {
    Departure out{member, team.id(), team.party()};
    RemoveFrom(team, member);
    gone_since.erase(member);
    quiet_since.erase(member);
    if (bbcoop::Team* still = FindTeam(out.team)) {
        auto& list = invites[member];
        std::erase_if(list, [&](const bbcoop::TeamInvite& i) { return i.team() == out.team; });
        bbcoop::TeamInvite& invite = list.emplace_back();
        invite.set_team(out.team);
        invite.set_from(still->leader());
        invite.set_from_name(names[still->leader()]);
    }
    return out;
}

void TeamLobby::InvitesFromLeader(const bbcoop::Team& team) {
    for (auto& [to, list] : invites) {
        for (bbcoop::TeamInvite& invite : list) {
            if (invite.team() == team.id()) {
                invite.set_from(team.leader());
                invite.set_from_name(names[team.leader()]);
            }
        }
    }
}

TeamLobby::Outcome TeamLobby::Lead(const std::string& member) {
    bbcoop::Team* team = const_cast<bbcoop::Team*>(TeamOf(member));
    if (!team || team->leader() == member) {
        return {};
    }
    team->set_leader(member);
    GiveSigns(*team);
    InvitesFromLeader(*team);
    return {.changed = true};
}

void TeamLobby::Quiet(const std::string& member, bool quiet, s64 now_ms) {
    if (!quiet) {
        quiet_since.erase(member);
    } else if (players.contains(member)) {
        quiet_since.try_emplace(member, now_ms);
    }
}

void TeamLobby::DropTeam(const std::string& team_id) {
    const std::string id = team_id; // `team_id` may be the team's own, gone below
    if (const bbcoop::Team* team = FindTeam(id)) {
        for (const bbcoop::TeamMember& m : team->members()) {
            gone_since.erase(m.member());
            quiet_since.erase(m.member());
        }
    }
    std::erase_if(teams, [&](const bbcoop::Team& t) { return t.id() == id; });
    played.erase(id);
    asks.erase(id);
    for (auto& [to, list] : invites) {
        std::erase_if(list, [&](const bbcoop::TeamInvite& i) { return i.team() == id; });
    }
}

void TeamLobby::GiveSigns(bbcoop::Team& team) {
    constexpr std::array<u32, 3> GuestSigns{7, 6, 5};
    std::vector<u32> taken;
    for (bbcoop::TeamMember& m : *team.mutable_members()) {
        const bool guest_sign = std::ranges::find(GuestSigns, m.sign()) != GuestSigns.end();
        if (m.member() == team.leader() || !guest_sign ||
            std::ranges::find(taken, m.sign()) != taken.end()) {
            m.set_sign(0);
        } else {
            taken.push_back(m.sign());
        }
    }
    for (bbcoop::TeamMember& m : *team.mutable_members()) {
        if (m.member() == team.leader() || m.sign() != 0) {
            continue;
        }
        for (const u32 sign : GuestSigns) {
            if (std::ranges::find(taken, sign) == taken.end()) {
                m.set_sign(sign);
                taken.push_back(sign);
                break;
            }
        }
    }
}

void TeamLobby::Settle() {
    const auto plays = [this](const std::string& member) {
        return StateOf(member) == bbcoop::LobbyPlayer::PLAYING;
    };
    for (bbcoop::Team& team : teams) {
        if (team.started() && AnyPlays(team, plays)) {
            played.insert(team.id());
        } else if (team.started() && played.contains(team.id())) {
            // Back at the title, every one: the next session needs a Start again.
            played.erase(team.id());
            team.set_started(false);
            for (bbcoop::TeamMember& m : *team.mutable_members()) {
                m.set_ready(false);
            }
        }
    }
}

TeamLobby::Outcome TeamLobby::Arrive(const std::string& member, const std::string& name,
                                     bool playing) {
    Changed();
    const bool arrived = !players.contains(member);
    gone_since.erase(member);
    quiet_since.erase(member); // heard
    Player& player = players[member];
    // Phase 11: the character's name, when the game says one (playing); else the last one said.
    std::string& known = names[member];
    if (!name.empty()) {
        known = name;
    } else if (known.empty()) {
        known = "Hunter";
    }
    const bool changed = arrived || player.name != known || player.playing != playing;
    const bool was_playing = player.playing;
    player.name = known;
    player.playing = playing;
    for (bbcoop::Team& team : teams) {
        if (bbcoop::TeamMember* m = MemberIn(team, member)) {
            m->set_name(known);
        }
    }
    for (auto& [to, list] : invites) {
        for (bbcoop::TeamInvite& invite : list) {
            if (invite.from() == member) {
                invite.set_from_name(known);
            }
        }
    }
    // Kaniel 2026-10-17: the character a game plays in its team's world is one of its hunters (the
    // title's Ready names none).
    bool worlds_changed = false;
    if (const bbcoop::Team* team = TeamOf(member);
        playing && !name.empty() && team && team->started() && !team->world().empty()) {
        if (bbcoop::TeamWorldInfo* world = FindWorld(team->world())) {
            worlds_changed = AddCharacter(*world, name);
        }
    }
    std::vector<Departure> departed;
    if (was_playing && !playing) {
        // Back at the title: not ready any more (its game loads nothing now).
        for (bbcoop::Team& team : teams) {
            if (bbcoop::TeamMember* m = MemberIn(team, member)) {
                m->set_ready(false);
            }
        }
        // Kaniel 2026-10-25: and out of a started team another member still plays in.
        if (bbcoop::Team* team = const_cast<bbcoop::Team*>(TeamOf(member));
            team && team->started() && OthersPlay(*team, member)) {
            departed.push_back(Depart(*team, member));
        }
    }
    Settle();
    return {.changed = changed || worlds_changed || !departed.empty(),
            .departed = std::move(departed),
            .worlds_changed = worlds_changed};
}

TeamLobby::Outcome TeamLobby::Leave(const std::string& member) {
    Changed();
    if (players.erase(member) == 0) {
        return {};
    }
    DropAsksBy(member);
    for (bbcoop::Team& team : teams) {
        if (bbcoop::TeamMember* m = MemberIn(team, member)) {
            m->set_ready(false);
        }
    }
    // Kaniel 2026-10-17: a team lives while one of its members is here.
    std::vector<std::string> empty;
    for (const bbcoop::Team& team : teams) {
        if (std::ranges::none_of(team.members(), [this](const bbcoop::TeamMember& m) {
                return players.contains(m.member());
            })) {
            empty.push_back(team.id());
        }
    }
    for (const std::string& id : empty) {
        DropTeam(id);
    }
    Settle();
    return {.changed = true};
}

TeamLobby::Outcome TeamLobby::Expire(s64 now_ms) {
    Changed();
    std::vector<std::string> left;    // out of a team the others play on in
    std::vector<std::string> expired; // gone the whole grace
    for (const bbcoop::Team& team : teams) {
        for (const bbcoop::TeamMember& m : team.members()) {
            const bool signed_in = players.contains(m.member());
            const auto quiet = quiet_since.find(m.member());
            if (signed_in && quiet == quiet_since.end()) {
                continue;
            }
            const s64 gone =
                signed_in ? 0 : gone_since.try_emplace(m.member(), now_ms).first->second;
            const s64 since = quiet != quiet_since.end() ? quiet->second : gone;
            if (team.started() && OthersPlay(team, m.member()) && now_ms - since >= LeftGraceMs) {
                left.push_back(m.member());
            } else if (!signed_in && now_ms - gone >= OfflineGraceMs) {
                expired.push_back(m.member());
            }
        }
    }
    Outcome outcome;
    for (const std::string& member : left) {
        if (bbcoop::Team* team = const_cast<bbcoop::Team*>(TeamOf(member))) {
            outcome.departed.push_back(Depart(*team, member));
        }
    }
    for (const std::string& member : expired) {
        gone_since.erase(member);
        if (bbcoop::Team* team = const_cast<bbcoop::Team*>(TeamOf(member))) {
            outcome.departed.push_back({member, team->id(), team->party()});
            RemoveFrom(*team, member);
        }
    }
    std::erase_if(wrong_lookups, [&](const auto& tries) {
        return std::ranges::all_of(tries.second,
                                   [&](s64 at) { return now_ms - at >= LookupWindowMs; });
    });
    if (outcome.departed.empty()) {
        return {};
    }
    Settle();
    outcome.changed = true;
    return outcome;
}

TeamLobby::Outcome TeamLobby::Request(const std::string& member, const bbcoop::TeamRequest& request,
                                      s64 now_ms) {
    Changed();
    const auto refuse = [](std::string why) { return Outcome{.notice = std::move(why)}; };
    if (!players.contains(member)) {
        return refuse("not signed in to the lobby");
    }
    bbcoop::Team* mine = const_cast<bbcoop::Team*>(TeamOf(member));
    using Action = bbcoop::TeamRequest;
    switch (request.action()) {
    case Action::CREATE: {
        if (mine) {
            return refuse("you are in a team already: leave it first");
        }
        std::string id = make_id();
        while (FindTeam(id) || FindWorld(id)) {
            id = make_id();
        }
        bbcoop::Team& team = teams.emplace_back();
        team.set_id(id);
        team.set_leader(member);
        // A region: a team made again on the shard it moved to keeps its code (its members join it
        // with that one); only a ticket's request carries one.
        const std::string kept = NormalizeTeamCode(request.code());
        team.set_code(!kept.empty() &&
                              std::ranges::none_of(
                                  teams, [&](const bbcoop::Team& t) { return t.code() == kept; })
                          ? kept
                          : NewCode());
        bbcoop::TeamMember& m = *team.add_members();
        m.set_member(member);
        m.set_name(names[member]);
        // Kaniel 2026-10-17: the world its maker picked before it.
        if (const std::string& pick = players[member].world;
            !pick.empty() && FindWorld(pick) && !PlayedBy(pick, nullptr)) {
            PickWorld(team, pick);
        }
        *team.mutable_rules() = players[member].rules; // Kaniel 2026-10-25: its host rules
        return {.changed = true};
    }
    case Action::INVITE: {
        if (!mine) {
            return refuse("create a team first");
        }
        if (static_cast<size_t>(mine->members_size()) >= TeamSize) {
            return refuse("the team is full");
        }
        if (!players.contains(request.member())) {
            return refuse("that player is not online");
        }
        if (MemberIn(*mine, request.member())) {
            return refuse("already in your team");
        }
        auto& list = invites[request.member()];
        if (std::none_of(list.begin(), list.end(),
                         [&](const bbcoop::TeamInvite& i) { return i.team() == mine->id(); })) {
            bbcoop::TeamInvite& invite = list.emplace_back();
            invite.set_team(mine->id());
            invite.set_from(member);
            invite.set_from_name(names[member]);
        }
        return {.changed = true};
    }
    case Action::ACCEPT:
    case Action::DECLINE: {
        auto& list = invites[member];
        const auto invite = std::find_if(list.begin(), list.end(),
                                         [&](const auto& i) { return i.team() == request.team(); });
        if (invite == list.end()) {
            return refuse("no such invite");
        }
        if (request.action() == Action::DECLINE) {
            list.erase(invite);
            return {.changed = true};
        }
        if (mine) {
            return refuse("you are in a team already: leave it first");
        }
        if (const std::string& id = invite->team(); id.starts_with('@')) {
            // Another shard's team ("@<shard>/<code>"): the player goes there and joins it.
            const size_t slash = id.find('/');
            Move move{member, id.substr(1, slash - 1), {}};
            bbcoop::TeamRequest& join = move.requests.emplace_back();
            join.set_action(Action::JOIN_CODE);
            join.set_code(slash == std::string::npos ? std::string{} : id.substr(slash + 1));
            list.erase(invite);
            return {.changed = true, .moves = {std::move(move)}};
        }
        bbcoop::Team* team = FindTeam(request.team());
        if (!team) {
            list.erase(invite);
            return {.changed = true, .notice = "that team is gone"};
        }
        if (static_cast<size_t>(team->members_size()) >= TeamSize) {
            return refuse("the team is full");
        }
        AddMember(*team, member);
        return {.changed = true};
    }
    case Action::JOIN_CODE: {
        if (mine) {
            return refuse("you are in a team already: leave it first");
        }
        if (!MayLookUp(member, now_ms)) {
            return refuse("too many wrong codes or names: try again in a minute");
        }
        const std::string code = NormalizeTeamCode(request.code());
        const auto team = std::ranges::find_if(
            teams, [&](const bbcoop::Team& t) { return !code.empty() && t.code() == code; });
        if (team == teams.end()) {
            if (const std::string shard =
                    region.shard_of_code && !code.empty() ? region.shard_of_code(code) : "";
                !shard.empty() && shard != shard_name) {
                Move move{member, shard, {}};
                bbcoop::TeamRequest& join = move.requests.emplace_back();
                join.set_action(Action::JOIN_CODE);
                join.set_code(code);
                return {.moves = {std::move(move)}};
            }
            WrongLookup(member, now_ms);
            return refuse("no team has that code");
        }
        if (static_cast<size_t>(team->members_size()) >= TeamSize) {
            return refuse("the team is full");
        }
        AddMember(*team, member);
        return {.changed = true};
    }
    case Action::NEW_CODE: {
        if (!mine || mine->leader() != member) {
            return refuse("only the team's leader makes a new code");
        }
        mine->set_code(NewCode());
        return {.changed = true};
    }
    case Action::INVITE_NAME: {
        if (!mine) {
            return refuse("create a team first");
        }
        if (static_cast<size_t>(mine->members_size()) >= TeamSize) {
            return refuse("the team is full");
        }
        if (!MayLookUp(member, now_ms)) {
            return refuse("too many wrong codes or names: try again in a minute");
        }
        const std::string wanted = Folded(request.name());
        std::vector<std::string> found;
        for (const auto& [other, player] : players) {
            if (other != member && !wanted.empty() && Folded(player.name) == wanted) {
                found.push_back(other);
            }
        }
        std::vector<Elsewhere> far;
        if (found.empty() && region.named && !wanted.empty()) {
            far = region.named(wanted);
        }
        if (found.empty() && far.size() == 1) {
            RemoteInvite out{far.front().shard, far.front().member, {}};
            out.invite.set_team("@" + shard_name + "/" + mine->code());
            out.invite.set_from(member);
            out.invite.set_from_name(names[member]);
            return {.remote_invites = {std::move(out)}};
        }
        if (found.size() + far.size() != 1) {
            WrongLookup(member, now_ms);
            return refuse(found.empty() && far.empty()
                              ? "no hunter of that name is online"
                              : "several hunters have that name: give yours the team's code");
        }
        // The invite as if the lobby had listed it; then it is listed to the inviter (StateFor).
        bbcoop::TeamRequest invite;
        invite.set_action(Action::INVITE);
        invite.set_member(found.front());
        return Request(member, invite, now_ms);
    }
    case Action::LEAVE: {
        if (!mine) {
            return refuse("you are not in a team");
        }
        Departure out{member, mine->id(), mine->party()};
        RemoveFrom(*mine, member);
        return {.changed = true, .departed = {std::move(out)}};
    }
    case Action::REMOVE: {
        if (!mine || mine->leader() != member) {
            return refuse("only the team's leader removes members");
        }
        if (request.member() == member || !MemberIn(*mine, request.member())) {
            return refuse("not a member you can remove");
        }
        Departure out{request.member(), mine->id(), mine->party()};
        RemoveFrom(*mine, request.member());
        return {.changed = true, .departed = {std::move(out)}};
    }
    case Action::READY:
    case Action::UNREADY: {
        if (!mine) {
            return refuse("you are not in a team");
        }
        bbcoop::TeamMember& m = *MemberIn(*mine, member);
        const bool ready = request.action() == Action::READY;
        m.set_ready(ready);
        Outcome outcome{.changed = true};
        if (ready) {
            m.set_slot(request.slot());
            m.set_character(request.character());
            if (mine->started()) {
                outcome.starts.push_back(member); // the team plays: in at once
            }
        }
        return outcome;
    }
    case Action::WORLD: {
        if (!mine) {
            // Kaniel 2026-10-17: picked before the team: kept for the team this player makes.
            if (!request.world().empty() && !FindWorld(request.world())) {
                return refuse("the server has no such world");
            }
            std::string& pick = players[member].world;
            if (pick == request.world()) {
                return {};
            }
            pick = request.world();
            return {.changed = true};
        }
        if (mine->leader() != member) {
            return refuse("only the team's leader picks its world");
        }
        if (mine->started()) {
            return refuse("the team plays its world now");
        }
        if (!request.world().empty() && !FindWorld(request.world())) {
            return refuse("the server has no such world");
        }
        if (hosted && !request.world().empty() &&
            !Lists(FindWorld(request.world())->members(), member)) {
            return refuse("the server has no such world");
        }
        if (!request.world().empty() && PlayedBy(request.world(), mine)) {
            return refuse("another team plays that world now");
        }
        if (mine->world() == request.world()) {
            return {};
        }
        PickWorld(*mine, request.world());
        return {.changed = true};
    }
    case Action::RULES: {
        // Kaniel 2026-10-25: a leader's launcher rules are its team's; anyone's are kept for the
        // team it makes.
        players[member].rules = request.rules();
        if (!mine) {
            return {};
        }
        if (mine->leader() != member) {
            return refuse("only the team's leader sets its rules");
        }
        if (mine->rules().SerializeAsString() == request.rules().SerializeAsString()) {
            return {};
        }
        *mine->mutable_rules() = request.rules();
        return {.changed = true};
    }
    case Action::ROT: {
        if (!mine) {
            return refuse("you are not in a team");
        }
        if (static_cast<size_t>(request.rot_size()) > MaxRot) {
            return refuse("too much rot");
        }
        for (const u32 kind : request.rot()) {
            if (kind == bbcoop::ROT_NONE || !bbcoop::RotKind_IsValid(static_cast<int>(kind))) {
                return refuse("no such rot");
            }
        }
        bbcoop::TeamMember& m = *MemberIn(*mine, member);
        if (std::equal(m.rot().begin(), m.rot().end(), request.rot().begin(),
                       request.rot().end())) {
            return {};
        }
        *m.mutable_rot() = request.rot();
        return {.changed = true};
    }
    case Action::FORGET_WORLD: {
        // Kaniel 2026-10-17: the picker's way to shorten the list (the server keeps the folder).
        if (mine && mine->leader() != member) {
            return refuse("only a team's leader forgets a world"); // or one in no team
        }
        if (!FindWorld(request.world())) {
            return refuse("the server has no such world");
        }
        if (PlayedBy(request.world(), nullptr)) {
            return refuse("a team plays that world now");
        }
        if (hosted) {
            // Only its players forget it, each for itself; the last one's goes.
            auto& keepers = *FindWorld(request.world())->mutable_members();
            const auto mine = std::ranges::find(keepers, member);
            if (mine == keepers.end()) {
                return refuse("the server has no such world");
            }
            // The last one's goes, its line still naming it: restored, it is that player's
            // again (the others forgot it for themselves).
            if (keepers.size() > 1) {
                keepers.erase(mine);
                for (bbcoop::Team& team : teams) {
                    if (team.world() == request.world() && team.leader() == member) {
                        PickWorld(team, {});
                    }
                }
                if (const auto player = players.find(member);
                    player != players.end() && player->second.world == request.world()) {
                    player->second.world.clear();
                }
                return {.changed = true, .worlds_changed = true};
            }
        }
        const bbcoop::TeamWorldInfo line = *FindWorld(request.world());
        std::erase_if(worlds, [&](const auto& w) { return w.id() == request.world(); });
        for (bbcoop::Team& team : teams) {
            if (team.world() == request.world()) {
                PickWorld(team, {});
            }
        }
        for (auto& [id, player] : players) {
            if (player.world == request.world()) {
                player.world.clear();
            }
        }
        return {.changed = true, .forgotten = {request.world()}, .forgotten_lines = {line}};
    }
    case Action::RESET_LAMPS: {
        // Provi 2026-10-29: a world's own fix, as forgetting it is (the leader's, never in play).
        if (mine && mine->leader() != member) {
            return refuse("only a team's leader resets a world's lamps"); // or one in no team
        }
        bbcoop::TeamWorldInfo* const world = FindWorld(request.world());
        if (!world ||
            (hosted && std::ranges::find(world->members(), member) == world->members().end())) {
            return refuse("the server has no such world");
        }
        if (PlayedBy(request.world(), nullptr)) {
            return refuse("a team plays that world now");
        }
        world->set_lamps(0);
        return {.changed = true, .worlds_changed = true, .lamps_reset = {request.world()}};
    }
    case Action::INVADERS: {
        // Invasions (docs/INVASIONS.md): the leader's to allow; off ends a running invasion
        // (the service's).
        if (!mine || mine->leader() != member) {
            return refuse("only the team's leader allows invaders");
        }
        if (mine->allow_invaders() == request.allow()) {
            return {};
        }
        mine->set_allow_invaders(request.allow());
        return {.changed = true};
    }
    case Action::DEATH_MODE: {
        // Kaniel 2026-10-25: one mode for the whole team, so its members' deaths play out alike;
        // the leader may change it while the team plays (each game takes it on its next death).
        if (!mine) {
            return refuse("you are not in a team");
        }
        if (mine->leader() != member) {
            return refuse("only the team's leader picks how its deaths play out");
        }
        if (request.death_mode() > MaxDeathMode) {
            return refuse("no such death mode");
        }
        if (mine->death_mode() == request.death_mode()) {
            return {};
        }
        mine->set_death_mode(request.death_mode());
        return {.changed = true};
    }
    case Action::OPEN: {
        // Amanises 2026-10-25, open teams (matchmaking): the leader says how its team is joined.
        if (!mine || mine->leader() != member) {
            return refuse("only the team's leader says how the team is joined");
        }
        if (mine->join() == request.join()) {
            return {};
        }
        mine->set_join(request.join());
        if (request.join() == bbcoop::Team::INVITE_ONLY) {
            asks.erase(mine->id()); // not listed: nobody waits on it
        }
        return {.changed = true};
    }
    case Action::ASK_JOIN: {
        if (mine) {
            return refuse("you are in a team already: leave it first");
        }
        bbcoop::Team* team = FindTeam(request.team());
        if (!team || team->join() == bbcoop::Team::INVITE_ONLY) {
            return refuse("that team is not open any more");
        }
        if (static_cast<size_t>(team->members_size()) >= TeamSize) {
            return refuse("the team is full");
        }
        if (team->join() == bbcoop::Team::OPEN) {
            AddMember(*team, member); // open to all: in at once (its asks are over)
            return {.changed = true};
        }
        // One ask at a time: asking another team takes the last one back.
        DropAsksBy(member);
        asks[team->id()].push_back(member);
        return {.changed = true};
    }
    case Action::ASK_CANCEL: {
        DropAsksBy(member);
        return {.changed = true};
    }
    case Action::ASK_ACCEPT:
    case Action::ASK_DECLINE: {
        if (!mine || mine->leader() != member) {
            return refuse("only the team's leader answers who asked to join");
        }
        const auto found = asks.find(mine->id());
        if (found == asks.end() ||
            std::ranges::find(found->second, request.member()) == found->second.end()) {
            return refuse("that hunter did not ask to join");
        }
        if (request.action() == Action::ASK_DECLINE) {
            std::erase(found->second, request.member());
            if (found->second.empty()) {
                asks.erase(found);
            }
            return {.changed = true};
        }
        if (!players.contains(request.member()) || TeamOf(request.member())) {
            std::erase(found->second, request.member());
            if (found->second.empty()) {
                asks.erase(found);
            }
            return {.changed = true, .notice = "that hunter is not free to join now"};
        }
        if (static_cast<size_t>(mine->members_size()) >= TeamSize) {
            return refuse("the team is full");
        }
        AddMember(*mine, request.member());
        return {.changed = true};
    }
    case Action::START: {
        if (!mine || mine->leader() != member) {
            return refuse("only the team's leader starts");
        }
        if (!MemberIn(*mine, member)->ready()) {
            return refuse("choose your character first");
        }
        // Kaniel 2026-10-20: the leader begins as soon as the members are in (each member's game
        // says ready by itself); one not ready yet goes in once it is (a started team takes it).
        if (!mine->started() && !mine->world().empty() && PlayedBy(mine->world(), mine)) {
            return refuse("another team plays that world now");
        }
        // A region: the world's writer is another shard; the team goes there and starts again.
        if (const std::string holder =
                !mine->started() && !mine->world().empty() && region.world_holder
                    ? region.world_holder(mine->world())
                    : "";
            !holder.empty() && holder != shard_name) {
            Outcome outcome;
            for (const bbcoop::TeamMember& m : mine->members()) {
                Move& move = outcome.moves.emplace_back(Move{m.member(), holder, {}});
                if (m.member() == mine->leader()) {
                    bbcoop::TeamRequest& pick = move.requests.emplace_back();
                    pick.set_action(Action::WORLD);
                    pick.set_world(mine->world());
                    bbcoop::TeamRequest& create = move.requests.emplace_back();
                    create.set_action(Action::CREATE);
                    create.set_code(mine->code());
                } else {
                    bbcoop::TeamRequest& join = move.requests.emplace_back();
                    join.set_action(Action::JOIN_CODE);
                    join.set_code(mine->code());
                }
            }
            return outcome;
        }
        Outcome outcome{.changed = true};
        if (!mine->started()) {
            played.erase(mine->id()); // its games load: none plays yet
            // Kaniel 2026-10-16: the world picked, or a new one; it is the last played now (the
            // server notes the Start's time after this).
            if (mine->world().empty() || !FindWorld(mine->world())) {
                bbcoop::TeamWorldInfo& world = worlds.emplace_back();
                world.set_id(NewWorldId());
                PickWorld(*mine, world.id());
            }
            s64 latest = 0;
            for (const bbcoop::TeamWorldInfo& w : worlds) {
                latest = std::max<s64>(latest, w.played_ms());
            }
            bbcoop::TeamWorldInfo& world = *FindWorld(mine->world());
            world.set_played_ms(latest + 1);
            for (const bbcoop::TeamMember& m : mine->members()) {
                AddCharacter(world, m.character()); // each hunter's, as it readied
                if (hosted && !Lists(world.members(), m.member())) {
                    world.add_members(m.member()); // hosted: it is theirs now
                }
            }
            KeepLastWorlds(*mine, outcome.forgotten, outcome.forgotten_lines);
        }
        mine->set_started(true);
        for (const bbcoop::TeamMember& m : mine->members()) {
            if (m.ready() && players.contains(m.member())) {
                outcome.starts.push_back(m.member());
            }
        }
        return outcome;
    }
    default:
        return refuse("unknown request");
    }
}

TeamLobby::Outcome TeamLobby::InviteFromElsewhere(const std::string& member,
                                                  const bbcoop::TeamInvite& invite) {
    if (!players.contains(member) || !invite.team().starts_with('@')) {
        return {};
    }
    Changed();
    auto& list = invites[member];
    if (std::ranges::any_of(list, [&](const auto& i) { return i.team() == invite.team(); })) {
        return {};
    }
    list.push_back(invite);
    return {.changed = true};
}

std::vector<std::pair<std::string, std::string>> TeamLobby::Players() const {
    std::vector<std::pair<std::string, std::string>> out;
    out.reserve(players.size());
    for (const auto& [member, player] : players) {
        out.emplace_back(member, player.name);
    }
    return out;
}

std::vector<std::string> TeamLobby::Codes() const {
    std::vector<std::string> out;
    for (const bbcoop::Team& team : teams) {
        if (!team.code().empty()) {
            out.push_back(team.code());
        }
    }
    return out;
}

std::string TeamLobby::Fold(std::string_view name) {
    return Folded(name);
}

bbcoop::LobbyState TeamLobby::StateFor(const std::string& member) const {
    if (!indexed) {
        Index();
    }
    bbcoop::LobbyState state;
    const bbcoop::Team* mine = TeamOf(member);
    if (mine) {
        bbcoop::Team& out = *state.mutable_team();
        out = *mine;
        if (mine->leader() != member) {
            out.clear_code(); // the leader hands it out
        }
        for (bbcoop::TeamMember& m : *out.mutable_members()) {
            m.set_state(StateOf(m.member()));
        }
    }
    if (const auto list = invites.find(member); list != invites.end()) {
        for (const bbcoop::TeamInvite& invite : list->second) {
            *state.add_invites() = invite;
        }
    }
    const auto add_player = [&](const std::string& other, const Player& player) {
        bbcoop::LobbyPlayer& out = *state.add_players();
        out.set_member(other);
        out.set_name(player.name);
        out.set_state(StateOf(other));
        if (const bbcoop::Team* team = TeamOf(other)) {
            out.set_team(team->id());
        }
    };
    if (!hosted) {
        for (const auto& [other, player] : players) {
            if (other != member) {
                add_player(other, player);
            }
        }
    } else {
        // Its teammates, those who invited it and those its team invited, then players in no team
        // (to invite) up to lobby_players.
        std::set<std::string> listed{member};
        const auto list = [&](const std::string& other) {
            if (const auto player = players.find(other);
                player != players.end() && listed.insert(other).second) {
                add_player(other, player->second);
            }
        };
        if (mine) {
            for (const bbcoop::TeamMember& m : mine->members()) {
                list(m.member());
            }
        }
        for (const bbcoop::TeamInvite& invite : state.invites()) {
            list(invite.from());
        }
        if (mine) {
            for (const auto& [invited, list_of] : invites) {
                for (const bbcoop::TeamInvite& invite : list_of) {
                    if (invite.team() == mine->id()) {
                        list(invited);
                    }
                }
            }
        }
        size_t others = 0;
        for (const auto& [other, player] : players) {
            if (others >= lobby_players) {
                break;
            }
            if (!listed.contains(other) && !TeamOf(other)) {
                list(other);
                ++others;
            }
        }
    }
    size_t worlds_listed = 0;
    for (const bbcoop::TeamWorldInfo& world : worlds) {
        if (hosted) {
            if (worlds_listed >= worlds_per_player || !Lists(world.members(), member)) {
                continue;
            }
            ++worlds_listed;
        }
        bbcoop::TeamWorldInfo& out = *state.add_worlds();
        out = world;
        out.clear_members();
        if (const bbcoop::Team* team = PlayedBy(world.id(), nullptr)) {
            out.set_played_by(team->id());
        }
    }
    if (!state.has_team()) {
        if (const auto player = players.find(member); player != players.end()) {
            state.set_world(player->second.world);
        }
    }
    // Amanises 2026-10-25, open teams: the leader sees who asked; a player in no team the open
    // teams with room, the hunts not begun first.
    state.set_team_size(static_cast<u32>(TeamSize));
    if (mine && mine->leader() == member) {
        if (const auto found = asks.find(mine->id()); found != asks.end()) {
            for (const std::string& asker : found->second) {
                if (players.contains(asker) && !TeamOf(asker)) {
                    bbcoop::TeamAsk& ask = *state.add_asks();
                    ask.set_member(asker);
                    const auto name = names.find(asker);
                    ask.set_name(name != names.end() ? name->second : asker);
                }
            }
        }
    }
    if (!mine) {
        std::vector<const bbcoop::Team*> open;
        for (const bbcoop::Team& team : teams) {
            if (team.join() != bbcoop::Team::INVITE_ONLY &&
                static_cast<size_t>(team.members_size()) < TeamSize) {
                open.push_back(&team);
            }
        }
        std::ranges::stable_sort(open, [](const bbcoop::Team* a, const bbcoop::Team* b) {
            return !a->started() && b->started();
        });
        for (const bbcoop::Team* team : open) {
            if (static_cast<size_t>(state.open_teams_size()) >= OpenTeamsListed) {
                break;
            }
            bbcoop::OpenTeam& out = *state.add_open_teams();
            out.set_team(team->id());
            const auto name = names.find(team->leader());
            out.set_leader_name(name != names.end() ? name->second : team->leader());
            out.set_members(static_cast<u32>(team->members_size()));
            out.set_started(team->started());
            out.set_join(team->join());
            const auto found = asks.find(team->id());
            out.set_asked(found != asks.end() &&
                          std::ranges::find(found->second, member) != found->second.end());
        }
    }
    return state;
}

bbcoop::TeamStart TeamLobby::StartFor(const std::string& member) const {
    bbcoop::TeamStart start;
    const bbcoop::Team* team = TeamOf(member);
    if (!team) {
        return start;
    }
    start.set_team(team->id());
    start.set_party(team->party());
    start.set_world(team->world());
    start.set_host(team->leader());
    for (const bbcoop::TeamMember& m : team->members()) {
        if (m.member() == member) {
            start.set_slot(m.slot());
        }
    }
    return start;
}

std::vector<std::string> TeamLobby::SignedIn() const {
    std::vector<std::string> out;
    for (const auto& [member, player] : players) {
        out.push_back(member);
    }
    return out;
}

bool TeamLobby::NoteWorld(const bbcoop::TeamWorldInfo& note) {
    bbcoop::TeamWorldInfo* world = note.id().empty() ? nullptr : FindWorld(note.id());
    if (!world) {
        return false;
    }
    bool changed = false;
    if (!note.character().empty() && world->character() != note.character()) {
        world->set_character(note.character());
        changed = true;
    }
    if (note.flags_on() != 0 && world->flags_on() != note.flags_on()) {
        world->set_flags_on(note.flags_on());
        changed = true;
    }
    if (note.lamps() != 0 && world->lamps() != note.lamps()) {
        world->set_lamps(note.lamps());
        changed = true;
    }
    if (note.created_ms() != 0 && world->created_ms() == 0) {
        world->set_created_ms(note.created_ms());
        changed = true;
    }
    changed = AddCharacter(*world, note.character()) || changed;
    for (const std::string& name : note.characters()) {
        changed = AddCharacter(*world, name) || changed;
    }
    if (note.played_ms() != 0 && world->played_ms() != note.played_ms()) {
        world->set_played_ms(note.played_ms());
        std::ranges::stable_sort(worlds, LaterFirst);
        changed = true;
    }
    return changed;
}

} // namespace BloodborneCoop
