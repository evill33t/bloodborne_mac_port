// SPDX-FileCopyrightText: Copyright 2026 shadNet Project
// SPDX-License-Identifier: GPL-2.0-or-later

// Trading between party members, the server's side (TradeDesk, one per party) and the game's
// (TradeClient), shared by shadNet, the emulator and the port. The messages are coop.proto's
// Trade*: a member offers another member goods and may ask for some back; the server holds the
// offer until the other answers, then hands each side its part, or the offer back to the giver.
//
// What moves and when (the server owns every trade, the games own the inventories):
//   - the giver's game takes what it offers out of its inventory, then sends TradeOffer;
//   - shadNet checks it (both members online in the party, tradeable items, limits) and asks the
//     other member (TradeAsk), or hands the offer straight back (TradeGrant, returned);
//   - the other member's game asks its player; to accept it takes what was asked of it out of its
//     inventory and sends TradeAnswer with what it took;
//   - shadNet hands the offer to the other member and what it gave to the giver (two grants), or,
//     declined, expired (TradeExpiryMs) or withdrawn (TradeCancel), the offer back to the giver;
//   - a game adds a grant's items and says so (TradeGranted); shadNet keeps the grant (on disk
//     too) until then and hands it again whenever the member's game says hello.
#pragma once

#include <map>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "coop.pb.h"
#include "coop/coop_types.h"

namespace BloodborneCoop {

enum class TradeItemKind : u32 {
    Echoes = 1, // Blood Echoes (id 0)
    Goods = 2,  // goods (consumables, materials, key items), by EquipParamGoods id
    Item = 3,   // an item of its own (weapon, armor, gem): category | id, one each, with its data
    Insight = 4, // server-issued invasion reward only (id 0); never tradeable
};
enum class TradeState : u32 {
    Open = 1,
    Accepted = 2,
    Declined = 3,
    Expired = 4,
    Refused = 5,
    Cancelled = 6,
};
enum class TradeGrantReason : u32 {
    Received = 0, // the other member's part of a trade
    Returned = 1, // the member's own items back
    InvasionReward = 2, // server victory: Insight, optional Echoes and covenant Blood Dreg
};

struct TradeItem {
    TradeItemKind kind = TradeItemKind::Goods;
    u32 id = 0;
    u32 count = 0;
    std::string name; // as the giver's game calls it (empty: the known goods' names)
    std::string data; // an item of its own: what the giver's game wrote of it (upgrades, gems)
    bool operator==(const TradeItem&) const = default;
};
using TradeItems = std::vector<TradeItem>;

// What may be traded (Kaniel 2026-10-15: any item): Blood Echoes, any goods, and items of their
// own (weapons, armor, gems and runes), each with its data. Item categories as the game's item ids
// carry them (the top 4 bits).
constexpr u32 BloodVialGoods = 1000;
constexpr u32 BloodDregGoods = 704; // original ItemLotParam 5500/5510, one for Corruption victories
constexpr u32 QuicksilverBulletGoods = 900;
// Reward mailboxes use the high bit, so their acknowledgements cannot consume a party's trade.
constexpr u64 InvasionRewardGrantBit = u64{1} << 63;
constexpr u32 MaxTradeEchoes = 999'999'999;
constexpr u32 MaxTradeGoods = 99;
constexpr u32 MaxTradeGoodsId = 0x7FFFFF; // the game's goods handles keep 23 bits
constexpr u32 WeaponCategory = 0x00000000;
constexpr u32 ArmorCategory = 0x10000000;
constexpr u32 AccessoryCategory = 0x20000000;
constexpr u32 GoodsCategory = 0x40000000;
constexpr u32 GemCategory = 0x80000000; // blood gems and Caryll runes
constexpr size_t MaxTradeItemData = 1024;
constexpr size_t MaxTradeItemName = 96;
struct TradeGood {
    TradeItemKind kind;
    u32 id;
    const char* name;
    const char* plural;
};
inline constexpr TradeGood TradeGoods[]{
    {TradeItemKind::Echoes, 0, "Blood Echoes", "Blood Echoes"},
    {TradeItemKind::Insight, 0, "Insight", "Insight"},
    {TradeItemKind::Goods, BloodVialGoods, "Blood Vial", "Blood Vials"},
    {TradeItemKind::Goods, QuicksilverBulletGoods, "Quicksilver Bullet", "Quicksilver Bullets"},
};
// The goods with names of their own here (echoes, vials, bullets: plurals too), or nothing.
const TradeGood* FindTradeGood(TradeItemKind kind, u32 id);
// Why these items can't be offered (or asked for), or nothing: no such kind or category, a count
// of 0 or over the limit (one of an item of its own), a name or data too long, an item twice.
// `allow_empty`: the asked-for side may be empty, the offered one not.
std::optional<std::string> TradeItemsProblem(std::span<const TradeItem> items, bool allow_empty);
// Checks a server grant separately: Insight is valid only in the exact invasion-reward shape.
std::optional<std::string> TradeGrantItemsProblem(const bbcoop::TradeGrant& grant);
// "5 Blood Vials, 1,000 Blood Echoes, Saw Cleaver +5, Molotov Cocktail x3": how the game words a
// trade's items (others than the known goods by their names, "an item" without one).
std::string TradeItemsText(std::span<const TradeItem> items);

// An open offer lives this long unanswered, and a member may have this many open at once.
constexpr s64 TradeExpiryMs = 60'000;
constexpr size_t MaxOpenTradesPerMember = 4;

bbcoop::TradeItem ToMessage(const TradeItem& item);
TradeItem TradeItemOf(const bbcoop::TradeItem& message);
TradeItems TradeItemsOf(const google::protobuf::RepeatedPtrField<bbcoop::TradeItem>& items);
void PutTradeItems(std::span<const TradeItem> items,
                   google::protobuf::RepeatedPtrField<bbcoop::TradeItem>* out);

// The game's words: the question an offer asks, the quantity picker's line, and what a notice
// says (empty: nothing to show, the game's own item popup says it).
std::string TradeAskText(const std::string& from_name, std::span<const TradeItem> give,
                         std::span<const TradeItem> want);
std::string TradeGiveQuestion(const TradeItem& item, const std::string& to_name);

// The quantity picker for Blood Echoes (Kaniel 2026-10-16: a clearer soul trade): Up/Down move by
// `step`, a round amount sized to what the player holds (under a hundred presses to any of them);
// it opens on one step, its lowest, and past the last step comes all of them. `big_step` is the
// game's other step (its inputs 0x3C/0x3D, 0x42/0x43, not the shoulder buttons).
struct TradeEchoesPicker {
    u32 step = 1;
    u32 big_step = 10;
    bool operator==(const TradeEchoesPicker&) const = default;
};
TradeEchoesPicker TradeEchoesPickerFor(u32 held);
// Its line: to whom, how many the player holds and the step.
std::string TradeGiveEchoesQuestion(u32 held, const std::string& to_name);

// Whether a message is one of trading's (coop.proto's Trade*).
bool IsTradeMessage(const bbcoop::Envelope& envelope);

// The server's side: a party's trades.
class TradeDesk {
public:
    // A message for one member (by its board name).
    struct Send {
        std::string to;
        bbcoop::Envelope envelope;
    };
    struct Result {
        std::vector<Send> sends;
        std::vector<std::string> log;
        bool changed = false; // the stored trades changed (write them)
    };

    // `online`: the party's members with a game linked now.
    Result Offer(const std::string& from, const bbcoop::TradeOffer& offer,
                 const std::set<std::string>& online, s64 now_ms);
    Result Answer(const std::string& member, const bbcoop::TradeAnswer& answer);
    Result Cancel(const std::string& member, u64 id);
    Result Granted(const std::string& member, u64 grant);
    // Server-only victory payment, persisted and replayed by the normal grant/ack machinery.
    Result InvasionVictory(const std::string& member, u64 invasion,
                           const std::string& defeated, u32 echoes, bool blood_dreg = false);
    // Offers past their time, back to their givers.
    Result Expire(s64 now_ms);
    // A member's game said hello (a new run): what it is owed again, its grants and the offers
    // waiting for its answer. Its own open offers stay open (they are on the server).
    Result Resume(const std::string& member, s64 now_ms);

    bbcoop::StoredTrades Stored() const;
    // Read back from disk: open offers get a fresh TradeExpiryMs from `now_ms`.
    void Restore(const bbcoop::StoredTrades& stored, s64 now_ms);

    size_t OpenTrades() const {
        return open.size();
    }
    size_t PendingGrants() const;

private:
    struct Open {
        std::string from;
        std::string from_name;
        std::string to;
        std::string to_name;
        u32 number = 0;
        TradeItems give;
        TradeItems want;
        s64 expires_ms = 0;
    };
    void Grant(Result& result, const std::string& member, u64 trade, const std::string& from,
               const std::string& from_name, TradeItems items, TradeGrantReason reason);
    void Status(Result& result, const Open& trade, u64 id, TradeState state, std::string text = {});
    void Close(Result& result, u64 id, TradeState state);
    bbcoop::Envelope AskOf(u64 id, const Open& trade, s64 now_ms) const;

    u64 next_id = 1;
    std::map<u64, Open> open;
    // Each member's grants not yet added, by grant id.
    std::map<std::string, std::map<u64, bbcoop::TradeGrant>> grants;
};

// The game's side: its offers, the offers it was asked to answer, and the grants it added.
class TradeClient {
public:
    // What the game shows its player.
    struct Notice {
        enum class Kind {
            Accepted, // the other member took this member's offer
            Declined, // ... said no (the offer comes back)
            Expired,  // ... did not answer in time
            Refused,  // the server would not take it (text says why)
            Received, // items added from another member
            Returned, // this member's own items back
        };
        Kind kind;
        std::string who; // the other member's character name
        TradeItems items;
        std::string text;
        bool operator==(const Notice&) const = default;
    };

    // A new offer to another member, its items already taken out: the message to send.
    bbcoop::Envelope Offer(const std::string& to, const std::string& to_name,
                           const std::string& from_name, TradeItems give, TradeItems want);
    // The answer to the asked offer `id` (the front of Asked()); `taken`: what the game took out of
    // its inventory to accept (what was asked of it), nothing for a no.
    bbcoop::Envelope Answer(u64 id, bool accept, TradeItems taken);
    // The giver withdraws an open offer (by its number); nothing if it has no id yet or is over.
    std::optional<bbcoop::Envelope> Cancel(u32 number);

    // A message from the server. Grants come back as the items to add now (each grant once) and
    // their acks to send once added.
    struct Taken {
        std::vector<TradeItems> add;
        std::vector<bbcoop::Envelope> replies;
    };
    Taken Take(const bbcoop::Envelope& envelope);

    // The offers waiting for this member's answer, oldest first.
    const std::vector<bbcoop::TradeAsk>& Asked() const {
        return asked;
    }
    // Shown once: taken by the game's UI.
    std::vector<Notice> TakeNotices();
    // This member's offers the server has not settled.
    size_t OpenOffers() const {
        return offers.size();
    }

private:
    struct Outgoing {
        std::string to_name;
        TradeItems give;
        u64 id = 0;
    };
    u32 next_number = 1;
    std::map<u32, Outgoing> offers;
    std::vector<bbcoop::TradeAsk> asked;
    std::set<u64> granted; // grant ids added this run
    std::vector<Notice> notices;
};

std::string TradeNoticeText(const TradeClient::Notice& notice);

} // namespace BloodborneCoop
