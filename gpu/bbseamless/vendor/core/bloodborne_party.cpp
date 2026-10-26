// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <array>
#include <bit>
#include <cctype>
#include <charconv>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

#include <fmt/format.h>

#include "core/bloodborne_flags.h"
#include "core/bloodborne_lamp_points.h"
#include "core/bloodborne_party.h"

namespace Core::Bloodborne {

namespace {

// Must match shadNet's NormalizePartyKey so both sides agree on what a party is.
constexpr std::size_t MaxPartyKeySize = 128;

bool IsWhitespace(char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\v' || c == '\f';
}

} // namespace

std::optional<std::string> ParsePartySetting(std::string_view setting) {
    while (!setting.empty() && IsWhitespace(setting.front())) {
        setting.remove_prefix(1);
    }
    while (!setting.empty() && IsWhitespace(setting.back())) {
        setting.remove_suffix(1);
    }
    if (setting.empty() || setting.size() > MaxPartyKeySize) {
        return std::nullopt;
    }
    for (const char c : setting) {
        if (static_cast<unsigned char>(c) < 0x20 || c == 0x7F) {
            return std::nullopt;
        }
    }
    const std::size_t colon = setting.find(':');
    if (colon == std::string_view::npos || colon == 0 || colon == setting.size() - 1) {
        return std::nullopt;
    }
    return std::string{setting};
}

bool StartSummonSearch(u64 player, s32 effect_id, AddSpEffectFn add_sp_effect) {
    if (player == 0 || add_sp_effect == nullptr) {
        return false;
    }
    add_sp_effect(player, effect_id, player, 0, 1, 0, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 0);
    return true;
}

bool ShouldEndSessionOnWorldTeardown(s32 matching_state, s32 reload_state, bool reload_pending) {
    const bool in_session =
        matching_state == MatchingStateSummonedGuest || matching_state == MatchingStateHosting;
    return in_session && reload_state != SummonReloadInProgress && !reload_pending;
}

std::vector<u8> HexBytes(std::string_view hex) {
    std::vector<u8> bytes;
    for (size_t at = 0; at + 1 < hex.size(); at += 3) {
        u8 value = 0;
        if (hex.substr(at, 2) != "??") {
            std::from_chars(hex.data() + at, hex.data() + at + 2, value, 16);
        }
        bytes.push_back(value);
    }
    return bytes;
}

bool MatchesHexPattern(std::span<const u8> bytes, std::string_view pattern) {
    const std::vector<u8> expected = HexBytes(pattern);
    if (bytes.size() != expected.size()) {
        return false;
    }
    for (size_t index = 0; index < expected.size(); ++index) {
        if (pattern.substr(index * 3, 2) != "??" && bytes[index] != expected[index]) {
            return false;
        }
    }
    return true;
}

std::vector<std::string_view> AcceptedVariants(const CoopSite& site) {
    std::vector<std::string_view> variants;
    for (std::string_view rest = site.accepted; !rest.empty();) {
        const size_t bar = rest.find('|');
        variants.push_back(rest.substr(0, bar));
        rest = bar == std::string_view::npos ? std::string_view{} : rest.substr(bar + 1);
    }
    return variants;
}

bool MatchesCoopSite(std::span<const u8> bytes, const CoopSite& site) {
    return MatchesHexPattern(bytes, site.original) ||
           std::ranges::any_of(AcceptedVariants(site), [bytes](std::string_view variant) {
               return MatchesHexPattern(bytes, variant);
           });
}

const CoopFeature* FindCoopFeature(std::string_view name) {
    const auto found = std::ranges::find_if(
        CoopFeatures, [name](const CoopFeature& feature) { return feature.name == name; });
    return found == CoopFeatures.end() ? nullptr : &*found;
}

std::vector<CodePatch> CoopPatchesOf(std::string_view feature) {
    std::vector<CodePatch> patches;
    for (const CoopSite& site : CoopSites) {
        if (site.feature == feature && site.kind == CoopSiteKind::Patch) {
            patches.push_back({site.offset, HexBytes(site.original), HexBytes(site.patched)});
        }
    }
    return patches;
}

const CoopSite* FirstMismatchedCoopSite(std::span<const u8> image, std::string_view feature) {
    for (const CoopSite& site : CoopSites) {
        if (site.feature != feature) {
            continue;
        }
        const size_t size = HexBytes(site.original).size();
        if (site.offset > image.size() || size > image.size() - site.offset ||
            !MatchesCoopSite(image.subspan(site.offset, size), site)) {
            return &site;
        }
    }
    return nullptr;
}

bool IsRegisteredCoopHook(u64 offset, std::span<const u8> bytes) {
    return std::ranges::any_of(CoopSites, [offset, bytes](const CoopSite& site) {
        return site.kind == CoopSiteKind::Hook && site.offset == offset &&
               std::ranges::equal(HexBytes(site.original), bytes);
    });
}

std::string CoopSitesJson() {
    const auto quoted = [](std::string_view text) {
        std::string out = "\"";
        for (const char c : text) {
            if (c == '"' || c == '\\') {
                out += '\\';
            }
            out += c;
        }
        return out + '"';
    };
    std::string json = "{\n  \"generated_from\": \"emulator/src/core/bloodborne_patch_sites.h\",\n"
                       "  \"features\": [\n";
    for (size_t index = 0; index < CoopFeatures.size(); ++index) {
        const CoopFeature& feature = CoopFeatures[index];
        json += fmt::format("    {{\"name\": {}, \"required\": {}}}{}\n", quoted(feature.name),
                            feature.required ? "true" : "false",
                            index + 1 < CoopFeatures.size() ? "," : "");
    }
    json += "  ],\n  \"sites\": [\n";
    for (size_t index = 0; index < CoopSites.size(); ++index) {
        const CoopSite& site = CoopSites[index];
        const char* kind = site.kind == CoopSiteKind::Patch  ? "patch"
                           : site.kind == CoopSiteKind::Hook ? "hook"
                                                             : "call";
        std::string accepted;
        for (const std::string_view variant : AcceptedVariants(site)) {
            accepted += (accepted.empty() ? "" : ", ") + quoted(variant);
        }
        json += fmt::format("    {{\"name\": {}, \"feature\": {}, \"kind\": \"{}\", "
                            "\"offset\": \"0x{:08X}\", \"original\": {}, \"patched\": {}, "
                            "\"accepted\": [{}]}}{}\n",
                            quoted(site.name), quoted(site.feature), kind, site.offset,
                            quoted(site.original), quoted(site.patched), accepted,
                            index + 1 < CoopSites.size() ? "," : "");
    }
    return json + "  ]\n}\n";
}

void CoopFeatureReport::Set(std::string_view feature, std::optional<std::string> skipped) {
    const auto found = std::ranges::find_if(
        results, [feature](const Result& result) { return result.feature == feature; });
    if (found != results.end()) {
        found->skipped = std::move(skipped);
    } else {
        results.push_back({std::string{feature}, std::move(skipped)});
    }
}

void CoopFeatureReport::On(std::string_view feature) {
    Set(feature, std::nullopt);
}

void CoopFeatureReport::Skipped(std::string_view feature, std::string reason) {
    Set(feature, std::move(reason));
}

std::string CoopFeatureReport::Summary() const {
    std::string skipped;
    size_t on = 0;
    for (const Result& result : results) {
        if (!result.skipped) {
            ++on;
            continue;
        }
        skipped +=
            fmt::format("{}{} [{}]", skipped.empty() ? "" : ", ", result.feature, *result.skipped);
    }
    std::string summary = fmt::format("Co-op features: {}/{} on", on, results.size());
    if (!skipped.empty()) {
        summary += fmt::format(" (skipped: {})", skipped);
    }
    return summary;
}

std::vector<std::string> CoopFeatureReport::SkippedRequired() const {
    std::vector<std::string> required;
    for (const Result& result : results) {
        const CoopFeature* feature = FindCoopFeature(result.feature);
        if (result.skipped && feature != nullptr && feature->required) {
            required.push_back(result.feature);
        }
    }
    return required;
}

const std::vector<CodePatch>& GhostDeathKeepSessionPatches() {
    static const std::vector<CodePatch> patches = CoopPatchesOf("ghost death");
    return patches;
}

const std::vector<CodePatch>& GuestBloodstainPatches() {
    static const std::vector<CodePatch> patches = CoopPatchesOf("guest bloodstain");
    return patches;
}

const std::vector<CodePatch>& NoEchoTheftPatches() {
    static const std::vector<CodePatch> patches = CoopPatchesOf("no echo theft");
    return patches;
}

const std::vector<CodePatch>& HostDeathKeepGuestsPatches() {
    static const std::vector<CodePatch> patches = CoopPatchesOf("host death");
    return patches;
}

const std::vector<CodePatch>& BossClearKeepSessionPatches() {
    static const std::vector<CodePatch> patches = CoopPatchesOf("boss clear");
    return patches;
}

bool StartsTripOnWorldReload(u64 caller_offset, bool trip_active, bool in_party_session) {
    return IsWorldReloadStopRequest(caller_offset) && !trip_active && in_party_session;
}

bool IsWorldReloadStopRequest(u64 caller_offset) {
    constexpr u64 WorldLoadStopRequestReturn = 0x0193C65C;
    return caller_offset == WorldLoadStopRequestReturn;
}

bool IsWorldReloadLeaveSession(u64 caller_offset) {
    constexpr u64 WorldLoadLeaveSessionReturn = 0x0193C68E;
    return caller_offset == WorldLoadLeaveSessionReturn;
}

const std::vector<CodePatch>& JoinAnywherePatches() {
    static const std::vector<CodePatch> patches = CoopPatchesOf("join anywhere");
    return patches;
}

namespace {
u64 DecodeRel32(u8 opcode, u64 offset, std::span<const u8> bytes) {
    if (bytes.size() != 5 || bytes[0] != opcode) {
        return 0;
    }
    s32 displacement;
    std::memcpy(&displacement, bytes.data() + 1, sizeof(displacement));
    return offset + bytes.size() + static_cast<s64>(displacement);
}
} // namespace

u64 DecodeNearCallTarget(u64 offset, std::span<const u8> bytes) {
    return DecodeRel32(0xE8, offset, bytes);
}

u64 DecodeNearJumpTarget(u64 offset, std::span<const u8> bytes) {
    return DecodeRel32(0xE9, offset, bytes);
}

namespace {

bool HasReviveCalls(const GhostDeathCalls& calls) {
    return calls.set_event_flag && calls.clear_return_state && calls.request_full_recover &&
           calls.update_blood_mark && calls.revive_player && calls.revive_local_character &&
           calls.restore_alive_state && calls.chr_reset_animation && calls.warp_to_last_lamp;
}

// What both deaths share. `character_type` is set back after the local revive makes the player
// a live one (only a guest needs that).
void ReviveInPlace(const GhostDeathCalls& calls, std::optional<s32> character_type) {
    calls.set_event_flag(0, DeathEventFlag, false);
    calls.clear_return_state();
    calls.request_full_recover();
    calls.update_blood_mark();
    calls.revive_player();
    calls.revive_local_character();
    calls.restore_alive_state();
    if (character_type) {
        calls.set_chr_type(0, LocalPlayerEntity, *character_type);
    }
    calls.chr_reset_animation(0, LocalPlayerEntity);
    calls.warp_to_last_lamp();
}

} // namespace

const LampPoint* FindLampPoint(s32 warp_point) {
    const auto found = std::ranges::lower_bound(LampPoints, warp_point, {}, &LampPoint::warp_point);
    return found != LampPoints.end() && found->warp_point == warp_point ? &*found : nullptr;
}

bool ArrivalNeedsMove(const LampPoint* planned, const std::optional<LampPosition>& at) {
    if (planned == nullptr || !at) {
        return true;
    }
    const float dx = (*at)[0] - planned->x;
    const float dy = (*at)[1] - planned->y;
    const float dz = (*at)[2] - planned->z;
    return dx * dx + dy * dy + dz * dz > ArrivedAtLampMeters * ArrivedAtLampMeters;
}

bool PlaceCharacter(const PlacementCalls& calls, u64 character, const float* position,
                    const float* rotation) {
    if (!calls.teleport || !calls.snap_position || !calls.set_rotation || !calls.reset_follow ||
        !calls.snap_camera) {
        return false;
    }
    calls.teleport(character, position, rotation, 1);
    calls.snap_position(character, position);
    calls.set_rotation(character, rotation);
    calls.reset_follow(character, true);
    calls.snap_camera();
    return true;
}

bool ReviveDeadPartyGuest(const GhostDeathCalls& calls) {
    if (!HasReviveCalls(calls) || !calls.is_white_ghost || !calls.set_chr_type ||
        !calls.is_white_ghost()) {
        return false;
    }
    ReviveInPlace(calls, WhiteGhostCharacterType);
    return true;
}

bool ShouldReviveHostInPlace(s32 matching_state) {
    return matching_state == MatchingStateHosting || matching_state == MatchingStateSummonedGuest;
}

void ReviveDeadPartyHost(const GhostDeathCalls& calls) {
    if (HasReviveCalls(calls)) {
        ReviveInPlace(calls, std::nullopt);
    }
}

std::array<u8, 7> EncodeSignTimerStore(u8 modrm, float seconds) {
    const auto value = std::bit_cast<u32>(seconds);
    return {0xC7,
            modrm,
            0x28,
            static_cast<u8>(value),
            static_cast<u8>(value >> 8),
            static_cast<u8>(value >> 16),
            static_cast<u8>(value >> 24)};
}

bool IsSignTimerStore(std::span<const u8> bytes, const SignTimerSite& site) {
    const auto store = EncodeSignTimerStore(site.modrm, site.original_seconds);
    constexpr size_t TimeStart = 3;
    return bytes.size() >= store.size() &&
           std::ranges::equal(bytes.first(TimeStart), std::span{store}.first(TimeStart));
}

bool IsLampTravelFlag(s32 flag) {
    // 7MMM01LL: map code MMM (241 = m24_01), lamp LL (see FlagTable).
    return RuleFor(flag).Has(FlagLampTrip);
}

bool LampRestsEnabled(const char* setting) {
    return setting != nullptr && std::string_view{setting} == "1";
}

bool WaitsForMemberData(s32 kind, s32 step) {
    return (kind == RemoteHostTask || kind == RemoteGuestTask) && step >= 0 &&
           step < CreateCharacterStep;
}

bool ShouldRecreateAfterTrip(s32 kind, s32 step) {
    return (kind == RemoteHostTask || kind == RemoteGuestTask) && step == InSessionStep;
}

std::string EncodeMapUpdate(std::string_view member, u32 map) {
    std::array<char, 8> hex{};
    const auto [end, error] = std::to_chars(hex.data(), hex.data() + hex.size(), map, 16);
    return std::string{member} + ' ' + std::string{hex.data(), end};
}

std::optional<std::vector<MemberMap>> ParseMapBoard(std::string_view body) {
    std::vector<MemberMap> board;
    while (!body.empty()) {
        const auto end = body.find('\n');
        const std::string_view line = body.substr(0, end);
        body = end == std::string_view::npos ? std::string_view{} : body.substr(end + 1);
        if (line.empty()) {
            continue;
        }
        const auto space = line.find(' ');
        if (space == std::string_view::npos || space == 0) {
            return std::nullopt;
        }
        const std::string_view map_text = line.substr(space + 1);
        u32 map = 0;
        const auto [parsed_end, error] =
            std::from_chars(map_text.data(), map_text.data() + map_text.size(), map, 16);
        if (error != std::errc{} || parsed_end != map_text.data() + map_text.size()) {
            return std::nullopt;
        }
        board.push_back(MemberMap{std::string{line.substr(0, space)}, map});
    }
    return board;
}

bool EveryoneElseOnMap(std::span<const MemberMap> board, std::string_view self, u32 map) {
    bool anyone_else = false;
    for (const MemberMap& entry : board) {
        if (entry.member == self) {
            continue;
        }
        if (entry.map != map) {
            return false;
        }
        anyone_else = true;
    }
    return anyone_else;
}

bool NoOneElseOnMap(std::span<const MemberMap> board, std::string_view self, u32 map) {
    if (map == 0) {
        return false;
    }
    return std::ranges::none_of(
        board, [&](const MemberMap& entry) { return entry.member != self && entry.map == map; });
}

namespace {
bool IsOnlineIdChar(u8 c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '-' ||
           c == '_';
}
} // namespace

std::string BoardMemberOfOnlineId(std::string_view online_id) {
    std::string member;
    for (const char c : online_id) {
        member += fmt::format("{:02x}", static_cast<u8>(c));
    }
    return member;
}

std::string OnlineIdOfBoardMember(std::string_view member) {
    const auto nibble = [](char c) -> s32 {
        if (c >= '0' && c <= '9') {
            return c - '0';
        }
        if (c >= 'a' && c <= 'f') {
            return c - 'a' + 10;
        }
        return -1;
    };
    if (member.empty() || member.size() % 2 != 0) {
        return {};
    }
    std::string online_id;
    for (size_t index = 0; index < member.size(); index += 2) {
        const s32 high = nibble(member[index]);
        const s32 low = nibble(member[index + 1]);
        if (high < 0 || low < 0 || !IsOnlineIdChar(static_cast<u8>(high * 16 + low))) {
            return {};
        }
        online_id += static_cast<char>(high * 16 + low);
    }
    return online_id;
}

std::optional<std::string> FindOnlineIdIn(std::span<const u8> bytes,
                                          std::span<const std::string> candidates) {
    std::optional<std::string> found;
    for (const std::string& candidate : candidates) {
        if (candidate.empty() || candidate.size() >= bytes.size()) {
            continue;
        }
        for (size_t at = 0; at + candidate.size() < bytes.size(); ++at) {
            if (!std::equal(candidate.begin(), candidate.end(), bytes.begin() + at) ||
                bytes[at + candidate.size()] != 0 || (at > 0 && IsOnlineIdChar(bytes[at - 1]))) {
                continue;
            }
            if (found && *found != candidate) {
                return std::nullopt; // two members' names: not this member's object
            }
            found = candidate;
            break;
        }
    }
    return found;
}

RecreatePlan PlanRecreate(std::span<const std::optional<std::string>> found,
                          std::span<const MemberMap> board, std::string_view self, u32 map,
                          const std::set<std::string>& handled) {
    RecreatePlan plan;
    std::vector<std::optional<std::string>> members{found.begin(), found.end()};
    for (auto& member : members) {
        if (member && std::ranges::count(found, member) > 1) {
            member.reset();
        }
    }
    const auto others_on_board = static_cast<size_t>(
        std::ranges::count_if(board, [&](const MemberMap& entry) { return entry.member != self; }));
    if (std::ranges::any_of(members, [](const auto& member) { return !member.has_value(); })) {
        // Everyone else in the session, not only those on the board: one off it is elsewhere.
        if (others_on_board >= members.size() && EveryoneElseOnMap(board, self, map)) {
            for (size_t index = 0; index < members.size(); ++index) {
                plan.now.push_back(index);
            }
            plan.done = true;
        }
        return plan;
    }
    plan.done = true;
    std::vector<size_t> here;
    bool someone_new = false;
    for (size_t index = 0; index < members.size(); ++index) {
        const std::string& name = *members[index];
        const auto entry = std::ranges::find_if(
            board, [&](const MemberMap& member) { return member.member == name; });
        const bool on_map = entry != board.end() && entry->map == map;
        if (on_map) {
            here.push_back(index);
        }
        if (handled.contains(name)) {
            continue;
        }
        if (on_map) {
            someone_new = true;
        } else {
            plan.done = false;
        }
    }
    // A later arrival re-creates everyone here again: making its copy lost the one made before
    // (Kaniel 2026-10-11: Ada no longer saw Brom once the host arrived after her).
    if (someone_new) {
        plan.now = std::move(here);
    }
    return plan;
}

bool HostIsHere(std::optional<u32> host_map, std::span<const MemberMap> board,
                std::string_view self, u32 map) {
    return host_map && *host_map == map && map != 0 &&
           std::ranges::any_of(board, [&](const MemberMap& entry) {
               return entry.member != self && entry.map == map;
           });
}

bool HostAway(std::optional<u32> host_map, std::span<const MemberMap> board, std::string_view self,
              u32 map) {
    return host_map ? !HostIsHere(host_map, board, self, map) : NoOneElseOnMap(board, self, map);
}

bool SomeoneElsewhere(std::span<const MemberMap> board, std::string_view self, u32 map) {
    return std::ranges::any_of(
        board, [&](const MemberMap& entry) { return entry.member != self && entry.map != map; });
}

bool PostsKillEchoes(std::span<const MemberMap> board, std::string_view self, u32 map) {
    return SomeoneElsewhere(board, self, map) &&
           std::ranges::none_of(board, [&](const MemberMap& entry) {
               return entry.member != self && entry.map == map && entry.member < self;
           });
}

bool RoomForMore(bool hosting, size_t in_session, size_t party_online) {
    constexpr size_t MaxPlayers = 3; // the host and two cooperators
    return hosting && in_session < MaxPlayers && party_online > in_session;
}

std::optional<size_t> PickSavedJoinTask(std::span<const SavedJoinTask> saved, s32 kind,
                                        u64 identity) {
    std::optional<size_t> only;
    size_t of_kind = 0;
    for (size_t index = 0; index < saved.size(); ++index) {
        if (saved[index].kind != kind) {
            continue;
        }
        if (saved[index].identity == identity) {
            return index;
        }
        ++of_kind;
        only = index;
    }
    return of_kind == 1 ? only : std::nullopt;
}

bool RebuildsFromSave(const SavedJoinTask& saved) {
    return saved.step > CreateCharacterStep;
}

bool HasAllMemberData(u8 parts) {
    return (parts & MemberDataParts) == MemberDataParts;
}

MemberDataWait::Action MemberDataWait::Tick(s64 now_ms, bool member_here, u8 parts) {
    if (!member_here) {
        here_since.reset();
        requested_at.reset();
        return Action::None;
    }
    if (!here_since) {
        here_since = now_ms;
    }
    if (now_ms - *here_since < SettleMs) {
        return Action::None;
    }
    if (requested_at && HasAllMemberData(parts)) {
        return Action::Create;
    }
    if (!requested_at || now_ms - *requested_at >= RepeatMs) {
        requested_at = now_ms;
        ++requests;
        return Action::Request;
    }
    return Action::None;
}

bool OwnDataWatch::Observe(std::optional<u64> fingerprint, s64 now_ms) {
    if (!fingerprint) {
        sent.reset();
        pending.reset();
        return false;
    }
    if (!sent) {
        sent = fingerprint;
        return false;
    }
    if (*fingerprint == *sent) {
        pending.reset();
        return false;
    }
    if (pending != fingerprint) {
        pending = fingerprint;
        pending_since = now_ms;
    }
    if (now_ms - pending_since < SettleMs) {
        return false;
    }
    sent = fingerprint;
    pending.reset();
    return true;
}

std::vector<SavedJoinTask> MergeSavedJoinTasks(std::span<const SavedJoinTask> before,
                                               std::span<const SavedJoinTask> found) {
    std::vector<SavedJoinTask> merged{found.begin(), found.end()};
    for (const SavedJoinTask& task : before) {
        if (std::ranges::none_of(
                found, [&](const SavedJoinTask& now) { return now.identity == task.identity; })) {
            merged.push_back(task);
        }
    }
    return merged;
}

std::string EncodeTrophyUpdate(std::string_view member, std::span<const s32> trophies) {
    std::string body{member};
    for (const s32 trophy : trophies) {
        body += ' ' + std::to_string(trophy);
    }
    return body;
}

std::optional<std::vector<s32>> ParseTrophyBoard(std::string_view body) {
    std::vector<s32> trophies;
    while (!body.empty()) {
        const auto end = body.find('\n');
        const std::string_view line = body.substr(0, end);
        body = end == std::string_view::npos ? std::string_view{} : body.substr(end + 1);
        if (line.empty()) {
            continue;
        }
        s32 trophy = 0;
        const auto [parsed_end, error] =
            std::from_chars(line.data(), line.data() + line.size(), trophy);
        if (error != std::errc{} || parsed_end != line.data() + line.size() || trophy < 0) {
            return std::nullopt;
        }
        trophies.push_back(trophy);
    }
    return trophies;
}

std::vector<s32> TrophiesToUnlock(std::span<const s32> board, std::span<const s32> handled) {
    std::vector<s32> missing;
    for (const s32 trophy : board) {
        if (std::ranges::find(handled, trophy) == handled.end()) {
            missing.push_back(trophy);
        }
    }
    return missing;
}

std::optional<int> CharacterSlotOfSavePath(std::string_view path) {
    constexpr std::string_view prefix = "/savedata0/userdata000";
    if (path.size() != prefix.size() + 1 || !path.starts_with(prefix)) {
        return std::nullopt;
    }
    const char digit = path.back();
    if (digit < '0' || digit > '9') {
        return std::nullopt;
    }
    return digit - '0';
}

LampUnblock LampBytesToClear(bool blocked, bool missing, bool host_effect, bool in_party_session) {
    return {blocked && (host_effect || in_party_session), missing && in_party_session};
}

void SummonJoinWatch::Delivered(Clock::time_point now) {
    if (!since_ || (reported_ && now - last_ >= NewSummonGap)) {
        since_ = now;
        reported_ = false;
    }
    last_ = now;
}

bool SummonJoinWatch::Stalled(bool joining, Clock::time_point now) {
    if (joining) {
        since_.reset();
        reported_ = false;
        return false;
    }
    if (!since_ || reported_ || now - *since_ < Patience) {
        return false;
    }
    reported_ = true;
    return true;
}

bool IsOtherCharacter(s32 progress_now, s32 progress_kept) {
    // Event flags mostly add up as a character plays; far fewer set means another character. A
    // load clears some area flags (dozens to a few hundred: Brom's restarts read 5800 set
    // against 5859 kept), so "far" is a quarter of what was kept.
    const s32 slack = std::max<s32>(20, progress_kept / 4);
    // No world has 0 flags set: that is a store read mid-load, not a character.
    return progress_kept > 0 && progress_now > 0 && progress_now + slack < progress_kept;
}

std::optional<s32> SettledFlagCount(std::optional<s32> first, std::optional<s32> second) {
    if (!first || !second || *first <= 0 || *second <= 0) {
        return std::nullopt;
    }
    // Two reads a moment apart differ by a few flags at most; more is a read torn by the load.
    const s32 slack = std::max<s32>(20, std::max(*first, *second) / 100);
    if (std::abs(*first - *second) > slack) {
        return std::nullopt;
    }
    return std::max(*first, *second);
}

bool ForgetsSlotLists(std::optional<s32> progress_now, s32 progress_kept, bool new_in_slot) {
    if (!progress_now || *progress_now <= 0) {
        return false;
    }
    return new_in_slot || IsOtherCharacter(*progress_now, progress_kept);
}

std::string SlotBackupName(std::string_view file, std::string_view stamp) {
    return fmt::format("{}.bak-{}", file, stamp);
}

std::vector<std::string> SlotBackupsToRemove(std::string_view file,
                                             std::span<const std::string> names, size_t keep) {
    const std::string prefix = SlotBackupName(file, "");
    std::vector<std::string> backups;
    for (const std::string& name : names) {
        if (name.size() > prefix.size() && name.starts_with(prefix)) {
            backups.push_back(name);
        }
    }
    std::ranges::sort(backups);
    if (backups.size() <= keep) {
        return {};
    }
    backups.resize(backups.size() - keep);
    return backups;
}

std::string SlotFileName(std::string_view list, int slot) {
    return fmt::format("bloodborne_party_{}_slot{}.txt", list, slot);
}

void CoopTimeMeter::Add(std::string_view section, s64 ns, s64 now_ms) {
    std::scoped_lock lock{mutex};
    if (!since_ms) {
        since_ms = now_ms;
    }
    auto it = std::ranges::find(sections, section, &Section::name);
    if (it == sections.end()) {
        it = sections.insert(sections.end(), Section{std::string{section}});
    }
    it->total_ns += ns;
    it->max_ns = std::max(it->max_ns, ns);
    ++it->calls;
}

std::optional<std::string> CoopTimeMeter::Report(s64 now_ms, size_t slowest) {
    std::scoped_lock lock{mutex};
    if (!since_ms || now_ms - *since_ms < period_ms) {
        return std::nullopt;
    }
    if (slowest < sections.size()) {
        std::ranges::stable_sort(sections, std::ranges::greater{}, &Section::total_ns);
        sections.resize(slowest);
    }
    std::string line;
    for (const Section& section : sections) {
        line += fmt::format("{}{} avg {:.2f} ms max {:.2f} ms ({} calls)", line.empty() ? "" : ", ",
                            section.name,
                            static_cast<double>(section.total_ns) / 1e6 /
                                static_cast<double>(std::max<u64>(section.calls, 1)),
                            static_cast<double>(section.max_ns) / 1e6, section.calls);
    }
    line += fmt::format(" over {} s", (now_ms - *since_ms) / 1000);
    since_ms.reset();
    sections.clear();
    return line;
}

OwnRestore RestoresOwnCharacters(std::span<const u32> own,
                                 const std::unordered_set<u32>& hidden_here,
                                 const std::unordered_set<u32>& switched_off) {
    OwnRestore restore;
    for (const u32 handle : own) {
        if (hidden_here.contains(handle)) {
            restore.show.push_back(handle);
        }
        if (switched_off.contains(handle)) {
            restore.switch_on.push_back(handle);
        }
    }
    return restore;
}

StoryHoldStep StoryRemovals::Step(u32 map, s64 visit_ms, s64 now_ms,
                                  std::span<const StoryCharacter> seen) {
    StoryHoldStep step;
    std::set<u32>& remembered = removed[map];
    const bool settled = now_ms - visit_ms >= StoryHoldMs;
    std::set<u32> loaded;
    for (const StoryCharacter& character : seen) {
        loaded.insert(character.handle);
        const auto holding = held.find(character.handle);
        if (character.dead) {
            if (holding != held.end()) {
                step.release.push_back(character.handle);
                held.erase(holding);
            }
            remembered.erase(character.handle);
            continue;
        }
        if (character.followed || character.killed) {
            if (holding != held.end()) {
                step.hand_over.push_back(character.handle);
                held.erase(holding);
            }
            if (settled && character.followed && character.removed) {
                remembered.insert(character.handle);
            }
            continue;
        }
        if (holding != held.end()) {
            if (character.removed) {
                step.release.push_back(character.handle);
                held.erase(holding);
            } else if (now_ms - holding->second >= StoryHoldMs) {
                step.release.push_back(character.handle);
                held.erase(holding);
                remembered.erase(character.handle);
            }
            continue;
        }
        if (!settled) {
            if (remembered.contains(character.handle) && !character.removed &&
                !character.disabled) {
                step.hold.push_back(character.handle);
            }
        } else if (character.removed) {
            remembered.insert(character.handle);
        } else {
            remembered.erase(character.handle);
        }
    }
    // Unloaded: nothing left to switch on.
    std::erase_if(held, [&](const auto& entry) { return !loaded.contains(entry.first); });
    return step;
}

void StoryRemovals::Held(u32 handle, s64 now_ms) {
    held[handle] = now_ms;
}

void StoryRemovals::Forget() {
    removed.clear();
    held.clear();
}

size_t StoryRemovals::RememberedOn(u32 map) const {
    const auto it = removed.find(map);
    return it != removed.end() ? it->second.size() : 0;
}

bool StoryRemovals::Remembers(u32 map, u32 handle) const {
    const auto it = removed.find(map);
    return it != removed.end() && it->second.contains(handle);
}

bool FollowsTheHostsShown(s32 entity) {
    // m21_00: the right- and left-hand weapon givers and their weapon displays (12101020/21
    // hide them once their gift is taken), the note and the multiplayer items givers.
    constexpr std::array DreamMessengers{2100215, 2100216, 2100220, 2100221, 2100230, 2100231};
    return std::ranges::find(DreamMessengers, entity) == DreamMessengers.end();
}

bool IsNewCharacterInSlot(std::span<const s32> confirmed, size_t confirmed_set_now) {
    return !confirmed.empty() && confirmed_set_now == 0;
}

bool IsMapPickupFlag(s32 flag) {
    return RuleFor(flag).Has(FlagMapPickup);
}

std::string FormatFlagList(std::span<const s32> flags) {
    std::string text;
    for (const s32 flag : flags) {
        text += std::to_string(flag) + '\n';
    }
    return text;
}

std::vector<s32> ParseFlagList(std::string_view text) {
    std::vector<s32> flags;
    while (!text.empty()) {
        const auto end = text.find('\n');
        const std::string_view line = text.substr(0, end);
        text = end == std::string_view::npos ? std::string_view{} : text.substr(end + 1);
        s32 flag = 0;
        const auto [parsed_end, error] =
            std::from_chars(line.data(), line.data() + line.size(), flag);
        if (!line.empty() && error == std::errc{} && parsed_end == line.data() + line.size()) {
            flags.push_back(flag);
        }
    }
    return flags;
}

FlagChanges ChangedFlags(u32 block, u32 flags_per_block, std::span<const u8> before,
                         std::span<const u8> after) {
    FlagChanges changes;
    const size_t bytes = std::min(before.size(), after.size());
    for (size_t byte = 0; byte < bytes; ++byte) {
        const u8 differs = before[byte] ^ after[byte];
        if (differs == 0) {
            continue;
        }
        for (u32 bit = 0; bit < 8; ++bit) {
            const u8 mask = static_cast<u8>(0x80 >> bit);
            if ((differs & mask) == 0) {
                continue;
            }
            const s32 flag = static_cast<s32>(block * flags_per_block + byte * 8 + bit);
            ((after[byte] & mask) != 0 ? changes.set : changes.cleared).push_back(flag);
        }
    }
    return changes;
}

bool CarriesToGuestWorld(s32 flag) {
    return RuleFor(flag).Has(FlagCarriesHome);
}

bool GameThreadStalled(s64 now_ms, s64 last_tick_ms) {
    return last_tick_ms > 0 && now_ms - last_tick_ms >= GameStallMs;
}

std::span<const s32> ShopLineFlags() {
    static constexpr auto flags = FlagsWith<FlagShopLine>();
    return flags;
}

bool RechecksShopLines(bool party_configured, int slot, bool done_for_slot, u32 map) {
    constexpr u32 HuntersDream = 0x15000000;
    return party_configured && slot >= 0 && !done_for_slot && map != 0 && map != HuntersDream;
}

std::optional<bool> HeldItemFlagFix(bool party_configured, bool flag_on, bool holds) {
    if (!party_configured || flag_on == holds) {
        return std::nullopt;
    }
    return holds;
}

std::optional<bool> GestureFlagFix(bool party_configured, bool flag_on, bool learned) {
    if (!party_configured || !flag_on || learned) {
        return std::nullopt;
    }
    return false;
}

std::optional<bool> GiftTalkFlagFix(bool party_configured, bool flag_on, bool holds) {
    if (!party_configured || !flag_on || holds) {
        return std::nullopt;
    }
    return false;
}

bool ClearsNpcGiftLotFlag(s32 lot_flag, bool flag_on) {
    if (lot_flag <= 0 || !flag_on) {
        return false;
    }
    const FlagRule rule = RuleFor(lot_flag);
    return rule.owner == FlagOwner::Host && !rule.Has(FlagMapPickup);
}

std::optional<GestureSlot> DecodeGestureSlot(u32 entry) {
    // 0x02042B01: above 0xFFFEFFFF, or 0xFFFE / 0xFFFF in the low half, is no gesture.
    if (entry > 0xFFFEFFFF || (entry & 0xFFFE) == 0xFFFE) {
        return std::nullopt;
    }
    // The gesture table's row (0x014EC980: 31 rows, row 0 none, row k gesture k - 1).
    const s32 row = static_cast<s16>(entry & 0xFFFF) >> 1;
    if (row < 1 || row > 30) {
        return std::nullopt;
    }
    return GestureSlot{row - 1, (entry & 1) != 0};
}

bool HoldsWeapon(u32 weapon, std::span<const u32> held) {
    return std::ranges::any_of(held, [&](u32 id) {
        return id >= weapon && id - weapon <= 1000 && (id - weapon) % 100 == 0;
    });
}

std::optional<bool> OwnedWeaponFlagFix(bool party_configured, bool flag_on, bool holds,
                                       bool recheck) {
    if (!party_configured || flag_on == holds || (flag_on && !recheck)) {
        return std::nullopt;
    }
    return holds;
}

bool RechecksOwnedWeaponFlagAlways(s32 flag) {
    return flag >= 5500 && flag <= 5521;
}

std::optional<bool> HeadstoneLampsLit(s32 flag, const std::function<bool(s32)>& on) {
    for (const HeadstoneLamps& headstone : DreamHeadstones) {
        if (headstone.flag == flag) {
            return std::ranges::any_of(headstone.lamps,
                                       [&](s32 lamp) { return lamp != 0 && on(lamp); });
        }
    }
    return std::nullopt;
}

std::vector<s32> LitHeadstoneLamps(const std::function<bool(s32)>& on) {
    std::vector<s32> lit;
    for (const HeadstoneLamps& headstone : DreamHeadstones) {
        for (const s32 lamp : headstone.lamps) {
            if (lamp != 0 && on(lamp)) {
                lit.push_back(lamp);
            }
        }
    }
    std::ranges::sort(lit);
    lit.erase(std::unique(lit.begin(), lit.end()), lit.end());
    return lit;
}

std::vector<s32> LampsToRelight(const std::set<s32>& kept, std::span<const s32> team,
                                const std::function<bool(s32)>& on) {
    std::set<s32> lamps;
    for (const s32 flag : kept) {
        if (LampOfActiveFlag(flag) && !on(flag)) {
            lamps.insert(flag);
        }
    }
    for (const s32 flag : team) {
        if (LampOfActiveFlag(flag) && !on(flag)) {
            lamps.insert(flag);
        }
    }
    return {lamps.begin(), lamps.end()};
}

std::vector<s32> GuestProgressFlags(bool guest, bool in_session, u32 map, bool dream_visited,
                                    bool workshop_opened) {
    constexpr u32 HuntersDream = 0x15000000;
    constexpr s32 DreamFirstVisit = 9401;
    constexpr s32 WorkshopOpen = 9403;
    if (!guest || !in_session || map == 0 || map == HuntersDream) {
        return {};
    }
    std::vector<s32> flags;
    for (const s32 flag : FlagsWith<FlagGuestSetsInWorld>()) {
        if ((flag != DreamFirstVisit || dream_visited) &&
            (flag != WorkshopOpen || workshop_opened)) {
            flags.push_back(flag);
        }
    }
    return flags;
}

std::optional<std::pair<s32, s32>> CoopHpScale(s32 unscaled_max, s32 max, s32 hp,
                                               std::span<const s32> effects) {
    // SpEffectParam maxHpRate of the co-op boss effects 7500, 7501, 7502.
    static constexpr std::array<float, 3> Rates{1.5f, 2.0f, 2.5f};
    if (unscaled_max <= 0 || hp <= 0 || max != unscaled_max) {
        return std::nullopt;
    }
    for (const s32 effect : effects) {
        if (effect >= 7500 && effect < 7500 + static_cast<s32>(Rates.size())) {
            const float rate = Rates[effect - 7500];
            return std::pair{static_cast<s32>(std::lround(static_cast<float>(max) * rate)),
                             static_cast<s32>(std::lround(static_cast<float>(hp) * rate))};
        }
    }
    return std::nullopt;
}

bool SetsGuestWorldChangesOnLoad(bool in_session, bool on_trip) {
    return !in_session || on_trip;
}

std::string FormatCrashLine(u32 code, u64 address, u64 image_base, u64 image_size,
                            std::string_view thread) {
    const bool in_game = address >= image_base && address - image_base < image_size;
    return fmt::format("crash: exception {:#x} at {:#x} ({}) thread {}", code, address,
                       in_game ? fmt::format("eboot+{:#x}", address - image_base) : "emulator",
                       thread.empty() ? "?" : thread);
}

std::string FormatGameAddresses(std::span<const u64> words, u64 image_base, u64 image_size,
                                size_t max) {
    std::string text;
    size_t found = 0;
    for (const u64 word : words) {
        if (found == max) {
            break;
        }
        if (word >= image_base && word - image_base < image_size) {
            text += fmt::format(" eboot+{:#x}", word - image_base);
            ++found;
        }
    }
    return text;
}

bool IsBossFogEntryFlag(s32 flag) {
    return RuleFor(flag).Has(FlagBossFogEntry);
}

bool KeepsLocalFlag(bool party_configured, s32 flag) {
    // The messengers' gifts and pick, each boss's "found" flag (see FlagTable).
    return party_configured && RuleFor(flag).owner == FlagOwner::PerPlayer;
}

std::span<const EncounterInsight> EncounterInsights() {
    // {defeat event, found flag, insight}: each map's xx1800/xx1700/xx1850 defeat event and the
    // entrance event beside it (xx1802, xx1702, xx1852, or xx1801/xx1851 in the Chalice and DLC
    // maps) that starts 9350 behind the found flag.
    static constexpr std::array<EncounterInsight, 20> bosses{{
        {12101800, 9343, 1}, {12101850, 9307, 5}, {12201800, 9338, 1}, {12301800, 9339, 1},
        {12301700, 9340, 1}, {12401800, 9301, 1}, {12411700, 9300, 1}, {12411800, 9336, 1},
        {12421800, 9304, 3}, {12501800, 9341, 1}, {12601800, 9306, 3}, {12601850, 9342, 2},
        {12801800, 9305, 1}, {13201800, 9303, 2}, {13301800, 9302, 3}, {13401800, 9344, 1},
        {13401850, 9302, 1}, {13501800, 9346, 1}, {13501850, 9348, 2}, {13601800, 9347, 5},
    }};
    return bosses;
}

std::optional<s32> TracedFlagSetting(const char* setting) {
    if (setting == nullptr) {
        return std::nullopt;
    }
    const std::string_view text{setting};
    s32 flag = 0;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), flag);
    if (error != std::errc{} || end != text.data() + text.size() || flag <= 0) {
        return std::nullopt;
    }
    return flag;
}

FlagBit FlagBitLocation(s32 flag, u32 flags_per_block) {
    const u32 within = static_cast<u32>(flag) % flags_per_block;
    return {static_cast<u32>(flag) / flags_per_block, within / 8,
            static_cast<u8>(0x80 >> (within % 8))};
}

std::vector<s32> LampFlagsKeptAtRegistration(bool party_configured, s32 base, u16 on_before) {
    std::vector<s32> kept;
    if (!party_configured || base < 0) {
        return kept;
    }
    for (s32 index = 0; index < 12; ++index) {
        if ((on_before >> index) & 1) {
            kept.push_back(base + index);
        }
    }
    return kept;
}

DeathRespawn ChooseDeathRespawn(s32 point, bool in_loaded_block, bool arriving_from_trip,
                                bool reset, bool void_fall) {
    if (point <= 0) {
        return DeathRespawn::StayPut;
    }
    if ((reset || void_fall) && !arriving_from_trip) {
        return DeathRespawn::Travel;
    }
    if (in_loaded_block) {
        return DeathRespawn::MoveToPoint;
    }
    return arriving_from_trip ? DeathRespawn::StayPut : DeathRespawn::Travel;
}

bool WipeTripRespawnsOnArrival(bool reset, DeathRespawn respawn) {
    return reset && respawn == DeathRespawn::Travel;
}

bool DeathWasAVoidFall(bool void_bit, float fade) {
    return void_bit || fade < 1.0f;
}

s32 PartyRespawnPoint(bool guest, s32 own_point, std::optional<s32> own_point_alive,
                      std::optional<s32> host_point) {
    if (!guest || !host_point || *host_point <= 0 || !own_point_alive) {
        return own_point;
    }
    if (own_point != *own_point_alive) {
        return own_point; // the death set it: the Dream, the Gaol
    }
    return *host_point;
}

void ReturnPointBeforeDeath::Sample(s32 point, s64 now_ms) {
    if (!settled || point == *settled) {
        settled = point;
        changed.reset();
        return;
    }
    if (!changed || changed->first != point) {
        changed = std::pair{point, now_ms};
    }
    if (now_ms - changed->second >= ReturnPointSettleMs) {
        settled = point;
        changed.reset();
    }
}

std::vector<std::pair<s32, bool>> FlagsToShare(const FlagChanges& changes) {
    std::vector<std::pair<s32, bool>> shared;
    for (const s32 flag : changes.set) {
        if (SharedWithParty(flag)) {
            shared.emplace_back(flag, true);
        }
    }
    for (const s32 flag : changes.cleared) {
        if (SharesClearsWithParty(flag)) {
            shared.emplace_back(flag, false);
        }
    }
    return shared;
}

std::vector<std::pair<s32, bool>> LoneGuestFlagsToShare(const FlagChanges& changes, u32 map) {
    const s32 first = MapFlagBase(map);
    const auto takes = [first](s32 flag) {
        return flag >= first && flag < first + MapFlagCount && TakesHostMapFlag(flag) &&
               RuleFor(flag).owner == FlagOwner::Host;
    };
    std::vector<std::pair<s32, bool>> shared;
    for (const s32 flag : changes.set) {
        if (takes(flag)) {
            shared.emplace_back(flag, true);
        }
    }
    for (const s32 flag : changes.cleared) {
        if (takes(flag)) {
            shared.emplace_back(flag, false);
        }
    }
    std::ranges::sort(shared);
    return shared;
}

std::vector<std::pair<s32, bool>> LoneGuestShares::Take(const FlagChanges& changes, u32 map,
                                                        s64 since_load_ms) {
    std::set<s32>& known = load_flags[map];
    auto shared = LoneGuestFlagsToShare(changes, map);
    if (since_load_ms < LoadFlagsMs) {
        for (const auto& [flag, on] : shared) {
            known.insert(flag);
        }
        return {};
    }
    std::erase_if(shared, [&known](const std::pair<s32, bool>& change) {
        return known.contains(change.first);
    });
    return shared;
}

namespace {
constexpr s32 CurrentAltar = 92900000;
constexpr s32 AltarFlags = 6000;
} // namespace

bool IsCurrentAltarFlag(s32 flag) {
    return flag >= CurrentAltar && flag < CurrentAltar + AltarFlags;
}

std::optional<u32> AltarChaliceGoods(u32 depth_area) {
    // The standard chalices (goods 6109 Pthumeru, ...), as the server's StandardDungeon has them.
    constexpr std::array<std::pair<u32, u32>, 10> Chalices{{{10, 6109},
                                                            {20, 6209},
                                                            {21, 6219},
                                                            {30, 6309},
                                                            {31, 6319},
                                                            {40, 6409},
                                                            {42, 6429},
                                                            {50, 6509},
                                                            {52, 6529},
                                                            {53, 6539}}};
    for (const auto& [key, goods] : Chalices) {
        if (key == depth_area) {
            return goods;
        }
    }
    return std::nullopt;
}

bool SkipsOthersAltarWarp(s32 flag, bool talker_is_another_player) {
    const bool warp =
        (flag >= 72100420 && flag <= 72100429) || (flag >= 72100300 && flag <= 72100309);
    return warp && talker_is_another_player;
}

bool SharesClearsWithParty(s32 flag) {
    return SharedWithParty(flag) && RuleFor(flag).Has(FlagSharesClears);
}

bool SharedWithParty(s32 flag) {
    // A lamp's active flags, a boss fog's "entered" flag (see FlagTable).
    return RuleFor(flag).owner == FlagOwner::Shared;
}

std::optional<s32> LampOfActiveFlag(s32 flag) {
    constexpr s32 ActiveFlagBase = 10007810;
    const s32 offset = flag - ActiveFlagBase;
    if (offset < 0) {
        return std::nullopt;
    }
    const s32 map = offset / 10000;
    const s32 within = offset % 10000;
    if (map < 210 || map >= 400 || within % 20 != 0 || within / 20 >= 20) {
        return std::nullopt;
    }
    return map * 10000 + 1950 + within / 20;
}

namespace {

constexpr s32 LightRequestBase = 70000000;
constexpr s32 LightRequestLow = 200;
constexpr s32 LampMapFirst = 211;
constexpr s32 LampMapLast = 399;
constexpr s32 LampsPerMap = 20;

} // namespace

std::optional<LampLight> LampOfLightRequest(s32 flag) {
    const s32 offset = flag - LightRequestBase;
    if (offset < 0) {
        return std::nullopt;
    }
    const s32 map = offset / 10000;
    const s32 lamp = offset % 10000 - LightRequestLow;
    if (map < LampMapFirst || map > LampMapLast || lamp < 0 || lamp >= LampsPerMap) {
        return std::nullopt;
    }
    return LampLight{.character = map * 10000 + 950 + lamp,
                     .active_flag = 10007810 + map * 10000 + lamp * 20};
}

bool RefusesLampLight(s32 flag, std::optional<bool> lamp_character_off) {
    return LampOfLightRequest(flag).has_value() && lamp_character_off.value_or(false);
}

bool ClearsDollLevelUpFlag(bool party_configured, bool flag_on) {
    return party_configured && flag_on;
}

std::vector<s32> LightRequestsToClear(u32 packed_map, const std::function<bool(s32)>& on) {
    const s32 map = static_cast<s32>((packed_map >> 24) & 0xFF) * 10 +
                    static_cast<s32>((packed_map >> 16) & 0xFF);
    std::vector<s32> clear;
    if (map < LampMapFirst || map > LampMapLast) {
        return clear;
    }
    for (s32 lamp = 0; lamp < LampsPerMap; ++lamp) {
        const s32 request = LightRequestBase + map * 10000 + LightRequestLow + lamp;
        if (on(request) && !on(LampOfLightRequest(request)->active_flag)) {
            clear.push_back(request);
        }
    }
    return clear;
}

bool IsBulkFlagChange(size_t changed_flags) {
    return changed_flags > 64;
}

void RecordWorldChanges(std::map<s32, bool>& world, const FlagChanges& changes) {
    for (const s32 flag : changes.set) {
        if (CarriesToGuestWorld(flag)) {
            world[flag] = true;
        }
    }
    for (const s32 flag : changes.cleared) {
        if (CarriesToGuestWorld(flag)) {
            world[flag] = false;
        }
    }
}

std::string FormatWorldChanges(const std::map<s32, bool>& world) {
    std::string text;
    for (const auto& [flag, on] : world) {
        text += std::to_string(flag) + (on ? " 1\n" : " 0\n");
    }
    return text;
}

std::map<s32, bool> ParseWorldChanges(std::string_view text) {
    std::map<s32, bool> world;
    while (!text.empty()) {
        const auto end = text.find('\n');
        const std::string_view line = text.substr(0, end);
        text = end == std::string_view::npos ? std::string_view{} : text.substr(end + 1);
        const auto space = line.find(' ');
        if (space == std::string_view::npos || space + 2 != line.size()) {
            continue;
        }
        s32 flag = 0;
        const auto [parsed_end, error] = std::from_chars(line.data(), line.data() + space, flag);
        const char state = line[space + 1];
        if (error == std::errc{} && parsed_end == line.data() + space &&
            (state == '0' || state == '1') && CarriesToGuestWorld(flag)) {
            world[flag] = state == '1';
        }
    }
    return world;
}

std::string EncodeFlagUpdate(std::string_view member, s64 since,
                             std::span<const std::pair<s32, bool>> changes) {
    std::string body = std::string{member} + ' ' + std::to_string(since);
    for (const auto& [flag, on] : changes) {
        body += ' ' + std::to_string(flag) + (on ? ":1" : ":0");
    }
    return body;
}

std::optional<FlagBoardAnswer> ParseFlagBoard(std::string_view body) {
    FlagBoardAnswer answer;
    const auto first_end = body.find('\n');
    const std::string_view first = body.substr(0, first_end);
    const auto [latest_end, latest_error] =
        std::from_chars(first.data(), first.data() + first.size(), answer.latest);
    if (latest_error != std::errc{} || latest_end != first.data() + first.size()) {
        return std::nullopt;
    }
    body = first_end == std::string_view::npos ? std::string_view{} : body.substr(first_end + 1);
    while (!body.empty()) {
        const auto end = body.find('\n');
        const std::string_view line = body.substr(0, end);
        body = end == std::string_view::npos ? std::string_view{} : body.substr(end + 1);
        if (line.empty()) {
            continue;
        }
        const auto space = line.find(' ');
        s32 flag = 0;
        const auto [flag_end, flag_error] =
            std::from_chars(line.data(), line.data() + std::min(space, line.size()), flag);
        if (space == std::string_view::npos || flag_error != std::errc{} ||
            flag_end != line.data() + space || line.size() != space + 2 ||
            (line[space + 1] != '0' && line[space + 1] != '1')) {
            return std::nullopt;
        }
        answer.changes.emplace_back(flag, line[space + 1] == '1');
    }
    return answer;
}

std::string EncodeResumePoint(const ResumePoint& point) {
    return fmt::format("{} {} {} {} {}", point.map, point.position[0], point.position[1],
                       point.position[2], point.progress);
}

std::optional<ResumePoint> ParseResumePoint(std::string_view text) {
    ResumePoint point;
    const char* cursor = text.data();
    const char* end = text.data() + text.size();
    const auto [map_end, map_error] = std::from_chars(cursor, end, point.map);
    if (map_error != std::errc{}) {
        return std::nullopt;
    }
    cursor = map_end;
    for (float& coordinate : point.position) {
        if (cursor == end || *cursor != ' ') {
            return std::nullopt;
        }
        ++cursor;
        const auto [next, error] = std::from_chars(cursor, end, coordinate);
        if (error != std::errc{}) {
            return std::nullopt;
        }
        cursor = next;
    }
    if (cursor == end || *cursor != ' ') {
        return std::nullopt;
    }
    ++cursor;
    const auto [progress_end, progress_error] = std::from_chars(cursor, end, point.progress);
    if (progress_error != std::errc{}) {
        return std::nullopt;
    }
    cursor = progress_end;
    while (cursor != end && (*cursor == '\n' || *cursor == '\r' || *cursor == ' ')) {
        ++cursor;
    }
    if (cursor != end) {
        return std::nullopt;
    }
    return point;
}

s32 MapWarpPoint(u32 packed_map) {
    constexpr u32 HuntersDream = 0x15000000;
    constexpr s32 HuntersDreamHeadstones = 2102962;
    if (packed_map == HuntersDream) {
        return HuntersDreamHeadstones;
    }
    const s32 area = static_cast<s32>((packed_map >> 24) & 0xFF);
    const s32 block = static_cast<s32>((packed_map >> 16) & 0xFF);
    return (area * 10 + block) * 10000 + 2950;
}

ResumeStep ChooseResumeStep(const ResumePoint& point, u32 map, const LampPosition& at, s32 progress,
                            u32 loaded_map, std::optional<LampPosition> loaded_at) {
    if (point.map == 0 || (loaded_map != 0 && loaded_map != map)) {
        return {};
    }
    if (loaded_at) {
        const float mx = at[0] - (*loaded_at)[0];
        const float my = at[1] - (*loaded_at)[1];
        const float mz = at[2] - (*loaded_at)[2];
        if (mx * mx + my * my + mz * mz > 1.0f) {
            return {}; // it set off on its own
        }
    }
    if (IsOtherCharacter(progress, point.progress)) {
        return {ResumeStep::Kind::Forget, 0};
    }
    if (point.map != map) {
        return {ResumeStep::Kind::Travel, MapWarpPoint(point.map)};
    }
    const float dx = point.position[0] - at[0];
    const float dy = point.position[1] - at[1];
    const float dz = point.position[2] - at[2];
    if (dx * dx + dy * dy + dz * dz <= 5.0f * 5.0f) {
        return {};
    }
    return {ResumeStep::Kind::Move, 0};
}

std::optional<ResumePoint> PickResumePoint(bool in_session, std::optional<ResumePoint> from_file,
                                           std::optional<ResumePoint> from_server,
                                           std::optional<ResumePoint> from_host) {
    const auto own = from_server ? from_server : (in_session ? std::nullopt : from_file);
    if (from_host && !(own && own->map == from_host->map)) {
        return from_host;
    }
    return own;
}

bool WaitsForHostPlace(bool team_load, bool host_known, s64 waited_ms) {
    return team_load && !host_known && waited_ms < HostPlaceWaitMs;
}

bool WaitsForTeamSession(bool team_load, bool in_session, s64 waited_ms) {
    return team_load && !in_session && waited_ms < TeamSessionWaitMs;
}

std::optional<ResumePoint> HostPlaceOf(std::span<const MemberVitals> board, std::string_view host) {
    if (host.empty()) {
        return std::nullopt;
    }
    const auto it = std::ranges::find(board, host, &MemberVitals::member);
    if (it == board.end() || it->map == 0 || (it->vital != Vital::Up && it->vital != Vital::Down)) {
        return std::nullopt;
    }
    return ResumePoint{it->map, it->at, 0};
}

s32 MapFlagBase(u32 packed_map) {
    const s32 area = static_cast<s32>((packed_map >> 24) & 0xFF);
    const s32 block = static_cast<s32>((packed_map >> 16) & 0xFF);
    return (1000 + area * 10 + block) * MapFlagCount;
}

bool TakesHostMapFlag(s32 flag) {
    return RuleFor(flag).Has(FlagTakesHostMap);
}

std::string EncodeMapStateUpdate(std::string_view member, u32 map, std::span<const u8> bits) {
    std::string body = std::string{member} + ' ' + std::to_string(map);
    if (!bits.empty()) {
        body += ' ';
        for (const u8 byte : bits) {
            body += fmt::format("{:02x}", byte);
        }
    }
    return body;
}

std::optional<std::vector<MemberMapState>> ParseMapStateBoard(std::string_view body) {
    std::vector<MemberMapState> states;
    const auto nibble = [](char c) -> s32 {
        if (c >= '0' && c <= '9') {
            return c - '0';
        }
        if (c >= 'a' && c <= 'f') {
            return c - 'a' + 10;
        }
        return -1;
    };
    while (!body.empty()) {
        const auto end = body.find('\n');
        const std::string_view line = body.substr(0, end);
        body = end == std::string_view::npos ? std::string_view{} : body.substr(end + 1);
        if (line.empty()) {
            continue;
        }
        const auto space = line.find(' ');
        if (space == std::string_view::npos) {
            return std::nullopt;
        }
        MemberMapState state;
        const auto [map_end, error] = std::from_chars(line.data(), line.data() + space, state.map);
        const std::string_view hex = line.substr(space + 1);
        if (error != std::errc{} || map_end != line.data() + space || hex.size() % 2 != 0) {
            return std::nullopt;
        }
        for (size_t index = 0; index < hex.size(); index += 2) {
            const s32 high = nibble(hex[index]);
            const s32 low = nibble(hex[index + 1]);
            if (high < 0 || low < 0) {
                return std::nullopt;
            }
            state.bits.push_back(static_cast<u8>(high * 16 + low));
        }
        states.push_back(std::move(state));
    }
    return states;
}

u64 VisitTracker::Update(u32 new_map, u64 new_player, bool in_session, u64 new_trips) {
    // A new player character is a load: outside a session, or onto another map (a summon there).
    // A map change with the same character is a walk across a border (Central Yharnam into the
    // Cathedral Ward): it loads nothing and the enemies stay as they are, so the visit goes on.
    const bool reloaded = new_player != player && (!in_session || new_map != seen_map);
    if (new_trips != trips || reloaded || generation == 0) {
        ++generation;
        map = new_map;
    }
    seen_map = new_map;
    player = new_player;
    trips = new_trips;
    return (u64{map} << 32) | generation;
}

std::optional<PlaceChange> PlaceTracker::Update(const PlaceReading& reading) {
    const bool known_map = reading.map != 0 && reading.map != 0xFFFFFFFFu;
    trip_seen = trip_seen || reading.trip;
    if (reading.player != 0 && reading.dead) {
        death_seen = true;
    }
    if (reading.loading || reading.player == 0) {
        pending.reset();
        last_at.reset();
        jumped_ms.reset();
        if (place.state == PlaceState::Loading) {
            return std::nullopt;
        }
        return Move(PlaceState::Loading, place.map, false, LoadCause(), reading.now_ms);
    }
    // Another character object: the world was rebuilt between two ticks (a reload the ticks did
    // not see). VisitTracker has always ended a visit there too.
    if (player != 0 && reading.player != player) {
        arrival_due = true;
    }
    player = reading.player;
    if (reading.at && last_at &&
        std::hypot((*reading.at)[0] - (*last_at)[0], (*reading.at)[1] - (*last_at)[1],
                   (*reading.at)[2] - (*last_at)[2]) > TeleportMeters) {
        jumped_ms = reading.now_ms;
    }
    if (reading.at) {
        last_at = reading.at;
    }
    if (!known_map) {
        return std::nullopt; // a failed read keeps the place, and a pending crossing
    }
    if (place.state == PlaceState::Unknown || place.state == PlaceState::Loading || arrival_due) {
        const PlaceCause cause = LoadCause();
        arrival_due = false;
        trip_seen = false;
        death_seen = false;
        pending.reset();
        jumped_ms.reset();
        return Move(PlaceState::Playing, reading.map, true, cause, reading.now_ms);
    }
    if (place.state == PlaceState::Cutscene) {
        if (reading.cutscene) {
            return std::nullopt; // what the map reads while a cutscene plays is not followed
        }
        return Move(PlaceState::Playing, place.map, false, PlaceCause::CutsceneEnd, reading.now_ms);
    }
    // Playing. Up again without a load (a party death revive): the next load is no death reload.
    if (!reading.dead) {
        death_seen = false;
    }
    if (reading.cutscene) {
        pending.reset();
        return Move(PlaceState::Cutscene, place.map, false, PlaceCause::Cutscene, reading.now_ms);
    }
    if (reading.map == place.map) {
        pending.reset(); // back before the crossing committed
        return std::nullopt;
    }
    if (jumped_ms && reading.now_ms - *jumped_ms <= TeleportWindowMs) {
        pending.reset();
        jumped_ms.reset();
        return Move(PlaceState::Playing, reading.map, true, PlaceCause::Teleport, reading.now_ms);
    }
    if (!pending || pending->first != reading.map) {
        pending = std::pair{reading.map, reading.now_ms};
        return std::nullopt;
    }
    if (reading.now_ms - pending->second < BorderSteadyMs) {
        return std::nullopt;
    }
    pending.reset();
    return Move(PlaceState::Playing, reading.map, true, PlaceCause::Border, reading.now_ms);
}

std::optional<PlaceChange> PlaceTracker::Move(PlaceState state, u32 map, bool arrival,
                                              PlaceCause cause, s64 now_ms) {
    PlaceChange change{.from = place, .cause = cause};
    place.state = state;
    place.map = map;
    place.visit += arrival ? 1 : 0;
    place.since_ms = now_ms;
    change.to = place;
    return change;
}

// A death and its reload come before a trip: a party wipe's respawn travels to the lamp.
PlaceCause PlaceTracker::LoadCause() const {
    return death_seen ? PlaceCause::DeathReload : trip_seen ? PlaceCause::Lamp : PlaceCause::Load;
}

std::string_view PlaceStateName(PlaceState state) {
    switch (state) {
    case PlaceState::Unknown:
        return "Unknown";
    case PlaceState::Loading:
        return "Loading";
    case PlaceState::Cutscene:
        return "Cutscene";
    case PlaceState::Playing:
        return "Playing";
    }
    return "?";
}

std::string_view PlaceCauseName(PlaceCause cause) {
    switch (cause) {
    case PlaceCause::Load:
        return "Load";
    case PlaceCause::Border:
        return "Border";
    case PlaceCause::Lamp:
        return "Lamp";
    case PlaceCause::Teleport:
        return "Teleport";
    case PlaceCause::DeathReload:
        return "DeathReload";
    case PlaceCause::Cutscene:
        return "Cutscene";
    case PlaceCause::CutsceneEnd:
        return "CutsceneEnd";
    }
    return "?";
}

std::string FormatPlaceChange(const PlaceChange& change) {
    return fmt::format("Co-op place: {} -> {} map {:#x} visit {} ({})",
                       PlaceStateName(change.from.state), PlaceStateName(change.to.state),
                       change.to.map, change.to.visit, PlaceCauseName(change.cause));
}

std::string_view LifecycleName(Lifecycle event) {
    switch (event) {
    case Lifecycle::Wipe:
        return "wipe";
    case Lifecycle::Trip:
        return "trip";
    case Lifecycle::Leave:
        return "leave";
    case Lifecycle::Join:
        return "join";
    case Lifecycle::NewCharacter:
        return "new character";
    case Lifecycle::MemberLeft:
        return "member left";
    }
    return "?";
}

size_t MemberDepartures::Tick(bool in_session, bool steady, size_t others, s64 now_ms) {
    if (!in_session) {
        known = 0;
        fewer_since.reset();
        return 0;
    }
    if (others >= known) {
        known = others;
        fewer_since.reset();
        return 0;
    }
    if (!steady) {
        fewer_since.reset();
        return 0;
    }
    if (!fewer_since) {
        fewer_since = now_ms;
        return 0;
    }
    if (now_ms - *fewer_since < SettleMs) {
        return 0;
    }
    const size_t left = known - others;
    known = others;
    fewer_since.reset();
    return left;
}

void LifecycleResets::Register(std::string name, std::initializer_list<Lifecycle> events,
                               std::function<void()> reset) {
    u32 bits = 0;
    for (const Lifecycle event : events) {
        bits |= 1u << static_cast<u32>(event);
    }
    entries.push_back(Entry{std::move(name), bits, std::move(reset)});
}

std::vector<std::string> LifecycleResets::Dispatch(Lifecycle event) {
    // A trip keeps the session (seamless travel), and so does another member leaving it;
    // everything else here starts a new one.
    if (event != Lifecycle::Trip && event != Lifecycle::MemberLeft) {
        ++epoch;
    }
    std::vector<std::string> ran;
    for (const Entry& entry : entries) {
        if ((entry.events & (1u << static_cast<u32>(event))) != 0) {
            entry.reset();
            ran.push_back(entry.name);
        }
    }
    return ran;
}

namespace {
template <typename T>
void AppendLittleEndian(std::vector<u8>& bytes, T value) {
    for (size_t index = 0; index < sizeof(T); ++index) {
        bytes.push_back(static_cast<u8>(static_cast<u64>(value) >> (8 * index)));
    }
}

template <typename T>
T ReadLittleEndian(std::span<const u8> bytes, size_t offset) {
    u64 value = 0;
    for (size_t index = 0; index < sizeof(T); ++index) {
        value |= u64{bytes[offset + index]} << (8 * index);
    }
    return static_cast<T>(value);
}
} // namespace

void SharedDamage::ForgetDamageBefore(std::span<const EnemyHpReport> others) {
    for (const EnemyHpReport& report : others) {
        before_reset.insert(report.visit);
    }
}

SharedDamage::Step SharedDamage::Update(u64 new_visit, bool now_active,
                                        std::span<const EnemyCopy> alive,
                                        std::span<const EnemyHpReport> others, s64 now_ms,
                                        PartyRole role) {
    // A new visit starts over. A switch between one world and worlds apart starts the others'
    // damage over (their posts restart: a guest's counts do, below) but not the host's counts:
    // its copies are the one world's, and what they lost there is its damage apart (rig
    // 20261013-011544: cleared here, the host's 284 damage in one world counted as none apart
    // while its one-world HP report went, and the guest's fresh copy stayed at full HP).
    const bool new_visit_now = new_visit != visit;
    const bool switched = !new_visit_now && now_active != active;
    if (new_visit_now || switched) {
        written.clear();
        written_by.clear();
        held_since.clear();
    }
    // A guest's drop in one world is the host's HP (the game's sync, LowerToHostHp) and its own
    // hits that reached the host by the game: the host posts all of it, so the guest counts from
    // where its copies stand.
    if (new_visit_now || (switched && role == PartyRole::Guest)) {
        tracks.clear();
    }
    visit = new_visit;
    active = now_active;
    Step step;
    for (const EnemyCopy& copy : alive) {
        if (copy.hp <= 0 || copy.max <= 0) {
            continue;
        }
        auto [track, first] = tracks.try_emplace(copy.handle, Track{copy.hp, 0});
        if (first) {
            track->second.hits_before = copy.own_hits.value_or(0);
        }
        Track& seen = track->second;
        if (switched && copy.own_hits) {
            if (active) {
                // Apart now: the guest's hits the game sent in one world become the host's.
                seen.hits_before -= seen.unclaimed;
            }
            seen.unclaimed = 0;
            seen.last_own = 0; // the last read's unclaimed drop while in one world
        }
        // This member's own damage: what its own hits took off (counted by the game's hit
        // function), or else everything the board did not write.
        const s32 hits = copy.own_hits ? static_cast<s32>(*copy.own_hits - seen.hits_before) : 0;
        // The rest of the drop: other players' hits the game sent (after a trip a guest's hits
        // still reach the host's copy), which pays for their posts before anything is written.
        s32 sent = seen.start - copy.hp - seen.written - hits;
        if (sent < 0) { // it came back up (a reload, a heal): count from here
            seen.start = copy.hp + seen.written + hits;
            seen.paid = 0;
            sent = 0;
        }
        const s32 own = copy.own_hits ? hits : sent;
        if (!active) {
            // One world (or no session): nothing is posted or written; the counts go on (see
            // the switch above). Counted hit by hit, the drop the host's hits did not take is
            // kept, two reads agreeing, as the debounce below does with `own`.
            if (copy.own_hits) {
                const s32 unclaimed = std::max(sent - seen.paid, 0);
                seen.unclaimed = std::min(unclaimed, seen.last_own);
                seen.last_own = unclaimed;
            } else {
                seen.last_own = own;
            }
            continue;
        }
        if (!copy.own_hits) {
            sent = 0; // not told apart: all of it is this member's
        }
        s32 unpaid = std::max(sent - seen.paid, 0);
        if (copy.own_hits) {
            // Counted hit by hit: a world showing its characters at 1 HP is no damage.
            if (own > 0) {
                step.post.push_back({copy.handle, own, copy.max});
            }
        } else {
            // Two reads must agree: a world showing its characters at 1 HP for one read (hidden
            // or loading) posted that as damage.
            const s32 confirmed = std::min(own, seen.last_own);
            seen.last_own = own;
            if (confirmed > 0) {
                step.post.push_back({copy.handle, confirmed, copy.max});
            }
        }
        // Each other member's damage on its copy, as a share of this copy's max HP, on top of
        // what this copy already took from that member (from the board or through the game).
        s32 hp = copy.hp;
        for (const EnemyHpReport& report : others) {
            if ((report.visit & HostHpReport) != 0) {
                continue; // HP from one world, not damage
            }
            if (before_reset.contains(report.visit)) {
                continue; // damage on the world before a map reset
            }
            for (const EnemyHp& damage : report.enemies) {
                if (damage.handle != copy.handle || damage.max <= 0 || damage.hp <= 0) {
                    continue;
                }
                const s32 scaled =
                    static_cast<s32>((s64{damage.hp} * copy.max + damage.max / 2) / damage.max);
                const std::pair<u64, u32> key{report.visit, copy.handle};
                s32& taken = written[key];
                s32 delta = scaled - taken;
                if (delta <= 0) {
                    continue;
                }
                const s32 paid = std::min(delta, unpaid);
                unpaid -= paid;
                seen.paid += paid;
                taken += paid;
                delta -= paid;
                if (delta <= 0) {
                    held_since.erase(key);
                    continue;
                }
                // A member whose hits the game sends here too: wait a moment for its copy.
                if (now_ms != 0 && game_sends.contains(report.visit)) {
                    const auto [since, fresh] = held_since.try_emplace(key, now_ms);
                    if (now_ms - since->second < SharedDamageHoldMs) {
                        continue;
                    }
                }
                held_since.erase(key);
                if (hp <= 1) {
                    continue;
                }
                const s32 lowered = std::max(hp - delta, 1);
                taken += hp - lowered;
                seen.written += hp - lowered;
                written_by[key] += hp - lowered;
                hp = lowered;
            }
        }
        // The game's copy of a hit already written from a report (it came straight over the P2P
        // link first): the write is given back, and that member's damage is held from now on.
        if (const s32 back = std::min(unpaid, seen.written); back > 0) {
            s32 left = back;
            for (auto& [key, amount] : written_by) {
                if (key.second != copy.handle || amount <= 0 || left <= 0) {
                    continue;
                }
                const s32 part = std::min(amount, left);
                amount -= part;
                left -= part;
                game_sends.insert(key.first);
            }
            seen.written -= back;
            seen.paid += back;
            hp = std::min(hp + back, copy.max);
        }
        if (hp != copy.hp) {
            step.writes.push_back({copy.handle, hp});
        }
    }
    return step;
}

void OwnDamage::Hit(u32 handle, bool mine, s32 taken) {
    last_hit_mine[handle] = mine;
    if (mine && taken > 0) {
        totals[handle] += taken;
    }
}

void OwnDamage::Hit(u32 handle, HitBy by, s32 taken) {
    if (by != HitBy::Nobody) {
        Hit(handle, by == HitBy::Me, taken);
        return;
    }
    const auto last = last_hit_mine.find(handle);
    if (taken > 0 && (last == last_hit_mine.end() || last->second)) {
        totals[handle] += taken;
    }
}

void OwnDamage::Effect(u32 handle, s32 taken) {
    const auto last = last_hit_mine.find(handle);
    if (taken > 0 && last != last_hit_mine.end() && last->second) {
        totals[handle] += taken;
    }
}

s64 OwnDamage::Total(u32 handle) const {
    const auto total = totals.find(handle);
    return total != totals.end() ? total->second : 0;
}

std::chrono::milliseconds HpBoardWait(bool own_damage_changed) {
    return own_damage_changed ? std::chrono::milliseconds{0} : HpBoardRound;
}

std::vector<u8> EncodeDirectDamage(u32 map, std::span<const u8> report) {
    std::vector<u8> frame{'S', 'H', 'A', 'D', DirectDamageType};
    frame.reserve(frame.size() + 4 + report.size());
    AppendLittleEndian(frame, map);
    frame.insert(frame.end(), report.begin(), report.end());
    return frame;
}

std::optional<MemberMapState> DecodeDirectDamage(std::span<const u8> frame) {
    constexpr size_t Header = 5 + 4;
    if (frame.size() < Header || !std::ranges::equal(frame.first(4), std::string_view{"SHAD"}) ||
        frame[4] != DirectDamageType) {
        return std::nullopt;
    }
    MemberMapState state{ReadLittleEndian<u32>(frame, 5), {}};
    state.bits.assign(frame.begin() + Header, frame.end());
    if (!DecodeEnemyHp(state.bits)) {
        return std::nullopt;
    }
    return state;
}

void DirectDamageInbox::Take(u64 sender, MemberMapState state, s64 now_ms) {
    by_sender[sender] = Received{std::move(state), now_ms};
}

std::vector<MemberMapState> DirectDamageInbox::Fresh(s64 now_ms) {
    std::erase_if(by_sender, [now_ms](const auto& entry) {
        return now_ms - entry.second.at > DirectDamageLifeMs;
    });
    std::vector<const Received*> fresh;
    for (const auto& [sender, received] : by_sender) {
        fresh.push_back(&received);
    }
    std::ranges::stable_sort(fresh, std::greater{}, &Received::at);
    std::vector<MemberMapState> states;
    for (const Received* received : fresh) {
        states.push_back(received->state);
    }
    return states;
}

std::vector<EnemyWrite> LowerToHostHp(std::span<const EnemyCopy> mine,
                                      std::span<const EnemyHpReport> reports) {
    std::vector<EnemyWrite> writes;
    for (const EnemyCopy& copy : mine) {
        std::optional<s32> lowest;
        for (const EnemyHpReport& report : reports) {
            if ((report.visit & HostHpReport) == 0) {
                continue;
            }
            for (const EnemyHp& enemy : report.enemies) {
                if (enemy.handle == copy.handle) {
                    if (const auto target = SharedEnemyHp(lowest.value_or(copy.hp), copy.max,
                                                          enemy.hp, enemy.max)) {
                        lowest = target;
                    }
                }
            }
        }
        if (lowest) {
            writes.push_back({copy.handle, *lowest});
        }
    }
    return writes;
}

std::optional<s32> LowestSharedHp(u32 handle, s32 hp, s32 max, u32 map,
                                  std::span<const MemberMapState> others) {
    std::optional<s32> lowest;
    for (const MemberMapState& state : others) {
        // Any block of the same area (packed map 0xAABBCCDD, area AA): a member who joined in
        // place stands on another block of it (Cathedral Ward next to Central Yharnam), and the
        // blocks give their enemies distinct handles; another area may reuse a handle.
        const auto report =
            (state.map >> 24) == (map >> 24) ? DecodeEnemyHp(state.bits) : std::nullopt;
        if (!report) {
            continue;
        }
        for (const EnemyHp& enemy : report->enemies) {
            if (enemy.handle != handle) {
                continue;
            }
            if (const auto target = SharedEnemyHp(lowest.value_or(hp), max, enemy.hp, enemy.max)) {
                lowest = target;
            }
        }
    }
    return lowest;
}

std::optional<s32> SharedEnemyHp(s32 hp, s32 max, s32 other_hp, s32 other_max) {
    if (hp <= 0 || max <= 0 || other_max <= 0 || other_hp < 0) {
        return std::nullopt;
    }
    // Rounded up: a sliver left on either side is still alive here.
    const s64 scaled = (s64{other_hp} * max + other_max - 1) / other_max;
    const s32 target = static_cast<s32>(std::max<s64>(scaled, 1));
    return target < hp ? std::optional<s32>{target} : std::nullopt;
}

bool IsDeadInItsWorld(u8 death_flags) {
    return (death_flags & 0x05) != 0;
}

bool IsDeadInItsWorld(u8 death_flags, s32 hp, s32 max) {
    return IsDeadInItsWorld(death_flags) || (max > 0 && hp <= 0);
}

std::unordered_set<u32> DeadInTheHostsWorld(std::span<const CharacterShown> host) {
    std::unordered_set<u32> dead;
    for (const CharacterShown& character : host) {
        if (character.dead) {
            dead.insert(character.handle);
        }
    }
    return dead;
}

bool DiesInsteadOfHiding(const std::optional<LampPosition>& me,
                         const std::optional<LampPosition>& enemy) {
    if (!me || !enemy) {
        return false;
    }
    float squared = 0.0f;
    for (size_t axis = 0; axis < 3; ++axis) {
        const float delta = (*enemy)[axis] - (*me)[axis];
        squared += delta * delta;
    }
    return squared < DiesNearMetres * DiesNearMetres;
}

bool FarFromHost(const std::optional<LampPosition>& host,
                 const std::optional<LampPosition>& character) {
    if (!host || !character) {
        return false;
    }
    float squared = 0.0f;
    for (size_t axis = 0; axis < 3; ++axis) {
        const float delta = (*character)[axis] - (*host)[axis];
        squared += delta * delta;
    }
    return squared > FarFromHostMetres * FarFromHostMetres;
}

std::vector<CharacterShown> ShownLikeTheHost(std::span<const CharacterShown> host,
                                             std::span<const CharacterShown> mine,
                                             const std::unordered_set<u32>& kill_hidden,
                                             const std::unordered_set<u32>& hidden_here) {
    std::unordered_map<u32, CharacterShown> hosts;
    for (const CharacterShown& character : host) {
        hosts[character.handle] = character;
    }
    std::vector<CharacterShown> changes;
    for (const CharacterShown& character : mine) {
        const auto it = hosts.find(character.handle);
        if (it == hosts.end() || kill_hidden.contains(character.handle)) {
            continue;
        }
        // Far from the host: only undo what this member did.
        const bool undo = character.hidden && hidden_here.contains(character.handle);
        const bool hidden = it->second.distant ? character.hidden && !undo : it->second.hidden;
        if (hidden != character.hidden) {
            changes.push_back({character.handle, hidden});
        }
    }
    return changes;
}

std::vector<CharacterShown> HostNearStates::Fill(u32 host_map,
                                                 std::span<const CharacterShown> host) {
    if (host_map != map) {
        map = host_map;
        near_states.clear();
    }
    std::vector<CharacterShown> filled;
    filled.reserve(host.size());
    for (const CharacterShown& character : host) {
        if (!character.distant) {
            near_states[character.handle] = character;
            filled.push_back(character);
        } else if (const auto it = near_states.find(character.handle); it != near_states.end()) {
            filled.push_back(it->second);
        } else {
            filled.push_back(character);
        }
    }
    return filled;
}

std::optional<s32> AmbushSlotFlag(s32 entity) {
    constexpr s32 FirstEnemy = 2200500;
    constexpr s32 FirstSlotFlag = 12204000;
    constexpr s32 Slots = 9;
    if (entity < FirstEnemy || entity >= FirstEnemy + Slots) {
        return std::nullopt;
    }
    return FirstSlotFlag + (entity - FirstEnemy);
}

std::vector<CharacterSwitch> UndoneOnMapChange(const std::unordered_set<u32>& switched_off,
                                               std::span<const CharacterSwitch> mine) {
    std::vector<CharacterSwitch> changes;
    for (const CharacterSwitch& character : mine) {
        if (character.disabled && switched_off.contains(character.handle)) {
            changes.push_back({character.handle, false});
        }
    }
    return changes;
}

std::vector<CharacterSwitch> SwitchedLikeTheHost(std::span<const CharacterShown> host,
                                                 std::span<const CharacterSwitch> mine,
                                                 const std::unordered_set<u32>& switched_off,
                                                 const std::unordered_set<u32>& kill_hidden,
                                                 const std::unordered_set<u32>& off_for_good) {
    struct HostSwitch {
        bool off = false; // removed or switched off near the host (or for good)
        bool on = false;  // on near the host, alive
    };
    std::unordered_map<u32, HostSwitch> hosts;
    for (const CharacterShown& character : host) {
        const bool off = character.hidden || character.disabled;
        const bool followed = !character.distant || off_for_good.contains(character.handle);
        hosts[character.handle] = {followed && off, !character.distant && !off && !character.dead};
    }
    std::vector<CharacterSwitch> changes;
    for (const CharacterSwitch& character : mine) {
        const auto it = hosts.find(character.handle);
        if (it == hosts.end() || kill_hidden.contains(character.handle)) {
            continue;
        }
        // A boss is its own fight's: only what this member switched off itself comes back on.
        const bool own_off = switched_off.contains(character.handle);
        if (it->second.off && !character.disabled && !character.boss) {
            changes.push_back({character.handle, true});
        } else if (!it->second.off && character.disabled &&
                   (own_off || (it->second.on && !character.dead && !character.boss))) {
            changes.push_back({character.handle, false});
        }
    }
    return changes;
}

std::vector<CharacterHits> HitsLikeTheHost(std::span<const CharacterShown> host,
                                           std::span<const CharacterHits> mine,
                                           const std::unordered_set<u32>& kill_hidden) {
    std::unordered_map<u32, bool> hosts; // handle -> hits off, for those on near the host
    for (const CharacterShown& character : host) {
        if (!character.distant && !character.hidden && !character.disabled && !character.dead) {
            hosts[character.handle] = character.hits_off;
        }
    }
    std::vector<CharacterHits> changes;
    for (const CharacterHits& character : mine) {
        const auto it = hosts.find(character.handle);
        if (it != hosts.end() && it->second != character.off &&
            !kill_hidden.contains(character.handle)) {
            changes.push_back({character.handle, it->second});
        }
    }
    return changes;
}

bool WatchesKills(bool sharing, bool trip_active, bool loading) {
    return sharing && !trip_active && !loading;
}

bool CharacterSettles(s64 now_ms, s64 world_changed_ms, std::optional<s64> owner_changed_ms) {
    const s64 settle =
        std::chrono::duration_cast<std::chrono::milliseconds>(EnemyOwnerChangeSettles).count();
    return now_ms - world_changed_ms < settle ||
           (owner_changed_ms && now_ms - *owner_changed_ms < settle);
}

bool SettlingHidesDeath(bool settling, std::optional<bool> last_hit_mine) {
    return settling && last_hit_mine != true;
}

bool SeenDead(u8 death_flags, s32 hp, std::optional<s64> zero_hit_ms, s64 now_ms) {
    constexpr u8 PutDownByTheWorld = 0x20;
    if (IsRealDeath(death_flags)) {
        return true;
    }
    return hp <= 0 && (death_flags & PutDownByTheWorld) == 0 && zero_hit_ms &&
           now_ms - *zero_hit_ms <= ZeroHitCountsMs;
}

void OwnDamage::HitLeft(u32 handle, s32 hp, s64 now_ms) {
    if (hp <= 0) {
        zero_hit_ms[handle] = now_ms;
    } else {
        zero_hit_ms.erase(handle);
    }
}

std::optional<s64> OwnDamage::ZeroHitMs(u32 handle) const {
    const auto hit = zero_hit_ms.find(handle);
    return hit != zero_hit_ms.end() ? std::optional{hit->second} : std::nullopt;
}

bool FinishesStuckDeath(bool killed_by_member, s32 hp, u8 death_flags,
                        std::optional<s64> zero_since_ms, s64 now_ms) {
    constexpr u8 PutDownByTheWorld = 0x20;
    return killed_by_member && hp <= 0 && !IsRealDeath(death_flags) &&
           (death_flags & PutDownByTheWorld) == 0 && zero_since_ms &&
           now_ms - *zero_since_ms >= StuckDeathMs;
}

bool IsRealDeath(u8 death_flags) {
    constexpr u8 PutDownByTheWorld = 0x20;
    return (death_flags & 0x05) != 0 && (death_flags & PutDownByTheWorld) == 0;
}

std::vector<std::pair<s32, bool>> VisitKills::Observe(u64 visit,
                                                      std::span<const SeenCharacter> characters,
                                                      std::span<const u32> others_maps, u32 map) {
    std::vector<std::pair<s32, bool>> changes;
    const auto map_of = [](u64 v) { return static_cast<u32>(v >> 32); };
    const auto someone_on = [&](u32 on) {
        return on != 0 && std::ranges::any_of(others_maps, [&](u32 other) {
                   return other != 0 && MapsLoadedTogether(other, on);
               });
    };
    if (visit != current) {
        for (const u32 handle : killed) {
            const u32 on = killed_on[handle];
            if (!someone_on(on)) {
                changes.emplace_back(static_cast<s32>(handle), false);
            } else if (auto& standing = held[on];
                       std::ranges::find(standing, handle) == standing.end()) {
                standing.push_back(handle);
            }
        }
        killed.clear();
        killed_on.clear();
        seen.clear();
        current = visit;
    }
    const u32 here = map != 0 ? map : map_of(current);
    for (auto it = held.begin(); it != held.end();) {
        if (MapsLoadedTogether(it->first, here) || someone_on(it->first)) {
            ++it;
            continue;
        }
        for (const u32 handle : it->second) {
            changes.emplace_back(static_cast<s32>(handle), false);
        }
        it = held.erase(it);
    }
    for (const auto& [handle, now, instance, settling, untouched_boss, others_kill] : characters) {
        const auto [it, added] = seen.try_emplace(handle, Seen{now, instance, untouched_boss});
        // The same object dead, now alive: a map generator brought it back (the chalice widow's
        // skeletons, the Witch of Hemwick's Mad Ones; Kaniel 2026-10-20). Its kill is taken back.
        if (const auto at = std::ranges::find(killed, handle);
            !added && !settling && at != killed.end() && it->second.instance == instance &&
            it->second.hp <= 0 && now > 0) {
            killed.erase(at);
            killed_on.erase(handle);
            changes.emplace_back(static_cast<s32>(handle), false);
            it->second = {now, instance, untouched_boss};
            continue;
        }
        // The same object seen alive, now dead: a world that loaded brings new objects, some
        // dead already (in a save that killed them for good), and one that settles after a
        // takeover puts such ones down where they stand. Nor a boss-sized one last seen
        // untouched (see SeenCharacter::untouched_boss), nor another member's kill.
        if (!added && !settling && !others_kill && it->second.instance == instance &&
            it->second.hp > 0 && !it->second.untouched_boss && now <= 0 &&
            std::ranges::find(killed, handle) == killed.end()) {
            killed.push_back(handle);
            killed_on[handle] = here;
            changes.emplace_back(static_cast<s32>(handle), true);
        }
        it->second = {now, instance, untouched_boss};
    }
    return changes;
}

std::vector<u32> VisitKills::Held(u32 map) const {
    const auto it = held.find(map);
    return it != held.end() ? it->second : std::vector<u32>{};
}

WorldLogKills VisitKills::Standing() const {
    WorldLogKills kills;
    for (const auto& [map, handles] : held) {
        kills[map].insert(handles.begin(), handles.end());
    }
    for (const auto& [handle, map] : killed_on) {
        kills[map].insert(handle);
    }
    return kills;
}

std::vector<std::pair<s32, bool>> VisitKills::Forget() {
    std::vector<std::pair<s32, bool>> changes;
    for (const u32 handle : killed) {
        changes.emplace_back(static_cast<s32>(handle), false);
    }
    for (const auto& [map, handles] : held) {
        for (const u32 handle : handles) {
            if (std::ranges::find(killed, handle) == killed.end()) {
                changes.emplace_back(static_cast<s32>(handle), false);
            }
        }
    }
    killed.clear();
    killed_on.clear();
    held.clear();
    seen.clear();
    return changes;
}

std::vector<std::pair<s32, bool>> VisitKills::ForgetOn(std::span<const u32> maps) {
    std::vector<std::pair<s32, bool>> changes;
    const auto reset = [&](u32 map) { return std::ranges::find(maps, map) != maps.end(); };
    std::erase_if(killed, [&](u32 handle) {
        if (!reset(killed_on[handle])) {
            return false;
        }
        changes.emplace_back(static_cast<s32>(handle), false);
        killed_on.erase(handle);
        return true;
    });
    for (auto it = held.begin(); it != held.end();) {
        if (!reset(it->first)) {
            ++it;
            continue;
        }
        for (const u32 handle : it->second) {
            if (std::ranges::none_of(changes, [&](const auto& change) {
                    return change.first == static_cast<s32>(handle);
                })) {
                changes.emplace_back(static_cast<s32>(handle), false);
            }
        }
        it = held.erase(it);
    }
    return changes;
}

bool AppliesMemberKill(bool member_killed, s32 hp) {
    return member_killed && hp > 0;
}

bool CopyJustMade(s64 now_ms, s64 seen_ms) {
    return now_ms - seen_ms < NewCopySettlesMs;
}

bool KilledByAnother(bool hooks, std::optional<bool> last_hit_mine, bool hosting,
                     bool runs_enemies) {
    if (!hooks || runs_enemies) {
        return false;
    }
    return last_hit_mine ? !*last_hit_mine : !hosting;
}

bool GetsMemberKillDrop(bool in_session, bool member_kill, s32 lot_flag, bool flag_on) {
    return in_session && member_kill && lot_flag > 0 && !flag_on;
}

bool GetsKillDrop(bool in_session, bool member_kill, bool own_kill_as_guest, s32 lot_flag,
                  bool flag_on) {
    return in_session && (member_kill || own_kill_as_guest) && lot_flag > 0 && !flag_on;
}

bool SharesWithParty(bool party_configured, bool player_loaded) {
    return party_configured && player_loaded;
}

bool SharesKillEchoes(u64 caller_offset, bool posts) {
    constexpr u64 EnemyKillReward = 0x018FC99B;
    constexpr u64 BossKillReward = 0x01385263;
    return posts && (caller_offset == EnemyKillReward || caller_offset == BossKillReward);
}

void KillEchoLedger::Paid(KillEchoes kill, s64 now_ms) {
    paid.emplace_back(kill, now_ms);
}

void KillEchoLedger::Received(KillEchoes kill, s64 now_ms) {
    received.emplace_back(kill, now_ms);
}

std::vector<KillEchoes> KillEchoLedger::Due(s64 now_ms) {
    std::vector<KillEchoes> due;
    std::vector<std::pair<KillEchoes, s64>> waiting;
    for (const auto& [kill, read_ms] : received) {
        const auto reward = std::ranges::find_if(paid, [&](const auto& entry) {
            return entry.first.amount == kill.amount &&
                   (kill.map == 0 || entry.first.map == kill.map) &&
                   entry.second >= read_ms - PaidWindowMs &&
                   entry.second <= read_ms + ReceivedGraceMs;
        });
        if (reward != paid.end()) {
            paid.erase(reward); // the game paid this member: it was on the kill's map
        } else if (now_ms - read_ms < ReceivedGraceMs) {
            waiting.emplace_back(kill, read_ms);
        } else {
            due.push_back(kill);
        }
    }
    received = std::move(waiting);
    std::erase_if(paid, [&](const auto& entry) {
        return entry.second < now_ms - PaidWindowMs - ReceivedGraceMs;
    });
    return due;
}

std::string EncodeEchoUpdate(std::string_view member, s64 since,
                             std::span<const KillEchoes> echoes) {
    std::string body = std::string{member} + ' ' + std::to_string(since);
    for (const KillEchoes& kill : echoes) {
        body += ' ' + std::to_string(kill.amount);
        if (kill.map != 0) {
            body += ':' + std::to_string(kill.map);
        }
    }
    return body;
}

std::optional<KillBoardAnswer> ParseKillBoard(std::string_view body) {
    KillBoardAnswer answer;
    const auto first_end = body.find('\n');
    const std::string_view first = body.substr(0, first_end);
    const auto [latest_end, latest_error] =
        std::from_chars(first.data(), first.data() + first.size(), answer.latest);
    if (latest_error != std::errc{} || latest_end != first.data() + first.size()) {
        return std::nullopt;
    }
    body = first_end == std::string_view::npos ? std::string_view{} : body.substr(first_end + 1);
    while (!body.empty()) {
        const auto end = body.find('\n');
        const std::string_view line = body.substr(0, end);
        body = end == std::string_view::npos ? std::string_view{} : body.substr(end + 1);
        if (line.empty()) {
            continue;
        }
        const auto space = line.find(' ');
        s32 handle = 0;
        const auto [handle_end, handle_error] =
            std::from_chars(line.data(), line.data() + std::min(space, line.size()), handle);
        if (space == std::string_view::npos || handle_error != std::errc{} ||
            handle_end != line.data() + space || line.size() < space + 2 ||
            (line[space + 1] != '0' && line[space + 1] != '1') ||
            (line.size() > space + 2 && line[space + 2] != ' ')) {
            return std::nullopt;
        }
        const std::string_view member =
            line.size() > space + 3 ? line.substr(space + 3) : std::string_view{};
        answer.changes.push_back(KillChange{handle, line[space + 1] == '1', std::string{member}});
    }
    return answer;
}

void PartyKills::Apply(std::span<const KillChange> changes) {
    for (const KillChange& change : changes) {
        const u32 handle = static_cast<u32>(change.handle);
        if (change.killed) {
            by_member[change.member].insert(handle);
        } else if (const auto it = by_member.find(change.member); it != by_member.end()) {
            it->second.erase(handle);
        }
    }
}

bool PartyKills::Contains(u32 handle) const {
    return std::ranges::any_of(by_member,
                               [&](const auto& entry) { return entry.second.contains(handle); });
}

std::unordered_set<u32> PartyKills::All() const {
    std::unordered_set<u32> all;
    for (const auto& [member, handles] : by_member) {
        all.insert(handles.begin(), handles.end());
    }
    return all;
}

std::unordered_set<u32> PartyKills::AllExcept(std::string_view member) const {
    std::unordered_set<u32> all;
    for (const auto& [name, handles] : by_member) {
        if (name != member) {
            all.insert(handles.begin(), handles.end());
        }
    }
    return all;
}

std::optional<EchoBoardAnswer> ParseEchoBoard(std::string_view body) {
    EchoBoardAnswer answer;
    bool first = true;
    while (!body.empty()) {
        const auto end = body.find('\n');
        const std::string_view line = body.substr(0, end);
        body = end == std::string_view::npos ? std::string_view{} : body.substr(end + 1);
        if (line.empty() && !first) {
            continue;
        }
        if (first) {
            const auto [latest_end, error] =
                std::from_chars(line.data(), line.data() + line.size(), answer.latest);
            if (error != std::errc{} || latest_end != line.data() + line.size()) {
                return std::nullopt;
            }
            first = false;
            continue;
        }
        const auto space = line.find(' ');
        const std::string_view amount_text = line.substr(0, space);
        s32 amount = 0;
        const auto [amount_end, error] =
            std::from_chars(amount_text.data(), amount_text.data() + amount_text.size(), amount);
        if (error != std::errc{} || amount_end != amount_text.data() + amount_text.size() ||
            amount <= 0) {
            return std::nullopt;
        }
        u32 map = 0;
        if (space != std::string_view::npos) {
            const std::string_view map_text = line.substr(space + 1);
            const auto [map_end, map_error] =
                std::from_chars(map_text.data(), map_text.data() + map_text.size(), map);
            if (map_error != std::errc{} || map_end != map_text.data() + map_text.size()) {
                return std::nullopt;
            }
        }
        answer.echoes.push_back(KillEchoes{amount, map});
    }
    if (first) {
        return std::nullopt;
    }
    return answer;
}

bool EndsStaleSessionEffect(PartyRole role, bool in_session, std::span<const s32> effects) {
    return role == PartyRole::Guest && !in_session &&
           std::ranges::find(effects, SessionEffect) != effects.end();
}

std::vector<s32> EffectsToRestoreAfterTrip(std::span<const s32> before,
                                           std::span<const s32> after) {
    std::vector<s32> missing;
    for (const s32 effect : before) {
        // The bells' effects; 9001 (in a session as host) the game puts back itself.
        constexpr std::array<s32, 4> BellEffects{9000, 9003, 9005, 9006};
        if (std::ranges::find(BellEffects, effect) != BellEffects.end() &&
            std::ranges::find(after, effect) == after.end()) {
            missing.push_back(effect);
        }
    }
    return missing;
}

bool GetsImpurityBonus(bool in_party_session, bool party_guest) {
    return in_party_session && (party_guest || ImpurityBonusForHost);
}

EffectChange ImpurityBonusChange(bool gets_bonus, std::span<const s32> effects, bool added) {
    const bool wears = std::ranges::find(effects, ImpurityRuneEffect) != effects.end();
    const bool has = std::ranges::find(effects, ImpurityHpEffect) != effects.end();
    if (gets_bonus && wears) {
        return has ? EffectChange::None : EffectChange::Add;
    }
    return has && added ? EffectChange::Remove : EffectChange::None;
}

bool ShouldRestoreSearchAfterTrip(bool in_party_session, bool search_effect_active) {
    return in_party_session && !search_effect_active;
}

s32 LocalTaskKind(bool hosting) {
    return hosting ? 3 : 2;
}

bool ReopensClosedSession(bool party, s32 matching_state, u8 closing) {
    return party && matching_state == MatchingStateHosting && closing != 0;
}

s32 RemoteTaskKind(bool hosting) {
    return hosting ? RemoteGuestTask : RemoteHostTask;
}

bool RecreateIsForAJoin(bool pending, bool for_join) {
    return !pending || for_join;
}

u8 RebuiltTaskFlags(u8 constructed, const SavedJoinTask* saved, bool waiting) {
    if (!saved) {
        return constructed & JoinTaskFlagBits;
    }
    return (waiting ? saved->flags : static_cast<u8>(saved->flags | 1)) & JoinTaskFlagBits;
}

s32 RebuiltTaskKind(bool hosting, std::span<const SavedJoinTask> saved,
                    std::span<const SavedJoinTask> live, u64 identity) {
    if (hosting) {
        return RemoteGuestTask;
    }
    for (const SavedJoinTask& task : live) {
        if ((task.kind == RemoteHostTask || task.kind == RemoteGuestTask) &&
            task.identity == identity) {
            return task.kind;
        }
    }
    const auto is_host = [](const SavedJoinTask& task) { return task.kind == RemoteHostTask; };
    if (std::ranges::any_of(live, is_host)) {
        return RemoteGuestTask; // a session has one host
    }
    bool host_elsewhere = false;
    for (const SavedJoinTask& task : saved) {
        if (task.kind != RemoteHostTask && task.kind != RemoteGuestTask) {
            continue; // this player's own task
        }
        if (task.identity == identity) {
            return task.kind;
        }
        host_elsewhere |= task.kind == RemoteHostTask;
    }
    return host_elsewhere ? RemoteGuestTask : RemoteHostTask;
}

bool TalksAsHost(bool in_party_session, s32 character_type) {
    return in_party_session && character_type == WhiteGhostCharacterType;
}

bool SettingIsOn(const char* setting) {
    return setting != nullptr && std::string_view{setting} == "1";
}

bool ShouldPlayGuestAsHost(bool switched_on, bool in_party_session, s32 character_type) {
    return switched_on && in_party_session && character_type == WhiteGhostCharacterType;
}

bool ShowsGuestBloodstain(bool party_guest, bool in_session, bool loading, bool has_stain,
                          s32 echoes, u32 record_block, u32 map, bool shown, int tries) {
    const bool same_map = map != 0 && (record_block >> 16) == (map >> 16);
    return party_guest && in_session && !loading && has_stain && echoes > 0 && same_map && !shown &&
           tries < GuestBloodstainTries;
}

bool RestoresBloodstainMark(bool party_guest, bool mark_before, bool mark_now, s32 echoes,
                            u32 block) {
    return party_guest && mark_before && !mark_now && echoes > 0 && block != 0xFFFFFFFF;
}

bool LevelSyncsGuest(bool in_party_session, bool above_host_band) {
    return above_host_band && !in_party_session;
}

bool NpcTurnsIntoWanderGhost(bool in_party_session, bool param_says_wander_ghost) {
    return param_says_wander_ghost && !in_party_session;
}

bool IsGameVtable(u64 vtable, u64 image_base, std::span<const u64> vtables) {
    return image_base != 0 && vtable >= image_base &&
           std::ranges::find(vtables, vtable - image_base) != vtables.end();
}

bool RefusedCallLog::FirstTime(std::string_view site, u64 vtable) {
    if (logged.size() >= MaxLogged) {
        return false;
    }
    return logged.emplace(std::string{site}, vtable).second;
}

std::string DescribeVtable(u64 vtable, u64 image_base, u64 image_size) {
    if (image_base != 0 && vtable >= image_base && vtable - image_base < image_size) {
        return fmt::format("eboot+{:#x}", vtable - image_base);
    }
    return fmt::format("{:#x}", vtable);
}

const std::array<u8, GuestItemPickupAllowedOffset + 8>& SoloMapItemStandIn() {
    static const auto stand_in = [] {
        std::array<u8, GuestItemPickupAllowedOffset + 8> bytes{};
        constexpr s32 no_session = -1;
        std::memcpy(bytes.data() + JoinedSessionOffset, &no_session, sizeof(no_session));
        bytes[GuestItemPickupAllowedOffset] = 1;
        return bytes;
    }();
    return stand_in;
}

bool DefersItemLotAward(bool party_configured, bool summoned_guest, u64 return_offset) {
    return party_configured && summoned_guest && return_offset == ClientItemLotAwardReturn;
}

std::optional<NpcGift> NpcGiftFor(s32 trigger) {
    for (const NpcGift& gift : NpcGifts) {
        if (gift.trigger == trigger) {
            return gift;
        }
    }
    return std::nullopt;
}

std::optional<s32> NpcGiftLot(s32 trigger) {
    const auto gift = NpcGiftFor(trigger);
    return gift ? std::optional<s32>{gift->lot} : std::nullopt;
}

bool GivesGuestNpcGift(bool in_party_session, bool summoned_guest, s32 flag) {
    return in_party_session && summoned_guest && NpcGiftLot(flag).has_value();
}

bool MenuSeesAClient(bool party_configured, bool summoned_client) {
    return summoned_client && !party_configured;
}

bool GuestHandlesMapItemsAlone(bool party_configured) {
    return party_configured;
}

std::array<u8, 8> RetargetRipLoad(std::span<const u8, 8> original, u64 address, u64 target) {
    std::array<u8, 8> patched{};
    std::copy(original.begin(), original.end(), patched.begin());
    const s32 displacement =
        static_cast<s32>(static_cast<s64>(target) - static_cast<s64>(address + patched.size()));
    std::memcpy(patched.data() + 4, &displacement, sizeof(displacement));
    return patched;
}

u32 MapItemPacketHeaderSeen(u32 header, bool party_configured) {
    return party_configured ? 0 : header;
}

u32 PackedMapOfWarpPoint(s32 warp_point) {
    const u32 area = static_cast<u32>(warp_point / 100000);
    const u32 block = static_cast<u32>(warp_point / 10000 % 10);
    return area << 24 | (block & 0xFF) << 16;
}

bool IsHeadstoneTravelFlag(s32 flag) {
    return RuleFor(flag).Has(FlagHeadstoneTrip);
}

s32 HeadstoneWarpPoint(s32 flag) {
    return flag % 10000 / 10 * 10000 + 2950 + flag % 10;
}

bool GuestMakesItsOwnLampTrip(bool in_party_session, PartyRole role) {
    return in_party_session && role == PartyRole::Guest;
}

std::vector<MultiplayerWall> MultiplayerWalls() {
    // InitializeEvent(_, 7600, object, sfx) in m22-m35's event files (01.09).
    static constexpr std::array<s32, 18> Walls{
        2201999, 2301999, 2401999, 2401998, 2401997, 2401996, 2401995, 2411999, 2411998,
        2411997, 2421999, 2421998, 2701999, 2701998, 2801999, 2801998, 3201999, 3501990,
    };
    std::vector<MultiplayerWall> walls;
    for (const s32 object : Walls) {
        walls.push_back(MultiplayerWall{object, object + 2000});
    }
    return walls;
}

bool IsParamOfType(std::span<const u8> param, std::string_view type) {
    if (param.size() < 0x0C + type.size() + 1) {
        return false;
    }
    return std::equal(type.begin(), type.end(), param.begin() + 0x0C) &&
           param[0x0C + type.size()] == 0;
}

std::optional<u64> FindParamRow(std::span<const u8> param, s32 id) {
    if (param.size() < 0x40) {
        return std::nullopt;
    }
    u16 rows = 0;
    std::memcpy(&rows, param.data() + 0x0A, sizeof(rows));
    if (param.size() < 0x40 + std::size_t{rows} * 24) {
        return std::nullopt;
    }
    std::size_t low = 0;
    std::size_t high = rows;
    while (low < high) {
        const std::size_t middle = (low + high) / 2;
        s32 row_id = 0;
        std::memcpy(&row_id, param.data() + 0x40 + middle * 24, sizeof(row_id));
        if (row_id == id) {
            u64 offset = 0;
            std::memcpy(&offset, param.data() + 0x40 + middle * 24 + 8, sizeof(offset));
            return offset;
        }
        if (row_id < id) {
            low = middle + 1;
        } else {
            high = middle;
        }
    }
    return std::nullopt;
}

std::optional<HarderEnemies> ParseHarderEnemies(std::string_view setting) {
    std::array<float, 3> rates{};
    std::size_t count = 0;
    while (true) {
        const std::size_t comma = setting.find(',');
        std::string_view part = setting.substr(0, comma);
        while (!part.empty() && IsWhitespace(part.front())) {
            part.remove_prefix(1);
        }
        while (!part.empty() && IsWhitespace(part.back())) {
            part.remove_suffix(1);
        }
        float rate = 0.0f;
        const auto [end, error] = std::from_chars(part.data(), part.data() + part.size(), rate);
        if (count == rates.size() || part.empty() || error != std::errc{} ||
            end != part.data() + part.size() || !(rate >= 1.0f && rate <= 10.0f)) {
            return std::nullopt;
        }
        rates[count++] = rate;
        if (comma == std::string_view::npos) {
            break;
        }
        setting.remove_prefix(comma + 1);
    }
    if (count != rates.size() || (rates[0] == 1.0f && rates[1] == 1.0f && rates[2] == 1.0f)) {
        return std::nullopt;
    }
    return HarderEnemies{rates[0], rates[1], rates[2]};
}

u32 HarderEnemyHp(u32 hp, float rate) {
    if (hp <= 1) {
        return hp;
    }
    const double scaled = std::round(static_cast<double>(hp) * rate);
    return static_cast<u32>(std::min(scaled, 9'999'999.0));
}

s16 HarderEnemyPoise(s16 poise, u32 hp, float rate) {
    if (rate <= 1.0f || hp <= 1 || poise >= 9999 || poise < 0) {
        return poise;
    }
    const double floor = (hp >= 1000 ? 50.0 : 15.0) * rate;
    const double scaled = std::max(std::round(static_cast<double>(poise) * rate), floor);
    return static_cast<s16>(std::min(scaled, 9998.0));
}

u16 HarderEnemyAttack(u16 attack, float rate) {
    const double scaled = std::round(static_cast<double>(attack) * rate);
    return static_cast<u16>(std::min(scaled, 65535.0));
}

namespace {

// Calls `scale` with each distinct row's data that holds `row_end` bytes; returns how many.
template <typename Scale>
std::size_t ForEachParamRow(std::span<u8> param, std::string_view type, u64 row_end,
                            Scale&& scale) {
    if (!IsParamOfType(param, type)) {
        return 0;
    }
    u16 rows = 0;
    std::memcpy(&rows, param.data() + 0x0A, sizeof(rows));
    if (param.size() < 0x40 + std::size_t{rows} * 24) {
        return 0;
    }
    std::vector<u64> seen;
    std::size_t changed = 0;
    for (std::size_t row = 0; row < rows; ++row) {
        u64 data = 0;
        std::memcpy(&data, param.data() + 0x40 + row * 24 + 8, sizeof(data));
        if (data < 0x40 + std::size_t{rows} * 24 || data > param.size() ||
            param.size() - data < row_end ||
            std::find(seen.begin(), seen.end(), data) != seen.end()) {
            continue;
        }
        seen.push_back(data);
        if (scale(param.data() + data)) {
            ++changed;
        }
    }
    return changed;
}

} // namespace

std::size_t MakeNpcParamHarder(std::span<u8> param, const HarderEnemies& harder) {
    return ForEachParamRow(param, "NPC_PARAM_ST", NpcPoiseOffset + 2, [&](u8* row) {
        u32 hp = 0;
        s16 poise = 0;
        std::memcpy(&hp, row + NpcHpOffset, sizeof(hp));
        std::memcpy(&poise, row + NpcPoiseOffset, sizeof(poise));
        const u32 new_hp = HarderEnemyHp(hp, harder.hp);
        const s16 new_poise = HarderEnemyPoise(poise, hp, harder.poise);
        std::memcpy(row + NpcHpOffset, &new_hp, sizeof(new_hp));
        std::memcpy(row + NpcPoiseOffset, &new_poise, sizeof(new_poise));
        return new_hp != hp || new_poise != poise;
    });
}

std::size_t RemoveCoopBossHpBonus(std::span<u8> sp_effect_param) {
    if (!IsParamOfType(sp_effect_param, "SP_EFFECT_PARAM_ST")) {
        return 0;
    }
    std::size_t changed = 0;
    for (const s32 effect : CoopBossHpEffects) {
        const auto row = FindParamRow(sp_effect_param, effect);
        if (!row || *row > sp_effect_param.size() ||
            sp_effect_param.size() - *row < SpEffectMaxHpRateOffset + sizeof(float)) {
            continue;
        }
        float rate = 0.0f;
        std::memcpy(&rate, sp_effect_param.data() + *row + SpEffectMaxHpRateOffset, sizeof(rate));
        if (rate != 1.0f) {
            rate = 1.0f;
            std::memcpy(sp_effect_param.data() + *row + SpEffectMaxHpRateOffset, &rate,
                        sizeof(rate));
            ++changed;
        }
    }
    return changed;
}

std::vector<std::pair<s32, float>> CoopBossHpRates(std::span<const u8> sp_effect_param) {
    std::vector<std::pair<s32, float>> rates;
    if (!IsParamOfType(sp_effect_param, "SP_EFFECT_PARAM_ST")) {
        return rates;
    }
    for (const s32 effect : CoopBossHpEffects) {
        const auto row = FindParamRow(sp_effect_param, effect);
        if (!row || *row > sp_effect_param.size() ||
            sp_effect_param.size() - *row < SpEffectMaxHpRateOffset + sizeof(float)) {
            continue;
        }
        float rate = 0.0f;
        std::memcpy(&rate, sp_effect_param.data() + *row + SpEffectMaxHpRateOffset, sizeof(rate));
        rates.emplace_back(effect, rate);
    }
    return rates;
}

void SetCoopBossHpRates(std::span<u8> sp_effect_param,
                        std::span<const std::pair<s32, float>> rates) {
    if (!IsParamOfType(sp_effect_param, "SP_EFFECT_PARAM_ST")) {
        return;
    }
    for (const auto& [effect, rate] : rates) {
        const auto row = FindParamRow(sp_effect_param, effect);
        if (!row || *row > sp_effect_param.size() ||
            sp_effect_param.size() - *row < SpEffectMaxHpRateOffset + sizeof(float)) {
            continue;
        }
        std::memcpy(sp_effect_param.data() + *row + SpEffectMaxHpRateOffset, &rate, sizeof(rate));
    }
}

std::size_t MakeNpcAttacksHarder(std::span<u8> param, float damage) {
    if (!FindParamRow(param, NpcAttackMarkerRow)) {
        return 0;
    }
    return ForEachParamRow(param, "ATK_PARAM_ST", AtkPhysOffset + 8, [&](u8* row) {
        bool changed = false;
        for (u64 field = AtkPhysOffset; field < AtkPhysOffset + 8; field += 2) {
            u16 attack = 0;
            std::memcpy(&attack, row + field, sizeof(attack));
            const u16 scaled = HarderEnemyAttack(attack, damage);
            std::memcpy(row + field, &scaled, sizeof(scaled));
            changed |= scaled != attack;
        }
        return changed;
    });
}

std::span<const PolishedMessage> PolishedMessages() {
    static constexpr std::array<PolishedMessage, 26> messages{{
        // Name plates.
        {EventTextCategory, 107000, u"Beckoner", u"Host"},
        {EventTextCategory, 107100, u"Cooperator", u"Guest"},
        // Joins.
        {EventTextCategory, 102001, u"The bell's resonance has beckoned <?joinName?>.",
         u"<?joinName?> joined."},
        {EventTextCategory, 102100, u"Entered the world of <?hostName?>,\nwho beckoned you here.",
         u"Joined <?hostName?>."},
        {EventTextCategory, 101001, u"Discovered the chime of a Small Resonant Bell.",
         u"Found a guest."},
        {EventTextCategory, 101100, u"Discovered the chime of a Beckoning Bell.", u"Found a host."},
        // Searches.
        {EventTextCategory, 100001, u"Searching for the chime of a Small Resonant Bell...",
         u"Waiting for guests..."},
        {EventTextCategory, 100100, u"Searching for the chime of a Beckoning Bell...",
         u"Looking for a host..."},
        {EventTextCategory, 100300,
         u"Could not discover the chime of a Small Resonant Bell.\nNo insight was consumed.",
         u"No guests found.\nNo Insight was used."},
        {SpDialogCategory, 302200, u"Searching for the chime of a Beckoning Bell...",
         u"Looking for a host..."},
        // Deaths, bosses and doors.
        {EventTextCategory, 103001, u"Cooperator <?deadName?> has died.",
         u"Guest <?deadName?> has died."},
        {EventTextCategory, 103101, u"Your beckoner has died.\nReturning to own world.",
         u"Your host has died.\nReturning to own world."},
        {EventTextCategory, 105100, u"Beckoner has challenged\n<?npcNameId@500000?>.",
         u"Host has challenged\n<?npcNameId@500000?>."},
        {EventTextCategory, 105199, u"The beckoner has begun the final battle.",
         u"The host has begun the final battle."},
        {EventTextCategory, 105299,
         u"The beckoner has begun the final battle.\nReturning to own world.",
         u"The host has begun the final battle.\nReturning to own world."},
        {EventTextCategory, 106200,
         u"Beckoner <?hostName?> has fallen.\nIt is time to bid farewell.",
         u"Host <?hostName?> has fallen.\nIt is time to bid farewell."},
        {EventTextCategory, 10010740, u"Only the beckoner can open this door.",
         u"Only the host can open this door."},
        {EventTextCategory, 10010741, u"Only the beckoner can operate this device.",
         u"Only the host can operate this device."},
        // Leaving.
        {EventTextCategory, 20000400, u"Sent cooperator <?leaveName?> back\nto their own world.",
         u"Sent guest <?leaveName?> home."},
        {EventTextCategory, 20000401, u"Sent back to own world by\nbeckoner <?hostName?>.",
         u"Sent home by host <?hostName?>."},
        {EventTextCategory, 20000402, u"Cooperator <?leaveName?>\nreturned to their own world.",
         u"Guest <?leaveName?> left."},
        {EventTextCategory, 20000404, u"Cooperator <?leaveName?>\nreturned to their own world.",
         u"Guest <?leaveName?> left."},
        {EventTextCategory, 20000405, u"Cooperator <?leaveName?>\nreturned to their own world.",
         u"Guest <?leaveName?> left."},
        {InGameMenuCategory, 141000, u"Send Phantom home", u"Send Guest home"},
        {DialogCategory, 18100, u"Send this phantom home?", u"Send this guest home?"},
        {DialogCategory, 18101, u"No phantom to send home", u"No guest to send home"},
    }};
    return messages;
}

std::optional<std::u16string_view> PolishedText(u32 category, s32 id, std::u16string_view current) {
    for (const PolishedMessage& message : PolishedMessages()) {
        if (message.category == category && message.id == id && message.original == current) {
            return message.replacement;
        }
    }
    return std::nullopt;
}

std::size_t RemoveBellIcons(std::span<u8> sp_effect_param) {
    if (!IsParamOfType(sp_effect_param, "SP_EFFECT_PARAM_ST")) {
        return 0;
    }
    std::size_t changed = 0;
    for (const s32 effect : CoopBellIconEffects) {
        const auto row = FindParamRow(sp_effect_param, effect);
        if (!row || *row > sp_effect_param.size() ||
            sp_effect_param.size() - *row < SpEffectIconOffset + sizeof(s32)) {
            continue;
        }
        s32 icon = 0;
        std::memcpy(&icon, sp_effect_param.data() + *row + SpEffectIconOffset, sizeof(icon));
        if (icon != -1) {
            icon = -1;
            std::memcpy(sp_effect_param.data() + *row + SpEffectIconOffset, &icon, sizeof(icon));
            ++changed;
        }
    }
    return changed;
}

std::size_t NameUnnamedGuestRole(std::span<u8, RoleTableSize> roles) {
    std::size_t changed = 0;
    for (std::size_t at = RoleTextsAt; at < RoleTextsEnd; at += sizeof(s32)) {
        s32 guest = 0, unnamed = 0;
        std::memcpy(&guest, roles.data() + GuestRole * RoleTableEntrySize + at, sizeof(guest));
        std::memcpy(&unnamed, roles.data() + UnnamedGuestRole * RoleTableEntrySize + at,
                    sizeof(unnamed));
        if (guest != 0 && unnamed == 0) {
            std::memcpy(roles.data() + UnnamedGuestRole * RoleTableEntrySize + at, &guest,
                        sizeof(guest));
            ++changed;
        }
    }
    return changed;
}

const std::vector<CodePatch>& QuietSummonBannerPatches() {
    static const std::vector<CodePatch> patches = CoopPatchesOf("quiet summon banners");
    return patches;
}

std::optional<SaveBackups> ParseSaveBackups(std::string_view setting) {
    const auto trim = [](std::string_view text) {
        while (!text.empty() && IsWhitespace(text.front())) {
            text.remove_prefix(1);
        }
        while (!text.empty() && IsWhitespace(text.back())) {
            text.remove_suffix(1);
        }
        return text;
    };
    setting = trim(setting);
    if (setting == "off" || setting == "0") {
        return std::nullopt;
    }
    const std::size_t comma = setting.find(',');
    if (comma == std::string_view::npos) {
        return SaveBackups{};
    }
    const std::string_view minutes_text = trim(setting.substr(0, comma));
    const std::string_view kept_text = trim(setting.substr(comma + 1));
    int minutes = 0;
    std::size_t kept = 0;
    const auto [minutes_end, minutes_error] =
        std::from_chars(minutes_text.data(), minutes_text.data() + minutes_text.size(), minutes);
    const auto [kept_end, kept_error] =
        std::from_chars(kept_text.data(), kept_text.data() + kept_text.size(), kept);
    if (minutes_error != std::errc{} || kept_error != std::errc{} ||
        minutes_end != minutes_text.data() + minutes_text.size() ||
        kept_end != kept_text.data() + kept_text.size() || minutes < 1 || kept < 1) {
        return SaveBackups{};
    }
    return SaveBackups{std::chrono::minutes{minutes}, kept};
}

std::string SaveBackupName(std::chrono::sys_seconds when) {
    const auto day = std::chrono::floor<std::chrono::days>(when);
    const std::chrono::year_month_day date{day};
    const std::chrono::hh_mm_ss time{when - day};
    return fmt::format("{:04}{:02}{:02}-{:02}{:02}{:02}", static_cast<int>(date.year()),
                       static_cast<unsigned>(date.month()), static_cast<unsigned>(date.day()),
                       time.hours().count(), time.minutes().count(), time.seconds().count());
}

namespace {

namespace fs = std::filesystem;

bool IsSaveBackupName(const std::string& name) {
    return name.size() == 15 && name[8] == '-' &&
           std::ranges::all_of(name.substr(0, 8), [](char c) { return c >= '0' && c <= '9'; }) &&
           std::ranges::all_of(name.substr(9), [](char c) { return c >= '0' && c <= '9'; });
}

// The backups in `user_dir`, oldest first.
std::vector<fs::path> SaveBackupsIn(const fs::path& user_dir) {
    std::vector<fs::path> backups;
    std::error_code error;
    for (const auto& entry : fs::directory_iterator(user_dir / SaveBackupsFolder, error)) {
        if (entry.is_directory(error) && IsSaveBackupName(entry.path().filename().string())) {
            backups.push_back(entry.path());
        }
    }
    std::ranges::sort(backups);
    return backups;
}

// Every save file under `root` (savedata, home/<id>/savedata, the party files), by its path
// relative to `root`, with its bytes.
std::map<std::string, std::string> SaveFilesUnder(const fs::path& root) {
    std::map<std::string, std::string> files;
    std::error_code error;
    const auto add = [&](const fs::path& path) {
        std::ifstream file{path, std::ios::binary};
        if (file) {
            files[fs::relative(path, root, error).generic_string()] =
                std::string{std::istreambuf_iterator<char>{file}, {}};
        }
    };
    const auto add_tree = [&](const fs::path& folder) {
        for (auto it = fs::recursive_directory_iterator(folder, error);
             !error && it != fs::recursive_directory_iterator(); it.increment(error)) {
            if (it->is_regular_file(error)) {
                add(it->path());
            }
        }
        error.clear();
    };
    if (fs::is_directory(root / "savedata", error)) {
        add_tree(root / "savedata");
    }
    for (const auto& entry : fs::directory_iterator(root / "home", error)) {
        if (fs::is_directory(entry.path() / "savedata", error)) {
            add_tree(entry.path() / "savedata");
        }
    }
    error.clear();
    for (const auto& entry : fs::directory_iterator(root, error)) {
        const std::string name = entry.path().filename().string();
        if (entry.is_regular_file(error) && name.starts_with("bloodborne_party_") &&
            name.ends_with(".txt")) {
            add(entry.path());
        }
    }
    return files;
}

} // namespace

std::optional<fs::path> BackUpSaves(const fs::path& user_dir, std::string_view name) {
    const auto files = SaveFilesUnder(user_dir);
    if (files.empty()) {
        return std::nullopt;
    }
    const auto backups = SaveBackupsIn(user_dir);
    if (!backups.empty() && SaveFilesUnder(backups.back()) == files) {
        return std::nullopt;
    }
    const fs::path backup = user_dir / SaveBackupsFolder / std::string{name};
    std::error_code error;
    for (const auto& [relative, bytes] : files) {
        const fs::path path = backup / fs::path{relative};
        fs::create_directories(path.parent_path(), error);
        std::ofstream file{path, std::ios::binary | std::ios::trunc};
        file.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        if (!file) {
            fs::remove_all(backup, error);
            return std::nullopt;
        }
    }
    // The game wrote while this read: the copy may be half old, half new.
    if (SaveFilesUnder(user_dir) != files) {
        fs::remove_all(backup, error);
        return std::nullopt;
    }
    return backup;
}

std::size_t PruneSaveBackups(const fs::path& user_dir, std::size_t kept) {
    std::error_code error;
    auto backups = SaveBackupsIn(user_dir);
    std::erase_if(backups, [&](const fs::path& backup) {
        return fs::exists(backup / LauncherBackupNote, error);
    });
    std::size_t removed = 0;
    for (std::size_t index = 0; index + kept < backups.size(); ++index) {
        if (fs::remove_all(backups[index], error) > 0) {
            ++removed;
        }
    }
    return removed;
}

bool CoopTraceEnabled(const char* setting) {
    if (setting == nullptr) {
        return true;
    }
    const std::string_view value{setting};
    return value != "0" && value != "off";
}

std::optional<std::string> TraceRecord::Field(std::string_view key) const {
    for (const auto& [name, value] : fields) {
        if (name == key) {
            return value;
        }
    }
    return std::nullopt;
}

namespace {

int TraceHexDigit(char c) {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

template <typename T>
std::optional<T> ParseTraceNumber(std::string_view text, int base) {
    T value{};
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value, base);
    if (text.empty() || error != std::errc{} || end != text.data() + text.size()) {
        return std::nullopt;
    }
    return value;
}

std::vector<std::string_view> SplitTrace(std::string_view text, char separator) {
    std::vector<std::string_view> parts;
    while (true) {
        const size_t end = text.find(separator);
        parts.push_back(text.substr(0, end));
        if (end == std::string_view::npos) {
            return parts;
        }
        text.remove_prefix(end + 1);
    }
}

std::string_view TraceKeyOf(std::string_view token) {
    return token.substr(0, token.find(':'));
}

} // namespace

std::string TraceEscape(std::string_view text) {
    std::string escaped;
    escaped.reserve(text.size());
    for (const char c : text) {
        const auto byte = static_cast<u8>(c);
        if (c == '\\') {
            escaped += "\\\\";
        } else if (c == ' ') {
            escaped += "\\s";
        } else if (c == '\n') {
            escaped += "\\n";
        } else if (c == '\r') {
            escaped += "\\r";
        } else if (c == '\t') {
            escaped += "\\t";
        } else if (byte < 0x20 || byte >= 0x7F) {
            escaped += fmt::format("\\x{:02x}", byte);
        } else {
            escaped += c;
        }
    }
    return escaped;
}

std::optional<std::string> TraceUnescape(std::string_view text) {
    std::string plain;
    plain.reserve(text.size());
    for (size_t index = 0; index < text.size(); ++index) {
        if (text[index] != '\\') {
            plain += text[index];
            continue;
        }
        if (++index >= text.size()) {
            return std::nullopt;
        }
        switch (text[index]) {
        case '\\':
            plain += '\\';
            break;
        case 's':
            plain += ' ';
            break;
        case 'n':
            plain += '\n';
            break;
        case 'r':
            plain += '\r';
            break;
        case 't':
            plain += '\t';
            break;
        case 'x': {
            if (index + 2 >= text.size()) {
                return std::nullopt;
            }
            const int high = TraceHexDigit(text[index + 1]);
            const int low = TraceHexDigit(text[index + 2]);
            if (high < 0 || low < 0) {
                return std::nullopt;
            }
            plain += static_cast<char>(high * 16 + low);
            index += 2;
            break;
        }
        default:
            return std::nullopt;
        }
    }
    return plain;
}

std::string EncodeTraceRecord(const TraceRecord& record) {
    std::string line = fmt::format("{} {}", record.utc_ms, record.kind);
    for (const auto& [key, value] : record.fields) {
        line += ' ';
        line += key;
        line += '=';
        line += TraceEscape(value);
    }
    return line;
}

std::optional<TraceRecord> ParseTraceRecord(std::string_view line) {
    while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) {
        line.remove_suffix(1);
    }
    std::vector<std::string_view> tokens;
    for (const std::string_view token : SplitTrace(line, ' ')) {
        if (!token.empty()) {
            tokens.push_back(token);
        }
    }
    if (tokens.size() < 2) {
        return std::nullopt;
    }
    const auto utc_ms = ParseTraceNumber<s64>(tokens[0], 10);
    if (!utc_ms) {
        return std::nullopt;
    }
    TraceRecord record{*utc_ms, std::string{tokens[1]}, {}};
    for (size_t index = 2; index < tokens.size(); ++index) {
        const size_t equals = tokens[index].find('=');
        if (equals == std::string_view::npos || equals == 0) {
            return std::nullopt;
        }
        auto value = TraceUnescape(tokens[index].substr(equals + 1));
        if (!value) {
            return std::nullopt;
        }
        record.fields.emplace_back(std::string{tokens[index].substr(0, equals)}, std::move(*value));
    }
    return record;
}

void AddTraceBody(TraceRecord& record, std::string_view body, std::size_t cap) {
    record.fields.emplace_back("size", std::to_string(body.size()));
    record.fields.emplace_back("body", std::string{body.substr(0, cap)});
}

std::string TraceHex(std::span<const u8> bytes) {
    static constexpr char Digits[] = "0123456789abcdef";
    std::string hex;
    hex.reserve(bytes.size() * 2);
    for (const u8 byte : bytes) {
        hex += Digits[byte >> 4];
        hex += Digits[byte & 0xF];
    }
    return hex;
}

std::optional<std::vector<u8>> ParseTraceHex(std::string_view hex) {
    if (hex.size() % 2 != 0) {
        return std::nullopt;
    }
    std::vector<u8> bytes;
    bytes.reserve(hex.size() / 2);
    for (size_t index = 0; index < hex.size(); index += 2) {
        const int high = TraceHexDigit(hex[index]);
        const int low = TraceHexDigit(hex[index + 1]);
        if (high < 0 || low < 0) {
            return std::nullopt;
        }
        bytes.push_back(static_cast<u8>(high * 16 + low));
    }
    return bytes;
}

std::string TraceHandles(std::span<const u32> handles) {
    std::string text;
    for (const u32 handle : handles) {
        text += fmt::format("{}{:x}", text.empty() ? "" : ",", handle);
    }
    return text;
}

std::optional<std::vector<u32>> ParseTraceHandles(std::string_view text) {
    std::vector<u32> handles;
    if (text.empty()) {
        return handles;
    }
    for (const std::string_view part : SplitTrace(text, ',')) {
        const auto handle = ParseTraceNumber<u32>(part, 16);
        if (!handle) {
            return std::nullopt;
        }
        handles.push_back(*handle);
    }
    return handles;
}

std::string TraceToken(const SeenCharacter& character) {
    const int flags = (character.untouched_boss ? 1 : 0) | (character.others_kill ? 2 : 0);
    return fmt::format("{:x}:{}:{:x}:{}{}", character.handle, character.hp > 0 ? 1 : 0,
                       character.instance, character.settling ? 1 : 0,
                       flags != 0 ? fmt::format(":{}", flags) : std::string{});
}

std::optional<SeenCharacter> ParseSeenToken(std::string_view token) {
    const auto parts = SplitTrace(token, ':');
    if (parts.size() != 4 && parts.size() != 5) {
        return std::nullopt;
    }
    const auto handle = ParseTraceNumber<u32>(parts[0], 16);
    const auto alive = ParseTraceNumber<int>(parts[1], 10);
    const auto instance = ParseTraceNumber<u64>(parts[2], 16);
    const auto settling = ParseTraceNumber<int>(parts[3], 10);
    const auto flags = parts.size() == 5 ? ParseTraceNumber<int>(parts[4], 10) : std::optional{0};
    if (!handle || !alive || !instance || !settling || !flags || *alive > 1 || *settling > 1 ||
        *flags < 0 || *flags > 3 || (parts.size() == 5 && *flags == 0)) {
        return std::nullopt;
    }
    return SeenCharacter{*handle,           *alive,           *instance, *settling == 1,
                         (*flags & 1) != 0, (*flags & 2) != 0};
}

std::string TraceToken(const EnemyCopy& copy) {
    return fmt::format("{:x}:{}:{}:{}", copy.handle, copy.hp, copy.max,
                       copy.own_hits ? std::to_string(*copy.own_hits) : std::string{"-"});
}

std::optional<EnemyCopy> ParseCopyToken(std::string_view token) {
    const auto parts = SplitTrace(token, ':');
    if (parts.size() != 4) {
        return std::nullopt;
    }
    const auto handle = ParseTraceNumber<u32>(parts[0], 16);
    const auto hp = ParseTraceNumber<s32>(parts[1], 10);
    const auto max = ParseTraceNumber<s32>(parts[2], 10);
    const auto own = parts[3] == "-" ? std::nullopt : ParseTraceNumber<s64>(parts[3], 10);
    if (!handle || !hp || !max || (parts[3] != "-" && !own)) {
        return std::nullopt;
    }
    return EnemyCopy{*handle, *hp, *max, own};
}

std::string TraceToken(const EnemyHpReport& report) {
    std::string token = fmt::format("{:x}:", report.visit);
    for (size_t index = 0; index < report.enemies.size(); ++index) {
        const EnemyHp& enemy = report.enemies[index];
        token +=
            fmt::format("{}{:x}/{}/{}", index == 0 ? "" : "+", enemy.handle, enemy.hp, enemy.max);
    }
    return token;
}

std::optional<EnemyHpReport> ParseReportToken(std::string_view token) {
    const size_t colon = token.find(':');
    if (colon == std::string_view::npos) {
        return std::nullopt;
    }
    const auto source = ParseTraceNumber<u64>(token.substr(0, colon), 16);
    if (!source) {
        return std::nullopt;
    }
    EnemyHpReport report{*source, {}};
    const std::string_view enemies = token.substr(colon + 1);
    if (enemies.empty()) {
        return report;
    }
    for (const std::string_view enemy : SplitTrace(enemies, '+')) {
        const auto parts = SplitTrace(enemy, '/');
        const auto handle = parts.size() == 3 ? ParseTraceNumber<u32>(parts[0], 16) : std::nullopt;
        const auto hp = parts.size() == 3 ? ParseTraceNumber<s32>(parts[1], 10) : std::nullopt;
        const auto max = parts.size() == 3 ? ParseTraceNumber<s32>(parts[2], 10) : std::nullopt;
        if (!handle || !hp || !max) {
            return std::nullopt;
        }
        report.enemies.push_back({*handle, *hp, *max});
    }
    return report;
}

std::string TraceListDelta::Next(std::span<const std::string> tokens) {
    std::string delta;
    const auto add = [&delta](std::string_view part) {
        if (!delta.empty() && delta != "=") {
            delta += ',';
        }
        delta += part;
    };
    if (whole) {
        whole = false;
        delta = "=";
        for (const std::string& token : tokens) {
            add(token);
        }
    } else {
        std::map<std::string, std::string, std::less<>> before;
        for (const std::string& token : last) {
            before.emplace(TraceKeyOf(token), token);
        }
        std::set<std::string, std::less<>> keys;
        for (const std::string& token : tokens) {
            keys.emplace(TraceKeyOf(token));
            const auto it = before.find(TraceKeyOf(token));
            if (it == before.end() || it->second != token) {
                add(token);
            }
        }
        for (const std::string& token : last) {
            if (!keys.contains(TraceKeyOf(token))) {
                add(fmt::format("-{}", TraceKeyOf(token)));
            }
        }
    }
    last.assign(tokens.begin(), tokens.end());
    return delta;
}

bool ApplyTraceListDelta(std::vector<std::string>& list, std::string_view delta) {
    if (delta.starts_with('=')) {
        list.clear();
        delta.remove_prefix(1);
    }
    if (delta.empty()) {
        return true;
    }
    for (const std::string_view part : SplitTrace(delta, ',')) {
        if (part.starts_with('-')) {
            std::erase_if(list, [key = part.substr(1)](const std::string& token) {
                return TraceKeyOf(token) == key;
            });
            continue;
        }
        if (part.find(':') == std::string_view::npos) {
            return false;
        }
        const auto it = std::ranges::find_if(
            list, [key = TraceKeyOf(part)](const std::string& t) { return TraceKeyOf(t) == key; });
        if (it != list.end()) {
            *it = std::string{part};
        } else {
            list.emplace_back(part);
        }
    }
    return true;
}

TraceSampler::Fields TraceSampler::Changes(
    u32 file_now, const Fields& values,
    const std::vector<std::pair<std::string, std::vector<std::string>>>& lists) {
    if (file != file_now) {
        file = file_now;
        last.clear();
        for (auto& [key, delta] : deltas) {
            delta.Reset();
        }
    }
    Fields changes;
    for (const auto& [key, value] : values) {
        auto [it, added] = last.try_emplace(key, value);
        if (added || it->second != value) {
            it->second = value;
            changes.emplace_back(key, value);
        }
    }
    for (const auto& [key, tokens] : lists) {
        if (std::string delta = deltas[key].Next(tokens); !delta.empty()) {
            changes.emplace_back(key, std::move(delta));
        }
    }
    return changes;
}

TraceSampler::Fields TracePlaceFields(const PlaceChange& change) {
    TraceSampler::Fields fields;
    for (const auto& [side, place] : {std::pair{"from", change.from}, std::pair{"to", change.to}}) {
        fields.emplace_back(fmt::format("{}-state", side),
                            std::string{PlaceStateName(place.state)});
        fields.emplace_back(fmt::format("{}-map", side), fmt::format("{:x}", place.map));
        fields.emplace_back(fmt::format("{}-visit", side), std::to_string(place.visit));
        fields.emplace_back(fmt::format("{}-since", side), std::to_string(place.since_ms));
    }
    fields.emplace_back("cause", std::string{PlaceCauseName(change.cause)});
    return fields;
}

TraceSampler::Fields WorldLogTraceFields(std::string_view role, std::string_view op,
                                         std::span<const u8> payload) {
    return {{"role", std::string{role}}, {"op", std::string{op}}, {"payload", TraceHex(payload)}};
}

std::optional<PlaceChange> ParsePlaceFields(const TraceRecord& record) {
    const auto state_of = [](std::string_view name) -> std::optional<PlaceState> {
        for (const PlaceState state : {PlaceState::Unknown, PlaceState::Loading,
                                       PlaceState::Cutscene, PlaceState::Playing}) {
            if (PlaceStateName(state) == name) {
                return state;
            }
        }
        return std::nullopt;
    };
    const auto place_of = [&](std::string_view side) -> std::optional<Place> {
        const auto field = [&](const char* name) {
            return record.Field(fmt::format("{}-{}", side, name)).value_or("");
        };
        const auto state = state_of(field("state"));
        const auto map = ParseTraceNumber<u32>(field("map"), 16);
        const auto visit = ParseTraceNumber<u64>(field("visit"), 10);
        const auto since = ParseTraceNumber<s64>(field("since"), 10);
        if (!state || !map || !visit || !since) {
            return std::nullopt;
        }
        return Place{*state, *map, *visit, *since};
    };
    const auto from = place_of("from");
    const auto to = place_of("to");
    const std::string cause = record.Field("cause").value_or("");
    for (const PlaceCause known :
         {PlaceCause::Load, PlaceCause::Border, PlaceCause::Lamp, PlaceCause::Teleport,
          PlaceCause::DeathReload, PlaceCause::Cutscene, PlaceCause::CutsceneEnd}) {
        if (from && to && PlaceCauseName(known) == cause) {
            return PlaceChange{*from, *to, known};
        }
    }
    return std::nullopt;
}

bool TraceQueue::Push(std::string line) {
    {
        std::scoped_lock lock{mutex};
        if (lines.size() >= capacity) {
            ++dropped;
            return false;
        }
        lines.push_back(std::move(line));
    }
    ready.notify_one();
    return true;
}

std::vector<std::string> TraceQueue::Take(std::chrono::milliseconds wait) {
    std::unique_lock lock{mutex};
    ready.wait_for(lock, wait, [this] { return !lines.empty(); });
    std::vector<std::string> taken;
    taken.swap(lines);
    return taken;
}

u64 TraceQueue::TakeDropped() {
    std::scoped_lock lock{mutex};
    return std::exchange(dropped, 0);
}

std::string CoopTraceFileName(std::chrono::sys_seconds local_when, u32 process) {
    return fmt::format("{}{}-{}.txt", CoopTracePrefix, SaveBackupName(local_when), process);
}

std::size_t PruneCoopTraces(const fs::path& log_dir, std::size_t kept) {
    const std::string_view prefix{CoopTracePrefix};
    std::vector<fs::path> traces;
    std::error_code error;
    for (const auto& entry : fs::directory_iterator(log_dir, error)) {
        const std::string name = entry.path().filename().string();
        if (entry.is_regular_file(error) && name.starts_with(prefix) && name.ends_with(".txt") &&
            name.size() > prefix.size() + 15 && IsSaveBackupName(name.substr(prefix.size(), 15))) {
            traces.push_back(entry.path());
        }
    }
    std::ranges::sort(traces);
    std::size_t removed = 0;
    for (std::size_t index = 0; index + kept < traces.size(); ++index) {
        if (fs::remove(traces[index], error)) {
            ++removed;
        }
    }
    return removed;
}

PacketTraceMode ParsePacketTraceMode(const char* setting) {
    const std::string_view value{setting != nullptr ? setting : ""};
    if (value == "1" || value == "on") {
        return PacketTraceMode::On;
    }
    return value == "full" ? PacketTraceMode::Full : PacketTraceMode::Off;
}

bool NativePayloadUnknown(u32 channel) {
    return channel == 0x29 || channel == 0x34 || channel == 0x3D || channel == 0x41 ||
           channel == 0x43;
}

std::size_t TracedPayloadBytes(PacketTraceMode mode, u32 channel, std::size_t size) {
    const bool shown = mode == PacketTraceMode::Full ||
                       (mode == PacketTraceMode::On && NativePayloadUnknown(channel));
    return shown ? std::min(size, NativePayloadTraceCap) : 0;
}

std::optional<NativePacketView> DecodeNativePacket(std::span<const u8> bytes) {
    if (bytes.size() < 8) {
        return std::nullopt;
    }
    u32 channel = 0;
    u32 size = 0;
    std::memcpy(&channel, bytes.data(), 4);
    std::memcpy(&size, bytes.data() + 4, 4);
    if (channel >= NativeChannelCount || size > NativePacketMaxBytes - 8 ||
        bytes.size() < 8 + static_cast<std::size_t>(size)) {
        return std::nullopt;
    }
    return NativePacketView{channel, bytes.subspan(8, size)};
}

std::optional<std::string> NpOnlineIdAt(std::span<const u8> bytes) {
    std::string id;
    for (const u8 byte : bytes.first(std::min<std::size_t>(bytes.size(), 17))) {
        if (byte == 0) {
            return id.size() >= 3 ? std::optional{id} : std::nullopt;
        }
        if (!std::isalnum(byte) && byte != '-' && byte != '_') {
            return std::nullopt;
        }
        id += static_cast<char>(byte);
    }
    return std::nullopt;
}

const char* NativePacketTraceKind(bool sent) {
    return sent ? "net-out" : "net-in";
}

TraceSampler::Fields NativePacketFields(const NativePacketRecord& packet) {
    TraceSampler::Fields fields{{"ch", fmt::format("{:02x}", packet.channel)},
                                {"size", std::to_string(packet.size)},
                                {"peer", packet.peer},
                                {"map", fmt::format("{:x}", packet.map)},
                                {"sync", std::to_string(packet.sync_state)},
                                {"match", std::to_string(packet.matching_state)}};
    if (!packet.payload.empty()) {
        fields.emplace_back("payload", TraceHex(packet.payload));
    }
    return fields;
}

std::optional<NativePacketRecord> ParseNativePacketRecord(const TraceRecord& record) {
    if (record.kind != NativePacketTraceKind(true) && record.kind != NativePacketTraceKind(false)) {
        return std::nullopt;
    }
    const auto field = [&](const char* name) { return record.Field(name).value_or(""); };
    const auto channel = ParseTraceNumber<u32>(field("ch"), 16);
    const auto size = ParseTraceNumber<u32>(field("size"), 10);
    const auto map = ParseTraceNumber<u32>(field("map"), 16);
    const auto sync = ParseTraceNumber<s32>(field("sync"), 10);
    const auto match = ParseTraceNumber<s32>(field("match"), 10);
    const auto payload = record.Field("payload")
                             ? ParseTraceHex(field("payload"))
                             : std::optional<std::vector<u8>>{std::vector<u8>{}};
    if (!channel || *channel >= NativeChannelCount || !size || !map || !sync || !match ||
        !payload) {
        return std::nullopt;
    }
    return NativePacketRecord{record.kind == NativePacketTraceKind(true),
                              *channel,
                              *size,
                              field("peer"),
                              *map,
                              *sync,
                              *match,
                              *payload};
}

const EnemyOwnerEntry* EnemyOwnerOf(std::span<const EnemyOwnerEntry> owners, u32 map) {
    const u32 area = EnemyOwnerArea(map);
    if (area == 0) {
        return nullptr;
    }
    const auto owner = std::ranges::find(owners, area, &EnemyOwnerEntry::area);
    return owner != owners.end() ? &*owner : nullptr;
}

std::optional<u16> AwakeEnemyWord(EnemyRole role, s32 level, s32 applied, u16 word, bool kept_out,
                                  bool handed_out) {
    if (role != EnemyRole::Owner || level != EnemyAwake || applied != EnemyAwake || kept_out ||
        handed_out || static_cast<s16>(word) >= 0) {
        return std::nullopt;
    }
    return OwnedEnemyWord;
}

bool LoneGuestSwitchesOff(bool lone_guest, bool hidden, bool disabled, s32 wanted, s32 applied) {
    return lone_guest && hidden && !disabled && wanted == 0 && applied > 0;
}

bool LoneGuestSwitchesBackOn(bool lone_guest, bool hidden) {
    return !lone_guest || !hidden;
}

std::optional<u16> HostFollowerWord(EnemyRole role, bool hosting, u16 word) {
    if (role != EnemyRole::Follower || !hosting || static_cast<s16>(word) < 0) {
        return std::nullopt;
    }
    return u16{0xFFFF};
}

bool WakesAroundOthers(EnemyRole role, bool hosting) {
    return role == EnemyRole::Owner || (role == EnemyRole::Follower && hosting);
}

bool SendsFullPositionPacket(EnemyRole role, s32 entries, s32 per_packet, s32 slot, s32 slots,
                             u32 sent_this_frame) {
    // The game sends the round's last packet itself (and one a frame of its own).
    return role != EnemyRole::Unknown && per_packet > 0 && entries >= per_packet &&
           slot + 1 < slots && sent_this_frame + 1 < MaxPositionPacketsPerFrame;
}

bool OwnerSendsPositionOf(EnemyRole role, s32 level, s32 applied, u16 word, bool kept_out) {
    return role == EnemyRole::Owner && level == EnemyAwake && applied == EnemyAwake && !kept_out &&
           static_cast<s16>(word) >= 0;
}

float PositionRoundSeconds(EnemyRole role) {
    return role != EnemyRole::Unknown ? 0.1f : GamePositionRoundSeconds;
}

float HandOutSeconds(EnemyRole role) {
    return role != EnemyRole::Unknown ? 0.25f : GameHandOutSeconds;
}

float NearListSeconds(EnemyRole role) {
    return role != EnemyRole::Unknown ? 0.25f : GameNearListSeconds;
}

std::vector<u32> HandOutIds(std::span<const u8> payload) {
    u32 count = 0;
    if (payload.size() < 4) {
        return {};
    }
    std::memcpy(&count, payload.data(), 4);
    if (count > 0x800 || payload.size() < 4 + size_t{count} * 4) {
        return {};
    }
    std::vector<u32> ids;
    for (u32 i = 0; i < count; ++i) {
        u32 entry = 0;
        std::memcpy(&entry, payload.data() + 4 + size_t{i} * 4, 4);
        if ((entry & 0xFFFF) != HandOutHeldEntry) { // held by the owner (HandOutSettle)
            ids.push_back(entry & 0xFFFF);
        }
    }
    return ids;
}

std::vector<u32> HandOutSettle::Filter(const std::string& member, std::span<const u32> entries,
                                       s64 now_ms) {
    Member& state = members[member];
    if (now_ms - state.last_ms > HandOutFreshMs) {
        state = {}; // a load, a trip away: what it was handed before means nothing now
    }
    state.last_ms = now_ms;
    // The first one with enemies in it goes as it is (the game sends one a period, empty or not).
    const bool first = !state.seen;
    state.seen = state.seen || !entries.empty();
    std::vector<u32> out;
    std::set<u32> wanted;
    for (const u32 entry : entries) {
        const u32 index = entry & 0xFFFF;
        wanted.insert(index);
        const s64 since = state.wanted_since.try_emplace(index, now_ms).first->second;
        if (first || state.handed.contains(index) || now_ms - since >= HandOverSettleMs) {
            state.handed.insert(index);
            out.push_back(entry);
        } else {
            out.push_back(HandOutHeldEntry);
        }
    }
    // Taken back: forgotten at once, so a hand-out again settles anew.
    std::erase_if(state.wanted_since,
                  [&](const auto& item) { return !wanted.contains(item.first); });
    std::erase_if(state.handed, [&](u32 index) { return !wanted.contains(index); });
    return out;
}

bool EnemyJumped(const std::array<float, 3>& before, const std::array<float, 3>& now) {
    const float dx = now[0] - before[0];
    const float dy = now[1] - before[1];
    const float dz = now[2] - before[2];
    return dx * dx + dy * dy + dz * dz > EnemyJumpMetres * EnemyJumpMetres;
}

bool HandedOut(std::span<const u32> handed, u32 index) {
    return std::ranges::find(handed, index) != handed.end();
}

std::string PositionRatesText(const std::map<u32, u32>& taken,
                              std::span<const std::pair<u32, u32>> nearby, double seconds) {
    if (nearby.empty() || seconds <= 0.0) {
        return "none";
    }
    std::vector<std::pair<u32, u32>> rates;
    for (const auto& [handle, index] : nearby) {
        const auto found = taken.find(index);
        rates.emplace_back(handle, found != taken.end() ? found->second : 0);
    }
    std::ranges::stable_sort(rates, std::ranges::greater{}, &std::pair<u32, u32>::second);
    std::string text;
    for (const auto& [handle, count] : rates) {
        text += fmt::format("{}{:x} {:.1f}", text.empty() ? "" : ", ", handle, count / seconds);
    }
    return text;
}

WakeGroups WithOthersGroups(const WakeGroups& record, const WakeGroups& own,
                            std::span<const WakeGroups> others) {
    const auto shares = [](const GroupSet& a, const GroupSet& b) {
        for (size_t i = 0; i < a.size(); ++i) {
            if ((a[i] & b[i]) != 0) {
                return true;
            }
        }
        return false;
    };
    const auto placed = [](const GroupSet& set) {
        return std::ranges::any_of(set, [](u32 word) { return word != 0; }) &&
               !std::ranges::all_of(set, [](u32 word) { return word == 0xFFFFFFFFu; });
    };
    if (!shares(record.wakes, own.wakes) && !shares(record.counted, own.counted) &&
        !shares(record.half, own.half)) {
        return record;
    }
    WakeGroups groups = record;
    for (const WakeGroups& other : others) {
        if (!placed(other.wakes) || !placed(other.half)) {
            continue;
        }
        for (size_t i = 0; i < groups.wakes.size(); ++i) {
            groups.wakes[i] |= other.wakes[i];
            groups.counted[i] |= other.counted[i];
            groups.half[i] |= other.half[i];
        }
    }
    return groups;
}

EnemyRole EnemyRoleOn(std::span<const EnemyOwnerEntry> owners, std::string_view self, u32 map) {
    const EnemyOwnerEntry* owner = EnemyOwnerOf(owners, map);
    if (owner == nullptr || self.empty() || owner->member.empty()) {
        return EnemyRole::Unknown;
    }
    return owner->member == self ? EnemyRole::Owner : EnemyRole::Follower;
}

std::optional<s32> EnemySyncState(EnemyRole role, s32 state) {
    if (state != MatchingStateHosting && state != MatchingStateSummonedGuest) {
        return std::nullopt;
    }
    const s32 wanted = role == EnemyRole::Owner      ? MatchingStateHosting
                       : role == EnemyRole::Follower ? MatchingStateSummonedGuest
                                                     : state;
    return wanted != state ? std::optional{wanted} : std::nullopt;
}

std::optional<s32> EnemyHitState(EnemyRole role, s32 state) {
    if (role == EnemyRole::Follower && state == MatchingStateHosting) {
        return MatchingStateSummonedGuest;
    }
    return std::nullopt;
}

std::optional<s32> HeldOwnHitHp(s32 current, s32 after_own_hit, s64 age_ms) {
    if (current <= 0 || current <= after_own_hit || age_ms >= OwnHitHoldMs) {
        return std::nullopt;
    }
    return after_own_hit;
}

std::optional<s32> HitNewsHp(EnemyRole role, s32 current, s32 copy_max, s32 hp, s32 max) {
    if (role != EnemyRole::Follower || current <= 0 || copy_max <= 0 || max <= 0 || hp < 0) {
        return std::nullopt;
    }
    // Rounded up: a sliver left on either side is still alive here.
    const s64 scaled = (s64{hp} * copy_max + max - 1) / max;
    const s32 target = static_cast<s32>(std::clamp<s64>(scaled, 0, copy_max));
    return target < current ? std::optional<s32>{target} : std::nullopt;
}

bool TakesOverEnemyHp(const EnemyOwnerEntry& owner) {
    return owner.epoch > 1;
}

std::optional<u16> EnemyOwnershipWord(EnemyRole role) {
    switch (role) {
    case EnemyRole::Owner:
        return u16{0};
    case EnemyRole::Follower:
        return u16{0xFFFF};
    case EnemyRole::Unknown:
        break;
    }
    return std::nullopt;
}

bool ChangesHandsWithTheMap(s32 entity) {
    return entity != HuntersDreamDoll;
}

std::optional<u16> KeptLocalWord(EnemyRole role, s32 entity, u16 word) {
    if (role == EnemyRole::Unknown || ChangesHandsWithTheMap(entity) ||
        static_cast<s16>(word) >= 0) {
        return std::nullopt;
    }
    return OwnedEnemyWord;
}

EnemyPacketFate EnemyPacketOut(u32 channel, EnemyRole role) {
    // A follower's near list (0x16) goes to the owner: the owner's game sends a member the state
    // of the enemies on its list (attacks too), and with none a far guest's enemies came for it
    // but never struck (rig apart-hurt 2026-10-15). The owner's answer (0x17), which hands
    // enemies away, never goes.
    if (channel == 0x16) {
        return role == EnemyRole::Follower ? EnemyPacketFate::ToOwner : EnemyPacketFate::Drop;
    }
    if (channel == 0x17) {
        return role == EnemyRole::Owner ? EnemyPacketFate::Keep : EnemyPacketFate::Drop;
    }
    if (channel == 0x15) {
        return role == EnemyRole::Owner      ? EnemyPacketFate::Drop
               : role == EnemyRole::Follower ? EnemyPacketFate::ToOwner
                                             : EnemyPacketFate::Keep;
    }
    if (BloodborneCoop::IsOwnersEnemyChannel(channel) && role == EnemyRole::Follower) {
        return EnemyPacketFate::Drop;
    }
    return EnemyPacketFate::Keep;
}

std::optional<std::string> EnemyRoleLog::Change(EnemyRole role, u32 map,
                                                const EnemyOwnerEntry* owner) {
    std::string line;
    switch (role) {
    case EnemyRole::Owner:
        line = fmt::format("enemy owner: this game runs map {:#x}'s enemies (epoch {})",
                           EnemyOwnerArea(map), owner != nullptr ? owner->epoch : 0);
        break;
    case EnemyRole::Follower:
        line = fmt::format("enemy owner: this game follows {} on map {:#x} (epoch {})",
                           owner != nullptr ? owner->member : std::string{"?"}, EnemyOwnerArea(map),
                           owner != nullptr ? owner->epoch : 0);
        break;
    case EnemyRole::Unknown:
        line = "enemy owner: none (the game's own way)";
        break;
    }
    if (!last && role == EnemyRole::Unknown) {
        last = line;
        return std::nullopt; // the start: nothing changed
    }
    if (last == line) {
        return std::nullopt;
    }
    last = line;
    return line;
}

const std::vector<DeathFlagSite>& DeathFlagSites() {
    static const std::vector<DeathFlagSite> sites{
        {0x018BF941, {0x80, 0x8B, 0xE3, 0x01, 0x00, 0x00, 0x01}, DeathRegister::Rbx, "death"},
        {0x018CE6C5,
         {0x80, 0x89, 0xE3, 0x01, 0x00, 0x00, 0x01},
         DeathRegister::Rcx,
         "death in the character update"},
        {0x018BFAD3,
         {0x41, 0x80, 0x8F, 0xE3, 0x01, 0x00, 0x00, 0x01},
         DeathRegister::R15,
         "the game's kill"},
    };
    return sites;
}

bool TracesDeathOf(s32 max_hp) {
    return max_hp >= DeathTraceMinMaxHp;
}

bool IsPartySession(bool party_configured, s32 matching_state) {
    return party_configured &&
           (matching_state == MatchingStateHosting || matching_state == MatchingStateSummonedGuest);
}

void LampRestState::Rest(s32 rested_lamp, const LampPosition& at) {
    lamp = rested_lamp;
    rested_at = at;
    joined = false;
}

LampUse LampRestState::Use(s32 used_lamp, const LampPosition& at) {
    if (lamp == used_lamp) {
        lamp.reset();
        return LampUse::Travel;
    }
    Rest(used_lamp, at);
    return LampUse::Rest;
}

namespace {
float DistanceSquared(const LampPosition& a, const LampPosition& b) {
    float sum = 0.0f;
    for (std::size_t axis = 0; axis < a.size(); ++axis) {
        sum += (a[axis] - b[axis]) * (a[axis] - b[axis]);
    }
    return sum;
}

std::string FormatCoordinate(float value) {
    std::array<char, 32> text{};
    const auto [end, error] = std::to_chars(text.data(), text.data() + text.size(), value);
    return error == std::errc{} ? std::string{text.data(), end} : std::string{"0"};
}

std::optional<float> ParseCoordinate(std::string_view text) {
    float value = 0.0f;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (error != std::errc{} || end != text.data() + text.size()) {
        return std::nullopt;
    }
    return value;
}
} // namespace

bool LampRestState::JoinNearby(std::span<const LampRest> board,
                               const std::optional<LampPosition>& player) {
    if (lamp || !player) {
        return false;
    }
    for (const LampRest& rest : board) {
        if (rest.lamp != NotResting && rest.at &&
            DistanceSquared(*rest.at, *player) <= JoinDistance * JoinDistance) {
            Rest(rest.lamp, *rest.at);
            joined = true;
            return true;
        }
    }
    return false;
}

std::optional<LampPosition> LampRestState::JoinablePosition() const {
    return lamp && !joined ? std::optional{rested_at} : std::nullopt;
}

u64 RestoreScriptFlagsAfterRespawn(u64 flags_before, u64 flags_after, s32 hp_before) {
    if (hp_before <= 0) {
        return flags_after;
    }
    return (flags_after & ~ScriptCharacterFlags) | (flags_before & ScriptCharacterFlags);
}

bool RespawnsAtPartyRest(s32 hp, bool alive_this_visit) {
    return hp <= 0 && alive_this_visit;
}

bool LeftDeadByItsSave(const RespawnCandidate& character) {
    // Bits 0 and 2: killed (IsRealDeath); bit 1 comes with either; bit 5: put down by its world.
    constexpr u8 DeathMarks = 0x27;
    return character.hp <= 0 && character.max_hp > 0 && (character.death_flags & DeathMarks) == 0 &&
           ((!character.disabled && !character.hidden) || character.changed_by_party);
}

bool RespawnsInWorldRespawn(WorldRespawnKind kind, const RespawnCandidate& character,
                            bool alive_this_visit) {
    switch (kind) {
    case WorldRespawnKind::PartyRest:
        return RespawnsAtPartyRest(character.hp, alive_this_visit);
    case WorldRespawnKind::AfterWipe:
        return LeftDeadByItsSave(character);
    case WorldRespawnKind::BroughtBack:
        return character.brought_back && character.hp <= 0;
    }
    return false;
}

std::vector<u32> BroughtBackByTheHost(std::span<const CharacterShown> host,
                                      std::span<const DeadCopy> dead_here,
                                      const std::unordered_set<u32>& party_kills, s64 now_ms) {
    std::unordered_set<u32> alive_at_host;
    for (const CharacterShown& character : host) {
        if (!character.hidden && !character.disabled && !character.distant && !character.dead) {
            alive_at_host.insert(character.handle);
        }
    }
    std::vector<u32> back;
    for (const DeadCopy& copy : dead_here) {
        if (alive_at_host.contains(copy.handle) && !party_kills.contains(copy.handle) &&
            now_ms - copy.dead_since_ms >= BringBackAfterMs) {
            back.push_back(copy.handle);
        }
    }
    std::ranges::sort(back);
    return back;
}

bool KeepDisabledAfterRespawn(bool disabled_before, s32 hp_before) {
    return disabled_before && hp_before > 0;
}

std::optional<s32> LampRestState::Tick(const std::optional<LampPosition>& player,
                                       bool in_party_session) {
    if (!in_party_session) {
        lamp.reset();
    } else if (lamp && player) {
        if (DistanceSquared(*player, rested_at) > LeaveDistance * LeaveDistance) {
            lamp.reset();
        }
    }
    return lamp;
}

std::string EncodeLampRestUpdate(std::string_view member, s32 lamp,
                                 const std::optional<LampPosition>& at) {
    std::string update = std::string{member} + ' ' + std::to_string(lamp);
    if (lamp != NotResting && at) {
        for (const float coordinate : *at) {
            update += ' ' + FormatCoordinate(coordinate);
        }
    }
    return update;
}

std::optional<std::vector<LampRest>> ParseLampRestBoard(std::string_view body) {
    std::vector<LampRest> board;
    while (!body.empty()) {
        const std::size_t end = body.find('\n');
        std::string_view line = body.substr(0, end);
        body.remove_prefix(end == std::string_view::npos ? body.size() : end + 1);
        std::vector<std::string_view> fields;
        while (!line.empty()) {
            const std::size_t space = line.find(' ');
            fields.push_back(line.substr(0, space));
            line.remove_prefix(space == std::string_view::npos ? line.size() : space + 1);
        }
        if (fields.empty()) {
            continue;
        }
        if ((fields.size() != 2 && fields.size() != 5) || fields[0].empty()) {
            return std::nullopt;
        }
        LampRest rest{.member = std::string{fields[0]}};
        const auto [lamp_end, error] =
            std::from_chars(fields[1].data(), fields[1].data() + fields[1].size(), rest.lamp);
        if (error != std::errc{} || lamp_end != fields[1].data() + fields[1].size()) {
            return std::nullopt;
        }
        if (fields.size() == 5) {
            LampPosition at{};
            for (std::size_t axis = 0; axis < at.size(); ++axis) {
                const auto coordinate = ParseCoordinate(fields[2 + axis]);
                if (!coordinate) {
                    return std::nullopt;
                }
                at[axis] = *coordinate;
            }
            rest.at = at;
        }
        board.push_back(std::move(rest));
    }
    return board;
}

bool LampRestCoordinator::Tick(std::optional<s32> resting_lamp, s32 session_members,
                               std::span<const LampRest> board) {
    bool everyone_resting = false;
    if (resting_lamp && session_members > 1) {
        const auto resting_here = std::ranges::count_if(
            board, [&](const LampRest& rest) { return rest.lamp == *resting_lamp; });
        everyone_resting = resting_here >= session_members;
    }
    const bool refresh = everyone_resting && !everyone_was_resting;
    everyone_was_resting = everyone_resting;
    return refresh;
}

bool RefreshAtLamp(const LampRestCalls& calls) {
    if (!calls.request_full_recover || !calls.clamp_restock_item || !calls.restock_from_storage ||
        !calls.request_world_respawn) {
        return false;
    }
    calls.request_full_recover();
    calls.clamp_restock_item();
    calls.restock_from_storage();
    calls.request_world_respawn();
    return true;
}

bool SkipsLeaveLimit(s32 character_type, size_t other_members) {
    return character_type != WhiteGhostCharacterType || other_members > 0;
}

// A host with room whose bell stays on: on the port the effect outlasted the game's search after
// the first guest arrived (rig join3 2026-10-09), so a bell the game no longer searches with is
// rung again until the session is full. Stale only when the game posted no search for
// StaleSearchAfter (rig join3 at 30 FPS 2026-10-12: ringing again every RetryDelay ended searches
// that still ran, and the game posted none for 2m45s). The effect is ended first and the search
// started on a later tick, once it is off: the game starts a search when the effect goes on.
std::optional<PartyAction> PartyController::RingAgain(const PartyObservation& observation,
                                                      Clock::time_point now) {
    if (role != PartyRole::Host || !observation.room_for_more || observation.white_phantom) {
        ringing_since.reset();
        ending_since.reset();
        return std::nullopt;
    }
    if (ending_since) {
        if (!observation.search_effect_active) {
            ending_since.reset();
            ringing_since.reset();
            stable_ticks = 0;
            last_search = now;
            return PartyAction::StartSearch;
        }
        if (now - *ending_since < RetryDelay) {
            return PartyAction::Wait;
        }
        ending_since = now; // the effect did not go: end it again
        return PartyAction::EndSearch;
    }
    if (!observation.search_effect_active) {
        ringing_since.reset();
        return std::nullopt;
    }
    if (!ringing_since) {
        ringing_since = now;
    }
    const Clock::time_point heard = observation.last_search_post
                                        ? std::max(*ringing_since, *observation.last_search_post)
                                        : *ringing_since;
    if (now - heard < StaleSearchAfter) {
        return PartyAction::Wait;
    }
    ending_since = now;
    return PartyAction::EndSearch;
}

PartyAction PartyController::Tick(const PartyObservation& observation, Clock::time_point now) {
    if (waiting.load()) {
        return PartyAction::Wait;
    }
    if (!observation.player_valid || !observation.map_usable || observation.stage_transition ||
        observation.boss_sequence) {
        stable_ticks = 0;
        ending_since.reset();
        return PartyAction::Wait;
    }
    const bool may_search =
        observation.matching_idle || (role == PartyRole::Host && observation.room_for_more);
    if (const auto action = RingAgain(observation, now)) {
        return *action;
    }
    if (!may_search || observation.search_effect_active || observation.white_phantom) {
        // In (or joining) a session, or our search already took: the next time we are back home
        // alone is a fresh start, so forget the old cooldown.
        stable_ticks = 0;
        last_search.reset();
        return PartyAction::Wait;
    }
    if (++stable_ticks < StableTicksBeforeSearch) {
        return PartyAction::Wait;
    }
    if (last_search && now - *last_search < RetryDelay) {
        return PartyAction::Wait;
    }
    last_search = now;
    stable_ticks = 0;
    return PartyAction::StartSearch;
}

std::vector<AutoplayCommand> ParseAutoplayScript(std::string_view text) {
    std::vector<AutoplayCommand> script;
    while (!text.empty()) {
        const auto end = text.find('\n');
        std::string_view line = text.substr(0, end);
        text = end == std::string_view::npos ? std::string_view{} : text.substr(end + 1);
        if (const auto comment = line.find('#'); comment != std::string_view::npos) {
            line = line.substr(0, comment);
        }
        std::vector<std::string> words;
        size_t at = 0;
        while (at < line.size()) {
            const auto start = line.find_first_not_of(" \t\r", at);
            if (start == std::string_view::npos) {
                break;
            }
            const auto stop = line.find_first_of(" \t\r", start);
            words.emplace_back(line.substr(start, stop - start));
            at = stop == std::string_view::npos ? line.size() : stop;
        }
        if (!words.empty()) {
            script.push_back({words.front(), {words.begin() + 1, words.end()}});
        }
    }
    return script;
}

u32 AutoplayButton(std::string_view name) {
    static constexpr std::array<std::pair<std::string_view, u32>, 15> Buttons{{
        {"cross", 0x4000},
        {"circle", 0x2000},
        {"square", 0x8000},
        {"triangle", 0x1000},
        {"options", 0x8},
        {"up", 0x10},
        {"right", 0x20},
        {"down", 0x40},
        {"left", 0x80},
        {"l1", 0x400},
        {"r1", 0x800},
        {"l2", 0x100},
        {"r2", 0x200},
        {"l3", 0x2},
        {"r3", 0x4},
    }};
    for (const auto& [button, bit] : Buttons) {
        if (button == name) {
            return bit;
        }
    }
    return 0;
}

bool TitlePressesCross(u64 ms) {
    constexpr u64 Period = 2000;
    constexpr u64 Held = 150;
    return ms >= Period && ms % Period < Held;
}

namespace {
// A file name a script gives (shot, sync): 1 to 40 letters, digits, '-' or '_'.
bool IsPlainAutoplayName(std::string_view name) {
    const auto plain = [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
               c == '-' || c == '_';
    };
    return !name.empty() && name.size() <= 40 && std::ranges::all_of(name, plain);
}
} // namespace

std::optional<std::string> AutoplayShotPath(std::string_view request, std::string_view name) {
    if (request.empty() || !IsPlainAutoplayName(name)) {
        return std::nullopt;
    }
    const std::filesystem::path shots = std::filesystem::path{request}.parent_path() / "shots";
    return (shots / (std::string{name} + ".png")).string();
}

std::optional<std::string> AutoplaySyncPath(std::string_view script, std::string_view name) {
    if (script.empty() || !IsPlainAutoplayName(name)) {
        return std::nullopt;
    }
    return (std::filesystem::path{script}.parent_path() / "sync" / std::string{name}).string();
}

void PositionTrail::Note(u64 ms, const LampPosition& at) {
    if (last_ms && ms < *last_ms + IntervalMs) {
        return;
    }
    last_ms = ms;
    points.push_back(at);
    if (points.size() > Kept) {
        points.erase(points.begin());
    }
}

std::string FormatTrail(const std::vector<LampPosition>& points) {
    std::string text;
    for (const LampPosition& point : points) {
        text += fmt::format("{}{:.1f} {:.1f} {:.1f}", text.empty() ? "" : " ", point[0], point[1],
                            point[2]);
    }
    return text;
}

std::optional<LampPosition> GlidePoint(const std::vector<LampPosition>& path, double elapsed) {
    double left = elapsed * GlideMetersPerSecond;
    for (size_t at = 1; at < path.size(); ++at) {
        const LampPosition& from = path[at - 1];
        const LampPosition& to = path[at];
        const double length = std::hypot(to[0] - from[0], to[1] - from[1], to[2] - from[2]);
        if (left < length) {
            const float part = static_cast<float>(left / length);
            return LampPosition{from[0] + (to[0] - from[0]) * part,
                                from[1] + (to[1] - from[1]) * part,
                                from[2] + (to[2] - from[2]) * part};
        }
        left -= length;
    }
    return std::nullopt;
}

bool PickupPlayerOffTheItem(const LampPosition& player, const LampPosition& item) {
    // The item button's reach is short: its prompt went 1.4 m aside from the spot (rig 2026-10-22).
    // Up and down only a fall counts (the feet stand a little under the spot).
    const bool aside = std::hypot(player[0] - item[0], player[2] - item[2]) > 0.75f;
    const bool fell = item[1] - player[1] > 2.0f;
    return aside || fell;
}

std::optional<size_t> NextAutoplayChapter(const std::vector<AutoplayCommand>& script,
                                          size_t failed) {
    for (size_t at = failed + 1; at < script.size(); ++at) {
        if (script[at].verb == "chapter") {
            return at;
        }
    }
    return std::nullopt;
}

std::optional<double> AutoplayChapterBudget(const AutoplayCommand& command) {
    if (command.verb != "chapter" || command.args.size() < 2) {
        return std::nullopt;
    }
    const std::string& text = command.args[1];
    double seconds = 0.0;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), seconds);
    if (error != std::errc{} || end != text.data() + text.size() || seconds <= 0.0) {
        return std::nullopt;
    }
    return seconds;
}

std::optional<std::string> AutoplayMenuKeys(const std::vector<std::string>& tokens) {
    static constexpr std::array<std::string_view, 17> Keys{
        "toggle", "up",  "down", "left",   "right",  "enter",    "back", "l1", "r1",
        "kdown",  "kup", "map",  "circle", "square", "triangle", "r3",   "l3"};
    if (tokens.empty()) {
        return std::nullopt;
    }
    std::string text;
    for (const std::string& token : tokens) {
        if (std::ranges::find(Keys, token) == Keys.end()) {
            return std::nullopt;
        }
        text += text.empty() ? token : " " + token;
    }
    return text + "\n";
}

std::optional<u32> ParseAutoplayTarget(std::string_view text) {
    if (text == "nearest") {
        return 0;
    }
    if (text == "self") {
        return AutoplaySelf;
    }
    if (text == "marked") {
        return AutoplayMarked;
    }
    if (text == "player") {
        return AutoplayPlayer;
    }
    if (text.starts_with("0x")) {
        text.remove_prefix(2);
    }
    u32 handle = 0;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), handle, 16);
    if (text.empty() || error != std::errc{} || end != text.data() + text.size()) {
        return std::nullopt;
    }
    return handle;
}

std::string DescribeEffectChanges(std::vector<s32> before, std::vector<s32> after) {
    std::ranges::sort(before);
    std::ranges::sort(after);
    std::vector<s32> gained;
    std::vector<s32> lost;
    std::ranges::set_difference(after, before, std::back_inserter(gained));
    std::ranges::set_difference(before, after, std::back_inserter(lost));
    std::vector<std::pair<s32, char>> changes;
    for (const s32 effect : gained) {
        changes.emplace_back(effect, '+');
    }
    for (const s32 effect : lost) {
        changes.emplace_back(effect, '-');
    }
    std::ranges::sort(changes);
    std::string text;
    for (const auto& [effect, sign] : changes) {
        text += fmt::format("{}{}{}", text.empty() ? "" : " ", sign, effect);
    }
    return text;
}

std::optional<bool> CompareAutoplay(s64 value, std::string_view op, s64 limit) {
    if (op == "<") {
        return value < limit;
    }
    if (op == "<=") {
        return value <= limit;
    }
    if (op == "==") {
        return value == limit;
    }
    if (op == "!=") {
        return value != limit;
    }
    if (op == ">=") {
        return value >= limit;
    }
    if (op == ">") {
        return value > limit;
    }
    return std::nullopt;
}

AttackStance AttackStanceFor(float target_x, float target_z, u32 cycle) {
    static constexpr std::array<std::pair<float, float>, 4> Sides{
        {{0.0f, 1.0f}, {1.0f, 0.0f}, {0.0f, -1.0f}, {-1.0f, 0.0f}}};
    const auto [x, z] = Sides[cycle % Sides.size()];
    return {target_x + AttackDistance * x, target_z + AttackDistance * z};
}

float HeadingTowards(float x, float z, float to_x, float to_z) {
    return std::atan2(-(to_x - x), to_z - z);
}

u32 AttackCycleAt(double elapsed) {
    return elapsed <= 0.0 ? 0u : static_cast<u32>(elapsed / AttackCycleSeconds);
}

bool AttackHoldsR1(double elapsed) {
    const double into = std::max(elapsed, 0.0) - AttackCycleAt(elapsed) * AttackCycleSeconds;
    return into >= 0.3 && into < 0.5;
}

bool PartyDeathEnabled(const char* setting, bool by_default) {
    if (setting == nullptr || *setting == '\0') {
        return by_default;
    }
    return std::string_view{setting} != "0";
}

namespace {
// Read from the 01.09 eboot: each is the address after a `call IsNoDead` (see the eboot tests).
constexpr std::array<NoDeadCaller, 12> NoDeadCallerTable{{
    // Hit damage (0x01A2A770: HP minus the hit, asked only when that is 0 or less), and its check
    // once the hit is applied, asked after every hit (0x01A2C03B skips only the kill).
    {0x01A2BF7D, NoDeadCheck::Lethal},
    {0x01A2C05F, NoDeadCheck::AfterHit},
    // Special effect HP changes (0x01A2FA40: poison, rapid poison, frenzy and the like).
    {0x01A2FB4A, NoDeadCheck::Lethal},
    {0x01A2FBE6, NoDeadCheck::Lethal},
    {0x01A2FDD4, NoDeadCheck::Lethal},
    // Fall damage (0x01A35A00, from the landing at 0x01516138), and its check afterwards.
    {0x01A35E10, NoDeadCheck::Lethal},
    {0x01A35E7E, NoDeadCheck::Lethal},
    // The game's kills: an event's ForceCharacterDeath (KillCharacterOffset), the character's
    // own kill (0x018BF760, from its vtable) and another (0x018CDE10).
    {0x018BF9EE, NoDeadCheck::Instant},
    {0x018BF86C, NoDeadCheck::Instant},
    {0x018CE5EE, NoDeadCheck::Instant},
    // HP set outright (0x018BCD40, which the revive and the event bindings use, and 0x018BC470).
    {0x018BCD9C, NoDeadCheck::Instant},
    {0x018BC5C5, NoDeadCheck::Instant},
}};

template <typename T>
std::optional<T> ParseNumber(std::string_view text, int base = 10) {
    T value{};
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value, base);
    if (text.empty() || error != std::errc{} || end != text.data() + text.size()) {
        return std::nullopt;
    }
    return value;
}

std::vector<std::string_view> SplitFields(std::string_view line) {
    std::vector<std::string_view> fields;
    while (!line.empty()) {
        const std::size_t space = line.find(' ');
        fields.push_back(line.substr(0, space));
        line.remove_prefix(space == std::string_view::npos ? line.size() : space + 1);
    }
    return fields;
}
} // namespace

std::span<const NoDeadCaller> NoDeadCallers() {
    return NoDeadCallerTable;
}

NoDeadCheck ClassifyNoDeadCheck(u64 return_offset) {
    const auto caller =
        std::ranges::find(NoDeadCallerTable, return_offset,
                          [](const NoDeadCaller& entry) { return entry.return_offset; });
    return caller == NoDeadCallerTable.end() ? NoDeadCheck::Other : caller->check;
}

NoDeadCheck NoDeadCheckAt(NoDeadCheck check, s32 hp) {
    if (check != NoDeadCheck::AfterHit) {
        return check;
    }
    return hp <= 0 ? NoDeadCheck::Lethal : NoDeadCheck::Other;
}

bool PartyDeathWatches(bool in_session, Vital vital) {
    if (vital == Vital::Dead) {
        return false;
    }
    return in_session || vital == Vital::Down || vital == Vital::Spectate;
}

bool KeepsPlayerAlive(NoDeadCheck check, bool armed, bool down, bool answer) {
    return (check == NoDeadCheck::Lethal && (armed || down)) || answer;
}

std::string_view VitalName(Vital vital) {
    switch (vital) {
    case Vital::Up:
        return "up";
    case Vital::Down:
        return "down";
    case Vital::Dead:
        return "dead";
    case Vital::Spectate:
        return "spectate";
    case Vital::Off:
        break;
    }
    return "off";
}

std::optional<Vital> ParseVital(std::string_view name) {
    for (const Vital vital : {Vital::Off, Vital::Up, Vital::Down, Vital::Dead, Vital::Spectate}) {
        if (VitalName(vital) == name) {
            return vital;
        }
    }
    return std::nullopt;
}

std::optional<std::string> VitalsPostLog::Next(Vital vital, u32 map) {
    const std::pair now{vital, map};
    if (last == now) {
        return std::nullopt;
    }
    std::string line = fmt::format("posting {} (map {:#x})", VitalName(vital), map);
    if (last) {
        line += fmt::format(", was {} (map {:#x})", VitalName(last->first), last->second);
    }
    last = now;
    return line;
}

u32 ReviveDamageNeeded(u32 downs, s32 max) {
    const u32 bar = static_cast<u32>(std::max(max, 1));
    return downs <= 1 ? bar : downs == 2 ? bar * 2 : bar * 4;
}

u32 ReviveHitDamage(s32 attack_rating, bool heavy) {
    const u32 rating = static_cast<u32>(std::max(attack_rating, 1));
    return heavy ? std::max(rating * 3 / 2, 1u) : rating;
}

u32 RealReviveHitDamage(u32 dealt, s32 attack_rating, bool heavy) {
    return std::max(dealt, ReviveHitDamage(attack_rating, heavy));
}

u32 ReviveHitScale::Count(u32 dealt, s32 attack_rating, bool heavy) {
    if (attack_rating != baseline_rating) {
        baseline = 0;
        baseline_rating = attack_rating;
    }
    if (!heavy && dealt > 0 && (baseline == 0 || dealt < baseline)) {
        baseline = dealt;
    }
    const u32 floor = RealReviveHitDamage(dealt, attack_rating, heavy);
    if (baseline == 0) {
        return floor;
    }
    const u64 scaled = u64{dealt} * static_cast<u64>(std::max(attack_rating, 1)) / u64{baseline};
    return std::max(floor, static_cast<u32>(std::min<u64>(scaled, 0xFFFFFFFFu)));
}

float ReviveShare(u32 landed, u32 needed) {
    return needed == 0 ? 0.0f
                       : std::min(static_cast<float>(landed) / static_cast<float>(needed), 1.0f);
}

s32 RevivedHp(s32 max) {
    return std::max(max / 2, 1);
}

std::string GameHudString(std::span<const u8, GameHudStringSize> raw,
                          const std::function<std::vector<char16_t>(u64, size_t)>& read) {
    constexpr size_t MaxChars = 64;
    u64 capacity = 0;
    std::memcpy(&capacity, raw.data() + 0x20, sizeof(capacity));
    std::vector<char16_t> chars;
    if (capacity < 8) {
        chars.resize(8);
        std::memcpy(chars.data(), raw.data() + 0x08, 16);
    } else {
        u64 pointer = 0;
        std::memcpy(&pointer, raw.data() + 0x08, sizeof(pointer));
        chars = read(pointer, static_cast<size_t>(std::min<u64>(capacity, MaxChars)));
    }
    std::string text;
    for (const char16_t c : chars) {
        if (c == 0 || text.size() == MaxChars) {
            break;
        }
        text += c < 0x80 ? static_cast<char>(c) : '?';
    }
    return text;
}

std::string GameStringUtf8(std::span<const u8, GameHudStringSize> raw,
                           const std::function<std::vector<char16_t>(u64, size_t)>& read) {
    constexpr size_t MaxChars = 16;
    u64 capacity = 0;
    std::memcpy(&capacity, raw.data() + 0x20, sizeof(capacity));
    std::vector<char16_t> chars;
    if (capacity < 8) {
        chars.resize(8);
        std::memcpy(chars.data(), raw.data() + 0x08, 16);
    } else {
        u64 pointer = 0;
        std::memcpy(&pointer, raw.data() + 0x08, sizeof(pointer));
        chars = read(pointer, MaxChars + 1);
    }
    const auto end = std::ranges::find(chars, u'\0');
    if (end == chars.end() || end - chars.begin() > static_cast<std::ptrdiff_t>(MaxChars)) {
        return {}; // no end within a name's length: not a name
    }
    std::string text;
    for (auto it = chars.begin(); it != end; ++it) {
        u32 c = *it;
        if (c >= 0xD800 && c < 0xDC00 && it + 1 != end && it[1] >= 0xDC00 && it[1] < 0xE000) {
            c = 0x10000 + ((c - 0xD800) << 10) + (*++it - 0xDC00);
        }
        if (c < 0x80) {
            text += static_cast<char>(c);
        } else if (c < 0x800) {
            text += static_cast<char>(0xC0 | (c >> 6));
            text += static_cast<char>(0x80 | (c & 0x3F));
        } else if (c < 0x10000) {
            text += static_cast<char>(0xE0 | (c >> 12));
            text += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
            text += static_cast<char>(0x80 | (c & 0x3F));
        } else {
            text += static_cast<char>(0xF0 | (c >> 18));
            text += static_cast<char>(0x80 | ((c >> 12) & 0x3F));
            text += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
            text += static_cast<char>(0x80 | (c & 0x3F));
        }
    }
    return text;
}

std::string HtmlText(std::string_view text) {
    std::string out;
    for (const char c : text) {
        switch (c) {
        case '&':
            out += "&amp;";
            break;
        case '<':
            out += "&lt;";
            break;
        case '>':
            out += "&gt;";
            break;
        default:
            out += c;
        }
    }
    return out;
}

std::optional<std::string> NameplateNameLine(const PartyPanel& panel, std::string_view name) {
    if (name.empty()) {
        return std::nullopt;
    }
    for (const PanelMember& member : panel.members) {
        if (member.name == name && !member.shown.empty()) {
            return HtmlText(member.shown);
        }
    }
    return std::nullopt;
}

bool PanelHasDowned(const PartyPanel& panel) {
    return std::ranges::any_of(
        panel.members, [](const PanelMember& member) { return member.vital == Vital::Down; });
}

std::optional<std::string> NameplateRoleLine(const PartyPanel& panel, std::string_view name) {
    if (name.empty()) {
        return std::nullopt;
    }
    for (const PanelMember& member : panel.members) {
        const std::string_view id = member.name.empty() ? member.member : member.name;
        // In a party the game names a member by its character (PlayerNameForTextOffset).
        if ((id == name || member.shown == name) && member.vital == Vital::Down) {
            return fmt::format("<font color=\"#e74141\">Downed {}%</font>",
                               std::lround(ReviveShare(member.landed, member.needed) * 100.0f));
        }
    }
    return std::nullopt;
}

namespace {
std::string RoleLine(std::string_view color, std::string_view text) {
    return fmt::format("<font color=\"#{}\">{}</font>", color, text);
}
} // namespace

std::string ShownName(const PanelMember& member) {
    return member.shown.empty() ? std::string{UnknownHunterName} : member.shown;
}

std::array<PartyListEntry, PartyListSize> PartyListEntries(const PartyPanel& panel) {
    std::array<PartyListEntry, PartyListSize> entries{};
    for (size_t i = 0; i < entries.size() && i < panel.members.size(); ++i) {
        const PanelMember& member = panel.members[i];
        PartyListEntry& entry = entries[i];
        entry.visible = true;
        // No role line: the icon says the state; the name goes on white after the tag's update
        // (name_line), since the game's own name text took html badly (rig 2026-10-12: a hang).
        entry.name = ShownName(member);
        entry.member = member.member;
        entry.vials = member.vials;
        entry.bullets = member.bullets;
        entry.name_line = RoleLine("ece8df", HtmlText(entry.name));
        entry.max = std::max(member.max, 1);
        entry.hp = std::clamp(member.hp, 0, entry.max);
        entry.icon = PartyListIconHunter;
        if (member.vital == Vital::Down) {
            const long percent = std::lround(ReviveShare(member.landed, member.needed) * 100.0f);
            entry.hp = static_cast<s32>(std::max(percent, 1L));
            entry.max = 100;
            entry.icon = PartyListIconDowned0 + static_cast<u32>((percent + 5) / 10);
        } else if (member.vital == Vital::Dead || member.vital == Vital::Spectate) {
            // Kaniel 2026-10-25: a spectating member is out of the fight as a dead one is.
            entry.hp = 0;
            entry.icon = PartyListIconFallen;
        } else if (!member.here) {
            entry.icon = PartyListIconElsewhere;
        }
    }
    return entries;
}

std::array<std::string, 2> SupplyTexts(const PartyListEntry& entry) {
    if (!entry.visible) {
        return {};
    }
    return {std::to_string(entry.vials), std::to_string(entry.bullets)};
}

float SupplyNameWidth(std::string_view name) {
    float width = 0.0f;
    for (size_t at = 0; at < name.size(); ++at) {
        const auto c = static_cast<unsigned char>(name[at]);
        if ((c & 0xC0) == 0x80) {
            continue; // a UTF-8 continuation: its letter counted once
        }
        if (std::string_view{"iljtfr.,'!|I1 "}.find(char(c)) != std::string_view::npos) {
            width += 7.0f;
        } else if (std::string_view{"mwMW"}.find(char(c)) != std::string_view::npos) {
            width += 17.0f;
        } else if (c >= 'A' && c <= 'Z') {
            width += 14.0f;
        } else {
            width += 12.0f;
        }
    }
    return width;
}

u32 SupplyFrame(const PartyListEntry& entry) {
    if (!entry.visible) {
        return 1;
    }
    const float past = std::max(0.0f, SupplyNameWidth(entry.name) - SupplyNameRoom);
    const u32 steps = static_cast<u32>(std::ceil(past / SupplyShiftPx));
    return 2 + std::min(steps, SupplyShifts - 1);
}

PartyPrompt PartyPromptFor(const PartyPanel& panel) {
    if (panel.vital == Vital::Spectate) {
        return {}; // the overlay's spectating line says what the buttons do
    }
    if (panel.vital == Vital::Down) {
        return PartyPrompt{.shown = true, .text = "Hold to give up", .input = PromptInputCircle};
    }
    if (panel.revive_target >= 0 && panel.revive_target < static_cast<s32>(panel.members.size())) {
        const PanelMember& member = panel.members[static_cast<size_t>(panel.revive_target)];
        // The character's name, as the party list shows it (website shots 2026-10-19: the id).
        return PartyPrompt{.shown = true,
                           .text = fmt::format("Strike to revive {}", ShownName(member)),
                           .input = PromptInputR1};
    }
    return {};
}

void WritePromptData(std::span<u8, HudPromptDataSize> data,
                     std::span<const u8, GameHudStringFullSize> string, const PartyPrompt& prompt,
                     u64 text, size_t length) {
    std::ranges::fill(data, u8{0});
    std::memcpy(data.data() + HudPromptTextAt, string.data(), GameHudStringFullSize);
    data[0] = prompt.shown ? 1 : 0;
    const auto put = [&](size_t at, u64 value) { std::memcpy(data.data() + at, &value, 8); };
    put(HudPromptTextAt + 0x08, text);
    put(HudPromptTextAt + 0x18, std::min<u64>(length, 63));
    put(HudPromptTextAt + 0x20, 63);
}

namespace {
std::atomic<float> hud_shift_x{0.0f}, hud_shift_y{0.0f};
} // namespace

void SetHudEdgeShift(float dx, float dy) {
    hud_shift_x.store(dx, std::memory_order_relaxed);
    hud_shift_y.store(dy, std::memory_order_relaxed);
}

std::pair<s32, s32> PartyListPosition(int index) {
    // hud_patch.py LIST_X/LIST_Y/LIST_STEP: each tag's HP bar starts where the player's own does
    // (x 61 on the 1920 stage: the tag's bar starts 143 left of its position), its icon left of it;
    // with the player's bars where the port's aspect hooks moved them (SetHudEdgeShift).
    return {204 - s32(std::lround(hud_shift_x.load(std::memory_order_relaxed))),
            330 + 95 * index - s32(std::lround(hud_shift_y.load(std::memory_order_relaxed)))};
}

void WriteTagData(std::span<u8, HudTagDataSize> data, std::span<const u8, HudTagDataSize> game,
                  const PartyListEntry& entry, s32 x, s32 y, u64 role, u64 name) {
    std::ranges::fill(data, u8{0});
    for (const size_t at : {size_t{0x28}, size_t{HudTagNameAt}}) {
        std::memcpy(data.data() + at, game.data() + at, GameHudStringFullSize);
    }
    const auto put = [&](size_t at, auto value) {
        std::memcpy(data.data() + at, &value, sizeof(value));
    };
    data[0] = entry.visible ? 1 : 0;
    data[1] = 1;
    put(0x04, x);
    put(0x08, y);
    put(0x0C, entry.hp);
    put(0x14, entry.max);
    const auto string = [&](size_t at, u64 chars, size_t length) {
        put(at + 0x08, chars);
        put(at + 0x18, static_cast<u64>(length));
        put(at + 0x20, u64{63});
    };
    string(0x28, role, std::min<size_t>(entry.role.size(), 63));
    string(HudTagNameAt, name, std::min<size_t>(entry.name.size(), 63));
}

std::optional<int> RemoteTagIndex(u64 tag_data, u64 fe_data) {
    const u64 first = fe_data + HudRemoteTagsAt;
    if (tag_data < first || (tag_data - first) % HudTagDataSize != 0) {
        return std::nullopt;
    }
    const u64 index = (tag_data - first) / HudTagDataSize;
    return index < HudRemoteTagCount ? std::optional<int>{static_cast<int>(index)} : std::nullopt;
}

HudValueKeep HudValueFate(u32 type, bool kept_for_the_hud) {
    if ((type & HudValueLinkedBit) == 0) {
        return HudValueKeep::Drop;
    }
    return kept_for_the_hud ? HudValueKeep::KeepForTheHud : HudValueKeep::Release;
}

s32 DownedHp(s32 max, u32 landed, u32 needed) {
    if (max <= 1 || needed == 0) {
        return 1;
    }
    const s64 done = std::min<s64>(landed, needed);
    return static_cast<s32>(std::min<s64>(1 + (s64{max} - 1) * done / needed, s64{max} - 1));
}

u32 DownedState::GoDown(s64 now_ms, s32 max) {
    if (up_since && now_ms - *up_since >= PenaltyResetMs) {
        downs = 0;
    }
    ++downs;
    vital = Vital::Down;
    down_id = next_id++;
    needed = ReviveDamageNeeded(downs, max);
    drain_step = static_cast<u32>(std::max(max / 3, 1));
    landed = 0;
    seen_damage = 0;
    return down_id;
}

bool DownedState::Damage(u32 total, s64 now_ms) {
    if (vital != Vital::Down || total <= seen_damage) {
        return false;
    }
    landed = std::min(needed, landed + (total - seen_damage));
    seen_damage = total;
    drain_at = now_ms + RefillDelayMs;
    return landed >= needed;
}

void DownedState::Tick(s64 now_ms) {
    if (vital != Vital::Down || landed == 0 || now_ms < drain_at) {
        return;
    }
    // One drain step per RefillStepMs due since drain_at, worked out at once: as a loop, clang made
    // it a 128-bit division (__udivti3), which clang-cl's links on Windows lack.
    const s64 due = (now_ms - drain_at) / RefillStepMs + 1;
    const s64 steps_left = (static_cast<s64>(landed) + drain_step - 1) / drain_step;
    const s64 steps = due < steps_left ? due : steps_left;
    const s64 drained = steps * drain_step;
    landed = drained < static_cast<s64>(landed) ? landed - static_cast<u32>(drained) : 0;
    drain_at += steps * RefillStepMs;
}

void DownedState::GetUp(s64 now_ms) {
    vital = Vital::Up;
    landed = 0;
    up_since = now_ms;
}

void DownedState::Died() {
    vital = Vital::Dead;
    landed = 0;
}

void DownedState::Spectate() {
    vital = Vital::Spectate;
    landed = 0;
}

void DownedState::Alive(s64 now_ms) {
    vital = Vital::Up;
    downs = 0;
    up_since = now_ms;
}

void DownedState::ClearPenalty() {
    downs = 0;
}

bool LethalCatches::Take(u32 count, Vital vital) {
    const bool caught = count != seen;
    seen = count;
    return caught && (vital == Vital::Up || vital == Vital::Off);
}

bool HoldToConfirm::Tick(bool held, s64 now_ms) {
    if (!held) {
        held_since.reset();
        fired = false;
        return false;
    }
    if (!held_since) {
        held_since = now_ms;
    }
    if (fired || now_ms - *held_since < hold_ms) {
        return false;
    }
    fired = true;
    return true;
}

void HoldToConfirm::Restart(bool held) {
    held_since.reset();
    fired = held;
}

float HoldToConfirm::Progress(s64 now_ms) const {
    if (!held_since || fired || hold_ms <= 0) {
        return 0.0f;
    }
    return std::clamp(static_cast<float>(now_ms - *held_since) / static_cast<float>(hold_ms), 0.0f,
                      1.0f);
}

std::string EncodeVitalsUpdate(const MemberVitals& self, std::span<const ReviveHitTotal> hits) {
    std::string update =
        fmt::format("{} {:x} {} {} {} {} {} {}", self.member, self.map, VitalName(self.vital),
                    self.hp, self.max, self.down, self.landed, self.needed);
    for (const float coordinate : self.at) {
        update += ' ' + FormatCoordinate(coordinate);
    }
    for (const ReviveHitTotal& total : hits) {
        update += fmt::format(" {}:{}:{}", total.target, total.down, total.damage);
    }
    return update;
}

std::optional<VitalsBoard> ParseVitalsBoard(std::string_view body) {
    VitalsBoard board;
    while (!body.empty()) {
        const std::size_t end = body.find('\n');
        const std::string_view line = body.substr(0, end);
        body.remove_prefix(end == std::string_view::npos ? body.size() : end + 1);
        const std::vector<std::string_view> fields = SplitFields(line);
        if (fields.empty()) {
            continue;
        }
        if (fields[0] == "wipe") {
            const auto id = fields.size() >= 4 ? ParseNumber<u32>(fields[1]) : std::nullopt;
            const auto map = fields.size() >= 4 ? ParseNumber<u32>(fields[2], 16) : std::nullopt;
            if (!id || !map) {
                return std::nullopt;
            }
            board.wipes.push_back(PartyWipe{*id, *map, {fields.begin() + 3, fields.end()}});
            continue;
        }
        if (fields.size() != 12 || fields[0].empty()) {
            return std::nullopt;
        }
        const auto map = ParseNumber<u32>(fields[1], 16);
        const auto vital = ParseVital(fields[2]);
        const auto hp = ParseNumber<s32>(fields[3]);
        const auto max = ParseNumber<s32>(fields[4]);
        const auto down = ParseNumber<u32>(fields[5]);
        const auto landed = ParseNumber<u32>(fields[6]);
        const auto needed = ParseNumber<u32>(fields[7]);
        const auto x = ParseCoordinate(fields[8]);
        const auto y = ParseCoordinate(fields[9]);
        const auto z = ParseCoordinate(fields[10]);
        const auto revive_damage = ParseNumber<u32>(fields[11]);
        if (!map || !vital || !hp || !max || !down || !landed || !needed || !x || !y || !z ||
            !revive_damage) {
            return std::nullopt;
        }
        board.members.push_back(MemberVitals{.member = std::string{fields[0]},
                                             .map = *map,
                                             .vital = *vital,
                                             .hp = *hp,
                                             .max = *max,
                                             .down = *down,
                                             .landed = *landed,
                                             .needed = *needed,
                                             .at = {*x, *y, *z},
                                             .revive_damage = *revive_damage});
    }
    return board;
}

bool IsCoopFrameType(u8 type) {
    return (type & 0xF0) == 0x30;
}

std::vector<u8> EncodeCoopFrame(u8 type, std::span<const u8> payload) {
    std::vector<u8> frame{'S', 'H', 'A', 'D', type};
    frame.insert(frame.end(), payload.begin(), payload.end());
    return frame;
}

std::optional<std::pair<u8, std::vector<u8>>> DecodeCoopFrame(std::span<const u8> frame) {
    if (frame.size() < 5 || !std::ranges::equal(frame.first(4), std::string_view{"SHAD"}) ||
        !IsCoopFrameType(frame[4])) {
        return std::nullopt;
    }
    return std::pair{frame[4], std::vector<u8>(frame.begin() + 5, frame.end())};
}

std::vector<u8> EncodeDirectKills(std::string_view member, std::span<const u32> handles) {
    std::vector<u8> payload;
    const size_t name = std::min<size_t>(member.size(), 0xFF);
    const size_t count = std::min<size_t>(handles.size(), 300);
    payload.push_back(static_cast<u8>(name));
    payload.insert(payload.end(), member.begin(), member.begin() + name);
    AppendLittleEndian(payload, static_cast<u16>(count));
    for (size_t index = 0; index < count; ++index) {
        AppendLittleEndian(payload, handles[index]);
    }
    return payload;
}

std::optional<DirectKills> DecodeDirectKills(std::span<const u8> payload) {
    if (payload.empty() || payload.size() < 1u + payload[0] + 2u) {
        return std::nullopt;
    }
    const size_t name = payload[0];
    DirectKills kills{std::string(payload.begin() + 1, payload.begin() + 1 + name), {}};
    const size_t count = ReadLittleEndian<u16>(payload, 1 + name);
    const size_t start = 1 + name + 2;
    if (payload.size() != start + count * 4) {
        return std::nullopt;
    }
    for (size_t index = 0; index < count; ++index) {
        kills.handles.push_back(ReadLittleEndian<u32>(payload, start + index * 4));
    }
    return kills;
}

std::vector<u8> EncodePeerHello(const PeerHello& hello) {
    std::vector<u8> payload{hello.protocol};
    const size_t revision = std::min<size_t>(hello.revision.size(), 0xFF);
    payload.push_back(static_cast<u8>(revision));
    payload.insert(payload.end(), hello.revision.begin(), hello.revision.begin() + revision);
    AppendLittleEndian(payload, hello.event_files);
    const size_t member = std::min<size_t>(hello.member.size(), 0xFF);
    payload.push_back(static_cast<u8>(member));
    payload.insert(payload.end(), hello.member.begin(), hello.member.begin() + member);
    payload.push_back(hello.features);
    const size_t build = std::min<size_t>(hello.build.size(), 0xFFFF);
    AppendLittleEndian(payload, static_cast<u16>(build));
    payload.insert(payload.end(), hello.build.begin(), hello.build.begin() + build);
    return payload;
}

std::optional<PeerHello> DecodePeerHello(std::span<const u8> payload) {
    if (payload.size() < 2 || payload.size() < 2u + payload[1] + 8u + 1u) {
        return std::nullopt;
    }
    PeerHello hello{.protocol = payload[0]};
    const size_t revision = payload[1];
    hello.revision.assign(payload.begin() + 2, payload.begin() + 2 + revision);
    hello.event_files = ReadLittleEndian<u64>(payload, 2 + revision);
    const size_t member_at = 2 + revision + 8;
    const size_t member = payload[member_at];
    if (payload.size() < member_at + 1 + member) {
        return std::nullopt;
    }
    hello.member.assign(payload.begin() + member_at + 1, payload.begin() + member_at + 1 + member);
    const size_t features_at = member_at + 1 + member;
    if (payload.size() > features_at) {
        hello.features = payload[features_at];
    }
    if (const size_t build_at = features_at + 1; payload.size() > build_at) {
        if (payload.size() < build_at + 2) {
            return std::nullopt;
        }
        const size_t build = ReadLittleEndian<u16>(payload, build_at);
        if (payload.size() < build_at + 2 + build) {
            return std::nullopt;
        }
        hello.build.assign(payload.begin() + build_at + 2, payload.begin() + build_at + 2 + build);
    }
    return hello;
}

std::optional<std::string> PeerBuildCheck::Take(const PeerHello& theirs, const PeerHello& mine) {
    // A game that could not read its event files (0) says nothing about them.
    const bool events_differ =
        theirs.event_files != 0 && mine.event_files != 0 && theirs.event_files != mine.event_files;
    const bool features_differ = theirs.features != mine.features;
    // Build check on join: both say their build fingerprint, so the differences go by field (the
    // server refused a game on other settings; this names them in the games' logs too).
    const auto their_build = BloodborneCoop::ParseBuildFingerprint(theirs.build);
    const auto my_build = BloodborneCoop::ParseBuildFingerprint(mine.build);
    const std::string fields =
        their_build && my_build ? BloodborneCoop::DescribeDifferences(*my_build, *their_build) : "";
    const bool no_fingerprint = my_build && !their_build;
    if (theirs.protocol == mine.protocol && theirs.revision == mine.revision && !events_differ &&
        !features_differ && fields.empty() && !no_fingerprint) {
        return std::nullopt;
    }
    if (!warned
             .emplace(theirs.member, theirs.protocol, theirs.revision, theirs.event_files,
                      theirs.features, theirs.build)
             .second) {
        return std::nullopt;
    }
    const std::string who = theirs.member.empty() ? "a peer" : theirs.member;
    if (their_build && my_build) {
        std::string line = fmt::format("Co-op: {}'s build differs from this game's: {}", who,
                                       fields.empty() ? "build " + theirs.revision : fields);
        if (features_differ) {
            line += fmt::format("; world log {}, this game's {}",
                                (theirs.features & WorldLogFeature) != 0 ? "on" : "off",
                                (mine.features & WorldLogFeature) != 0 ? "on" : "off");
        }
        return line;
    }
    std::string line =
        fmt::format("Co-op: {} runs build {}{}, this game runs {}", who,
                    theirs.revision.empty() ? "?" : theirs.revision,
                    no_fingerprint ? " (no build fingerprint: an older release)" : "",
                    mine.revision.empty() ? "?" : mine.revision);
    if (theirs.protocol != mine.protocol) {
        line += fmt::format(" (co-op protocol {}, this game's {})", theirs.protocol, mine.protocol);
    }
    if (events_differ) {
        line += fmt::format("; event files {:016x}, this game's {:016x}", theirs.event_files,
                            mine.event_files);
    }
    if (features_differ) {
        // One build for everyone: the world log on in one game and off in another splits the
        // party between the log and the boards.
        line += fmt::format("; world log {}, this game's {}",
                            (theirs.features & WorldLogFeature) != 0 ? "on" : "off",
                            (mine.features & WorldLogFeature) != 0 ? "on" : "off");
    }
    return line;
}

u64 FileHash(const std::filesystem::path& file) {
    std::ifstream stream{file, std::ios::binary};
    if (!stream) {
        return 0;
    }
    // FNV-1a, as EventFilesHash.
    u64 hash = 0xcbf29ce484222325ull;
    std::vector<char> chunk(1 << 16);
    while (stream.read(chunk.data(), static_cast<std::streamsize>(chunk.size())) ||
           stream.gcount() > 0) {
        for (std::streamsize i = 0; i < stream.gcount(); ++i) {
            hash = (hash ^ static_cast<u8>(chunk[i])) * 0x100000001b3ull;
        }
    }
    return hash != 0 ? hash : 1;
}

BloodborneCoop::BuildFingerprint CoopBuildFingerprint(
    const BuildIdentity& identity, const std::function<const char*(const char*)>& env) {
    const auto setting = [&env](const char* name, std::string_view absent) {
        const char* value = env(name);
        return std::string{value != nullptr && *value != '\0' ? std::string_view{value} : absent};
    };
    // 12 hex digits of a hash (enough to tell files apart; the link's text holds 255 bytes).
    const auto hash = [](u64 value) {
        return value != 0 ? fmt::format("{:012x}", value & 0xFFFF'FFFF'FFFFull) : std::string{};
    };
    BloodborneCoop::BuildFingerprint build;
    build.Set("version", setting("BB_RELEASE_VERSION", "dev"));
    build.Set("revision", BloodborneCoop::FingerprintRevision(identity.revision));
    build.Set("protocol", std::to_string(CoopProtocolVersion));
    build.Set("eboot", hash(identity.eboot));
    build.Set("events", hash(identity.event_files));
    // The multipliers as the game reads them (ParseHarderEnemies): "off" is the normal game.
    const auto harder = ParseHarderEnemies(setting("SHADPS4_BLOODBORNE_HARDER_ENEMIES", ""));
    build.Set("harder",
              harder ? fmt::format("{:g},{:g},{:g}", harder->hp, harder->damage, harder->poise)
                     : "off");
    build.Set("cheats", setting("BB_PATCH_GAMEPLAY", "none"));
    const char* revive = env("SHADPS4_BLOODBORNE_REVIVE_HITS");
    std::string rules = fmt::format(
        "lamp rests {}, revive hits {}",
        LampRestsEnabled(env("SHADPS4_BLOODBORNE_LAMP_REST")) ? "on" : "off",
        revive != nullptr && std::string_view{revive} == "presses" ? "presses" : "real");
    if (SettingIsOn(env("SHADPS4_BLOODBORNE_GUEST_AS_HOST"))) {
        rules += ", guest as host";
    }
    build.Set("rules", rules);
    // Only logged: they change how the game looks, not what happens in it.
    build.Set("fps", setting("BB_FPS_PRESET", "?") +
                         (setting("BB_ONLINE_UNLOCKED_FPS", "") == "1" ? " unlocked" : ""));
    build.Set("mods", setting("BB_MODS_ENABLED", "1") == "0" ? "off" : "on");
    build.Set("patches", setting("BB_PATCH_OTHER", "none"));
    build.Set("pbin", hash(identity.patches_bin));
    return build;
}

u64 EventFilesHash(const std::filesystem::path& folder) {
    std::error_code error;
    std::vector<std::filesystem::path> files;
    for (const auto& entry : std::filesystem::directory_iterator{folder, error}) {
        const std::string name = entry.path().filename().string();
        if (name.ends_with(".emevd.dcx")) {
            files.push_back(entry.path());
        }
    }
    if (files.empty()) {
        return 0;
    }
    std::ranges::sort(files);
    // FNV-1a over each file's name and bytes.
    u64 hash = 0xcbf29ce484222325ull;
    const auto mix = [&hash](std::span<const char> bytes) {
        for (const char byte : bytes) {
            hash = (hash ^ static_cast<u8>(byte)) * 0x100000001b3ull;
        }
    };
    std::vector<char> chunk(1 << 16);
    for (const auto& path : files) {
        const std::string name = path.filename().string();
        mix(name);
        std::ifstream file{path, std::ios::binary};
        if (!file) {
            return 0;
        }
        while (file.read(chunk.data(), static_cast<std::streamsize>(chunk.size())) ||
               file.gcount() > 0) {
            mix(std::span{chunk.data(), static_cast<size_t>(file.gcount())});
        }
    }
    return hash != 0 ? hash : 1;
}

std::unordered_set<u32> DirectKillHandles(std::span<const DirectKills> kills) {
    std::unordered_set<u32> handles;
    for (const DirectKills& member : kills) {
        handles.insert(member.handles.begin(), member.handles.end());
    }
    return handles;
}

std::string FormatWorldLogEntry(const WorldLogEntry& entry) {
    switch (entry.kind) {
    case WorldLogKind::Reset:
        return entry.map == 0
                   ? fmt::format("seq {} reset every map{}", entry.seq,
                                 entry.subject == WorldLogResetAfterWipe ? " (a wipe)" : "")
                   : fmt::format("seq {} reset {:#x}", entry.seq, entry.map);
    case WorldLogKind::Kill:
        return fmt::format("seq {} kill {:#x} on {:#x}", entry.seq, entry.subject, entry.map);
    case WorldLogKind::Unkill:
        return fmt::format("seq {} unkill {:#x} on {:#x}", entry.seq, entry.subject, entry.map);
    case WorldLogKind::ShownMap:
        return fmt::format("seq {} shown map {:#x}", entry.seq, entry.map);
    case WorldLogKind::Unshown:
        return fmt::format("seq {} unshown {:#x} on {:#x}", entry.seq, entry.subject, entry.map);
    case WorldLogKind::Shown:
        return fmt::format("seq {} shown {:#x} on {:#x} bits {:x}", entry.seq, entry.subject,
                           entry.map, entry.value);
    case WorldLogKind::FlagSet:
        return fmt::format("seq {} flag {} on", entry.seq, static_cast<s32>(entry.subject));
    case WorldLogKind::FlagClear:
        return fmt::format("seq {} flag {} off", entry.seq, static_cast<s32>(entry.subject));
    case WorldLogKind::MapStateMap:
        return fmt::format("seq {} map state of {:#x}", entry.seq, entry.map);
    case WorldLogKind::HpSource:
        return fmt::format("seq {} hp of {:x}{:08x} on {:#x}", entry.seq, entry.subject,
                           entry.value, entry.map);
    case WorldLogKind::HpValue:
        return fmt::format("seq {} hp {:#x} {}/{}", entry.seq, entry.subject,
                           static_cast<s32>(entry.value), static_cast<s32>(entry.map));
    case WorldLogKind::HpGone:
        return fmt::format("seq {} hp {:#x} gone", entry.seq, entry.subject);
    case WorldLogKind::HpSourceGone:
        return fmt::format("seq {} hp of {:x}{:08x} gone", entry.seq, entry.subject, entry.value);
    case WorldLogKind::ReturnPoint:
        return fmt::format("seq {} return point {} on {:#x}", entry.seq,
                           static_cast<s32>(entry.subject), entry.map);
    case WorldLogKind::MapFlagOn:
    case WorldLogKind::MapFlagOff:
        return fmt::format("seq {} map flag {} {}", entry.seq,
                           MapFlagBase(entry.map) + static_cast<s32>(entry.subject),
                           entry.kind == WorldLogKind::MapFlagOn ? "on" : "off");
    }
    return fmt::format("seq {} kind {} on {:#x}", entry.seq, static_cast<int>(entry.kind),
                       entry.map);
}

namespace {
constexpr u8 WorldSnapshotVersion = 1;
} // namespace

std::vector<u8> EncodeWorldSnapshot(const WorldSnapshotFrames& frames) {
    std::vector<u8> blob{WorldSnapshotVersion};
    AppendLittleEndian(blob, static_cast<u16>(frames.size()));
    for (const auto& [type, payload] : frames) {
        blob.push_back(type);
        AppendLittleEndian(blob, static_cast<u16>(payload.size()));
        blob.insert(blob.end(), payload.begin(), payload.end());
    }
    return blob;
}

std::optional<WorldSnapshotFrames> DecodeWorldSnapshot(std::span<const u8> blob) {
    if (blob.size() < 3 || blob[0] != WorldSnapshotVersion) {
        return std::nullopt;
    }
    const size_t count = ReadLittleEndian<u16>(blob, 1);
    WorldSnapshotFrames frames;
    size_t at = 3;
    while (at < blob.size()) {
        if (at + 3 > blob.size()) {
            return std::nullopt;
        }
        const u8 type = blob[at];
        const size_t size = ReadLittleEndian<u16>(blob, at + 1);
        at += 3;
        if (at + size > blob.size()) {
            return std::nullopt;
        }
        frames.emplace_back(type,
                            std::vector<u8>(blob.begin() + static_cast<std::ptrdiff_t>(at),
                                            blob.begin() + static_cast<std::ptrdiff_t>(at + size)));
        at += size;
    }
    if (frames.size() != count) {
        return std::nullopt; // cut short
    }
    return frames;
}

std::string EncodeWorldSnapshotUpdate(std::string_view member, std::span<const u8> blob) {
    std::string body{member};
    if (!blob.empty()) {
        body += ' ' + TraceHex(blob);
    }
    return body;
}

std::optional<WorldSnapshotAnswer> ParseWorldSnapshotAnswer(std::string_view body) {
    while (!body.empty() && (body.back() == '\n' || body.back() == '\r')) {
        body.remove_suffix(1);
    }
    if (body.empty()) {
        return WorldSnapshotAnswer{};
    }
    const size_t space = body.find(' ');
    if (space == std::string_view::npos) {
        return std::nullopt;
    }
    WorldSnapshotAnswer answer;
    const auto [end, error] = std::from_chars(body.data(), body.data() + space, answer.age_ms);
    auto blob = ParseTraceHex(body.substr(space + 1));
    if (error != std::errc{} || end != body.data() + space || !blob) {
        return std::nullopt;
    }
    answer.blob = std::move(*blob);
    return answer;
}

std::optional<DirectVitals> ParseVitalsUpdate(std::string_view line) {
    const std::vector<std::string_view> fields = SplitFields(line);
    if (fields.size() < 11 || fields[0].empty()) {
        return std::nullopt;
    }
    const auto map = ParseNumber<u32>(fields[1], 16);
    const auto vital = ParseVital(fields[2]);
    const auto hp = ParseNumber<s32>(fields[3]);
    const auto max = ParseNumber<s32>(fields[4]);
    const auto down = ParseNumber<u32>(fields[5]);
    const auto landed = ParseNumber<u32>(fields[6]);
    const auto needed = ParseNumber<u32>(fields[7]);
    const auto x = ParseCoordinate(fields[8]);
    const auto y = ParseCoordinate(fields[9]);
    const auto z = ParseCoordinate(fields[10]);
    if (!map || !vital || !hp || !max || !down || !landed || !needed || !x || !y || !z) {
        return std::nullopt;
    }
    DirectVitals update{MemberVitals{.member = std::string{fields[0]},
                                     .map = *map,
                                     .vital = *vital,
                                     .hp = *hp,
                                     .max = *max,
                                     .down = *down,
                                     .landed = *landed,
                                     .needed = *needed,
                                     .at = {*x, *y, *z}},
                        {}};
    for (size_t index = 11; index < fields.size(); ++index) {
        const std::string_view hit = fields[index];
        const size_t first = hit.find(':');
        const size_t second = first == std::string_view::npos ? first : hit.find(':', first + 1);
        if (second == std::string_view::npos) {
            return std::nullopt;
        }
        const auto hit_down = ParseNumber<u32>(hit.substr(first + 1, second - first - 1));
        const auto damage = ParseNumber<u32>(hit.substr(second + 1));
        if (first == 0 || !hit_down || !damage) {
            return std::nullopt;
        }
        update.hits.push_back({std::string{hit.substr(0, first)}, *hit_down, *damage});
    }
    return update;
}

VitalsBoard MergeDirectVitals(const VitalsBoard& board, std::span<const DirectVitals> direct) {
    VitalsBoard merged = board;
    for (const DirectVitals& update : direct) {
        const auto same =
            std::ranges::find(merged.members, update.self.member, &MemberVitals::member);
        if (same == merged.members.end()) {
            merged.members.push_back(update.self);
            merged.members.back().revive_damage = 0;
            continue;
        }
        const bool same_down = same->down == update.self.down;
        const u32 board_damage = same_down ? same->revive_damage : 0;
        const u32 board_cutscene = same->cutscene;
        const u32 board_vials = same->vials, board_bullets = same->bullets;
        const u32 board_lamp_uses = same->lamp_uses;
        std::vector<u32> board_bosses = same->bosses;
        *same = update.self;
        same->revive_damage = board_damage;
        same->cutscene = board_cutscene; // the direct line does not carry these
        same->vials = board_vials;
        same->bullets = board_bullets;
        same->lamp_uses = board_lamp_uses;
        same->bosses = std::move(board_bosses);
    }
    for (MemberVitals& member : merged.members) {
        u32 hits = 0;
        for (const DirectVitals& update : direct) {
            for (const ReviveHitTotal& total : update.hits) {
                if (total.target == member.member && total.down == member.down) {
                    hits += total.damage;
                }
            }
        }
        member.revive_damage = std::max(member.revive_damage, hits);
    }
    return merged;
}

std::optional<u32> WipeFor(const VitalsBoard& board, std::string_view self) {
    for (const PartyWipe& wipe : board.wipes) {
        if (std::ranges::find(wipe.members, self) != wipe.members.end()) {
            return wipe.id;
        }
    }
    return std::nullopt;
}

bool HandsOutBoss(std::optional<u8> npc_type, bool boss_sized) {
    return npc_type ? *npc_type == NpcTypeBoss : boss_sized;
}

std::string BossRunnerPick::Tick(u32 map, u32 handle, const LampPosition& boss,
                                 std::span<const MemberVitals> board, std::string_view owner) {
    const auto distance = [&](const MemberVitals& member) {
        float sum = 0.0f;
        for (size_t axis = 0; axis < 3; ++axis) {
            const float delta = member.at[axis] - boss[axis];
            sum += delta * delta;
        }
        return std::sqrt(sum);
    };
    // On the boss's map, standing or down (a fight it is still in), however far.
    const auto holds = [&](const MemberVitals& member) {
        return member.map == map && (member.vital == Vital::Up || member.vital == Vital::Down);
    };
    // A claimer keeps its fight: the game's own runner first, then by name (the same in every
    // game).
    const MemberVitals* claimer = nullptr;
    for (const MemberVitals& member : board) {
        if (!holds(member) || !std::ranges::contains(member.bosses, handle)) {
            continue;
        }
        const bool owns = !owner.empty() && member.member == owner;
        const bool claimer_owns = claimer != nullptr && claimer->member == owner;
        if (claimer == nullptr || (owns && !claimer_owns) ||
            (owns == claimer_owns && member.member < claimer->member)) {
            claimer = &member;
        }
    }
    if (claimer != nullptr) {
        current = claimer->member == owner ? std::string{} : claimer->member;
        return current;
    }
    const auto owner_line = std::ranges::find(board, owner, &MemberVitals::member);
    if (!owner.empty() && owner_line != board.end() && holds(*owner_line) &&
        distance(*owner_line) <= BossLeaveMetres) {
        current.clear();
        return current; // the game's own runner fights it already
    }
    const MemberVitals* nearest = nullptr;
    for (const MemberVitals& member : board) {
        if (member.map != map || member.vital != Vital::Up || distance(member) > BossEnterMetres) {
            continue;
        }
        if (nearest == nullptr || distance(member) < distance(*nearest) - 0.5f ||
            (distance(member) <= distance(*nearest) + 0.5f && member.member < nearest->member)) {
            nearest = &member;
        }
    }
    current = nearest ? nearest->member : std::string{};
    return current;
}

void BossClaims::Ran(u32 handle, s64 now_ms) {
    last[handle] = now_ms;
}

std::vector<u32> BossClaims::Current(s64 now_ms) {
    std::erase_if(last, [&](const auto& entry) { return now_ms - entry.second > KeepMs; });
    std::vector<u32> handles;
    for (const auto& [handle, at] : last) {
        handles.push_back(handle);
    }
    return handles;
}

bool ClaimsBoss(std::string_view runner, std::string_view self, bool own_runner, bool claimed,
                float distance) {
    if (!runner.empty()) {
        return !self.empty() && runner == self;
    }
    return own_runner && (claimed || distance <= BossLeaveMetres);
}

std::vector<u32> ClearBossGauges(std::span<u8> gauges) {
    std::vector<u32> up;
    for (std::size_t slot = 0; slot < BossGaugeSlots; ++slot) {
        if ((slot + 1) * BossGaugeSize > gauges.size()) {
            break;
        }
        u8* gauge = gauges.data() + slot * BossGaugeSize;
        s32 handle = 0;
        std::memcpy(&handle, gauge + 4, sizeof(handle));
        if (handle != -1) {
            up.push_back(static_cast<u32>(handle));
        }
        constexpr s32 None = -1;
        std::memcpy(gauge, &None, sizeof(None));
        std::memcpy(gauge + 4, &None, sizeof(None));
        std::memset(gauge + 8, 0, 8);
        gauge[0x10] = 0;
    }
    return up;
}

std::vector<u32> BossGaugesUp(std::span<const u8> gauges) {
    std::vector<u32> up;
    for (std::size_t slot = 0; (slot + 1) * BossGaugeSize <= gauges.size(); ++slot) {
        s32 handle = 0;
        std::memcpy(&handle, gauges.data() + slot * BossGaugeSize + 4, sizeof(handle));
        if (handle != -1) {
            up.push_back(static_cast<u32>(handle));
        }
    }
    return up;
}

void BulletTally::Add(u32 owner, s32 bullet) {
    ++counts[{owner, bullet}];
}

std::string BulletTally::TakeSummary(std::size_t most) {
    std::vector<std::pair<std::pair<u32, s32>, u32>> made{counts.begin(), counts.end()};
    counts.clear();
    if (made.empty()) {
        return "none";
    }
    std::ranges::stable_sort(made,
                             [](const auto& a, const auto& b) { return a.second > b.second; });
    std::string text;
    for (std::size_t index = 0; index < made.size() && index < most; ++index) {
        const auto& [key, count] = made[index];
        text += fmt::format("{}{} x{} from {:#x}", text.empty() ? "" : ", ", key.second, count,
                            key.first);
    }
    if (made.size() > most) {
        text += fmt::format(", and {} more", made.size() - most);
    }
    return text;
}

bool SkipsEnemyBarDamage(u32 handle, std::span<const u32> downed_bodies) {
    return std::ranges::find(downed_bodies, handle) != downed_bodies.end();
}

bool SomeoneElseStands(std::span<const MemberVitals> board, std::string_view self, u32 map) {
    return map != 0 && std::ranges::any_of(board, [&](const MemberVitals& member) {
               return member.member != self && member.map == map && member.vital == Vital::Up;
           });
}

bool SomeoneElseStandsAnywhere(std::span<const MemberVitals> board, std::string_view self) {
    return std::ranges::any_of(board, [&](const MemberVitals& member) {
        return member.member != self && member.map != 0 && member.vital == Vital::Up;
    });
}

std::vector<MemberVitals> OnlySessionMembers(
    std::span<const MemberVitals> board, std::string_view self,
    const std::optional<std::vector<std::string>>& session) {
    std::vector<MemberVitals> kept;
    for (const MemberVitals& member : board) {
        if (!session || member.member == self || std::ranges::contains(*session, member.member)) {
            kept.push_back(member);
        }
    }
    return kept;
}

std::optional<std::vector<std::string>> SessionBoardNames(
    const std::vector<std::optional<std::string>>& found) {
    std::vector<std::string> names;
    for (const auto& name : found) {
        if (!name) {
            return std::nullopt;
        }
        names.push_back(*name);
    }
    return names;
}

bool SessionArrival::Tick(bool in_session, bool summoned, u64 visit, bool playing, s64 now_ms) {
    if (!in_session) {
        joined.reset();
        arrived = false;
        return false;
    }
    if (!joined) {
        joined = std::pair{visit, now_ms};
        arrived = !summoned;
    }
    if (!arrived) {
        arrived = (playing && visit != joined->first) || now_ms - joined->second >= LimitMs;
    }
    return arrived;
}

void RevivePressFallback::Swung(DownedMember target, u32 damage, s64 now_ms) {
    waiting.push_back(Swing{std::move(target), damage, now_ms});
}

bool RevivePressFallback::Landed(std::string_view member, u32 down, s64 now_ms) {
    const auto at = [&](const Swing& swing) {
        return swing.target.member == member && swing.target.down == down;
    };
    std::erase_if(waiting, at);
    std::erase_if(counted, [&](const Swing& swing) { return now_ms - swing.at > LateMs; });
    const auto late = std::ranges::find_if(counted, at);
    if (late == counted.end()) {
        return false;
    }
    counted.erase(late);
    return true;
}

std::vector<RevivePressFallback::Swing> RevivePressFallback::Due(s64 now_ms) {
    std::vector<Swing> due;
    std::erase_if(waiting, [&](const Swing& swing) {
        if (now_ms - swing.at < WaitMs) {
            return false;
        }
        due.push_back(swing);
        counted.push_back(Swing{swing.target, swing.damage, now_ms});
        return true;
    });
    return due;
}

bool ReviveHits::Press(std::string_view target, u32 down, u32 damage, s64 now_ms) {
    if (last_press && now_ms - *last_press < GapMs) {
        return false;
    }
    last_press = now_ms;
    const auto total = std::ranges::find(totals, target, &ReviveHitTotal::target);
    if (total == totals.end()) {
        totals.push_back(ReviveHitTotal{std::string{target}, down, damage});
    } else if (total->down != down) {
        *total = ReviveHitTotal{std::string{target}, down, damage};
    } else {
        total->damage += damage;
    }
    return true;
}

bool TakesAnimationRequest(u64 character,
                           const std::function<u64(u64 address, size_t size)>& read) {
    u64 at = character;
    for (const u64 offset : AnimationRequestPath) {
        if (at < 0x10000) {
            return false;
        }
        at = read(at + offset, 8);
    }
    return at >= 0x10000 && (read(at + AnimationRequestFlagAt, 1) & 0xFF) != 0;
}

bool KneelsNow(bool waiting, s64 down_for_ms, bool takes_request) {
    return waiting && down_for_ms >= DownedKneelDelayMs && takes_request;
}

s32 DownedAnimation(const char* setting) {
    s32 animation = 0;
    if (setting != nullptr) {
        std::from_chars(setting, setting + std::strlen(setting), animation);
    }
    return animation;
}

bool PartyListOutdated(u64 hud, u64 generation, u64 list_hud, u64 list_generation) {
    return hud != list_hud || generation != list_generation;
}

bool SkipsHitReaction(bool down, u64 victim, u64 local_player) {
    return down && victim != 0 && victim == local_player;
}

bool StartsNewWorld(const PlaceChange& change) {
    return change.to.state == PlaceState::Playing && change.to.visit != change.from.visit &&
           change.cause != PlaceCause::Border && change.cause != PlaceCause::Teleport;
}

bool PostsShownState(s64 now_ms, s64 world_changed_ms) {
    return now_ms - world_changed_ms >= ShownSettleMs;
}

void ReviveHits::Hit(std::string_view target, u32 down, u32 damage) {
    if (damage == 0) {
        return;
    }
    const auto total = std::ranges::find(totals, target, &ReviveHitTotal::target);
    if (total == totals.end()) {
        totals.push_back(ReviveHitTotal{std::string{target}, down, damage});
    } else if (total->down != down) {
        *total = ReviveHitTotal{std::string{target}, down, damage};
    } else {
        total->damage += damage;
    }
}

std::vector<CopyTeam> DownedTeams::Tick(const std::vector<CopyTeam>& downed,
                                        const std::vector<CopyTeam>& present,
                                        std::span<const u32> invaders, bool make_hostile) {
    std::vector<CopyTeam> writes;
    const auto invader = [&](u32 handle) {
        return std::ranges::find(invaders, handle) != invaders.end();
    };
    std::erase_if(saved, [&](const CopyTeam& own) { return invader(own.handle); });
    if (make_hostile) {
        for (const CopyTeam& body : present) {
            if (invader(body.handle) && body.team != Hostile && body.team != Unchangeable) {
                writes.push_back(CopyTeam{body.handle, Hostile});
            }
        }
    }
    for (const CopyTeam& body : downed) {
        if (body.team == Unchangeable || invader(body.handle)) {
            continue;
        }
        if (std::ranges::find(saved, body.handle, &CopyTeam::handle) == saved.end()) {
            saved.push_back(body);
        }
        if (body.team != Hostile) {
            writes.push_back(CopyTeam{body.handle, Hostile});
        }
    }
    std::erase_if(saved, [&](const CopyTeam& own) {
        if (std::ranges::find(downed, own.handle, &CopyTeam::handle) != downed.end()) {
            return false;
        }
        const auto body = std::ranges::find(present, own.handle, &CopyTeam::handle);
        if (body != present.end() && body->team == Hostile) {
            writes.push_back(own);
        }
        return true;
    });
    return writes;
}

std::vector<DownedBodyPick> DownedBodyPicker::Pick(std::span<const MemberVitals> board,
                                                   std::string_view self, u32 map,
                                                   std::span<const BodyPosition> bodies) {
    const auto distance = [](const LampPosition& a, const LampPosition& b) {
        return std::hypot(a[0] - b[0], a[1] - b[1], a[2] - b[2]);
    };
    std::vector<const MemberVitals*> downed, standing;
    for (const MemberVitals& entry : board) {
        if (map == 0 || entry.member == self || entry.map != map) {
            continue;
        }
        if (entry.vital == Vital::Down) {
            downed.push_back(&entry);
        } else if (entry.vital != Vital::Off) {
            standing.push_back(&entry);
        }
    }
    std::vector<DownedBodyPick> picks;
    std::vector<u32> taken;
    // The body picked for the same down, still here and within reach of the spot.
    std::vector<const MemberVitals*> unpicked;
    for (const MemberVitals* entry : downed) {
        const auto old = std::ranges::find_if(kept, [&](const DownedBodyPick& pick) {
            return pick.member.member == entry->member && pick.member.down == entry->down;
        });
        const auto body = old != kept.end()
                              ? std::ranges::find(bodies, old->handle, &BodyPosition::handle)
                              : bodies.end();
        if (body != bodies.end() && distance(body->at, entry->at) <= Reach &&
            !std::ranges::contains(taken, body->handle)) {
            picks.push_back(*old);
            taken.push_back(body->handle);
        } else {
            unpicked.push_back(entry);
        }
    }
    // New picks: the nearest member and body pairs first, the members standing up claiming theirs.
    struct Pair {
        float distance;
        const MemberVitals* member;
        u32 handle;
    };
    std::vector<Pair> pairs;
    for (const BodyPosition& body : bodies) {
        if (std::ranges::contains(taken, body.handle)) {
            continue;
        }
        for (const MemberVitals* entry : unpicked) {
            if (const float d = distance(body.at, entry->at); d <= Reach) {
                pairs.push_back(Pair{d, entry, body.handle});
            }
        }
        for (const MemberVitals* entry : standing) {
            pairs.push_back(Pair{distance(body.at, entry->at), entry, body.handle});
        }
    }
    std::ranges::stable_sort(pairs, {}, &Pair::distance);
    std::vector<const MemberVitals*> matched;
    for (const Pair& pair : pairs) {
        if (std::ranges::contains(matched, pair.member) ||
            std::ranges::contains(taken, pair.handle)) {
            continue;
        }
        matched.push_back(pair.member);
        taken.push_back(pair.handle);
        if (pair.member->vital == Vital::Down) {
            picks.push_back(
                DownedBodyPick{DownedMember{pair.member->member, pair.member->down}, pair.handle});
        }
    }
    kept = picks;
    return picks;
}

std::vector<ReviveHitTotal> ReviveHits::Totals(std::span<const MemberVitals> board, bool fresh) {
    if (!fresh) {
        return totals;
    }
    std::erase_if(totals, [&](const ReviveHitTotal& total) {
        return std::ranges::none_of(board, [&](const MemberVitals& member) {
            return member.member == total.target && member.vital == Vital::Down &&
                   member.down == total.down;
        });
    });
    return totals;
}

std::optional<DownedMember> ReviveTarget(std::span<const MemberVitals> board, std::string_view self,
                                         u32 map, const LampPosition& me) {
    std::optional<DownedMember> nearest;
    float nearest_distance = ReviveHits::Reach * ReviveHits::Reach;
    for (const MemberVitals& member : board) {
        if (member.member == self || member.map != map || member.vital != Vital::Down) {
            continue;
        }
        const float distance = DistanceSquared(member.at, me);
        if (distance <= nearest_distance) {
            nearest_distance = distance;
            nearest = DownedMember{member.member, member.down};
        }
    }
    return nearest;
}

PadInput DownedPad(const PadInput& pad) {
    return PadInput{.sticks = {128, 128, pad.sticks[2], pad.sticks[3]}};
}

bool ReleasesLockOn(s32 cursor, s64 now_ms, std::optional<s64> last_press_ms) {
    return cursor == 0 && (!last_press_ms || now_ms - *last_press_ms >= LockOnReleaseGapMs);
}

bool AttackPressed(u32 before, u32 now) {
    return (now & ~before & (PadR1 | PadR2)) != 0;
}

bool HeavyAttackPressed(u32 before, u32 now) {
    return (now & ~before & PadR2) != 0 && (now & PadR1) == 0;
}

DownEnd EndOfDown(bool wiped, bool gave_up, bool revived, s64 board_age_ms, bool in_session) {
    if (wiped) {
        return DownEnd::Wipe;
    }
    if (gave_up) {
        return DownEnd::GiveUp;
    }
    if (revived) {
        return DownEnd::Revive;
    }
    if (!in_session) {
        return DownEnd::SessionLost;
    }
    return board_age_ms >= BoardLostMs ? DownEnd::BoardLost : DownEnd::None;
}

bool ForgetsTargetNode(u8 slot, s32 kind, u32 handle, std::span<const u32> downed) {
    return (slot & 0x7F) == 1 && kind == 1 && std::ranges::find(downed, handle) != downed.end();
}

std::string TargetCandidateBlock(const TargetCandidate& candidate) {
    if (candidate.state != 6) {
        return fmt::format("state {} (not 6)", candidate.state);
    }
    if (!candidate.built) {
        return "not built";
    }
    if (!candidate.listed) {
        return "not in the AI list";
    }
    if ((candidate.control & 1) != 0) {
        return "disabled";
    }
    if ((candidate.control & 4) != 0) {
        return "control 0x4";
    }
    if ((candidate.control & 2) != 0 && !(candidate.has_info && (candidate.info_18 & 1) != 0)) {
        return "control 0x2 without info bit 0";
    }
    if (candidate.wake != 4) {
        return fmt::format("wake {} (not 4)", candidate.wake);
    }
    if (candidate.has_info && (candidate.info_d & 0x10) != 0) {
        return "info 0xd bit 0x10";
    }
    if (candidate.has_info && (candidate.info_18 & 1) != 0) {
        return "marked dead (info 0x18 bit 0)";
    }
    return "";
}

std::vector<u32> StaleDeathMarks::Tick(std::span<const Body> bodies, s64 now_ms) {
    std::vector<u32> clear;
    std::vector<std::pair<u32, s64>> kept;
    for (const Body& body : bodies) {
        if (!body.marked || body.dead || body.downed || body.invader) {
            continue;
        }
        const auto seen = std::ranges::find(up_since, body.handle, &std::pair<u32, s64>::first);
        const s64 since = seen != up_since.end() ? seen->second : now_ms;
        if (now_ms - since >= HoldMs) {
            clear.push_back(body.handle);
        } else {
            kept.emplace_back(body.handle, since);
        }
    }
    up_since = std::move(kept);
    return clear;
}

std::vector<LampPosition> OthersDownOnMap(std::span<const MemberVitals> board,
                                          std::string_view self, u32 map) {
    std::vector<LampPosition> downed;
    for (const MemberVitals& member : board) {
        if (map != 0 && member.member != self && member.map == map && member.vital == Vital::Down) {
            downed.push_back(member.at);
        }
    }
    return downed;
}

bool DeathResetsMap(bool wiped, bool someone_else_stands) {
    return wiped || !someone_else_stands;
}

std::optional<Bloodstain> BloodstainOf(bool has_mark, u32 map, const LampPosition& at, s32 echoes) {
    if (!has_mark || map == 0 || map == 0xFFFFFFFF || echoes <= 0) {
        return std::nullopt;
    }
    return Bloodstain{.map = map, .at = at, .echoes = echoes};
}

std::optional<Bloodstain> BloodstainOfRecord(bool has_mark, std::span<const u8> record) {
    if (record.size() < BloodstainRecordSize) {
        return std::nullopt;
    }
    const auto read = [record]<typename T>(size_t offset, T value) {
        std::memcpy(&value, record.data() + offset, sizeof(value));
        return value;
    };
    return BloodstainOf(has_mark, read(0x3C, u32{}),
                        {read(0x00, 0.0f), read(0x04, 0.0f), read(0x08, 0.0f)}, read(0x38, s32{}));
}

PartyPanel BuildPartyPanel(std::span<const MemberVitals> board, std::string_view self, u32 map,
                           const std::optional<LampPosition>& me) {
    PartyPanel panel;
    const auto target = me ? ReviveTarget(board, self, map, *me) : std::nullopt;
    for (const MemberVitals& entry : board) {
        if (entry.member == self || entry.vital == Vital::Off) {
            continue;
        }
        if (target && entry.member == target->member) {
            panel.revive_target = static_cast<s32>(panel.members.size());
        }
        panel.members.push_back(PanelMember{
            .member = entry.member,
            .name = OnlineIdOfBoardMember(entry.member),
            .vital = entry.vital,
            .here = map != 0 && entry.map == map,
            .hp = entry.hp,
            .max = entry.max,
            .landed = entry.landed,
            .needed = entry.needed,
            // Kaniel 2026-10-25: a spectator is nowhere to go to (its body lies unseen).
            .at = map != 0 && entry.map == map && entry.vital != Vital::Spectate
                      ? std::optional{entry.at}
                      : std::nullopt,
            .map = entry.map,
            .vials = entry.vials,
            .bullets = entry.bullets,
        });
    }
    return panel;
}

bool PresenceHides(s64 now_ms, s64 first_seen_ms, s64 unmatched_since_ms) {
    return now_ms - unmatched_since_ms >= PresenceGraceMs &&
           now_ms - first_seen_ms >= PresenceNewCharacterMs;
}

std::vector<bool> ShownOtherPlayers(std::span<const std::optional<LampPosition>> characters,
                                    std::span<const MemberVitals> board, std::string_view self,
                                    u32 map) {
    const std::vector<std::optional<size_t>> members =
        VouchingMembers(characters, board, self, map);
    std::vector<bool> shown(characters.size(), false);
    for (size_t c = 0; c < characters.size(); ++c) {
        shown[c] = !characters[c] || members[c].has_value();
    }
    return shown;
}

std::vector<std::optional<size_t>> VouchingMembers(
    std::span<const std::optional<LampPosition>> characters, std::span<const MemberVitals> board,
    std::string_view self, u32 map) {
    // Kaniel's playtest 2026-10-16: krolebord's copy stayed by the lantern after his Dream trip,
    // kept shown by Lisara standing 16 m away. Each member on this map vouches for one character
    // only: the nearest pairs first.
    std::vector<std::optional<size_t>> vouched_by(characters.size());
    struct Pair {
        float distance;
        size_t character;
        size_t member;
    };
    std::vector<Pair> pairs;
    for (size_t c = 0; c < characters.size(); ++c) {
        const std::optional<LampPosition>& at = characters[c];
        if (!at) {
            continue;
        }
        for (size_t m = 0; m < board.size(); ++m) {
            const MemberVitals& member = board[m];
            if (member.member == self || member.map != map || map == 0) {
                continue;
            }
            const float dx = member.at[0] - (*at)[0];
            const float dy = member.at[1] - (*at)[1];
            const float dz = member.at[2] - (*at)[2];
            const float distance = dx * dx + dy * dy + dz * dz;
            if (distance <= PresenceMetres * PresenceMetres) {
                pairs.push_back({distance, c, m});
            }
        }
    }
    std::ranges::sort(pairs, {}, &Pair::distance);
    std::vector<bool> vouched(board.size(), false);
    for (const Pair& pair : pairs) {
        if (!vouched_by[pair.character] && !vouched[pair.member]) {
            vouched_by[pair.character] = pair.member;
            vouched[pair.member] = true;
        }
    }
    return vouched_by;
}

bool FadeBack::Tick(float fade, bool someone_died, std::optional<Vital> member) {
    if (fade >= 1.0f) {
        died = false;
        return false;
    }
    died = died || someone_died;
    if (!died || member != Vital::Up) {
        return false;
    }
    died = false;
    return true;
}

std::vector<bool> StaleDuplicatePlayers(std::span<const u64> identities,
                                        std::span<const s64> first_seen_ms) {
    std::vector<bool> stale(identities.size(), false);
    for (size_t i = 0; i < identities.size(); ++i) {
        for (size_t j = 0; j < identities.size() && !stale[i]; ++j) {
            const bool later = first_seen_ms[j] > first_seen_ms[i] ||
                               (first_seen_ms[j] == first_seen_ms[i] && j > i);
            stale[i] = j != i && identities[i] != 0 && identities[j] == identities[i] && later;
        }
    }
    return stale;
}

bool InGameImage(u64 address, u64 base, u64 size) {
    return address >= base && address - base < size;
}

bool MemberPacketTakers::Takes(u64 network, u64 identity, s64 now_ms) {
    std::erase_if(takers, [&](const Taker& taker) {
        return taker.network != network && now_ms - taker.last_ms > ForgetMs;
    });
    auto mine = std::ranges::find(takers, network, &Taker::network);
    if (mine == takers.end()) {
        takers.push_back({network, identity, now_ms, now_ms});
        mine = std::prev(takers.end());
    } else if (mine->identity != identity || now_ms - mine->last_ms > ForgetMs) {
        *mine = {network, identity, now_ms, now_ms};
    }
    mine->last_ms = now_ms;
    if (identity == 0) {
        return true;
    }
    const Taker me = *mine;
    return std::ranges::none_of(takers, [&](const Taker& other) {
        return other.network != me.network && other.identity == me.identity &&
               (other.first_ms > me.first_ms ||
                (other.first_ms == me.first_ms && other.network > me.network));
    });
}

} // namespace Core::Bloodborne
