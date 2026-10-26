// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <array>
#include <cstddef>
#include <Zydis/Zydis.h>
#include <xbyak/xbyak.h>
#include "common/alignment.h"
#include "common/decoder.h"
#include "common/logging/log.h"
#include "core/cpu_patches.h"
#include "core/guest_hook.h"

using namespace Xbyak::util;

namespace Core {

bool CanCopyGuestHookInstructions(const void* address, std::span<const u8> instructions) {
    const auto* code = static_cast<const u8*>(address);
    size_t offset = 0;
    while (offset < instructions.size()) {
        ZydisDecodedInstruction instruction{};
        std::array<ZydisDecodedOperand, ZYDIS_MAX_OPERAND_COUNT> operands{};
        const auto status = Common::Decoder::Instance()->decodeInstruction(
            instruction, operands.data(), const_cast<u8*>(code) + offset,
            instructions.size() - offset);
        if (!ZYAN_SUCCESS(status) || instruction.length == 0 ||
            instruction.length > instructions.size() - offset) {
            LOG_ERROR(Core, "Guest code hook at {} does not cover whole instructions",
                      fmt::ptr(code));
            return false;
        }

        for (u8 index = 0; index < instruction.operand_count; ++index) {
            const auto& operand = operands[index];
            const bool relative_immediate =
                operand.type == ZYDIS_OPERAND_TYPE_IMMEDIATE && operand.imm.is_relative;
            const bool instruction_pointer_memory =
                operand.type == ZYDIS_OPERAND_TYPE_MEMORY &&
                (operand.mem.base == ZYDIS_REGISTER_RIP || operand.mem.base == ZYDIS_REGISTER_EIP);
            if (relative_immediate || instruction_pointer_memory) {
                LOG_ERROR(Core,
                          "Guest code hook at {} contains relative addressing at instruction {}",
                          fmt::ptr(code), fmt::ptr(code + offset));
                return false;
            }
        }
        offset += instruction.length;
    }
    return true;
}

void EmitGuestHookTrampoline(Xbyak::CodeGenerator& trampoline, const u8* code,
                             std::span<const u8> expected_instructions, u64 tag, const void* hook,
                             GuestHookReturn returns) {
    const bool can_return = returns != GuestHookReturn::No;
    constexpr size_t GuestRedZoneBytes = 128;
    constexpr size_t SnapshotOffset = 0;
    constexpr size_t ScratchR11Offset = sizeof(GuestRegisterSnapshot);
    constexpr size_t ScratchRflagsOffset = ScratchR11Offset + sizeof(u64);
    constexpr size_t FrameReserve =
        Common::AlignUp(ScratchRflagsOffset + sizeof(u64) + GuestRedZoneBytes, 16);
    static_assert(FrameReserve % 16 == 0);

    const auto snapshot = [](size_t field_offset) {
        return qword[rsp + SnapshotOffset + field_offset];
    };

    trampoline.lea(rsp, ptr[rsp - FrameReserve]);
    trampoline.mov(qword[rsp + ScratchR11Offset], r11);
    trampoline.pushfq();
    trampoline.pop(r11);
    trampoline.mov(qword[rsp + ScratchRflagsOffset], r11);
    trampoline.mov(r11, rsp);
    trampoline.and_(rsp, -16);

    trampoline.mov(snapshot(offsetof(GuestRegisterSnapshot, rax)), rax);
    trampoline.mov(snapshot(offsetof(GuestRegisterSnapshot, rbx)), rbx);
    trampoline.mov(snapshot(offsetof(GuestRegisterSnapshot, rcx)), rcx);
    trampoline.mov(snapshot(offsetof(GuestRegisterSnapshot, rdx)), rdx);
    trampoline.mov(snapshot(offsetof(GuestRegisterSnapshot, rsi)), rsi);
    trampoline.mov(snapshot(offsetof(GuestRegisterSnapshot, rdi)), rdi);
    trampoline.mov(snapshot(offsetof(GuestRegisterSnapshot, rbp)), rbp);
    trampoline.mov(snapshot(offsetof(GuestRegisterSnapshot, r8)), r8);
    trampoline.mov(snapshot(offsetof(GuestRegisterSnapshot, r9)), r9);
    trampoline.mov(snapshot(offsetof(GuestRegisterSnapshot, r10)), r10);
    trampoline.mov(snapshot(offsetof(GuestRegisterSnapshot, r12)), r12);
    trampoline.mov(snapshot(offsetof(GuestRegisterSnapshot, r13)), r13);
    trampoline.mov(snapshot(offsetof(GuestRegisterSnapshot, r14)), r14);
    trampoline.mov(snapshot(offsetof(GuestRegisterSnapshot, r15)), r15);
    trampoline.mov(rax, qword[r11 + ScratchR11Offset]);
    trampoline.mov(snapshot(offsetof(GuestRegisterSnapshot, r11)), rax);
    trampoline.mov(rax, qword[r11 + ScratchRflagsOffset]);
    trampoline.mov(snapshot(offsetof(GuestRegisterSnapshot, rflags)), rax);
    trampoline.lea(rax, ptr[r11 + FrameReserve]);
    trampoline.mov(snapshot(offsetof(GuestRegisterSnapshot, rsp)), rax);
    trampoline.stmxcsr(dword[rsp + SnapshotOffset + offsetof(GuestRegisterSnapshot, mxcsr)]);

    for (size_t index = 0; index < 16; ++index) {
        const size_t offset = SnapshotOffset + offsetof(GuestRegisterSnapshot, ymm) +
                              index * sizeof(std::array<u8, 32>);
        trampoline.vmovdqu(yword[rsp + offset], Xbyak::Ymm(static_cast<int>(index)));
    }

    // Keep the aligned snapshot base in a callee-saved register across the host call.
    trampoline.mov(r12, rsp);
    trampoline.mov(rdi, tag);
    trampoline.lea(rsi, ptr[r12 + SnapshotOffset]);
    trampoline.mov(rax, reinterpret_cast<u64>(hook));
    trampoline.call(rax);
    trampoline.mov(rsp, r12);

    for (size_t index = 0; index < 16; ++index) {
        const size_t offset = SnapshotOffset + offsetof(GuestRegisterSnapshot, ymm) +
                              index * sizeof(std::array<u8, 32>);
        trampoline.vmovdqu(Xbyak::Ymm(static_cast<int>(index)), yword[rsp + offset]);
    }
    trampoline.ldmxcsr(dword[rsp + SnapshotOffset + offsetof(GuestRegisterSnapshot, mxcsr)]);
    Xbyak::Label return_to_caller;
    if (can_return) {
        // The override returned true: skip the function. Nothing above touched al.
        trampoline.test(al, al);
        trampoline.jnz(return_to_caller, Xbyak::CodeGenerator::LabelType::T_NEAR);
    }
    const auto restore_registers = [&] {
        trampoline.mov(rbx, snapshot(offsetof(GuestRegisterSnapshot, rbx)));
        trampoline.mov(rcx, snapshot(offsetof(GuestRegisterSnapshot, rcx)));
        trampoline.mov(rdx, snapshot(offsetof(GuestRegisterSnapshot, rdx)));
        trampoline.mov(rsi, snapshot(offsetof(GuestRegisterSnapshot, rsi)));
        trampoline.mov(rdi, snapshot(offsetof(GuestRegisterSnapshot, rdi)));
        trampoline.mov(rbp, snapshot(offsetof(GuestRegisterSnapshot, rbp)));
        trampoline.mov(r8, snapshot(offsetof(GuestRegisterSnapshot, r8)));
        trampoline.mov(r9, snapshot(offsetof(GuestRegisterSnapshot, r9)));
        trampoline.mov(r10, snapshot(offsetof(GuestRegisterSnapshot, r10)));
        trampoline.mov(r11, snapshot(offsetof(GuestRegisterSnapshot, r11)));
        trampoline.mov(r12, snapshot(offsetof(GuestRegisterSnapshot, r12)));
        trampoline.mov(r13, snapshot(offsetof(GuestRegisterSnapshot, r13)));
        trampoline.mov(r14, snapshot(offsetof(GuestRegisterSnapshot, r14)));
        trampoline.mov(r15, snapshot(offsetof(GuestRegisterSnapshot, r15)));
        trampoline.mov(rax, snapshot(offsetof(GuestRegisterSnapshot, rflags)));
        trampoline.push(rax);
        trampoline.popfq();
    };
    restore_registers();
    trampoline.mov(rax, snapshot(offsetof(GuestRegisterSnapshot, rax)));
    trampoline.mov(rsp, snapshot(offsetof(GuestRegisterSnapshot, rsp)));
    trampoline.db(expected_instructions.data(), expected_instructions.size());
    trampoline.jmp(code + expected_instructions.size(), Xbyak::CodeGenerator::LabelType::T_NEAR);
    if (can_return) {
        // At a function entry: return 0 (or the hook's rax) to the caller with its registers
        // intact.
        trampoline.L(return_to_caller);
        restore_registers();
        if (returns == GuestHookReturn::Rax) {
            trampoline.mov(rax, snapshot(offsetof(GuestRegisterSnapshot, rax)));
        } else {
            trampoline.mov(rax, 0);
        }
        trampoline.mov(rsp, snapshot(offsetof(GuestRegisterSnapshot, rsp)));
        trampoline.ret();
    }
}

} // namespace Core
