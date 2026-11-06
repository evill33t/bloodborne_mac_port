// SPDX-FileCopyrightText: Copyright 2026 CLS_AMG
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <string>
#include <vector>

namespace CoopDiscord {

// Amanises 2026-10-25: a Discord friend who pressed Ask to Join on this player's status.
struct DiscordAsk {
    std::string user_id;
    std::string name;
};
// The requests waiting for an answer (the oldest first).
std::vector<DiscordAsk> DiscordJoinRequests();
// Accept sends the friend Discord's invite (its game joins with the team's secret); no closes it.
void AnswerDiscordJoinRequest(const std::string& user_id, bool accept);

// Starts the gameplay-aware Discord status: only with BB_DISCORD_STATUS=1 and BB_DISCORD_APP_ID,
// which the launcher sets when the player turned it on (off by default).
void StartDiscordStatus();

} // namespace CoopDiscord
