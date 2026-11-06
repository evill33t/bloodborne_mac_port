// bbport: replaces shadPS4's spdlog-based logger with a small fmt sink.
// Level from BB_GPU_LOG (trace|debug|info|warning|error; default warning).
// A production build (BB_PRODUCTION=1, make-release) has no LOG_TRACE or LOG_DEBUG lines at all,
// and its log file keeps warnings and worse unless the bug-report switch is on (log.cpp).
#pragma once
#include <algorithm>
#include <array>
#include <cstring>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>
#include <fmt/ranges.h>
#include <fmt/std.h>
#include <string_view>
#include <fmt/format.h>
#include "common/logging/classes.h"
#include "common/path_util.h"
#include "common/thread.h"

// Level type keeps shadPS4's spdlog spelling so vendored callers compile unchanged.
namespace spdlog {
enum class level : int { trace, debug, info, warn, err, critical, off };
}
#ifndef BB_PRODUCTION
#define BB_PRODUCTION 0
#endif

namespace Common::Log {
using Level = spdlog::level;
// The lowest level that goes anywhere (the console's or the file's), read from the settings once.
int ComputeLowestLevel();
// Every LOG_* line asks this first: a load and a compare once the settings are read, so a line
// below the level costs the game thread next to nothing (no call, no formatting, no lock).
inline bool ShouldLog(Level level) {
    static const int lowest = ComputeLowestLevel();
    return static_cast<int>(level) >= lowest;
}
void Write(std::string_view log_class, Level level, const char* file, int line, const char* function,
           std::string&& message);
// Writes out every line still waiting for the log's writer thread (see log.cpp): the crash and
// freeze reports and the loader's exits call it. Bounded waits, so a crash handler may call it.
void Flush();
} // namespace Common::Log

#define LOG_GENERIC(log_class, log_level, fmt_str, ...)                                             \
    do {                                                                                           \
        if (Common::Log::ShouldLog(log_level)) {                                                   \
            Common::Log::Write(log_class, log_level, __FILE__, __LINE__, __func__,                 \
                               fmt::format(fmt_str __VA_OPT__(, ) __VA_ARGS__));                    \
        }                                                                                          \
    } while (false)
#if BB_PRODUCTION
// Compiled out: still type-checked (a dev build must not break on them), never run, and their
// arguments are never evaluated.
#define LOG_DISCARDED(log_class, log_level, ...)                                                   \
    do {                                                                                           \
        if constexpr (false) {                                                                     \
            LOG_GENERIC(log_class, log_level, __VA_ARGS__);                                        \
        }                                                                                          \
    } while (false)
#define LOG_TRACE(log_class, ...) LOG_DISCARDED(Common::Log::Class::log_class, spdlog::level::trace, __VA_ARGS__)
#define LOG_DEBUG(log_class, ...) LOG_DISCARDED(Common::Log::Class::log_class, spdlog::level::debug, __VA_ARGS__)
#else
#define LOG_TRACE(log_class, ...) LOG_GENERIC(Common::Log::Class::log_class, spdlog::level::trace, __VA_ARGS__)
#define LOG_DEBUG(log_class, ...) LOG_GENERIC(Common::Log::Class::log_class, spdlog::level::debug, __VA_ARGS__)
#endif
#define LOG_INFO(log_class, ...) LOG_GENERIC(Common::Log::Class::log_class, spdlog::level::info, __VA_ARGS__)
#define LOG_WARNING(log_class, ...) LOG_GENERIC(Common::Log::Class::log_class, spdlog::level::warn, __VA_ARGS__)
#define LOG_ERROR(log_class, ...) LOG_GENERIC(Common::Log::Class::log_class, spdlog::level::err, __VA_ARGS__)
#define LOG_CRITICAL(log_class, ...) LOG_GENERIC(Common::Log::Class::log_class, spdlog::level::critical, __VA_ARGS__)
// A few lines a game that a production log still needs (the build, the start's settings): info in
// a dev build, as before, and a warning in a production one, whose file keeps warnings only.
#if BB_PRODUCTION
#define LOG_NOTICE(log_class, ...) LOG_WARNING(log_class, __VA_ARGS__)
#else
#define LOG_NOTICE(log_class, ...) LOG_INFO(log_class, __VA_ARGS__)
#endif
// Whether info lines go anywhere: work done only to write one (a string built before LOG_INFO, a
// position trail kept for a line) skips itself when it is false (a production game's default).
#define LOG_INFO_ON() Common::Log::ShouldLog(spdlog::level::info)
