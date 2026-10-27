// SPDX-FileCopyrightText: Copyright 2026 shadNet Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "coop/coop_messages.h"

namespace BloodborneCoop {

std::vector<u8> EncodeEnvelope(const bbcoop::Envelope& envelope) {
    std::vector<u8> bytes(envelope.ByteSizeLong());
    if (!bytes.empty()) {
        envelope.SerializeToArray(bytes.data(), static_cast<int>(bytes.size()));
    }
    return bytes;
}

std::optional<bbcoop::Envelope> DecodeEnvelope(std::span<const u8> bytes) {
    bbcoop::Envelope envelope;
    if (!envelope.ParseFromArray(bytes.data(), static_cast<int>(bytes.size()))) {
        return std::nullopt;
    }
    return envelope;
}

void PutKills(const WorldLogKills& kills,
              google::protobuf::RepeatedPtrField<bbcoop::MapKills>* out) {
    for (const auto& [map, handles] : kills) {
        if (handles.empty()) {
            continue;
        }
        bbcoop::MapKills* on_map = out->Add();
        on_map->set_map(map);
        for (const u32 handle : handles) {
            on_map->add_handles(handle);
        }
    }
}

WorldLogKills KillsOf(const google::protobuf::RepeatedPtrField<bbcoop::MapKills>& kills) {
    WorldLogKills out;
    for (const bbcoop::MapKills& on_map : kills) {
        if (on_map.handles_size() == 0) {
            continue;
        }
        std::set<u32>& handles = out[on_map.map()];
        handles.insert(on_map.handles().begin(), on_map.handles().end());
    }
    return out;
}

bbcoop::KillProposal ToMessage(const WorldLogProposal& proposal) {
    bbcoop::KillProposal message;
    message.set_epoch(proposal.epoch);
    message.set_run(proposal.run);
    message.set_number(proposal.number);
    PutKills(proposal.kills, message.mutable_kills());
    return message;
}

WorldLogProposal ProposalOf(const bbcoop::KillProposal& message, std::string member) {
    return WorldLogProposal{std::move(member), message.epoch(), message.run(), message.number(),
                            KillsOf(message.kills())};
}

bbcoop::Entry ToMessage(const WorldLogEntry& entry) {
    bbcoop::Entry message;
    message.set_seq(entry.seq);
    message.set_map(entry.map);
    message.set_kind(static_cast<u32>(entry.kind));
    message.set_subject(entry.subject);
    message.set_value(entry.value);
    return message;
}

WorldLogEntry EntryOf(const bbcoop::Entry& message, u64 epoch) {
    return WorldLogEntry{message.seq(),     epoch,
                         message.map(),     static_cast<WorldLogKind>(message.kind()),
                         message.subject(), message.value()};
}

bbcoop::Entries ToMessage(u64 epoch, u64 last, std::span<const WorldLogEntry> entries) {
    bbcoop::Entries message;
    message.set_epoch(epoch);
    message.set_last(last);
    for (const WorldLogEntry& entry : entries) {
        *message.add_entries() = ToMessage(entry);
    }
    return message;
}

WorldLogBatch BatchOf(const bbcoop::Entries& message) {
    WorldLogBatch batch{std::string{ServerWorldHost}, message.epoch(), message.last(), {}};
    for (const bbcoop::Entry& entry : message.entries()) {
        batch.entries.push_back(EntryOf(entry, message.epoch()));
    }
    return batch;
}

namespace {

void PutHp(const WorldLogHp& hp, google::protobuf::RepeatedPtrField<bbcoop::HpReport>* out) {
    for (const auto& [source, report] : hp) {
        bbcoop::HpReport* message = out->Add();
        message->set_source(source);
        message->set_map(report.map);
        for (const auto& [handle, value] : report.enemies) {
            bbcoop::HpEnemy* enemy = message->add_enemies();
            enemy->set_handle(handle);
            enemy->set_value(value.first);
            enemy->set_max(value.second);
        }
    }
}

WorldLogHp ReportsOf(const google::protobuf::RepeatedPtrField<bbcoop::HpReport>& reports) {
    WorldLogHp hp;
    for (const bbcoop::HpReport& message : reports) {
        WorldLogHpReport& report = hp[message.source()];
        report.map = message.map();
        for (const bbcoop::HpEnemy& enemy : message.enemies()) {
            report.enemies[enemy.handle()] = {enemy.value(), enemy.max()};
        }
    }
    return hp;
}

void PutShown(const WorldLogShown& shown,
              google::protobuf::RepeatedPtrField<bbcoop::ShownCharacter>* out) {
    for (const auto& [handle, bits] : shown.bits) {
        bbcoop::ShownCharacter* character = out->Add();
        character->set_handle(handle);
        character->set_bits(bits);
    }
}

WorldLogShown ShownOf(u32 map,
                      const google::protobuf::RepeatedPtrField<bbcoop::ShownCharacter>& shown) {
    WorldLogShown out{map, {}};
    for (const bbcoop::ShownCharacter& character : shown) {
        out.bits[character.handle()] = static_cast<u8>(character.bits());
    }
    return out;
}

std::vector<u8> BytesOf(const std::string& bytes) {
    return {bytes.begin(), bytes.end()};
}

std::string StringOf(std::span<const u8> bytes) {
    return {bytes.begin(), bytes.end()};
}

} // namespace

bbcoop::ShownReport ToMessage(u32 map, std::span<const CharacterShown> characters) {
    bbcoop::ShownReport message;
    message.set_map(map);
    for (const CharacterShown& character : characters) {
        bbcoop::ShownCharacter* shown = message.add_characters();
        shown->set_handle(character.handle);
        shown->set_bits(ShownBits(character));
    }
    return message;
}

std::vector<CharacterShown> CharactersOf(const bbcoop::ShownReport& message) {
    std::vector<CharacterShown> characters;
    characters.reserve(static_cast<size_t>(message.characters_size()));
    for (const bbcoop::ShownCharacter& shown : message.characters()) {
        characters.push_back(ShownFromBits(shown.handle(), static_cast<u8>(shown.bits())));
    }
    return characters;
}

bbcoop::FlagProposal ToMessage(const WorldLogFlagProposal& proposal) {
    bbcoop::FlagProposal message;
    message.set_epoch(proposal.epoch);
    message.set_run(proposal.run);
    for (const WorldLogFlagEvent& event : proposal.events) {
        bbcoop::FlagEvent* out = message.add_events();
        out->set_number(event.number);
        out->set_flag(event.flag);
        out->set_on(event.on);
    }
    return message;
}

WorldLogFlagProposal FlagsOf(const bbcoop::FlagProposal& message, std::string member) {
    WorldLogFlagProposal proposal{std::move(member), message.epoch(), message.run(), {}};
    for (const bbcoop::FlagEvent& event : message.events()) {
        proposal.events.push_back({event.number(), event.flag(), event.on()});
    }
    return proposal;
}

bbcoop::HpProposal ToMessage(const WorldLogHpProposal& proposal) {
    bbcoop::HpProposal message;
    message.set_epoch(proposal.epoch);
    message.set_run(proposal.run);
    message.set_number(proposal.number);
    message.set_map(proposal.map);
    message.set_source(proposal.report.visit);
    for (const EnemyHp& enemy : proposal.report.enemies) {
        bbcoop::HpEnemy* out = message.add_enemies();
        out->set_handle(enemy.handle);
        out->set_value(enemy.hp);
        out->set_max(enemy.max);
    }
    return message;
}

WorldLogHpProposal HpOf(const bbcoop::HpProposal& message, std::string member) {
    WorldLogHpProposal proposal{std::move(member), message.epoch(), message.run(),
                                message.number(),  message.map(),   {message.source(), {}}};
    for (const bbcoop::HpEnemy& enemy : message.enemies()) {
        proposal.report.enemies.push_back({enemy.handle(), enemy.value(), enemy.max()});
    }
    return proposal;
}

bbcoop::Snapshot SnapshotOf(const WorldLogHost& host) {
    bbcoop::Snapshot message;
    message.set_epoch(host.Epoch());
    message.set_last(host.Last());
    PutKills(host.Kills(), message.mutable_kills());
    message.set_shown_map(host.Shown().map);
    PutShown(host.Shown(), message.mutable_shown());
    message.set_map_state_map(host.MapStateNow().map);
    message.set_map_state_bits(StringOf(host.MapStateNow().bits));
    message.set_return_point(host.ReturnPointNow().point);
    message.set_return_point_map(host.ReturnPointNow().map);
    PutHp(host.Hp(), message.mutable_hp());
    const WorldLogFlagsSnapshot flags = host.FlagsSnapshot(ServerWorldHost);
    for (const WorldLogEntry& entry : flags.events) {
        *message.add_recent_flags() = ToMessage(entry);
    }
    message.set_after_wipe(flags.after_wipe);
    return message;
}

SnapshotParts PartsOf(const bbcoop::Snapshot& snapshot) {
    const std::string host{ServerWorldHost};
    const u64 epoch = snapshot.epoch();
    const u64 last = snapshot.last();
    SnapshotParts parts;
    parts.kills = {host, epoch, last, 0, 1, KillsOf(snapshot.kills())};
    parts.shown = {host, epoch, last, 0, 1, ShownOf(snapshot.shown_map(), snapshot.shown())};
    parts.map_state = {host,
                       epoch,
                       last,
                       {snapshot.map_state_map(), BytesOf(snapshot.map_state_bits())},
                       {snapshot.return_point(), snapshot.return_point_map()}};
    parts.hp = {host, epoch, last, 0, 1, ReportsOf(snapshot.hp())};
    parts.flags = {host, epoch, last, {}, snapshot.after_wipe()};
    for (const bbcoop::Entry& entry : snapshot.recent_flags()) {
        parts.flags.events.push_back(EntryOf(entry, epoch));
    }
    return parts;
}

bbcoop::Ping ToMessage(const TeamPing& ping) {
    bbcoop::Ping message;
    message.set_map(ping.map);
    message.mutable_at()->set_x(ping.at[0]);
    message.mutable_at()->set_y(ping.at[1]);
    message.mutable_at()->set_z(ping.at[2]);
    message.set_enemy(ping.enemy);
    message.set_seq(ping.seq);
    message.set_from_map(ping.from_map);
    return message;
}

TeamPing PingOf(const bbcoop::Ping& message) {
    return TeamPing{.member = message.member(),
                    .map = message.map(),
                    .at = {message.at().x(), message.at().y(), message.at().z()},
                    .enemy = message.enemy(),
                    .seq = message.seq(),
                    .from_map = message.from_map()};
}

bbcoop::StoredHost ToMessage(const WorldLogHost::State& state) {
    bbcoop::StoredHost message;
    message.set_epoch(state.epoch);
    message.set_last(state.last);
    for (const auto& [name, member] : state.members) {
        bbcoop::StoredMember* stored = message.add_members();
        stored->set_name(name);
        stored->set_run(member.run);
        stored->set_number(member.number);
        PutKills(member.kills, stored->mutable_kills());
        for (const u32 handle : member.voided) {
            stored->add_voided(handle);
        }
    }
    for (const auto& [handle, standing] : state.standing) {
        bbcoop::StoredStanding* stored = message.add_standing();
        stored->set_handle(handle);
        stored->set_map(standing.map);
        for (const std::string& by : standing.by) {
            stored->add_by(by);
        }
    }
    message.set_shown_map(state.shown.map);
    PutShown(state.shown, message.mutable_shown());
    for (const auto& [name, sender] : state.flag_senders) {
        bbcoop::StoredSender* stored = message.add_flag_senders();
        stored->set_name(name);
        stored->set_run(sender.run);
        stored->set_number(sender.number);
    }
    for (const auto& [event, at_ms] : state.flag_events) {
        bbcoop::StoredFlagSeen* stored = message.add_flag_events();
        stored->set_flag(event.first);
        stored->set_on(event.second);
        stored->set_at_ms(at_ms);
    }
    message.set_map_state_map(state.map_state.map);
    message.set_map_state_bits(StringOf(state.map_state.bits));
    for (const auto& [name, sender] : state.hp_senders) {
        bbcoop::StoredSender* stored = message.add_hp_senders();
        stored->set_name(name);
        stored->set_run(sender.run);
        stored->set_number(sender.number);
        stored->set_source(sender.source);
    }
    PutHp(state.hp, message.mutable_hp());
    for (const WorldLogEntry& entry : state.recent_flags) {
        *message.add_recent_flags() = ToMessage(entry);
    }
    message.set_started_after_wipe(state.started_after_wipe);
    message.set_return_point(state.return_point.point);
    message.set_return_point_map(state.return_point.map);
    return message;
}

WorldLogHost::State StateOf(const bbcoop::StoredHost& message) {
    WorldLogHost::State state;
    state.epoch = message.epoch();
    state.last = message.last();
    for (const bbcoop::StoredMember& stored : message.members()) {
        state.members[stored.name()] = {stored.run(),
                                        stored.number(),
                                        KillsOf(stored.kills()),
                                        {stored.voided().begin(), stored.voided().end()}};
    }
    for (const bbcoop::StoredStanding& stored : message.standing()) {
        WorldLogHost::Standing& standing = state.standing[stored.handle()];
        standing.map = stored.map();
        standing.by.insert(stored.by().begin(), stored.by().end());
    }
    state.shown = ShownOf(message.shown_map(), message.shown());
    for (const bbcoop::StoredSender& stored : message.flag_senders()) {
        state.flag_senders[stored.name()] = {stored.run(), stored.number()};
    }
    for (const bbcoop::StoredFlagSeen& stored : message.flag_events()) {
        state.flag_events[{stored.flag(), stored.on()}] = stored.at_ms();
    }
    state.map_state = {message.map_state_map(), BytesOf(message.map_state_bits())};
    for (const bbcoop::StoredSender& stored : message.hp_senders()) {
        state.hp_senders[stored.name()] = {stored.run(), stored.number(), stored.source()};
    }
    state.hp = ReportsOf(message.hp());
    for (const bbcoop::Entry& entry : message.recent_flags()) {
        state.recent_flags.push_back(EntryOf(entry, message.epoch()));
    }
    state.started_after_wipe = message.started_after_wipe();
    state.return_point = {message.return_point(), message.return_point_map()};
    return state;
}

} // namespace BloodborneCoop
