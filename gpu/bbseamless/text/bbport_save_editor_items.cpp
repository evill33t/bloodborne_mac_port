// SPDX-License-Identifier: GPL-2.0-or-later
#include "bbport_save_editor.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <map>
#include <nlohmann/json.hpp>

namespace BbSaveEditor {

namespace {

template <typename T>
bool ReadAt(std::span<const std::uint8_t> bytes, std::size_t at, T& value) {
    if (at > bytes.size() || bytes.size() - at < sizeof(T)) {
        return false;
    }
    std::memcpy(&value, bytes.data() + at, sizeof(T));
    return true;
}

// A NUL-ended UTF-16LE string at `at`, as UTF-8.
std::string Utf16At(std::span<const std::uint8_t> bytes, std::size_t at) {
    std::string out;
    for (std::uint16_t unit = 0; ReadAt(bytes, at, unit) && unit != 0; at += 2) {
        std::uint32_t code = unit;
        if (unit >= 0xD800 && unit < 0xDC00) {
            std::uint16_t low = 0;
            if (!ReadAt(bytes, at + 2, low) || low < 0xDC00 || low >= 0xE000) {
                break;
            }
            code = 0x10000 + ((unit - 0xD800u) << 10) + (low - 0xDC00u);
            at += 2;
        }
        if (code < 0x80) {
            out += static_cast<char>(code);
        } else if (code < 0x800) {
            out += static_cast<char>(0xC0 | (code >> 6));
            out += static_cast<char>(0x80 | (code & 0x3F));
        } else if (code < 0x10000) {
            out += static_cast<char>(0xE0 | (code >> 12));
            out += static_cast<char>(0x80 | ((code >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (code & 0x3F));
        } else {
            out += static_cast<char>(0xF0 | (code >> 18));
            out += static_cast<char>(0x80 | ((code >> 12) & 0x3F));
            out += static_cast<char>(0x80 | ((code >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (code & 0x3F));
        }
    }
    return out;
}

// A version 2 FMG's strings by id (groups of 16 bytes {index, first id, last id} at 0x28, string
// offsets (8 bytes each) at [0x18]).
std::map<std::uint32_t, std::string> ParseFmg(std::span<const std::uint8_t> fmg) {
    std::map<std::uint32_t, std::string> strings;
    std::uint32_t groups = 0;
    std::uint64_t offsets = 0;
    if (!ReadAt(fmg, 0x0C, groups) || !ReadAt(fmg, 0x18, offsets) || groups > 100000) {
        return strings;
    }
    for (std::uint32_t g = 0; g < groups; ++g) {
        std::int32_t index = 0, first = 0, last = 0;
        if (!ReadAt(fmg, 0x28 + g * 16, index) || !ReadAt(fmg, 0x28 + g * 16 + 4, first) ||
            !ReadAt(fmg, 0x28 + g * 16 + 8, last) || index < 0 || last < first ||
            last - first > 100000) {
            return strings;
        }
        for (std::int32_t id = first; id <= last; ++id) {
            std::int64_t at = 0;
            if (!ReadAt(fmg, offsets + static_cast<std::uint64_t>(index + id - first) * 8, at)) {
                return strings;
            }
            if (at > 0) {
                strings[static_cast<std::uint32_t>(id)] =
                    Utf16At(fmg, static_cast<std::size_t>(at));
            }
        }
    }
    return strings;
}

bool Placeholder(const std::string& name) {
    if (name.empty() || name == "*" || name.find("NOT NEEDED") != std::string::npos ||
        name.find("(not needed)") != std::string::npos) {
        return true;
    }
    return name.size() > 2 && name.compare(0, 2, "en") == 0 &&
           std::all_of(name.begin() + 2, name.end(), [](char c) { return c >= '0' && c <= '9'; });
}

// The FMGs of a BND4 (msgbnd inflated) by their ids; empty on bad input.
std::map<std::int32_t, std::span<const std::uint8_t>> FmgsOf(std::span<const std::uint8_t> bnd) {
    std::map<std::int32_t, std::span<const std::uint8_t>> fmgs;
    std::uint32_t count = 0;
    std::uint64_t entry_size = 0;
    if (bnd.size() < 0x40 || std::memcmp(bnd.data(), "BND4", 4) != 0 ||
        !ReadAt(bnd, 0x0C, count) || !ReadAt(bnd, 0x20, entry_size) || entry_size < 0x24 ||
        count > 1000) {
        return fmgs;
    }
    for (std::uint32_t i = 0; i < count; ++i) {
        const std::size_t entry = 0x40 + i * entry_size;
        std::uint64_t size = 0;
        std::uint32_t offset = 0;
        std::int32_t id = 0;
        if (!ReadAt(bnd, entry + 0x08, size) || !ReadAt(bnd, entry + 0x18, offset) ||
            !ReadAt(bnd, entry + 0x1C, id) || offset > bnd.size() || size > bnd.size() - offset) {
            return {};
        }
        fmgs[id] = bnd.subspan(offset, static_cast<std::size_t>(size));
    }
    return fmgs;
}

} // namespace

std::map<std::uint32_t, std::string> GameText(std::span<const std::uint8_t> bnd, std::int32_t fmg) {
    const auto fmgs = FmgsOf(bnd);
    const auto found = fmgs.find(fmg);
    return found == fmgs.end() ? std::map<std::uint32_t, std::string>{} : ParseFmg(found->second);
}

std::map<std::uint32_t, std::string> ItemNamesFromGameText(std::span<const std::uint8_t> items,
                                                           std::span<const std::uint8_t> menu) {
    std::map<std::uint32_t, std::string> names;
    const auto add = [&](std::span<const std::uint8_t> bnd, std::int32_t fmg,
                         std::uint32_t category) {
        const auto fmgs = FmgsOf(bnd);
        const auto found = fmgs.find(fmg);
        if (found == fmgs.end()) {
            return;
        }
        for (auto& [id, name] : ParseFmg(found->second)) {
            if (!Placeholder(name) && id <= 0x0FFFFFFF) {
                names[category | id] = std::move(name);
            }
        }
    };
    add(items, 10, GoodsCategory);
    add(items, 11, WeaponCategory);
    add(items, 12, ArmorCategory);
    add(menu, 31, 0x80000000u); // gems and runes
    return names;
}

std::vector<Item> ItemsFromGameText(std::span<const std::uint8_t> bnd) {
    std::vector<Item> items;
    for (const auto& [id, fmg] : FmgsOf(bnd)) {
        const std::uint32_t category = id == 10   ? GoodsCategory
                                       : id == 11 ? WeaponCategory
                                       : id == 12 ? ArmorCategory
                                                  : 0xFFFFFFFFu;
        if (category == 0xFFFFFFFFu) {
            continue;
        }
        for (auto& [item, name] : ParseFmg(fmg)) {
            if (Placeholder(name) || (category == GoodsCategory && item >= 800 && item < 900) ||
                (category == WeaponCategory && (item % 10000 != 0 || item == 1000000))) {
                continue;
            }
            items.push_back({category, item, std::move(name)});
        }
    }
    std::ranges::sort(items, [](const Item& a, const Item& b) {
        return a.category != b.category ? a.category > b.category : a.name < b.name;
    });
    return items;
}

std::vector<Item> ParseItems(std::string_view goods, std::string_view weapons,
                             std::string_view armors) {
    std::vector<Item> items;
    const auto add = [&](std::uint32_t category, const nlohmann::json& table) {
        if (!table.is_object()) {
            return;
        }
        for (const auto& [key, entry] : table.items()) {
            char* end = nullptr;
            const unsigned long id = std::strtoul(key.c_str(), &end, 10);
            if (end == key.c_str() || *end != '\0' || !entry.is_object()) {
                continue;
            }
            const auto name = entry.find("item_name");
            items.push_back({category, static_cast<std::uint32_t>(id),
                             name != entry.end() && name->is_string() ? name->get<std::string>()
                                                                      : key});
        }
    };
    const auto parse = [](std::string_view text) {
        return nlohmann::json::parse(text.begin(), text.end(), nullptr, false);
    };
    if (const auto j = parse(goods); j.is_object()) {
        for (const auto& [group, table] : j.items()) {
            add(GoodsCategory, table);
        }
    }
    if (const auto j = parse(weapons); j.is_object()) {
        for (const auto& [hand, table] : j.items()) {
            add(WeaponCategory, table);
        }
    }
    if (const auto j = parse(armors); j.is_object()) {
        add(ArmorCategory, j);
    }
    std::ranges::sort(items, [](const Item& a, const Item& b) {
        return a.category != b.category ? a.category > b.category : a.name < b.name;
    });
    return items;
}

} // namespace BbSaveEditor
