// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cstddef>

#include "common/types.h"

namespace Core::Bloodborne {

// Called on the writing thread right after a write to the watched byte: rip is the instruction
// after the write, rsp the stack it ran on.
using WriteObserver = void (*)(u64 rip, u64 rsp);

// A hardware write watch (debug register 0) on one byte, armed on every thread of the process
// that exists now; call again to arm threads started since. Diagnostics only. False off Windows
// or when no thread could be armed.
bool WatchByteWrites(u64 address, WriteObserver observer);

// Kaniel's playtest 2026-10-16: krolebord's game crashed reading the stack past its end in a
// watched write (a worker thread with a small stack). How many bytes from `address` can be read
// (at most `most`; 0 when it cannot): the rest of its committed, readable region.
size_t ReadableBytesAt(u64 address, size_t most);
// The stack slots (8 bytes) a scan from `rsp` may read: at most `most`, within `readable` bytes.
constexpr size_t StackSlotsToScan(size_t readable, size_t most) {
    return readable / 8 < most ? readable / 8 : most;
}

} // namespace Core::Bloodborne
