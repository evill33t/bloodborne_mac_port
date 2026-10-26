// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "core/bloodborne_write_watch.h"

#include <algorithm>
#include <atomic>
#include <mutex>

#ifdef _WIN32
#include <windows.h>
// After windows.h, which it needs.
#include <tlhelp32.h>
#endif

namespace Core::Bloodborne {

#ifdef _WIN32
namespace {

std::atomic<WriteObserver> write_observer{nullptr};
std::once_flag handler_once;

LONG WINAPI OnWatchedWrite(EXCEPTION_POINTERS* exception) noexcept {
    if (exception == nullptr || exception->ExceptionRecord == nullptr ||
        exception->ExceptionRecord->ExceptionCode != EXCEPTION_SINGLE_STEP ||
        (exception->ContextRecord->Dr6 & 1) == 0) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    exception->ContextRecord->Dr6 = 0;
    if (const WriteObserver observer = write_observer.load(); observer != nullptr) {
        observer(exception->ContextRecord->Rip, exception->ContextRecord->Rsp);
    }
    return EXCEPTION_CONTINUE_EXECUTION;
}

} // namespace

size_t ReadableBytesAt(u64 address, size_t most) {
    MEMORY_BASIC_INFORMATION info{};
    if (VirtualQuery(reinterpret_cast<const void*>(address), &info, sizeof(info)) == 0 ||
        info.State != MEM_COMMIT || (info.Protect & (PAGE_NOACCESS | PAGE_GUARD)) != 0) {
        return 0;
    }
    const u64 end = reinterpret_cast<u64>(info.BaseAddress) + info.RegionSize;
    return end > address ? static_cast<size_t>(std::min<u64>(end - address, most)) : 0;
}

bool WatchByteWrites(u64 address, WriteObserver observer) {
    std::call_once(handler_once, [] { AddVectoredExceptionHandler(1, OnWatchedWrite); });
    write_observer.store(observer);
    const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snapshot == INVALID_HANDLE_VALUE) {
        return false;
    }
    const DWORD process = GetCurrentProcessId();
    const DWORD self = GetCurrentThreadId();
    int armed = 0;
    THREADENTRY32 entry{};
    entry.dwSize = sizeof(entry);
    for (BOOL more = Thread32First(snapshot, &entry); more; more = Thread32Next(snapshot, &entry)) {
        if (entry.th32OwnerProcessID != process || entry.th32ThreadID == self) {
            continue;
        }
        const HANDLE thread =
            OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_SET_CONTEXT, FALSE,
                       entry.th32ThreadID);
        if (thread == nullptr) {
            continue;
        }
        if (SuspendThread(thread) != static_cast<DWORD>(-1)) {
            CONTEXT context{};
            context.ContextFlags = CONTEXT_DEBUG_REGISTERS;
            if (GetThreadContext(thread, &context)) {
                context.Dr0 = address;
                // L0 enabled, R/W0 = 01 (writes), LEN0 = 00 (one byte).
                context.Dr7 = (context.Dr7 & ~u64{0xF0003}) | 0x10001;
                if (SetThreadContext(thread, &context)) {
                    ++armed;
                }
            }
            ResumeThread(thread);
        }
        CloseHandle(thread);
    }
    CloseHandle(snapshot);
    return armed > 0;
}
#else
size_t ReadableBytesAt(u64, size_t most) {
    return most;
}

bool WatchByteWrites(u64, WriteObserver) {
    return false;
}
#endif

} // namespace Core::Bloodborne
