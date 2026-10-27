// SPDX-FileCopyrightText: Copyright 2026 shadNet Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "coop/trade.h"

#include <algorithm>

namespace BloodborneCoop {

const TradeGood* FindTradeGood(TradeItemKind kind, u32 id) {
    for (const TradeGood& good : TradeGoods) {
        if (good.kind == kind && good.id == id) {
            return &good;
        }
    }
    return nullptr;
}

namespace {

// Whether the game has such an item, and how many may go at once.
std::optional<std::string> ItemProblem(const TradeItem& item) {
    u32 limit = 0;
    switch (item.kind) {
    case TradeItemKind::Echoes:
        if (item.id != 0) {
            return "not an item the game has";
        }
        limit = MaxTradeEchoes;
        break;
    case TradeItemKind::Goods:
        if (item.id == 0 || item.id > MaxTradeGoodsId) {
            return "not an item the game has";
        }
        limit = MaxTradeGoods;
        break;
    case TradeItemKind::Item: {
        const u32 category = item.id & 0xF0000000;
        if ((category != WeaponCategory && category != ArmorCategory &&
             category != AccessoryCategory && category != GemCategory) ||
            (item.id & 0x0FFFFFFF) == 0x0FFFFFFF) {
            return "not an item the game has";
        }
        limit = 1; // each its own
        break;
    }
    default:
        return "not an item the game has";
    }
    if (item.count == 0 || item.count > limit) {
        return "too many or none";
    }
    if (item.name.size() > MaxTradeItemName || item.data.size() > MaxTradeItemData) {
        return "an item too long to say";
    }
    return std::nullopt;
}

// A malformed client claim is not an inventory we may safely recreate. Return only valid,
// distinct ordinary items, including on late answers and refused offers; never mint Insight
// or an unknown kind by calling it a refund.
TradeItems RefundableItems(std::span<const TradeItem> items) {
    TradeItems safe;
    for (const TradeItem& item : items) {
        if (!ItemProblem(item) && !std::ranges::any_of(safe, [&](const TradeItem& other) {
                return item.kind == other.kind && item.id == other.id && item.data == other.data;
            })) {
            safe.push_back(item);
        }
    }
    return safe;
}

} // namespace

std::optional<std::string> TradeItemsProblem(std::span<const TradeItem> items, bool allow_empty) {
    if (items.empty()) {
        return allow_empty ? std::nullopt : std::optional<std::string>{"nothing offered"};
    }
    for (size_t index = 0; index < items.size(); ++index) {
        const TradeItem& item = items[index];
        if (std::optional<std::string> problem = ItemProblem(item)) {
            return problem;
        }
        for (size_t other = 0; other < index; ++other) {
            if (items[other].kind == item.kind && items[other].id == item.id &&
                items[other].data == item.data) {
                return "an item twice";
            }
        }
    }
    return std::nullopt;
}

std::optional<std::string> TradeGrantItemsProblem(const bbcoop::TradeGrant& grant) {
    const TradeItems items = TradeItemsOf(grant.items());
    const auto reason = static_cast<TradeGrantReason>(grant.reason());
    if (reason == TradeGrantReason::Received || reason == TradeGrantReason::Returned) {
        if ((grant.id() & InvasionRewardGrantBit) != 0) {
            return "an ordinary grant with a reward id";
        }
        return TradeItemsProblem(items, false);
    }
    if (reason != TradeGrantReason::InvasionReward ||
        (grant.id() & InvasionRewardGrantBit) == 0 || items.empty() || items.size() > 3) {
        return "not a known server grant";
    }
    bool insight = false;
    bool echoes = false;
    bool blood_dreg = false;
    for (const TradeItem& item : items) {
        if (!item.name.empty() || !item.data.empty()) {
            return "not an invasion reward";
        }
        if (item.kind == TradeItemKind::Insight && item.id == 0 && item.count == 1 && !insight) {
            insight = true;
        } else if (item.kind == TradeItemKind::Echoes && item.id == 0 && item.count > 0 &&
                   item.count <= MaxTradeEchoes && !echoes) {
            echoes = true;
        } else if (item.kind == TradeItemKind::Goods && item.id == BloodDregGoods &&
                   item.count == 1 && !blood_dreg) {
            blood_dreg = true;
        } else {
            return "not an invasion reward";
        }
    }
    return insight ? std::nullopt : std::optional<std::string>{"no victory Insight"};
}

namespace {

std::string Thousands(u32 value) {
    std::string digits = std::to_string(value);
    for (int at = static_cast<int>(digits.size()) - 3; at > 0; at -= 3) {
        digits.insert(static_cast<size_t>(at), ",");
    }
    return digits;
}

} // namespace

std::string TradeItemsText(std::span<const TradeItem> items) {
    std::string text;
    for (const TradeItem& item : items) {
        if (!text.empty()) {
            text += ", ";
        }
        if (const TradeGood* good = FindTradeGood(item.kind, item.id)) {
            text += Thousands(item.count) + " " + (item.count == 1 ? good->name : good->plural);
            continue;
        }
        // Others by the giver's game's name (no plurals known): "Molotov Cocktail x3".
        text += item.name.empty() ? std::string{"an item"} : item.name;
        if (item.count != 1) {
            text += " x" + Thousands(item.count);
        }
    }
    return text;
}

bbcoop::TradeItem ToMessage(const TradeItem& item) {
    bbcoop::TradeItem message;
    message.set_kind(static_cast<u32>(item.kind));
    message.set_id(item.id);
    message.set_count(item.count);
    message.set_name(item.name);
    message.set_data(item.data);
    return message;
}

TradeItem TradeItemOf(const bbcoop::TradeItem& message) {
    return {static_cast<TradeItemKind>(message.kind()), message.id(), message.count(),
            message.name(), message.data()};
}

TradeItems TradeItemsOf(const google::protobuf::RepeatedPtrField<bbcoop::TradeItem>& items) {
    TradeItems result;
    result.reserve(static_cast<size_t>(items.size()));
    for (const bbcoop::TradeItem& item : items) {
        result.push_back(TradeItemOf(item));
    }
    return result;
}

void PutTradeItems(std::span<const TradeItem> items,
                   google::protobuf::RepeatedPtrField<bbcoop::TradeItem>* out) {
    out->Clear();
    for (const TradeItem& item : items) {
        *out->Add() = ToMessage(item);
    }
}

std::string TradeAskText(const std::string& from_name, std::span<const TradeItem> give,
                         std::span<const TradeItem> want) {
    std::string text = from_name + " offers you " + TradeItemsText(give);
    if (!want.empty()) {
        text += "\nfor " + TradeItemsText(want);
    }
    return text + ".\nAccept?";
}

std::string TradeGiveQuestion(const TradeItem& item, const std::string& to_name) {
    const TradeGood* good = FindTradeGood(item.kind, item.id);
    const std::string what = good != nullptr      ? good->plural
                             : !item.name.empty() ? item.name
                                                  : "items";
    return "Select quantity of " + what + " to give to " + to_name + ".";
}

TradeEchoesPicker TradeEchoesPickerFor(u32 held) {
    // The power of ten below a hundredth of what the player holds (1 under 100).
    const u32 most = std::min(held, MaxTradeEchoes);
    u32 step = 1;
    while (most / step >= 100) {
        step *= 10;
    }
    return {step, step * 10};
}

std::string TradeGiveEchoesQuestion(u32 held, const std::string& to_name) {
    return "Give Blood Echoes to " + to_name + " (you hold " + Thousands(held) +
           ").\nUp/Down: " + Thousands(TradeEchoesPickerFor(held).step) +
           " at a time, up to all of them.";
}

std::string TradeNoticeText(const TradeClient::Notice& notice) {
    using Kind = TradeClient::Notice::Kind;
    const std::string items = TradeItemsText(notice.items);
    const bool one = notice.items.size() == 1 && notice.items.front().count == 1;
    const std::string back = one ? "It was returned to you." : "They were returned to you.";
    switch (notice.kind) {
    case Kind::Accepted:
        return notice.who + " accepted your " + items + ".";
    case Kind::Declined:
        return notice.who + " declined your " + items + ".\n" + back;
    case Kind::Expired:
        return notice.who + " did not answer your offer of " + items + ".\n" + back;
    case Kind::Refused:
        return "Could not offer " + notice.who + " " + items + ": " + notice.text + ".\n" + back;
    case Kind::Received: {
        // Goods come with the game's own item popup; echoes have none.
        TradeItems echoes;
        for (const TradeItem& item : notice.items) {
            if (item.kind == TradeItemKind::Echoes) {
                echoes.push_back(item);
            }
        }
        return echoes.empty() ? std::string{}
                              : "Received " + TradeItemsText(echoes) + " from " + notice.who + ".";
    }
    case Kind::Returned:
        return {}; // the declined, expired or refused notice says so
    }
    return {};
}

bool IsTradeMessage(const bbcoop::Envelope& envelope) {
    switch (envelope.body_case()) {
    case bbcoop::Envelope::kTradeOffer:
    case bbcoop::Envelope::kTradeStatus:
    case bbcoop::Envelope::kTradeAsk:
    case bbcoop::Envelope::kTradeAnswer:
    case bbcoop::Envelope::kTradeCancel:
    case bbcoop::Envelope::kTradeClosed:
    case bbcoop::Envelope::kTradeGrant:
    case bbcoop::Envelope::kTradeGranted:
        return true;
    default:
        return false;
    }
}

// TradeDesk

void TradeDesk::Grant(Result& result, const std::string& member, u64 trade, const std::string& from,
                      const std::string& from_name, TradeItems items, TradeGrantReason reason) {
    if (items.empty()) {
        return;
    }
    bbcoop::TradeGrant grant;
    const u64 id = next_id++;
    grant.set_id(id | (reason == TradeGrantReason::InvasionReward ? InvasionRewardGrantBit : 0));
    grant.set_trade(trade);
    grant.set_from(from);
    grant.set_from_name(from_name);
    PutTradeItems(items, grant.mutable_items());
    grant.set_reason(static_cast<u32>(reason));
    grants[member][grant.id()] = grant;
    bbcoop::Envelope envelope;
    *envelope.mutable_trade_grant() = grant;
    result.sends.push_back({member, std::move(envelope)});
    result.log.push_back("trade " + std::to_string(trade) + ": " + TradeItemsText(items) +
                         (reason == TradeGrantReason::Returned ? " back to " : " to ") + member);
    result.changed = true;
}

void TradeDesk::Status(Result& result, const Open& trade, u64 id, TradeState state,
                       std::string text) {
    bbcoop::Envelope envelope;
    bbcoop::TradeStatus& status = *envelope.mutable_trade_status();
    status.set_number(trade.number);
    status.set_id(id);
    status.set_state(static_cast<u32>(state));
    status.set_text(std::move(text));
    result.sends.push_back({trade.from, std::move(envelope)});
}

bbcoop::Envelope TradeDesk::AskOf(u64 id, const Open& trade, s64 now_ms) const {
    bbcoop::Envelope envelope;
    bbcoop::TradeAsk& ask = *envelope.mutable_trade_ask();
    ask.set_id(id);
    ask.set_from(trade.from);
    ask.set_from_name(trade.from_name);
    PutTradeItems(trade.give, ask.mutable_give());
    PutTradeItems(trade.want, ask.mutable_want());
    ask.set_expires_in_ms(static_cast<u32>(std::max<s64>(trade.expires_ms - now_ms, 0)));
    return envelope;
}

// An open trade ends without an exchange: the offer goes back to the giver, the question away.
void TradeDesk::Close(Result& result, u64 id, TradeState state) {
    const auto found = open.find(id);
    if (found == open.end()) {
        return;
    }
    const Open trade = found->second;
    open.erase(found);
    Status(result, trade, id, state);
    bbcoop::Envelope closed;
    closed.mutable_trade_closed()->set_id(id);
    result.sends.push_back({trade.to, std::move(closed)});
    static constexpr const char* States[] = {"",        "open",    "accepted", "declined",
                                             "expired", "refused", "withdrawn"};
    result.log.push_back("trade " + std::to_string(id) + " from " + trade.from + " to " + trade.to +
                         ": " + States[static_cast<u32>(state)]);
    Grant(result, trade.from, id, trade.to, trade.to_name, trade.give, TradeGrantReason::Returned);
    result.changed = true;
}

TradeDesk::Result TradeDesk::Offer(const std::string& from, const bbcoop::TradeOffer& offer,
                                   const std::set<std::string>& online, s64 now_ms) {
    Result result;
    Open trade{.from = from,
               .from_name = offer.from_name(),
               .to = offer.to(),
               .to_name = offer.to_name(),
               .number = offer.number(),
               .give = TradeItemsOf(offer.give()),
               .want = TradeItemsOf(offer.want()),
               .expires_ms = now_ms + TradeExpiryMs};
    std::optional<std::string> problem = TradeItemsProblem(trade.give, false);
    if (!problem) {
        problem = TradeItemsProblem(trade.want, true);
    }
    // The asked side's game can't say which of its weapons it would give: only echoes and goods
    // may be asked for.
    if (!problem && std::ranges::any_of(trade.want, [](const TradeItem& item) {
            return item.kind == TradeItemKind::Item;
        })) {
        problem = "an item asked for";
    }
    const auto open_from =
        std::ranges::count_if(open, [&](const auto& entry) { return entry.second.from == from; });
    if (problem) {
        // as checked
    } else if (trade.to == from) {
        problem = "an offer to oneself";
    } else if (!online.contains(trade.to)) {
        problem = "the other member is not online";
    } else if (static_cast<size_t>(open_from) >= MaxOpenTradesPerMember) {
        problem = "too many open offers";
    }
    const u64 id = next_id++;
    if (problem) {
        Status(result, trade, id, TradeState::Refused, *problem);
        result.log.push_back("trade " + std::to_string(id) + " from " + from +
                             " refused: " + *problem);
        // Return only ordinary, valid items; malformed claims cannot become grants.
        Grant(result, from, id, trade.to, trade.to_name, RefundableItems(trade.give),
              TradeGrantReason::Returned);
        result.changed = true;
        return result;
    }
    open[id] = trade;
    Status(result, trade, id, TradeState::Open);
    result.sends.push_back({trade.to, AskOf(id, trade, now_ms)});
    result.log.push_back("trade " + std::to_string(id) + ": " + from + " offers " + trade.to + " " +
                         TradeItemsText(trade.give) +
                         (trade.want.empty() ? "" : " for " + TradeItemsText(trade.want)));
    result.changed = true;
    return result;
}

TradeDesk::Result TradeDesk::Answer(const std::string& member, const bbcoop::TradeAnswer& answer) {
    Result result;
    const TradeItems taken = TradeItemsOf(answer.taken());
    const auto found = open.find(answer.id());
    if (found == open.end() || found->second.to != member) {
        // Over already (expired, withdrawn) or not this member's: what it took comes back.
        if (answer.accept()) {
            Grant(result, member, answer.id(), {}, {}, RefundableItems(taken),
                  TradeGrantReason::Returned);
        }
        bbcoop::Envelope closed;
        closed.mutable_trade_closed()->set_id(answer.id());
        result.sends.push_back({member, std::move(closed)});
        return result;
    }
    if (!answer.accept()) {
        Close(result, answer.id(), TradeState::Declined);
        return result;
    }
    const Open trade = found->second;
    if (taken != trade.want) {
        // It took something else than what was asked: no exchange, both sides get theirs back.
        Grant(result, member, answer.id(), {}, {}, RefundableItems(taken),
              TradeGrantReason::Returned);
        Close(result, answer.id(), TradeState::Declined);
        return result;
    }
    open.erase(found);
    Status(result, trade, answer.id(), TradeState::Accepted);
    result.log.push_back("trade " + std::to_string(answer.id()) + " from " + trade.from + " to " +
                         trade.to + ": accepted");
    Grant(result, trade.to, answer.id(), trade.from, trade.from_name, trade.give,
          TradeGrantReason::Received);
    Grant(result, trade.from, answer.id(), trade.to, trade.to_name, trade.want,
          TradeGrantReason::Received);
    result.changed = true;
    return result;
}

TradeDesk::Result TradeDesk::InvasionVictory(const std::string& member, u64 invasion,
                                            const std::string& defeated, u32 echoes,
                                            bool blood_dreg) {
    Result result;
    TradeItems items{{TradeItemKind::Insight, 0, 1}};
    if (echoes > 0 && echoes <= MaxTradeEchoes) {
        items.push_back({TradeItemKind::Echoes, 0, echoes});
    }
    if (blood_dreg) {
        items.push_back({TradeItemKind::Goods, BloodDregGoods, 1});
    }
    Grant(result, member, invasion, defeated, "Invasion victory", std::move(items),
          TradeGrantReason::InvasionReward);
    return result;
}

TradeDesk::Result TradeDesk::Cancel(const std::string& member, u64 id) {
    Result result;
    const auto found = open.find(id);
    if (found != open.end() && found->second.from == member) {
        Close(result, id, TradeState::Cancelled);
    }
    return result;
}

TradeDesk::Result TradeDesk::Granted(const std::string& member, u64 grant) {
    Result result;
    const auto found = grants.find(member);
    if (found != grants.end() && found->second.erase(grant) > 0) {
        if (found->second.empty()) {
            grants.erase(found);
        }
        result.changed = true;
    }
    return result;
}

TradeDesk::Result TradeDesk::Expire(s64 now_ms) {
    Result result;
    std::vector<u64> due;
    for (const auto& [id, trade] : open) {
        if (now_ms >= trade.expires_ms) {
            due.push_back(id);
        }
    }
    for (const u64 id : due) {
        Close(result, id, TradeState::Expired);
    }
    return result;
}

TradeDesk::Result TradeDesk::Resume(const std::string& member, s64 now_ms) {
    Result result;
    if (const auto found = grants.find(member); found != grants.end()) {
        for (const auto& [id, grant] : found->second) {
            bbcoop::Envelope envelope;
            *envelope.mutable_trade_grant() = grant;
            result.sends.push_back({member, std::move(envelope)});
        }
    }
    for (const auto& [id, trade] : open) {
        if (trade.to == member) {
            result.sends.push_back({member, AskOf(id, trade, now_ms)});
        }
    }
    return result;
}

size_t TradeDesk::PendingGrants() const {
    size_t count = 0;
    for (const auto& [member, owed] : grants) {
        count += owed.size();
    }
    return count;
}

bbcoop::StoredTrades TradeDesk::Stored() const {
    bbcoop::StoredTrades stored;
    stored.set_next_id(next_id);
    for (const auto& [id, trade] : open) {
        bbcoop::StoredTrade& out = *stored.add_open();
        out.set_id(id);
        out.set_from(trade.from);
        out.set_from_name(trade.from_name);
        out.set_to(trade.to);
        out.set_to_name(trade.to_name);
        out.set_number(trade.number);
        PutTradeItems(trade.give, out.mutable_give());
        PutTradeItems(trade.want, out.mutable_want());
        out.set_expires_ms(trade.expires_ms);
    }
    for (const auto& [member, owed] : grants) {
        for (const auto& [id, grant] : owed) {
            bbcoop::StoredGrant& out = *stored.add_grants();
            out.set_member(member);
            *out.mutable_grant() = grant;
        }
    }
    return stored;
}

void TradeDesk::Restore(const bbcoop::StoredTrades& stored, s64 now_ms) {
    next_id = std::max<u64>(stored.next_id(), 1);
    open.clear();
    grants.clear();
    for (const bbcoop::StoredTrade& trade : stored.open()) {
        open[trade.id()] = Open{.from = trade.from(),
                                .from_name = trade.from_name(),
                                .to = trade.to(),
                                .to_name = trade.to_name(),
                                .number = trade.number(),
                                .give = TradeItemsOf(trade.give()),
                                .want = TradeItemsOf(trade.want()),
                                .expires_ms = now_ms + TradeExpiryMs};
        next_id = std::max(next_id, trade.id() + 1);
    }
    for (const bbcoop::StoredGrant& grant : stored.grants()) {
        grants[grant.member()][grant.grant().id()] = grant.grant();
        next_id = std::max(next_id, (grant.grant().id() & ~InvasionRewardGrantBit) + 1);
    }
}

// TradeClient

bbcoop::Envelope TradeClient::Offer(const std::string& to, const std::string& to_name,
                                    const std::string& from_name, TradeItems give,
                                    TradeItems want) {
    const u32 number = next_number++;
    bbcoop::Envelope envelope;
    bbcoop::TradeOffer& offer = *envelope.mutable_trade_offer();
    offer.set_number(number);
    offer.set_to(to);
    offer.set_to_name(to_name);
    offer.set_from_name(from_name);
    PutTradeItems(give, offer.mutable_give());
    PutTradeItems(want, offer.mutable_want());
    offers[number] = Outgoing{to_name, std::move(give), 0};
    return envelope;
}

bbcoop::Envelope TradeClient::Answer(u64 id, bool accept, TradeItems taken) {
    std::erase_if(asked, [&](const bbcoop::TradeAsk& ask) { return ask.id() == id; });
    bbcoop::Envelope envelope;
    bbcoop::TradeAnswer& answer = *envelope.mutable_trade_answer();
    answer.set_id(id);
    answer.set_accept(accept);
    PutTradeItems(taken, answer.mutable_taken());
    return envelope;
}

std::optional<bbcoop::Envelope> TradeClient::Cancel(u32 number) {
    const auto found = offers.find(number);
    if (found == offers.end() || found->second.id == 0) {
        return std::nullopt;
    }
    bbcoop::Envelope envelope;
    envelope.mutable_trade_cancel()->set_id(found->second.id);
    found->second.id = 0; // asked once
    return envelope;
}

TradeClient::Taken TradeClient::Take(const bbcoop::Envelope& envelope) {
    Taken taken;
    switch (envelope.body_case()) {
    case bbcoop::Envelope::kTradeStatus: {
        const bbcoop::TradeStatus& status = envelope.trade_status();
        const auto found = offers.find(status.number());
        if (found == offers.end()) {
            break;
        }
        const auto state = static_cast<TradeState>(status.state());
        if (state == TradeState::Open) {
            found->second.id = status.id();
            break;
        }
        using Kind = Notice::Kind;
        const Outgoing outgoing = found->second;
        offers.erase(found);
        switch (state) {
        case TradeState::Accepted:
            notices.push_back({Kind::Accepted, outgoing.to_name, outgoing.give, {}});
            break;
        case TradeState::Declined:
            notices.push_back({Kind::Declined, outgoing.to_name, outgoing.give, {}});
            break;
        case TradeState::Expired:
            notices.push_back({Kind::Expired, outgoing.to_name, outgoing.give, {}});
            break;
        case TradeState::Refused:
            notices.push_back({Kind::Refused, outgoing.to_name, outgoing.give, status.text()});
            break;
        default:
            break; // withdrawn: the giver knows
        }
        break;
    }
    case bbcoop::Envelope::kTradeAsk: {
        const bbcoop::TradeAsk& ask = envelope.trade_ask();
        const bool known = std::ranges::any_of(
            asked, [&](const bbcoop::TradeAsk& other) { return other.id() == ask.id(); });
        if (!known) {
            asked.push_back(ask);
        }
        break;
    }
    case bbcoop::Envelope::kTradeClosed:
        std::erase_if(asked, [&](const bbcoop::TradeAsk& ask) {
            return ask.id() == envelope.trade_closed().id();
        });
        break;
    case bbcoop::Envelope::kTradeGrant: {
        const bbcoop::TradeGrant& grant = envelope.trade_grant();
        if (TradeGrantItemsProblem(grant)) {
            break; // no application and no ack for an invalid or server-only forged grant
        }
        if (granted.insert(grant.id()).second) {
            TradeItems items = TradeItemsOf(grant.items());
            const auto reason = static_cast<TradeGrantReason>(grant.reason());
            notices.push_back({reason == TradeGrantReason::Returned ? Notice::Kind::Returned
                                                                    : Notice::Kind::Received,
                               grant.from_name(),
                               items,
                               {}});
            taken.add.push_back(std::move(items));
        }
        bbcoop::Envelope ack;
        ack.mutable_trade_granted()->set_id(grant.id());
        taken.replies.push_back(std::move(ack));
        break;
    }
    default:
        break;
    }
    return taken;
}

std::vector<TradeClient::Notice> TradeClient::TakeNotices() {
    return std::exchange(notices, {});
}

} // namespace BloodborneCoop
