// SPDX-FileCopyrightText: Copyright 2026 shadNet Project
// SPDX-License-Identifier: GPL-2.0-or-later
#include "bloodborne_party_boards.h"

#include <algorithm>

namespace BloodborneWorld {

namespace {

constexpr std::uint32_t VitalUp = 1;
constexpr std::uint32_t VitalDown = 2;
constexpr std::uint32_t VitalDead = 3;

bool Same(const google::protobuf::MessageLite& a, const google::protobuf::MessageLite& b) {
    return a.SerializeAsString() == b.SerializeAsString();
}

} // namespace

unsigned PartyBoards::Map(const std::string& member, std::uint32_t map, std::int64_t now_ms) {
    MapEntry& entry = maps[member];
    const bool changed = entry.at_ms == 0 || entry.map != map;
    entry.map = map;
    entry.at_ms = now_ms;
    return changed ? Maps : None;
}

unsigned PartyBoards::Rest(const std::string& member, const bbcoop::Rest& rest,
                           std::int64_t now_ms) {
    if (rest.lamp() == 0) {
        return rests.erase(member) > 0 ? Rests : None;
    }
    RestEntry& entry = rests[member];
    const bool changed = entry.at_ms == 0 || !Same(entry.rest, rest);
    entry.rest = rest;
    entry.at_ms = now_ms;
    return changed ? Rests : None;
}

unsigned PartyBoards::Trophies(const bbcoop::Trophies& message) {
    unsigned changed = None;
    for (const std::int32_t id : message.ids()) {
        if (id >= 0 && id <= MaxTrophyId && trophies.insert(id).second) {
            changed = TrophyBoard;
        }
    }
    return changed;
}

unsigned PartyBoards::Vitals(const std::string& member, const bbcoop::Vitals& message,
                             std::int64_t now_ms) {
    VitalsEntry& entry = vitals[member];
    unsigned changed = entry.at_ms == 0 || !Same(entry.vitals, message) ? VitalsBoard : None;
    const auto fallen = [](std::uint32_t vital) {
        return vital == VitalDown || vital == VitalDead;
    };
    const bool was_up = entry.at_ms != 0 && entry.vitals.vital() == VitalUp;
    const bool was_fallen = entry.at_ms != 0 && fallen(entry.vitals.vital());
    if (message.vital() == VitalUp && !was_up) {
        entry.up_since_ms = now_ms;
    }
    if (fallen(message.vital()) && !was_fallen) {
        entry.fell_ms = now_ms;
    }
    entry.vitals = message;
    entry.at_ms = now_ms;
    const std::uint32_t map = message.map();
    if (map == 0) {
        return changed;
    }
    bool standing = false;
    std::vector<std::string> down;
    for (const auto& [name, other] : vitals) {
        if (other.vitals.map() != map) {
            continue;
        }
        standing = standing || other.vitals.vital() == VitalUp;
        const bool wiped = std::ranges::any_of(wipes, [&](const Wipe& wipe) {
            return std::ranges::find(wipe.wipe.members(), name) != wipe.wipe.members().end();
        });
        if (other.vitals.vital() == VitalDown && !wiped) {
            down.push_back(name);
        }
    }
    if (!standing && !down.empty()) {
        Wipe wipe;
        wipe.wipe.set_id(++last_wipe);
        wipe.wipe.set_map(map);
        for (const std::string& name : down) {
            wipe.wipe.add_members(name);
        }
        wipe.declared_ms = now_ms;
        wipes.push_back(std::move(wipe));
        changed |= VitalsBoard;
    }
    return changed;
}

bool PartyBoards::StandsSinceFell(const VitalsEntry& other, const std::string& member) const {
    if (other.vitals.vital() != VitalUp) {
        return false;
    }
    const auto self = vitals.find(member);
    return self == vitals.end() || self->second.fell_ms == 0 ||
           other.up_since_ms <= self->second.fell_ms;
}

bool PartyBoards::SomeoneStandsBesides(const std::string& member) const {
    return std::ranges::any_of(vitals, [&](const auto& entry) {
        return entry.first != member && entry.second.vitals.map() != 0 &&
               StandsSinceFell(entry.second, member);
    });
}

bool PartyBoards::SomeoneStandsOn(const std::vector<std::uint32_t>& maps,
                                  const std::string& member) const {
    std::set<std::string> wiped_with;
    for (const Wipe& wipe : wipes) {
        const auto& members = wipe.wipe.members();
        if (std::ranges::find(members, member) != members.end()) {
            wiped_with.insert(members.begin(), members.end());
        }
    }
    return std::ranges::any_of(vitals, [&](const auto& entry) {
        return entry.first != member && !wiped_with.contains(entry.first) &&
               StandsSinceFell(entry.second, member) &&
               std::ranges::find(maps, entry.second.vitals.map()) != maps.end();
    });
}

unsigned PartyBoards::Expire(std::int64_t now_ms) {
    unsigned changed = None;
    const auto stale = [now_ms](const auto& entry) {
        return now_ms - entry.second.at_ms > FreshMs;
    };
    if (std::erase_if(maps, stale) > 0) {
        changed |= Maps;
    }
    if (std::erase_if(rests, stale) > 0) {
        changed |= Rests;
    }
    if (std::erase_if(vitals, stale) > 0) {
        changed |= VitalsBoard;
    }
    if (std::erase_if(
            wipes, [now_ms](const Wipe& wipe) { return now_ms - wipe.declared_ms > WipeMs; }) > 0) {
        changed |= VitalsBoard;
    }
    return changed;
}

unsigned PartyBoards::Forget(const std::string& member) {
    unsigned changed = None;
    changed |= maps.erase(member) > 0 ? Maps : None;
    changed |= rests.erase(member) > 0 ? Rests : None;
    changed |= vitals.erase(member) > 0 ? VitalsBoard : None;
    return changed;
}

bbcoop::MapBoard PartyBoards::MapMessage() const {
    bbcoop::MapBoard board;
    for (const auto& [member, entry] : maps) {
        bbcoop::MapBoard::Entry* out = board.add_members();
        out->set_member(member);
        out->set_map(entry.map);
    }
    return board;
}

bbcoop::RestBoard PartyBoards::RestMessage() const {
    bbcoop::RestBoard board;
    for (const auto& [member, entry] : rests) {
        bbcoop::RestBoard::Entry* out = board.add_rests();
        out->set_member(member);
        out->set_lamp(entry.rest.lamp());
        if (entry.rest.has_at()) {
            *out->mutable_at() = entry.rest.at();
        }
    }
    return board;
}

bbcoop::Trophies PartyBoards::TrophyMessage() const {
    bbcoop::Trophies board;
    for (const std::int32_t id : trophies) {
        board.add_ids(id);
    }
    return board;
}

bbcoop::VitalsBoard PartyBoards::VitalsMessage() const {
    bbcoop::VitalsBoard board;
    for (const auto& [member, entry] : vitals) {
        bbcoop::VitalsBoard::Member* out = board.add_members();
        out->set_member(member);
        *out->mutable_vitals() = entry.vitals;
        out->mutable_vitals()->clear_hits();
        std::uint32_t damage = 0;
        for (const auto& [striker, other] : vitals) {
            for (const bbcoop::ReviveHit& hit : other.vitals.hits()) {
                if (hit.target() == member && hit.down() == entry.vitals.down()) {
                    damage += hit.damage();
                }
            }
        }
        out->set_revive_damage(damage);
    }
    for (const Wipe& wipe : wipes) {
        *board.add_wipes() = wipe.wipe;
    }
    return board;
}

bool EnemyOwners::Place(const std::string& member, std::uint32_t map, std::int64_t now_ms,
                        bool host) {
    const std::uint32_t area = AreaOf(map);
    latest_ms = std::max(latest_ms, now_ms);
    if (area == 0) {
        return members.erase(member) > 0 && Settle();
    }
    Presence& presence = members[member];
    if (presence.area != area || presence.heard_ms == 0) {
        presence.area = area;
        presence.since_ms = now_ms;
    }
    presence.heard_ms = now_ms;
    presence.host = host;
    return Settle();
}

bool EnemyOwners::Expire(std::int64_t now_ms) {
    latest_ms = std::max(latest_ms, now_ms);
    if (std::erase_if(members, [now_ms](const auto& entry) {
            return now_ms - entry.second.heard_ms > SilentMs;
        }) == 0) {
        return false;
    }
    return Settle();
}

bool EnemyOwners::Forget(const std::string& member) {
    return members.erase(member) > 0 && Settle();
}

std::string EnemyOwners::OwnerOf(std::uint32_t map) const {
    const auto owner = owners.find(AreaOf(map));
    return owner != owners.end() ? owner->second.member : std::string{};
}

bbcoop::EnemyOwners EnemyOwners::Message() const {
    bbcoop::EnemyOwners message;
    for (const auto& [area, owner] : owners) {
        bbcoop::EnemyOwner* out = message.add_owners();
        out->set_map(area);
        out->set_member(owner.member);
        out->set_epoch(owner.epoch);
    }
    return message;
}

bool EnemyOwners::Settle() {
    bool changed =
        std::erase_if(owners, [this](const auto& entry) {
            const auto member = members.find(entry.second.member);
            if (member == members.end() || member->second.area != entry.first) {
                return true;
            }
            // The session's host takes the area back, once it has followed its owner a while.
            return !member->second.host && std::ranges::any_of(members, [&](const auto& other) {
                return other.second.host && other.second.area == entry.first &&
                       latest_ms - other.second.since_ms >= HostTakeBackMs;
            });
        }) > 0;
    for (const auto& [name, presence] : members) {
        auto owner = owners.find(presence.area);
        if (owner == owners.end()) {
            // The member there longest (the map's order breaks a tie).
            const Presence* first = &presence;
            const std::string* first_name = &name;
            for (const auto& [other, there] : members) {
                if (there.area == presence.area && first->host) {
                    break;
                }
                if (there.area == presence.area &&
                    (there.host || there.since_ms < first->since_ms)) {
                    first = &there;
                    first_name = &other;
                }
            }
            owners[presence.area] = Owner{*first_name, ++epoch};
            changed = true;
        }
    }
    return changed;
}

} // namespace BloodborneWorld
