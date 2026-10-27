// SPDX-FileCopyrightText: Copyright 2026 shadNet Project
// SPDX-License-Identifier: GPL-2.0-or-later
// The IPv4 address a player reached the backend at, read from the request's Host header. ss.info
// sends each game back to that address: without a VPN the host's own address is its LAN one,
// which guests over the internet can't reach (2026-10-21). backend/bb_dummy_server.py
// address_from_host reads it the same way.
#pragma once

#include <string>
#include <string_view>

namespace Bloodborne::Backend {

// "203.0.113.7:20443" -> "203.0.113.7"; "" for anything else (no header, a name, IPv6, junk).
inline std::string AddressFromHostHeader(std::string_view value) {
    while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) {
        value.remove_prefix(1);
    }
    while (!value.empty() && (value.back() == ' ' || value.back() == '\t')) {
        value.remove_suffix(1);
    }
    const std::size_t colon = value.find(':');
    std::string_view host = value.substr(0, colon);
    if (colon != std::string_view::npos) {
        const std::string_view port = value.substr(colon + 1);
        if (port.empty() || port.size() > 5) {
            return {};
        }
        for (const char c : port) {
            if (c < '0' || c > '9') {
                return {};
            }
        }
    }
    int parts = 0;
    int digits = 0;
    int number = 0;
    for (const char c : host) {
        if (c == '.') {
            if (digits == 0 || ++parts > 3) {
                return {};
            }
            digits = number = 0;
        } else if (c >= '0' && c <= '9') {
            number = number * 10 + (c - '0');
            if (++digits > 3 || number > 255) {
                return {};
            }
        } else {
            return {};
        }
    }
    if (parts != 3 || digits == 0) {
        return {};
    }
    return std::string(host);
}

} // namespace Bloodborne::Backend
