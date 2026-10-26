// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "core/bloodborne_team.h"

#include <algorithm>
#include <charconv>
#include <string_view>

#include "coop/team_code.h"
#include "core/bloodborne_flags.h"
#include "core/bloodborne_party.h"

namespace Core::Bloodborne {

std::string LinkRefusalNotice(std::string_view refusal) {
    if (refusal.empty()) {
        return {};
    }
    std::string notice;
    if (const size_t line_end = refusal.find('\n'); line_end != std::string_view::npos) {
        // The build check's refusal: its first line is the screen's (what differs, both values),
        // the rest the whole reason for the log.
        notice = refusal.substr(0, line_end);
    } else {
        // The details in brackets stay in the log: the screen has one short line.
        notice = "The co-op server refused this game: " +
                 std::string{refusal.substr(0, refusal.find(" ("))};
    }
    if (notice.size() > BloodborneCoop::RefusalNoticeMax) {
        notice = notice.substr(0, BloodborneCoop::RefusalNoticeMax - 3) + "...";
    }
    return notice;
}

std::string TeamScreenNotice(std::string_view lobby_notice, std::string_view link_warning) {
    if (!lobby_notice.empty()) {
        return std::string{lobby_notice};
    }
    std::string notice{link_warning.substr(0, link_warning.find('\n'))};
    if (notice.size() > BloodborneCoop::RefusalNoticeMax) {
        notice = notice.substr(0, BloodborneCoop::RefusalNoticeMax - 3) + "...";
    }
    return notice;
}

u32 TeamGuestSign(const bbcoop::LobbyState& lobby, const std::string& me,
                  const std::string& session_host) {
    constexpr u32 FirstGuestSign = 7;
    if (!lobby.has_team()) {
        return FirstGuestSign;
    }
    for (const bbcoop::TeamMember& m : lobby.team().members()) {
        if (m.member() == me && m.sign() != 0) {
            return m.sign(); // the server's (phase 7)
        }
    }
    if (!session_host.empty() && session_host != me) {
        for (const bbcoop::TeamMember& m : lobby.team().members()) {
            if (m.member() == session_host && m.sign() != 0) {
                return m.sign(); // the slot the session's host (once a guest) left
            }
        }
    }
    u32 guest = 0;
    for (const bbcoop::TeamMember& m : lobby.team().members()) {
        if (m.member() == lobby.team().leader()) {
            continue;
        }
        if (m.member() == me) {
            return FirstGuestSign - guest;
        }
        ++guest;
    }
    return FirstGuestSign;
}

std::vector<std::pair<std::string, std::string>> TeamLoadMembers(const bbcoop::LobbyState& lobby,
                                                                 const std::string& me) {
    std::vector<std::pair<std::string, std::string>> members;
    if (!lobby.has_team()) {
        return members;
    }
    for (const bbcoop::TeamMember& m : lobby.team().members()) {
        if (m.member() == me || (m.ready() && m.state() != bbcoop::LobbyPlayer::OFFLINE)) {
            members.emplace_back(m.member(), m.name());
        }
    }
    return members;
}

bool TeamCanBegin(const bbcoop::LobbyState& lobby, const std::string& me) {
    if (!lobby.has_team()) {
        return false;
    }
    if (lobby.team().started()) {
        return true;
    }
    return std::ranges::all_of(lobby.team().members(), [&](const bbcoop::TeamMember& m) {
        return m.member() == me || (m.ready() && m.state() != bbcoop::LobbyPlayer::OFFLINE);
    });
}

bool TeamMemberReady(const bbcoop::LobbyState& lobby, const std::string& me) {
    return lobby.has_team() &&
           std::ranges::any_of(lobby.team().members(), [&](const bbcoop::TeamMember& m) {
               return m.member() == me && m.ready();
           });
}

TeamGate::Decision TeamGate::Tick(const std::string& me, u32 slot_now, s64 now_ms) {
    Decision decision;
    if (!holding_since) {
        holding_since = now_ms;
    }
    slot = slot_now;
    if (started && !alone) {
        decision.start = started;
        return decision;
    }
    if (alone) {
        return decision;
    }
    decision.step = Step::Hold; // the team screen is up: the leader begins
    // Kaniel 2026-10-20: a member is ready by itself, with the save its screen is up for.
    if (lobby && lobby->has_team()) {
        const auto& members = lobby->team().members();
        const auto mine = std::ranges::find_if(
            members, [&](const bbcoop::TeamMember& m) { return m.member() == me; });
        if (mine != members.end() && (!mine->ready() || mine->slot() != slot_now) &&
            (!ready_said || now_ms - *ready_said >= ReadyAgainMs)) {
            ready_said = now_ms;
            decision.ready = true;
        }
    }
    return decision;
}

std::vector<bbcoop::TeamRequest> TeamGate::Choose(Choice choice, const std::string& me) {
    std::vector<bbcoop::TeamRequest> requests;
    switch (choice) {
    case Choice::Alone:
        alone = true;
        break;
    case Choice::Ready:
    case Choice::Start: {
        bbcoop::TeamRequest& ready = requests.emplace_back();
        ready.set_action(bbcoop::TeamRequest::READY);
        ready.set_slot(slot);
        if (choice == Choice::Start && lobby && lobby->has_team() && lobby->team().leader() == me) {
            requests.emplace_back().set_action(bbcoop::TeamRequest::START);
        }
        break;
    }
    }
    return requests;
}

void TeamGate::Reset() {
    started.reset();
    holding_since.reset();
    ready_said.reset();
    alone = false;
}

TeamPlayStep TeamPlayStepOf(const bbcoop::LobbyState* lobby, std::string_view party,
                            const bbcoop::TeamStart* start, const std::string& me) {
    if (lobby == nullptr) {
        return TeamPlayStep::None; // no word from the lobby yet
    }
    // Kaniel 2026-10-17: the world the team plays (a world of the server's, "world:<id>").
    const std::string mine = lobby->has_team() ? lobby->team().party() : std::string{};
    const auto in_mine = [&](std::string_view key) { return !mine.empty() && key == mine; };
    if (!party.empty()) {
        return in_mine(party) ? TeamPlayStep::None : TeamPlayStep::Leave;
    }
    if (start == nullptr || !in_mine(start->party())) {
        return TeamPlayStep::None;
    }
    const auto& members = lobby->team().members();
    const bool ready = std::ranges::any_of(
        members, [&](const bbcoop::TeamMember& m) { return m.member() == me && m.ready(); });
    return ready ? TeamPlayStep::Adopt : TeamPlayStep::None;
}

std::vector<bbcoop::TeamRequest> TeamJoinHuntRequests(const bbcoop::LobbyState& lobby,
                                                      const std::string& me,
                                                      const std::string& invite_team, u32 slot,
                                                      const std::string& character,
                                                      std::string_view world) {
    std::vector<bbcoop::TeamRequest> requests;
    if (!lobby.has_team()) {
        if (invite_team.empty()) {
            return requests;
        }
        bbcoop::TeamRequest& accept = requests.emplace_back();
        accept.set_action(bbcoop::TeamRequest::ACCEPT);
        accept.set_team(invite_team);
    }
    bbcoop::TeamRequest& ready = requests.emplace_back();
    ready.set_action(bbcoop::TeamRequest::READY);
    ready.set_slot(slot);
    ready.set_character(character);
    if (lobby.has_team() && lobby.team().leader() == me) {
        const std::string pick = DefaultTeamWorld(lobby, world, false);
        if (!lobby.team().started() && lobby.team().world() != pick) {
            bbcoop::TeamRequest& choose = requests.emplace_back();
            choose.set_action(bbcoop::TeamRequest::WORLD);
            choose.set_world(pick);
        }
        requests.emplace_back().set_action(bbcoop::TeamRequest::START);
    }
    return requests;
}

bbcoop::TeamRequest TeamMenuRequest(bbcoop::TeamRequest::Action action, const std::string& id) {
    using Action = bbcoop::TeamRequest;
    bbcoop::TeamRequest request;
    request.set_action(action);
    switch (action) {
    case Action::ACCEPT:
    case Action::DECLINE:
        request.set_team(id);
        break;
    case Action::WORLD:
    case Action::FORGET_WORLD:
        request.set_world(id);
        break;
    case Action::JOIN_CODE: {
        const std::string code = BloodborneCoop::NormalizeTeamCode(id);
        request.set_code(code.empty() ? id : code);
        break;
    }
    case Action::INVITE_NAME:
        request.set_name(id);
        break;
    case Action::NEW_CODE:
        break;
    case Action::DEATH_MODE:
        request.set_death_mode(id == "1" ? 1 : 0); // Kaniel 2026-10-25: "1" spectate, "0" down
        break;
    default:
        request.set_member(id);
        break;
    }
    return request;
}

std::string DefaultTeamWorld(const bbcoop::LobbyState& lobby, std::string_view hint, bool fresh) {
    if (fresh || hint.empty()) {
        return {};
    }
    const bool kept = std::ranges::any_of(
        lobby.worlds(), [&](const bbcoop::TeamWorldInfo& w) { return w.id() == hint; });
    return kept ? std::string{hint} : std::string{};
}

std::optional<std::string> TeamScreenWorldPick(const bbcoop::LobbyState& lobby,
                                               const std::string& me, std::string_view hint,
                                               bool fresh) {
    if (lobby.has_team() && (lobby.team().leader() != me || lobby.team().started())) {
        return std::nullopt;
    }
    const std::string pick = DefaultTeamWorld(lobby, hint, fresh);
    const std::string& now = lobby.has_team() ? lobby.team().world() : lobby.world();
    return now == pick ? std::nullopt : std::optional<std::string>{pick};
}

std::string TeamWorldName(const bbcoop::TeamWorldInfo& world) {
    if (!world.name().empty()) {
        return world.name();
    }
    return world.character().empty() ? "A world" : world.character() + "'s world";
}

DeathMode TeamDeathMode(const bbcoop::LobbyState& lobby) {
    return lobby.has_team() && lobby.team().death_mode() == static_cast<u32>(DeathMode::Spectate)
               ? DeathMode::Spectate
               : DeathMode::GoDown;
}

std::string TitleLoadCharacterName(bool fresh, std::string_view slot_name) {
    while (!slot_name.empty() && (slot_name.back() == '\n' || slot_name.back() == '\r')) {
        slot_name.remove_suffix(1);
    }
    // A character's name is at most 16 characters (UTF-8: count the lead bytes).
    const auto letters = std::ranges::count_if(
        slot_name, [](char c) { return (static_cast<unsigned char>(c) & 0xC0) != 0x80; });
    if (fresh || letters > 16) {
        return {};
    }
    return std::string{slot_name};
}

bool CharacterNameReadable(s64 title_load_ms, s64 save_opened_ms, s64 now_ms) {
    return title_load_ms == 0 ||
           (save_opened_ms > title_load_ms && now_ms - save_opened_ms >= CharacterNameSettleMs);
}

namespace {
s32 CountIn(std::span<const HeldGoods> list, u32 goods) {
    s32 count = 0;
    for (const HeldGoods& held : list) {
        if (held.goods == goods) {
            count += held.count;
        }
    }
    return count;
}
} // namespace

std::vector<HeldGoods> OwnKeyItemsForPhantom(std::span<const HeldGoods> own,
                                             std::span<const HeldGoods> phantom) {
    std::vector<HeldGoods> put;
    for (const HeldGoods& held : own) {
        if (IsBellGoods(held.goods) ||
            std::ranges::any_of(put, [&](const HeldGoods& p) { return p.goods == held.goods; })) {
            continue;
        }
        if (const s32 count = CountIn(own, held.goods) - CountIn(phantom, held.goods); count > 0) {
            put.push_back({held.goods, count});
        }
    }
    return put;
}

namespace {
thread_local u32 silent_item_gets = 0;
} // namespace

SilentItemGets::SilentItemGets() {
    ++silent_item_gets;
}

SilentItemGets::~SilentItemGets() {
    --silent_item_gets;
}

bool ItemGetsSilent() {
    return silent_item_gets > 0;
}

std::vector<HeldGoods> PhantomKeyItemsToGiveBack(std::span<const HeldGoods> first_look,
                                                 std::span<const HeldGoods> last_look,
                                                 std::span<const HeldGoods> home) {
    const auto count_in = [](std::span<const HeldGoods> list, u32 goods) {
        s32 count = 0;
        for (const HeldGoods& held : list) {
            if (held.goods == goods) {
                count += held.count;
            }
        }
        return count;
    };
    std::vector<HeldGoods> give;
    for (const HeldGoods& held : last_look) {
        if (IsBellGoods(held.goods)) {
            continue;
        }
        const s32 gained = held.count - count_in(first_look, held.goods);
        const s32 missing = held.count - count_in(home, held.goods);
        if (const s32 count = std::min(gained, missing); count > 0) {
            give.push_back({held.goods, count});
        }
    }
    return give;
}

bool IsWorldFlag(s32 flag) {
    // A map item pickup is the character's, as the item it gave (Kaniel's play test 2026-10-18:
    // the Workshop Tool's chest opened again after the host's save took a world without it).
    // A map's load-reset flags (FlagResetsOnLoad) neither: the team's world set Darkbeast Paarl's
    // fight going again after the wipe's reload (Kaniel's playtest 2026-10-22).
    const FlagRule rule = RuleFor(flag);
    return (rule.owner == FlagOwner::Host || rule.owner == FlagOwner::Shared) &&
           !rule.Has(FlagMapPickup) && !rule.Has(FlagResetsOnLoad);
}

namespace {

// The world flags on in one block.
void OnInBlock(u32 flags_per_block, const FlagBlockBits& block, std::vector<s32>& out,
               bool all = false) {
    for (size_t byte = 0; byte < block.bytes.size(); ++byte) {
        const u8 bits = block.bytes[byte];
        if (bits == 0) {
            continue;
        }
        for (u32 bit = 0; bit < 8; ++bit) {
            if ((bits & (0x80 >> bit)) == 0) {
                continue;
            }
            const s32 flag = static_cast<s32>(block.id * flags_per_block + byte * 8 + bit);
            if (all || IsWorldFlag(flag)) {
                out.push_back(flag);
            }
        }
    }
}

} // namespace

WorldFlags CaptureWorldFlags(u32 flags_per_block, std::span<const FlagBlockBits> blocks, bool all) {
    WorldFlags world;
    for (const FlagBlockBits& block : blocks) {
        world.blocks.push_back(block.id);
        OnInBlock(flags_per_block, block, world.on, all);
    }
    std::sort(world.blocks.begin(), world.blocks.end());
    std::sort(world.on.begin(), world.on.end());
    return world;
}

std::vector<std::pair<s32, bool>> WorldFlagChanges(u32 flags_per_block,
                                                   std::span<const FlagBlockBits> blocks,
                                                   const WorldFlags& target, bool all) {
    std::vector<std::pair<s32, bool>> changes;
    if (flags_per_block == 0) {
        return changes;
    }
    for (const FlagBlockBits& block : blocks) {
        if (!std::binary_search(target.blocks.begin(), target.blocks.end(), block.id)) {
            continue;
        }
        std::vector<s32> on;
        OnInBlock(flags_per_block, block, on, all);
        const s32 first = static_cast<s32>(block.id * flags_per_block);
        const s32 end = first + static_cast<s32>(block.bytes.size() * 8);
        const auto lo = std::lower_bound(target.on.begin(), target.on.end(), first);
        const auto hi = std::lower_bound(target.on.begin(), target.on.end(), end);
        std::vector<s32> wanted;
        std::copy_if(lo, hi, std::back_inserter(wanted),
                     [&](s32 flag) { return all || IsWorldFlag(flag); });
        std::vector<s32> off_now, on_now;
        std::set_difference(on.begin(), on.end(), wanted.begin(), wanted.end(),
                            std::back_inserter(off_now));
        std::set_difference(wanted.begin(), wanted.end(), on.begin(), on.end(),
                            std::back_inserter(on_now));
        for (const s32 flag : off_now) {
            changes.emplace_back(flag, false);
        }
        for (const s32 flag : on_now) {
            changes.emplace_back(flag, true);
        }
    }
    std::sort(changes.begin(), changes.end());
    return changes;
}

bool TakesTeamWorldBlock(u32 block, const std::set<u32>& taken, std::span<const u32> covered,
                         bool guest_trip) {
    return (guest_trip || !taken.contains(block)) &&
           std::binary_search(covered.begin(), covered.end(), block);
}

std::vector<s32> TeamWorldLamps(u32 flags_per_block, std::span<const u32> blocks,
                                const WorldFlags& target) {
    std::vector<s32> lamps;
    for (const s32 flag : target.on) {
        const u32 block = static_cast<u32>(flag) / flags_per_block;
        if (std::find(blocks.begin(), blocks.end(), block) == blocks.end()) {
            continue;
        }
        if (const auto lamp = LampOfActiveFlag(flag)) {
            lamps.push_back(*lamp);
        }
    }
    return lamps;
}

std::vector<s32> TeamLampFlagsToSet(const WorldFlags& team, const std::function<bool(s32)>& on) {
    std::vector<s32> flags;
    for (const s32 flag : team.on) {
        if ((LampOfActiveFlag(flag) || LampOfActiveFlag(flag - 1)) && !on(flag)) {
            flags.push_back(flag);
        }
    }
    return flags;
}

std::string FormatWorldFlags(const WorldFlags& world) {
    std::string text = "blocks\n";
    for (const u32 block : world.blocks) {
        text += std::to_string(block) + "\n";
    }
    text += "on\n";
    for (const s32 flag : world.on) {
        text += std::to_string(flag) + "\n";
    }
    return text + "end\n";
}

std::optional<WorldFlags> ParseWorldFlags(std::string_view text) {
    WorldFlags world;
    int part = 0; // 1 blocks, 2 on, 3 the end
    while (!text.empty()) {
        const auto end = text.find('\n');
        std::string_view line = text.substr(0, end);
        text = end == std::string_view::npos ? std::string_view{} : text.substr(end + 1);
        if (!line.empty() && line.back() == '\r') {
            line.remove_suffix(1);
        }
        if (line == "blocks") {
            part = 1;
            continue;
        }
        if (line == "on") {
            part = 2;
            continue;
        }
        if (line == "end" && part == 2) {
            part = 3;
            continue;
        }
        if (line.empty()) {
            continue;
        }
        s64 value = 0;
        const auto [parsed, error] = std::from_chars(line.data(), line.data() + line.size(), value);
        if (error != std::errc{} || parsed != line.data() + line.size() || part == 0 || part == 3) {
            return std::nullopt;
        }
        if (part == 1) {
            world.blocks.push_back(static_cast<u32>(value));
        } else {
            world.on.push_back(static_cast<s32>(value));
        }
    }
    if (part != 3) {
        return std::nullopt; // cut short
    }
    std::sort(world.blocks.begin(), world.blocks.end());
    std::sort(world.on.begin(), world.on.end());
    return world;
}

WorldFlags WorldFlagsOf(const bbcoop::TeamWorld& message) {
    WorldFlags world;
    world.on.assign(message.on().begin(), message.on().end());
    world.blocks.assign(message.blocks().begin(), message.blocks().end());
    std::sort(world.on.begin(), world.on.end());
    std::sort(world.blocks.begin(), world.blocks.end());
    return world;
}

std::vector<s32> TimeOfDayToFollow(const WorldFlags& team, const std::function<bool(s32)>& on) {
    s32 latest_own = 0;
    for (const s32 flag : TimeOfDayFlags) {
        if (on(flag)) {
            latest_own = flag;
        }
    }
    std::vector<s32> take;
    for (const s32 flag : TimeOfDayFlags) {
        if (flag > latest_own && std::binary_search(team.on.begin(), team.on.end(), flag)) {
            take.push_back(flag);
        }
    }
    return take;
}

std::vector<std::pair<s32, bool>> TeamTimeOfDayAtLoad(const WorldFlags& team, u32 flags_per_block,
                                                      const std::function<bool(s32)>& on) {
    std::vector<std::pair<s32, bool>> changes;
    const u32 block = flags_per_block != 0 ? u32(TimeOfDayFlags[0]) / flags_per_block : 0;
    if (flags_per_block == 0 ||
        !std::binary_search(team.blocks.begin(), team.blocks.end(), block)) {
        return changes;
    }
    for (const s32 flag : TimeOfDayFlags) {
        const bool team_on = std::binary_search(team.on.begin(), team.on.end(), flag);
        if (on(flag) != team_on) {
            changes.emplace_back(flag, team_on);
        }
    }
    return changes;
}

namespace {

bool IsTeamStoryFlag(s32 flag) {
    constexpr s32 MapsFirst = 12000000;
    constexpr s32 MapsLast = 13999999;
    constexpr s32 ItemsFirst = 50000000;
    constexpr s32 ItemsLast = 53999999;
    // Only the table's "every other flag" (docs/FLAGS.md): a flag with a row of its own has its
    // own way (9401 a guest sets itself, the per-player ones, the shared ones live already).
    return RowFor(flag) == nullptr && !(flag >= MapsFirst && flag <= MapsLast) &&
           !(flag >= ItemsFirst && flag <= ItemsLast) &&
           std::ranges::find(TimeOfDayFlags, flag) == TimeOfDayFlags.end();
}

} // namespace

std::vector<std::pair<s32, bool>> TeamStoryToFollow(const WorldFlags& before,
                                                    const WorldFlags& after) {
    if (before.blocks.empty() || before.blocks != after.blocks) {
        return {}; // nothing taken yet, or another store's world: loads take it
    }
    std::vector<std::pair<s32, bool>> follow;
    const auto add = [&](std::span<const s32> from, std::span<const s32> other, bool on) {
        for (const s32 flag : from) {
            if (IsTeamStoryFlag(flag) && !std::binary_search(other.begin(), other.end(), flag)) {
                follow.emplace_back(flag, on);
            }
        }
    };
    add(after.on, before.on, true);
    add(before.on, after.on, false);
    std::ranges::sort(follow);
    return follow;
}

bool KeptChangeYieldsToTeam(s32 flag, std::optional<s64> kept_ms, s64 now_ms,
                            std::span<const u32> team_blocks, u32 flags_per_block) {
    if (flags_per_block == 0 || flag < 0 || !IsTeamStoryFlag(flag)) {
        return false;
    }
    const u32 block = static_cast<u32>(flag) / flags_per_block;
    return std::find(team_blocks.begin(), team_blocks.end(), block) != team_blocks.end() &&
           (!kept_ms || now_ms - *kept_ms >= KeptChangeReportMs);
}

TeamJoin::Action TeamJoin::Tick(const Frame& frame) {
    if (!frame.host) {
        if (frame.matching_state == MatchingStateSummonedGuest && frame.session_members <= 1) {
            if (!alone_since) {
                alone_since = frame.now_ms;
            }
            if (frame.standing && frame.now_ms - *alone_since >= AloneLeaveMs) {
                alone_since.reset();
                return Action::LeaveAlone;
            }
        } else {
            alone_since.reset();
        }
        if (frame.matching_state == MatchingStateSummonedGuest && frame.room_owner &&
            frame.room_has_others) {
            return Action::TakeHost;
        }
        return frame.ready && frame.matching_state == 0 && frame.session_known ? Action::Join
                                                                               : Action::None;
    }
    if (frame.matching_state == MatchingStateHosting) {
        if (published) {
            return Action::None; // the server keeps it for the guests still out
        }
        published = true;
        return Action::Publish;
    }
    if (published) {
        published = false;
        return Action::PublishClosed;
    }
    return frame.ready && frame.matching_state == 0 ? Action::CreateSession : Action::None;
}

void TeamLoad::Start(s64 now_ms) {
    started_ms = now_ms;
    own = bbcoop::LoadProgress::LOADING;
    own_since_ms = now_ms;
    open = false;
    sent.reset();
}

bool CutsceneSkip::Press(bool covered, int step, s64 step_ms, s64 now_ms) {
    const bool playing =
        covered && (step == PrologueStep || step == PlayStep) && now_ms - step_ms <= LongestMs;
    if (!playing) {
        pressed_ms.reset();
        return false;
    }
    if (pressed_ms && now_ms - *pressed_ms < HoldMs) {
        return true;
    }
    if (presses >= MostPresses || (pressed_ms && now_ms - *pressed_ms < EveryMs)) {
        return false;
    }
    pressed_ms = now_ms;
    ++presses;
    return true;
}

CutsceneVote::Pad CutsceneVote::Tick(const Frame& frame) {
    if (!frame.watching) {
        *this = {};
        return Pad::Leave;
    }
    watching = true;
    voted = voted || frame.options;
    watchers = 1 + frame.others_watching;
    want = (voted ? 1 : 0) + std::min(frame.others_skip, frame.others_watching);
    if (want < watchers) {
        pressed_ms.reset();
        return Pad::Hide;
    }
    if (pressed_ms && frame.now_ms - *pressed_ms < HoldMs) {
        return Pad::Press;
    }
    if (pressed_ms && frame.now_ms - *pressed_ms < EveryMs) {
        return Pad::Hide;
    }
    pressed_ms = frame.now_ms;
    return Pad::Press;
}

TeamLoad::Stage TeamLoad::OwnStage(bool loading, bool in_session, size_t others_seen,
                                   size_t others_needed) {
    if (loading) {
        return bbcoop::LoadProgress::LOADING;
    }
    return in_session && others_seen >= others_needed ? bbcoop::LoadProgress::READY
                                                      : bbcoop::LoadProgress::JOINING;
}

size_t TeamLoad::OthersHere(std::span<const std::pair<std::string, std::string>> members,
                            std::string_view me, std::span<const std::pair<std::string, u32>> maps,
                            u32 map) {
    size_t here = 0;
    for (const auto& [member, name] : members) {
        if (member == me) {
            continue;
        }
        const auto at = std::ranges::find(maps, member, &std::pair<std::string, u32>::first);
        here += at == maps.end() || at->second == 0 || at->second == map ? 1 : 0;
    }
    return here;
}

void TeamLoad::SetStage(TeamLoad::Stage stage, s64 now_ms) {
    // Ready stays ready: a member who then leaves this map must not cover it again.
    if (own == bbcoop::LoadProgress::READY) {
        return;
    }
    if (stage != own) {
        own = stage;
        own_since_ms = now_ms;
    }
}

void TeamLoad::Take(const bbcoop::LoadProgress& other, s64 now_ms) {
    for (auto& [member, heard] : others) {
        if (member == other.member()) {
            heard = {other, now_ms};
            return;
        }
    }
    others.emplace_back(other.member(), Heard{other, now_ms});
}

namespace {

// A loading bar's share of the load: LoadedPercent over LoadingGuessMs, never quite all of it.
u32 LoadingPercent(s64 elapsed_ms) {
    const s64 share = std::clamp<s64>(elapsed_ms, 0, TeamLoad::LoadingGuessMs);
    return static_cast<u32>(5 + (TeamLoad::LoadedPercent - 10) * share / TeamLoad::LoadingGuessMs);
}

} // namespace

u32 TeamLoad::OwnPercent(s64 now_ms) const {
    switch (own) {
    case bbcoop::LoadProgress::LOADING:
        return LoadingPercent(now_ms - own_since_ms);
    case bbcoop::LoadProgress::JOINING:
        return TeamLoad::LoadedPercent +
               static_cast<u32>(std::clamp<s64>((now_ms - own_since_ms) / 500, 0, 25));
    case bbcoop::LoadProgress::READY:
        return 100;
    default:
        return 0;
    }
}

TeamLoad::View TeamLoad::Tick(std::span<const std::pair<std::string, std::string>> members,
                              const std::string& me, s64 now_ms) {
    View view;
    if (!started_ms || open) {
        return view;
    }
    bool all_ready = true;
    for (const auto& [member, name] : members) {
        Row row{.member = member, .name = name, .self = member == me};
        if (row.self) {
            row.stage = own;
            row.percent = OwnPercent(now_ms);
        } else {
            for (const auto& [who, heard] : others) {
                if (who == member) {
                    row.stage = heard.load.stage();
                    row.percent = heard.load.percent();
                    // Between its messages a load's bar goes on growing here too.
                    if (row.stage == bbcoop::LoadProgress::LOADING) {
                        row.percent =
                            std::max(row.percent, std::min(TeamLoad::LoadedPercent - 5,
                                                           LoadingPercent(now_ms - heard.at_ms) +
                                                               row.percent - 5));
                    }
                }
            }
        }
        all_ready = all_ready && row.stage == bbcoop::LoadProgress::READY;
        view.rows.push_back(std::move(row));
    }
    if (all_ready || now_ms - *started_ms >= LimitMs) {
        open = true;
        view.rows.clear();
        return view;
    }
    view.up = true;
    return view;
}

std::optional<bbcoop::LoadProgress> TeamLoad::ToSend(s64 now_ms) {
    if (!started_ms) {
        return std::nullopt;
    }
    const u32 percent = OwnPercent(now_ms);
    // On a stage's change, and every 10 points of a bar.
    if (sent && sent->first == own && percent < sent->second + 10) {
        return std::nullopt;
    }
    sent = std::pair{own, percent};
    bbcoop::LoadProgress load;
    load.set_stage(own);
    load.set_percent(percent);
    return load;
}

} // namespace Core::Bloodborne
