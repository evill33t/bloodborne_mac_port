// SPDX-License-Identifier: GPL-2.0-or-later
// The team screen's Hunters online: who is listed, and who can be invited (coop.cpp, the overlay).
#pragma once

#include <string_view>

namespace LobbyRows {

// Kaniel 2026-11-01: this player's teammates are on the cards, not in the list: inviting someone
// took them off it. Anyone else shows, with their team.
inline bool ListedOnline(std::string_view team, std::string_view own_team) {
    return own_team.empty() || team != own_team;
}

// Only a hunter in no team can be invited.
inline bool MayInvite(bool can_invite, bool in_my_team, std::string_view team_leader) {
    return can_invite && !in_my_team && team_leader.empty();
}

} // namespace LobbyRows
