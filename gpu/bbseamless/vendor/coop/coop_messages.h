// SPDX-FileCopyrightText: Copyright 2026 shadNet Project
// SPDX-License-Identifier: GPL-2.0-or-later

// coop.proto's messages and the world log's types (coop/world_log.h), both ways: the games and
// shadNet build the same messages from the same values.
#pragma once

#include <array>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "coop.pb.h"
#include "coop/coop_types.h"
#include "coop/world_log.h"

namespace BloodborneCoop {

// The name the party's log goes by on the server (WorldLogBatch::host and the like).
constexpr std::string_view ServerWorldHost = "shadnet";
constexpr u32 CoopProtocol = bbcoop::PROTOCOL_CURRENT;

std::vector<u8> EncodeEnvelope(const bbcoop::Envelope& envelope);
std::optional<bbcoop::Envelope> DecodeEnvelope(std::span<const u8> bytes);

void PutKills(const WorldLogKills& kills,
              google::protobuf::RepeatedPtrField<bbcoop::MapKills>* out);
WorldLogKills KillsOf(const google::protobuf::RepeatedPtrField<bbcoop::MapKills>& kills);

// A member's proposal; the member's name is the server's (from its hello), not the message's.
bbcoop::KillProposal ToMessage(const WorldLogProposal& proposal);
WorldLogProposal ProposalOf(const bbcoop::KillProposal& message, std::string member);

bbcoop::Entry ToMessage(const WorldLogEntry& entry);
WorldLogEntry EntryOf(const bbcoop::Entry& message, u64 epoch);
bbcoop::Entries ToMessage(u64 epoch, u64 last, std::span<const WorldLogEntry> entries);
WorldLogBatch BatchOf(const bbcoop::Entries& message);

// The host's characters, flags, map state, return point and HP (phase 3b).
bbcoop::ShownReport ToMessage(u32 map, std::span<const CharacterShown> characters);
std::vector<CharacterShown> CharactersOf(const bbcoop::ShownReport& message);
bbcoop::FlagProposal ToMessage(const WorldLogFlagProposal& proposal);
WorldLogFlagProposal FlagsOf(const bbcoop::FlagProposal& message, std::string member);
bbcoop::HpProposal ToMessage(const WorldLogHpProposal& proposal);
WorldLogHpProposal HpOf(const bbcoop::HpProposal& message, std::string member);

// The party's world at the log's last seq, and back as the parts a member takes (in this order:
// the kills, the shown characters, the map state, the HP reports, the recent flags last).
bbcoop::Snapshot SnapshotOf(const WorldLogHost& host);
struct SnapshotParts {
    WorldLogSnapshot kills;
    WorldLogShownSnapshot shown;
    WorldLogMapStateSnapshot map_state;
    WorldLogHpSnapshot hp;
    WorldLogFlagsSnapshot flags;
};
SnapshotParts PartsOf(const bbcoop::Snapshot& snapshot);

// Kaniel 2026-10-24: a team ping as the games keep it (bbcoop::Ping); `member` is the server's.
struct TeamPing {
    std::string member;
    u32 map = 0;
    std::array<float, 3> at{};
    u32 enemy = 0;
    u32 seq = 0;
    bool from_map = false;
};
bbcoop::Ping ToMessage(const TeamPing& ping);
TeamPing PingOf(const bbcoop::Ping& message);

// The world store's copy of a log (WorldLogHost::State).
bbcoop::StoredHost ToMessage(const WorldLogHost::State& state);
WorldLogHost::State StateOf(const bbcoop::StoredHost& message);

} // namespace BloodborneCoop
