// SPDX-FileCopyrightText: Copyright 2026 shadNet Project
// SPDX-License-Identifier: GPL-2.0-or-later

// A team's code (TeamRequest.JOIN_CODE): what a leader reads out to a friend the lobby does not
// list. Both sides use these: the server makes and checks codes, the game cleans what was typed.
#pragma once

#include <string>
#include <string_view>

namespace BloodborneCoop {

// No 0/O, 1/I: read out loud or off a screen they get mixed up. 32 letters, 6 of them: 2^30
// codes, against a few wrong tries a minute (TeamLobby::LookupTries).
constexpr std::string_view TeamCodeAlphabet = "23456789ABCDEFGHJKLMNPQRSTUVWXYZ";
constexpr size_t TeamCodeLength = 6;

// What a player typed as a code: upper case, without spaces and dashes; empty when it cannot be
// one (another length, a letter not in the alphabet).
inline std::string NormalizeTeamCode(std::string_view typed) {
    std::string code;
    for (const char c : typed) {
        if (c == ' ' || c == '-' || c == '\t') {
            continue;
        }
        const char upper = c >= 'a' && c <= 'z' ? static_cast<char>(c - 'a' + 'A') : c;
        if (TeamCodeAlphabet.find(upper) == std::string_view::npos ||
            code.size() == TeamCodeLength) {
            return {};
        }
        code += upper;
    }
    return code.size() == TeamCodeLength ? code : std::string{};
}

// A code as a screen shows it: two halves of three ("ABC 234"), easier to read out.
inline std::string FormatTeamCode(std::string_view code) {
    if (code.size() != TeamCodeLength) {
        return std::string{code};
    }
    return std::string{code.substr(0, 3)} + " " + std::string{code.substr(3)};
}

} // namespace BloodborneCoop
