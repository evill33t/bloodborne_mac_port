// bbport: see log.h. Messages go to stderr from BB_GPU_LOG's level on and, with BB_LOG_FILE set,
// to that file from BB_LOG_LEVEL's level on (default info), in shadPS4's own line format: the
// co-op rig's tools read it as an emulator log (tools/re/check_party_log.py).
//
// The calling thread only formats its line and queues it: a thread of the log's own writes the
// lines out and flushes them at once, so the game thread never waits for the disk or the console
// (Kaniel 2026-10-25: performance first). Nothing is held back for long: the writer wakes for the
// first line of each burst, and the crash and freeze reports, the in-game restart and every exit
// path of the loader call Flush (bbgpu_log_flush), so a crash keeps its last lines.
// BB_LOG_APPEND=1 (run.py's in-game restart) keeps what the run before the restart wrote.
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <system_error>
#include <thread>
#include <vector>
#include "common/logging/log.h"

namespace Common::Log {
static Level ParseLevel(const char* v, Level fallback) {
    if (!v) return fallback;
    static const char* names[] = {"trace", "debug", "info", "warning", "error", "critical"};
    for (int i = 0; i < 6; ++i)
        if (!std::strcmp(v, names[i])) return Level(i);
    return fallback;
}
static Level threshold() {
    static const Level level = ParseLevel(std::getenv("BB_GPU_LOG"), Level::warn);
    return level;
}
struct FileSink {
    std::FILE* file{};
    Level threshold{Level::off};
};
static const FileSink& file_sink() {
    static const FileSink sink = [] {
        FileSink opened;
        const char* path = std::getenv("BB_LOG_FILE");
        if (!path || !*path) return opened;
        std::error_code error;
        std::filesystem::create_directories(std::filesystem::path{path}.parent_path(), error);
        const char* append = std::getenv("BB_LOG_APPEND");
        const bool restarted = append && !std::strcmp(append, "1");
        opened.file = std::fopen(path, restarted ? "a" : "w");
        if (opened.file) opened.threshold = ParseLevel(std::getenv("BB_LOG_LEVEL"), Level::info);
        // At the end first: Windows' C runtime opens "a" at position 0 until the first write.
        if (opened.file && restarted && std::fseek(opened.file, 0, SEEK_END) == 0 &&
            std::ftell(opened.file) > 0) {
            std::fputs("[Log] <Info> () log.cpp:0 Open: the game restarted; its log goes on below\n",
                       opened.file);
            std::fflush(opened.file);
        }
        return opened;
    }();
    return sink;
}

namespace {
constexpr std::size_t QueueCap = 64u << 20; // bytes waiting; past it lines are dropped and counted

struct Line {
    std::string to_stderr; // "" when it goes to only one of them
    std::string to_file;
};

// The lines waiting for the writer. Never destroyed: at exit the detached writer may still wait
// on it, and other threads may still log while the statics go in no defined order.
struct Queue {
    std::mutex mutex;
    std::condition_variable ready;
    std::vector<Line> lines;
    std::size_t bytes = 0;
    unsigned long long dropped = 0;
    // Whoever writes lines out holds it (the writer thread, or Flush), so they keep their order.
    // Taken before `mutex`, never while holding it.
    std::mutex writing;
};
Queue& queue = *new Queue;

// Takes what waits; the caller holds queue.writing.
std::vector<Line> TakeLines(std::unique_lock<std::mutex>& lock, unsigned long long& dropped) {
    std::vector<Line> lines;
    lines.swap(queue.lines);
    queue.bytes = 0;
    dropped = std::exchange(queue.dropped, 0);
    lock.unlock();
    return lines;
}

void WriteOut(const std::vector<Line>& lines, unsigned long long dropped) {
    const auto& sink = file_sink();
    bool to_stderr = false;
    for (const Line& line : lines) {
        if (!line.to_stderr.empty()) {
            std::fwrite(line.to_stderr.data(), 1, line.to_stderr.size(), stderr);
            to_stderr = true;
        }
        if (!line.to_file.empty() && sink.file) {
            std::fwrite(line.to_file.data(), 1, line.to_file.size(), sink.file);
        }
    }
    if (dropped != 0 && sink.file) {
        std::fprintf(sink.file, "[Log] <Warning> () log.cpp:0 WriteOut: %llu lines lost (queue full)\n",
                     dropped);
    }
    if (sink.file) std::fflush(sink.file); // the rig reads it while the game runs
    if (to_stderr) std::fflush(stderr);
}

void Writer() {
    Common::SetCurrentThreadName("LogWriter"); // in the Runtime CPU report's list
    while (true) {
        {
            std::unique_lock lock{queue.mutex};
            queue.ready.wait(lock, [] { return !queue.lines.empty() || queue.dropped != 0; });
        }
        std::scoped_lock writing{queue.writing};
        std::unique_lock lock{queue.mutex};
        unsigned long long dropped;
        const std::vector<Line> lines = TakeLines(lock, dropped);
        WriteOut(lines, dropped);
    }
}

void StartWriter() {
    static const bool started = [] {
        std::thread{Writer}.detach();
        std::atexit(Flush);
        return true;
    }();
    (void)started;
}
} // namespace

bool ShouldLog(Level level) {
    return level >= threshold() || (file_sink().file && level >= file_sink().threshold);
}
void Write(std::string_view log_class, Level level, const char* file, int line, const char* function,
           std::string&& message) {
    static const char* names[] = {"Trace", "Debug", "Info", "Warning", "Error", "Critical"};
    const char* base = std::strrchr(file, '/');
#ifdef _WIN32
    if (const char* backslash = std::strrchr(base ? base : file, '\\')) base = backslash;
#endif
    base = base ? base + 1 : file;
    const auto& sink = file_sink();
    Line text;
    if (level >= threshold()) {
        text.to_stderr = fmt::format("GPU [{}] <{}> {}:{} {}: {}\n", log_class, names[int(level)], base,
                                     line, function, message);
    }
    if (sink.file && level >= sink.threshold) {
        text.to_file = fmt::format("[{}] <{}> ({}) {}:{} {}: {}\n", log_class, names[int(level)],
                                   Common::GetCurrentThreadName(), base, line,
                                   std::strcmp(function, "operator()") ? function : "lambda", message);
    }
    const std::size_t size = text.to_stderr.size() + text.to_file.size();
    if (size == 0) return;
    StartWriter();
    bool first = false;
    {
        std::scoped_lock lock{queue.mutex};
        if (queue.bytes + size > QueueCap) {
            ++queue.dropped;
            return;
        }
        first = queue.lines.empty();
        queue.bytes += size;
        queue.lines.push_back(std::move(text));
    }
    if (first) queue.ready.notify_one(); // a burst wakes the writer once
}
// Up to half a second.
template <typename Lock>
static bool TryLockFor(Lock& lock) {
    for (int tries = 0; !lock.try_lock(); ++tries) {
        if (tries == 50) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds{10});
    }
    return true;
}
void Flush() {
    // From a crash handler too: a thread that died holding a lock must not hang the report, so
    // both waits are bounded, and a lock not had leaves the lines where they are.
    std::unique_lock writing{queue.writing, std::defer_lock};
    std::unique_lock lock{queue.mutex, std::defer_lock};
    if (!TryLockFor(writing) || !TryLockFor(lock)) return;
    unsigned long long dropped;
    const std::vector<Line> lines = TakeLines(lock, dropped);
    WriteOut(lines, dropped);
}
} // namespace Common::Log

extern "C" void bbgpu_log_flush(void) {
    Common::Log::Flush();
}
