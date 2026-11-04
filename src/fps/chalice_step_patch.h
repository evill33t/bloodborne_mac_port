// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstdint>
#include <cstring>
#include <limits>
#include <span>
#include <vector>

namespace MacChaliceStep {
// These constructors pass {vtable + 16, seconds} to a two-argument advance call.
// Fold ADD RCX,16 into LEA, then use the recovered bytes to load measured dt.
// RCX is not an argument of that call; RAX and the owner/descriptor arguments stay intact.
inline bool Build(std::span<const uint8_t> original, uint64_t address, uint64_t step,
                  std::vector<uint8_t>& result) {
    constexpr uint8_t lea[] = {0x48, 0x8d, 0x0d};
    constexpr uint8_t add[] = {0x48, 0x83, 0xc1, 0x10};
    if ((original.size() != 22 && original.size() != 25) ||
        std::memcmp(original.data(), lea, sizeof(lea)) ||
        std::memcmp(original.data() + 7, add, sizeof(add))) return false;
    const size_t store = original.size() == 22 ? 4 : 7;
    const uint8_t modrm = store == 4 ? 0x4d : 0x8d;
    if (original[11] != 0x48 || original[12] != 0x89 || original[13] != modrm ||
        original[11 + store] != 0xc7 || original[12 + store] != 0x45) return false;
    int32_t table;
    std::memcpy(&table, original.data() + 3, sizeof(table));
    if (table > INT32_MAX - 16) return false;
    const int64_t delta = static_cast<int64_t>(step) - static_cast<int64_t>(address + 7 + store + 6);
    if (delta < INT32_MIN || delta > INT32_MAX) return false;
    result.assign(original.begin(), original.begin() + 7);
    table += 16;
    std::memcpy(result.data() + 3, &table, sizeof(table));
    result.insert(result.end(), original.begin() + 11, original.begin() + 11 + store);
    result.insert(result.end(), {0x8b, 0x0d});
    const int32_t displacement = static_cast<int32_t>(delta);
    const auto* bytes = reinterpret_cast<const uint8_t*>(&displacement);
    result.insert(result.end(), bytes, bytes + sizeof(displacement));
    result.insert(result.end(), {0x89, 0x4d, original[13 + store], 0x66, 0x90});
    return result.size() == original.size();
}
} // namespace MacChaliceStep
