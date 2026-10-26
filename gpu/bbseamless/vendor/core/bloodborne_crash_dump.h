// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "common/types.h"

namespace Core::Bloodborne {

// The crash catcher for the random co-op crashes whose logs just stop: an unhandled exception,
// std::terminate or SIGABRT writes crash-<time>.txt (see FormatCrashLine) and a minidump
// crash-<time>.dmp into the log folder, without the (asynchronous) logger. Windows only.
void InstallCrashCatcher(u64 image_base, u64 image_size);

// Writes the report for an exception (EXCEPTION_POINTERS*, or nullptr outside one). Only the
// first call writes; later ones return at once.
void WriteCrashReport(void* exception_pointers);

// A freeze (see GameThreadStalled): freeze-<time>.txt with the stuck thread's registers and its
// stack's game addresses (the thread is suspended for the read), and freeze-<time>.dmp.
void WriteFreezeReport(u32 thread_id, s64 stalled_ms);

// The party tick's heartbeat (the game thread), and the window's close: a game stuck for
// GameStallMs writes the freeze report before the process ends.
void NoteGameThreadTick();
void ReportFreezeOnClose();

} // namespace Core::Bloodborne
