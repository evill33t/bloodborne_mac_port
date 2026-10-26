// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <string_view>

namespace Core::Bloodborne {

// The game's ss.info address comes from a table by region (0 Japan, 1 US, 2 EU, 3 Asia, 4 EU).
// The "Unlock Game Region" patch forces region 5, whose entry is empty, so the game asked for ""
// and failed to acquire the server information (a player's log, 2026-10-22). That empty address
// is taken as the EU one; the host overrides then send it to the co-op server like any other.
inline constexpr std::string_view RegionFiveUrl = "https://ss4.scej-network.jp:20443/bb-eu/ss.info";

inline std::string_view GameServerUrl(std::string_view url) {
    return url.empty() ? RegionFiveUrl : url;
}

} // namespace Core::Bloodborne
