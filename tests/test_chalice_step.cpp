// SPDX-License-Identifier: GPL-3.0-or-later
#include <array>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <sys/mman.h>
#include <unistd.h>
#include "src/fps/chalice_step_patch.h"

struct Site { uint32_t at; const char* was; };
static constexpr Site sites[] = {
#include "src/fps/chalice_step_sites.inc"
};
static std::vector<uint8_t> hex(const char* text) {
    std::vector<uint8_t> out;
    for (; *text; text += 2) {
        unsigned byte;
        assert(std::sscanf(text, "%2x", &byte) == 1);
        out.push_back(static_cast<uint8_t>(byte));
    }
    return out;
}
int main() {
    const size_t page = static_cast<size_t>(getpagesize());
    auto* memory = static_cast<uint8_t*>(mmap(nullptr, page * 2, PROT_READ | PROT_WRITE,
                                             MAP_ANON | MAP_PRIVATE, -1, 0));
    assert(memory != MAP_FAILED);
    auto* step = reinterpret_cast<float*>(memory + page);
    unsigned cases = 0;
    for (const auto& site : sites) {
        const auto original = hex(site.was);
        const size_t store = original.size() == 22 ? 4 : 7;
        int32_t header_offset = static_cast<int8_t>(original[14]);
        if (store == 7) std::memcpy(&header_offset, original.data() + 14, 4);
        const int32_t seconds_offset = static_cast<int8_t>(original[13 + store]);
        int32_t table;
        std::memcpy(&table, original.data() + 3, 4);
        const uint64_t code = reinterpret_cast<uintptr_t>(memory + 4);
        const uint64_t expected_table = code + 7 + table + 16;
        for (bool replacement : {false, true}) {
            std::vector<uint8_t> bytes = original;
            if (replacement) assert(MacChaliceStep::Build(original, code,
                                        reinterpret_cast<uintptr_t>(step), bytes));
            assert(mprotect(memory, page, PROT_READ | PROT_WRITE) == 0);
            const uint8_t enter[] = {0x55, 0x48, 0x89, 0xfd}; // save RBP; use the supplied frame
            std::memcpy(memory, enter, sizeof(enter));
            std::memcpy(memory + 4, bytes.data(), bytes.size());
            memory[4 + bytes.size()] = 0x5d; memory[5 + bytes.size()] = 0xc3;
            assert(mprotect(memory, page, PROT_READ | PROT_EXEC) == 0);
            for (float dt : {1.f / 240, 1.f / 120, 1.f / 60, .025f, 1.f / 30}) {
                *step = dt;
                std::array<uint8_t, 512> frame, expected;
                frame.fill(0xa5); expected = frame;
                const float seconds = replacement ? dt : 1.f / 30;
                std::memcpy(expected.data() + 256 + header_offset, &expected_table, 8);
                std::memcpy(expected.data() + 256 + seconds_offset, &seconds, 4);
                reinterpret_cast<void(*)(void*)>(memory)(frame.data() + 256);
                assert(frame == expected); // all other fields and padding are preserved
                ++cases;
            }
        }
        std::vector<uint8_t> changed;
        auto wrong = original; wrong[0] ^= 1;
        assert(!MacChaliceStep::Build(wrong, code, reinterpret_cast<uintptr_t>(step), changed));
        assert(!MacChaliceStep::Build(original, code, code + UINT64_C(0x100000000), changed));
    }
    assert(munmap(memory, page * 2) == 0);
    std::printf("PASS: %zu Chalice constructors, %u original/replacement executions, bounds and opcode controls\n",
                std::size(sites), cases);
}
