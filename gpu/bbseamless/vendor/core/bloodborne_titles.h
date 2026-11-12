// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <algorithm>
#include <array>
#include <string_view>

namespace Core::Bloodborne {

// Every retail release of Bloodborne, as the 01.09 eboot's own content-id table lists them: Japan,
// US, EU and Asia, each with its Game of the Year edition. They share one 1.09 executable (the
// launcher checks its image, bbport/scripts/game_check.py SUPPORTED_IMAGE; the same list is its
// SUPPORTED_TITLES and bbport_coop.py's TITLE_IDS).
inline constexpr std::array<std::string_view, 9> RetailTitles{
    "CUSA03173", "CUSA00207", "CUSA00208", "CUSA00900", "CUSA03179",
    "CUSA01363", "CUSA03023", "CUSA00299", "CUSA03014",
};

// Whether a param.sfo TITLE_ID is one of them.
constexpr bool IsBloodborneTitle(std::string_view title) {
    return std::ranges::find(RetailTitles, title) != RetailTitles.end();
}

// The update the co-op code is written for (param.sfo APP_VER).
inline constexpr std::string_view CoopAppVersion = "01.09";

// What the game's param.sfo says about seamless co-op.
enum class CoopGame {
    Yes,          // Bloodborne at 01.09
    OtherVersion, // Bloodborne, but param.sfo names another version than 01.09
    OtherGame,    // not Bloodborne: no co-op
};

// Seamless co-op was for CUSA03173 alone: every other release (the US CUSA00900 above all) started
// without co-op's code under the party HUD the launcher had made for it, whose sprites then played
// through their frames ("test" names and 99 vials flashing, Create a team grey; Discord, 2026-10-25
// to 2026-11-07). The code is the same in every release, so each gets co-op.
constexpr CoopGame SeamlessCoopGameFor(std::string_view title, std::string_view app_version) {
    if (!IsBloodborneTitle(title)) {
        return CoopGame::OtherGame;
    }
    return app_version == CoopAppVersion ? CoopGame::Yes : CoopGame::OtherVersion;
}

// Another version in param.sfo does not refuse co-op: the launcher starts a game folder by its
// executable, whatever its sce_sys says (the 1.09 eboot.bin copied over a base game), and co-op's
// site check compares the code itself before anything is written (bloodborne_patch_sites.h), so
// an executable that is not 1.09 still gets none.
constexpr bool GetsSeamlessCoop(CoopGame game) {
    return game != CoopGame::OtherGame;
}

} // namespace Core::Bloodborne
