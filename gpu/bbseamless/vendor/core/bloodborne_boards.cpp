// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// The party boards' clients (rest, HP, vitals) and the direct P2P frames. Split from
// bloodborne_re.cpp; the shared declarations are in bloodborne_re_internal.h. With the party's
// world on the server (phase 3c) the boards go over its stream instead (RunServerBoards).

#include "core/bloodborne_crash_dump.h"
#include "core/bloodborne_party.h"
#include "core/bloodborne_portrait.h"
#include "core/bloodborne_re.h"
#include "core/bloodborne_re_internal.h"
#include "core/bloodborne_trade.h"
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
namespace {} // namespace

namespace Coop {

// The signed-in user's online id, or "" before shadNet's login.
std::string LocalOnlineId() {
    int user_id = -1;
    if (Libraries::UserService::sceUserServiceGetInitialUser(&user_id) != 0 || user_id < 0) {
        return {};
    }
    auto& np = Libraries::Np::NpHandler::GetInstance();
    if (!np.IsPsnSignedIn(user_id)) {
        return {};
    }
    const auto online_id = np.GetOnlineId(user_id);
    return std::string{online_id.data, strnlen(online_id.data, sizeof(online_id.data))};
}

} // namespace Coop

namespace {

// The boards know a member by its online id (hex, see BoardMemberOfOnlineId): a party of three
// tells its session members apart by it. Random if shadNet never signs in.
std::string FindBoardMember(std::stop_token stop) {
    std::string member;
    for (int attempt = 0; attempt < 120 && !stop.stop_requested() && member.empty(); ++attempt) {
        if (const std::string online_id = LocalOnlineId(); !online_id.empty()) {
            member = BoardMemberOfOnlineId(online_id);
        } else {
            std::this_thread::sleep_for(std::chrono::milliseconds{500});
        }
    }
    if (member.empty()) {
        std::array<u8, 8> random_bytes{};
        std::random_device random;
        for (auto& byte : random_bytes) {
            byte = static_cast<u8>(random());
        }
        member = BytesToHex(random_bytes);
    }
    return member;
}

} // namespace

namespace Coop {

// Enemy HP (see SharedDamage) on its own client, every HpBoardRound and at once when this
// member's own damage changes (see HpBoardWait): on the rest board's one-second round the other
// members' hits showed late (friend run 2026-10-10), and on a 200 ms round the host's two-hit
// kills were over before the guests' bars moved (forest run 2026-10-11).
std::condition_variable_any enemy_hp_wake;
bool enemy_hp_changed = false; // enemy_hp_mutex

} // namespace Coop

namespace {

void StopBoardClientsAtExit();

} // namespace

namespace Coop {

void StartRestBoardClient() {
    const char* party_setting = std::getenv("SHADPS4_BLOODBORNE_PARTY");
    const auto party = party_setting ? ParsePartySetting(party_setting) : std::nullopt;
    const std::string base_url = EmulatorSettings.GetShadNetWebApiServer();
    if (!lamp_rest_hooks_installed || (!party && !TeamModeOn()) ||
        !EmulatorSettings.IsShadNetEnabled() || base_url.empty()) {
        return;
    }
    // The boards go over the server's world (StartVitalsBoardClient starts them).
    Libraries::Np::NpTrophy::SetTrophyUnlockObserver(NoteGameTrophyUnlock);
}

// Which build each game runs (see PeerHelloType): this game's, from SetBuildIdentity; no hello
// goes out before it is set.
std::mutex peer_hello_mutex;
std::optional<PeerHello> own_hello;

} // namespace Coop

namespace {

PeerBuildCheck peer_build_check;

void NotePeerHello(PeerHello theirs, u32 addr, u16 port) {
    std::optional<std::string> warning;
    {
        std::scoped_lock lock{peer_hello_mutex};
        if (!own_hello) {
            return;
        }
        if (theirs.member.empty()) {
            theirs.member = fmt::format("peer {:08x}:{}", addr, port);
        }
        warning = peer_build_check.Take(theirs, *own_hello);
    }
    if (warning) {
        LOG_WARNING(Debug, "{}", *warning);
    }
}

} // namespace

namespace {

// Party portraits (bloodborne_portrait.h): the members' (from their frames, on the game thread) and
// the status page's captures (from the port's texture cache).
std::mutex portraits_mutex;
PartyPortraits party_portraits; // portraits_mutex
FaceCapture face_capture;       // portraits_mutex
PortraitAssembler portrait_assembler;

void TakePortraitPart(std::span<const u8> payload) {
    const auto part = DecodePortraitPart(payload);
    if (!part) {
        return;
    }
    if (part->None()) {
        std::scoped_lock lock{portraits_mutex};
        if (party_portraits.Has(part->member)) {
            LOG_INFO(Debug, "Bloodborne party portraits: {} has no portrait any more",
                     part->member);
            party_portraits.Forget(part->member);
        }
        return;
    }
    auto pixels = portrait_assembler.Take(*part);
    if (!pixels) {
        return;
    }
    LOG_INFO(Debug, "Bloodborne party portraits: {}'s portrait came ({:08x})", part->member,
             part->id);
    std::scoped_lock lock{portraits_mutex};
    party_portraits.Take(part->member, std::move(*pixels));
}

} // namespace

namespace Coop {

void TakeServerPortrait(const std::string& member, u32 id, std::vector<u8> pixels) {
    if (pixels.empty()) {
        TakePortraitPart(EncodePortraitNone(member));
        return;
    }
    const u8 parts = static_cast<u8>((pixels.size() + PortraitPartBytes - 1) / PortraitPartBytes);
    for (u8 part = 0; part < parts; ++part) {
        const size_t at = size_t{part} * PortraitPartBytes;
        const size_t size = std::min(PortraitPartBytes, pixels.size() - at);
        TakePortraitPart(EncodePortraitPart(
            {member, id, part, parts, {pixels.begin() + at, pixels.begin() + at + size}}));
    }
}

} // namespace Coop

namespace {

// One co-op frame from a room peer (`bytes` whole, `payload` after the type); whether it brought
// what the enemy step uses (HP, kills, the world log), so that step runs on the next frame.
bool TakeCoopFrame(u8 type, std::span<const u8> payload, std::span<const u8> bytes, u32 addr,
                   u16 port) {
    const u64 sender = (u64{addr} << 16) | port;
    const s64 now = SteadyMilliseconds();
    if (type == DirectVitalsType) {
        if (auto vitals = ParseVitalsUpdate(std::string(payload.begin(), payload.end()))) {
            std::scoped_lock lock{direct_boards_mutex};
            direct_vitals_inbox.Take(sender, std::move(*vitals), now);
        }
        return false;
    }
    if (type == DirectKillsType) {
        auto kills = DecodeDirectKills(payload);
        if (kills) {
            std::scoped_lock lock{direct_boards_mutex};
            direct_kills_inbox.Take(sender, std::move(*kills), now);
        }
        return kills.has_value();
    }
    if (type == PortraitPartType) {
        TakePortraitPart(payload);
        return false;
    }
    if (type == PeerHelloType) {
        if (auto hello = DecodePeerHello(payload)) {
            NotePeerHello(std::move(*hello), addr, port);
        }
        return false;
    }
    if (IsWorldLogFrameType(type)) {
        TakeWorldLogFrame(type, payload, addr, port);
        return true;
    }
    auto state = DecodeDirectDamage(bytes);
    if (state) {
        direct_damage_inbox.Take(sender, std::move(*state), now);
    }
    return state.has_value();
}

} // namespace

namespace Coop {

bool TakeDirectDamage() {
    std::array<u8, 1400> buffer{};
    std::optional<std::vector<Libraries::Np::NpMatching2::RoomPeerAddress>> peers;
    bool took = false;
    for (int frames = 0; frames < 64; ++frames) {
        u32 addr = 0;
        u16 port = 0;
        const int size = Libraries::Net::P2PCoopRecvFrom(
            buffer.data(), static_cast<u32>(buffer.size()), &addr, &port);
        if (size <= 0) {
            break;
        }
        if (!peers) {
            peers = Libraries::Np::NpMatching2::GetActiveRoomPeers();
        }
        const bool from_peer = std::ranges::any_of(
            *peers, [&](const auto& peer) { return peer.addr == addr && peer.port == port; });
        const std::span<const u8> bytes{buffer.data(), static_cast<size_t>(size)};
        const auto frame = DecodeCoopFrame(bytes);
        TraceFrame("p2p-in", bytes, addr, port, from_peer);
        if (from_peer && frame) {
            took = TakeCoopFrame(frame->first, frame->second, bytes, addr, port) || took;
        }
    }
    return took;
}

} // namespace Coop

namespace Coop {

void SendToRoomPeers(const std::vector<u8>& frame) {
    for (const auto& peer : Libraries::Np::NpMatching2::GetActiveRoomPeers()) {
        SendToPeer(frame, peer.addr, peer.port);
    }
}

void SendToPeer(const std::vector<u8>& frame, u32 addr, u16 port) {
    Libraries::Net::P2PCoopSendTo(frame.data(), static_cast<u32>(frame.size()), addr, port);
    TraceFrame("p2p-out", frame, addr, port);
}

} // namespace Coop

namespace {

std::string BoardMemberName();

// Another member left the session (MemberLeft): the frames of the senders no longer linked go at
// once (a dead game's last vitals line counted it as standing for DirectDamageLifeMs); the
// remaining members' stay.
[[maybe_unused]] const bool member_frames_reset_registered =
    RegisterLifecycleReset("frames of members gone", {Lifecycle::MemberLeft}, [] {
        std::vector<u64> linked;
        for (const auto& peer : Libraries::Np::NpMatching2::GetActiveRoomPeers()) {
            linked.push_back((u64{peer.addr} << 16) | peer.port);
        }
        direct_damage_inbox.KeepOnly(linked);
        std::scoped_lock lock{direct_boards_mutex};
        direct_vitals_inbox.KeepOnly(linked);
        direct_kills_inbox.KeepOnly(linked);
    });

} // namespace

namespace Coop {

// This member's standing kills to every room peer: at once when they changed, else every
// DirectDamageResendMs. Its whole set each time: a lost frame is made good by the next one.
void SendDirectKills(std::span<const u32> kills, bool changed) {
    static s64 sent_at = 0;
    const s64 now = SteadyMilliseconds();
    if (!changed && now - sent_at < DirectDamageResendMs) {
        return;
    }
    sent_at = now;
    SendToRoomPeers(EncodeCoopFrame(DirectKillsType, EncodeDirectKills(BoardMemberName(), kills)));
}

// This member's report to every room peer: at once when it changed, else every
// DirectDamageResendMs (a lost frame is made good by the next one: its counts only grow).
void SendDirectDamage(const MemberMapState& mine, bool changed) {
    static s64 sent_at = 0;
    const s64 now = SteadyMilliseconds();
    if (mine.map == 0 || (!changed && now - sent_at < DirectDamageResendMs)) {
        return;
    }
    sent_at = now;
    const std::vector<u8> frame = EncodeDirectDamage(mine.map, mine.bits);
    for (const auto& peer : Libraries::Np::NpMatching2::GetActiveRoomPeers()) {
        Libraries::Net::P2PCoopSendTo(frame.data(), static_cast<u32>(frame.size()), peer.addr,
                                      peer.port);
        TraceFrame("p2p-out", frame, peer.addr, peer.port);
    }
}

// This game's hello to every room peer: every PeerHelloResendMs, and at once when a peer it has
// not greeted joins the room's links (a new member, or one whose link came back).
void SendPeerHello() {
    static s64 checked_at = 0;
    static s64 sent_at = 0;
    static std::set<u64> greeted;
    const s64 now = SteadyMilliseconds();
    if (now - checked_at < DirectDamageResendMs) {
        return;
    }
    checked_at = now;
    PeerHello hello;
    {
        std::scoped_lock lock{peer_hello_mutex};
        if (!own_hello) {
            return;
        }
        hello = *own_hello;
    }
    const auto peers = Libraries::Np::NpMatching2::GetActiveRoomPeers();
    std::set<u64> linked;
    for (const auto& peer : peers) {
        linked.insert((u64{peer.addr} << 16) | peer.port);
    }
    const bool new_peer =
        std::ranges::any_of(linked, [&](u64 peer) { return !greeted.contains(peer); });
    for (const u64 peer : linked) {
        if (!greeted.contains(peer)) {
            TraceLife("peer-join", {{"peer", fmt::format("{:08x}:{}", peer >> 16, peer & 0xFFFF)}});
        }
    }
    for (const u64 peer : greeted) {
        if (!linked.contains(peer)) {
            TraceLife("peer-leave",
                      {{"peer", fmt::format("{:08x}:{}", peer >> 16, peer & 0xFFFF)}});
        }
    }
    greeted = std::move(linked);
    if (peers.empty() || (!new_peer && now - sent_at < PeerHelloResendMs)) {
        return;
    }
    sent_at = now;
    hello.member = BoardMemberName();
    SendToRoomPeers(EncodeCoopFrame(PeerHelloType, EncodePeerHello(hello)));
}

} // namespace Coop

namespace {

// Kaniel 2026-10-18: this player's Blood Vials and Quicksilver Bullets (the game's thread reads
// them, TickPortraits), for the others' party lists.
std::atomic<u32> own_vials{};
std::atomic<u32> own_bullets{};

// The character's face data (PlayerGameData +0x4AC, 0xF0 bytes, as channel 8 sends it): its
// portrait is kept on disk by it. Nothing before the character is loaded.
std::optional<u64> OwnFaceFingerprint() {
    const u64 data = Coop::OwnGameData();
    if (data < 0x10000 || !HasMemoryAccess(data + 0x4AC, 0xF0, MemoryProt::CpuRead)) {
        return std::nullopt;
    }
    u64 hash = 0xCBF29CE484222325ull;
    bool any = false;
    for (u64 index = 0; index < 0xF0; ++index) {
        const u8 byte = ReadValue<u8>(data, 0x4AC + index);
        any = any || byte != 0;
        hash = (hash ^ byte) * 0x100000001B3ull;
    }
    return any ? std::optional<u64>{hash} : std::nullopt;
}

// The armour the character wears (the status page's model shows it): PlayerGameData's ChrAsm
// (+0x240) +0x84, +0x88, +0x8C and +0x90, the head, body, arms and legs ids (the rig's 2026-10-13
// dump of Kaniel's guest in the Foreign set: 230000, 231000, 232000, 233000; the weapons at
// +0x64..+0x70 before them). Nothing before the character is loaded.
std::optional<std::array<s32, 4>> OwnOutfit() {
    const u64 data = Coop::OwnGameData();
    if (data < 0x10000 || !HasMemoryAccess(data + 0x240 + 0x84, 0x10, MemoryProt::CpuRead)) {
        return std::nullopt;
    }
    std::array<s32, 4> outfit{};
    for (u64 piece = 0; piece < outfit.size(); ++piece) {
        outfit[piece] = ReadValue<s32>(data, 0x240 + 0x84 + piece * 4);
    }
    return outfit;
}

// Whether the off-screen model is wanted now (OffscreenPortraitWanted; the emulator, without the
// party HUD, never wants it).
std::atomic<bool> offscreen_wanted{false};

#ifdef BLOODBORNE_PARTY_HUD
// The status page's body renderer (CSMenuAsmModelRend, vtable 0x53A4F00) built off-screen, so a
// character has its portrait without opening the status page (Kaniel 2026-10-13). It is a task of
// its own: its constructor registers it, and it steps (Init .. Wait_Setup, the parts' load, then
// Init_Play, Wait_Play: drawn into its target every frame) by itself. The status window
// (0x1B35810) only fills its request and destroys it (0x1B35A40); so do we. Index 5 is
// RankingBody: 560x768 with the status body's camera, a target of its own.
constexpr u64 MenuHeapPointerOffset = 0x05540418;
constexpr u64 BodyRendererConstructorOffset = 0x01DB1190;
constexpr u64 BodyRendererSetArmourOffset = 0x01DB1B90;
// Kaniel 2026-10-17: the camera the character creator zooms onto the face with (0x1DB1960: mode
// +0x804, the renderer table's per-mode lens, distance and angles; mode 1: a 16 degree lens on the
// head, mode 0 the 49 degree body view; the creator's menu 0x1B07870 switches its four previews).
constexpr u64 BodyRendererSetCameraModeOffset = 0x01DB1960;
constexpr s32 FaceCameraMode = 1;
constexpr u64 OwnerHeapOffset = 0x0207B720;
constexpr u64 GameDataManPointerOffset = 0x0553B130;
constexpr s32 RankingBodyIndex = 5;
constexpr u64 BodyRendererSize = 0x8A0;
struct OffscreenModel {
    OffscreenState state = OffscreenState::Idle;
    u64 renderer = 0;
    s64 since = 0;
    s64 built_at = -1'000'000;
    s32 logged_step = -2;
};
OffscreenModel offscreen;

u64 PlayerGameDataNow() {
    const u64 man = ReadValue<u64>(image_base + GameDataManPointerOffset, 0);
    return man >= 0x10000 ? ReadValue<u64>(man, 8) : 0;
}

// Allocates and fills the renderer's request as the status window does (0x1B35963): the face
// (PlayerGameData +0x4AC, 0xF0 bytes) at +0x568, the gender (+0xCA) at +0x658, the armour (its
// ChrAsm, PlayerGameData +0x240) through 0x1DB1B90, no frames to wait (+0x7F0), the request
// (+0x660).
u64 BuildOffscreenModel() {
    const u64 data = PlayerGameDataNow();
    const u64 heap = ReadValue<u64>(image_base + MenuHeapPointerOffset, 0);
    if (data < 0x10000 || heap < 0x10000 ||
        !HasMemoryAccess(data + 0x4AC, 0xF0, MemoryProt::CpuRead)) {
        return 0;
    }
    const u64 allocate = ReadValue<u64>(ReadValue<u64>(heap, 0), 0x58);
    const u64 renderer = reinterpret_cast<u64 PS4_SYSV_ABI (*)(u64, u64, u64)>(allocate)(
        heap, BodyRendererSize, 0x10);
    if (renderer < 0x10000) {
        return 0;
    }
    reinterpret_cast<void PS4_SYSV_ABI (*)(u64, s32)>(image_base + BodyRendererConstructorOffset)(
        renderer, RankingBodyIndex);
    std::memcpy(reinterpret_cast<void*>(renderer + 0x568),
                reinterpret_cast<const void*>(data + 0x4AC), 0xF0);
    WriteValue<u8>(renderer, 0x658, ReadValue<u8>(data, 0xCA));
    reinterpret_cast<void PS4_SYSV_ABI (*)(u64, u64)>(image_base + BodyRendererSetArmourOffset)(
        renderer, data + 0x240);
    reinterpret_cast<void PS4_SYSV_ABI (*)(u64, s32)>(image_base + BodyRendererSetCameraModeOffset)(
        renderer, FaceCameraMode);
    WriteValue<s32>(renderer, 0x7F0, 0);
    WriteValue<u8>(renderer, 0x660, 1);
    return renderer;
}

// Wait_Play (6) with the parts loaded (+0x662) and its target in the draw list (+0xC8 +0x58).
bool OffscreenModelDrawn(u64 renderer) {
    const u64 target = ReadValue<u64>(renderer, 0xC8);
    return ReadValue<s32>(renderer, 0x50) == 6 && ReadValue<u8>(renderer, 0x662) != 0 &&
           target >= 0x10000 && ReadValue<u8>(target, 0x58) == 1;
}

void DestroyOffscreenModel(u64 renderer) {
    const u64 heap =
        reinterpret_cast<u64 PS4_SYSV_ABI (*)(u64)>(image_base + OwnerHeapOffset)(renderer);
    const u64 vtable = ReadValue<u64>(renderer, 0);
    reinterpret_cast<void PS4_SYSV_ABI (*)(u64)>(ReadValue<u64>(vtable, 0x08))(renderer);
    if (heap >= 0x10000) {
        reinterpret_cast<void PS4_SYSV_ABI (*)(u64, u64)>(
            ReadValue<u64>(ReadValue<u64>(heap, 0), 0x70))(heap, renderer);
    }
}

// One step of the off-screen portrait (TickPortraits, every 250 ms on the game's thread; see
// NextOffscreenAction).
void TickOffscreenPortrait(bool wanted, bool captured, s64 now) {
    OffscreenModel& m = offscreen;
    const u64 global = GetGlobalState();
    const bool in_world =
        global >= 0x10000 && ReadValue<u8>(global, 0x08) == 0 && GetLocalPlayer() >= 0x10000;
    if (m.renderer != 0) {
        const s32 step = ReadValue<s32>(m.renderer, 0x50);
        if (step != m.logged_step) {
            m.logged_step = step;
            LOG_INFO(Debug, "Bloodborne party portraits: off-screen model step {} after {} ms",
                     step, now - m.since);
        }
    }
    const bool drawn = m.renderer != 0 && OffscreenModelDrawn(m.renderer);
    switch (NextOffscreenAction(m.state, wanted, in_world, drawn, captured, now - m.since,
                                now - m.built_at)) {
    case OffscreenAction::Wait:
        return;
    case OffscreenAction::Build:
        m.built_at = now;
        m.since = now;
        m.logged_step = -2;
        m.renderer = BuildOffscreenModel();
        LOG_INFO(Debug, "Bloodborne party portraits: off-screen model {:#x} built (RankingBody)",
                 m.renderer);
        if (m.renderer != 0) {
            m.state = OffscreenState::Building;
        }
        return;
    case OffscreenAction::ReadBack:
        m.state = OffscreenState::Drawn;
        m.since = now;
        offscreen_wanted.store(true);
        LOG_INFO(Debug, "Bloodborne party portraits: off-screen model drawn; reading it back");
        return;
    case OffscreenAction::Close:
        if (m.state == OffscreenState::Building) {
            LOG_INFO(Debug, "Bloodborne party portraits: off-screen model not drawn in time");
        }
        offscreen_wanted.store(false);
        WriteValue<u8>(m.renderer, 0x661, 1);
        m.state = OffscreenState::Closing;
        m.since = now;
        return;
    case OffscreenAction::Destroy:
        offscreen_wanted.store(false);
        DestroyOffscreenModel(m.renderer);
        LOG_INFO(Debug, "Bloodborne party portraits: off-screen model {:#x} destroyed", m.renderer);
        m.state = OffscreenState::Idle;
        m.renderer = 0;
        return;
    }
}

#endif

} // namespace

namespace Coop {

std::string OwnCharacterName() {
    // PlayerGameDataNow is part of the optional party-HUD renderer. Character names are also
    // needed by headless/non-HUD builds, so use the shared game-data accessor here.
    const u64 data = OwnGameData();
    if (data < 0x10000 || !HasMemoryAccess(data + PlayerGameNameHolderAt, 8, MemoryProt::CpuRead)) {
        return {};
    }
    const u64 holder = ReadValue<u64>(data, PlayerGameNameHolderAt);
    if (holder < 0x10000 || !HasMemoryAccess(holder, 8, MemoryProt::CpuRead)) {
        return {};
    }
    // Its name: at +0xC8 when the holder's kind (+0xC) is 1, else at +0x18 (0x01782045).
    const u64 names = ReadValue<u64>(holder, 0);
    if (names < 0x10000 || !HasMemoryAccess(names, 0xC8 + GameHudStringSize, MemoryProt::CpuRead)) {
        return {};
    }
    const u64 at = names + (ReadValue<u32>(names, 0xC) == 1 ? 0xC8 : 0x18);
    std::array<u8, GameHudStringSize> raw{};
    std::memcpy(raw.data(), reinterpret_cast<const void*>(at), raw.size());
    return GameStringUtf8(raw, [](u64 chars, size_t count) {
        std::vector<char16_t> read(count);
        if (chars < 0x10000 || !HasMemoryAccess(chars, count * 2, MemoryProt::CpuRead)) {
            return std::vector<char16_t>{};
        }
        std::memcpy(read.data(), reinterpret_cast<const void*>(chars), count * 2);
        return read;
    });
}

// This character's portrait: the status page's last capture (kept on disk by its face), else the
// one kept for its face; to every room peer at once when it changed or a peer came, and again now
// and then (PortraitSender).
void TickPortraits() {
    static s64 ticked_at = 0;
    static std::optional<PortraitKey> own_key;
    static std::optional<PortraitPixels> own;
    static PortraitSender sender;
    static std::pair<u32, size_t> logged{};
    const s64 now = SteadyMilliseconds();
    if (now - ticked_at < 250) {
        return;
    }
    ticked_at = now;
    own_vials.store(
        Coop::TradeHeld({BloodborneCoop::TradeItemKind::Goods, BloodborneCoop::BloodVialGoods, 0}));
    own_bullets.store(Coop::TradeHeld(
        {BloodborneCoop::TradeItemKind::Goods, BloodborneCoop::QuicksilverBulletGoods, 0}));
    const auto face = OwnFaceFingerprint();
    const auto outfit = face ? OwnOutfit() : std::nullopt;
    const auto key = outfit
                         ? std::optional<PortraitKey>{PortraitKey{.face = *face, .outfit = *outfit}}
                         : std::nullopt;
    std::optional<PortraitPixels> settled;
    {
        std::scoped_lock lock{portraits_mutex};
        settled = face_capture.Settled(now);
    }
    const auto dir = Common::FS::GetUserPath(Common::FS::PathType::UserDir) / "portraits";
    if (key && key != own_key) {
        // Another character or outfit: its own picture, else none until the status page shows it.
        own_key = key;
        own = LoadPortrait(PortraitCachePath(dir, *key));
        LOG_INFO(Debug, "Bloodborne party portraits: outfit {}/{}/{}/{}: {}", key->outfit[0],
                 key->outfit[1], key->outfit[2], key->outfit[3],
                 own ? "the character's portrait from " + PortraitCachePath(dir, *key).string()
                     : std::string{"no portrait yet (the status page takes it)"});
    }
#ifdef BLOODBORNE_PARTY_HUD
    // No portrait for this character and outfit yet: the status page's model, drawn off-screen
    // (the port's texture cache reads it back).
    TickOffscreenPortrait(key.has_value() && !own && !settled, settled.has_value(), now);
#endif
    if (settled) {
        own = std::move(settled);
        const bool kept = key && SavePortrait(PortraitCachePath(dir, *key), *own);
        LOG_INFO(Debug,
                 "Bloodborne party portraits: took the character's portrait from the status page "
                 "({:08x}; {})",
                 PortraitId(*own),
                 kept ? PortraitCachePath(dir, *key).string() : std::string{"not kept: no face"});
    }
    const std::string member = BoardMemberName();
    if (member.empty()) {
        return;
    }
    std::vector<u64> peers;
    for (const auto& peer : Libraries::Np::NpMatching2::GetActiveRoomPeers()) {
        peers.push_back((u64{peer.addr} << 16) | peer.port);
    }
    std::ranges::sort(peers);
    const u32 id = own ? PortraitId(*own) : 0;
    if (!sender.Due(id, peers, now)) {
        return;
    }
    if (ServerWorldOn()) {
        // The server keeps it for the party's games, the ones that come later too.
        std::vector<u8> pixels;
        if (own) {
            for (const auto& part : SplitPortrait(member, *own)) {
                pixels.insert(pixels.end(), part.bytes.begin(), part.bytes.end());
            }
        }
        SendServerPortrait(id, pixels);
    } else if (own) {
        for (const auto& part : SplitPortrait(member, *own)) {
            SendToRoomPeers(EncodeCoopFrame(PortraitPartType, EncodePortraitPart(part)));
        }
    } else {
        SendToRoomPeers(EncodeCoopFrame(PortraitPartType, EncodePortraitNone(member)));
    }
    if (logged != std::pair{id, peers.size()}) {
        logged = {id, peers.size()};
        LOG_INFO(Debug, "Bloodborne party portraits: sent {} ({:08x}) to {} peer(s) as {}",
                 own ? "the character's portrait" : "no portrait", id, peers.size(), member);
    }
}

bool ShowPartyPortrait(int slot, std::string_view member) {
    std::scoped_lock lock{portraits_mutex};
    return party_portraits.Show(slot, member);
}

// The vitals board (see RunVitalsBoardClient): what the game thread posts and what the client
// last heard back, as the member it posts as.
std::mutex vitals_mutex;

} // namespace Coop

namespace {

std::condition_variable_any vitals_wake;
MemberVitals vitals_mine;
std::vector<ReviveHitTotal> vitals_hits;
bool vitals_changed{};
// This member's cutscene vote (CutsceneVote::Own), posted with its vitals.
std::atomic<u32> own_cutscene{};

} // namespace

namespace Coop {

std::string vitals_member;

} // namespace Coop

namespace {

std::string BoardMemberName() {
    std::scoped_lock lock{vitals_mutex};
    return vitals_member;
}

// The net sync object (SummonSessionRulesPointerOffset, see MemberDataWait): +0x84 is 4 in a
// session, +0x68 the list of members with a remote player slot (head node; node: next +0, member
// id +0x10, slot +0x30). The slot's byte of received parts is GameDataMan [+0x18][slot].
std::optional<s32> MemberDataSlot(u64 net, u64 member_id) {
    if (net < 0x10000 || !HasMemoryAccess(net, 0x88, MemoryProt::CpuRead)) {
        return std::nullopt;
    }
    using IdEqual = bool PS4_SYSV_ABI (*)(u64 a, u64 b);
    const auto id_equal = reinterpret_cast<IdEqual>(image_base + MemberIdEqualOffset);
    const u64 head = ReadValue<u64>(net, 0x68);
    if (head < 0x10000 || !HasMemoryAccess(head, 8, MemoryProt::CpuRead)) {
        return std::nullopt;
    }
    u64 node = ReadValue<u64>(head, 0);
    for (s32 hops = 0; node >= 0x10000 && node != head && hops < 16; ++hops) {
        if (!HasMemoryAccess(node, 0x34, MemoryProt::CpuRead)) {
            break;
        }
        if (id_equal(node + 0x10, member_id)) {
            const s32 slot = ReadValue<s32>(node, 0x30);
            return slot >= 0 && slot < 4 ? std::optional{slot} : std::nullopt;
        }
        node = ReadValue<u64>(node, 0);
    }
    return std::nullopt;
}

u64 MemberDataPartsAddress(s32 slot) {
    const u64 data = ReadValue<u64>(image_base + GameDataManPointerOffset, 0);
    const u64 parts = data >= 0x10000 && HasMemoryAccess(data, 0x20, MemoryProt::CpuRead)
                          ? ReadValue<u64>(data, 0x18)
                          : 0;
    return parts >= 0x10000 && HasMemoryAccess(parts + slot, 1, MemoryProt::CpuWrite) ? parts + slot
                                                                                      : 0;
}

// The game's own request (0x01781A70 sends the same): one zero byte on channel 0x0A, which the
// member's game answers with all of its data.
void SendMemberDataRequest(u64 net, u64 member_id) {
    using Send =
        s32 PS4_SYSV_ABI (*)(u64 net, u64 member_id, u32 channel, const void* buffer, u32 size);
    const u8 zero = 0;
    reinterpret_cast<Send>(image_base + NetSyncSendOffset)(net, member_id, MemberDataRequestChannel,
                                                           &zero, 1);
}

} // namespace

namespace Coop {

// See joins_waiting_for_data: once another member has stood on this map a moment, the waiting
// members are asked for their data until all of it is in, then their characters are created.
void FinishWaitingJoins(bool in_session) {
    if (joins_waiting_for_data.empty()) {
        return;
    }
    if (!in_session) {
        joins_waiting_for_data.clear();
        return;
    }
    const VitalsView view = ReadVitalsBoard();
    const bool here = view.Fresh() && !view.member.empty() &&
                      SomeoneElseStands(view.board.members, view.member, PlayingMap());
    const s64 now = SteadyMilliseconds();
    const u64 net = ReadValue<u64>(image_base + SummonSessionRulesPointerOffset, 0);
    using SetStep = void PS4_SYSV_ABI (*)(u64 task, s32 step);
    std::vector<u64> done;
    ForEachJoinTask([&](u64 task) {
        const u64 identity = ReadValue<u64>(task, 0xC8);
        const auto waiting = joins_waiting_for_data.find(identity);
        if (waiting == joins_waiting_for_data.end()) {
            return;
        }
        const s32 step = ReadValue<s32>(task, 0x50);
        const u8 flags = ReadValue<u8>(task, 0xED);
        if (step > CreateCharacterStep) {
            done.push_back(identity);
            return;
        }
        const u64 member_id = task + 0xC8;
        const std::optional<s32> slot = MemberDataSlot(net, member_id);
        const u64 parts = slot ? MemberDataPartsAddress(*slot) : 0;
        switch (waiting->second.Tick(now, here, parts ? ReadValue<u8>(parts, 0) : 0)) {
        case MemberDataWait::Action::None:
            return;
        case MemberDataWait::Action::Request:
            if (parts) {
                WriteValue(parts, 0, static_cast<u8>(ReadValue<u8>(parts, 0) & ~MemberDataParts));
            }
            SendMemberDataRequest(net, member_id);
            LOG_INFO(Debug,
                     "Bloodborne party trip: asked a member still joining for its data (request "
                     "{}, task {:#x} step={} flags={:#x} slot={})",
                     waiting->second.Requests(), task, step, flags,
                     slot ? fmt::format("{}", *slot) : "none");
            return;
        case MemberDataWait::Action::Create:
            WriteValue(task, 0xED, static_cast<u8>(flags | 1));
            if (step < CreateCharacterStep) {
                reinterpret_cast<SetStep>(ReadValue<u64>(ReadValue<u64>(task, 0), 0x28))(
                    task, CreateCharacterStep);
            }
            done.push_back(identity);
            LOG_INFO(Debug,
                     "Bloodborne party trip: a member that was still joining sent its data; "
                     "creating its character (task {:#x} step={} flags={:#x} slot={})",
                     task, step, flags, slot.value_or(-1));
            return;
        }
    });
    for (const u64 identity : done) {
        joins_waiting_for_data.erase(identity);
    }
}

} // namespace Coop

namespace {

// See OwnDataWatch: the stats and face channel 8 carries, PlayerGameData ([GameDataMan +0x08],
// the remote slots at [GameDataMan +0x10] + 0x6A0 each have the same layout) +0x40..+0x90 (every
// 8 bytes), +0xD4, +0xE0, +0xE4 and the face (0xF0 bytes at +0x4AC).
std::optional<u64> MemberDataFingerprint(u64 data) {
    if (data < 0x10000 || !HasMemoryAccess(data, 0x5A0, MemoryProt::CpuRead)) {
        return std::nullopt;
    }
    u64 hash = 0xCBF29CE484222325ull;
    const auto mix = [&](u64 at, u64 size) {
        for (u64 index = 0; index < size; ++index) {
            hash = (hash ^ ReadValue<u8>(data, at + index)) * 0x100000001B3ull;
        }
    };
    for (u64 at = 0x40; at <= 0x90; at += 8) {
        mix(at, 4);
    }
    mix(0xD4, 4);
    mix(0xE0, 8);
    mix(0x4AC, 0xF0);
    return hash;
}

} // namespace

namespace Coop {

u64 OwnGameData() {
    const u64 data = ReadValue<u64>(image_base + GameDataManPointerOffset, 0);
    return data >= 0x10000 && HasMemoryAccess(data, 0x10, MemoryProt::CpuRead)
               ? ReadValue<u64>(data, 0x08)
               : 0;
}

} // namespace Coop

namespace {

std::optional<u64> OwnDataFingerprint() {
    return MemberDataFingerprint(OwnGameData());
}

} // namespace

namespace Coop {

// The remote player slots' stats and face, logged when they change in a session (a member's
// level up seen here; the rig's raise-stat).
void LogMemberDataChanges(bool in_session) {
    static std::array<std::optional<u64>, 4> seen;
    const u64 data = ReadValue<u64>(image_base + GameDataManPointerOffset, 0);
    const u64 slots = data >= 0x10000 && HasMemoryAccess(data, 0x20, MemoryProt::CpuRead)
                          ? ReadValue<u64>(data, 0x10)
                          : 0;
    for (s32 slot = 0; slot < 4; ++slot) {
        const auto now = in_session && slots >= 0x10000
                             ? MemberDataFingerprint(slots + static_cast<u64>(slot) * 0x6A0)
                             : std::nullopt;
        if (now && seen[slot] && *now != *seen[slot]) {
            LOG_INFO(Debug, "Bloodborne party: member slot {} stats or face changed (first {})",
                     slot, ReadValue<s32>(slots + static_cast<u64>(slot) * 0x6A0, 0x40));
        }
        seen[slot] = now;
    }
}

// A level up (or any change to the stats or face) in a session reaches the others: the game's
// own after-load broadcast of all of the player's data (BroadcastMemberDataOffset).
void ResendOwnDataOnChange(bool in_session) {
    static OwnDataWatch watch;
    const u64 net = ReadValue<u64>(image_base + SummonSessionRulesPointerOffset, 0);
    const u64 state = GetGlobalState();
    const bool can_send =
        in_session && net >= 0x10000 && HasMemoryAccess(net, 0x88, MemoryProt::CpuRead) &&
        ReadValue<s32>(net, 0x84) == 4 && ReadValue<u64>(net, 0x10) != 0 && state >= 0x10000 &&
        HasMemoryAccess(state, 0x09, MemoryProt::CpuRead) && ReadValue<u8>(state, 0x08) == 0;
    if (watch.Observe(can_send ? OwnDataFingerprint() : std::nullopt, SteadyMilliseconds())) {
        reinterpret_cast<void PS4_SYSV_ABI (*)()>(image_base + BroadcastMemberDataOffset)();
        LOG_INFO(Debug, "Bloodborne party: the player's stats or face changed; sent its data to "
                        "the session again");
    }
}

// This member's cutscene vote: posted with its vitals at once when it changes (any thread: the
// pad's).
void NoteCutsceneVote(u32 state) {
    if (own_cutscene.exchange(state) == state) {
        return;
    }
    std::scoped_lock lock{vitals_mutex};
    vitals_changed = true;
    vitals_wake.notify_all();
}

// Hands the client this member's vitals: posted at once when more than its position changed.
void PostVitals(const MemberVitals& mine, std::vector<ReviveHitTotal> hits) {
    // On change only (the rig checks SessionArrival by it).
    static VitalsPostLog post_log;
    if (const auto line = post_log.Next(mine.vital, mine.map)) {
        LOG_INFO(Debug, "Bloodborne party vitals: {}", *line);
    }
    std::scoped_lock lock{vitals_mutex};
    MemberVitals moved = vitals_mine;
    moved.at = mine.at;
    const bool changed = moved != mine || hits != vitals_hits;
    vitals_mine = mine;
    vitals_hits = std::move(hits);
    if (changed) {
        vitals_changed = true;
        vitals_wake.notify_all();
    }
    // Straight to the room's peers too (see DirectVitalsType), once the board named this member.
    static s64 sent_at = 0;
    const s64 now = SteadyMilliseconds();
    if (mine.vital != Vital::Off && !vitals_member.empty() &&
        (changed || now - sent_at >= DirectDamageResendMs)) {
        sent_at = now;
        MemberVitals self = mine;
        self.member = vitals_member;
        const std::string line = EncodeVitalsUpdate(self, vitals_hits);
        SendToRoomPeers(EncodeCoopFrame(
            DirectVitalsType,
            std::span<const u8>{reinterpret_cast<const u8*>(line.data()), line.size()}));
    }
}

} // namespace Coop

namespace {

// The boards over the party's world on the server (phase 3c): this member's map every second, its
// rest while it rests (and once when it stands up), its kill echoes and the party's trophies every
// three seconds, and its vitals as the vitals client posted them (soon after a change, four times
// a second while someone is down, else every second). The server sends the boards back as they
// change (TakeServer*).
void RunServerBoards(std::stop_token stop) {
    const std::string member = FindBoardMember(stop);
    {
        std::scoped_lock lock{vitals_mutex};
        vitals_member = member;
    }
    LOG_INFO(Debug, "Bloodborne party boards on the server's world as member {}", member);
    StartServerWorld();
    using Clock = std::chrono::steady_clock;
    auto next_second = Clock::now();
    auto next_echoes = Clock::now();
    auto next_vitals = Clock::now();
    auto rounded_at = Clock::now();
    s32 posted_rest = NotResting;
    bool posted_on = false;
    u32 posted_map = 0;
    while (!stop.stop_requested()) {
        bool someone_down = false;
        MemberVitals mine;
        std::vector<ReviveHitTotal> hits;
        bool vitals_due = false;
        {
            std::unique_lock lock{vitals_mutex};
            // At least 100 ms apart, then until a change or the next round is due.
            vitals_wake.wait_until(lock, stop, rounded_at + std::chrono::milliseconds{100},
                                   [] { return false; });
            vitals_wake.wait_until(lock, stop, std::min(next_vitals, next_second),
                                   [] { return vitals_changed; });
            rounded_at = Clock::now();
            someone_down = std::ranges::any_of(vitals_board.members, [](const MemberVitals& other) {
                return other.vital == Vital::Down;
            });
            vitals_due = vitals_changed || Clock::now() >= next_vitals;
            if (vitals_due) {
                vitals_changed = false;
                mine = vitals_mine;
                mine.cutscene = own_cutscene.load();
                mine.vials = own_vials.load();
                mine.lamp_uses = lamp_uses.load();
                mine.bullets = own_bullets.load();
                hits = vitals_hits;
            }
        }
        if (stop.stop_requested()) {
            break;
        }
        const auto now = Clock::now();
        if (vitals_due) {
            next_vitals = now + std::chrono::milliseconds{someone_down ? 250 : 1'000};
            if (mine.vital != Vital::Off || posted_on) {
                SendServerVitals(mine, hits);
                posted_on = mine.vital != Vital::Off;
            }
        }
        // With phase 4's owners a load (0) goes too: the area is handed on at once. A new map goes
        // at once as well: shadNet routes the game's packets by it (phase 9).
        const u32 map = party_map_now.load();
        const bool map_wanted = map != 0 || EnemyOwnerOn();
        if (map_wanted && map != posted_map) {
            SendServerMap(map);
            posted_map = map;
        }
        if (now < next_second) {
            continue;
        }
        next_second = now + std::chrono::seconds{1};
        if (map_wanted) {
            SendServerMap(map);
            posted_map = map;
        }
        const bool active = rest_board_active.load();
        const s32 lamp = active ? rest_board_lamp.load() : NotResting;
        if (active || posted_rest != NotResting) {
            std::optional<LampPosition> at;
            {
                std::scoped_lock lock{rest_board_position_mutex};
                at = rest_board_position;
            }
            SendServerRest(lamp, at);
            posted_rest = lamp;
        }
        if (now < next_echoes) {
            continue;
        }
        next_echoes = now + std::chrono::seconds{3};
        std::vector<KillEchoes> echoes;
        {
            std::scoped_lock lock{echo_board_mutex};
            echoes.swap(echoes_to_post);
        }
        if (!echoes.empty()) {
            SendServerEchoes(echoes);
        }
        if (trophy_board_active.load()) {
            std::vector<s32> earned;
            {
                std::scoped_lock lock{trophy_mutex};
                earned = trophies_earned_in_party;
            }
            SendServerTrophies(earned);
        }
    }
}
std::jthread server_boards_client;

// The board clients use state that lives in the other co-op files (flags, kills, HP, the trace
// queue), and the order the files' statics are destroyed in at exit() is not defined across
// files. So they are stopped and joined before any static destructor runs: by an atexit handler
// registered once the first client starts, after all static initialization (handlers registered
// later run earlier). The window's close ends the process with _Exit and runs neither; the port's
// guest exit() and STOP paths run both.
void StopBoardClientsAtExit() {
    static std::once_flag registered;
    std::call_once(registered, [] {
        std::atexit([] {
            server_boards_client.request_stop();
            if (server_boards_client.joinable()) {
                server_boards_client.join();
            }
        });
    });
}

} // namespace

namespace Coop {

void StartVitalsBoardClient() {
    const char* party_setting = std::getenv("SHADPS4_BLOODBORNE_PARTY");
    const auto party = party_setting ? ParsePartySetting(party_setting) : std::nullopt;
    const std::string base_url = EmulatorSettings.GetShadNetWebApiServer();
    if (!party_controller || (!party && !TeamModeOn()) || !EmulatorSettings.IsShadNetEnabled() ||
        base_url.empty()) {
        return;
    }
    // Every board goes over the party's world on the server (one client for all of them); there are
    // no boards without it (the HTTP boards went with phase 3c).
    if (!ServerWorldOn()) {
        LOG_WARNING(Debug, "Bloodborne party boards: none (no party world on the server: the "
                           "server link is off)");
        return;
    }
    server_boards_client = std::jthread(RunServerBoards);
    StopBoardClientsAtExit();
}

void TakeServerMapBoard(std::vector<MemberMap> maps) {
    std::string member;
    {
        std::scoped_lock lock{vitals_mutex};
        member = vitals_member;
    }
    std::scoped_lock lock{map_board_mutex};
    map_board = std::move(maps);
    map_board_member = std::move(member);
    map_board_time = std::chrono::steady_clock::now();
}

void TakeServerRestBoard(std::vector<LampRest> rests) {
    std::scoped_lock lock{rest_board_mutex};
    rest_board = std::move(rests);
    rest_board_time = std::chrono::steady_clock::now();
}

void TakeServerEchoes(std::vector<KillEchoes> echoes) {
    std::scoped_lock lock{echo_board_mutex};
    echo_ledger.Received(echoes, SteadyMilliseconds());
}

void TakeServerVitals(VitalsBoard board) {
    // Invasions: the invader's line is no teammate's: everything that reads the board (the panel,
    // downs, revives, the cutscene vote, the boss runner) goes without it; presence alone keeps
    // its body shown.
    std::vector<MemberVitals> hostile = TakeHostileLines(board);
    std::scoped_lock lock{vitals_mutex};
    vitals_board = std::move(board);
    hostile_vitals = std::move(hostile);
    vitals_heard_at = std::chrono::steady_clock::now();
}

} // namespace Coop

bool OffscreenPortraitWanted() {
    return offscreen_wanted.load(std::memory_order_relaxed);
}

void NoteStatusModel(std::span<const u8> rgba, int width, int height) {
    // Only the off-screen model's (the face camera's): the status page's own is the whole body.
    if (!offscreen_wanted.load()) {
        return;
    }
    auto portrait = FacePortraitFromModel(rgba, width, height);
    if (!portrait) {
        return;
    }
    std::scoped_lock lock{portraits_mutex};
    face_capture.Take(std::move(*portrait), SteadyMilliseconds());
}

bool ReadPortraitSlot(int slot, std::span<u8> rgba, u32& version) {
    std::scoped_lock lock{portraits_mutex};
    version = party_portraits.Version(slot);
    const auto pixels = party_portraits.Pixels(slot);
    if (!pixels || rgba.size() < pixels->size()) {
        return false;
    }
    std::ranges::copy(CircularPortrait(*pixels), rgba.begin());
    return true;
}

bool ReadMemberPortrait(std::string_view member, std::span<u8> rgba, u32& id) {
    std::scoped_lock lock{portraits_mutex};
    id = party_portraits.IdOf(member);
    const auto pixels = party_portraits.PixelsOf(member);
    if (!pixels || rgba.size() < pixels->size()) {
        return false;
    }
    std::ranges::copy(CircularPortrait(*pixels), rgba.begin());
    return true;
}

u32 PortraitSlotVersion(int slot) {
    std::scoped_lock lock{portraits_mutex};
    return party_portraits.Version(slot);
}

} // namespace Core::Bloodborne
