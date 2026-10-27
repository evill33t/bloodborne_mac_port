// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "core/bloodborne_crash_dump.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <exception>
#include <span>
#include <string>

#include <fmt/format.h>

#include "common/logging/log.h"
#include "common/path_util.h"
#include "core/bloodborne_party.h"

#ifdef _WIN32
#include <windows.h>
// After windows.h, which it needs.
#include <dbghelp.h>
#endif

namespace Core::Bloodborne {

#ifdef _WIN32
namespace {

std::atomic<u64> crash_image_base{0};
std::atomic<u64> crash_image_size{0};
std::atomic<bool> report_written{false};

std::string ThreadName() {
    PWSTR description = nullptr;
    std::string name;
    if (SUCCEEDED(GetThreadDescription(GetCurrentThread(), &description)) && description) {
        for (const wchar_t* c = description; *c != 0 && name.size() < 64; ++c) {
            name += *c < 0x80 ? static_cast<char>(*c) : '?';
        }
        LocalFree(description);
    }
    return name;
}

void WriteFileText(const std::wstring& path, const std::string& text) {
    const HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                    FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return;
    }
    DWORD written = 0;
    WriteFile(file, text.data(), static_cast<DWORD>(text.size()), &written, nullptr);
    CloseHandle(file);
}

LONG WINAPI OnUnhandledException(EXCEPTION_POINTERS* exception) {
    WriteCrashReport(exception);
    return EXCEPTION_CONTINUE_SEARCH;
}

void OnAbort(int) {
    WriteCrashReport(nullptr);
}

void OnTerminate() {
    WriteCrashReport(nullptr);
    std::abort();
}

} // namespace

void WriteCrashReport(void* exception_pointers) {
    if (report_written.exchange(true)) {
        return;
    }
    auto* exception = static_cast<EXCEPTION_POINTERS*>(exception_pointers);
    const bool has_record = exception != nullptr && exception->ExceptionRecord != nullptr;
    const u32 code = has_record ? exception->ExceptionRecord->ExceptionCode : 0;
    const u64 address =
        has_record ? reinterpret_cast<u64>(exception->ExceptionRecord->ExceptionAddress) : 0;
    std::string text = FormatCrashLine(code, address, crash_image_base.load(),
                                       crash_image_size.load(), ThreadName());
    if (has_record && code == EXCEPTION_ACCESS_VIOLATION &&
        exception->ExceptionRecord->NumberParameters >= 2) {
        text += fmt::format(" ({} {:#x})",
                            exception->ExceptionRecord->ExceptionInformation[0] ? "write to"
                                                                                : "read from",
                            exception->ExceptionRecord->ExceptionInformation[1]);
    }
    if (exception == nullptr) {
        text += " (abort or std::terminate)";
    }
    text += '\n';
    if (exception != nullptr && exception->ContextRecord != nullptr) {
        const CONTEXT& context = *exception->ContextRecord;
        text +=
            fmt::format("rax={:#x} rbx={:#x} rcx={:#x} rdx={:#x} rsi={:#x} rdi={:#x} rbp={:#x} "
                        "rsp={:#x}\nr8={:#x} r9={:#x} r10={:#x} r11={:#x} r12={:#x} r13={:#x} "
                        "r14={:#x} r15={:#x}\n",
                        context.Rax, context.Rbx, context.Rcx, context.Rdx, context.Rsi,
                        context.Rdi, context.Rbp, context.Rsp, context.R8, context.R9, context.R10,
                        context.R11, context.R12, context.R13, context.R14, context.R15);
        // The guest's stack is not in the minidump: its eboot words are the call chain.
        MEMORY_BASIC_INFORMATION region{};
        if (VirtualQuery(reinterpret_cast<void*>(context.Rsp), &region, sizeof(region)) != 0 &&
            region.State == MEM_COMMIT && (region.Protect & (PAGE_READWRITE | PAGE_READONLY))) {
            const u64 end = reinterpret_cast<u64>(region.BaseAddress) + region.RegionSize;
            const size_t count = static_cast<size_t>(std::min<u64>((end - context.Rsp) / 8, 2048));
            const std::span words{reinterpret_cast<const u64*>(context.Rsp), count};
            text +=
                "stack:" +
                FormatGameAddresses(words, crash_image_base.load(), crash_image_size.load(), 48) +
                '\n';
        }
    }

    const auto stamp = std::chrono::duration_cast<std::chrono::seconds>(
                           std::chrono::system_clock::now().time_since_epoch())
                           .count();
    const auto folder = Common::FS::GetUserPath(Common::FS::PathType::LogDir);
    WriteFileText((folder / fmt::format("crash-{}.txt", stamp)).wstring(), text);
    // The log's last lines before the dump, which can take seconds (the port's log writes on a
    // thread of its own; Flush's waits are bounded).
    Common::Log::Flush();

    const HANDLE dump =
        CreateFileW((folder / fmt::format("crash-{}.dmp", stamp)).c_str(), GENERIC_WRITE, 0,
                    nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (dump == INVALID_HANDLE_VALUE) {
        return;
    }
    MINIDUMP_EXCEPTION_INFORMATION info{};
    info.ThreadId = GetCurrentThreadId();
    info.ExceptionPointers = exception;
    info.ClientPointers = FALSE;
    const auto type = static_cast<MINIDUMP_TYPE>(MiniDumpWithIndirectlyReferencedMemory |
                                                 MiniDumpScanMemory | MiniDumpWithThreadInfo);
    MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), dump, type,
                      exception != nullptr ? &info : nullptr, nullptr, nullptr);
    CloseHandle(dump);
}

void WriteFreezeReport(u32 thread_id, s64 stalled_ms) {
    std::string text =
        fmt::format("freeze: the game thread {} has not ticked for {} ms\n", thread_id, stalled_ms);
    const HANDLE thread = OpenThread(
        THREAD_GET_CONTEXT | THREAD_SUSPEND_RESUME | THREAD_QUERY_INFORMATION, FALSE, thread_id);
    if (thread != nullptr && SuspendThread(thread) != static_cast<DWORD>(-1)) {
        CONTEXT context{};
        context.ContextFlags = CONTEXT_FULL;
        if (GetThreadContext(thread, &context)) {
            const u64 base = crash_image_base.load();
            const u64 size = crash_image_size.load();
            text += fmt::format("rip={:#x}{} rsp={:#x} rbp={:#x}\n", context.Rip,
                                context.Rip >= base && context.Rip < base + size
                                    ? fmt::format(" (eboot+{:#x})", context.Rip - base)
                                    : std::string{},
                                context.Rsp, context.Rbp);
            text += fmt::format("rax={:#x} rbx={:#x} rcx={:#x} rdx={:#x} rsi={:#x} rdi={:#x}\n"
                                "r8={:#x} r9={:#x} r10={:#x} r11={:#x} r12={:#x} r13={:#x} "
                                "r14={:#x} r15={:#x}\n",
                                context.Rax, context.Rbx, context.Rcx, context.Rdx, context.Rsi,
                                context.Rdi, context.R8, context.R9, context.R10, context.R11,
                                context.R12, context.R13, context.R14, context.R15);
            MEMORY_BASIC_INFORMATION region{};
            if (VirtualQuery(reinterpret_cast<void*>(context.Rsp), &region, sizeof(region)) != 0 &&
                region.State == MEM_COMMIT && (region.Protect & (PAGE_READWRITE | PAGE_READONLY))) {
                const u64 end = reinterpret_cast<u64>(region.BaseAddress) + region.RegionSize;
                const size_t count =
                    static_cast<size_t>(std::min<u64>((end - context.Rsp) / 8, 2048));
                const std::span words{reinterpret_cast<const u64*>(context.Rsp), count};
                text += "stack:" + FormatGameAddresses(words, base, size, 48) + '\n';
            }
        }
        ResumeThread(thread);
    }
    if (thread != nullptr) {
        CloseHandle(thread);
    }
    const auto stamp = std::chrono::duration_cast<std::chrono::seconds>(
                           std::chrono::system_clock::now().time_since_epoch())
                           .count();
    const auto folder = Common::FS::GetUserPath(Common::FS::PathType::LogDir);
    WriteFileText((folder / fmt::format("freeze-{}.txt", stamp)).wstring(), text);
    Common::Log::Flush();
    const HANDLE dump =
        CreateFileW((folder / fmt::format("freeze-{}.dmp", stamp)).c_str(), GENERIC_WRITE, 0,
                    nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (dump == INVALID_HANDLE_VALUE) {
        return;
    }
    const auto type = static_cast<MINIDUMP_TYPE>(MiniDumpWithIndirectlyReferencedMemory |
                                                 MiniDumpScanMemory | MiniDumpWithThreadInfo);
    MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), dump, type, nullptr, nullptr,
                      nullptr);
    CloseHandle(dump);
}

void InstallCrashCatcher(u64 image_base, u64 image_size) {
    crash_image_base.store(image_base);
    crash_image_size.store(image_size);
    SetUnhandledExceptionFilter(OnUnhandledException);
    std::set_terminate(OnTerminate);
    std::signal(SIGABRT, OnAbort);
}
#else
void InstallCrashCatcher(u64, u64) {}
void WriteCrashReport(void*) {}
void WriteFreezeReport(u32, s64) {}
#endif

namespace {
std::atomic<s64> game_tick_ms{0};
std::atomic<u32> game_thread_id{0};

s64 NowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}
} // namespace

void NoteGameThreadTick() {
#ifdef _WIN32
    game_thread_id.store(GetCurrentThreadId(), std::memory_order_relaxed);
#endif
    game_tick_ms.store(NowMs(), std::memory_order_release);
}

void ReportFreezeOnClose() {
    const s64 last = game_tick_ms.load(std::memory_order_acquire);
    const s64 now = NowMs();
    if (GameThreadStalled(now, last)) {
        WriteFreezeReport(game_thread_id.load(), now - last);
    }
}

} // namespace Core::Bloodborne
