// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <span>

#include "common/types.h"

namespace Xbyak {
class CodeGenerator;
}

namespace Core {

// The trampoline half of a guest code hook (InstallGuestCodeHook and its variants in
// cpu_patches.h), shared by every host that installs them: shadPS4's CPU patcher here and the
// native port's hook installer (bbport/gpu/bbcoop/guest_hooks.cpp).

/// True when `instructions`, the bytes at `address`, are whole instructions that can run from a
/// trampoline: no relative immediates and no RIP-relative memory operands.
bool CanCopyGuestHookInstructions(const void* address, std::span<const u8> instructions);

/// What a hook at a function's entry may make the function return to its caller instead of
/// running: nothing (it never skips), 0, or the rax the hook leaves in the snapshot.
enum class GuestHookReturn : u8 { No, Zero, Rax };

/// Writes the trampoline of a hook at `code` into `trampoline`. It saves the registers, flags,
/// MXCSR and YMM state to a GuestRegisterSnapshot, calls `hook(tag, snapshot)`, reloads them from
/// the snapshot, runs the copied `expected_instructions` and jumps back after them. Unless
/// `returns` is No, a hook returning true makes the guest function return to its caller instead
/// (0, or the snapshot's rax). The caller patches `code` to jump to the trampoline's start.
/// Throws Xbyak::Error when the trampoline does not fit or cannot reach `code`; the caller
/// rewinds the generator.
void EmitGuestHookTrampoline(Xbyak::CodeGenerator& trampoline, const u8* code,
                             std::span<const u8> expected_instructions, u64 tag, const void* hook,
                             GuestHookReturn returns);

} // namespace Core
