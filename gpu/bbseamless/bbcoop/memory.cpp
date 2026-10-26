// SPDX-License-Identifier: GPL-2.0-or-later
// bbport co-op: the MemoryManager queries of shadPS4 that the co-op code makes before it reads or
// writes game memory (core/memory.h), answered with what the guest sees: its mappings in the PS4
// range (the C runtime), the game's image segments (the loader) and, anywhere else (the guest
// threads' stacks), the host's own protection.

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <vector>
#ifdef _WIN32
#include <windows.h>
#else
#include <fstream>
#include <sstream>
#include <string>
#endif
#include "bbcoop/bbcoop_internal.h"
#include "core/memory.h"

extern "C" int runtime_memory_protection(uintptr_t address, uintptr_t* start, uintptr_t* end,
                                         uint32_t* prot);

namespace Core {
namespace {

constexpr u32 Read = static_cast<u32>(MemoryProt::CpuRead);
constexpr u32 Write = static_cast<u32>(MemoryProt::CpuWrite);
constexpr u32 Execute = static_cast<u32>(MemoryProt::CpuExec);

struct Region {
    uintptr_t start;
    uintptr_t end;
    u32 prot;
};

std::mutex segments_mutex;
std::vector<Region> segments;

bool FindIn(const std::vector<Region>& regions, uintptr_t address, Region& found) {
    const auto at = std::ranges::find_if(regions, [address](const Region& region) {
        return address >= region.start && address < region.end;
    });
    if (at == regions.end()) {
        return false;
    }
    found = *at;
    return true;
}

#ifdef _WIN32
bool HostRegion(uintptr_t address, Region& found) {
    MEMORY_BASIC_INFORMATION info{};
    if (VirtualQuery(reinterpret_cast<const void*>(address), &info, sizeof(info)) == 0 ||
        info.State != MEM_COMMIT || (info.Protect & (PAGE_GUARD | PAGE_NOACCESS)) != 0) {
        return false;
    }
    u32 prot = 0;
    switch (info.Protect & 0xff) {
    case PAGE_READONLY:
        prot = Read;
        break;
    case PAGE_READWRITE:
    case PAGE_WRITECOPY:
        prot = Read | Write;
        break;
    case PAGE_EXECUTE:
        prot = Execute;
        break;
    case PAGE_EXECUTE_READ:
        prot = Read | Execute;
        break;
    case PAGE_EXECUTE_READWRITE:
    case PAGE_EXECUTE_WRITECOPY:
        prot = Read | Write | Execute;
        break;
    default:
        return false;
    }
    const auto start = reinterpret_cast<uintptr_t>(info.BaseAddress);
    found = {start, start + info.RegionSize, prot};
    return true;
}
#else
// /proc/self/maps, read again when an address is not in it or it is older than a moment: the
// co-op code asks about stack words in bursts (a call chain), one region after another.
bool HostRegion(uintptr_t address, Region& found) {
    using Clock = std::chrono::steady_clock;
    static std::mutex mutex;
    static std::vector<Region> regions;
    static Clock::time_point read_at;
    std::scoped_lock lock{mutex};
    const auto now = Clock::now();
    if (now - read_at < std::chrono::milliseconds{250} && FindIn(regions, address, found)) {
        return true;
    }
    regions.clear();
    read_at = now;
    std::ifstream maps{"/proc/self/maps"};
    for (std::string line; std::getline(maps, line);) {
        uintptr_t start{}, end{};
        char dash{};
        std::string perms;
        std::istringstream fields{line};
        fields >> std::hex >> start >> dash >> end >> perms;
        if (perms.size() >= 3) {
            regions.push_back({start, end,
                               (perms[0] == 'r' ? Read : 0) | (perms[1] == 'w' ? Write : 0) |
                                   (perms[2] == 'x' ? Execute : 0)});
        }
    }
    return FindIn(regions, address, found) && found.prot != 0;
}
#endif

} // namespace

void NoteImageSegment(uintptr_t start, u64 size, unsigned flags) {
    // ELF segment flags: 1 execute, 2 write, 4 read.
    const u32 prot =
        ((flags & 4) ? Read : 0) | ((flags & 2) ? Write : 0) | ((flags & 1) ? Execute : 0);
    std::scoped_lock lock{segments_mutex};
    segments.push_back({start, start + size, prot});
}

bool MemoryManager::IsValidMapping(VAddr virtual_addr, u64 size) {
    // The guest's pointers are 40-bit, its stacks and image included in this port.
    constexpr VAddr End = 0x100'0000'0000ULL;
    return virtual_addr != 0 && size <= End && virtual_addr <= End - size;
}

s32 MemoryManager::QueryProtection(VAddr addr, void** start, void** end, u32* prot) {
    Region found{};
    uintptr_t first{}, last{};
    u32 guest_prot{};
    bool known = false;
    if (runtime_memory_protection(addr, &first, &last, &guest_prot)) {
        found = {first, last, guest_prot};
        known = true;
    } else {
        std::scoped_lock lock{segments_mutex};
        known = FindIn(segments, addr, found);
    }
    if (!known && !HostRegion(addr, found)) {
        return -1;
    }
    if (start) {
        *start = reinterpret_cast<void*>(found.start);
    }
    if (end) {
        *end = reinterpret_cast<void*>(found.end);
    }
    if (prot) {
        *prot = found.prot;
    }
    return 0;
}

} // namespace Core
