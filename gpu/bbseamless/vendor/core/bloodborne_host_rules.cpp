// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <array>
#include <cstring>

#include "core/bloodborne_host_rules.h"

namespace Core::Bloodborne {

namespace {

std::string_view Trimmed(std::string_view part) {
    while (!part.empty() && (part.front() == ' ' || part.front() == '\t')) {
        part.remove_prefix(1);
    }
    while (!part.empty() && (part.back() == ' ' || part.back() == '\t')) {
        part.remove_suffix(1);
    }
    return part;
}

bool IsRotKind(u32 kind) {
    return kind >= 1 && kind <= RotKinds;
}

} // namespace

HostRules ParseHostRules(std::string_view setting) {
    HostRules rules;
    while (!setting.empty()) {
        const std::size_t comma = setting.find(',');
        const std::string_view word = Trimmed(setting.substr(0, comma));
        if (word == "rot") {
            rules.rot = true;
        } else if (word == "travel") {
            rules.travel_together = true;
        } else if (word == "vote") {
            rules.travel_vote = true;
        } else if (word == "scale") {
            rules.enemy_scaling = true;
        }
        if (comma == std::string_view::npos) {
            break;
        }
        setting.remove_prefix(comma + 1);
    }
    rules.travel_vote = rules.travel_vote && rules.travel_together;
    return rules;
}

std::string HostRulesText(const HostRules& rules) {
    std::string text;
    const auto add = [&](bool on, std::string_view word) {
        if (on) {
            text += text.empty() ? "" : ",";
            text += word;
        }
    };
    add(rules.rot, "rot");
    add(rules.travel_together, "travel");
    add(rules.travel_vote && rules.travel_together, "vote");
    add(rules.enemy_scaling, "scale");
    return text.empty() ? "off" : text;
}

RotKind Rot::Die(u32 roll, u32 killer) {
    killer_ = killer;
    if (kinds_.size() >= MaxRot) {
        return RotKind::None;
    }
    const RotKind kind = static_cast<RotKind>(1 + roll % RotKinds);
    kinds_.push_back(kind);
    return kind;
}

bool Rot::Cure(RotCure) {
    killer_ = 0;
    if (kinds_.empty()) {
        return false;
    }
    kinds_.clear();
    return true;
}

bool Rot::Killed(u32 enemy) {
    if (enemy == 0 || enemy != killer_) {
        return false;
    }
    return Cure(RotCure::Revenge);
}

void Rot::Set(std::span<const u32> kinds) {
    kinds_.clear();
    for (const u32 kind : kinds) {
        if (IsRotKind(kind) && kinds_.size() < MaxRot) {
            kinds_.push_back(static_cast<RotKind>(kind));
        }
    }
}

std::vector<u32> Rot::Numbers() const {
    std::vector<u32> numbers;
    for (const RotKind kind : kinds_) {
        numbers.push_back(static_cast<u32>(kind));
    }
    return numbers;
}

RotEffect Rot::Effect() const {
    std::array<int, RotKinds + 1> count{};
    for (const RotKind kind : kinds_) {
        ++count[static_cast<std::size_t>(kind)];
    }
    RotEffect effect;
    effect.max_hp_rate = 1.0f - 0.10f * static_cast<float>(count[1]);
    effect.max_stamina_rate = 1.0f - 0.10f * static_cast<float>(count[2]);
    effect.stamina_use_rate = 1.0f + 0.10f * static_cast<float>(count[3]);
    effect.rally_rate = 1.0f - 0.15f * static_cast<float>(count[4]);
    effect.discovery = -10 * count[5];
    return effect;
}

std::string Rot::HudText() const {
    std::array<int, RotKinds + 1> count{};
    for (const RotKind kind : kinds_) {
        ++count[static_cast<std::size_t>(kind)];
    }
    static constexpr std::array<std::string_view, RotKinds + 1> words{
        "", "HP-", "Stam-", "Stam use+", "Rally-", "Disc-"};
    static constexpr std::array<int, RotKinds + 1> steps{0, 10, 10, 10, 15, 10};
    std::string text;
    for (std::size_t kind = 1; kind <= RotKinds; ++kind) {
        if (count[kind] == 0) {
            continue;
        }
        text += text.empty() ? "" : " ";
        text += words[kind];
        text += std::to_string(steps[kind] * count[kind]);
        text += kind == 5 ? "" : "%";
    }
    return text;
}

float RotRowValue(const RotEffect& effect, RotKind kind) {
    switch (kind) {
    case RotKind::MaxHp:
        return effect.max_hp_rate;
    case RotKind::MaxStamina:
        return effect.max_stamina_rate;
    case RotKind::StaminaUse:
        return effect.stamina_use_rate;
    case RotKind::Rally:
        return effect.rally_rate;
    case RotKind::Discovery:
        return static_cast<float>(effect.discovery);
    case RotKind::None:
        break;
    }
    return 0.0f;
}

bool WriteRotRow(std::span<u8> param, const RotRow& row, float value) {
    if (!IsParamOfType(param, "SP_EFFECT_PARAM_ST")) {
        return false;
    }
    const auto target = FindParamRow(param, row.row);
    const auto source = FindParamRow(param, RotTemplateRow);
    if (!target || !source || *target == *source || *target > param.size() ||
        param.size() - *target < SpEffectRowSize || *source > param.size() ||
        param.size() - *source < SpEffectRowSize) {
        return false;
    }
    // The row's data must be its own (a table may share one row's data among ids).
    u16 rows = 0;
    std::memcpy(&rows, param.data() + 0x0A, sizeof(rows));
    for (std::size_t index = 0; index < rows; ++index) {
        s32 id = 0;
        u64 data = 0;
        std::memcpy(&id, param.data() + 0x40 + index * 24, sizeof(id));
        std::memcpy(&data, param.data() + 0x40 + index * 24 + 8, sizeof(data));
        if (data == *target && id != row.row) {
            return false;
        }
    }
    u8* out = param.data() + *target;
    std::memcpy(out, param.data() + *source, SpEffectRowSize);
    const float one = 1.0f, none = 0.0f;
    std::memcpy(out + 0x10, &one, sizeof(one));    // the template's max HP
    std::memcpy(out + 0x1FC, &none, sizeof(none)); // and Item Discovery
    std::memcpy(out + row.field, &value, sizeof(value));
    return true;
}

bool OffersTeamTravel(const HostRules& rules, bool in_session, std::size_t others,
                      bool boss_fight) {
    return rules.travel_together && in_session && others > 0 && !boss_fight;
}

void TravelVote::Start(u32 id, std::span<const std::string> others, s64 now_ms) {
    id_ = id;
    started_ms_ = now_ms;
    active_ = true;
    stopped_ = false;
    waiting_.assign(others.begin(), others.end());
}

void TravelVote::Answer(u32 id, const std::string& member, bool yes) {
    if (!active_ || id != id_ || stopped_) {
        return;
    }
    const auto it = std::find(waiting_.begin(), waiting_.end(), member);
    if (it == waiting_.end()) {
        return;
    }
    waiting_.erase(it);
    stopped_ = !yes;
}

void TravelVote::Drop(const std::string& member) {
    std::erase(waiting_, member);
}

TravelVote::Result TravelVote::At(s64 now_ms) const {
    if (!active_) {
        return Result::None;
    }
    if (stopped_) {
        return Result::Stopped;
    }
    if (waiting_.empty() || now_ms - started_ms_ >= TravelVoteMs) {
        return Result::Go;
    }
    return Result::Waiting;
}

s64 TravelVote::LeftMs(s64 now_ms) const {
    return active_ ? std::max<s64>(0, TravelVoteMs - (now_ms - started_ms_)) : 0;
}

void TravelVote::Clear() {
    *this = TravelVote{};
}

std::optional<HarderEnemies> TeamScaledEnemies(std::size_t hunters) {
    if (hunters <= 1) {
        return std::nullopt;
    }
    const float step =
        static_cast<float>(std::min(hunters, TeamSlots) - 1) / static_cast<float>(TeamSlots - 1);
    const auto between = [&](float harder) { return 1.0f + (harder - 1.0f) * step; };
    return HarderEnemies{between(HarderDefaultHp), between(HarderDefaultDamage),
                         between(HarderDefaultPoise)};
}

std::size_t HuntersOnMap(std::span<const MemberMap> board, std::string_view self, u32 map) {
    return 1 +
           static_cast<std::size_t>(std::count_if(board.begin(), board.end(), [&](const auto& m) {
               return m.member != self && map != 0 && m.map == map;
           }));
}

} // namespace Core::Bloodborne
