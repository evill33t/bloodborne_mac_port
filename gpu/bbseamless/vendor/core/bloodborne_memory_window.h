// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <cstddef>
#include <limits>

#include "common/types.h"

namespace Core::Bloodborne {

// The guest regions one thread's memory checks found readable, for as long as the mapping table's
// generation they were read at lasts (odd while the table changes). A check that takes the table's
// lock and walks its run of mappings cost 54 000 of those in one co-op tick on a player's guest;
// a check inside a region seen this generation is one compare. A region unmapped after the check
// is the same race the check always had: it is proved at the read's own moment in both.
class MemoryWindows {
public:
    bool Covers(u64 generation, u64 address, std::size_t size, u32 required) const {
        if ((generation & 1) != 0 || size == 0 ||
            address > std::numeric_limits<u64>::max() - size) {
            return false;
        }
        for (const Window& window : windows) {
            if (window.generation == generation && window.end != 0 && address >= window.start &&
                address + size <= window.end && (window.protection & required) == required) {
                return true;
            }
        }
        return false;
    }

    void Note(u64 generation, u64 start, u64 end, u32 protection) {
        if ((generation & 1) != 0) {
            return;
        }
        windows[next++ % windows.size()] = Window{generation, start, end, protection};
    }

private:
    struct Window {
        u64 generation = 0;
        u64 start = 0;
        u64 end = 0;
        u32 protection = 0;
    };
    std::array<Window, 8> windows{};
    std::size_t next = 0;
};

} // namespace Core::Bloodborne
