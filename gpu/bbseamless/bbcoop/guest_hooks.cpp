// SPDX-License-Identifier: GPL-2.0-or-later
// bbport co-op: guest code hooks (core/cpu_patches.h in the emulator) for the native port. The
// trampolines come from the emulator's generator (core/guest_hook.cpp); this file keeps the
// registered modules and writes the jumps into the game's read-execute code.

#include <cstdint>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <vector>
#include <xbyak/xbyak.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <fstream>
#include <sstream>
#include <string>
#include <sys/mman.h>
#include <unistd.h>
#endif
#include "common/logging/log.h"
#include "core/cpu_patches.h"
#include "core/guest_hook.h"
#ifdef __APPLE__
#include "mac_memory.h"
#endif

namespace Core {
namespace {

constexpr size_t NearJumpBytes = 5;

struct HookModule {
    u8* start;
    u8* end;
    std::unique_ptr<Xbyak::CodeGenerator> trampolines;
    /// Hooked instruction spans: first byte and the game's instructions there.
    std::map<const u8*, std::vector<u8>> patched;
};

std::mutex modules_mutex;
std::map<const u8*, HookModule> modules;

HookModule* ContainingModule(const u8* address) {
    auto next = modules.upper_bound(address);
    if (next == modules.begin()) {
        return nullptr;
    }
    auto& module = std::prev(next)->second;
    return address < module.end ? &module : nullptr;
}

bool OverlapsHook(const HookModule& module, const u8* begin, const u8* end) {
    // The spans never overlap each other, so only the last one starting before `end` can.
    auto next = module.patched.lower_bound(end);
    if (next == module.patched.begin()) {
        return false;
    }
    const auto& [start, original] = *std::prev(next);
    return start + original.size() > begin;
}

#ifndef _WIN32
// The protection of the mapping holding `address`, from /proc/self/maps.
int PageProtection(uintptr_t address) {
#ifdef __APPLE__
    uintptr_t first,last;uint32_t protection;
    return BbSeamless::HostRegion(address,first,last,protection)?static_cast<int>(protection):-1;
#else
    std::ifstream maps{"/proc/self/maps"};
    for (std::string line; std::getline(maps, line);) {
        uintptr_t start{}, end{};
        char dash{};
        std::string perms;
        std::istringstream fields{line};
        fields >> std::hex >> start >> dash >> end >> perms;
        if (address >= start && address < end && perms.size() >= 3) {
            return (perms[0] == 'r' ? PROT_READ : 0) | (perms[1] == 'w' ? PROT_WRITE : 0) |
                   (perms[2] == 'x' ? PROT_EXEC : 0);
        }
    }
    return -1;
#endif
}
#endif

// Writes `bytes` over code that may be mapped read-execute, and puts the protection back.
bool WriteCode(u8* at, const std::vector<u8>& bytes) {
#ifdef _WIN32
    DWORD previous{};
    if (!VirtualProtect(at, bytes.size(), PAGE_EXECUTE_READWRITE, &previous)) {
        return false;
    }
    std::memcpy(at, bytes.data(), bytes.size());
    DWORD unused{};
    VirtualProtect(at, bytes.size(), previous, &unused);
    FlushInstructionCache(GetCurrentProcess(), at, bytes.size());
    return true;
#else
    const auto page_size = static_cast<uintptr_t>(sysconf(_SC_PAGESIZE));
    const auto first = reinterpret_cast<uintptr_t>(at) & ~(page_size - 1);
    const auto last = (reinterpret_cast<uintptr_t>(at) + bytes.size() - 1) & ~(page_size - 1);
    std::vector<int> previous;
    for (uintptr_t page = first; page <= last; page += page_size) {
        previous.push_back(PageProtection(page));
        if (previous.back() < 0) {
            return false;
        }
    }
    if (mprotect(reinterpret_cast<void*>(first), last - first + page_size,
                 PROT_READ | PROT_WRITE | PROT_EXEC) != 0) {
        return false;
    }
    std::memcpy(at, bytes.data(), bytes.size());
    for (size_t index = 0; index < previous.size(); ++index) {
        mprotect(reinterpret_cast<void*>(first + index * page_size), page_size, previous[index]);
    }
    __builtin___clear_cache(reinterpret_cast<char*>(at),
                            reinterpret_cast<char*>(at + bytes.size()));
    return true;
#endif
}

bool InstallGuestHook(void* address, std::span<const u8> expected_instructions, u64 tag,
                      const void* hook, GuestHookReturn returns) {
    auto* code = static_cast<u8*>(address);
    std::scoped_lock lock{modules_mutex};
    auto* module = ContainingModule(code);
    if (module == nullptr || hook == nullptr || expected_instructions.size() < NearJumpBytes ||
        expected_instructions.size() > static_cast<size_t>(module->end - code)) {
        return false;
    }
    if (std::memcmp(code, expected_instructions.data(), expected_instructions.size()) != 0) {
        LOG_ERROR(Core, "Guest code hook signature mismatch at {}", fmt::ptr(code));
        return false;
    }
    if (!CanCopyGuestHookInstructions(code, expected_instructions)) {
        return false;
    }
    if (OverlapsHook(*module, code, code + expected_instructions.size())) {
        LOG_ERROR(Core, "Guest code hook overlaps an existing hook at {}", fmt::ptr(code));
        return false;
    }

    auto& trampolines = *module->trampolines;
    const size_t trampoline_offset = trampolines.getSize();
    const u8* trampoline = trampolines.getCurr();
    try {
        EmitGuestHookTrampoline(trampolines, code, expected_instructions, tag, hook, returns);
    } catch (const Xbyak::Error& error) {
        trampolines.setSize(trampoline_offset);
        LOG_ERROR(Core, "Could not generate guest code hook at {}: {}", fmt::ptr(code),
                  error.what());
        return false;
    }

    // jmp rel32 to the trampoline; the rest of the copied instructions become NOPs.
    const auto displacement =
        reinterpret_cast<intptr_t>(trampoline) - reinterpret_cast<intptr_t>(code + NearJumpBytes);
    if (displacement != static_cast<int32_t>(displacement)) {
        trampolines.setSize(trampoline_offset);
        LOG_ERROR(Core, "Guest code hook trampoline at {} is out of reach of {}",
                  fmt::ptr(trampoline), fmt::ptr(code));
        return false;
    }
    std::vector<u8> jump(expected_instructions.size(), 0x90);
    jump[0] = 0xe9;
    const auto rel32 = static_cast<int32_t>(displacement);
    std::memcpy(&jump[1], &rel32, sizeof(rel32));
    if (!WriteCode(code, jump)) {
        trampolines.setSize(trampoline_offset);
        LOG_ERROR(Core, "Could not write guest code hook at {}", fmt::ptr(code));
        return false;
    }
    module->patched.emplace(
        code, std::vector<u8>(expected_instructions.begin(), expected_instructions.end()));
    return true;
}

} // namespace

void RegisterPatchModule(void* module_ptr, u64 module_size, void* trampoline_area_ptr,
                         u64 trampoline_area_size) {
    auto* start = static_cast<u8*>(module_ptr);
    std::scoped_lock lock{modules_mutex};
    modules[start] = HookModule{
        .start = start,
        .end = start + module_size,
        .trampolines =
            std::make_unique<Xbyak::CodeGenerator>(trampoline_area_size, trampoline_area_ptr),
        .patched = {},
    };
}

bool InstallGuestCodeHook(void* address, std::span<const u8> expected_instructions, u64 tag,
                          GuestCodeHook hook) {
    return InstallGuestHook(address, expected_instructions, tag,
                            reinterpret_cast<const void*>(hook), GuestHookReturn::No);
}

bool InstallGuestFunctionOverride(void* address, std::span<const u8> expected_instructions, u64 tag,
                                  GuestFunctionOverride hook) {
    return InstallGuestHook(address, expected_instructions, tag,
                            reinterpret_cast<const void*>(hook), GuestHookReturn::Zero);
}

bool InstallGuestFunctionResult(void* address, std::span<const u8> expected_instructions, u64 tag,
                                GuestFunctionResult hook) {
    return InstallGuestHook(address, expected_instructions, tag,
                            reinterpret_cast<const void*>(hook), GuestHookReturn::Rax);
}

bool InstallGuestRegisterEditHook(void* address, std::span<const u8> expected_instructions, u64 tag,
                                  GuestRegisterEditHook hook) {
    // The trampoline already reloads every register (and the flags) from the snapshot.
    return InstallGuestHook(address, expected_instructions, tag,
                            reinterpret_cast<const void*>(hook), GuestHookReturn::No);
}

bool RemoveGuestCodeHook(void* address) {
    auto* code = static_cast<u8*>(address);
    std::scoped_lock lock{modules_mutex};
    auto* module = ContainingModule(code);
    if (module == nullptr) {
        return false;
    }
    const auto hook = module->patched.find(code);
    if (hook == module->patched.end() || !WriteCode(code, hook->second)) {
        return false;
    }
    // The trampoline stays where it is, unused.
    module->patched.erase(hook);
    return true;
}

} // namespace Core
