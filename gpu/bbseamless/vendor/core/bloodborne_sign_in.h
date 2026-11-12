// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>

#include "common/types.h"

namespace Core::Bloodborne {

// Players' reports (Discord, 1.0.0 to 1.3.x, about ten of them): whenever the co-op server could
// not be reached (the host's server not started, a firewall, a wrong address) the game said "not
// signed in to PlayStation Network", its own text for a console without a sign-in, and sent
// people looking in the wrong place. The game is told nothing else (it stays signed out), so the
// reason is kept here for the screens: the game's dialog and the team screen say it instead.
enum class SignInProblem : int {
    None = 0,       // signed in, still signing in, or playing offline by choice
    NoAnswer = 1,   // nothing answered at the server's address
    WrongLogin = 2, // the server answered and refused the login or its password
    Other = 3,      // the server answered and the sign-in still failed
    Refused = 4,    // the server refused this game itself (another release): its own reason
};

struct SignInFailure {
    SignInProblem problem = SignInProblem::None;
    std::string server; // as the player set it: "26.1.2.3:31313"
};

// The server's address as the screens say it.
inline std::string SignInServerText(std::string_view host, u16 port) {
    if (host.empty()) {
        return {};
    }
    return port == 0 ? std::string{host} : std::string{host} + ":" + std::to_string(port);
}

// The last sign-in's failure, none once a sign-in worked.
class SignInNotes {
public:
    void Failed(SignInProblem problem, std::string_view host, u16 port) {
        std::lock_guard lock{mutex};
        failure = {problem, SignInServerText(host, port)};
    }
    void SignedIn() {
        std::lock_guard lock{mutex};
        failure = {};
    }
    SignInFailure Last() const {
        std::lock_guard lock{mutex};
        return failure;
    }

private:
    mutable std::mutex mutex;
    SignInFailure failure;
};

inline SignInNotes& SignInNotesOfThisGame() {
    static SignInNotes notes;
    return notes;
}

// The server refusing the account itself (a wrong password on a local server, a Global account
// not signed in in the launcher): a wrong login, not another release (the rig, 2026-11-10).
inline bool IsAccountRefusal(std::string_view link_refusal) {
    return link_refusal.starts_with("sign-in refused") || link_refusal.starts_with("account:");
}

// What the screens say of a failed sign-in: the server's own refusal of the link (the build check
// on join, the account) comes before the failure it caused (the sign-in through a link that
// refused never answers).
inline SignInProblem ShownSignInProblem(SignInProblem noted, std::string_view link_refusal) {
    if (noted == SignInProblem::None) {
        return SignInProblem::None;
    }
    if (link_refusal.empty()) {
        return noted;
    }
    return IsAccountRefusal(link_refusal) ? SignInProblem::WrongLogin : SignInProblem::Refused;
}

// The game's own texts for "not signed in to PlayStation Network" (MsgRepository category and
// id, eboot 1.09; every language's file has them at the same place): the ones the port's lookup
// hook answers with the co-op server's reason while a sign-in failure is noted.
inline constexpr std::array<std::pair<u32, u32>, 1> PsnSignInTexts{{
    // SP_Dialog: 'You are currently not signed into "PlayStation Network". Please sign in to use
    // the online features.'
    {204, 401203},
}};

inline bool IsPsnSignInText(u32 category, u32 id) {
    for (const auto& [text_category, text_id] : PsnSignInTexts) {
        if (text_category == category && text_id == id) {
            return true;
        }
    }
    return false;
}

} // namespace Core::Bloodborne
