// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// Diagnostics: the co-op trace, the item and state logs. Split from bloodborne_re.cpp; the
// shared declarations are in bloodborne_re_internal.h.

#include "core/bloodborne_crash_dump.h"
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
namespace Coop {

// Diagnostics for a guest reviving in place (Phase 2): each side logs the other players'
// character type and HP (WorldChrMan +0x30 count, +0x38 entries of 0x38 bytes) when they change.
void LogOtherPlayersChanges() {
    static std::string last;
    const u64 world = ReadValue<u64>(image_base + CandidateLocalStatePointerOffset, 0);
    const s32 count = world >= 0x10000 ? ReadValue<s32>(world, 0x30) : 0;
    const u64 entries = world >= 0x10000 ? ReadValue<u64>(world, 0x38) : 0;
    std::string state;
    for (s32 index = 0; index < std::min(count, 8); ++index) {
        const u64 player = ReadValue<u64>(entries + static_cast<u64>(index) * 0x38, 0);
        if (player < 0x10000) {
            continue;
        }
        const u64 data = ReadValue<u64>(ReadValue<u64>(player, 0x3B0), 0x20);
        // The death flags too: a guest that got up invisible to the host (friend run
        // 2026-10-09) left no trace here.
        state += fmt::format(" [type={} hp={}/{} dead={:#x}]", ReadValue<s32>(player, 0x78),
                             ReadValue<s32>(data, 0x128), ReadValue<s32>(data, 0x12C),
                             ReadValue<u8>(player, 0x1E3));
    }
    if (state != last) {
        last = state;
        LOG_INFO(Debug, "Bloodborne other players:{}", state.empty() ? " none" : state);
    }
}

// Diagnostics for a guest reviving in place (Phase 2): the local player's active special
// effects, logged when the set changes, to see what a phantom's death leaves on it.
void LogLocalEffectChanges() {
    static std::string last;
    const u64 player = GetLocalPlayer();
    const u64 manager = player >= 0x10000 ? ReadValue<u64>(player, 0x1C8) : 0;
    std::string effects;
    u64 node = manager >= 0x10000 ? ReadValue<u64>(manager, 0x08) : 0;
    for (size_t index = 0; node >= 0x10000 && index < 128; ++index) {
        const u64 next = ReadValue<u64>(node, 0x58);
        if ((ReadValue<u32>(node, 0x1C) & 0x800C0003U) == 0) {
            effects += fmt::format(" {}", ReadValue<s32>(node, 0x40));
        }
        if (next == node) {
            break;
        }
        node = next;
    }
    if (effects != last) {
        last = effects;
        LOG_INFO(Debug, "Bloodborne local effects:{}", effects.empty() ? " none" : effects);
    }
}

// Diagnostics for a guest that can move and attack but not use X after a boss kill: the states
// a boss clear or a leave could leave behind, logged on change. The event manager's "returning"
// state ([SprjEventMan+0x10]+8, see ReviveCalls::clear_return_state), the player's state flags
// (+0x1E0..+0x1E7) and action state ([+0x3B0]+8: +0x1B4/+0x1B8), its character type (+0x78),
// GlobalState +0x1520 (summon warp) and +0x1592 (pickups allowed), and the leave/death flags.
void LogActionBlockingState() {
    static std::string last;
    static std::chrono::steady_clock::time_point checked{};
    const auto now = std::chrono::steady_clock::now();
    if (now - checked < std::chrono::milliseconds{500}) {
        return;
    }
    checked = now;
    const u64 player = GetLocalPlayer();
    const u64 global = GetGlobalState();
    const u64 events = ReadValue<u64>(image_base + EventManPointerOffset, 0);
    const u64 event_state = events >= 0x10000 ? ReadValue<u64>(events, 0x10) : 0;
    const u64 model = player >= 0x10000 ? ReadValue<u64>(player, 0x3B0) : 0;
    const u64 action = model >= 0x10000 ? ReadValue<u64>(model, 0x08) : 0;
    const std::string text = fmt::format(
        "returning={} flags={:#x} action={} sub={} type={} warp={} pickups={} f4000={} f4047={}",
        event_state >= 0x10000 ? ReadValue<s32>(event_state, 0x08) : -1,
        player >= 0x10000 ? ReadValue<u64>(player, 0x1E0) : 0,
        action >= 0x10000 ? ReadValue<s32>(action, 0x1B4) : -1,
        action >= 0x10000 ? ReadValue<s32>(action, 0x1B8) : -1,
        player >= 0x10000 ? ReadValue<s32>(player, 0x78) : -1,
        global >= 0x10000 ? ReadValue<u8>(global, 0x1520) : 0xFF,
        global >= 0x10000 ? ReadValue<u8>(global, 0x1592) : 0xFF, EventFlagOn(4000),
        EventFlagOn(4047));
    if (text != last) {
        last = text;
        LOG_INFO(Debug, "Bloodborne action state: {}", text);
    }
}

} // namespace Coop

namespace {

// The co-op trace (see CoopTraceVersion): records from any thread go to a queue that a thread of
// its own writes out, so the game never waits for the disk. Off with SHADPS4_BLOODBORNE_TRACE=0.
// The queue is never destroyed: at exit() the writer (detached) still waits on it and any
// file's thread may still trace, while the files' statics go in no defined order.
std::atomic<bool> coop_trace_on{false};
TraceQueue& coop_trace_queue = *new TraceQueue{TraceQueueCap};

s64 UtcMilliseconds() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

} // namespace

namespace Coop {

bool CoopTraceOn() {
    return coop_trace_on.load(std::memory_order_relaxed);
}

void Trace(std::string kind, TraceSampler::Fields fields) {
    if (CoopTraceOn()) {
        coop_trace_queue.Push(
            EncodeTraceRecord({UtcMilliseconds(), std::move(kind), std::move(fields)}));
    }
}

void TraceLife(const char* event, TraceSampler::Fields more) {
    more.insert(more.begin(), std::pair<std::string, std::string>{"event", event});
    Trace("life", std::move(more));
}

// A co-op frame sent or taken (see DecodeCoopFrame); `from_peer` for one taken: whether it came
// from a room peer (only those are used).
void TraceFrame(const char* kind, std::span<const u8> frame, u32 addr, u16 port,
                std::optional<bool> from_peer) {
    if (!CoopTraceOn()) {
        return;
    }
    const auto decoded = DecodeCoopFrame(frame);
    if (!decoded) {
        return;
    }
    TraceSampler::Fields fields{{"type", fmt::format("{:02x}", decoded->first)},
                                {"peer", fmt::format("{:08x}:{}", addr, port)}};
    if (from_peer) {
        fields.emplace_back("from-peer", *from_peer ? "1" : "0");
    }
    fields.emplace_back("payload", TraceHex(decoded->second));
    Trace(kind, std::move(fields));
}

// A board post and its answer. A body the same as the endpoint's last one in this file is only
// noted ("same"): the HP board posts every 100 ms.
void TraceBoard(const char* kind, const std::string& endpoint, std::optional<int> status,
                std::string_view body) {
    static std::mutex mutex;
    static std::map<std::string, size_t> last_body; // kind and endpoint -> the last body's hash
    static u32 last_file = 0;
    const size_t hash = std::hash<std::string_view>{}(body);
    bool same = false;
    {
        std::scoped_lock lock{mutex};
        if (const u32 file = coop_trace_file.load(); file != last_file) {
            last_file = file;
            last_body.clear();
        }
        const auto [it, added] = last_body.try_emplace(fmt::format("{} {}", kind, endpoint), hash);
        same = !added && it->second == hash;
        it->second = hash;
    }
    TraceRecord record{0, kind, {{"endpoint", endpoint}}};
    if (status) {
        record.fields.emplace_back("status", std::to_string(*status));
    }
    if (same) {
        record.fields.emplace_back("same", "1");
    } else {
        AddTraceBody(record, body);
    }
    Trace(std::move(record.kind), std::move(record.fields));
}

// Now in the PC's local time, as the player reads it (file names).
std::chrono::sys_seconds LocalTimeNow() {
    const std::time_t now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    std::tm local{};
#ifdef _WIN32
    localtime_s(&local, &now);
#else
    localtime_r(&now, &local);
#endif
    using namespace std::chrono;
    return sys_days{year{local.tm_year + 1900} / static_cast<unsigned>(local.tm_mon + 1) /
                    static_cast<unsigned>(local.tm_mday)} +
           hours{local.tm_hour} + minutes{local.tm_min} + seconds{local.tm_sec};
}

// The trace's writer: a new file at the start and whenever one reaches TraceFileCap, the newest
// TraceFilesKept kept. Lines are flushed every quarter second, so a crash loses little.
void StartCoopTrace() {
    if (!CoopTraceEnabled(std::getenv("SHADPS4_BLOODBORNE_TRACE"))) {
        LOG_INFO(Debug, "Bloodborne co-op trace is off");
        return;
    }
    const std::filesystem::path dir = Common::FS::GetUserPath(Common::FS::PathType::LogDir);
#ifdef _WIN32
    const u32 process = static_cast<u32>(_getpid());
#else
    const u32 process = static_cast<u32>(getpid());
#endif
    const char* role = std::getenv("SHADPS4_BLOODBORNE_PARTY_ROLE");
    coop_trace_on = true;
    std::thread([dir, process, role = std::string{role != nullptr ? role : ""}] {
        std::ofstream file;
        std::uintmax_t written = 0;
        const auto open = [&] {
            file.close();
            std::error_code error;
            std::filesystem::create_directories(dir, error);
            file.open(dir / CoopTraceFileName(LocalTimeNow(), process),
                      std::ios::binary | std::ios::trunc);
            ++coop_trace_file;
            PruneCoopTraces(dir, TraceFilesKept);
            const std::string header =
                EncodeTraceRecord({UtcMilliseconds(),
                                   "trace",
                                   {{"version", std::to_string(CoopTraceVersion)},
                                    {"role", role},
                                    {"process", std::to_string(process)}}});
            file << header << '\n';
            written = header.size() + 1;
        };
        open();
        while (true) {
            std::vector<std::string> lines = coop_trace_queue.Take(std::chrono::milliseconds{250});
            if (const u64 dropped = coop_trace_queue.TakeDropped(); dropped > 0) {
                lines.push_back(EncodeTraceRecord(
                    {UtcMilliseconds(), "trace-dropped", {{"count", std::to_string(dropped)}}}));
            }
            for (const std::string& line : lines) {
                file << line << '\n';
                written += line.size() + 1;
            }
            if (!lines.empty()) {
                file.flush();
            }
            if (written >= TraceFileCap) {
                open();
            }
        }
    }).detach();
    LOG_INFO(Debug, "Bloodborne co-op trace: {}{}*.txt (the newest {} kept)",
             Common::FS::PathToUTF8String(dir / ""), CoopTracePrefix, TraceFilesKept);
}

void InstallItemModuleDiagnostics(u64 base, u64 image_size) {
    if (!party_controller || !EnvFlagEnabled("SHADPS4_BLOODBORNE_RE_TRACE")) {
        return;
    }
    std::optional<u64> failed;
    for (size_t index = 0; index < ItemModuleChecks.size(); ++index) {
        const ItemModuleCheck& check = ItemModuleChecks[index];
        if (check.offset + check.bytes.size() > image_size ||
            !InstallCoopHook(reinterpret_cast<void*>(base + check.offset), check.bytes, index,
                             CountItemModuleCheck)) {
            LOG_ERROR(Debug, "Bloodborne item diagnostics: no hook at {:#x}", check.offset);
            failed = failed.value_or(check.offset);
        }
    }
    if (failed) {
        coop_features.Skipped("item diagnostics", SiteFailure(base, image_size, *failed));
    } else {
        coop_features.On("item diagnostics");
    }
}

void LogItemModuleChecks() {
    static std::array<u64, ItemModuleChecks.size()> logged{};
    static std::chrono::steady_clock::time_point last{};
    const auto now = std::chrono::steady_clock::now();
    if (now - last < std::chrono::seconds{2}) {
        return;
    }
    last = now;
    std::string text;
    for (size_t index = 0; index < ItemModuleChecks.size(); ++index) {
        const u64 hits = item_module_check_hits[index].load(std::memory_order_relaxed);
        if (hits != logged[index]) {
            text += fmt::format(" {:#x}+{}", ItemModuleChecks[index].offset, hits - logged[index]);
            logged[index] = hits;
        }
    }
    if (!text.empty()) {
        const u64 multi_play = GetMatchingState();
        const s32 state = multi_play >= 0x10000 ? ReadValue<s32>(multi_play, 0x124) : -1;
        LOG_INFO(Debug, "Bloodborne item checks (state {}):{}", state, text);
    }
}

} // namespace Coop

namespace {

// Diagnostics for the guest's missing pick-up prompt: the player's item button update
// (0x012FD880) asks MapItemMan for the nearest usable map item (0x017DA500, list +0x28,
// [rbp-0x58] = its handle), passes a busy check (0x0176B7E0), shows the prompt and, once
// pressed (0x01CD91F0 true), picks it up (0x017D8570). Inside the search, a usable item is
// examined at 0x017DA58B and one in the button's range lands at 0x017DA7CE.
constexpr u64 ItemButtonUpdateOffset = 0x012FD8CD;
constexpr std::array<u8, 10> ItemButtonUpdate{0x48, 0x8D, 0x7D, 0xC0, 0x41,
                                              0xBC, 0x04, 0x10, 0x00, 0x00};
constexpr u64 ItemButtonFoundOffset = 0x012FDC17;
constexpr std::array<u8, 8> ItemButtonFound{0x8B, 0x45, 0xA8, 0x89, 0x45, 0x9C, 0x85, 0xC0};
constexpr u64 ItemButtonNotBusyOffset = 0x012FDC84;
constexpr u64 ItemButtonPressedOffset = 0x012FDD95;
constexpr std::array<u8, 7> ItemButtonPlayerLoad{0x49, 0x8B, 0x85, 0xB0, 0x03, 0x00, 0x00};
constexpr u64 MapItemSearchExaminedOffset = 0x017DA58B;
constexpr std::array<u8, 6> MapItemSearchExamined{0xC4, 0xC1, 0x7A, 0x10, 0x46, 0x30};
constexpr u64 MapItemSearchInRangeOffset = 0x017DA7CE;
constexpr std::array<u8, 11> MapItemSearchInRange{0x41, 0x8B, 0x46, 0x20, 0x48, 0x8B,
                                                  0x8D, 0x10, 0xFF, 0xFF, 0xFF};

} // namespace

namespace Coop {

std::array<std::atomic<u64>, 6> item_button_counts{};

} // namespace Coop

namespace {

std::atomic<u32> item_button_last_handle{};

void PS4_SYSV_ABI NoteItemButton(u64 tag, const GuestRegisterSnapshot* registers) {
    if (tag >= item_button_counts.size()) {
        return;
    }
    if (tag == 1) {
        const u32 handle = ReadValue<u32>(registers->rbp - 0x58, 0);
        if (handle == 0) {
            return;
        }
        item_button_last_handle.store(handle, std::memory_order_relaxed);
    }
    item_button_counts[tag].fetch_add(1, std::memory_order_relaxed);
}

} // namespace

namespace Coop {

// On in every co-op game (was SHADPS4_BLOODBORNE_RE_TRACE only): a host could not pick up
// items after its guest's crash (friend run 2026-10-09) and the logs could not say where the
// pickup stopped. Six counting hooks, logged with the map items when they change.
void InstallItemButtonDiagnostics(u64 base, u64 image_size) {
    if (!party_controller) {
        return;
    }
    const std::array<std::pair<u64, std::span<const u8>>, 6> probes{{
        {ItemButtonUpdateOffset, ItemButtonUpdate},
        {ItemButtonFoundOffset, ItemButtonFound},
        {ItemButtonNotBusyOffset, ItemButtonPlayerLoad},
        {ItemButtonPressedOffset, ItemButtonPlayerLoad},
        {MapItemSearchExaminedOffset, MapItemSearchExamined},
        {MapItemSearchInRangeOffset, MapItemSearchInRange},
    }};
    for (u64 tag = 0; tag < probes.size(); ++tag) {
        const auto& [offset, bytes] = probes[tag];
        if (!InstallCoopHook(reinterpret_cast<void*>(base + offset), bytes, tag, NoteItemButton)) {
            LOG_ERROR(Debug, "Bloodborne item button diagnostics unavailable");
            coop_features.Skipped("item button diagnostics", SiteFailure(base, image_size, offset));
            return;
        }
    }
    coop_features.On("item button diagnostics");
}

// The local player's position, where the map item search reads it
// ([[[player+0x58]+8]+0x3B0]+0x68]+0x1E0).
std::optional<std::array<float, 3>> MapItemSearchPlayerPosition() {
    const u64 player = GetLocalPlayer();
    u64 node = player >= 0x10000 ? ReadValue<u64>(player, 0x58) : 0;
    for (const u64 offset : {u64{8}, u64{0x3B0}, u64{0x68}}) {
        if (node < 0x10000 || !HasMemoryAccess(node, offset + 8, MemoryProt::CpuRead)) {
            return std::nullopt;
        }
        node = ReadValue<u64>(node, offset);
    }
    if (node < 0x10000 || !HasMemoryAccess(node, 0x1EC, MemoryProt::CpuRead)) {
        return std::nullopt;
    }
    return std::array<float, 3>{ReadValue<float>(node, 0x1E0), ReadValue<float>(node, 0x1E4),
                                ReadValue<float>(node, 0x1E8)};
}

// The usable map items (MapItemMan's list +0x28, flag 0x40 at +0x48; see
// LogItemButtonAndMapItems) with their distance from `from`, nearest first.
std::vector<std::pair<float, MapItemSpot>> UsableMapItemsNear(const std::array<float, 3>& from) {
    std::vector<std::pair<float, MapItemSpot>> found;
    const u64 manager = ReadValue<u64>(image_base + MapItemManPointerOffset, 0);
    if (manager < 0x10000 || !HasMemoryAccess(manager, 0x40, MemoryProt::CpuRead)) {
        return found;
    }
    u64 item = ReadValue<u64>(manager, 0x28);
    for (u32 count = 0;
         item >= 0x10000 && count < 4096 && HasMemoryAccess(item, 0x58, MemoryProt::CpuRead);
         ++count, item = ReadValue<u64>(item, 0)) {
        if ((ReadValue<u16>(item, 0x48) & 0x40) == 0) {
            continue;
        }
        const std::array<float, 3> at{ReadValue<float>(item, 0x30), ReadValue<float>(item, 0x34),
                                      ReadValue<float>(item, 0x38)};
        const float dx = at[0] - from[0];
        const float dy = at[1] - from[1];
        const float dz = at[2] - from[2];
        found.push_back(
            {std::sqrt(dx * dx + dy * dy + dz * dz), MapItemSpot{ReadValue<u32>(item, 0x20), at}});
    }
    std::ranges::sort(found, {}, &std::pair<float, MapItemSpot>::first);
    return found;
}

void LogItemButtonAndMapItems() {
    static std::string last;
    static std::chrono::steady_clock::time_point last_time{};
    const auto now = std::chrono::steady_clock::now();
    if (now - last_time < std::chrono::seconds{3}) {
        return;
    }
    last_time = now;
    std::string lists;
    std::string nearest;
    const auto position = MapItemSearchPlayerPosition();
    if (position) {
        nearest = fmt::format(" player at ({:.1f}, {:.1f}, {:.1f});", (*position)[0],
                              (*position)[1], (*position)[2]);
    }
    const u64 manager = ReadValue<u64>(image_base + MapItemManPointerOffset, 0);
    if (manager >= 0x10000 && HasMemoryAccess(manager, 0x40, MemoryProt::CpuRead)) {
        std::vector<std::pair<float, std::string>> closest;
        for (const u64 list : {0x18, 0x28, 0x38}) {
            u32 count = 0, usable = 0, with_handle = 0;
            u64 item = ReadValue<u64>(manager, list);
            while (item >= 0x10000 && count < 4096 &&
                   HasMemoryAccess(item, 0x58, MemoryProt::CpuRead)) {
                ++count;
                const u16 flags = ReadValue<u16>(item, 0x48);
                usable += (flags & 0x40) != 0;
                with_handle += (ReadValue<u32>(item, 0x20) >> 4) != 0;
                if (position) {
                    const float x = ReadValue<float>(item, 0x30);
                    const float y = ReadValue<float>(item, 0x34);
                    const float z = ReadValue<float>(item, 0x38);
                    const float dx = x - (*position)[0];
                    const float dy = y - (*position)[1];
                    const float dz = z - (*position)[2];
                    closest.emplace_back(
                        std::sqrt(dx * dx + dy * dy + dz * dz),
                        fmt::format(" [{:#x} at ({:.1f}, {:.1f}, {:.1f}) flags {:#x}]",
                                    ReadValue<u32>(item, 0x20), x, y, z, flags));
                }
                item = ReadValue<u64>(item, 0);
            }
            lists += fmt::format(" [{:#x}: {} items, {} usable, {} with handle]", list, count,
                                 usable, with_handle);
        }
        std::ranges::sort(closest, {}, &std::pair<float, std::string>::first);
        for (size_t index = 0; index < std::min<size_t>(closest.size(), 4); ++index) {
            nearest += fmt::format(" {:.1f} m{}", closest[index].first, closest[index].second);
        }
    }
    const std::string text = fmt::format(
        "button updates {} examined {} in range {} found {} (last handle {:#x}) not busy {} "
        "pressed {};{}{}",
        item_button_counts[0].load() > 0 ? "yes" : "no", item_button_counts[4].load(),
        item_button_counts[5].load(), item_button_counts[1].load(), item_button_last_handle.load(),
        item_button_counts[2].load(), item_button_counts[3].load(), lists, nearest);
    if (text != last) {
        last = text;
        LOG_INFO(Debug, "Bloodborne map items: {}", text);
    }
}

} // namespace Coop

void LogGameCallChain(const char* what) {
    if (image_base == 0) {
        return;
    }
    // The HLE function runs on the guest thread's stack, so the game frames sit above this one.
    volatile u64 marker = 0;
    const u64 start = reinterpret_cast<u64>(&marker);
    std::vector<u64> words;
    for (u64 address = start; address < start + 0x4000; address += 8) {
        if (!HasMemoryAccess(address, 8, MemoryProt::CpuRead)) {
            break;
        }
        words.push_back(*reinterpret_cast<const u64*>(address));
    }
    LOG_INFO(Debug, "Bloodborne {} from the game:{}", what,
             FormatGameAddresses(words, image_base, MemoryPatcher::g_eboot_image_size, 24));
}

} // namespace Core::Bloodborne
