// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <string>
#include <string_view>

#include "core/bloodborne_altar.h"
#include "core/bloodborne_party.h"

namespace Core::Bloodborne {

namespace {

// The chalice dungeons' map folders of the game's files (dvdroot_ps4/map/mapstudio/
// m29_<key>_<variant>_00, patch 1.09) and the highest layout file each has (m29_<key>_<variant>_
// <layout>.msb.dcx, every one from 00): the story variant 90 of each standard key, the root
// variants of each depth and area. The m29_0x_* folders are no chalice's.
struct ChaliceMaps {
    u8 key;
    u8 variant;
    u8 last_layout;
};
constexpr std::array<ChaliceMaps, 33> ChaliceMapFolders{{
    {10, 0, 99},  {10, 90, 1}, {20, 0, 99},  {20, 90, 1},  {21, 0, 99},  {21, 90, 0},  {30, 0, 99},
    {30, 20, 99}, {30, 90, 1}, {31, 0, 99},  {31, 20, 99}, {31, 90, 0},  {40, 10, 99}, {40, 11, 99},
    {40, 90, 1},  {42, 0, 99}, {42, 1, 99},  {42, 90, 1},  {50, 0, 99},  {50, 1, 99},  {50, 20, 99},
    {50, 21, 99}, {50, 90, 1}, {52, 0, 99},  {52, 1, 99},  {52, 20, 99}, {52, 21, 99}, {52, 90, 1},
    {53, 0, 99},  {53, 1, 99}, {53, 20, 99}, {53, 21, 99}, {53, 90, 1},
}};

constexpr std::array<u8, 10> StandardKeys{10, 20, 21, 30, 31, 40, 42, 50, 52, 53};
constexpr u32 StandardChannelBase = 2'000'000'000;
constexpr u8 StoryVariant = 90;

u32 BigEndianWord(std::span<const u8> record, size_t word) {
    u32 value = 0;
    for (size_t byte = 0; byte < 4; ++byte) {
        value = (value << 8) | record[word * 4 + byte];
    }
    return value;
}

// A ritual's record mirrors the server's FormData (bloodborne_backend.cpp, StandardDungeon) from
// its fourth byte on, every 32-bit word big endian: the rig's altar-record-capture (Pthumeru
// Chalice, 2026-10-24) logged 1D 0A 5A 00, the ChannelId, 0, then FormData's 1D 0A 5A 00, the goods
// (6109), its nine feature slots (id, code), "Hunter" twice, 02 00 30 00, the glyph's length and
// text ("chs2c"), and the server's date (00 18 37 DF) twice. Each standard chalice's slots are the
// game's own for it (HolygrailExParam row <goods>, its eight slot ids; DungeonSubFeatLotParam
// gives each id its one code), as the server has them.
struct FeatureSlot {
    u32 id = 0xFFFFFFFFu; // none
    u8 code = 0xFF;
    u8 flag = 0; // the code's DungeonFeatureParam flag, 5300 + this in the record
};
struct StandardChalice {
    u8 key;
    u32 goods;
    std::array<FeatureSlot, 9> slots;
};
constexpr FeatureSlot No{};
constexpr FeatureSlot S90{995'010'110, 90, 78};
constexpr FeatureSlot S91{995'010'120, 91, 0};
constexpr FeatureSlot S93{995'010'140, 93, 0};
constexpr FeatureSlot S94{995'010'220, 94, 78};
constexpr FeatureSlot S96{995'010'340, 96, 78};
constexpr FeatureSlot S45{994'500'000, 45, 45};
constexpr FeatureSlot S10{991'000'000, 10, 10};
constexpr FeatureSlot S62{996'200'000, 62, 0};
constexpr std::array<StandardChalice, 10> StandardChalices{{
    {10, 6109, {{No, No, S90, No, No, S45}}},
    {20, 6209, {{No, No, S90, S91, No, S45}}},
    {21, 6219, {{No, No, S94, S10, No, S45}}},
    {30, 6309, {{{996'100'000, 61, 60}, {998'500'000, 85, 85}, S90, S91, No, S45}}},
    {31, 6319, {{No, {998'600'000, 86, 86}, S94, {995'010'230, 95, 0}, S10, S45}}},
    {40, 6409, {{S62, {997'000'000, 70, 70}, S90, S91, S93, S45}}},
    {42, 6429, {{No, No, S96, No, No, S45}}},
    {50, 6509, {{S62, {998'700'000, 87, 87}, S90, S91, S93, S45}}},
    {52, 6529, {{No, {998'800'000, 88, 88}, S96, {995'010'350, 97, 0}, No, S45}}},
    {53, 6539, {{S90, {998'900'000, 89, 89}, {995'010'450, 98, 78}, S91, S94, S45}}},
}};
constexpr std::string_view GlyphAlphabet = "23456789abcdefghijkmnpqrstuvwxyz";
// An empty altar's record: FF FF FF FF, 0, 80 00 00 00, FF up to the conductor, zeros from there
// but 02 00 30 00 at byte 124 (the capture's untouched altars).
constexpr size_t ConductorAt = 92;
constexpr size_t SettingsAt = 124;
constexpr size_t GlyphAt = 128;
constexpr size_t DatesAt = 164;
constexpr size_t SlotsAt = 20;
// Bytes from 625 (flags 5000-5999: "a chalice on it" 5001, the layers' switches) are the
// dungeon's state, not its name: a rebuild keeps them (the merge of 2026-10-24 left them right).
constexpr size_t StateAt = 625;
// But for flags 5310-5399, the slots' features: a ritual clears them and sets 5300 + each code's
// DungeonFeatureParam flag (eboot 0x01AB25A2). The chalice reward (m29 event 12901693) gives the
// next chalice only with 5385 or 5378 on, which only a story chalice's own features set.
constexpr s32 FeatureFlagsFrom = 5310;
constexpr s32 FeatureFlagsTo = 5399;
constexpr s32 FeatureFlagBase = 5300;

void PutBigEndian(AltarRecord& record, size_t at, u32 value) {
    for (size_t byte = 0; byte < 4; ++byte) {
        record[at + byte] = static_cast<u8>(value >> (24 - 8 * byte));
    }
}

// Text the way the record holds it: each 4-byte group reversed (a little endian word put big
// endian).
void PutText(AltarRecord& record, size_t at, std::string_view text, size_t size) {
    for (size_t index = 0; index < size; ++index) {
        const size_t source = (index & ~size_t{3}) + (3 - (index & 3));
        record[at + index] = source < text.size() ? static_cast<u8>(text[source]) : 0;
    }
}

void PutFlag(AltarRecord& record, s32 flag, bool on) {
    const u8 bit = static_cast<u8>(0x80 >> (flag % 8));
    record[flag / 8] = on ? record[flag / 8] | bit : record[flag / 8] & ~bit;
}

void PutSlots(AltarRecord& record, const StandardChalice& chalice) {
    for (size_t slot = 0; slot < chalice.slots.size(); ++slot) {
        const FeatureSlot& feature = chalice.slots[slot];
        PutBigEndian(record, SlotsAt + slot * 8, feature.id);
        PutBigEndian(record, SlotsAt + 4 + slot * 8,
                     feature.code == 0xFF ? 0xFFFFFFFFu : feature.code);
    }
}

void PutFeatures(AltarRecord& record, const StandardChalice& chalice) {
    for (s32 flag = FeatureFlagsFrom; flag <= FeatureFlagsTo; ++flag) {
        PutFlag(record, flag, false);
    }
    for (const FeatureSlot& slot : chalice.slots) {
        if (slot.code != 0xFF) {
            PutFlag(record, FeatureFlagBase + slot.flag, true);
        }
    }
}

} // namespace

std::optional<s32> AltarOfFlag(s32 flag) {
    if (flag < AltarFlagBase(0) || flag >= AltarFlagBase(RitualAltarCount)) {
        return std::nullopt;
    }
    const s32 altar = (flag - AltarFlagBase(0)) / 100000;
    return flag - AltarFlagBase(altar) < AltarRecordFlags ? std::optional{altar} : std::nullopt;
}

std::optional<AltarDungeon> AltarRecordDungeon(std::span<const u8> record) {
    if (record.size() < 4 || record[0] != 0x1D) {
        return std::nullopt;
    }
    return AltarDungeon{record[1], record[2], record[3]};
}

bool ChaliceDungeonExists(const AltarDungeon& dungeon) {
    return std::ranges::any_of(ChaliceMapFolders, [&dungeon](const ChaliceMaps& folder) {
        return folder.key == dungeon.key && folder.variant == dungeon.variant &&
               dungeon.layout <= folder.last_layout;
    });
}

AltarRecordKind ClassifyAltarRecord(std::span<const u8> record) {
    if (record.size() < 8) {
        return AltarRecordKind::Broken;
    }
    if (std::ranges::all_of(record.first(4), [](u8 byte) { return byte == 0xFF; })) {
        return AltarRecordKind::Empty;
    }
    const auto dungeon = AltarRecordDungeon(record);
    return dungeon && ChaliceDungeonExists(*dungeon) ? AltarRecordKind::Dungeon
                                                     : AltarRecordKind::Broken;
}

std::optional<u8> StandardChannelKey(std::span<const u8> record) {
    if (record.size() < 8) {
        return std::nullopt;
    }
    const u32 channel = BigEndianWord(record, 1);
    for (const u8 key : StandardKeys) {
        if (channel == StandardChannelBase + key) {
            return key;
        }
    }
    return std::nullopt;
}

std::vector<std::pair<s32, bool>> AltarRecordChanges(s32 altar, const AltarRecord& record) {
    std::vector<std::pair<s32, bool>> changes;
    changes.reserve(AltarRecordFlags);
    const s32 base = AltarFlagBase(altar);
    for (size_t byte = 0; byte < record.size(); ++byte) {
        for (u32 bit = 0; bit < 8; ++bit) {
            changes.emplace_back(base + static_cast<s32>(byte * 8 + bit),
                                 (record[byte] & (0x80 >> bit)) != 0);
        }
    }
    return changes;
}

bool SharesAltarRecord(const AltarShareFrame& frame) {
    return frame.playing && frame.map == HuntersDreamMap &&
           frame.since_place_ms >= AltarShareSettleMs &&
           frame.since_own_write_ms >= AltarOwnWriteQuietMs;
}

AltarRepair RepairAltarRecord(std::span<const u8> record) {
    AltarRepair repair;
    const AltarRecordKind kind = ClassifyAltarRecord(record);
    if (kind == AltarRecordKind::Dungeon && record.size() == AltarRecordBytes) {
        // A standard chalice's story dungeon from a server before 2026-10-25 (root features): its
        // slots and features the chalice's own, all else kept (its layout's files are loaded).
        const auto key = StandardChannelKey(record);
        const auto chalice = key ? std::ranges::find(StandardChalices, *key, &StandardChalice::key)
                                 : StandardChalices.end();
        const auto dungeon = AltarRecordDungeon(record);
        if (chalice != StandardChalices.end() && dungeon && dungeon->key == *key &&
            dungeon->variant == StoryVariant) {
            std::ranges::copy(record, repair.record.begin());
            PutSlots(repair.record, *chalice);
            PutFeatures(repair.record, *chalice);
            if (!std::ranges::equal(repair.record, record)) {
                repair.action = AltarRepair::Action::Features;
            }
        }
        return repair;
    }
    if (kind != AltarRecordKind::Broken) {
        return repair;
    }
    if (const auto key = StandardChannelKey(record)) {
        if (const auto rebuilt = StandardAltarRecord(*key, record)) {
            repair.action = AltarRepair::Action::Rebuild;
            repair.record = *rebuilt;
            return repair;
        }
    }
    repair.action = AltarRepair::Action::Clear;
    repair.record = EmptyAltarRecord();
    // The ritual took the chalice the record names (FormData's goods, byte 16).
    if (record.size() >= 20) {
        const u32 goods = BigEndianWord(record, 4);
        if (goods >= 6100 && goods <= 6599) {
            repair.chalice = goods;
        }
    }
    return repair;
}

std::optional<AltarRecord> StandardAltarRecord(u8 key, std::span<const u8> state) {
    const auto chalice = std::ranges::find(StandardChalices, key, &StandardChalice::key);
    if (chalice == StandardChalices.end()) {
        return std::nullopt;
    }
    AltarRecord record = EmptyAltarRecord();
    const u32 name = 0x1D000000u | u32{key} << 16 | u32{StoryVariant} << 8;
    PutBigEndian(record, 0, name);
    PutBigEndian(record, 4, StandardChannelBase + key);
    PutBigEndian(record, 8, 0);
    PutBigEndian(record, 12, name);
    PutBigEndian(record, 16, chalice->goods);
    PutSlots(record, *chalice);
    PutText(record, ConductorAt, "Hunter", 16);
    PutText(record, ConductorAt + 16, "Hunter", 16);
    PutBigEndian(record, GlyphAt, 6);
    const std::string glyph{'c', 'h', 's', GlyphAlphabet[key / 32], GlyphAlphabet[key % 32]};
    PutText(record, GlyphAt + 4, glyph, 8);
    PutBigEndian(record, DatesAt, 0x001837DF);
    PutBigEndian(record, DatesAt + 4, 0x001837DF);
    if (state.size() == AltarRecordBytes) {
        std::copy(state.begin() + StateAt, state.end(), record.begin() + StateAt);
    } else {
        // A ritual's own (Pthumeru's three layers): a chalice on it, its layers.
        for (const s32 flag : {5001, 5002, 5100, 5200, 5300, 5500}) {
            PutFlag(record, flag, true);
        }
    }
    PutFeatures(record, *chalice);
    return record;
}

AltarRecord EmptyAltarRecord() {
    AltarRecord record{};
    std::fill(record.begin(), record.begin() + ConductorAt, u8{0xFF});
    PutBigEndian(record, 4, 0);
    PutBigEndian(record, 8, 0x80000000u);
    PutBigEndian(record, SettingsAt, 0x02003000u);
    return record;
}

} // namespace Core::Bloodborne
