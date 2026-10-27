// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include "bbport_threads.h"

namespace BbGuest {

// Guest tables can be null, stale or unmapped. Contain recovery within the copy so a
// fault never jumps across a caller's C++ objects. Discard even a partially read value,
// restore any enclosing recovery scope, and retry normally on the next read.
inline bool Read(void* destination, uintptr_t address, size_t size) noexcept {
    if (!address) {
        std::memset(destination, 0, size);
        return false;
    }
    BbRecoverBuf* const outer = runtime_fault_recover;
    BbRecoverBuf recover;
    if (BB_RECOVER_SET(recover)) {
        runtime_fault_recover = outer;
        std::memset(destination, 0, size);
        return false;
    }
    runtime_fault_recover = &recover;
    std::memcpy(destination, reinterpret_cast<const void*>(address), size);
    runtime_fault_recover = outer;
    return true;
}

} // namespace BbGuest
