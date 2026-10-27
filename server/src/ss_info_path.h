// SPDX-FileCopyrightText: Copyright 2026 shadNet Project
// SPDX-License-Identifier: GPL-2.0-or-later
// The game asks its region's ss.info: /bb-eu/ss.info from an EU game, /bb-us/ss.info from a US
// one (2026-10-22: the US path got 404, and the host's game failed to acquire the server
// information). Every region gets the same document. backend/bb_dummy_server.py
// is_ss_info_path reads it the same way.
#pragma once

#include <string_view>

namespace Bloodborne::Backend {

// "/bb-<2 to 8 lowercase letters>/ss.info".
inline bool IsSsInfoPath(std::string_view path) {
    constexpr std::string_view prefix = "/bb-";
    constexpr std::string_view suffix = "/ss.info";
    if (!path.starts_with(prefix) || !path.ends_with(suffix) ||
        path.size() < prefix.size() + suffix.size()) {
        return false;
    }
    const std::string_view region =
        path.substr(prefix.size(), path.size() - prefix.size() - suffix.size());
    if (region.size() < 2 || region.size() > 8) {
        return false;
    }
    for (const char c : region) {
        if (c < 'a' || c > 'z') {
            return false;
        }
    }
    return true;
}

} // namespace Bloodborne::Backend
