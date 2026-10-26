// SPDX-License-Identifier: GPL-2.0-or-later
// bbport co-op: what the files in gpu/bbcoop/ share.
#pragma once
#include <cstdint>
#include "common/types.h"

namespace Core {
/// A segment of the game's image (bbcoop_note_segment), for MemoryManager::QueryProtection.
void NoteImageSegment(uintptr_t start, u64 size, unsigned flags);
} // namespace Core
