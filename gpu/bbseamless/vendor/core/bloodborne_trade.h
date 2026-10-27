// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// Trading between party members, the game's side (bloodborne_trade.cpp; the protocol is
// server/coop/trade.h). The co-op runtime's files only.
#pragma once

#include <optional>
#include <span>
#include <string>
#include <vector>

#include "coop/trade.h"
#include "core/bloodborne_party.h"

namespace Core::Bloodborne::Coop {

using BloodborneCoop::TradeClient;
using BloodborneCoop::TradeItem;
using BloodborneCoop::TradeItems;

// The port with the server world; on once shadNet said it keeps trades too.
bool TradeSwitchOn();
bool TradesOn();
// The game's functions trading calls are the ones it knows (InstallTrading).
void EnableTrading();
// From the server world's stream thread.
void TakeServerTradeWelcome(bool trades);
void TakeServerTrade(bbcoop::Envelope envelope);
// Queues a message on the server world's stream (bloodborne_server_world.cpp).
void QueueServerWorld(bbcoop::Envelope envelope);

// The player's items (the game's thread). An item of its own is held once per inventory entry;
// taking one out (the one with `handle`, else the first) writes what it is into its data, adding
// one makes it anew from its data (its gems too).
u32 TradeHeld(const TradeItem& item);
// Whether the player's items read at all (its game data is there): TradeHeld's 0 means none.
bool PlayerItemsReadable();
// The inventory's key item list (where +0x58 points: the guest's own, or the game's list for a
// phantom, see PhantomKeyItemsToGiveBack) and what is in it; 0 and none when there is no data.
u64 KeyItemList();
std::vector<HeldGoods> HeldKeyGoods();
// Kaniel 2026-10-25 (duels): the goods the player holds that are not key items (consumables,
// materials), with their counts (the game's thread).
std::vector<HeldGoods> HeldItemGoods();
struct InventoryRow { u32 kind, id, count, handle; };
std::vector<InventoryRow> HeldTradeInventory();
// The weapon ids (with their upgrade level) in the player's inventory, not the storage.
std::vector<u32> HeldWeaponIds();
// The player's gestures (PlayerGameData +0x5B8, see DecodeGestureSlot), learned or not; none when
// there is no data. LearnGesture does what talk command 66 does: the gesture's slot learned
// (false when the list has no slot for it).
std::vector<GestureSlot> PlayerGestures();
bool LearnGesture(s32 gesture);
bool TakeTradeItems(std::span<TradeItem> items, u32 handle = 0);
// Whether a give shows the game's item-received popup (Silent: see SilentItemGets).
enum class ItemPopup { Shown, Silent };
void AddTradeItems(std::span<const TradeItem> items, ItemPopup popup = ItemPopup::Shown);
// Whether AddTradeItems can (the game's functions it calls are the ones it knows).
bool TradeItemsCanBeAdded();
// An item's name for the other member's boxes (the port's, from the game's own text): the name it
// has, else the namer's (SetTradeItemNamer), else nothing.
std::string TradeItemName(const TradeItem& item);
void SetTradeItemNamer(std::string (*namer)(const TradeItem& item));

// The other members a trade may go to now (board names), and how a member is called.
std::vector<std::string> TradePartners();
std::string TradeName(const std::string& member);

enum class TradeOfferResult { Sent, Off, Bad, NotEnough };
// `handle`: which of the player's items of their own to give (the inventory row's), else the first.
TradeOfferResult OfferTrade(const std::string& to, TradeItems give, TradeItems want,
                            u32 handle = 0);
std::optional<bbcoop::TradeAsk> FrontTradeAsk();
// Accepting takes what was asked out first; false when it was a no (or could not be given).
bool AnswerTrade(u64 id, bool accept);
u32 TradeNoticesSeen(TradeClient::Notice::Kind kind);
std::optional<TradeClient::Notice> NextTradeNotice();
// Every party tick on the game's thread; it runs the port's trade menus' tick too (SetTradeUiTick).
void TickTrades();
void SetTradeUiTick(void (*tick)());
// The rig's: a gem (item id 0x80000000 | id, its first effect) in the first slot of the player's
// first weapon of an id; whether one of them has it.
bool SocketTradeGem(u32 weapon, u32 gem, u32 effect);
bool HasSocketedGem(u32 weapon, u32 gem, u32 effect);
// The question an offer to this player asks (TradeAskText with the giver's name).
std::string TradeAskQuestion(const bbcoop::TradeAsk& ask);

} // namespace Core::Bloodborne::Coop
