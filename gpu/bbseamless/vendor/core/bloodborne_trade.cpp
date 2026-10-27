// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// Trading between party members, the game's side (server/coop/trade.h has the protocol): with
// the server world (the port only, always on there) a member offers another Blood Echoes or any
// item (Kaniel 2026-10-15) and may ask for echoes or goods back; shadNet holds the offer until the
// other answers. This file takes items out of the player's inventory and puts
// them in (with the game's own "item obtained" popup), keeps the trades' client and answers to
// the rig's verbs; the native menus are in bloodborne_trade_ui.cpp.
//
// The game's functions (1.09, static RE 2026-10-14, docs/TRADING.md):
//   PlayerGameData = [GameDataMan 0x0553B130] +0x08: +0x94 Blood Echoes (0..999,999,999), the
//   inventory at +0x328 (entries of 16 bytes: handle, item id, count), EquipGameData at +0x1D0.
//   Find 0x014D9F60(inventory, &item id) -> index; count at the entry's +8.
//   Remove 0x014D0BE0(EquipGameData, index, -n, force, no_replace, allow 111) -> new count (n
//   clamped to the count first: a count below 0 turns into the max).
//   Give 0x017D89F0(MapItemMan [0x0553D6E0], list {n, {handle, item id, count, -1}[n]}): what a
//   pickup calls, up to the max held, the rest to the storage box, each with the popup.
//   Echoes: the shop's pay writes +0x94 itself; AddSouls (player vtable +0x3D0) adds them.
// Items of their own (weapons, armor, gems and runes; static RE 2026-10-15):
//   Their inventory entries hold a handle with bit 0x800000 into SprjGaitem [0x0553E990]: the
//   object at +8 + (handle & 0xFFFF) * 8 (its +8 the handle, +0xC the item id), the reference
//   count at +0x800C + index * 8. Make one 0x01A86FF0(u32* out, SprjGaitem, const u32* item id)
//   (a reference held in *out), let go of a reference 0x01A8BC00(u32*).
//   Each object writes and reads all it is through its vtable +0x40 / +0x48 (stream): a weapon
//   its upgrade fields (+0x18, +0x1C, +0x28) and its five gem slots, each slot its shape, and a
//   gem in it whole (id, +0x18, +0x1C, six effects); reading makes the gems anew. A stream is
//   {vtable}, +0x20 = (self, void* bytes, u32 size), the same call for both ways.
//   Take one out 0x014D0310(EquipGameData, index, 1): unequipped first (the 15 equipment slots,
//   the quick items), then the entry and its reference go (the inventory's Discard does so).
//   Add one as a pickup: 0x017D89F0 with {handle, item id, 1, -1}.

#include "core/bloodborne_re_internal.h"
#include "core/bloodborne_team.h"
#include "core/bloodborne_trade.h"

#include <array>
#include <cstring>
#include <deque>
#include <functional>

extern "C" int bbseamless_reward_lookup(const char*,uint64_t,const void*,size_t,int);
extern "C" int bbseamless_reward_begin(const char*,uint64_t,const void*,size_t,int);
extern "C" int bbseamless_reward_arm(uint64_t,int);
extern "C" int bbseamless_reward_disarm(uint64_t);
extern "C" int bbseamless_reward_cancel(uint64_t);

namespace Core::Bloodborne {
namespace {

constexpr u64 GameDataManPointerOffset = 0x0553B130;
constexpr u64 PlayerGameDataOffset = 0x08;
constexpr u64 EchoesOffset = 0x94;
constexpr u64 EquipGameDataOffset = 0x1D0;
constexpr u64 InventoryOffset = 0x328;
constexpr u64 GestureListOffset = 0x5B8;
constexpr u32 GestureListSlots = 30; // u32 slots from +0x10 (talk command 66's loop, 0x02042B5C)
constexpr u64 InventoryFindOffset = 0x014D9F60;
constexpr u64 InventoryChangeCountOffset = 0x014D0BE0;
constexpr u64 GiveItemsOffset = 0x017D89F0;
constexpr u64 SprjGaitemPointerOffset = 0x0553E990;
constexpr u64 MakeGaitemOffset = 0x01A86FF0;
constexpr u64 ReleaseGaitemOffset = 0x01A8BC00;
constexpr u64 RemoveEntryOffset = 0x014D0310;
constexpr u64 GaitemWriteSlot = 0x40;
constexpr u64 GaitemReadSlot = 0x48;
// The objects' vtables: weapon, armor, gem.
constexpr std::array<u64, 3> GaitemVtables{0x05336330, 0x053362D0, 0x05336180};
constexpr u32 GoodsHandle = 0xB0000000;
constexpr u32 AccessoryHandle = 0xA0000000;
constexpr u32 GaitemBit = 0x800000;

// Always on in the port (Kaniel 2026-10-14), with the server world; the port only
// (docs/SERVER_COOP_PLAN.md).
#ifdef BLOODBORNE_NATIVE_PORT
constexpr bool trade_switch = true;
#else
constexpr bool trade_switch = false;
#endif

std::atomic<bool> server_trades{false};
std::atomic<bool> trade_sites{false};
// The port's trade menus (bbport_trade_menu.cpp), run on the game's thread with the trades.
std::atomic<void (*)()> trade_ui_tick{nullptr};

// The client and what came from the server for it: the stream's thread queues, the game's
// thread takes.
std::mutex trade_mutex;
TradeClient client;
std::deque<bbcoop::Envelope> trade_inbox;
// The notices seen this run, for the rig (each kind's count) and the UI (the queue).
std::map<TradeClient::Notice::Kind, u32> notices_seen;
std::deque<TradeClient::Notice> notices_to_show;

u64 PlayerGameData() {
    const u64 manager = ReadValue<u64>(image_base + GameDataManPointerOffset, 0);
    return manager >= 0x10000 ? ReadValue<u64>(manager, PlayerGameDataOffset) : 0;
}

s32 GoodsIndex(u64 data, u32 goods) {
    const u32 item_id = GoodsCategory | goods;
    using Find = s32 PS4_SYSV_ABI (*)(u64 inventory, const u32* item_id);
    return reinterpret_cast<Find>(image_base + InventoryFindOffset)(data + InventoryOffset,
                                                                    &item_id);
}

// The inventory entry of a goods (its address), or 0 when the player has none.
u64 GoodsEntry(u64 data, u32 goods) {
    const s32 index = GoodsIndex(data, goods);
    if (index < 0) {
        return 0;
    }
    const u64 inventory = data + InventoryOffset;
    const u32 split = ReadValue<u32>(inventory, 0x24);
    const bool first = static_cast<u32>(index) < split;
    const u64 entries = ReadValue<u64>(inventory, first ? 0x58 : 0x48);
    const u64 entry =
        entries + 16ull * (first ? static_cast<u32>(index) : static_cast<u32>(index) - split);
    return entries >= 0x10000 && ReadValue<u32>(entry, 4) == (GoodsCategory | goods) ? entry : 0;
}

// The inventory's entries (their addresses), in order.
std::vector<u64> InventoryEntries(u64 data) {
    std::vector<u64> found;
    const u64 inventory = data + InventoryOffset;
    const s32 last = ReadValue<s32>(inventory, 0x88);
    const u32 split = ReadValue<u32>(inventory, 0x24);
    const u64 first_entries = ReadValue<u64>(inventory, 0x58);
    const u64 later_entries = ReadValue<u64>(inventory, 0x48);
    for (s32 index = 0; index <= last && index < 0x1000; ++index) {
        const bool first = static_cast<u32>(index) < split;
        const u64 entries = first ? first_entries : later_entries;
        if (entries < 0x10000) {
            continue;
        }
        const u64 entry =
            entries + 16ull * (first ? static_cast<u32>(index) : static_cast<u32>(index) - split);
        if (ReadValue<u32>(entry, 0) != 0) {
            found.push_back(entry);
        }
    }
    return found;
}

// The index of an entry (its address) in the inventory.
s32 EntryIndex(u64 data, u64 entry) {
    const u64 inventory = data + InventoryOffset;
    const u32 split = ReadValue<u32>(inventory, 0x24);
    const u64 first_entries = ReadValue<u64>(inventory, 0x58);
    if (entry >= first_entries && entry < first_entries + 16ull * split) {
        return static_cast<s32>((entry - first_entries) / 16);
    }
    return static_cast<s32>(split + (entry - ReadValue<u64>(inventory, 0x48)) / 16);
}

// The inventory entries of an item of its own (by item id), or the one with `handle`.
std::vector<u64> ItemEntries(u64 data, u32 item_id, u32 handle = 0) {
    std::vector<u64> found;
    for (const u64 entry : InventoryEntries(data)) {
        if (ReadValue<u32>(entry, 4) == item_id &&
            (handle == 0 || ReadValue<u32>(entry, 0) == handle)) {
            found.push_back(entry);
        }
    }
    return found;
}

bool IsGaitemHandle(u32 handle) {
    return handle != 0 && (handle & 0xFFFFFF) != 0xFFFFFF && (handle & GaitemBit) != 0;
}

// The SprjGaitem object of a handle, or 0.
u64 GaitemOf(u32 handle) {
    const u64 table = ReadValue<u64>(image_base + SprjGaitemPointerOffset, 0);
    if (table < 0x10000 || !IsGaitemHandle(handle) || (handle & 0xFFFF) == 0xFFFF) {
        return 0;
    }
    const u64 object = ReadValue<u64>(table, 8 + 8ull * (handle & 0xFFFF));
    return object >= 0x10000 && ReadValue<u32>(object, 8) == handle &&
                   HasVtable(object, GaitemVtables, "trade item")
               ? object
               : 0;
}

// A stream for the objects' own write and read (vtable +0x20 the only call they make).
struct GaitemStream {
    const void* const* vtable;
    std::string* bytes;
    size_t at;
    bool failed;
};
PS4_SYSV_ABI void StreamWrite(GaitemStream* self, const void* bytes, u32 size) {
    self->bytes->append(static_cast<const char*>(bytes), size);
}
PS4_SYSV_ABI void StreamRead(GaitemStream* self, void* bytes, u32 size) {
    if (self->bytes->size() - self->at < size) {
        std::memset(bytes, 0, size);
        self->failed = true;
        return;
    }
    std::memcpy(bytes, self->bytes->data() + self->at, size);
    self->at += size;
}
PS4_SYSV_ABI void StreamOther(GaitemStream* self) {
    self->failed = true;
}
const void* const WriteStreamVtable[8] = {
    reinterpret_cast<const void*>(&StreamOther), reinterpret_cast<const void*>(&StreamOther),
    reinterpret_cast<const void*>(&StreamOther), reinterpret_cast<const void*>(&StreamOther),
    reinterpret_cast<const void*>(&StreamWrite), reinterpret_cast<const void*>(&StreamOther),
    reinterpret_cast<const void*>(&StreamOther), reinterpret_cast<const void*>(&StreamOther)};
const void* const ReadStreamVtable[8] = {
    reinterpret_cast<const void*>(&StreamOther), reinterpret_cast<const void*>(&StreamOther),
    reinterpret_cast<const void*>(&StreamOther), reinterpret_cast<const void*>(&StreamOther),
    reinterpret_cast<const void*>(&StreamRead),  reinterpret_cast<const void*>(&StreamOther),
    reinterpret_cast<const void*>(&StreamOther), reinterpret_cast<const void*>(&StreamOther)};

// What an object is (the game's own write), or nothing.
std::optional<std::string> WriteGaitem(u64 object) {
    std::string bytes;
    GaitemStream stream{WriteStreamVtable, &bytes, 0, false};
    using Write = void PS4_SYSV_ABI (*)(u64 object, GaitemStream* stream);
    reinterpret_cast<Write>(ReadValue<u64>(ReadValue<u64>(object, 0), GaitemWriteSlot))(object,
                                                                                        &stream);
    if (stream.failed) {
        return std::nullopt;
    }
    return bytes;
}

// Makes an object what `bytes` say (the game's own read): false if they did not fit.
bool ReadGaitem(u64 object, const std::string& bytes) {
    std::string copy = bytes;
    GaitemStream stream{ReadStreamVtable, &copy, 0, false};
    using Read = void PS4_SYSV_ABI (*)(u64 object, GaitemStream* stream);
    reinterpret_cast<Read>(ReadValue<u64>(ReadValue<u64>(object, 0), GaitemReadSlot))(object,
                                                                                      &stream);
    return !stream.failed && stream.at == copy.size();
}

// A new object of an item (its reference in the handle), or 0.
u32 MakeGaitem(u32 item_id) {
    const u64 table = ReadValue<u64>(image_base + SprjGaitemPointerOffset, 0);
    if (table < 0x10000) {
        return 0;
    }
    u32 handle = 0;
    using Make = void PS4_SYSV_ABI (*)(u32* out, u64 table, const u32* item_id);
    reinterpret_cast<Make>(image_base + MakeGaitemOffset)(&handle, table, &item_id);
    return handle;
}

void ReleaseGaitem(u32 handle) {
    using Release = void PS4_SYSV_ABI (*)(u32* handle);
    reinterpret_cast<Release>(image_base + ReleaseGaitemOffset)(&handle);
}

bool HasGaitem(u32 item_id) {
    const u32 category = item_id & 0xF0000000;
    return category == WeaponCategory || category == ArmorCategory || category == GemCategory;
}

std::atomic<std::string (*)(const TradeItem&)> trade_namer{nullptr};

} // namespace

namespace Coop {

bool TradeSwitchOn() {
    return trade_switch && ServerWorldOn();
}

bool TradesOn() {
    return TradeSwitchOn() && trade_sites.load() && server_trades.load();
}

void EnableTrading() {
    trade_sites.store(true);
}

void TakeServerTradeWelcome(bool trades) {
    server_trades.store(trades);
}

void TakeServerTrade(bbcoop::Envelope envelope) {
    std::scoped_lock lock{trade_mutex};
    trade_inbox.push_back(std::move(envelope));
}

bool PlayerItemsReadable() {
    return PlayerGameData() >= 0x10000;
}

u64 KeyItemList() {
    const u64 data = PlayerGameData();
    return data >= 0x10000 ? ReadValue<u64>(data + InventoryOffset, 0x58) : 0;
}

std::vector<HeldGoods> HeldKeyGoods() {
    std::vector<HeldGoods> held;
    const u64 data = PlayerGameData();
    if (data < 0x10000) {
        return held;
    }
    const u32 split = ReadValue<u32>(data + InventoryOffset, 0x24);
    for (const u64 entry : InventoryEntries(data)) {
        const u32 id = ReadValue<u32>(entry, 4);
        const s32 count = ReadValue<s32>(entry, 8);
        if (static_cast<u32>(EntryIndex(data, entry)) < split &&
            (id & 0xF0000000) == GoodsCategory && count > 0) {
            held.push_back({id & ~GoodsCategory, count});
        }
    }
    return held;
}

std::vector<HeldGoods> HeldItemGoods() {
    std::vector<HeldGoods> held;
    const u64 data = PlayerGameData();
    if (data < 0x10000) {
        return held;
    }
    const u32 split = ReadValue<u32>(data + InventoryOffset, 0x24);
    for (const u64 entry : InventoryEntries(data)) {
        const u32 id = ReadValue<u32>(entry, 4);
        const s32 count = ReadValue<s32>(entry, 8);
        if (static_cast<u32>(EntryIndex(data, entry)) >= split &&
            (id & 0xF0000000) == GoodsCategory && count > 0) {
            held.push_back({id & ~GoodsCategory, count});
        }
    }
    return held;
}

// Native port: enumerate existing entries for the trade picker, on the game thread.
std::vector<InventoryRow> HeldTradeInventory() {
    std::vector<InventoryRow> rows;
    const u64 data = PlayerGameData();
    if (data < 0x10000) return rows;
    const u32 echoes = TradeHeld({BloodborneCoop::TradeItemKind::Echoes, 0, 1});
    if (echoes) rows.push_back({1, 0, echoes, 0});
    for (const u64 entry : InventoryEntries(data)) {
        const u32 id = ReadValue<u32>(entry, 4);
        const s32 count = ReadValue<s32>(entry, 8);
        if (count <= 0) continue;
        const bool goods = (id & 0xF0000000) == GoodsCategory;
        const u32 kind = goods ? 2 : 3;
        // Empty weapon slots (Bare Fists) are not inventory objects to offer.
        if (!goods && (id == 0 || GaitemOf(ReadValue<u32>(entry, 0)) == 0)) continue;
        const u32 item = goods ? id & ~GoodsCategory : id;
        const BloodborneCoop::TradeItem checked{static_cast<BloodborneCoop::TradeItemKind>(kind), item, 1};
        if (BloodborneCoop::TradeItemsProblem(std::span{&checked, 1}, false)) continue;
        rows.push_back({kind, item, static_cast<u32>(count), ReadValue<u32>(entry, 0)});
        if (rows.size() == 512) break;
    }
    return rows;
}

std::vector<u32> HeldWeaponIds() {
    std::vector<u32> held;
    const u64 data = PlayerGameData();
    if (data < 0x10000) {
        return held;
    }
    for (const u64 entry : InventoryEntries(data)) {
        const u32 id = ReadValue<u32>(entry, 4);
        if ((id & 0xF0000000) == WeaponCategory && ReadValue<s32>(entry, 8) > 0) {
            held.push_back(id);
        }
    }
    return held;
}

// The gesture list's slot addresses, none without a list.
std::vector<u64> GestureSlotAddresses() {
    std::vector<u64> slots;
    const u64 data = PlayerGameData();
    const u64 list = data >= 0x10000 ? ReadValue<u64>(data, GestureListOffset) : 0;
    if (list < 0x10000 ||
        !HasMemoryAccess(list + 0x10, GestureListSlots * 4, MemoryProt::CpuRead)) {
        return slots;
    }
    for (u32 slot = 0; slot < GestureListSlots; ++slot) {
        slots.push_back(list + 0x10 + slot * 4);
    }
    return slots;
}

std::vector<GestureSlot> PlayerGestures() {
    std::vector<GestureSlot> gestures;
    for (const u64 slot : GestureSlotAddresses()) {
        if (const auto gesture = DecodeGestureSlot(ReadValue<u32>(slot, 0))) {
            gestures.push_back(*gesture);
        }
    }
    return gestures;
}

bool LearnGesture(s32 gesture) {
    for (const u64 slot : GestureSlotAddresses()) {
        const auto found = DecodeGestureSlot(ReadValue<u32>(slot, 0));
        if (found && found->gesture == gesture) {
            WriteValue(slot, 0, static_cast<u8>(ReadValue<u8>(slot, 0) | 1)); // 0x02043E9E
            return true;
        }
    }
    return false;
}

// What the player holds of an item: its echoes, or the goods in its inventory (not the storage).
u32 TradeHeld(const TradeItem& item) {
    const u64 data = PlayerGameData();
    if (data < 0x10000) {
        return 0;
    }
    if (item.kind == TradeItemKind::Echoes) {
        return static_cast<u32>(std::max(ReadValue<s32>(data, EchoesOffset), 0));
    }
    if (item.kind == TradeItemKind::Item) {
        u32 held = 0;
        for (const u64 entry : ItemEntries(data, item.id)) {
            held += static_cast<u32>(std::max(ReadValue<s32>(entry, 8), 0));
        }
        return held;
    }
    const u64 entry = GoodsEntry(data, item.id);
    return entry != 0 ? static_cast<u32>(std::max(ReadValue<s32>(entry, 8), 0)) : 0;
}

// Takes the items out of the inventory, all or none (the game's thread); an item of its own
// (the one with `handle` if given) keeps what it is in its data.
bool TakeTradeItems(std::span<TradeItem> items, u32 handle) {
    const u64 data = PlayerGameData();
    if (!trade_sites.load() || data < 0x10000 || TradeItemsProblem(items, true)) {
        return false;
    }
    std::vector<u64> item_entries(items.size(), 0);
    for (size_t index = 0; index < items.size(); ++index) {
        TradeItem& item = items[index];
        if (item.kind == TradeItemKind::Goods && IsBellGoods(item.id)) {
            return false; // the bell items go from the port's game (RemoveBellItems)
        }
        if (item.kind != TradeItemKind::Item) {
            if (TradeHeld(item) < item.count) {
                return false;
            }
            continue;
        }
        // Each its own entry: the one asked for, else the first that is not taken already.
        const std::vector<u64> entries = ItemEntries(data, item.id, handle);
        const auto free = std::ranges::find_if(entries, [&](u64 entry) {
            return std::ranges::find(item_entries, entry) == item_entries.end();
        });
        if (free == entries.end()) {
            return false;
        }
        item_entries[index] = *free;
        item.data.clear();
        const u32 item_handle = ReadValue<u32>(*free, 0);
        if (IsGaitemHandle(item_handle)) {
            const u64 object = GaitemOf(item_handle);
            std::optional<std::string> written = object != 0 ? WriteGaitem(object) : std::nullopt;
            if (!written || written->size() > MaxTradeItemData) {
                LOG_ERROR(Debug, "Bloodborne trade: could not write item {:#x} (handle {:#x})",
                          item.id, item_handle);
                return false;
            }
            item.data = std::move(*written);
        }
    }
    using ChangeCount = s32 PS4_SYSV_ABI (*)(u64 equip, u32 index, s32 delta, bool force,
                                             bool no_replace, bool allow_111);
    const auto change = reinterpret_cast<ChangeCount>(image_base + InventoryChangeCountOffset);
    using Remove = s32 PS4_SYSV_ABI (*)(u64 equip, u32 index, bool unlink);
    const auto remove = reinterpret_cast<Remove>(image_base + RemoveEntryOffset);
    // Items of their own first (by their entries' addresses, which a removal can change), the last
    // entry first.
    std::vector<s32> item_indexes;
    for (const u64 entry : item_entries) {
        if (entry != 0) {
            item_indexes.push_back(EntryIndex(data, entry));
        }
    }
    std::ranges::sort(item_indexes, std::greater{});
    for (const s32 index : item_indexes) {
        remove(data + EquipGameDataOffset, static_cast<u32>(index), true);
    }
    for (const TradeItem& item : items) {
        if (item.kind == TradeItemKind::Echoes) {
            const s32 echoes = ReadValue<s32>(data, EchoesOffset);
            WriteValue(data, EchoesOffset, std::max(echoes - static_cast<s32>(item.count), 0));
            continue;
        }
        if (item.kind == TradeItemKind::Item) {
            continue;
        }
        // Forced: key items and other goods that are not used up go too.
        const s32 index = GoodsIndex(data, item.id);
        change(data + EquipGameDataOffset, static_cast<u32>(index), -static_cast<s32>(item.count),
               true, true, false);
    }
    if (!items.empty()) {
        LOG_INFO(Debug, "Bloodborne trade: took {} out of the inventory", TradeItemsText(items));
    }
    return true;
}

bool TradeItemsCanBeAdded() {
    return trade_sites.load();
}

// Puts the items in as a pickup does: goods up to the max held, the rest to the storage box,
// each with the game's popup unless Silent; echoes through AddSouls (the game's thread).
void AddTradeItems(std::span<const TradeItem> items, ItemPopup popup) {
    if (!trade_sites.load()) {
        return;
    }
    std::optional<SilentItemGets> silent;
    if (popup == ItemPopup::Silent) {
        silent.emplace();
    }
    struct GiveEntry {
        u32 handle;
        u32 item_id;
        s32 count;
        s32 extra;
    };
    struct GiveList {
        u32 count;
        GiveEntry entries[6];
    };
    GiveList list{};
    std::vector<u32> made; // our references to the new objects, let go once given
    const auto give = [&] {
        if (list.count == 0) {
            return;
        }
        const u64 item_man = ReadValue<u64>(image_base + MapItemManPointerOffset, 0);
        if (HasVtable(item_man, MapItemManVtable, "trade give")) {
            using Give = void PS4_SYSV_ABI (*)(u64 item_man, GiveList* list);
            reinterpret_cast<Give>(image_base + GiveItemsOffset)(item_man, &list);
        } else {
            LOG_ERROR(Debug, "Bloodborne trade: no item man to add {}", TradeItemsText(items));
        }
        list = {};
        for (const u32 handle : made) {
            ReleaseGaitem(handle);
        }
        made.clear();
    };
    for (const TradeItem& item : items) {
        if(item.kind==TradeItemKind::Insight) {
            // Server-only validated grant (TradeGrantItemsProblem), never an offer or refund.
            // Common event 9350 awards one Insight through effect 4680, as GiveEncounterInsight.
            const u64 player=GetLocalPlayer();
            if(item.id==0 && item.count==1 && HasVirtual(player,CharacterAddSpEffectSlot,AddSpEffectOffset,"invasion reward Insight")) {
                reinterpret_cast<AddSpEffectFn>(image_base+AddSpEffectOffset)(player,4680,player,0,1,0,1.f,1.f,1.f,1.f,1.f,0);
                LOG_INFO(Debug,"Co-op invasion reward: native Insight +1");
            }
            continue;
        }
        if (list.count == std::size(list.entries)) {
            give();
        }
        if (item.kind == TradeItemKind::Item) {
            u32 handle = (item.id & 0xF0000000) == AccessoryCategory
                             ? AccessoryHandle | (item.id & 0xFFFFFF)
                             : 0;
            if (HasGaitem(item.id)) {
                handle = MakeGaitem(item.id);
                const u64 object = GaitemOf(handle);
                if (object == 0) {
                    LOG_ERROR(Debug, "Bloodborne trade: could not make item {:#x}", item.id);
                    if (handle != 0) {
                        ReleaseGaitem(handle);
                    }
                    continue;
                }
                made.push_back(handle);
                if (!item.data.empty() && !ReadGaitem(object, item.data)) {
                    LOG_ERROR(Debug, "Bloodborne trade: item {:#x}'s {} bytes did not fit", item.id,
                              item.data.size());
                }
            }
            list.entries[list.count++] = {handle, item.id, 1, -1};
            continue;
        }
        if (item.kind == TradeItemKind::Echoes) {
            const u64 player = GetLocalPlayer();
            if (HasVirtual(player, 0x3D0, AddSoulsOffset, "trade AddSouls")) {
                using AddSouls = void PS4_SYSV_ABI (*)(u64 player, s32 amount, s32, bool rates);
                reinterpret_cast<AddSouls>(image_base + AddSoulsOffset)(
                    player, static_cast<s32>(item.count), 0, false);
            }
            continue;
        }
        if(item.kind!=TradeItemKind::Goods) continue;
        list.entries[list.count++] = {GoodsHandle | item.id, GoodsCategory | item.id,
                                      static_cast<s32>(item.count), -1};
    }
    give();
    LOG_INFO(Debug, "Bloodborne trade: added {}{}", TradeItemsText(items),
             popup == ItemPopup::Silent ? " (no popup)" : "");
}

// The other members a trade may go to: on the vitals board, on this map, standing, the nearest
// first.
std::vector<std::string> TradePartners() {
    const VitalsView view = ReadVitalsBoard();
    const auto self = std::ranges::find(view.board.members, view.member, &MemberVitals::member);
    std::vector<std::pair<float, std::string>> partners;
    for (const MemberVitals& member : view.board.members) {
        if (member.member == view.member || member.vital != Vital::Up ||
            (self != view.board.members.end() && self->map != member.map)) {
            continue;
        }
        float distance = 0.0f;
        if (self != view.board.members.end()) {
            for (size_t axis = 0; axis < 3; ++axis) {
                const float delta = member.at[axis] - self->at[axis];
                distance += delta * delta;
            }
        }
        partners.emplace_back(distance, member.member);
    }
    std::ranges::sort(partners);
    std::vector<std::string> names;
    for (auto& [distance, name] : partners) {
        names.push_back(std::move(name));
    }
    return names;
}

std::string TradeName(const std::string& member) {
    // Phase 11: the character's name, as everywhere else.
    // Never the online id (website shots 2026-10-19): a Hunter until the name is known.
    std::string shown = Coop::CharacterNameOf(member);
    return shown.empty() ? std::string{UnknownHunterName} : shown;
}

std::string TradeItemName(const TradeItem& item) {
    if (!item.name.empty() || FindTradeGood(item.kind, item.id) != nullptr) {
        return item.name;
    }
    const auto namer = trade_namer.load();
    return namer != nullptr ? namer(item) : std::string{};
}

void SetTradeItemNamer(std::string (*namer)(const TradeItem& item)) {
    trade_namer.store(namer);
}

TradeOfferResult OfferTrade(const std::string& to, TradeItems give, TradeItems want, u32 handle) {
    if (!TradesOn()) {
        return TradeOfferResult::Off;
    }
    for (TradeItem& item : give) {
        item.name = TradeItemName(item);
    }
    for (TradeItem& item : want) {
        item.name = TradeItemName(item);
    }
    if (TradeItemsProblem(give, false) || TradeItemsProblem(want, true) ||
        std::ranges::any_of(
            want, [](const TradeItem& item) { return item.kind == TradeItemKind::Item; })) {
        return TradeOfferResult::Bad;
    }
    if (!TakeTradeItems(give, handle)) {
        return TradeOfferResult::NotEnough;
    }
    bbcoop::Envelope envelope;
    {
        std::scoped_lock lock{trade_mutex};
        // No name yet: "" (the other side names us itself, never by our online id).
        envelope = client.Offer(to, TradeName(to), Coop::OwnCharacterName(), give, want);
    }
    QueueServerWorld(std::move(envelope));
    LOG_INFO(Debug, "Bloodborne trade: offered {} {}{}", TradeName(to), TradeItemsText(give),
             want.empty() ? "" : " for " + TradeItemsText(want));
    return TradeOfferResult::Sent;
}

std::optional<bbcoop::TradeAsk> FrontTradeAsk() {
    std::scoped_lock lock{trade_mutex};
    if (client.Asked().empty()) {
        return std::nullopt;
    }
    return client.Asked().front();
}

bool AnswerTrade(u64 id, bool accept) {
    TradeItems taken;
    if (accept) {
        const auto ask = FrontTradeAsk();
        if (!ask || ask->id() != id) {
            return false;
        }
        taken = TradeItemsOf(ask->want());
        if (!TakeTradeItems(taken)) {
            accept = false; // can't give what was asked: a no
            taken.clear();
        }
    }
    bbcoop::Envelope envelope;
    {
        std::scoped_lock lock{trade_mutex};
        envelope = client.Answer(id, accept, taken);
    }
    QueueServerWorld(std::move(envelope));
    LOG_INFO(Debug, "Bloodborne trade: {} offer {}", accept ? "accepted" : "declined", id);
    return accept;
}

u32 TradeNoticesSeen(TradeClient::Notice::Kind kind) {
    std::scoped_lock lock{trade_mutex};
    const auto found = notices_seen.find(kind);
    return found == notices_seen.end() ? 0 : found->second;
}

std::optional<TradeClient::Notice> NextTradeNotice() {
    std::scoped_lock lock{trade_mutex};
    if (notices_to_show.empty()) {
        return std::nullopt;
    }
    TradeClient::Notice notice = std::move(notices_to_show.front());
    notices_to_show.pop_front();
    return notice;
}

// The game's thread, every party tick: what came from the server, grants added.
// Kaniel 2026-10-14: the bell items go from the port's game (a team joins without them): any the
// player holds leave the inventory, checked every few seconds in the world (a gift or a pickup that
// gives one again loses it the same way).
void RemoveBellItems() {
    static s64 checked_ms = 0;
    const s64 now_ms = SteadyMilliseconds();
    if (now_ms - checked_ms < 3'000 || GetLocalPlayer() < 0x10000) {
        return;
    }
    checked_ms = now_ms;
    const u64 data = PlayerGameData();
    if (data < 0x10000) {
        return;
    }
    using ChangeCount = s32 PS4_SYSV_ABI (*)(u64 equip, u32 index, s32 delta, bool force,
                                             bool no_replace, bool allow_111);
    const auto change = reinterpret_cast<ChangeCount>(image_base + InventoryChangeCountOffset);
    for (const u32 bell : BellGoods) {
        const u64 entry = GoodsEntry(data, bell);
        const s32 count = entry != 0 ? ReadValue<s32>(entry, 8) : 0;
        if (count <= 0) {
            continue;
        }
        const s32 index = GoodsIndex(data, bell);
        const s32 left =
            change(data + EquipGameDataOffset, static_cast<u32>(index), -count, true, true, false);
        LOG_INFO(Debug, "Bloodborne team: bell item {} out of the inventory ({} -> {})", bell,
                 count, left);
    }
}

namespace {
constexpr u64 NativeSaveIdleOffset=0x0156AA10,NativeFreshSaveOffset=0x0156D570;
constexpr std::array<u8,18> NativeSaveIdleBytes{0x48,0x8b,0x05,0x61,0xbc,0xfe,0x03,0x83,0xb8,0x54,0x15,0,0,0,0x0f,0x94,0xc0,0xc3};
constexpr std::array<u8,25> NativeFreshSaveBytes{0x55,0x48,0x89,0xe5,0x41,0x57,0x41,0x56,0x41,0x55,0x41,0x54,0x53,0x48,0x83,0xec,0x48,0x89,0xd3,0x41,0x89,0xf6,0x41,0x89,0xfd};

// The original native repository is the inventory pointer at PlayerGameData+0x5B0.
// Both it and +0x328 use 16-byte entries; capacities are 99 held and 99 stored (Goods704).
std::optional<u32> DregsInInventory(u64 inventory) {
    if(inventory<0x10000 || !HasMemoryAccess(inventory,0x90,MemoryProt::CpuRead)) return {};
    const s32 last=ReadValue<s32>(inventory,0x88);
    const u32 split=ReadValue<u32>(inventory,0x24);
    if(last < -1 || last>=0x1000 || split>0x1000) return {};
    u32 count=0;
    for(s32 index=0;index<=last;++index) {
        const bool first=static_cast<u32>(index)<split;
        const u64 entries=ReadValue<u64>(inventory,first?0x58:0x48);
        const u64 entry=entries+16ull*(first?static_cast<u32>(index):static_cast<u32>(index)-split);
        if(entries<0x10000 || !HasMemoryAccess(entry,16,MemoryProt::CpuRead)) return {};
        if(ReadValue<u32>(entry,0) && ReadValue<u32>(entry,4)==(GoodsCategory|BloodborneCoop::BloodDregGoods)) {
            const u32 n=ReadValue<u32>(entry,8);
            if(n>99 || count>99-n) return {};
            count+=n;
        }
    }
    return count;
}

bool NativeSaveReady() {
    const u64 global=GetGlobalState();
    return global>=0x10000 && HasMemoryAccess(global+0x1490,0x1580-0x1490,MemoryProt::CpuRead) &&
        ReadValue<s32>(global,0x1490)==character_slot.load() && ReadValue<u8>(global,0x1578)!=0 &&
        std::ranges::equal(std::span<const u8>{reinterpret_cast<const u8*>(image_base+NativeSaveIdleOffset),NativeSaveIdleBytes.size()},NativeSaveIdleBytes) &&
        std::ranges::equal(std::span<const u8>{reinterpret_cast<const u8*>(image_base+NativeFreshSaveOffset),NativeFreshSaveBytes.size()},NativeFreshSaveBytes) &&
        reinterpret_cast<bool PS4_SYSV_ABI (*)()>(image_base+NativeSaveIdleOffset)();
}

std::string RewardScope() {
    std::string scope;
    for(const char* name:{"BB_SHADNET_SERVER","BB_SHADNET_SERVER_KEY","BB_SHADNET_NPID"}) {
        const char* value=std::getenv(name);
        const std::string part=value?value:"";
        scope+=std::to_string(part.size())+":"+part;
    }
    return scope;
}

// Deterministic real-game kill/restart probes are enabled only with the existing autoplay rig.
bool RewardTestPause(const char* point) {
    const char* rig=std::getenv("SHADPS4_BLOODBORNE_AUTOPLAY");
    const char* pause=std::getenv("BB_TEST_INVASION_PAUSE");
    if(!rig || !*rig || !pause || std::strcmp(pause,point)) return false;
    static std::set<std::string> reported;
    if(reported.insert(point).second) LOG_INFO(Debug,"Co-op atomic reward test pause: {}",point);
    return true;
}

struct AtomicReward {
    u64 id=0,data=0;
    int slot=-1;
    std::string scope,payload;
    s32 echoes=0,insight=0;
    std::optional<u32> dregs;
    bool queued=false,echoes_verified=false,insight_verified=false,dregs_verified=false;
    s64 retry_at=0;
};
std::optional<AtomicReward> atomic_reward;

// No TradeClient::Take until commit: its in-memory 'granted' set is not a durable receipt.
bool TakeAtomicReward(const bbcoop::TradeGrant& grant) {
    if(BloodborneCoop::TradeGrantItemsProblem(grant)) return false;
    const int slot=character_slot.load();
    if(slot<0 || slot>9) return false;
    const std::string scope=RewardScope(),payload=grant.SerializeAsString();
    const int recorded=bbseamless_reward_lookup(scope.c_str(),grant.id(),payload.data(),payload.size(),slot);
    if(recorded==1) {
        if(RewardTestPause("before-ack")) return false;
        if(atomic_reward && atomic_reward->id==grant.id()) atomic_reward.reset();
        LOG_INFO(Debug,"Co-op invasion reward: durable receipt {} acknowledged",grant.id());
        return true;
    }
    if(recorded<0 || (atomic_reward && (atomic_reward->id!=grant.id() || atomic_reward->payload!=payload || atomic_reward->scope!=scope))) return false;
    const u64 data=PlayerGameData(),global=GetGlobalState(),player=GetLocalPlayer();
    const u32 map=GetCurrentPackedMap();
    const bool ready=data>=0x10000 && global>=0x10000 && player>=0x10000 &&
        HasMemoryAccess(global,0x1521,MemoryProt::CpuRead) &&
        ReadValue<u8>(global,0x08)==0 && ReadValue<u8>(global,0x1520)==0 &&
        IsUsablePackedMap(map) && LocalPlayerPosition() && !HasPlayerEffect(player,9001) &&
        !Invading() && !PartyTripActive() && !guest_lamp_trip_at.has_value() &&
        pending_summon_reload_map.load(std::memory_order_acquire)==0 &&
        pending_summon_reload_state_map.load(std::memory_order_acquire)==0 &&
        HasMemoryAccess(data+0x80,0x18,MemoryProt::CpuRead);
    // The trip state can report home while the native return warp still owns the body.
    // As in the reference responder resume, wait for the native warp byte to clear and
    // the same live character/map to settle before adding transient Insight effect 4680.
    static u64 settled_player=0,settled_data=0;
    static u32 settled_map=0;
    static int settled_slot=-1;
    static s64 settled_since=-1;
    if(!ready) {settled_since=-1;return false;}
    const s64 now=SteadyMilliseconds();
    if(settled_since<0 || settled_player!=player || settled_data!=data || settled_map!=map || settled_slot!=slot) {
        settled_player=player;settled_data=data;settled_map=map;settled_slot=slot;settled_since=now;
    }
    if(!atomic_reward && now-settled_since<3'000) return false;
    if(atomic_reward && (atomic_reward->slot!=slot || atomic_reward->data!=data)) {
        // A different character loaded before an uncommitted award could be saved.
        if(!atomic_reward->queued && !bbseamless_reward_cancel(atomic_reward->id)) atomic_reward.reset();
        return false;
    }
    if(!atomic_reward) {
        if(!NativeSaveReady() || !HasVirtual(player,CharacterAddSpEffectSlot,AddSpEffectOffset,"atomic Insight") ||
           !HasVirtual(player,0x3D0,AddSoulsOffset,"atomic Echoes")) return false;
        AtomicReward pending{.id=grant.id(),.data=data,.slot=slot,.scope=scope,.payload=payload,
                             .echoes=ReadValue<s32>(data,0x94),.insight=std::min(ReadValue<s32>(data,0x84)+1,99)};
        const TradeItems items=TradeItemsOf(grant.items());
        for(const TradeItem& item:items) {
            if(item.kind==TradeItemKind::Echoes) pending.echoes=static_cast<s32>(std::min<u64>(u64(std::max(pending.echoes,0))+item.count,999'999'999));
            if(item.kind==TradeItemKind::Goods) {
                const u64 item_man=ReadValue<u64>(image_base+MapItemManPointerOffset,0);
                if(!HasVtable(item_man,MapItemManVtable,"atomic Blood Dreg")) return false;
                const auto counts=InvasionDregs();
                if(!counts || counts->first+counts->second>=198) return false;
                pending.dregs=counts->first+counts->second+1;
            }
        }
        if(bbseamless_reward_begin(scope.c_str(),grant.id(),payload.data(),payload.size(),slot)) return false;
        atomic_reward=std::move(pending); // Pins the old canonical save before any memory award.
        AddTradeItems(items);
        LOG_INFO(Debug,"Co-op invasion reward: {} applied once; waiting for native save",grant.id());
    }
    AtomicReward& pending=*atomic_reward;
    if(RewardTestPause("after-apply")) return false;
    pending.echoes_verified|=ReadValue<s32>(data,0x94)==pending.echoes;
    pending.insight_verified|=ReadValue<s32>(data,0x84)==pending.insight;
    const auto counts=pending.dregs?InvasionDregs():std::nullopt;
    pending.dregs_verified|=!pending.dregs || (counts && counts->first+counts->second==*pending.dregs);
    // Prove each native application once; later legitimate spending must not pin autosaves.
    if(!pending.echoes_verified || !pending.insight_verified || !pending.dregs_verified) return false;
    if(SteadyMilliseconds()<pending.retry_at) return false;
    if(!NativeSaveReady()) return false;
    if(pending.queued) {
        // A failed write/sync/swap cannot consume the grant. Retry a fresh snapshot, not the award.
        if(bbseamless_reward_disarm(grant.id())) return false;
        pending.queued=false;
    }
    if(bbseamless_reward_arm(grant.id(),slot)) return false;
    // Independently verified 1.09 native wrapper 0x1C2A9F0 uses (-1,0,0). This synchronously
    // serializes current game data, then queues a fresh 0x140000 character file. Idle is 1554=0.
    const bool saved=reinterpret_cast<bool PS4_SYSV_ABI (*)(s32,bool,bool)>(image_base+NativeFreshSaveOffset)(-1,false,false);
    if(!saved) {bbseamless_reward_disarm(grant.id());return false;}
    pending.queued=true;
    pending.retry_at=SteadyMilliseconds()+1'000;
    LOG_INFO(Debug,"Co-op invasion reward: {} fresh native save queued for slot {}",grant.id(),slot);
    return false; // Only runtime's durably swapped save+receipt may release the acknowledgement.
}
} // namespace

std::optional<std::pair<u32,u32>> InvasionDregs() {
    const u64 data=PlayerGameData();
    if(data<0x10000 || !HasMemoryAccess(data+0x5B0,8,MemoryProt::CpuRead)) return {};
    const auto held=DregsInInventory(data+InventoryOffset);
    const auto stored=DregsInInventory(ReadValue<u64>(data,0x5B0));
    if(!held || !stored) return {};
    return std::pair{*held,*stored};
}

void TickTrades() {
    if (!TradeSwitchOn() || !trade_sites.load()) {
        return;
    }
    RemoveBellItems();
    std::vector<TradeItems> add;
    std::vector<bbcoop::Envelope> replies;
    std::vector<bbcoop::Envelope> rewards;
    {
        std::scoped_lock lock{trade_mutex};
        // Grants wait while the player is not in the world (a load): the popup needs it.
        const bool can_add = GetLocalPlayer() >= 0x10000 && PlayerGameData() >= 0x10000;
        std::deque<bbcoop::Envelope> kept;
        for (bbcoop::Envelope& envelope : trade_inbox) {
            const bool reward=envelope.has_trade_grant() && envelope.trade_grant().reason()==static_cast<u32>(BloodborneCoop::TradeGrantReason::InvasionReward);
            if(reward) { rewards.push_back(std::move(envelope));continue; }
            if (envelope.has_trade_grant() && !can_add) {
                kept.push_back(std::move(envelope));
                continue;
            }
            TradeClient::Taken taken = client.Take(envelope);
            for (TradeItems& items : taken.add) {
                add.push_back(std::move(items));
            }
            for (bbcoop::Envelope& reply : taken.replies) {
                replies.push_back(std::move(reply));
            }
        }
        trade_inbox = std::move(kept);
        for (TradeClient::Notice& notice : client.TakeNotices()) {
            ++notices_seen[notice.kind];
            notices_to_show.push_back(std::move(notice));
            if (notices_to_show.size() > 16) {
                notices_to_show.pop_front(); // nobody shows them (no port menus)
            }
        }
    }
    for(bbcoop::Envelope& envelope:rewards) {
        if(TakeAtomicReward(envelope.trade_grant())) {
            bbcoop::Envelope ack;ack.mutable_trade_granted()->set_id(envelope.trade_grant().id());
            QueueInvasionRewardAck(std::move(ack));
        } else {
            std::scoped_lock lock{trade_mutex};trade_inbox.push_back(std::move(envelope));
        }
    }
    // Ordinary trades retain the reference path; invasion payouts use the transaction above.
    for (const TradeItems& items : add) {
        AddTradeItems(items);
    }
    for (bbcoop::Envelope& reply : replies) {
        if(reply.has_trade_granted() && (reply.trade_granted().id() & BloodborneCoop::InvasionRewardGrantBit))
            QueueInvasionRewardAck(std::move(reply));
        else QueueServerWorld(std::move(reply));
    }
    if (const auto tick = trade_ui_tick.load(); tick != nullptr && TradesOn()) {
        tick();
    }
}

void SetTradeUiTick(void (*tick)()) {
    trade_ui_tick.store(tick);
}

// A weapon's data (its object's own write): three fields, then five gem slots, each its shape and
// a handle, a gem in it whole (its item id, two fields and six effects).
namespace {

struct WeaponSlot {
    u32 shape = 0;
    u32 handle = 0;
    u32 gem = 0;
    std::array<u32, 8> fields{}; // +0x18, +0x1C, then the six effects
};
struct WeaponData {
    std::array<u32, 3> fields{};
    std::array<WeaponSlot, 5> slots{};
};

std::optional<WeaponData> ParseWeaponData(const std::string& bytes) {
    size_t at = 0;
    const auto next = [&](u32& value) {
        if (bytes.size() - at < 4) {
            return false;
        }
        std::memcpy(&value, bytes.data() + at, 4);
        at += 4;
        return true;
    };
    WeaponData weapon;
    for (u32& field : weapon.fields) {
        if (!next(field)) {
            return std::nullopt;
        }
    }
    for (WeaponSlot& slot : weapon.slots) {
        if (!next(slot.shape) || !next(slot.handle)) {
            return std::nullopt;
        }
        if (IsGaitemHandle(slot.handle)) {
            if (!next(slot.gem)) {
                return std::nullopt;
            }
            for (u32& field : slot.fields) {
                if (!next(field)) {
                    return std::nullopt;
                }
            }
        }
    }
    return at == bytes.size() ? std::optional{weapon} : std::nullopt;
}

std::string WeaponDataBytes(const WeaponData& weapon) {
    std::string bytes;
    const auto put = [&](u32 value) { bytes.append(reinterpret_cast<const char*>(&value), 4); };
    for (const u32 field : weapon.fields) {
        put(field);
    }
    for (const WeaponSlot& slot : weapon.slots) {
        put(slot.shape);
        put(slot.handle);
        if (IsGaitemHandle(slot.handle)) {
            put(slot.gem);
            for (const u32 field : slot.fields) {
                put(field);
            }
        }
    }
    return bytes;
}

// The objects of the player's weapons of an id.
std::vector<u64> WeaponObjects(u32 weapon) {
    std::vector<u64> objects;
    const u64 data = PlayerGameData();
    if (data < 0x10000) {
        return objects;
    }
    for (const u64 entry : ItemEntries(data, weapon)) {
        if (const u64 object = GaitemOf(ReadValue<u32>(entry, 0)); object != 0) {
            objects.push_back(object);
        }
    }
    return objects;
}

} // namespace

bool SocketTradeGem(u32 weapon, u32 gem, u32 effect) {
    const std::vector<u64> objects = WeaponObjects(weapon);
    if (objects.empty()) {
        return false;
    }
    const std::optional<std::string> written = WriteGaitem(objects.front());
    std::optional<WeaponData> data = written ? ParseWeaponData(*written) : std::nullopt;
    if (!data) {
        return false;
    }
    WeaponSlot& slot = data->slots.front();
    slot.handle = 0xC0800000; // a gem object: the read makes it anew
    slot.gem = gem;
    slot.fields = {0, 0, effect, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF};
    if (!ReadGaitem(objects.front(), WeaponDataBytes(*data))) {
        return false;
    }
    LOG_INFO(Debug, "Bloodborne trade: gem {:#x} ({}) in weapon {:#x}", gem, effect, weapon);
    return true;
}

bool HasSocketedGem(u32 weapon, u32 gem, u32 effect) {
    for (const u64 object : WeaponObjects(weapon)) {
        const std::optional<std::string> written = WriteGaitem(object);
        const std::optional<WeaponData> data = written ? ParseWeaponData(*written) : std::nullopt;
        if (data && std::ranges::any_of(data->slots, [&](const WeaponSlot& slot) {
                return IsGaitemHandle(slot.handle) && slot.gem == gem && slot.fields[2] == effect;
            })) {
            return true;
        }
    }
    return false;
}

std::string TradeAskQuestion(const bbcoop::TradeAsk& ask) {
    const std::string from = ask.from_name().empty() ? TradeName(ask.from()) : ask.from_name();
    return TradeAskText(from, TradeItemsOf(ask.give()), TradeItemsOf(ask.want()));
}

} // namespace Coop

} // namespace Core::Bloodborne
