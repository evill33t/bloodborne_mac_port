// SPDX-License-Identifier: GPL-2.0-or-later
// bbport co-op: trading's C side for the port's trade menus (bbcoop.h, bbcoop_trade_*), on top of
// the co-op code's bloodborne_trade.h.
#include <algorithm>
#include <atomic>
#include <cstring>
#include <string>

#include "bbcoop.h"
#include "core/bloodborne_team.h"
#include "core/bloodborne_trade.h"

namespace {

using namespace Core::Bloodborne::Coop;
using BloodborneCoop::TradeItemKind;

int Copy(const std::string& text, char* out, size_t size) {
    if (out == nullptr || size == 0) {
        return 0;
    }
    const size_t length = std::min(text.size(), size - 1);
    std::memcpy(out, text.data(), length);
    out[length] = '\0';
    return 1;
}

TradeItem ItemOf(uint32_t kind, uint32_t id, uint32_t count = 0) {
    TradeItem item{static_cast<TradeItemKind>(kind), id, count};
    item.name = TradeItemName(item);
    return item;
}

std::atomic<int (*)(uint32_t, char*, size_t)> namer{nullptr};

std::string NameOf(const TradeItem& item) {
    const auto call = namer.load();
    if (call == nullptr || item.kind == TradeItemKind::Echoes) {
        return {};
    }
    const uint32_t item_id =
        item.kind == TradeItemKind::Goods ? BloodborneCoop::GoodsCategory | item.id : item.id;
    char text[128] = {};
    return call(item_id, text, sizeof text) ? std::string{text} : std::string{};
}

} // namespace

extern "C" {

int bbcoop_give_own_item(uint32_t item_id, uint32_t count) {
    if (!TradeItemsCanBeAdded() || count == 0) {
        return 0;
    }
    TradeItems items(std::min<uint32_t>(count, 20), TradeItem{TradeItemKind::Item, item_id, 1});
    AddTradeItems(items);
    return 1;
}

int bbcoop_trade_on(void) {
    return TradesOn() ? 1 : 0;
}

int bbcoop_trade_tradeable(uint32_t kind, uint32_t id) {
    const TradeItem item = ItemOf(kind, id, 1);
    if (item.kind == TradeItemKind::Goods && Core::Bloodborne::IsBellGoods(id)) {
        return 0;
    }
    return BloodborneCoop::TradeItemsProblem(std::span{&item, 1}, false) ? 0 : 1;
}

uint32_t bbcoop_trade_held(uint32_t kind, uint32_t id) {
    return TradeHeld(ItemOf(kind, id));
}

int bbcoop_trade_give_question(uint32_t kind, uint32_t id, char* text, size_t size) {
    const std::vector<std::string> partners = TradePartners();
    if (partners.empty()) {
        return 0;
    }
    const std::string to = TradeName(partners.front());
    if (static_cast<TradeItemKind>(kind) == TradeItemKind::Echoes) {
        return Copy(BloodborneCoop::TradeGiveEchoesQuestion(TradeHeld(ItemOf(kind, id)), to), text,
                    size);
    }
    return Copy(BloodborneCoop::TradeGiveQuestion(ItemOf(kind, id), to), text, size);
}

void bbcoop_trade_echoes_picker(uint32_t* step, uint32_t* big_step) {
    const BloodborneCoop::TradeEchoesPicker picker =
        BloodborneCoop::TradeEchoesPickerFor(TradeHeld(ItemOf(1, 0)));
    *step = picker.step;
    *big_step = picker.big_step;
}

int bbcoop_trade_offer(uint32_t kind, uint32_t id, uint32_t count, uint32_t handle) {
    const std::vector<std::string> partners = TradePartners();
    if (partners.empty() || count == 0) {
        return 0;
    }
    return OfferTrade(partners.front(), {ItemOf(kind, id, count)}, {}, handle) ==
                   TradeOfferResult::Sent
               ? 1
               : 0;
}

int bbcoop_trade_partner_count(void) {
    return static_cast<int>(TradePartners().size());
}

int bbcoop_trade_partner(int index, char* member, size_t member_size, char* name,
                         size_t name_size) {
    const std::vector<std::string> partners = TradePartners();
    if (index < 0 || static_cast<size_t>(index) >= partners.size()) {
        return 0;
    }
    const std::string& who = partners[static_cast<size_t>(index)];
    return Copy(who, member, member_size) && Copy(TradeName(who), name, name_size);
}

int bbcoop_trade_give_question_to(const char* member, uint32_t kind, uint32_t id, char* text,
                                  size_t size) {
    const std::vector<std::string> partners = TradePartners();
    if (member == nullptr || std::ranges::find(partners, member) == partners.end()) {
        return 0;
    }
    const std::string to = TradeName(member);
    if (static_cast<TradeItemKind>(kind) == TradeItemKind::Echoes) {
        return Copy(BloodborneCoop::TradeGiveEchoesQuestion(TradeHeld(ItemOf(kind, id)), to), text,
                    size);
    }
    return Copy(BloodborneCoop::TradeGiveQuestion(ItemOf(kind, id), to), text, size);
}

int bbcoop_trade_offer_to(const char* member, uint32_t kind, uint32_t id, uint32_t count,
                          uint32_t handle) {
    const std::vector<std::string> partners = TradePartners();
    if (member == nullptr || count == 0 ||
        std::ranges::find(partners, member) == partners.end()) {
        return 0;
    }
    return OfferTrade(member, {ItemOf(kind, id, count)}, {}, handle) == TradeOfferResult::Sent ? 1
                                                                                             : 0;
}

int bbcoop_trade_ask(uint64_t* id, char* text, size_t size) {
    const auto ask = FrontTradeAsk();
    if (!ask) {
        return 0;
    }
    *id = ask->id();
    return Copy(TradeAskQuestion(*ask), text, size);
}

int bbcoop_trade_ask_open(uint64_t id) {
    const auto ask = FrontTradeAsk();
    return ask && ask->id() == id ? 1 : 0;
}

int bbcoop_trade_answer(uint64_t id, int accept) {
    return AnswerTrade(id, accept != 0) ? 1 : 0;
}

int bbcoop_trade_notice(char* text, size_t size) {
    while (const auto notice = NextTradeNotice()) {
        const std::string line = BloodborneCoop::TradeNoticeText(*notice);
        if (!line.empty()) {
            return Copy(line, text, size);
        }
    }
    return 0;
}

void bbcoop_trade_set_ui_tick(void (*tick)(void)) {
    SetTradeUiTick(tick);
}

void bbcoop_trade_set_namer(int (*call)(uint32_t item_id, char* text, size_t size)) {
    namer.store(call);
    SetTradeItemNamer(call != nullptr ? &NameOf : nullptr);
}

} // extern "C"
