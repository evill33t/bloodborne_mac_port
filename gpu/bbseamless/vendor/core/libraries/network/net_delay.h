// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <algorithm>
#include <charconv>
#include <chrono>
#include <optional>
#include <random>
#include <string_view>

#include "common/types.h"

namespace Libraries::Net {

// A test network's one-way delay in milliseconds (SHADPS4_P2P_DELAY_MS, "40" or "25-50"): the
// rig's ping between games (tools/rig/latency.py). Nothing, 0 or anything else: no delay.
struct DelayRange {
    u32 min_ms{};
    u32 max_ms{};
};

inline std::optional<DelayRange> ParseDelayRange(std::string_view text) {
    constexpr u32 kMaxDelayMs = 10000;
    const auto number = [](std::string_view part) -> std::optional<u32> {
        u32 value = 0;
        const auto [end, error] = std::from_chars(part.data(), part.data() + part.size(), value);
        if (part.empty() || error != std::errc{} || end != part.data() + part.size()) {
            return std::nullopt;
        }
        return value;
    };
    const size_t dash = text.find('-');
    const auto low = number(text.substr(0, dash));
    const auto high = dash == std::string_view::npos ? low : number(text.substr(dash + 1));
    if (!low || !high || *low > *high || *high == 0 || *high > kMaxDelayMs) {
        return std::nullopt;
    }
    return DelayRange{*low, *high};
}

// When each packet sent one way is due: a random point in the range after it was sent, but
// never before the one sent ahead of it (a real link delays without reordering).
class DelayLine {
public:
    using Clock = std::chrono::steady_clock;

    DelayLine(DelayRange range, u32 seed) : range{range}, random{seed} {}

    Clock::time_point DueAt(Clock::time_point sent) {
        std::uniform_int_distribution<u32> pick{range.min_ms * 1000, range.max_ms * 1000};
        const auto due = sent + std::chrono::microseconds{pick(random)};
        last_due = std::max(due, last_due);
        return last_due;
    }

private:
    DelayRange range;
    std::minstd_rand random;
    Clock::time_point last_due{};
};

} // namespace Libraries::Net
