// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// Joining: summon placement, cross-map joins, join tasks, session members. Split from
// bloodborne_re.cpp; the shared declarations are in bloodborne_re_internal.h.

#include "core/bloodborne_crash_dump.h"
#include "core/bloodborne_lamp_points.h"
#include "core/bloodborne_party.h"
#include "core/bloodborne_re.h"
#include "core/bloodborne_re_internal.h"
#include "core/bloodborne_write_watch.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <charconv>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <map>
#include <mutex>
#include <optional>
#include <random>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>
#ifdef _WIN32
#include <process.h>
#else
#include <unistd.h>
#endif

#include <fmt/ranges.h>
#include "common/logging/log.h"
#include "common/memory_patcher.h"
#include "common/path_util.h"
#include "common/singleton.h"
#include "core/cpu_patches.h"
#include "core/debugger.h"
#include "core/emulator_settings.h"
#include "core/file_format/psf.h"
#include "core/libraries/network/sockets.h"
#include "core/libraries/np/np_handler.h"
#include "core/libraries/np/np_matching2/np_matching2_signaling.h"
#include "core/libraries/np/np_trophy.h"
#include "core/libraries/system/userservice.h"
#include "core/memory.h"

namespace Core::Bloodborne {
namespace {

struct NativeCallSignature {
    std::string_view name;
    u64 offset;
    std::array<u8, 16> prologue;
    u8 prologue_size;
};
constexpr u64 SummonManagerRootPointerOffset = 0x0553B120; // FrpgNetMan
constexpr u64 MatchingStatePointerOffset = 0x05540290;
constexpr u64 SetForcedSummonMapOffset = 0x0156CF10;
constexpr u64 SetForcedSummonPositionOffset = 0x0156CF20;
constexpr u64 SetForcedSummonOrientationOffset = 0x0156CF40;
constexpr u64 SetForcedSummonWarpOffset = 0x0156CF60;
constexpr u64 SelectSummonedPlacementOffset = 0x01332BC0;
constexpr u64 SummonedMapReloadOffset = 0x01336B90;
constexpr u64 StageTransitionOffset = 0x013CDE30;
constexpr u64 SetSummonReloadStateOffset = 0x0178D9A0;
constexpr u32 ResponderResumeStableObservations = 20;
constexpr u32 ResponderResumeMaxAttempts = 2;
constexpr auto ResponderResumeRetryDelay = std::chrono::seconds{15};

constexpr auto CrossMapNativeCalls = std::to_array<NativeCallSignature>({
    {"SetForcedSummonMap",
     SetForcedSummonMapOffset,
     {0x48, 0x8B, 0x05, 0x61, 0x97, 0xFE, 0x03, 0x8B, 0x0F, 0x89, 0x88, 0xF0, 0x14, 0x00, 0x00,
      0xC3},
     16},
    {"SetForcedSummonPosition",
     SetForcedSummonPositionOffset,
     {0x48, 0x8B, 0x05, 0x51, 0x97, 0xFE, 0x03, 0xC5, 0xF8, 0x28, 0x07, 0xC5, 0xF8, 0x29, 0x80,
      0x00},
     16},
    {"SetForcedSummonOrientation",
     SetForcedSummonOrientationOffset,
     {0x48, 0x8B, 0x05, 0x31, 0x97, 0xFE, 0x03, 0xC5, 0xF8, 0x28, 0x07, 0xC5, 0xF8, 0x29, 0x80,
      0x10},
     16},
    {"SetForcedSummonWarp",
     SetForcedSummonWarpOffset,
     {0x48, 0x8B, 0x05, 0x11, 0x97, 0xFE, 0x03, 0xC6, 0x80, 0x20, 0x15, 0x00, 0x00, 0x01, 0xC3},
     15},
    {"SelectSummonedPlacement",
     SelectSummonedPlacementOffset,
     {0x55, 0x48, 0x89, 0xE5, 0x41, 0x57, 0x41, 0x56, 0x53, 0x48, 0x83, 0xEC, 0x18},
     13},
    {"SummonedMapReload",
     SummonedMapReloadOffset,
     {0x55, 0x48, 0x89, 0xE5, 0x48, 0x8D, 0x05, 0x2D, 0x45, 0x20, 0x04, 0x48, 0x83, 0x38, 0x00},
     15},
    {"StageTransition",
     StageTransitionOffset,
     {0x55, 0x48, 0x89, 0xE5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54, 0x53, 0x50},
     14},
    {"SetSummonReloadState",
     SetSummonReloadStateOffset,
     {0xC7, 0x87, 0x84, 0x00, 0x00, 0x00, 0x03, 0x00, 0x00, 0x00, 0xC3},
     11},
    {"MultiPlayStopRequest",
     MultiPlayStopRequestOffset,
     {0x55, 0x48, 0x89, 0xE5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54, 0x53, 0x48, 0x83,
      0xEC},
     16},
    {"AddSpEffect",
     AddSpEffectOffset,
     {0x55, 0x48, 0x89, 0xE5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54, 0x53, 0x48, 0x83,
      0xEC},
     16},
});

} // namespace

namespace Coop {

std::atomic<u64> event_sequence{};
std::mutex capture_mutex;

thread_local HostPlacementRewriteRecord host_placement_rewrite{};

} // namespace Coop

namespace {

std::mutex seamless_placement_mutex;
std::optional<SummonPlacementDescriptor> seamless_host_placement;
std::optional<SummonPlacementDescriptor> seamless_received_host_placement;
bool seamless_received_host_placement_consumed{};
JoinPlacementLife seamless_received_host_placement_life;

} // namespace

namespace Coop {

std::atomic<u32> pending_summon_reload_map{};
std::atomic<u32> pending_summon_reload_state_map{};

thread_local GuestPlacementHandoffRecord guest_placement_handoff{};

thread_local SummonReloadStateRecord summon_reload_state{};

thread_local DeferredSummonReloadRecord deferred_summon_reload{};

thread_local PreMatchGuestWarpRecord pre_match_guest_warp{};

thread_local PendingResponderResume pending_responder_resume{};

thread_local ResponderResumeRecord responder_resume{};

thread_local HealingFountainAvailabilityRecord healing_fountain_availability{};

} // namespace Coop

namespace {

bool ReadLocalSummonPlacement(SummonPlacementDescriptor& placement, std::string_view& result) {
    const u64 manager_root = GetSummonManagerRoot();
    if (manager_root < 0x10000 ||
        !HasMemoryAccess(manager_root, 0xA80 + sizeof(s32), MemoryProt::CpuRead)) {
        result = "invalid_manager";
        return false;
    }

    // Raw: the summon hooks run before the tick, while the join's load places the player.
    placement.packed_region = GetCurrentPackedMap();
    placement.area = ReadValue<s32>(manager_root, 0xA80);
    if (!IsUsablePackedMap(placement.packed_region)) {
        result = "invalid_map";
        return false;
    }

    const u64 local_state = ReadValue<u64>(image_base + CandidateLocalStatePointerOffset, 0);
    const u64 local_world = local_state >= 0x10000 ? ReadValue<u64>(local_state, 0x60) : 0;
    const u64 character = local_world >= 0x10000 ? ReadValue<u64>(local_world, 0x58) : 0;
    const u64 character_state = character >= 0x10000 ? ReadValue<u64>(character, 0x08) : 0;
    const u64 character_model =
        character_state >= 0x10000 ? ReadValue<u64>(character_state, 0x3B0) : 0;
    const u64 transform = character_model >= 0x10000 ? ReadValue<u64>(character_model, 0x68) : 0;
    if (transform < 0x10000 || !HasMemoryAccess(transform, 0x1EC, MemoryProt::CpuRead)) {
        result = "invalid_transform";
        return false;
    }

    placement.heading = ReadValue<float>(transform, 0x1D4);
    placement.x = ReadValue<float>(transform, 0x1E0);
    placement.y = ReadValue<float>(transform, 0x1E4);
    placement.z = ReadValue<float>(transform, 0x1E8);
    if (!std::isfinite(placement.x) || !std::isfinite(placement.y) || !std::isfinite(placement.z) ||
        !std::isfinite(placement.heading)) {
        result = "non_finite_transform";
        return false;
    }
    result = "captured";
    return true;
}

} // namespace

namespace Coop {

void RefreshSeamlessLocalPlacement() {
    SummonPlacementDescriptor placement{};
    std::string_view result;
    if (!ReadLocalSummonPlacement(placement, result)) {
        return;
    }
    std::scoped_lock lock{seamless_placement_mutex};
    seamless_host_placement = placement;
}

void ApplyCrossMapSummonHostPlacement(const GuestRegisterSnapshot& registers) {
    host_placement_rewrite = {};
    host_placement_rewrite.result = "invalid_prepared";
    host_placement_rewrite.prepared = registers.rsi;
    {
        std::scoped_lock lock{seamless_placement_mutex};
        seamless_host_placement.reset();
    }

    const u64 prepared = registers.rsi;
    if (prepared < 0x10000 ||
        !HasMemoryAccess(prepared, 0x04 + sizeof(SummonPlacementDescriptor), MemoryProt::CpuRead)) {
        return;
    }
    std::memcpy(&host_placement_rewrite.candidate, reinterpret_cast<const void*>(prepared + 0x04),
                sizeof(SummonPlacementDescriptor));

    auto& host = host_placement_rewrite.host;
    if (!IsUsablePackedMap(host_placement_rewrite.candidate.packed_region)) {
        host_placement_rewrite.result = "invalid_map";
        return;
    }
    if (!ReadLocalSummonPlacement(host, host_placement_rewrite.result)) {
        return;
    }
    if (host_placement_rewrite.candidate.packed_region == host.packed_region) {
        host_placement_rewrite.result = "same_map";
        return;
    }

    if (!WriteValue(prepared, 0x04, host)) {
        host_placement_rewrite.result = "write_rejected";
        return;
    }
    {
        std::scoped_lock lock{seamless_placement_mutex};
        seamless_host_placement = host;
    }
    host_placement_rewrite.result = "applied";
}

void ApplyCrossMapSummonGuestPlacement(const GuestRegisterSnapshot& registers) {
    guest_placement_handoff = {};
    guest_placement_handoff.result = "invalid_object";
    guest_placement_handoff.object = registers.rbx;

    const u64 object = registers.rbx;
    if (object < 0x10000 || !HasMemoryAccess(object, 0xEC, MemoryProt::CpuRead)) {
        return;
    }
    if (ReadValue<u32>(object, 0xE8) != 0) {
        guest_placement_handoff.result = "not_summoned_client";
        return;
    }

    auto& record = guest_placement_handoff;
    record.result = "invalid_state";
    record.state = GetGlobalState();
    const u64 state = record.state;
    if (state < 0x10000 || !HasMemoryAccess(state, 0x1649, MemoryProt::CpuRead)) {
        return;
    }
    if (ReadValue<u8>(state, 0x1648) == 0) {
        record.result = "received_placement_not_ready";
        return;
    }

    std::optional<SummonPlacementDescriptor> transported_host;
    {
        std::scoped_lock lock{seamless_placement_mutex};
        transported_host = seamless_received_host_placement;
    }
    record.transported_host = transported_host.has_value();

    record.current_map = GetCurrentPackedMap(); // raw: a summon hook, in the join's load
    record.received_map = transported_host.has_value() ? transported_host->packed_region
                                                       : ReadValue<u32>(state, 0x14C4);
    const u32 transport_map = transported_host.has_value() ? transported_host->packed_region
                                                           : ReadValue<u32>(state, 0x1644);
    if (!IsUsablePackedMap(record.current_map) || !IsUsablePackedMap(record.received_map)) {
        record.result = "invalid_map";
        return;
    }
    if (record.received_map != transport_map) {
        record.result = "placement_copy_pending";
        return;
    }
    if (record.current_map == record.received_map) {
        // A party guest joining in place: the summon reload uses the placement it was handed
        // (the host's spot); its own goes there instead.
        if (IsPartyGuestRole() && transported_host.has_value() &&
            HasMemoryAccess(state, 0x14F0, MemoryProt::CpuWrite) &&
            WriteTransportedSummonPlacement(state, *transported_host)) {
            record.placement_refreshed = true;
        }
        record.result = "same_map";
        return;
    }
    if (!HasMemoryAccess(state, 0x1521, MemoryProt::CpuWrite)) {
        record.result = "state_not_writable";
        return;
    }
    if (transported_host.has_value()) {
        if (!WriteTransportedSummonPlacement(state, *transported_host)) {
            record.result = "transport_write_rejected";
            return;
        }
        record.placement_refreshed = true;
    }
    if (ReadValue<u8>(state, 0x08) != 0) {
        record.result = "stage_transition_pending";
        return;
    }
    if (ReadValue<u8>(state, 0x1520) != 0) {
        record.result = "native_forced_warp_pending";
        return;
    }

    for (size_t index = 0; index < 4; ++index) {
        const float position = ReadValue<float>(state, 0x14D0 + index * sizeof(float));
        const float orientation = ReadValue<float>(state, 0x14E0 + index * sizeof(float));
        if (!std::isfinite(position) || !std::isfinite(orientation)) {
            record.result = "non_finite_placement";
            return;
        }
    }

    const u64 image_size = MemoryPatcher::g_eboot_image_size;
    if (image_base == 0 || std::ranges::any_of(CrossMapNativeCalls, [image_size](const auto& site) {
            return site.offset >= image_size;
        })) {
        record.result = "native_call_out_of_range";
        return;
    }

    using PlacementSetter = void PS4_SYSV_ABI (*)(const void* value);
    using FlagSetter = void PS4_SYSV_ABI (*)();
    using PlacementSelector = bool PS4_SYSV_ABI (*)(u64 context, s32 warp_info_id);
    using StageTransition = s32 PS4_SYSV_ABI (*)();
    const auto set_map = reinterpret_cast<PlacementSetter>(image_base + SetForcedSummonMapOffset);
    const auto set_position =
        reinterpret_cast<PlacementSetter>(image_base + SetForcedSummonPositionOffset);
    const auto set_orientation =
        reinterpret_cast<PlacementSetter>(image_base + SetForcedSummonOrientationOffset);
    const auto set_warp = reinterpret_cast<FlagSetter>(image_base + SetForcedSummonWarpOffset);
    const auto select =
        reinterpret_cast<PlacementSelector>(image_base + SelectSummonedPlacementOffset);
    const auto transition = reinterpret_cast<StageTransition>(image_base + StageTransitionOffset);

    set_position(reinterpret_cast<const void*>(state + 0x14D0));
    set_orientation(reinterpret_cast<const void*>(state + 0x14E0));
    set_map(reinterpret_cast<const void*>(state + 0x14C4));
    set_warp();
    record.select_result = select(0, -1);
    record.selected_map = ReadValue<u32>(state, 0x0C);
    if (!record.select_result || record.selected_map != record.received_map) {
        record.result = "native_select_failed";
        return;
    }

    pending_summon_reload_map.store(record.received_map, std::memory_order_release);
    pending_summon_reload_state_map.store(record.received_map, std::memory_order_release);
    record.summon_reload_armed = true;
    record.transition_result = transition();
    if (record.transition_result == 0) {
        pending_summon_reload_map.store(0, std::memory_order_release);
        pending_summon_reload_state_map.store(0, std::memory_order_release);
        record.summon_reload_armed = false;
        record.result = "stage_transition_failed";
        return;
    }
    record.result = "applied";
}

void ApplySummonReloadStateAtDescriptor(const GuestRegisterSnapshot& registers) {
    summon_reload_state = {};
    auto& record = summon_reload_state;
    record.target_map = pending_summon_reload_state_map.load(std::memory_order_acquire);
    if (!IsUsablePackedMap(record.target_map)) {
        return;
    }

    const u64 descriptor = registers.r9;
    if (descriptor < 0x10000 || !HasMemoryAccess(descriptor, 0x0C, MemoryProt::CpuRead)) {
        record.result = "invalid_descriptor";
        return;
    }
    record.descriptor_map = ReadValue<u32>(descriptor, 0x08);
    if (record.descriptor_map != record.target_map) {
        record.result = "waiting_for_target_descriptor";
        return;
    }

    u32 expected_map = record.target_map;
    if (!pending_summon_reload_state_map.compare_exchange_strong(
            expected_map, 0, std::memory_order_acq_rel, std::memory_order_acquire)) {
        record.result = "already_dispatched";
        return;
    }

    record.reload_state = ReadValue<u64>(image_base + SummonSessionRulesPointerOffset, 0);
    if (record.reload_state < 0x10000 ||
        !HasMemoryAccess(record.reload_state, 0x88, MemoryProt::CpuWrite)) {
        pending_summon_reload_state_map.store(record.target_map, std::memory_order_release);
        record.result = "invalid_reload_state";
        return;
    }

    record.state_before = ReadValue<s32>(record.reload_state, 0x84);
    u32 pending_reload_map = record.target_map;
    const bool cleared_deferred_reload = pending_summon_reload_map.compare_exchange_strong(
        pending_reload_map, 0, std::memory_order_acq_rel, std::memory_order_acquire);

    using SummonedMapReload = void PS4_SYSV_ABI (*)();
    const auto reload_summoned_map =
        reinterpret_cast<SummonedMapReload>(image_base + SummonedMapReloadOffset);
    reload_summoned_map();
    record.summon_reload_started = true;
    record.state_after = ReadValue<s32>(record.reload_state, 0x84);
    if (record.state_after != 3) {
        using SummonReloadStateSetter = void PS4_SYSV_ABI (*)(u64 state);
        const auto set_reload_state =
            reinterpret_cast<SummonReloadStateSetter>(image_base + SetSummonReloadStateOffset);
        set_reload_state(record.reload_state);
        record.state_setter_used = true;
        record.state_after = ReadValue<s32>(record.reload_state, 0x84);
    }
    if (record.state_after != 3) {
        pending_summon_reload_state_map.store(record.target_map, std::memory_order_release);
        if (cleared_deferred_reload) {
            pending_summon_reload_map.store(record.target_map, std::memory_order_release);
        }
        record.result = "state_transition_failed";
        return;
    }

    std::optional<SummonPlacementDescriptor> transported_host;
    {
        std::scoped_lock lock{seamless_placement_mutex};
        transported_host = seamless_received_host_placement;
    }
    const u64 placement_state = GetGlobalState();
    if (transported_host.has_value() && transported_host->packed_region == record.target_map &&
        placement_state >= 0x10000 &&
        WriteTransportedSummonPlacement(placement_state, *transported_host)) {
        using PlacementSetter = void PS4_SYSV_ABI (*)(const void* value);
        const auto set_map =
            reinterpret_cast<PlacementSetter>(image_base + SetForcedSummonMapOffset);
        const auto set_position =
            reinterpret_cast<PlacementSetter>(image_base + SetForcedSummonPositionOffset);
        const auto set_orientation =
            reinterpret_cast<PlacementSetter>(image_base + SetForcedSummonOrientationOffset);
        set_position(reinterpret_cast<const void*>(placement_state + 0x14D0));
        set_orientation(reinterpret_cast<const void*>(placement_state + 0x14E0));
        set_map(reinterpret_cast<const void*>(placement_state + 0x14C4));
        record.placement_refreshed = true;
    }
    record.result = "applied";
}

void ApplyDeferredSummonReload() {
    deferred_summon_reload = {};
    auto& record = deferred_summon_reload;
    record.target_map = pending_summon_reload_map.load(std::memory_order_acquire);
    if (!IsUsablePackedMap(record.target_map)) {
        return;
    }

    record.current_map = GetCurrentPackedMap(); // raw: runs before the tick, during the load
    if (record.current_map != record.target_map) {
        record.result = "waiting_for_target_map";
        return;
    }

    const u64 state = GetGlobalState();
    if (state < 0x10000 || !HasMemoryAccess(state, 0x1521, MemoryProt::CpuRead)) {
        record.result = "invalid_state";
        return;
    }
    if (ReadValue<u8>(state, 0x08) != 0 || ReadValue<u8>(state, 0x1520) != 0) {
        record.result = "waiting_for_transition";
        return;
    }

    u32 expected_map = record.target_map;
    if (!pending_summon_reload_map.compare_exchange_strong(
            expected_map, 0, std::memory_order_acq_rel, std::memory_order_acquire)) {
        record.result = "already_dispatched";
        return;
    }
    pending_summon_reload_state_map.store(0, std::memory_order_release);

    using SummonedMapReload = void PS4_SYSV_ABI (*)();
    const auto reload_summoned_map =
        reinterpret_cast<SummonedMapReload>(image_base + SummonedMapReloadOffset);
    reload_summoned_map();
    record.summon_reload_started = true;
    record.result = "applied";
}

void ApplyPreMatchCrossMapGuestWarp() {
    pre_match_guest_warp = {};
    auto& record = pre_match_guest_warp;

    const u64 matching = GetMatchingState();
    const bool matching_readable =
        matching >= 0x10000 && HasMemoryAccess(matching, 0x128, MemoryProt::CpuRead);
    const s32 matching_state = matching_readable ? ReadValue<s32>(matching, 0x124) : -1;
    const u64 global = GetGlobalState();
    const bool session_over =
        matching_readable && matching_state == 0 && ReadValue<u64>(matching, 0x18) == 0 &&
        global >= 0x10000 && HasMemoryAccess(global, 0x1521, MemoryProt::CpuRead) &&
        ReadValue<u8>(global, 0x08) == 0 && ReadValue<u8>(global, 0x1520) == 0 &&
        pending_summon_reload_map.load(std::memory_order_acquire) == 0 &&
        pending_summon_reload_state_map.load(std::memory_order_acquire) == 0;
    std::optional<SummonPlacementDescriptor> transported_host;
    bool placement_consumed = false;
    {
        std::scoped_lock lock{seamless_placement_mutex};
        if (seamless_received_host_placement.has_value() &&
            seamless_received_host_placement_life.Spent(matching_state, session_over)) {
            LOG_INFO(Debug, "Bloodborne party: the join's placement on map {:#x} is spent",
                     seamless_received_host_placement->packed_region);
            seamless_received_host_placement.reset();
            seamless_received_host_placement_consumed = false;
        }
        transported_host = seamless_received_host_placement;
        placement_consumed = seamless_received_host_placement_consumed;
    }
    if (!transported_host.has_value()) {
        record.result = "no_placement";
        return;
    }
    if (placement_consumed) {
        record.result = "placement_consumed";
        return;
    }

    record.current_map = GetCurrentPackedMap(); // raw: runs before the tick, during the load
    record.target_map = transported_host->packed_region;
    if (!IsUsablePackedMap(record.current_map) || !IsUsablePackedMap(record.target_map)) {
        record.result = "invalid_map";
        return;
    }
    if (record.current_map == record.target_map) {
        record.result = "destination_reached";
        return;
    }

    const u64 multi_play = ReadValue<u64>(image_base + MatchingStatePointerOffset, 0);
    record.multi_play_state = multi_play >= 0x10000 ? ReadValue<s32>(multi_play, 0x124) : -1;
    if (multi_play >= 0x10000 &&
        (record.multi_play_state != 0 || ReadValue<u64>(multi_play, 0x18) != 0)) {
        record.result = "matching_already_active";
        return;
    }

    record.state = GetGlobalState();
    const u64 state = record.state;
    if (state < 0x10000 || !HasMemoryAccess(state, 0x1521, MemoryProt::CpuWrite)) {
        record.result = "invalid_state";
        return;
    }
    if (ReadValue<u8>(state, 0x08) != 0) {
        record.result = "stage_transition_pending";
        return;
    }
    if (ReadValue<u8>(state, 0x1520) != 0) {
        record.result = "native_forced_warp_pending";
        return;
    }
    if (pending_summon_reload_map.load(std::memory_order_acquire) != 0 ||
        pending_summon_reload_state_map.load(std::memory_order_acquire) != 0) {
        record.result = "summon_reload_pending";
        return;
    }
    if (!WriteTransportedSummonPlacement(state, *transported_host)) {
        record.result = "transport_write_rejected";
        return;
    }
    record.placement_written = true;

    using PlacementSetter = void PS4_SYSV_ABI (*)(const void* value);
    using FlagSetter = void PS4_SYSV_ABI (*)();
    using PlacementSelector = bool PS4_SYSV_ABI (*)(u64 context, s32 warp_info_id);
    using StageTransition = s32 PS4_SYSV_ABI (*)();
    const auto set_map = reinterpret_cast<PlacementSetter>(image_base + SetForcedSummonMapOffset);
    const auto set_position =
        reinterpret_cast<PlacementSetter>(image_base + SetForcedSummonPositionOffset);
    const auto set_orientation =
        reinterpret_cast<PlacementSetter>(image_base + SetForcedSummonOrientationOffset);
    const auto set_warp = reinterpret_cast<FlagSetter>(image_base + SetForcedSummonWarpOffset);
    const auto select =
        reinterpret_cast<PlacementSelector>(image_base + SelectSummonedPlacementOffset);
    const auto transition = reinterpret_cast<StageTransition>(image_base + StageTransitionOffset);

    set_position(reinterpret_cast<const void*>(state + 0x14D0));
    set_orientation(reinterpret_cast<const void*>(state + 0x14E0));
    set_map(reinterpret_cast<const void*>(state + 0x14C4));
    set_warp();
    record.select_result = select(0, -1);
    record.selected_map = ReadValue<u32>(state, 0x0C);
    if (!record.select_result || record.selected_map != record.target_map) {
        record.result = "native_select_failed";
        return;
    }

    record.transition_result = transition();
    if (record.transition_result == 0) {
        record.result = "stage_transition_failed";
        return;
    }
    {
        std::scoped_lock lock{seamless_placement_mutex};
        if (seamless_received_host_placement.has_value() &&
            IsSameSummonPlacement(*seamless_received_host_placement, *transported_host)) {
            seamless_received_host_placement_consumed = true;
        }
    }
    pending_responder_resume = {
        .target_map = record.target_map,
        .target_area = transported_host->area,
    };
    responder_resume = {};
    responder_resume.result = "armed";
    responder_resume.target_map = record.target_map;
    responder_resume.target_area = transported_host->area;
    record.responder_resume_armed = true;
    record.result = "applied";
}

// See ArrivalNeedsMove: the pre-match warp's calls with the lamp's own point as the placement, so
// the stage loads around the lamp. Returns whether the trip started.
bool WarpGuestToLampPoint(const LampPoint& point) {
    const u64 state = GetGlobalState();
    const u64 image_size = MemoryPatcher::g_eboot_image_size;
    if (state < 0x10000 || !HasMemoryAccess(state, 0x1530, MemoryProt::CpuWrite) ||
        ReadValue<u8>(state, 0x08) != 0 || image_base == 0 ||
        std::ranges::any_of(
            CrossMapNativeCalls,
            [image_size](const auto& site) { return site.offset >= image_size; })) {
        return false;
    }
    const SummonPlacementDescriptor placement{
        .packed_region = point.packed_map,
        .x = point.x,
        .y = point.y,
        .z = point.z,
        .heading = point.heading,
    };
    if (!WriteTransportedSummonPlacement(state, placement)) {
        return false;
    }
    using PlacementSetter = void PS4_SYSV_ABI (*)(const void* value);
    using FlagSetter = void PS4_SYSV_ABI (*)();
    using PlacementSelector = bool PS4_SYSV_ABI (*)(u64 context, s32 warp_info_id);
    using StageTransition = s32 PS4_SYSV_ABI (*)();
    reinterpret_cast<PlacementSetter>(image_base + SetForcedSummonPositionOffset)(
        reinterpret_cast<const void*>(state + 0x14D0));
    reinterpret_cast<PlacementSetter>(image_base + SetForcedSummonOrientationOffset)(
        reinterpret_cast<const void*>(state + 0x14E0));
    reinterpret_cast<PlacementSetter>(image_base + SetForcedSummonMapOffset)(
        reinterpret_cast<const void*>(state + 0x14C4));
    reinterpret_cast<FlagSetter>(image_base + SetForcedSummonWarpOffset)();
    const bool selected =
        reinterpret_cast<PlacementSelector>(image_base + SelectSummonedPlacementOffset)(0, -1);
    if (!selected || ReadValue<u32>(state, 0x0C) != point.packed_map) {
        WriteValue(state, 0x1520, u8{0}); // the lamp lookup's trip instead
        return false;
    }
    return reinterpret_cast<StageTransition>(image_base + StageTransitionOffset)() != 0;
}

const LampPoint* StartTripToLamp(s32 warp_point) {
    // As the events do with SetPlayerRespawnPoint: the lamp a death respawns at. The summon warp
    // byte (+0x1520, SetSosSignWarp) makes the placement selector use the summon placement
    // (0x01332BC0): clear for the lamp lookup's trip, set by WarpGuestToLampPoint.
    if (const u64 global = GetGlobalState(); global >= 0x10000) {
        LOG_INFO(Debug, "Bloodborne party guest trip: summon warp byte was {}",
                 ReadValue<u8>(global, 0x1520));
        WriteValue(global, 0x1528, warp_point);
        WriteValue(global, 0x1520, u8{0});
    }
    const LampPoint* lamp = FindLampPoint(warp_point);
    if (lamp == nullptr || !WarpGuestToLampPoint(*lamp)) {
        lamp = nullptr;
        using WarpNextStageBonfire = void PS4_SYSV_ABI (*)(u64 unused, s32 warp_point);
        reinterpret_cast<WarpNextStageBonfire>(image_base + WarpNextStageBonfireOffset)(0,
                                                                                        warp_point);
    }
    LOG_INFO(Debug, "Bloodborne party guest warps to {} ({:#x}) placed at the lamp={}", warp_point,
             PackedMapOfWarpPoint(warp_point), lamp != nullptr);
    return lamp;
}

// Starts the bell's summon search directly (see StartSummonSearch), only through the player's own
// add-effect virtual, and only if it is the 01.09 function we verified at startup.
bool StartPlayerSummonSearch(u64 player, s32 effect_id) {
    if (!HasVirtual(player, CharacterAddSpEffectSlot, AddSpEffectOffset,
                    "StartPlayerSummonSearch AddSpEffect")) {
        LOG_WARNING(Debug,
                    "Bloodborne summon search skipped: player {:#x} has no 01.09 "
                    "add-effect virtual",
                    player);
        return false;
    }
    return StartSummonSearch(player, effect_id,
                             reinterpret_cast<AddSpEffectFn>(image_base + AddSpEffectOffset));
}

void ResumePreMatchCrossMapResponder() {
    auto& pending = pending_responder_resume;
    if (!IsUsablePackedMap(pending.target_map)) {
        return;
    }

    responder_resume = {};
    auto& record = responder_resume;
    record.target_map = pending.target_map;
    record.target_area = pending.target_area;
    record.current_map = GetCurrentPackedMap(); // raw: runs before the tick, during the load
    if (record.current_map != record.target_map) {
        pending.ready_observations = 0;
        record.result = "waiting_for_target_map";
        return;
    }

    SummonPlacementDescriptor local{};
    std::string_view placement_result;
    if (!ReadLocalSummonPlacement(local, placement_result)) {
        pending.ready_observations = 0;
        record.result = placement_result;
        return;
    }
    record.current_area = local.area;
    if (record.current_area != record.target_area) {
        pending.ready_observations = 0;
        record.result = "waiting_for_target_area";
        return;
    }

    record.state = GetGlobalState();
    if (record.state < 0x10000 || !HasMemoryAccess(record.state, 0x1521, MemoryProt::CpuRead)) {
        pending.ready_observations = 0;
        record.result = "invalid_state";
        return;
    }
    if (ReadValue<u8>(record.state, 0x08) != 0 || ReadValue<u8>(record.state, 0x1520) != 0) {
        pending.ready_observations = 0;
        record.result = "waiting_for_stage_transition";
        return;
    }

    record.multi_play = GetMatchingState();
    if (record.multi_play < 0x10000 ||
        !HasMemoryAccess(record.multi_play, 0x128, MemoryProt::CpuRead)) {
        pending.ready_observations = 0;
        record.result = "invalid_matching_state";
        return;
    }
    record.multi_play_state = ReadValue<s32>(record.multi_play, 0x124);
    record.matching_controller = ReadValue<u64>(record.multi_play, 0x18);
    if (record.multi_play_state != 0 || record.matching_controller != 0) {
        pending.ready_observations = 0;
        record.result = "waiting_for_matching_idle";
        return;
    }

    record.player = GetLocalPlayer();
    if (record.player < 0x10000 ||
        !HasMemoryAccess(record.player, sizeof(u64), MemoryProt::CpuRead)) {
        pending.ready_observations = 0;
        record.result = "invalid_player";
        return;
    }
    record.effect_active = HasPlayerEffect(record.player, SmallResonantBellEffect);
    if (record.effect_active) {
        record.ready_observations = pending.ready_observations;
        record.attempts = pending.attempts;
        record.result = "completed";
        LOG_INFO(Debug,
                 "Bloodborne seamless responder resumed at map={:#x} area={} after {} native "
                 "attempt(s)",
                 record.target_map, record.target_area, record.attempts);
        pending = {};
        return;
    }

    if (pending.dispatched) {
        record.ready_observations = pending.ready_observations;
        record.attempts = pending.attempts;
        if (std::chrono::steady_clock::now() - pending.dispatched_at < ResponderResumeRetryDelay) {
            record.result = "waiting_for_effect";
            return;
        }
        if (pending.attempts >= ResponderResumeMaxAttempts) {
            record.result = "effect_timeout";
            return;
        }
        pending.dispatched = false;
        pending.ready_observations = 0;
    }

    ++pending.ready_observations;
    record.ready_observations = pending.ready_observations;
    record.attempts = pending.attempts;
    if (pending.ready_observations < ResponderResumeStableObservations) {
        record.result = "waiting_for_destination_stability";
        return;
    }

    record.native_result = StartPlayerSummonSearch(record.player, SmallResonantBellEffect);
    ++pending.attempts;
    pending.dispatched = true;
    pending.dispatched_at = std::chrono::steady_clock::now();
    record.attempts = pending.attempts;
    record.result = record.native_result ? "native_dispatched" : "native_rejected";
    LOG_INFO(Debug,
             "Bloodborne seamless responder search dispatch map={:#x} area={} attempt={} "
             "result={}",
             record.target_map, record.target_area, record.attempts, record.native_result);
}

void LogJoinTaskChanges() {
    static std::string last;
    std::string tasks;
    ForEachJoinTask([&](u64 task) {
        tasks += fmt::format("[kind={} step={} next={} flags={:#x}] ", ReadValue<s32>(task, 0xE8),
                             ReadValue<s32>(task, 0x50), ReadValue<s32>(task, 0x54),
                             ReadValue<u8>(task, 0xED));
    });
    if (tasks != last) {
        last = tasks;
        LOG_INFO(Debug, "Bloodborne join tasks: {}", tasks.empty() ? "none" : tasks);
    }
}

// See ReopensClosedSession: a crashed friend was kicked on every rejoin (Kaniel 2026-10-09).
void ReopenClosedPartySession() {
    const u64 session = ReadValue<u64>(image_base + SessionManagerPointerOffset, 0);
    if (session < 0x10000 || !HasMemoryAccess(session, 0x128, MemoryProt::CpuRead)) {
        return;
    }
    const u8 closing = ReadValue<u8>(session, SessionClosingOffset);
    if (!ReopensClosedSession(party_controller.has_value(), ReadValue<s32>(session, 0x124),
                              closing)) {
        return;
    }
    WriteValue(session, SessionClosingOffset, u8{0});
    LOG_INFO(Debug, "Bloodborne party: reopened the session the game had closed to joiners");
}

} // namespace Coop

namespace {

std::atomic<bool> summon_delivered{false};
// When the game last posted a summon search (steady clock ticks), 0 for never.
std::atomic<std::chrono::steady_clock::rep> last_summon_search{0};

} // namespace

void NoteSummonDelivered() {
    summon_delivered.store(true, std::memory_order_release);
}

void NoteSummonSearchPosted() {
    last_summon_search.store(std::chrono::steady_clock::now().time_since_epoch().count(),
                             std::memory_order_relaxed);
}

namespace Coop {

std::optional<std::chrono::steady_clock::time_point> LastSummonSearchPost() {
    const auto ticks = last_summon_search.load(std::memory_order_relaxed);
    if (ticks == 0) {
        return std::nullopt;
    }
    return std::chrono::steady_clock::time_point{std::chrono::steady_clock::duration{ticks}};
}

} // namespace Coop

namespace Coop {

// Characters near the local player that fight on while the party is told nothing of them
// (Kaniel's chalice playtest 2026-10-19: after a wipe the host fought red skeletons whose slots it
// posted as switched off). Every character set WorldChrMan lists (+0x278 count, +0x280 entries
// of 0x18 bytes: the set, its index), each set +0x00 count / +0x08 entries of 0x38 bytes: the
// living ones within 20 m that are switched off, hidden or outside the maps' sets (the players'
// and the second set, see 0x01914A40), logged when the list changes.
void LogUnsharedNearbyCharacters() {
    static s64 checked_ms = 0;
    if (const s64 now_ms = SteadyMilliseconds(); now_ms - checked_ms < 1000) {
        return;
    } else {
        checked_ms = now_ms;
    }
    const u64 world = ReadValue<u64>(image_base + CandidateLocalStatePointerOffset, 0);
    const auto me = LocalPlayerPosition();
    if (world < 0x10000 || !me) {
        return;
    }
    std::unordered_set<u64> players{GetLocalPlayer()};
    for (const OtherPlayer& other : OtherPlayers()) {
        players.insert(other.character);
    }
    const s32 map_sets = ReadValue<s32>(world, 0x20);
    const s32 sets = std::min(ReadValue<s32>(world, 0x278), 64);
    std::string now;
    for (s32 index = 0; index < sets; ++index) {
        const u64 entry = world + 0x280 + static_cast<u64>(index) * 0x18;
        const u64 set = ReadValue<u64>(entry, 0);
        const s32 set_index = ReadValue<s32>(entry, 8);
        if (set < 0x10000 || !HasMemoryAccess(set, 0x10, MemoryProt::CpuRead)) {
            continue;
        }
        const s32 count = std::min(ReadValue<s32>(set, 0), 1024);
        const u64 characters = ReadValue<u64>(set, 8);
        for (s32 slot = 0; characters >= 0x10000 && slot < count; ++slot) {
            const u64 character = ReadValue<u64>(characters + static_cast<u64>(slot) * 0x38, 0);
            if (character < 0x10000 || players.contains(character) ||
                !HasMemoryAccess(character, 0x3B8, MemoryProt::CpuRead)) {
                continue;
            }
            const auto at = CharacterPosition(character);
            const s32 hp = CharacterHp(character);
            if (!at || hp <= 0) {
                continue;
            }
            const float dx = (*at)[0] - (*me)[0], dy = (*at)[1] - (*me)[1],
                        dz = (*at)[2] - (*me)[2];
            if (dx * dx + dy * dy + dz * dz > 20.0f * 20.0f) {
                continue;
            }
            const bool off = IsCharacterDisabled(character);
            const u8 hidden = ReadValue<u8>(character, 0x28);
            const bool map_set = set_index < map_sets;
            if (!off && hidden == 0 && map_set) {
                continue; // an ordinary enemy, posted as it is
            }
            now += fmt::format(" [{:#x} set {} type {} hp {}{}{} flags {:#x}]",
                               ReadValue<u32>(character, 0x08), set_index,
                               ReadValue<s32>(character, 0x78), hp, off ? " off" : "",
                               hidden != 0 ? " hidden" : "", ReadValue<u32>(character, 0x1E0));
        }
    }
    static std::string last;
    static u32 logged = 0;
    if (now != last && ++logged <= 300) {
        LOG_INFO(Debug,
                 "Bloodborne party: living characters near the player the party may not "
                 "see:{}",
                 now.empty() ? " none" : now);
        std::string traced = now.empty() ? std::string{"none"} : now;
        std::ranges::replace(traced, ' ', '_');
        Trace("unshared", {{"near", std::move(traced)}});
    }
    last = std::move(now);
}

// See SummonJoinWatch: the guest's state when a summon has not become a join.
void WatchSummonJoin() {
    static SummonJoinWatch watch;
    const auto now = SummonJoinWatch::Clock::now();
    if (summon_delivered.exchange(false, std::memory_order_acq_rel)) {
        watch.Delivered(now);
    }
    bool joining = SessionPlayers() > 1;
    ForEachJoinTask([&](u64) { joining = true; });
    if (!watch.Stalled(joining, now)) {
        return;
    }
    const u64 player = GetLocalPlayer();
    const u64 multi_play = GetMatchingState();
    const u64 session = ReadValue<u64>(image_base + SessionManagerPointerOffset, 0);
    const bool session_readable =
        session >= 0x10000 && HasMemoryAccess(session, 0x128, MemoryProt::CpuRead);
    std::string effects;
    for (const s32 effect : EffectsOf(player)) {
        effects += fmt::format(" {}", effect);
    }
    LOG_WARNING(Debug,
                "Bloodborne party: a summon came {} s ago and the game has not joined: map {:#x} "
                "matching state {} session state {} closing {} players {} player flags {:#x} "
                "effects{}",
                std::chrono::duration_cast<std::chrono::seconds>(SummonJoinWatch::Patience).count(),
                CurrentPlace().map,
                multi_play >= 0x10000 && HasMemoryAccess(multi_play, 0x128, MemoryProt::CpuRead)
                    ? ReadValue<s32>(multi_play, 0x124)
                    : -1,
                session_readable ? ReadValue<s32>(session, 0x124) : -1,
                session_readable ? ReadValue<u8>(session, SessionClosingOffset) : 0xFF,
                SessionPlayers(), player >= 0x10000 ? ReadValue<u64>(player, 0x1E0) : 0,
                effects.empty() ? std::string{" none"} : effects);
}

} // namespace Coop

namespace {

constexpr u64 TaskAllocatorPointerOffset = 0x05540438;
constexpr u64 JoinTaskConstructOffset = 0x01E4F9F0;
constexpr u64 JoinTaskListAddOffset = 0x01E55A70;

} // namespace

namespace Coop {

// The remote players' join state from before the trip: member id, slot (+0xEC) and flags (+0xED,
// bit 0 = the member's data is synced). The rebuilt task needs both to create the character.
// A member id (0x20 bytes) is a ref-counted identity pointer at +0 plus a lock at +8, so only the
// pointer is kept: copying the lock and comparing through it hung the game.
std::vector<SavedJoinTask> saved_join_tasks;

// Merged at each trip's start (MergeSavedJoinTasks): a trip from a map a member was not on (the
// Dream) keeps that member's slot from before.
// The join list now, as saved tasks (remote players and a guest's own task).
static std::vector<SavedJoinTask> CurrentJoinTasks() {
    std::vector<SavedJoinTask> found;
    ForEachJoinTask([&](u64 task) {
        const s32 kind = ReadValue<s32>(task, 0xE8);
        if (kind != RemoteHostTask && kind != RemoteGuestTask && kind != LocalTaskKind(false)) {
            return;
        }
        SavedJoinTask saved{};
        saved.kind = kind;
        saved.identity = ReadValue<u64>(task, 0xC8);
        saved.slot = ReadValue<u8>(task, 0xEC);
        saved.flags = ReadValue<u8>(task, 0xED);
        saved.step = ReadValue<s32>(task, 0x50);
        found.push_back(saved);
    });
    return found;
}

// Diagnostic (Kaniel 2026-10-15: between two guests, one's actions played on the other's own
// character): the join list (kind, slot, flags, step) whenever it changes.
void LogJoinTasksOnChange() {
    std::string which;
    for (const SavedJoinTask& task : CurrentJoinTasks()) {
        which += fmt::format(" [kind={} slot={} flags={:#x} step={} identity={:#x}]", task.kind,
                             task.slot, task.flags, task.step, task.identity);
    }
    static std::string last;
    if (which != last) {
        last = which;
        LOG_INFO(Debug, "Bloodborne join tasks:{}", which.empty() ? " none" : which);
    }
}

void SaveRemoteJoinTasks() {
    const std::vector<SavedJoinTask> found = CurrentJoinTasks();
    saved_join_tasks = MergeSavedJoinTasks(saved_join_tasks, found);
    std::string which;
    for (const SavedJoinTask& saved : saved_join_tasks) {
        which += fmt::format(" [kind={} slot={} flags={:#x} step={}]", saved.kind, saved.slot,
                             saved.flags, saved.step);
    }
    LOG_INFO(Debug, "Bloodborne party trip: {} remote join tasks saved ({} found):{}",
             saved_join_tasks.size(), found.size(), which);
}

// Members rebuilt while their data had not come yet (see RebuildsFromSave): their tasks wait at
// their saved step until the member stands in the world (the vitals board), then ask it for its
// data (MemberDataWait, FinishWaitingJoins). A guest sends its data once, after its load: one that
// finished loading while the host was away had sent it to a world that is gone (rig hp-together
// 2026-10-10: host and guest never saw each other).
std::map<u64, MemberDataWait> joins_waiting_for_data; // by member identity pointer

static void SetJoinTaskStep(u64 task, s32 step) {
    using SetStep = void PS4_SYSV_ABI (*)(u64 task, s32 step);
    reinterpret_cast<SetStep>(ReadValue<u64>(ReadValue<u64>(task, 0), 0x28))(task, step);
}

// What the game does when a member joins (see above), into the step that creates the character,
// with the slot and synced-data flag saved at the trip's start.
static void RebuildJoinTask(u64 manager, u64 member_id, s32 kind, const SavedJoinTask* saved) {
    const u64 allocator = ReadValue<u64>(image_base + TaskAllocatorPointerOffset, 0);
    if (allocator < 0x10000) {
        return;
    }
    using Allocate = u64 PS4_SYSV_ABI (*)(u64 allocator, u64 size, u64 alignment);
    const u64 task = reinterpret_cast<Allocate>(ReadValue<u64>(ReadValue<u64>(allocator, 0), 0x58))(
        allocator, 0x100, 8);
    if (task < 0x10000) {
        return;
    }
    using Construct = void PS4_SYSV_ABI (*)(u64 task, u64 member_id, s32 kind);
    reinterpret_cast<Construct>(image_base + JoinTaskConstructOffset)(task, member_id, kind);
    // A member still joining when the trip began: back at its step, waiting for its data.
    const bool waiting = saved && !RebuildsFromSave(*saved);
    if (saved) {
        WriteValue(task, 0xEC, saved->slot);
    }
    WriteValue(task, 0xED, RebuiltTaskFlags(ReadValue<u8>(task, 0xED), saved, waiting));
    u64 holder = task;
    using ListAdd = void PS4_SYSV_ABI (*)(u64 list, u64 entry);
    reinterpret_cast<ListAdd>(image_base + JoinTaskListAddOffset)(manager + 8,
                                                                  reinterpret_cast<u64>(&holder));
    if (waiting) {
        SetJoinTaskStep(task, saved->step);
        joins_waiting_for_data[ReadValue<u64>(member_id, 0)] = MemberDataWait{};
    } else {
        SetJoinTaskStep(task, CreateCharacterStep);
    }
    LOG_INFO(
        Debug,
        "Bloodborne party trip: rebuilt the join task of a member kind={} task={:#x} "
        "slot known={} flags={:#x}{}",
        kind, task, saved != nullptr, ReadValue<u8>(task, 0xED),
        waiting ? fmt::format(" (it was still joining at step {}: waits for its data)", saved->step)
                : "");
}

void RecreateRemotePlayers(std::span<const u64> only, bool build_missing) {
    const u64 session = ReadValue<u64>(image_base + SessionManagerPointerOffset, 0);
    const u64 manager = ReadValue<u64>(image_base + MultiPlayManPointerOffset, 0);
    if (session < 0x10000 || manager < 0x10000 ||
        !HasMemoryAccess(session, 0x128, MemoryProt::CpuRead)) {
        return;
    }
    using IdEqual = bool PS4_SYSV_ABI (*)(u64 a, u64 b);
    const auto id_equal = reinterpret_cast<IdEqual>(image_base + MemberIdEqualOffset);
    const bool hosting = ReadValue<s32>(session, 0x124) == MatchingStateHosting;
    const u64 self = session + 0xD0;
    const u64 begin = ReadValue<u64>(session, 0xF8);
    const u64 end = ReadValue<u64>(session, 0x100);
    LOG_INFO(Debug, "Bloodborne party trip: {} session members, hosting={}",
             begin < end ? (end - begin) / 0x20 : 0, hosting);
    if (begin < 0x10000 || end <= begin || end - begin > 0x20 * 8 ||
        !HasMemoryAccess(begin, end - begin, MemoryProt::CpuRead)) {
        return;
    }
    const auto set_step = SetJoinTaskStep;
    const auto rebuild = [manager](u64 member_id, s32 kind, const SavedJoinTask* saved) {
        RebuildJoinTask(manager, member_id, kind, saved);
    };
    const auto task_for = [&](u64 member_id) {
        u64 found = 0;
        ForEachJoinTask([&](u64 task) {
            if (!found && id_equal(task + 0xC8, member_id)) {
                found = task;
            }
        });
        return found;
    };
    for (u64 member = begin; member < end; member += 0x20) {
        if (id_equal(member, self) ||
            (!only.empty() && std::ranges::find(only, member) == only.end())) {
            continue;
        }
        if (const u64 found = task_for(member)) {
            const s32 kind = ReadValue<s32>(found, 0xE8);
            const s32 step = ReadValue<s32>(found, 0x50);
            if (ShouldRecreateAfterTrip(kind, step)) {
                set_step(found, CreateCharacterStep);
                LOG_INFO(Debug, "Bloodborne party trip: re-creating remote player kind={}", kind);
            } else if (WaitsForMemberData(kind, step)) {
                joins_waiting_for_data.try_emplace(ReadValue<u64>(member, 0), MemberDataWait{});
                LOG_INFO(Debug,
                         "Bloodborne party: a remote player kind={} still joining at step {}: its "
                         "data is asked for",
                         kind, step);
            }
            continue;
        }
        if (!build_missing) {
            LOG_INFO(Debug, "Bloodborne party: a member with no join task yet is left to the "
                            "game's own join");
            continue;
        }
        const u64 identity = ReadValue<u64>(member, 0);
        // The live tasks too: a guest re-creating the others when a second guest joins has no
        // saved tasks yet, only the host's live one (see RebuiltTaskKind).
        const s32 kind = RebuiltTaskKind(hosting, saved_join_tasks, CurrentJoinTasks(), identity);
        // By identity, else the one remote player of that kind there was (two guests: no guess).
        const auto picked = PickSavedJoinTask(saved_join_tasks, kind, identity);
        rebuild(member, kind, picked ? &saved_join_tasks[*picked] : nullptr);
    }
    // On a guest its own task (kind 2) is the one that creates the host's character there.
    if (build_missing && !hosting && !task_for(self)) {
        const auto saved = std::ranges::find_if(saved_join_tasks, [](const SavedJoinTask& entry) {
            return entry.kind == LocalTaskKind(false);
        });
        rebuild(self, LocalTaskKind(false), saved != saved_join_tasks.end() ? &*saved : nullptr);
    }
}

// The host's map from its own map state post (with two guests the other one may be elsewhere).
std::optional<u32> HostMapFromItsPost() {
    const std::optional<MemberMapState> state = HostMapStateNow();
    return state ? std::optional<u32>{state->map} : std::nullopt;
}

// The session's members other than this one (SessionManager +0xF8..+0x100, 0x20 bytes each).
std::vector<u64> OtherSessionMembers() {
    std::vector<u64> members;
    const u64 session = ReadValue<u64>(image_base + SessionManagerPointerOffset, 0);
    if (session < 0x10000 || !HasMemoryAccess(session, 0x128, MemoryProt::CpuRead)) {
        return members;
    }
    using IdEqual = bool PS4_SYSV_ABI (*)(u64 a, u64 b);
    const auto id_equal = reinterpret_cast<IdEqual>(image_base + MemberIdEqualOffset);
    const u64 begin = ReadValue<u64>(session, 0xF8);
    const u64 end = ReadValue<u64>(session, 0x100);
    if (begin < 0x10000 || end <= begin || end - begin > 0x20 * 8 ||
        !HasMemoryAccess(begin, end - begin, MemoryProt::CpuRead)) {
        return members;
    }
    for (u64 member = begin; member < end; member += 0x20) {
        if (!id_equal(member, session + 0xD0)) {
            members.push_back(member);
        }
    }
    return members;
}

// Hosting, fewer than three in the session, and a party member online that is not in it.
bool HostHasRoomForMore() {
    const u64 session = ReadValue<u64>(image_base + SessionManagerPointerOffset, 0);
    if (session < 0x10000 || !HasMemoryAccess(session, 0x128, MemoryProt::CpuRead) ||
        ReadValue<s32>(session, 0x124) != MatchingStateHosting) {
        return false;
    }
    const u64 begin = ReadValue<u64>(session, 0xF8);
    const u64 end = ReadValue<u64>(session, 0x100);
    const size_t in_session = begin >= 0x10000 && end > begin ? (end - begin) / 0x20 : 1;
    std::scoped_lock lock{map_board_mutex};
    if (std::chrono::steady_clock::now() - map_board_time > std::chrono::seconds{3}) {
        return false;
    }
    return RoomForMore(true, in_session, map_board.size());
}

// The board name of a session member: its identity object (the member id's first pointer), or an
// object it points to, holds its online id (see FindOnlineIdIn).
std::optional<std::string> SessionMemberBoardName(u64 member,
                                                  const std::vector<std::string>& online_ids) {
    const auto scan = [&](u64 address, u64 size) -> std::optional<std::string> {
        if (address < 0x10000 || !HasMemoryAccess(address, size, MemoryProt::CpuRead)) {
            return std::nullopt;
        }
        return FindOnlineIdIn(std::span<const u8>{reinterpret_cast<const u8*>(address), size},
                              online_ids);
    };
    // The objects the identity points at, two levels down (a guest found the host's name only
    // deeper than a host found a guest's, rig 2026-10-09).
    const auto pointers = [&](u64 address, u64 size) {
        std::vector<u64> found;
        for (u64 offset = 0; address >= 0x10000 && offset < size; offset += 8) {
            if (!HasMemoryAccess(address + offset, 8, MemoryProt::CpuRead)) {
                break;
            }
            const u64 value = ReadValue<u64>(address, offset);
            if (value >= 0x10000 && HasMemoryAccess(value, 0x40, MemoryProt::CpuRead)) {
                found.push_back(value);
            }
        }
        return found;
    };
    const u64 identity = ReadValue<u64>(member, 0);
    std::optional<std::string> found = scan(identity, 0x200);
    for (const u64 first : found ? std::vector<u64>{} : pointers(identity, 0x100)) {
        if ((found = scan(first, 0x100))) {
            break;
        }
        for (const u64 second : pointers(first, 0x80)) {
            if ((found = scan(second, 0x80))) {
                break;
            }
        }
        if (found) {
            break;
        }
    }
    if (!found) {
        static std::unordered_set<u64> logged;
        if (logged.insert(identity).second && logged.size() < 16 && identity >= 0x10000 &&
            HasMemoryAccess(identity, 0x40, MemoryProt::CpuRead)) {
            LOG_INFO(Debug, "Bloodborne party: no online id in session member {:#x}, identity {}",
                     member,
                     BytesToHex(std::span<const u8>{reinterpret_cast<const u8*>(identity), 0x40}));
        }
        return std::nullopt;
    }
    return BoardMemberOfOnlineId(*found);
}

// The bell's search effect is lost with the old stage; put it back once the new one is up.
bool restore_search_pending{};

void RestoreSearchAfterTrip(bool in_session) {
    if (!restore_search_pending || !party_controller) {
        return;
    }
    restore_search_pending = false;
    const u64 player = GetLocalPlayer();
    if (player < 0x10000 || !in_session) {
        return;
    }
    for (const s32 effect : EffectsToRestoreAfterTrip(effects_before_trip, LocalEffects())) {
        const bool result = StartPlayerSummonSearch(player, effect);
        LOG_INFO(Debug, "Bloodborne party trip: put effect {} back result={}", effect, result);
    }
}

} // namespace Coop

namespace {

// Experiment: SHADPS4_BLOODBORNE_GUEST_AS_HOST=1 makes a party guest a live player (SetChrType 0,
// the event binding) for as long as it is in the session.
const bool guest_as_host = SettingIsOn(std::getenv("SHADPS4_BLOODBORNE_GUEST_AS_HOST"));

} // namespace

namespace Coop {

void PlayGuestAsHost(bool in_session) {
    const u64 player = GetLocalPlayer();
    if (player < 0x10000 || !HasMemoryAccess(player, 0x7C, MemoryProt::CpuRead) ||
        !ShouldPlayGuestAsHost(guest_as_host, in_session, ReadValue<s32>(player, 0x78))) {
        return;
    }
    reinterpret_cast<void PS4_SYSV_ABI (*)(u64, s32, s32)>(image_base + SetChrTypeOffset)(
        0, LocalPlayerEntity, 0);
    LOG_INFO(Debug, "Bloodborne guest-as-host experiment: the guest is a live player now");
}

void ApplyHealingFountainHostAvailability(const GuestRegisterSnapshot& registers) {
    healing_fountain_availability = {};
    auto& record = healing_fountain_availability;
    record.result = "invalid_object";
    record.object = registers.r13;
    if (record.object < 0x10000 || !HasMemoryAccess(record.object, 0x4C, MemoryProt::CpuRead)) {
        return;
    }

    record.blocked_before = ReadValue<u8>(record.object, 0x49) != 0;
    record.blocked_after = record.blocked_before;
    const bool missing = ReadValue<u8>(record.object, 0x4A) != 0;
    if (!record.blocked_before && !missing) {
        record.result = "already_allowed";
        return;
    }

    // Invasions: an invader uses no lamp of the world it invades (as in the retail game).
    if (Invading()) {
        record.result = "invader";
        return;
    }
    record.player = GetLocalPlayer();
    record.host_effect = HasPlayerEffect(record.player, 9001);
    // A party session (either role, see InstallLampsAsCampfires) uses its lamps too, also one
    // whose entity reads missing (see LampBytesToClear).
    const LampUnblock clear =
        LampBytesToClear(record.blocked_before, missing, record.host_effect, InPartySession());
    if (clear == LampUnblock{}) {
        record.result = "not_multiplayer_host";
        return;
    }
    if (clear.missing) {
        // Once per lamp: this runs for every lamp near the player many times a second, and the
        // line filled the log by hundreds of thousands (Kaniel 2026-10-11: 3 FPS late in a run).
        static std::set<s32> logged_lamps;
        const s32 lamp = ReadValue<s32>(record.object, 0x20);
        if (logged_lamps.insert(lamp).second) {
            LOG_INFO(Debug, "Bloodborne lamp {} read its entity missing; cleared for the party",
                     lamp);
        }
        WriteValue(record.object, 0x4A, u8{});
    }
    if (clear.blocked && !WriteValue(record.object, 0x49, u8{})) {
        record.result = "write_rejected";
        return;
    }
    record.blocked_after = false;
    record.result = "applied";
    // Diagnostics for lamps in a party session: the other availability bytes (+0x48 special map,
    // +0x4A missing entity) and the lamp level (+0x28), once per lamp and change.
    static std::mutex log_mutex;
    static std::unordered_map<s32, std::string> logged;
    const s32 entity = ReadValue<s32>(record.object, 0x20);
    const std::string state =
        fmt::format("special={} missing={} level={}", ReadValue<u8>(record.object, 0x48),
                    ReadValue<u8>(record.object, 0x4A), ReadValue<s32>(record.object, 0x28));
    std::scoped_lock lock{log_mutex};
    if (logged[entity] != state) {
        logged[entity] = state;
        LOG_INFO(Debug, "Bloodborne lamp {} unlocked for the party: {}", entity, state);
    }
}

u64 GetSummonManagerRoot() {
    const u64 image_size = MemoryPatcher::g_eboot_image_size;
    if (image_base == 0 || image_size < sizeof(u64) ||
        SummonManagerRootPointerOffset > image_size - sizeof(u64)) {
        return 0;
    }
    return ReadValue<u64>(image_base + SummonManagerRootPointerOffset, 0);
}

u64 GetSummonManager() {
    const u64 root = GetSummonManagerRoot();
    return root >= 0x10000 ? ReadValue<u64>(root, 0xC50) : 0;
}

u64 GetMatchingState() {
    const u64 image_size = MemoryPatcher::g_eboot_image_size;
    if (image_base == 0 || image_size < sizeof(u64) ||
        MatchingStatePointerOffset > image_size - sizeof(u64)) {
        return 0;
    }
    return ReadValue<u64>(image_base + MatchingStatePointerOffset, 0);
}

} // namespace Coop

std::optional<std::string> GetSeamlessHostPlacementHeader() {
    std::optional<SummonPlacementDescriptor> placement;
    {
        std::scoped_lock lock{seamless_placement_mutex};
        placement = seamless_host_placement;
    }
    if (!placement.has_value()) {
        return std::nullopt;
    }

    std::ostringstream out;
    out << "1," << std::hex << std::setfill('0') << std::setw(8) << placement->packed_region << ','
        << std::setw(8) << std::bit_cast<u32>(placement->x) << ',' << std::setw(8)
        << std::bit_cast<u32>(placement->y) << ',' << std::setw(8)
        << std::bit_cast<u32>(placement->z) << ',' << std::setw(8)
        << std::bit_cast<u32>(placement->heading) << ',' << std::dec << placement->area;
    return out.str();
}

bool SetSeamlessHostPlacementHeader(std::string_view value) {
    std::array<std::string_view, 7> fields{};
    size_t begin = 0;
    for (size_t field_index = 0; field_index < fields.size(); ++field_index) {
        const size_t end = value.find(',', begin);
        const bool final_field = field_index + 1 == fields.size();
        if ((final_field && end != std::string_view::npos) ||
            (!final_field && end == std::string_view::npos)) {
            return false;
        }
        fields[field_index] =
            value.substr(begin, end == std::string_view::npos ? value.size() - begin : end - begin);
        begin = end == std::string_view::npos ? value.size() : end + 1;
    }
    if (fields[0] != "1") {
        return false;
    }

    auto parse_u32 = [](std::string_view field, int base, u32& output) {
        const auto [end, error] =
            std::from_chars(field.data(), field.data() + field.size(), output, base);
        return error == std::errc{} && end == field.data() + field.size();
    };
    SummonPlacementDescriptor placement{};
    std::array<u32, 4> float_bits{};
    if (!parse_u32(fields[1], 16, placement.packed_region) ||
        !parse_u32(fields[2], 16, float_bits[0]) || !parse_u32(fields[3], 16, float_bits[1]) ||
        !parse_u32(fields[4], 16, float_bits[2]) || !parse_u32(fields[5], 16, float_bits[3])) {
        return false;
    }
    const auto [area_end, area_error] =
        std::from_chars(fields[6].data(), fields[6].data() + fields[6].size(), placement.area, 10);
    if (area_error != std::errc{} || area_end != fields[6].data() + fields[6].size()) {
        return false;
    }
    placement.x = std::bit_cast<float>(float_bits[0]);
    placement.y = std::bit_cast<float>(float_bits[1]);
    placement.z = std::bit_cast<float>(float_bits[2]);
    placement.heading = std::bit_cast<float>(float_bits[3]);
    if (!IsUsablePackedMap(placement.packed_region) || !std::isfinite(placement.x) ||
        !std::isfinite(placement.y) || !std::isfinite(placement.z) ||
        !std::isfinite(placement.heading)) {
        return false;
    }

    // A party guest joins where it is (Kaniel 2026-10-08: no teleport to the host on a join, the
    // forked cross-map summon's leftover): its own map, position and heading replace the host's,
    // so the pre-match warp and the summon reload keep it in place.
    if (SummonPlacementDescriptor own{}; IsPartyGuestRole()) {
        std::string_view own_result;
        if (ReadLocalSummonPlacement(own, own_result)) {
            LOG_INFO(Debug,
                     "Bloodborne party: guest joins in place on map {:#x} ({}, {}, {}) instead of "
                     "the host's map {:#x}",
                     own.packed_region, own.x, own.y, own.z, placement.packed_region);
            placement = own;
        }
    }
    ReceiveSeamlessPlacement(placement);
    return true;
}

void ClearSeamlessHostPlacementHeader() {
    std::scoped_lock lock{seamless_placement_mutex};
    seamless_received_host_placement.reset();
    seamless_received_host_placement_consumed = false;
}

void TraceMatching2LeaveRoom(std::uintptr_t return_address, std::uint64_t room_id) {
    if (image_base == 0 || MemoryPatcher::g_game_serial != "CUSA03173") {
        return;
    }

    const u64 caller_offset = return_address >= image_base && return_address - image_base <
                                                                  MemoryPatcher::g_eboot_image_size
                                  ? return_address - image_base
                                  : 0;
    const u64 reload_state = ReadValue<u64>(image_base + SummonSessionRulesPointerOffset, 0);
    const s32 reload_phase = reload_state >= 0x10000 ? ReadValue<s32>(reload_state, 0x84) : -1;
    const u64 global_state = GetGlobalState();
    // Raw: a load-time hook, before the tick.
    const u32 current_map = GetCurrentPackedMap();
    const u32 requested_map = global_state >= 0x10000 ? ReadValue<u32>(global_state, 0x0C) : 0;
    const u32 pending_reload = pending_summon_reload_map.load(std::memory_order_acquire);
    const u32 pending_state = pending_summon_reload_state_map.load(std::memory_order_acquire);
    const u64 sequence = event_sequence.fetch_add(1, std::memory_order_relaxed) + 1;
    const auto timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(
                               std::chrono::system_clock::now().time_since_epoch())
                               .count();

    try {
        std::scoped_lock lock{capture_mutex};
        if (capture_file) {
            capture_file << "{\"type\":\"matching2_leave_room\",\"seq\":" << sequence
                         << ",\"time_ms\":" << timestamp << ",\"return_address\":";
            WriteHex(capture_file, return_address);
            capture_file << ",\"caller_offset\":";
            WriteHex(capture_file, caller_offset);
            capture_file << ",\"room_id\":" << room_id << ",\"reload_state\":";
            WriteHex(capture_file, reload_state);
            capture_file << ",\"reload_phase\":" << reload_phase << ",\"global_state\":";
            WriteHex(capture_file, global_state);
            capture_file << ",\"current_map\":";
            WriteHex(capture_file, current_map);
            capture_file << ",\"requested_map\":";
            WriteHex(capture_file, requested_map);
            capture_file << ",\"pending_reload_map\":";
            WriteHex(capture_file, pending_reload);
            capture_file << ",\"pending_state_map\":";
            WriteHex(capture_file, pending_state);
            capture_file << "}\n";
            capture_file.flush();
        }
    } catch (...) {
        // Diagnostics must never unwind into an NP library call.
    }

    LOG_INFO(Debug,
             "Bloodborne RE Matching2 leave room={} caller={:#x} offset={:#x} reload_phase={} "
             "current_map={:#x} requested_map={:#x}",
             room_id, return_address, caller_offset, reload_phase, current_map, requested_map);
}

namespace {

// Phase 6: the team join without signs or a bell (docs/native-netcode/session.md, "Team join").
// What the summon manager's update (0x01872360) does when a summon goes out or comes in, called
// by us instead: the host's session opened as a summon opens it, its join data written as the
// summon packet carries it, and a guest's join from that data with its own spot as the sign's.
constexpr u64 NetSyncPointerOffset = 0x0553D6D0;
constexpr u64 CreateSessionOffset = 0x01788ED0;  // NetSync, float (the summon's 55.0)
constexpr u64 JoinSessionOffset = 0x01789010;    // NetSync, join param
constexpr u64 SessionInfoOffset = 0x00C8F500;    // session +0x18, info +0x38, 1
constexpr u64 JoinParamCopyOffset = 0x00C90270;  // param, info: constructs the param
constexpr u64 JoinParamValidOffset = 0x00C90D80; // param
constexpr u64 JoinParamWriteOffset = 0x00C90B00; // param, writer
constexpr u64 JoinParamInitOffset = 0x00C8FBF0;  // param: constructs an empty one
constexpr u64 JoinParamReadOffset = 0x00C90DD0;  // param, reader
constexpr u64 JoinParamFreeOffset = 0x00C90520;  // param
constexpr u64 SummonedNoticeOffset = 0x01FF58B0; // after a guest's join (the summon manager's)
constexpr u64 StreamWriterVtableOffset = 0x053B2C80;
constexpr u64 StreamReaderVtableOffset = 0x053B2BE0;
// The session's search time (a summon's: 55 s). Over with nobody summoned, the game ends the
// search with "No guests found" (event text 100300, Kaniel 2026-10-14): a team's session waits
// for its members as long as it is open.
constexpr float SummonSessionSeconds = 24.0F * 3600.0F;
constexpr size_t JoinParamSize = 0x1A8; // the summon manager's frame holds it at rbp-0x1A8
constexpr size_t JoinDataCapacity = 0x200;

bool TeamJoinCallsMatch() {
    return image_base != 0 && team_join_ready;
}

// A field of one of our own buffers handed to the game (not guest memory: no access check).
template <typename T>
void Put(void* buffer, size_t offset, const T& value) {
    std::memcpy(static_cast<u8*>(buffer) + offset, &value, sizeof(value));
}

} // namespace

namespace Coop {

void ReceiveSeamlessPlacement(const SummonPlacementDescriptor& placement) {
    std::scoped_lock lock{seamless_placement_mutex};
    if (!seamless_received_host_placement.has_value() ||
        !IsSameSummonPlacement(*seamless_received_host_placement, placement)) {
        seamless_received_host_placement_consumed = false;
        seamless_received_host_placement_life.Received();
    }
    seamless_received_host_placement = placement;
}

bool OpenTeamSession() {
    const u64 netsync = image_base != 0 ? ReadValue<u64>(image_base + NetSyncPointerOffset, 0) : 0;
    if (netsync < 0x10000 || !TeamJoinCallsMatch()) {
        return false;
    }
    using Create = bool PS4_SYSV_ABI (*)(u64, float);
    const bool opened =
        reinterpret_cast<Create>(image_base + CreateSessionOffset)(netsync, SummonSessionSeconds);
    LOG_INFO(Debug, "Bloodborne team join: the host opens its session (ok={})", opened);
    return opened;
}

std::optional<std::vector<u8>> TeamSessionJoinData() {
    const u64 manager = GetMatchingState();
    if (manager < 0x10000 || !HasMemoryAccess(manager, 0x128, MemoryProt::CpuWrite) ||
        !TeamJoinCallsMatch()) {
        return std::nullopt;
    }
    using Info = void PS4_SYSV_ABI (*)(u64, u64, u32);
    using ParamFrom = void PS4_SYSV_ABI (*)(void*, u64);
    using ParamCheck = bool PS4_SYSV_ABI (*)(void*);
    using ParamWrite = void PS4_SYSV_ABI (*)(void*, void*);
    using ParamFree = void PS4_SYSV_ABI (*)(void*);
    alignas(16) std::array<u8, JoinParamSize + 0x58> param{};
    reinterpret_cast<Info>(image_base + SessionInfoOffset)(manager + 0x18, manager + 0x38, 1);
    reinterpret_cast<ParamFrom>(image_base + JoinParamCopyOffset)(param.data(), manager + 0x38);
    std::optional<std::vector<u8>> join;
    if (reinterpret_cast<ParamCheck>(image_base + JoinParamValidOffset)(param.data())) {
        // The game's memory writer as the summon packet's (0x018736DA): its own buffer, no
        // allocator; +0x30 is how much it wrote.
        alignas(16) std::array<u8, JoinDataCapacity> buffer{};
        alignas(16) std::array<u8, 0x60> writer{};
        Put(writer.data(), 0x00, image_base + StreamWriterVtableOffset);
        Put(writer.data(), 0x10, reinterpret_cast<u64>(buffer.data()));
        Put(writer.data(), 0x18, u64{JoinDataCapacity});
        Put(writer.data(), 0x38, u64{JoinDataCapacity});
        reinterpret_cast<ParamWrite>(image_base + JoinParamWriteOffset)(param.data(),
                                                                        writer.data());
        u64 written = 0;
        std::memcpy(&written, writer.data() + 0x30, sizeof(written));
        if (written > 0 && written <= JoinDataCapacity) {
            join.emplace(buffer.begin(), buffer.begin() + static_cast<std::ptrdiff_t>(written));
        }
    }
    reinterpret_cast<ParamFree>(image_base + JoinParamFreeOffset)(param.data());
    LOG_INFO(Debug, "Bloodborne team join: the host's session join data: {} bytes",
             join ? join->size() : 0);
    return join;
}

bool JoinTeamSession(std::span<const u8> join, u32 kind, u32 sign) {
    const u64 netsync = image_base != 0 ? ReadValue<u64>(image_base + NetSyncPointerOffset, 0) : 0;
    const u64 state = GetGlobalState();
    const u64 manager_root = GetSummonManagerRoot();
    if (netsync < 0x10000 || join.empty() || join.size() > JoinDataCapacity || state < 0x10000 ||
        !HasMemoryAccess(state, 0x1650, MemoryProt::CpuWrite) || manager_root < 0x10000 ||
        !HasMemoryAccess(manager_root, 0xAA0, MemoryProt::CpuWrite) || !TeamJoinCallsMatch()) {
        return false;
    }
    // The sign's spot is this guest's own: it joins where it stands (as a party guest's claim).
    SummonPlacementDescriptor own{};
    std::string_view own_result;
    if (!ReadLocalSummonPlacement(own, own_result)) {
        LOG_INFO(Debug, "Bloodborne team join: no spot to join from ({})", own_result);
        return false;
    }
    using ParamInit = void PS4_SYSV_ABI (*)(void*);
    using ParamRead = bool PS4_SYSV_ABI (*)(void*, void*);
    using ParamFree = void PS4_SYSV_ABI (*)(void*);
    using Join = bool PS4_SYSV_ABI (*)(u64, void*);
    using Notice = void PS4_SYSV_ABI (*)();
    alignas(16) std::array<u8, JoinParamSize + 0x58> param{};
    alignas(16) std::array<u8, JoinDataCapacity> data{};
    std::copy(join.begin(), join.end(), data.begin());
    // The game's memory reader as the summon manager's (0x018738F3).
    alignas(16) std::array<u8, 0x30> reader{};
    Put(reader.data(), 0x00, image_base + StreamReaderVtableOffset);
    Put(reader.data(), 0x08, u64{join.size()});
    Put(reader.data(), 0x10, reinterpret_cast<u64>(data.data()));
    Put(reader.data(), 0x24, u8{1});
    reinterpret_cast<ParamInit>(image_base + JoinParamInitOffset)(param.data());
    bool joined = false;
    const bool read =
        reinterpret_cast<ParamRead>(image_base + JoinParamReadOffset)(param.data(), reader.data());
    if (read) {
        WriteValue(state, 0x16F0, kind);
        WriteValue(state, 0x16F4, static_cast<u8>(sign));
        joined = reinterpret_cast<Join>(image_base + JoinSessionOffset)(netsync, param.data());
        // The summon's placement, as the summon manager writes it from the sign (0x01873A26).
        const std::array<float, 4> position{own.x, own.y, own.z, 1.0F};
        const std::array<float, 4> orientation{0.0F, own.heading, 0.0F, 0.0F};
        WriteValue(state, 0x14C0, own.packed_region);
        WriteValue(state, 0x14A0, position);
        WriteValue(state, 0x14B0, orientation);
        WriteValue(state, 0x14C4, own.packed_region);
        WriteValue(state, 0x14D0, position);
        WriteValue(state, 0x14E0, orientation);
        WriteValue(state, 0x1620, position);
        WriteValue(state, 0x1630, orientation);
        WriteValue(state, 0x1640, own.area);
        WriteValue(state, 0x1644, own.packed_region);
        WriteValue(state, 0x1648, u8{1});
        WriteValue(manager_root, 0xA98, u32{1});
        reinterpret_cast<Notice>(image_base + SummonedNoticeOffset)();
        ReceiveSeamlessPlacement(own);
    }
    reinterpret_cast<ParamFree>(image_base + JoinParamFreeOffset)(param.data());
    LOG_INFO(Debug,
             "Bloodborne team join: joining the host's session ({} bytes, read={} joined={}) in "
             "place on map {:#x} ({}, {}, {})",
             join.size(), read, joined, own.packed_region, own.x, own.y, own.z);
    return joined;
}

} // namespace Coop

} // namespace Core::Bloodborne
