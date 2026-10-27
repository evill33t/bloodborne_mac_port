// SPDX-FileCopyrightText: Copyright 2026 shadNet Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "coop/world_log.h"

#include <iterator>

namespace BloodborneCoop {

namespace {
template <typename T>
void AppendLittleEndian(std::vector<u8>& bytes, T value) {
    for (size_t index = 0; index < sizeof(T); ++index) {
        bytes.push_back(static_cast<u8>(static_cast<u64>(value) >> (8 * index)));
    }
}

template <typename T>
T ReadLittleEndian(std::span<const u8> bytes, size_t offset) {
    u64 value = 0;
    for (size_t index = 0; index < sizeof(T); ++index) {
        value |= u64{bytes[offset + index]} << (8 * index);
    }
    return static_cast<T>(value);
}
} // namespace

std::vector<EnemyHpReport> MergeEnemyHpReports(std::span<const EnemyHpReport> reports) {
    std::vector<EnemyHpReport> merged;
    for (const EnemyHpReport& report : reports) {
        const auto same = std::ranges::find(merged, report.visit, &EnemyHpReport::visit);
        if (same == merged.end()) {
            merged.push_back(report);
            continue;
        }
        const bool hp = (report.visit & HostHpReport) != 0;
        for (const EnemyHp& enemy : report.enemies) {
            const auto mine = std::ranges::find(same->enemies, enemy.handle, &EnemyHp::handle);
            if (mine == same->enemies.end()) {
                same->enemies.push_back(enemy);
            } else if (hp ? enemy.hp < mine->hp : enemy.hp > mine->hp) {
                *mine = enemy;
            }
        }
    }
    return merged;
}

std::vector<u8> EncodeEnemyHp(u64 visit, std::span<const EnemyHp> enemies) {
    std::vector<u8> bytes;
    bytes.reserve(8 + enemies.size() * 12);
    AppendLittleEndian(bytes, visit);
    for (const EnemyHp& enemy : enemies) {
        AppendLittleEndian(bytes, enemy.handle);
        AppendLittleEndian(bytes, static_cast<u32>(enemy.hp));
        AppendLittleEndian(bytes, static_cast<u32>(enemy.max));
    }
    return bytes;
}

std::optional<EnemyHpReport> DecodeEnemyHp(std::span<const u8> bytes) {
    if (bytes.size() < 8 || (bytes.size() - 8) % 12 != 0) {
        return std::nullopt;
    }
    EnemyHpReport report{ReadLittleEndian<u64>(bytes, 0), {}};
    for (size_t offset = 8; offset < bytes.size(); offset += 12) {
        report.enemies.push_back({ReadLittleEndian<u32>(bytes, offset),
                                  static_cast<s32>(ReadLittleEndian<u32>(bytes, offset + 4)),
                                  static_cast<s32>(ReadLittleEndian<u32>(bytes, offset + 8))});
    }
    return report;
}

std::vector<u8> EncodeShownCharacters(std::span<const CharacterShown> characters) {
    std::vector<u8> bytes;
    bytes.reserve(characters.size() * 5);
    for (const CharacterShown& character : characters) {
        AppendLittleEndian(bytes, character.handle);
        bytes.push_back(ShownBits(character));
    }
    return bytes;
}

std::optional<std::vector<CharacterShown>> DecodeShownCharacters(std::span<const u8> bytes) {
    if (bytes.size() % 5 != 0) {
        return std::nullopt;
    }
    std::vector<CharacterShown> characters;
    for (size_t offset = 0; offset < bytes.size(); offset += 5) {
        const u8 state = bytes[offset + 4];
        characters.push_back(ShownFromBits(ReadLittleEndian<u32>(bytes, offset), state));
    }
    return characters;
}

u8 ShownBits(const CharacterShown& character) {
    return static_cast<u8>((character.hidden ? 1 : 0) | (character.disabled ? 2 : 0) |
                           (character.distant ? 4 : 0) | (character.dead ? 8 : 0) |
                           (character.hits_off ? 16 : 0));
}

CharacterShown ShownFromBits(u32 handle, u8 bits) {
    return {handle,          (bits & 1) != 0, (bits & 2) != 0,
            (bits & 4) != 0, (bits & 8) != 0, (bits & 16) != 0};
}

std::vector<CharacterShown> WorldLogShownCharacters(const WorldLogShown& shown) {
    std::vector<CharacterShown> characters;
    characters.reserve(shown.bits.size());
    for (const auto& [handle, bits] : shown.bits) {
        characters.push_back(ShownFromBits(handle, bits));
    }
    return characters;
}

size_t WorldLogKillCount(const WorldLogKills& kills) {
    size_t count = 0;
    for (const auto& [map, handles] : kills) {
        count += handles.size();
    }
    return count;
}

std::unordered_set<u32> WorldLogHandles(const WorldLogKills& kills) {
    std::unordered_set<u32> handles;
    for (const auto& [map, on_map] : kills) {
        handles.insert(on_map.begin(), on_map.end());
    }
    return handles;
}

u64 WorldLogEpoch(u32 run, u32 count) {
    return (u64{run} << 32) | count;
}

bool WorldLogEpochFollows(u64 next, u64 current) {
    if (next == 0) {
        return false;
    }
    if (current == 0 || (next >> 32) != (current >> 32)) {
        return true;
    }
    return static_cast<u32>(next) > static_cast<u32>(current);
}

bool IsWorldLogFrameType(u8 type) {
    return type >= WorldLogProposeType && type <= WorldLogFlagsSnapshotType;
}

bool WorldLogEnabled(const char* setting) {
    const std::string_view value = setting != nullptr ? setting : "";
    return value != "0" && value != "off";
}

namespace {

// Reads a world log payload front to back; a read past the end fails it.
class PayloadReader {
public:
    explicit PayloadReader(std::span<const u8> bytes) : bytes{bytes} {}
    template <typename T>
    std::optional<T> Next() {
        if (failed || at + sizeof(T) > bytes.size()) {
            failed = true;
            return std::nullopt;
        }
        const T value = ReadLittleEndian<T>(bytes, at);
        at += sizeof(T);
        return value;
    }
    std::optional<std::string> Name() {
        const auto size = Next<u8>();
        if (!size || at + *size > bytes.size()) {
            failed = true;
            return std::nullopt;
        }
        std::string name(bytes.begin() + at, bytes.begin() + at + *size);
        at += *size;
        return name;
    }
    // Everything read, and nothing left over.
    bool Done() const {
        return !failed && at == bytes.size();
    }

private:
    std::span<const u8> bytes;
    size_t at = 0;
    bool failed = false;
};

void AppendName(std::vector<u8>& payload, std::string_view name) {
    const size_t size = std::min<size_t>(name.size(), 0xFF);
    payload.push_back(static_cast<u8>(size));
    payload.insert(payload.end(), name.begin(), name.begin() + size);
}

// u16 maps, then per map: u32 map, u16 count, the handles (u32). At most WorldLogHandlesPerFrame
// handles in all.
void AppendKills(std::vector<u8>& payload, const WorldLogKills& kills) {
    std::vector<std::pair<u32, std::vector<u32>>> kept;
    size_t room = WorldLogHandlesPerFrame;
    for (const auto& [map, handles] : kills) {
        const auto end =
            std::next(handles.begin(), static_cast<std::ptrdiff_t>(std::min(room, handles.size())));
        std::vector<u32> some(handles.begin(), end);
        room -= some.size();
        if (!some.empty()) {
            kept.emplace_back(map, std::move(some));
        }
    }
    AppendLittleEndian(payload, static_cast<u16>(kept.size()));
    for (const auto& [map, handles] : kept) {
        AppendLittleEndian(payload, map);
        AppendLittleEndian(payload, static_cast<u16>(handles.size()));
        for (const u32 handle : handles) {
            AppendLittleEndian(payload, handle);
        }
    }
}

std::optional<WorldLogKills> ReadKills(PayloadReader& reader) {
    const auto maps = reader.Next<u16>();
    if (!maps) {
        return std::nullopt;
    }
    WorldLogKills kills;
    for (u16 index = 0; index < *maps; ++index) {
        const auto map = reader.Next<u32>();
        const auto count = reader.Next<u16>();
        if (!map || !count || *count == 0) {
            return std::nullopt;
        }
        std::set<u32>& handles = kills[*map];
        for (u16 at = 0; at < *count; ++at) {
            const auto handle = reader.Next<u32>();
            if (!handle) {
                return std::nullopt;
            }
            handles.insert(*handle);
        }
    }
    return kills;
}

} // namespace

std::vector<u8> EncodeWorldLogProposal(const WorldLogProposal& proposal) {
    std::vector<u8> payload;
    AppendName(payload, proposal.member);
    AppendLittleEndian(payload, proposal.epoch);
    AppendLittleEndian(payload, proposal.run);
    AppendLittleEndian(payload, proposal.number);
    AppendKills(payload, proposal.kills);
    return payload;
}

std::optional<WorldLogProposal> DecodeWorldLogProposal(std::span<const u8> payload) {
    PayloadReader reader{payload};
    auto member = reader.Name();
    const auto epoch = reader.Next<u64>();
    const auto run = reader.Next<u32>();
    const auto number = reader.Next<u32>();
    auto kills = ReadKills(reader);
    if (!member || !epoch || !run || !number || !kills || !reader.Done()) {
        return std::nullopt;
    }
    return WorldLogProposal{std::move(*member), *epoch, *run, *number, std::move(*kills)};
}

// The host's name, epoch, last seq, the entry count (u8) and the first entry's seq (u64), then
// each entry: kind (u8; a Shown's low four bits in its low half, see WorldLogKind), map, subject,
// value (a Shown's bits, all of them).
std::vector<u8> EncodeWorldLogBatch(const WorldLogBatch& batch) {
    std::vector<u8> payload;
    AppendName(payload, batch.host);
    AppendLittleEndian(payload, batch.epoch);
    AppendLittleEndian(payload, batch.last);
    const size_t count = std::min<size_t>(batch.entries.size(), 0xFF);
    payload.push_back(static_cast<u8>(count));
    AppendLittleEndian(payload, count > 0 ? batch.entries.front().seq : batch.last + 1);
    for (size_t index = 0; index < count; ++index) {
        const WorldLogEntry& entry = batch.entries[index];
        payload.push_back(entry.kind == WorldLogKind::Shown
                              ? static_cast<u8>(static_cast<u8>(entry.kind) | (entry.value & 0x0F))
                              : static_cast<u8>(entry.kind));
        AppendLittleEndian(payload, entry.map);
        AppendLittleEndian(payload, entry.subject);
        AppendLittleEndian(payload, entry.value);
    }
    return payload;
}

std::optional<WorldLogBatch> DecodeWorldLogBatch(std::span<const u8> payload) {
    PayloadReader reader{payload};
    auto host = reader.Name();
    const auto epoch = reader.Next<u64>();
    const auto last = reader.Next<u64>();
    const auto count = reader.Next<u8>();
    const auto first = reader.Next<u64>();
    if (!host || !epoch || !last || !count || !first ||
        (*count > 0 && (*first == 0 || *first + *count - 1 > *last))) {
        return std::nullopt;
    }
    WorldLogBatch batch{std::move(*host), *epoch, *last, {}};
    for (u8 index = 0; index < *count; ++index) {
        const auto kind = reader.Next<u8>();
        const auto map = reader.Next<u32>();
        const auto subject = reader.Next<u32>();
        const auto value = reader.Next<u32>();
        if (!kind || !map || !subject || !value) {
            return std::nullopt;
        }
        const bool shown = (*kind & 0xF0) == static_cast<u8>(WorldLogKind::Shown);
        batch.entries.push_back({*first + index, *epoch, *map,
                                 shown ? WorldLogKind::Shown : static_cast<WorldLogKind>(*kind),
                                 *subject, *value});
    }
    if (!reader.Done()) {
        return std::nullopt;
    }
    return batch;
}

std::vector<u8> EncodeWorldLogSnapshotRequest(const WorldLogSnapshotRequest& request) {
    std::vector<u8> payload;
    AppendName(payload, request.member);
    AppendLittleEndian(payload, request.epoch);
    AppendLittleEndian(payload, request.seq);
    return payload;
}

std::optional<WorldLogSnapshotRequest> DecodeWorldLogSnapshotRequest(std::span<const u8> payload) {
    PayloadReader reader{payload};
    auto member = reader.Name();
    const auto epoch = reader.Next<u64>();
    const auto seq = reader.Next<u64>();
    if (!member || !epoch || !seq || !reader.Done()) {
        return std::nullopt;
    }
    return WorldLogSnapshotRequest{std::move(*member), *epoch, *seq};
}

std::vector<u8> EncodeWorldLogSnapshot(const WorldLogSnapshot& snapshot) {
    std::vector<u8> payload;
    AppendName(payload, snapshot.host);
    AppendLittleEndian(payload, snapshot.epoch);
    AppendLittleEndian(payload, snapshot.last);
    payload.push_back(snapshot.part);
    payload.push_back(snapshot.parts);
    AppendKills(payload, snapshot.kills);
    return payload;
}

std::optional<WorldLogSnapshot> DecodeWorldLogSnapshot(std::span<const u8> payload) {
    PayloadReader reader{payload};
    auto host = reader.Name();
    const auto epoch = reader.Next<u64>();
    const auto last = reader.Next<u64>();
    const auto part = reader.Next<u8>();
    const auto parts = reader.Next<u8>();
    auto kills = ReadKills(reader);
    if (!host || !epoch || !last || !part || !parts || *part >= *parts || !kills ||
        !reader.Done()) {
        return std::nullopt;
    }
    return WorldLogSnapshot{std::move(*host), *epoch, *last, *part, *parts, std::move(*kills)};
}

// The host's name, epoch, last seq, part, parts, the map, the count (u16), then each character:
// handle, bits (u8).
std::vector<u8> EncodeWorldLogShownSnapshot(const WorldLogShownSnapshot& snapshot) {
    std::vector<u8> payload;
    AppendName(payload, snapshot.host);
    AppendLittleEndian(payload, snapshot.epoch);
    AppendLittleEndian(payload, snapshot.last);
    payload.push_back(snapshot.part);
    payload.push_back(snapshot.parts);
    AppendLittleEndian(payload, snapshot.shown.map);
    const size_t count = std::min(snapshot.shown.bits.size(), WorldLogShownPerFrame);
    AppendLittleEndian(payload, static_cast<u16>(count));
    size_t written = 0;
    for (const auto& [handle, bits] : snapshot.shown.bits) {
        if (written++ == count) {
            break;
        }
        AppendLittleEndian(payload, handle);
        payload.push_back(bits);
    }
    return payload;
}

std::optional<WorldLogShownSnapshot> DecodeWorldLogShownSnapshot(std::span<const u8> payload) {
    PayloadReader reader{payload};
    auto host = reader.Name();
    const auto epoch = reader.Next<u64>();
    const auto last = reader.Next<u64>();
    const auto part = reader.Next<u8>();
    const auto parts = reader.Next<u8>();
    const auto map = reader.Next<u32>();
    const auto count = reader.Next<u16>();
    if (!host || !epoch || !last || !part || !parts || *part >= *parts || !map || !count) {
        return std::nullopt;
    }
    WorldLogShownSnapshot snapshot{std::move(*host), *epoch, *last, *part, *parts, {*map, {}}};
    for (u16 index = 0; index < *count; ++index) {
        const auto handle = reader.Next<u32>();
        const auto bits = reader.Next<u8>();
        if (!handle || !bits) {
            return std::nullopt;
        }
        snapshot.shown.bits[*handle] = *bits;
    }
    if (!reader.Done()) {
        return std::nullopt;
    }
    return snapshot;
}

void WorldLogFlagOutbox::Add(std::span<const std::pair<s32, bool>> changes, s64 now_ms) {
    for (const auto& [flag, on] : changes) {
        sent.push_back({{++next, flag, on}, now_ms});
        fresh = true;
    }
}

std::vector<WorldLogFlagEvent> WorldLogFlagOutbox::Pending(s64 now_ms) {
    std::erase_if(sent, [now_ms](const auto& entry) {
        return now_ms - entry.second > WorldLogFlagResendWindowMs;
    });
    // The ones not out yet first, oldest first (the host takes rising numbers); then the newest
    // frame again.
    std::vector<WorldLogFlagEvent> events;
    for (const auto& [event, at] : sent) {
        if (event.number > out_to && events.size() < WorldLogFlagEventsPerFrame) {
            events.push_back(event);
        }
    }
    if (!events.empty()) {
        out_to = events.back().number;
        return events;
    }
    const size_t from =
        sent.size() > WorldLogFlagEventsPerFrame ? sent.size() - WorldLogFlagEventsPerFrame : 0;
    for (size_t index = from; index < sent.size(); ++index) {
        events.push_back(sent[index].first);
    }
    return events;
}

bool FlagEventOnce::First(s32 flag, bool on, s64 now_ms) {
    std::erase_if(seen, [now_ms](const auto& entry) {
        return now_ms - entry.second >= WorldLogFlagRepeatMs;
    });
    return seen.try_emplace({flag, on}, now_ms).second;
}

// The host's name, epoch, last seq, the map, the bits' length (u16), the bits, then the host's
// return point (s32) and its map (since protocol 8).
std::vector<u8> EncodeWorldLogMapStateSnapshot(const WorldLogMapStateSnapshot& snapshot) {
    std::vector<u8> payload;
    AppendName(payload, snapshot.host);
    AppendLittleEndian(payload, snapshot.epoch);
    AppendLittleEndian(payload, snapshot.last);
    AppendLittleEndian(payload, snapshot.state.map);
    AppendLittleEndian(payload, static_cast<u16>(snapshot.state.bits.size()));
    payload.insert(payload.end(), snapshot.state.bits.begin(), snapshot.state.bits.end());
    AppendLittleEndian(payload, static_cast<u32>(snapshot.return_point.point));
    AppendLittleEndian(payload, snapshot.return_point.map);
    return payload;
}

std::optional<WorldLogMapStateSnapshot> DecodeWorldLogMapStateSnapshot(
    std::span<const u8> payload) {
    PayloadReader reader{payload};
    auto host = reader.Name();
    const auto epoch = reader.Next<u64>();
    const auto last = reader.Next<u64>();
    const auto map = reader.Next<u32>();
    const auto size = reader.Next<u16>();
    if (!host || !epoch || !last || !map || !size) {
        return std::nullopt;
    }
    WorldLogMapStateSnapshot snapshot{std::move(*host), *epoch, *last, {*map, {}}};
    for (u16 index = 0; index < *size; ++index) {
        const auto byte = reader.Next<u8>();
        if (!byte) {
            return std::nullopt;
        }
        snapshot.state.bits.push_back(*byte);
    }
    const auto point = reader.Next<u32>();
    const auto point_map = reader.Next<u32>();
    if (!point || !point_map || !reader.Done()) {
        return std::nullopt;
    }
    snapshot.return_point = {static_cast<s32>(*point), *point_map};
    return snapshot;
}

// The member's name, epoch, run, the count (u8), then each event: number, flag, on (u8). At most
// the newest WorldLogFlagEventsPerFrame.
std::vector<u8> EncodeWorldLogFlagProposal(const WorldLogFlagProposal& proposal) {
    std::vector<u8> payload;
    AppendName(payload, proposal.member);
    AppendLittleEndian(payload, proposal.epoch);
    AppendLittleEndian(payload, proposal.run);
    const size_t from = proposal.events.size() > WorldLogFlagEventsPerFrame
                            ? proposal.events.size() - WorldLogFlagEventsPerFrame
                            : 0;
    payload.push_back(static_cast<u8>(proposal.events.size() - from));
    for (size_t index = from; index < proposal.events.size(); ++index) {
        const WorldLogFlagEvent& event = proposal.events[index];
        AppendLittleEndian(payload, event.number);
        AppendLittleEndian(payload, static_cast<u32>(event.flag));
        payload.push_back(event.on ? 1 : 0);
    }
    return payload;
}

std::optional<WorldLogFlagProposal> DecodeWorldLogFlagProposal(std::span<const u8> payload) {
    PayloadReader reader{payload};
    auto member = reader.Name();
    const auto epoch = reader.Next<u64>();
    const auto run = reader.Next<u32>();
    const auto count = reader.Next<u8>();
    if (!member || !epoch || !run || !count) {
        return std::nullopt;
    }
    WorldLogFlagProposal proposal{std::move(*member), *epoch, *run, {}};
    for (u8 index = 0; index < *count; ++index) {
        const auto number = reader.Next<u32>();
        const auto flag = reader.Next<u32>();
        const auto on = reader.Next<u8>();
        if (!number || !flag || !on || *on > 1) {
            return std::nullopt;
        }
        proposal.events.push_back({*number, static_cast<s32>(*flag), *on == 1});
    }
    if (!reader.Done()) {
        return std::nullopt;
    }
    return proposal;
}

std::vector<std::pair<u32, EnemyHpReport>> WorldLogHpReports(const WorldLogHp& hp) {
    std::vector<std::pair<u32, EnemyHpReport>> reports;
    for (const auto& [source, report] : hp) {
        EnemyHpReport out{source, {}};
        for (const auto& [handle, value] : report.enemies) {
            out.enemies.push_back({handle, value.first, value.second});
        }
        reports.emplace_back(report.map, std::move(out));
    }
    return reports;
}

namespace {

// u16 count, then each enemy: handle, value, max.
template <typename Enemies>
void AppendHpEnemies(std::vector<u8>& payload, const Enemies& enemies, size_t count) {
    AppendLittleEndian(payload, static_cast<u16>(count));
    size_t written = 0;
    for (const auto& enemy : enemies) {
        if (written++ == count) {
            break;
        }
        if constexpr (std::is_same_v<std::decay_t<decltype(enemy)>, EnemyHp>) {
            AppendLittleEndian(payload, enemy.handle);
            AppendLittleEndian(payload, static_cast<u32>(enemy.hp));
            AppendLittleEndian(payload, static_cast<u32>(enemy.max));
        } else {
            AppendLittleEndian(payload, enemy.first);
            AppendLittleEndian(payload, static_cast<u32>(enemy.second.first));
            AppendLittleEndian(payload, static_cast<u32>(enemy.second.second));
        }
    }
}

std::optional<std::vector<EnemyHp>> ReadHpEnemies(PayloadReader& reader) {
    const auto count = reader.Next<u16>();
    if (!count) {
        return std::nullopt;
    }
    std::vector<EnemyHp> enemies;
    for (u16 index = 0; index < *count; ++index) {
        const auto handle = reader.Next<u32>();
        const auto value = reader.Next<u32>();
        const auto max = reader.Next<u32>();
        if (!handle || !value || !max) {
            return std::nullopt;
        }
        enemies.push_back({*handle, static_cast<s32>(*value), static_cast<s32>(*max)});
    }
    return enemies;
}

} // namespace

// The member's name, epoch, run, number, map, source (u64), then the enemies (at most
// WorldLogHpPerFrame).
std::vector<u8> EncodeWorldLogHpProposal(const WorldLogHpProposal& proposal) {
    std::vector<u8> payload;
    AppendName(payload, proposal.member);
    AppendLittleEndian(payload, proposal.epoch);
    AppendLittleEndian(payload, proposal.run);
    AppendLittleEndian(payload, proposal.number);
    AppendLittleEndian(payload, proposal.map);
    AppendLittleEndian(payload, proposal.report.visit);
    AppendHpEnemies(payload, proposal.report.enemies,
                    std::min(proposal.report.enemies.size(), WorldLogHpPerFrame));
    return payload;
}

std::optional<WorldLogHpProposal> DecodeWorldLogHpProposal(std::span<const u8> payload) {
    PayloadReader reader{payload};
    auto member = reader.Name();
    const auto epoch = reader.Next<u64>();
    const auto run = reader.Next<u32>();
    const auto number = reader.Next<u32>();
    const auto map = reader.Next<u32>();
    const auto source = reader.Next<u64>();
    auto enemies = ReadHpEnemies(reader);
    if (!member || !epoch || !run || !number || !map || !source || !enemies || !reader.Done()) {
        return std::nullopt;
    }
    return WorldLogHpProposal{std::move(*member),
                              *epoch,
                              *run,
                              *number,
                              *map,
                              EnemyHpReport{*source, std::move(*enemies)}};
}

// The host's name, epoch, last seq, part, parts, the sources (u16), then each: source (u64), map,
// the enemies.
std::vector<u8> EncodeWorldLogHpSnapshot(const WorldLogHpSnapshot& snapshot) {
    std::vector<u8> payload;
    AppendName(payload, snapshot.host);
    AppendLittleEndian(payload, snapshot.epoch);
    AppendLittleEndian(payload, snapshot.last);
    payload.push_back(snapshot.part);
    payload.push_back(snapshot.parts);
    AppendLittleEndian(payload, static_cast<u16>(snapshot.hp.size()));
    for (const auto& [source, report] : snapshot.hp) {
        AppendLittleEndian(payload, source);
        AppendLittleEndian(payload, report.map);
        AppendHpEnemies(payload, report.enemies, report.enemies.size());
    }
    return payload;
}

std::optional<WorldLogHpSnapshot> DecodeWorldLogHpSnapshot(std::span<const u8> payload) {
    PayloadReader reader{payload};
    auto host = reader.Name();
    const auto epoch = reader.Next<u64>();
    const auto last = reader.Next<u64>();
    const auto part = reader.Next<u8>();
    const auto parts = reader.Next<u8>();
    const auto sources = reader.Next<u16>();
    if (!host || !epoch || !last || !part || !parts || *part >= *parts || !sources) {
        return std::nullopt;
    }
    WorldLogHpSnapshot snapshot{std::move(*host), *epoch, *last, *part, *parts, {}};
    for (u16 index = 0; index < *sources; ++index) {
        const auto source = reader.Next<u64>();
        const auto map = reader.Next<u32>();
        const auto enemies = ReadHpEnemies(reader);
        if (!source || !map || !enemies) {
            return std::nullopt;
        }
        WorldLogHpReport& report = snapshot.hp[*source];
        report.map = *map;
        for (const EnemyHp& enemy : *enemies) {
            report.enemies[enemy.handle] = {enemy.hp, enemy.max};
        }
    }
    if (!reader.Done()) {
        return std::nullopt;
    }
    return snapshot;
}

// The host's name, epoch, last seq, the count (u8), then each event: seq (u64), kind (u8), flag.
std::vector<u8> EncodeWorldLogFlagsSnapshot(const WorldLogFlagsSnapshot& snapshot) {
    std::vector<u8> payload;
    AppendName(payload, snapshot.host);
    AppendLittleEndian(payload, snapshot.epoch);
    AppendLittleEndian(payload, snapshot.last);
    payload.push_back(snapshot.after_wipe ? 1 : 0);
    const size_t count = std::min(snapshot.events.size(), WorldLogRecentFlags);
    payload.push_back(static_cast<u8>(count));
    for (size_t index = snapshot.events.size() - count; index < snapshot.events.size(); ++index) {
        const WorldLogEntry& event = snapshot.events[index];
        AppendLittleEndian(payload, event.seq);
        payload.push_back(static_cast<u8>(event.kind));
        AppendLittleEndian(payload, event.subject);
    }
    return payload;
}

std::optional<WorldLogFlagsSnapshot> DecodeWorldLogFlagsSnapshot(std::span<const u8> payload) {
    PayloadReader reader{payload};
    auto host = reader.Name();
    const auto epoch = reader.Next<u64>();
    const auto last = reader.Next<u64>();
    const auto after_wipe = reader.Next<u8>();
    const auto count = reader.Next<u8>();
    if (!host || !epoch || !last || !after_wipe || *after_wipe > 1 || !count) {
        return std::nullopt;
    }
    WorldLogFlagsSnapshot snapshot{std::move(*host), *epoch, *last, {}, *after_wipe == 1};
    for (u8 index = 0; index < *count; ++index) {
        const auto seq = reader.Next<u64>();
        const auto kind = reader.Next<u8>();
        const auto flag = reader.Next<u32>();
        if (!seq || !kind || !flag) {
            return std::nullopt;
        }
        snapshot.events.push_back({*seq, *epoch, 0, static_cast<WorldLogKind>(*kind), *flag});
    }
    if (!reader.Done()) {
        return std::nullopt;
    }
    return snapshot;
}

std::vector<WorldLogBatch> BatchWorldLog(std::string_view host, u64 epoch, u64 last,
                                         std::span<const WorldLogEntry> entries) {
    std::vector<WorldLogBatch> batches;
    for (size_t at = 0; at < entries.size(); at += WorldLogEntriesPerFrame) {
        const size_t count = std::min(WorldLogEntriesPerFrame, entries.size() - at);
        const auto from = entries.begin() + static_cast<std::ptrdiff_t>(at);
        batches.push_back(
            {std::string{host}, epoch, last,
             std::vector<WorldLogEntry>(from, from + static_cast<std::ptrdiff_t>(count))});
    }
    if (batches.empty()) {
        batches.push_back({std::string{host}, epoch, last, {}});
    }
    return batches;
}

WorldLogEntry WorldLogHost::Append(WorldLogKind kind, u32 map, u32 subject, u32 value) {
    return {++last, epoch, map, kind, subject, value};
}

void WorldLogHost::AppendShown(std::vector<WorldLogEntry>& entries) {
    if (shown.map == 0) {
        return;
    }
    entries.push_back(Append(WorldLogKind::ShownMap, shown.map, shown.map));
    for (const auto& [handle, bits] : shown.bits) {
        entries.push_back(Append(WorldLogKind::Shown, shown.map, handle, bits));
    }
}

std::vector<WorldLogEntry> WorldLogHost::Show(u32 map, std::span<const CharacterShown> characters) {
    std::map<u32, u8> now;
    for (const CharacterShown& character : characters) {
        now[character.handle] = ShownBits(character);
    }
    std::vector<WorldLogEntry> entries;
    if (map != shown.map) {
        shown = {map, std::move(now)};
        AppendShown(entries);
        return entries;
    }
    for (const auto& [handle, bits] : shown.bits) {
        if (!now.contains(handle)) {
            entries.push_back(Append(WorldLogKind::Unshown, map, handle));
        }
    }
    for (const auto& [handle, bits] : now) {
        const auto before = shown.bits.find(handle);
        if (before == shown.bits.end() || before->second != bits) {
            entries.push_back(Append(WorldLogKind::Shown, map, handle, bits));
        }
    }
    shown.bits = std::move(now);
    return entries;
}

WorldLogHost::Taken WorldLogHost::ProposeFlags(const WorldLogFlagProposal& proposal, s64 now_ms) {
    Taken taken;
    if (proposal.epoch != 0 && proposal.epoch != epoch) {
        taken.stale = true;
        return taken;
    }
    FlagSender& sender = flag_senders[proposal.member];
    if (sender.run != proposal.run) {
        sender = {proposal.run, 0};
    }
    for (const WorldLogFlagEvent& event : proposal.events) {
        if (event.number <= sender.number) {
            continue; // taken before (the frame repeats)
        }
        sender.number = event.number;
        if (flag_events.First(event.flag, event.on, now_ms)) {
            taken.entries.push_back(
                Append(event.on ? WorldLogKind::FlagSet : WorldLogKind::FlagClear, 0,
                       static_cast<u32>(event.flag)));
            recent_flags.push_back(taken.entries.back());
            if (recent_flags.size() > WorldLogRecentFlags) {
                recent_flags.erase(recent_flags.begin());
            }
        }
    }
    return taken;
}

namespace {

bool MapBit(std::span<const u8> bits, s32 index) {
    return (bits[index / 8] & (0x80 >> (index % 8))) != 0;
}

} // namespace

void WorldLogHost::AppendMapState(std::vector<WorldLogEntry>& entries) {
    if (map_state.map == 0) {
        return;
    }
    entries.push_back(Append(WorldLogKind::MapStateMap, map_state.map, map_state.map));
    for (s32 index = 0; index < MapFlagCount; ++index) {
        if (MapBit(map_state.bits, index)) {
            entries.push_back(
                Append(WorldLogKind::MapFlagOn, map_state.map, static_cast<u32>(index)));
        }
    }
}

std::vector<WorldLogEntry> WorldLogHost::MapState(u32 map, std::span<const u8> bits) {
    std::vector<WorldLogEntry> entries;
    if (map == 0 || bits.size() * 8 < static_cast<size_t>(MapFlagCount)) {
        return entries;
    }
    if (map != map_state.map) {
        map_state = {map, std::vector<u8>(bits.begin(), bits.end())};
        AppendMapState(entries);
        return entries;
    }
    for (s32 index = 0; index < MapFlagCount; ++index) {
        const bool on = MapBit(bits, index);
        if (on != MapBit(map_state.bits, index)) {
            entries.push_back(Append(on ? WorldLogKind::MapFlagOn : WorldLogKind::MapFlagOff, map,
                                     static_cast<u32>(index)));
        }
    }
    map_state.bits.assign(bits.begin(), bits.end());
    return entries;
}

void WorldLogHost::DropHpSource(u64 source, std::vector<WorldLogEntry>& entries) {
    if (hp.erase(source) > 0) {
        entries.push_back(Append(WorldLogKind::HpSourceGone, 0, static_cast<u32>(source >> 32),
                                 static_cast<u32>(source)));
    }
}

WorldLogHost::Taken WorldLogHost::ProposeHp(const WorldLogHpProposal& proposal) {
    Taken taken;
    HpSender& sender = hp_senders[proposal.member];
    const bool late = sender.run == proposal.run && proposal.number <= sender.number;
    if (late || (proposal.epoch != 0 && proposal.epoch != epoch)) {
        taken.stale = true;
        return taken;
    }
    const u64 source = proposal.report.visit;
    if (sender.source != 0 && sender.source != source) {
        DropHpSource(sender.source, taken.entries);
    }
    sender = {proposal.run, proposal.number, source};
    std::map<u32, std::pair<s32, s32>> now;
    for (const EnemyHp& enemy : proposal.report.enemies) {
        now[enemy.handle] = {enemy.hp, enemy.max};
    }
    const auto known = hp.find(source);
    const WorldLogHpReport before = known != hp.end() ? known->second : WorldLogHpReport{};
    std::vector<WorldLogEntry> changes;
    for (const auto& [handle, value] : before.enemies) {
        if (!now.contains(handle)) {
            changes.push_back({0, 0, 0, WorldLogKind::HpGone, handle});
        }
    }
    for (const auto& [handle, value] : now) {
        const auto old = before.enemies.find(handle);
        if (old == before.enemies.end() || old->second != value) {
            changes.push_back({0, 0, static_cast<u32>(value.second), WorldLogKind::HpValue, handle,
                               static_cast<u32>(value.first)});
        }
    }
    if (!changes.empty() || (known != hp.end() && before.map != proposal.map)) {
        taken.entries.push_back(Append(WorldLogKind::HpSource, proposal.map,
                                       static_cast<u32>(source >> 32), static_cast<u32>(source)));
        for (const WorldLogEntry& change : changes) {
            taken.entries.push_back(Append(change.kind, change.map, change.subject, change.value));
        }
    }
    if (now.empty()) {
        hp.erase(source);
    } else {
        hp[source] = {proposal.map, std::move(now)};
    }
    return taken;
}

WorldLogFlagsSnapshot WorldLogHost::FlagsSnapshot(std::string_view host) const {
    return {std::string{host}, epoch, last, recent_flags, started_after_wipe};
}

std::vector<WorldLogHpSnapshot> WorldLogHost::HpSnapshot(std::string_view host) const {
    std::vector<WorldLogHpSnapshot> parts{{std::string{host}, epoch, last, 0, 1, {}}};
    size_t in_part = 0;
    for (const auto& [source, report] : hp) {
        for (const auto& [handle, value] : report.enemies) {
            if (in_part == WorldLogHpPerFrame && parts.size() < 0xFF) {
                parts.push_back(
                    {std::string{host}, epoch, last, static_cast<u8>(parts.size()), 1, {}});
                in_part = 0;
            }
            WorldLogHpReport& share = parts.back().hp[source];
            share.map = report.map;
            share.enemies[handle] = value;
            ++in_part;
        }
    }
    for (WorldLogHpSnapshot& part : parts) {
        part.parts = static_cast<u8>(parts.size());
    }
    return parts;
}

WorldLogMapStateSnapshot WorldLogHost::MapStateSnapshot(std::string_view host) const {
    return {std::string{host}, epoch, last, map_state, return_point};
}

std::vector<WorldLogEntry> WorldLogHost::ReturnPoint(s32 point, u32 map) {
    std::vector<WorldLogEntry> entries;
    if (point <= 0 || (point == return_point.point && map == return_point.map)) {
        return entries;
    }
    return_point = {point, map};
    entries.push_back(Append(WorldLogKind::ReturnPoint, map, static_cast<u32>(point)));
    return entries;
}

std::vector<WorldLogShownSnapshot> WorldLogHost::ShownSnapshot(std::string_view host) const {
    std::vector<WorldLogShownSnapshot> parts{
        {std::string{host}, epoch, last, 0, 1, {shown.map, {}}}};
    for (const auto& [handle, bits] : shown.bits) {
        if (parts.back().shown.bits.size() == WorldLogShownPerFrame && parts.size() < 0xFF) {
            parts.push_back({std::string{host},
                             epoch,
                             last,
                             static_cast<u8>(parts.size()),
                             1,
                             {shown.map, {}}});
        }
        parts.back().shown.bits[handle] = bits;
    }
    for (WorldLogShownSnapshot& part : parts) {
        part.parts = static_cast<u8>(parts.size());
    }
    return parts;
}

void WorldLogHost::Own(const std::string& member, u32 map, u32 handle,
                       std::vector<WorldLogEntry>& entries) {
    const auto [it, added] = standing.try_emplace(handle, Standing{map, {}});
    it->second.by.insert(member);
    if (added) {
        entries.push_back(Append(WorldLogKind::Kill, map, handle));
    }
}

void WorldLogHost::Disown(const std::string& member, u32 handle,
                          std::vector<WorldLogEntry>& entries) {
    const auto it = standing.find(handle);
    if (it == standing.end()) {
        return;
    }
    it->second.by.erase(member);
    if (it->second.by.empty()) {
        entries.push_back(Append(WorldLogKind::Unkill, it->second.map, handle));
        standing.erase(it);
    }
}

std::vector<WorldLogEntry> WorldLogHost::Start(u64 new_epoch, bool after_wipe) {
    epoch = new_epoch;
    last = 0;
    // Every member's set goes: a member's next proposal for this log brings what stands now (its
    // run and numbers stay, so a late one of the old run is still dropped).
    for (auto& [name, member] : members) {
        member.kills.clear();
    }
    standing.clear();
    hp.clear(); // no damage from before it (a wipe respawned the map)
    recent_flags.clear();
    started_after_wipe = after_wipe;
    if (after_wipe) {
        // A wipe respawns the map; what the host's world still shows dead is from before its
        // own wipe trip loaded it (rig smoke 2026-10-13), not a death the members should copy.
        for (auto& [handle, bits] : shown.bits) {
            bits = static_cast<u8>(bits & ~ShownBits({0, false, false, false, true}));
        }
    }
    std::vector<WorldLogEntry> entries{
        Append(WorldLogKind::Reset, 0, after_wipe ? WorldLogResetAfterWipe : 0)};
    AppendShown(entries); // the host's world as it is (not a kill: it stays the host's now)
    AppendMapState(entries);
    if (return_point.point > 0) {
        // The host's point stands through a new log (a wipe): a member on it needs it.
        entries.push_back(Append(WorldLogKind::ReturnPoint, return_point.map,
                                 static_cast<u32>(return_point.point)));
    }
    return entries;
}

bool StartsAfterAWipe(std::span<const WorldLogEntry> applied) {
    return std::ranges::any_of(applied, [](const WorldLogEntry& entry) {
        return entry.kind == WorldLogKind::Reset && entry.map == 0 &&
               entry.subject == WorldLogResetAfterWipe;
    });
}

bool WorldLogWipeHold::HoldsProposals(s64 now_ms) const {
    if (!host_wiped_ms || now_ms - *host_wiped_ms >= WorldLogWipeGraceMs) {
        return false;
    }
    return !own_wipe_ms || *own_wipe_ms < *host_wiped_ms - WorldLogWipeGraceMs;
}

bool WorldLogWipeHold::BoardStale(s64 now_ms) const {
    const auto recent = [now_ms](std::optional<s64> at) {
        return at && now_ms - *at < WorldLogWipeGraceMs;
    };
    return recent(own_wipe_ms) || recent(host_wiped_ms);
}

bool HoldsHpReports(const WorldLogWipeHold& hold, bool server_world, bool guest, s64 now_ms) {
    return (server_world || guest) && hold.HoldsProposals(now_ms);
}

WorldLogHost::Taken WorldLogHost::Propose(const WorldLogProposal& proposal) {
    Taken taken;
    const auto known = members.find(proposal.member);
    const bool late = known != members.end() && known->second.run == proposal.run &&
                      proposal.number <= known->second.number;
    if (late || (proposal.epoch != 0 && proposal.epoch != epoch)) {
        taken.stale = true;
        return taken;
    }
    Member& member = members[proposal.member];
    // Kills on a map reset since (ResetMap) stay out while the proposals hold them.
    WorldLogKills proposed = proposal.kills;
    const std::unordered_set<u32> held = WorldLogHandles(proposed);
    std::erase_if(member.voided, [&](u32 handle) { return !held.contains(handle); });
    for (auto it = proposed.begin(); it != proposed.end();) {
        std::erase_if(it->second, [&](u32 handle) { return member.voided.contains(handle); });
        it = it->second.empty() ? proposed.erase(it) : std::next(it);
    }
    const std::unordered_set<u32> before = WorldLogHandles(member.kills);
    const std::unordered_set<u32> after = WorldLogHandles(proposed);
    for (const auto& [map, handles] : member.kills) {
        for (const u32 handle : handles) {
            if (!after.contains(handle)) {
                Disown(proposal.member, handle, taken.entries);
            }
        }
    }
    for (const auto& [map, handles] : proposed) {
        for (const u32 handle : handles) {
            if (!before.contains(handle)) {
                Own(proposal.member, map, handle, taken.entries);
            }
        }
    }
    member.run = proposal.run;
    member.number = proposal.number;
    member.kills = std::move(proposed);
    return taken;
}

std::vector<WorldLogEntry> WorldLogHost::ResetMap(u32 map) {
    std::vector<WorldLogEntry> entries;
    if (map == 0) {
        return entries;
    }
    const std::vector<u32> maps = MapsLoadedWith(map);
    const auto reset = [&](u32 on) { return std::ranges::find(maps, on) != maps.end(); };
    for (const u32 on : maps) {
        entries.push_back(Append(WorldLogKind::Reset, on, 0));
    }
    for (auto& [name, member] : members) {
        for (auto it = member.kills.begin(); it != member.kills.end();) {
            if (!reset(it->first)) {
                ++it;
                continue;
            }
            member.voided.insert(it->second.begin(), it->second.end());
            it = member.kills.erase(it);
        }
    }
    std::erase_if(standing, [&](const auto& kill) { return reset(kill.second.map); });
    std::vector<u64> sources;
    for (const auto& [source, report] : hp) {
        if (reset(report.map)) {
            sources.push_back(source);
        }
    }
    for (const u64 source : sources) {
        DropHpSource(source, entries);
    }
    // The Reset takes the members' copies of the host's shown characters and map state there:
    // they come again, the dead alive (the map respawns, as Start's after a wipe).
    if (reset(shown.map)) {
        for (auto& [handle, bits] : shown.bits) {
            bits = static_cast<u8>(bits & ~ShownBits({0, false, false, false, true}));
        }
        AppendShown(entries);
    }
    if (reset(map_state.map)) {
        AppendMapState(entries);
    }
    return entries;
}

std::vector<WorldLogEntry> WorldLogHost::Drop(const std::string& member) {
    std::vector<WorldLogEntry> entries;
    if (const auto found = members.find(member); found != members.end()) {
        for (const auto& [map, handles] : found->second.kills) {
            for (const u32 handle : handles) {
                Disown(member, handle, entries);
            }
        }
        members.erase(found);
    }
    if (const auto sender = hp_senders.find(member); sender != hp_senders.end()) {
        DropHpSource(sender->second.source, entries);
        hp_senders.erase(sender);
    }
    flag_senders.erase(member);
    return entries;
}

WorldLogHost::State WorldLogHost::Save() const {
    return State{epoch,       last,         members,           standing,
                 shown,       flag_senders, flag_events.All(), map_state,
                 hp_senders,  hp,           recent_flags,      started_after_wipe,
                 return_point};
}

void WorldLogHost::Restore(State state) {
    epoch = state.epoch;
    last = state.last;
    members = std::move(state.members);
    standing = std::move(state.standing);
    shown = std::move(state.shown);
    flag_senders = std::move(state.flag_senders);
    flag_events.Restore(std::move(state.flag_events));
    map_state = std::move(state.map_state);
    hp_senders = std::move(state.hp_senders);
    hp = std::move(state.hp);
    recent_flags = std::move(state.recent_flags);
    started_after_wipe = state.started_after_wipe;
    return_point = state.return_point;
}

WorldLogKills WorldLogHost::Kills() const {
    WorldLogKills kills;
    for (const auto& [handle, kill] : standing) {
        kills[kill.map].insert(handle);
    }
    return kills;
}

std::vector<WorldLogSnapshot> WorldLogHost::Snapshot(std::string_view host) const {
    std::vector<WorldLogSnapshot> parts{{std::string{host}, epoch, last, 0, 1, {}}};
    size_t in_part = 0;
    for (const auto& [map, handles] : Kills()) {
        for (const u32 handle : handles) {
            if (in_part == WorldLogHandlesPerFrame && parts.size() < 0xFF) {
                parts.push_back(
                    {std::string{host}, epoch, last, static_cast<u8>(parts.size()), 1, {}});
                in_part = 0;
            }
            parts.back().kills[map].insert(handle);
            ++in_part;
        }
    }
    for (WorldLogSnapshot& part : parts) {
        part.parts = static_cast<u8>(parts.size());
    }
    return parts;
}

void WorldLogMember::Reset() {
    epoch = 0;
    applied = 0;
    host_last = 0;
    based = false;
    host.clear();
    waiting.clear();
    heard_ms.reset();
    asked_ms.reset();
    partial.reset();
    kills.clear();
    shown = {};
    shown_known = false;
    map_state = {};
    map_state_known = false;
    return_point = {};
    hp.clear();
    hp_source = 0;
}

bool WorldLogMember::Follow(u64 next_epoch, std::string_view next_host, s64 now_ms, Step& step) {
    if (next_epoch != epoch) {
        if (!WorldLogEpochFollows(next_epoch, epoch)) {
            step.stale = true;
            return false;
        }
        Reset();
        epoch = next_epoch;
        step.new_epoch = true;
    }
    host = next_host;
    heard_ms = now_ms;
    return true;
}

bool WorldLogMember::Behind() const {
    return !based || !waiting.empty() || host_last > applied;
}

void WorldLogMember::Apply(const WorldLogEntry& entry) {
    switch (entry.kind) {
    case WorldLogKind::Reset:
        if (entry.map == 0) {
            kills.clear();
        } else {
            kills.erase(entry.map);
        }
        if (entry.map == 0 || entry.map == shown.map) {
            shown = {};
        }
        if (entry.map == 0 || entry.map == map_state.map) {
            map_state = {};
        }
        if (entry.map == 0) {
            hp.clear();
        }
        return;
    case WorldLogKind::Kill:
        kills[entry.map].insert(entry.subject);
        return;
    case WorldLogKind::Unkill:
        if (const auto on_map = kills.find(entry.map); on_map != kills.end()) {
            on_map->second.erase(entry.subject);
            if (on_map->second.empty()) {
                kills.erase(on_map);
            }
        }
        return;
    case WorldLogKind::ShownMap:
        shown = {entry.map, {}};
        shown_known = true; // every character of it follows
        return;
    case WorldLogKind::Shown:
        if (entry.map == shown.map) {
            shown.bits[entry.subject] = entry.value;
        }
        return;
    case WorldLogKind::Unshown:
        if (entry.map == shown.map) {
            shown.bits.erase(entry.subject);
        }
        return;
    case WorldLogKind::MapStateMap:
    case WorldLogKind::MapFlagOn:
    case WorldLogKind::MapFlagOff:
        ApplyMapFlag(entry);
        return;
    case WorldLogKind::FlagSet:
    case WorldLogKind::FlagClear:
        return; // events: the caller sets them (Step::applied)
    case WorldLogKind::HpSource:
    case WorldLogKind::HpValue:
    case WorldLogKind::HpGone:
    case WorldLogKind::HpSourceGone:
        ApplyHp(entry);
        return;
    case WorldLogKind::ReturnPoint:
        return_point = {static_cast<s32>(entry.subject), entry.map};
        return;
    }
}

void WorldLogMember::ApplyHp(const WorldLogEntry& entry) {
    const u64 source = (u64{entry.subject} << 32) | entry.value;
    switch (entry.kind) {
    case WorldLogKind::HpSource:
        hp_source = source;
        hp[source].map = entry.map;
        return;
    case WorldLogKind::HpValue:
        hp[hp_source].enemies[entry.subject] = {static_cast<s32>(entry.value),
                                                static_cast<s32>(entry.map)};
        return;
    case WorldLogKind::HpGone:
        if (const auto report = hp.find(hp_source); report != hp.end()) {
            report->second.enemies.erase(entry.subject);
        }
        break;
    case WorldLogKind::HpSourceGone:
        hp.erase(source);
        return;
    default:
        return;
    }
    // A report with no enemies is none (as the host keeps them).
    if (const auto report = hp.find(hp_source);
        report != hp.end() && report->second.enemies.empty()) {
        hp.erase(report);
    }
}

void WorldLogMember::ApplyMapFlag(const WorldLogEntry& entry) {
    if (entry.kind == WorldLogKind::MapStateMap) {
        map_state = {entry.map, std::vector<u8>((MapFlagCount + 7) / 8)};
        map_state_known = true; // every flag on follows
        return;
    }
    if (entry.map != map_state.map || entry.subject >= static_cast<u32>(MapFlagCount)) {
        return;
    }
    const u8 mask = static_cast<u8>(0x80 >> (entry.subject % 8));
    u8& byte = map_state.bits[entry.subject / 8];
    byte = entry.kind == WorldLogKind::MapFlagOn ? static_cast<u8>(byte | mask)
                                                 : static_cast<u8>(byte & ~mask);
}

void WorldLogMember::Drain(Step& step) {
    std::erase_if(waiting, [this](const auto& entry) { return entry.first <= applied; });
    while (based && !waiting.empty() && waiting.begin()->first == applied + 1) {
        Apply(waiting.begin()->second);
        step.applied.push_back(waiting.begin()->second);
        applied = waiting.begin()->first;
        waiting.erase(waiting.begin());
    }
}

WorldLogMember::Step WorldLogMember::Take(const WorldLogBatch& batch, s64 now_ms) {
    Step step;
    if (!Follow(batch.epoch, batch.host, now_ms, step)) {
        return step;
    }
    host_last = std::max(host_last, batch.last);
    for (const WorldLogEntry& entry : batch.entries) {
        if (entry.seq > applied && waiting.size() < WorldLogWaitingCap) {
            waiting.emplace(entry.seq, entry);
        }
    }
    // A log from its start needs no snapshot.
    if (!based && applied == 0 && !waiting.empty() && waiting.begin()->first == 1) {
        based = true;
        shown_known = true;
    }
    Drain(step);
    step.gap = Behind();
    return step;
}

WorldLogMember::Step WorldLogMember::Take(const WorldLogSnapshot& part, s64 now_ms) {
    Step step;
    if (!Follow(part.epoch, part.host, now_ms, step)) {
        return step;
    }
    host_last = std::max(host_last, part.last);
    if (!based || part.last > applied) {
        PartialSnapshot& pending = Pending(part.last);
        if (pending.parts != part.parts) {
            pending.got.clear();
        }
        pending.parts = part.parts;
        pending.got[part.part] = part.kills;
        Complete(step);
    }
    step.gap = Behind();
    return step;
}

WorldLogMember::Step WorldLogMember::Take(const WorldLogShownSnapshot& part, s64 now_ms) {
    Step step;
    if (!Follow(part.epoch, part.host, now_ms, step)) {
        return step;
    }
    host_last = std::max(host_last, part.last);
    if (!based || part.last > applied) {
        PartialSnapshot& pending = Pending(part.last);
        if (pending.shown_parts != part.parts) {
            pending.shown_got.clear();
        }
        pending.shown_parts = part.parts;
        pending.shown_got[part.part] = part.shown;
        Complete(step);
    }
    step.gap = Behind();
    return step;
}

WorldLogMember::PartialSnapshot& WorldLogMember::Pending(u64 last) {
    if (!partial || partial->last != last) {
        partial = PartialSnapshot{.last = last};
    }
    return *partial;
}

WorldLogMember::Step WorldLogMember::Take(const WorldLogFlagsSnapshot& part, s64 now_ms) {
    Step step;
    if (!Follow(part.epoch, part.host, now_ms, step)) {
        return step;
    }
    host_last = std::max(host_last, part.last);
    if (!based || part.last > applied) {
        PartialSnapshot& pending = Pending(part.last);
        pending.flags = part.events;
        pending.after_wipe = part.after_wipe;
        Complete(step);
    }
    step.gap = Behind();
    return step;
}

WorldLogMember::Step WorldLogMember::Take(const WorldLogHpSnapshot& part, s64 now_ms) {
    Step step;
    if (!Follow(part.epoch, part.host, now_ms, step)) {
        return step;
    }
    host_last = std::max(host_last, part.last);
    if (!based || part.last > applied) {
        PartialSnapshot& pending = Pending(part.last);
        if (pending.hp_parts != part.parts) {
            pending.hp_got.clear();
        }
        pending.hp_parts = part.parts;
        pending.hp_got[part.part] = part.hp;
        Complete(step);
    }
    step.gap = Behind();
    return step;
}

WorldLogMember::Step WorldLogMember::Take(const WorldLogMapStateSnapshot& part, s64 now_ms) {
    Step step;
    if (!Follow(part.epoch, part.host, now_ms, step)) {
        return step;
    }
    host_last = std::max(host_last, part.last);
    if (!based || part.last > applied) {
        PartialSnapshot& pending = Pending(part.last);
        pending.map_state = part.state;
        pending.return_point = part.return_point;
        Complete(step);
    }
    step.gap = Behind();
    return step;
}

namespace {

// A snapshot's shown characters, from its parts.
WorldLogShown JoinShownParts(const std::map<u8, WorldLogShown>& parts) {
    WorldLogShown shown;
    for (const auto& [index, share] : parts) {
        shown.map = share.map;
        shown.bits.insert(share.bits.begin(), share.bits.end());
    }
    return shown;
}

} // namespace

// A snapshot is whole with every kills part, every shown part, its map state, every HP part and
// its recent flag events.
void WorldLogMember::Complete(Step& step) {
    const PartialSnapshot& pending = *partial;
    const bool kills_whole = pending.parts && pending.got.size() >= *pending.parts;
    const bool shown_whole =
        pending.shown_parts && pending.shown_got.size() >= *pending.shown_parts;
    const bool hp_whole = pending.hp_parts && pending.hp_got.size() >= *pending.hp_parts;
    if (!kills_whole || !shown_whole || !pending.map_state || !hp_whole || !pending.flags) {
        return;
    }
    // The flag events it missed: after what it had applied of this log (none for a member that
    // joins: it had none of it).
    if (based) {
        for (const WorldLogEntry& event : *pending.flags) {
            if (event.seq > applied && event.seq <= pending.last) {
                step.applied.push_back(event);
            }
        }
    } else {
        // A member that joins: the events it heard live while it waited (Drain drops the entries
        // the snapshot covers, and on the log path nothing else sets them).
        for (const auto& [seq, event] : waiting) {
            if (seq <= pending.last &&
                (event.kind == WorldLogKind::FlagSet || event.kind == WorldLogKind::FlagClear)) {
                step.applied.push_back(event);
            }
        }
        // Based on a log that started at a wipe without its Reset entry (see WorldLogWipeHold).
        step.after_wipe = pending.after_wipe;
    }
    kills.clear();
    for (const auto& [index, share] : pending.got) {
        for (const auto& [map, handles] : share) {
            kills[map].insert(handles.begin(), handles.end());
        }
    }
    shown = JoinShownParts(pending.shown_got);
    shown_known = true;
    map_state = *pending.map_state;
    map_state_known = true;
    return_point = pending.return_point;
    hp.clear();
    for (const auto& [index, share] : pending.hp_got) {
        for (const auto& [source, report] : share) {
            hp[source].map = report.map;
            hp[source].enemies.insert(report.enemies.begin(), report.enemies.end());
        }
    }
    applied = pending.last;
    based = true;
    partial.reset();
    step.snapshot = true;
    Drain(step);
}

bool WorldLogMember::SnapshotDue(s64 now_ms) {
    if (!heard_ms || !Behind() || (asked_ms && now_ms - *asked_ms < WorldLogSnapshotRetryMs)) {
        return false;
    }
    asked_ms = now_ms;
    return true;
}

bool WorldLogMember::Fresh(s64 now_ms) const {
    return !Behind() && heard_ms && now_ms - *heard_ms <= WorldLogLifeMs;
}

bool WorldLogPath::Decide(bool enabled, size_t peers, s64 now_ms) {
    const std::string before = why;
    bool next = enabled && peers > 0;
    if (!enabled) {
        why = "switched off here";
        unsure_since.reset();
    } else if (peers == 0) {
        unsure_since = unsure_since.value_or(now_ms);
        next = active && now_ms - *unsure_since < WorldLogUnsureMs;
        if (!next) {
            why = "no peers";
        }
    } else {
        unsure_since.reset();
        why = peers == 1 ? "1 peer" : std::to_string(peers) + " peers";
    }
    changed = changed || !decided || next != active || why != before;
    decided = true;
    active = next;
    return active;
}

std::optional<std::string> WorldLogPath::TakeChange() {
    if (!changed) {
        return std::nullopt;
    }
    changed = false;
    return active ? "Co-op world log: on, the kills, shown characters and flags go through the "
                    "host's log (" +
                        why + ")"
                  : "Co-op world log: off, the kills, shown characters and flags go through the "
                    "boards (" +
                        why + ")";
}

std::vector<u8> EncodeWorldLogStart(u64 epoch, bool after_wipe) {
    std::vector<u8> payload;
    AppendLittleEndian(payload, epoch);
    payload.push_back(after_wipe ? 1 : 0);
    return payload;
}

std::optional<std::pair<u64, bool>> DecodeWorldLogStart(std::span<const u8> payload) {
    PayloadReader reader{payload};
    const auto epoch = reader.Next<u64>();
    const auto keep = reader.Next<u8>();
    if (!epoch || !keep || *keep > 1 || !reader.Done()) {
        return std::nullopt;
    }
    return std::pair{*epoch, *keep == 1};
}

std::vector<u8> EncodeWorldLogShow(u32 map, std::span<const CharacterShown> characters) {
    std::vector<u8> payload;
    AppendLittleEndian(payload, map);
    const std::vector<u8> listed = EncodeShownCharacters(characters);
    payload.insert(payload.end(), listed.begin(), listed.end());
    return payload;
}

std::optional<std::pair<u32, std::vector<CharacterShown>>> DecodeWorldLogShow(
    std::span<const u8> payload) {
    if (payload.size() < 4) {
        return std::nullopt;
    }
    auto characters = DecodeShownCharacters(payload.subspan(4));
    if (!characters) {
        return std::nullopt;
    }
    return std::pair{ReadLittleEndian<u32>(payload, 0), std::move(*characters)};
}

std::vector<u8> EncodeWorldLogMapStateInput(u32 map, std::span<const u8> bits) {
    std::vector<u8> payload;
    AppendLittleEndian(payload, map);
    payload.insert(payload.end(), bits.begin(), bits.end());
    return payload;
}

std::optional<WorldLogMapState> DecodeWorldLogMapStateInput(std::span<const u8> payload) {
    if (payload.size() < 4) {
        return std::nullopt;
    }
    return WorldLogMapState{ReadLittleEndian<u32>(payload, 0),
                            std::vector<u8>(payload.begin() + 4, payload.end())};
}

std::vector<u8> EncodeWorldLogReturnPointInput(const WorldLogReturnPoint& point) {
    std::vector<u8> payload;
    AppendLittleEndian(payload, static_cast<u32>(point.point));
    AppendLittleEndian(payload, point.map);
    return payload;
}

std::optional<WorldLogReturnPoint> DecodeWorldLogReturnPointInput(std::span<const u8> payload) {
    if (payload.size() != 8) {
        return std::nullopt;
    }
    return WorldLogReturnPoint{static_cast<s32>(ReadLittleEndian<u32>(payload, 0)),
                               ReadLittleEndian<u32>(payload, 4)};
}

} // namespace BloodborneCoop
