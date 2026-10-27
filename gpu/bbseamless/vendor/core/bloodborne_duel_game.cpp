// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// Kaniel 2026-10-25: team duels in the game (bloodborne_duel.h is the pure part). The game thread
// follows the server's duel, sends this player's requests, makes the foes' bodies hostile while it
// fights (the game's own hits then land, as an invader's), puts it out on a lethal hit, and gives
// back what the duel took once it is over. The port's team screen, boxes and overlay read it.

#include "core/bloodborne_re_internal.h"

#include <algorithm>
#include <atomic>
#include <deque>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include <fmt/format.h>
#include <fmt/ranges.h>
#include "common/logging/log.h"
#include "core/bloodborne_duel.h"
#include "core/bloodborne_trade.h"

namespace Core::Bloodborne {
namespace {

using BloodborneCoop::TradeItem;
using BloodborneCoop::TradeItemKind;

// Asked from other threads (the port's screens, the rig), sent by the game thread.
std::atomic<u32> propose_asks{0};
std::atomic<u32> cancel_asks{0};
std::mutex mutex;
std::vector<std::pair<u32, bool>> answers;
std::optional<std::pair<u32, std::string>> ask_now;
std::deque<Coop::DuelNotice> notices;
Coop::DuelView view_now;

// The party death hook's: whether a lethal hit now is a duel's, and its catches.
std::atomic<bool> catches_lethal{false};
std::atomic<u32> catches{0};

// The game thread's own.
DuelFollower follower;
// As this player joined the duel: its HP, its consumables and the effects on it.
std::optional<s32> kept_hp;
std::vector<HeldGoods> goods_before;
std::vector<s32> effects_before;
u32 seen_catches = 0;
bool out_here = false; // put out by this game (waiting for the server to say so)
u8 out_flags_added = 0;
struct TakenTeam {
    u32 handle = 0;
    s32 team = 0;
};
std::vector<TakenTeam> taken_teams; // foes' bodies made hostile, with their own teams
std::optional<s64> off_map_since;

// The data module +0x200 flags an out duelist wears (as a downed one: no HP loss, no hit's damage).
constexpr u8 OutNoDamageFlags = 0x2 | 0x8;
// A duelist's game away from the duel's map this long gives back by itself (a server that never
// says the end, its link gone).
constexpr s64 OffMapGiveBackMs = 10'000;

u64 PlayerData(u64 player) {
    const u64 modules = player >= 0x10000 ? ReadValue<u64>(player, 0x3B0) : 0;
    return modules >= 0x10000 ? ReadValue<u64>(modules, 0x20) : 0;
}

void Banner(s32 effect) {
    const auto* start = reinterpret_cast<const u8*>(image_base + SetTextEffectOffset);
    if (!std::ranges::equal(std::span<const u8>{start, SetTextEffectStart.size()},
                            SetTextEffectStart)) {
        return;
    }
    reinterpret_cast<void PS4_SYSV_ABI (*)(u64, s32)>(image_base + SetTextEffectOffset)(0, effect);
}

void Notice(Coop::DuelNoticeKind kind, const std::string& member = {}) {
    std::scoped_lock lock{mutex};
    if (notices.size() < 8) {
        notices.push_back({kind, member.empty() ? std::string{} : CharacterNameOf(member)});
    }
}

void Send(bbcoop::DuelRequest::Action action, u32 duel) {
    Coop::SendDuelRequest(action, duel, PlayingMap());
    LOG_INFO(Debug, "Co-op duel: {} for duel {}", bbcoop::DuelRequest::Action_Name(action), duel);
}

// The board name of another player's body: its network identity holds the member's online id.
std::optional<std::string> BodyMember(u64 character, const std::vector<std::string>& online_ids) {
    const u64 control = ReadValue<u64>(character, 0x60);
    if (control < 0x10000 || !HasMemoryAccess(control + 0xD0, sizeof(u64), MemoryProt::CpuRead)) {
        return std::nullopt;
    }
    const u64 network = ReadValue<u64>(control, 0xD0);
    return network >= 0x10000 ? Coop::SessionMemberBoardName(network, online_ids) : std::nullopt;
}

// The bodies of this player's foes in this world: by their identities, else by where the board
// says each member stands (DuelFoeBodies).
std::vector<u32> FoeBodies(std::span<const OtherPlayer> players, const Coop::VitalsView& view,
                           const std::vector<std::string>& foes) {
    std::vector<u32> hostile;
    if (foes.empty()) {
        return hostile;
    }
    std::vector<std::string> online_ids;
    for (const MemberVitals& member : view.board.members) {
        if (member.member != view.member) {
            if (std::string id = OnlineIdOfBoardMember(member.member); !id.empty()) {
                online_ids.push_back(std::move(id));
            }
        }
    }
    std::vector<DuelBody> unknown;
    for (const OtherPlayer& other : players) {
        const u32 handle = ReadValue<u32>(other.character, 0x08);
        if (const auto member = BodyMember(other.character, online_ids)) {
            if (std::ranges::find(foes, *member) != foes.end()) {
                hostile.push_back(handle);
            }
        } else if (other.at) {
            unknown.push_back(DuelBody{handle, *other.at});
        }
    }
    std::vector<DuelMemberAt> posted;
    const u32 map = PlayingMap();
    for (const MemberVitals& member : view.board.members) {
        if (member.member != view.member && member.map == map) {
            posted.push_back(DuelMemberAt{member.member, member.at});
        }
    }
    for (const u32 handle : DuelFoeBodies(unknown, posted, foes)) {
        hostile.push_back(handle);
    }
    return hostile;
}

// While this player fights: its foes' bodies on the invaders' team (the game rewrites a team on
// its own type changes, so every tick); the bodies no longer foes' get their own back.
void SetFoeTeams(std::span<const OtherPlayer> players, const std::vector<u32>& hostile) {
    for (const OtherPlayer& other : players) {
        const u32 handle = ReadValue<u32>(other.character, 0x08);
        if (!HasMemoryAccess(other.character + CharacterTeamOffset, 4, MemoryProt::CpuWrite)) {
            continue;
        }
        const s32 team = ReadValue<s32>(other.character, CharacterTeamOffset);
        const auto taken = std::ranges::find(taken_teams, handle, &TakenTeam::handle);
        const bool foe = std::ranges::find(hostile, handle) != hostile.end();
        if (foe && team != DownedTeams::Hostile && team != DownedTeams::Unchangeable) {
            if (taken == taken_teams.end()) {
                taken_teams.push_back(TakenTeam{handle, team});
                LOG_INFO(Debug, "Co-op duel: body {:#x} (team {}) is a foe's", handle, team);
            }
            WriteValue(other.character, CharacterTeamOffset, DownedTeams::Hostile);
        } else if (!foe && taken != taken_teams.end()) {
            if (team == DownedTeams::Hostile) {
                WriteValue(other.character, CharacterTeamOffset, taken->team);
            }
            LOG_INFO(Debug, "Co-op duel: body {:#x} takes its team {} back", handle, taken->team);
            taken_teams.erase(taken);
        }
    }
    // Bodies gone from this world are forgotten.
    std::erase_if(taken_teams, [&](const TakenTeam& taken) {
        return std::ranges::none_of(players, [&](const OtherPlayer& other) {
            return ReadValue<u32>(other.character, 0x08) == taken.handle;
        });
    });
}

void GoOut(u64 data, u32 duel) {
    out_here = true;
    const u8 flags = ReadValue<u8>(data, 0x200);
    out_flags_added = OutNoDamageFlags & static_cast<u8>(~flags);
    WriteValue(data, 0x200, static_cast<u8>(flags | OutNoDamageFlags));
    WriteValue(data, 0xF8, std::max(ReadValue<s32>(data, 0xF8), s32{1}));
    Banner(DuelOutTextEffect);
    Send(bbcoop::DuelRequest::DEFEATED, duel);
    LOG_INFO(Debug, "Co-op duel: this player is out of duel {} (no party death)", duel);
}

// The duel is over for this player: friendly again, and what it took given back.
void GiveBack(u64 player, u64 data, std::string_view why) {
    catches_lethal.store(false);
    out_here = false;
    if (data >= 0x10000 && out_flags_added != 0) {
        WriteValue(data, 0x200, static_cast<u8>(ReadValue<u8>(data, 0x200) & ~out_flags_added));
    }
    out_flags_added = 0;
    if (!kept_hp) {
        return;
    }
    if (data >= 0x10000) {
        const s32 hp = ReadValue<s32>(data, 0xF8);
        const auto back = DuelHpBack(*kept_hp, hp, ReadValue<s32>(data, 0xFC));
        if (back) {
            WriteValue(data, 0xF8, *back);
        }
        // Every consumable used comes back (Kaniel 2026-10-25), without the item popups.
        std::vector<TradeItem> items;
        std::vector<std::string> given;
        for (const HeldGoods& goods : DuelGoodsGiveBack(goods_before, Coop::HeldItemGoods())) {
            items.push_back(
                TradeItem{TradeItemKind::Goods, goods.goods, static_cast<u32>(goods.count)});
            given.push_back(fmt::format("{} x{}", goods.goods, goods.count));
        }
        if (!items.empty() && Coop::TradeItemsCanBeAdded()) {
            Coop::AddTradeItems(items, Coop::ItemPopup::Silent);
        }
        const std::vector<s32> ailments = DuelAilmentsToEnd(effects_before, Coop::LocalEffects());
        for (const s32 effect : ailments) {
            Coop::EndPlayerEffect(player, effect);
        }
        LOG_INFO(Debug, "Co-op duel: given back ({}): hp {} -> {}, goods [{}], ailments [{}]", why,
                 hp, back.value_or(hp), fmt::join(given, ", "), fmt::join(ailments, " "));
    }
    kept_hp.reset();
    goods_before.clear();
    effects_before.clear();
}

void TakeNews(const DuelNews& news, DuelPart before, std::string_view self, u64 player, u64 data) {
    const bbcoop::DuelState& state = follower.State();
    if (!news.refused.empty()) {
        Notice(follower.MayPropose() ? Coop::DuelNoticeKind::Alone : Coop::DuelNoticeKind::Busy);
    }
    if (news.joined && data >= 0x10000) {
        kept_hp = ReadValue<s32>(data, 0xF8);
        goods_before = Coop::HeldItemGoods();
        effects_before = Coop::LocalEffects();
        LOG_INFO(Debug, "Co-op duel: in duel {} (hp {}, {} kinds of goods kept)", state.duel(),
                 *kept_hp, goods_before.size());
    }
    if (news.fight) {
        LOG_INFO(Debug, "Co-op duel: duel {} fights", state.duel());
    }
    if (news.over) {
        LOG_INFO(Debug, "Co-op duel: duel {} is over: {}{}", state.duel(),
                 bbcoop::DuelState::End_Name(state.end()),
                 news.won    ? ", this player's side won"
                 : news.lost ? ", this player lost"
                             : "");
        if (news.won) {
            Banner(DuelWonTextEffect);
        }
        switch (state.end()) {
        case bbcoop::DuelState::CANCELLED:
            Notice(Coop::DuelNoticeKind::Cancelled, state.ended_by());
            break;
        case bbcoop::DuelState::LEFT:
            Notice(Coop::DuelNoticeKind::Left, state.ended_by());
            break;
        case bbcoop::DuelState::MAP:
            Notice(Coop::DuelNoticeKind::Moved, state.ended_by());
            break;
        default:
            break;
        }
        GiveBack(player, data, "the duel ended");
    }
    // The proposer hears of a duel that never started.
    if (before == DuelPart::Answered && state.stage() == bbcoop::DuelState::OVER &&
        state.proposer() == self &&
        (state.end() == bbcoop::DuelState::DECLINED ||
         state.end() == bbcoop::DuelState::NO_ANSWER)) {
        Notice(Coop::DuelNoticeKind::Declined);
    }
}

} // namespace

namespace Coop {

bool DuelCatchesLethal() {
    return catches_lethal.load(std::memory_order_relaxed);
}

void NoteDuelCatch() {
    catches.fetch_add(1, std::memory_order_relaxed);
}

DuelView ReadDuel() {
    std::scoped_lock lock{mutex};
    return view_now;
}

void ProposeDuel() {
    propose_asks.fetch_add(1);
}

void CancelDuel() {
    cancel_asks.fetch_add(1);
}

std::optional<std::pair<u32, std::string>> DuelAsk() {
    std::scoped_lock lock{mutex};
    return ask_now;
}

bool DuelAskOpen(u32 duel) {
    std::scoped_lock lock{mutex};
    return ask_now && ask_now->first == duel;
}

void AnswerDuel(u32 duel, bool accept) {
    std::scoped_lock lock{mutex};
    if (ask_now && ask_now->first == duel) {
        ask_now.reset();
        answers.emplace_back(duel, accept);
    }
}

std::optional<DuelNotice> NextDuelNotice() {
    std::scoped_lock lock{mutex};
    if (notices.empty()) {
        return std::nullopt;
    }
    DuelNotice notice = std::move(notices.front());
    notices.pop_front();
    return notice;
}

void TickDuel() {
    const u64 player = GetLocalPlayer();
    const u64 data = PlayerData(player);
    const bool playing = data >= 0x10000 && HasMemoryAccess(data, 0x204, MemoryProt::CpuWrite);
    const VitalsView view = ReadVitalsBoard();
    const std::string& self = view.member;
    for (const bbcoop::DuelState& state : TakeDuelStates()) {
        const DuelPart before = follower.Part();
        TakeNews(follower.Take(state, self, SteadyMilliseconds()), before, self, player,
                 playing ? data : 0);
    }
    const bbcoop::DuelState& state = follower.State();
    const s64 now_ms = SteadyMilliseconds();

    // This player's requests.
    std::vector<std::pair<u32, bool>> answered;
    {
        std::scoped_lock lock{mutex};
        answered.swap(answers);
    }
    for (const auto& [duel, accept] : answered) {
        Send(accept ? bbcoop::DuelRequest::ACCEPT : bbcoop::DuelRequest::DECLINE, duel);
    }
    if (propose_asks.exchange(0) > 0) {
        Send(bbcoop::DuelRequest::PROPOSE, 0);
    }
    if (cancel_asks.exchange(0) > 0 && follower.InDuel()) {
        Send(bbcoop::DuelRequest::CANCEL, state.duel());
    }

    // A duelist away from the duel's map (a trip, a load) gives back by itself if the server's
    // end never comes.
    const u32 map = PlayingMap();
    if (follower.InDuel() && map != state.map()) {
        off_map_since = off_map_since.value_or(now_ms);
        if (now_ms - *off_map_since >= OffMapGiveBackMs) {
            LOG_INFO(Debug, "Co-op duel: off duel {}'s map for {} s: over here", state.duel(),
                     OffMapGiveBackMs / 1000);
            follower.Forget();
            GiveBack(player, playing ? data : 0, "off the duel's map");
        }
    } else {
        off_map_since.reset();
    }

    // Fighting: lethal hits put this player out; its foes' bodies are hostile.
    const bool fighting = follower.Part() == DuelPart::Fighting && !out_here;
    catches_lethal.store(fighting && playing);
    if (const u32 caught = catches.load(); caught != seen_catches) {
        seen_catches = caught;
        if (fighting && playing) {
            GoOut(data, state.duel());
        }
    }
    if (follower.Part() == DuelPart::Out && playing && out_flags_added == 0 && !out_here) {
        out_here = true; // put out by the server (its game came back): stays safe until the end
    }
    if (out_here && playing && ReadValue<s32>(data, 0xF8) < 1) {
        WriteValue(data, 0xF8, s32{1});
    }
    const std::vector<OtherPlayer> players = OtherPlayers();
    const std::vector<u32> hostile = fighting && playing && InPartySession()
                                         ? FoeBodies(players, view, follower.Foes())
                                         : std::vector<u32>{};
    SetFoeTeams(players, hostile);

    // What the port shows.
    DuelView shown;
    shown.part = follower.Part();
    shown.stage = state.stage();
    shown.seconds_left = static_cast<float>(state.left_ms()) / 1000.0f;
    // Kaniel 2026-10-25: 3, 2, 1 and the fight, on this game's clock from the server's time left.
    if (follower.Part() == DuelPart::Countdown || follower.Part() == DuelPart::Fighting) {
        shown.countdown = DuelCountdownAt(follower.CountdownUntil(), follower.FightSince(), now_ms);
    }
    // Each step of it once in the log (the rig checks them).
    static int counted = -1;
    if (const int step = !shown.countdown.shown  ? -1
                         : shown.countdown.fight ? 0
                                                 : shown.countdown.number;
        step != counted) {
        counted = step;
        if (step >= 0) {
            LOG_INFO(Debug, "Co-op duel: countdown {} (bar {:.2f})",
                     step == 0 ? std::string{"fight"} : std::to_string(step), shown.countdown.fill);
        }
    }
    shown.may_cancel = follower.InDuel().has_value() || follower.Part() == DuelPart::Answered;
    shown.may_propose = follower.MayPropose() && InPartySession() && map != 0 &&
                        std::ranges::any_of(view.board.members, [&](const MemberVitals& member) {
                            return member.member != self && member.map == map;
                        });
    for (const bbcoop::Duelist& duelist : state.duelists()) {
        shown.fighters += duelist.answer() == 1 && !duelist.out() ? 1 : 0;
    }
    std::scoped_lock lock{mutex};
    view_now = shown;
    if (const auto asked = follower.AskedDuel()) {
        if (!ask_now || ask_now->first != *asked) {
            ask_now = std::pair{*asked, CharacterNameOf(state.proposer())};
        }
    } else {
        ask_now.reset();
    }
}

} // namespace Coop
} // namespace Core::Bloodborne
