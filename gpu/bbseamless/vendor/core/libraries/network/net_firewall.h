// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <charconv>
#include <optional>
#include <string_view>
#include <vector>

#include "common/types.h"

namespace Libraries::Net {

// Phase 9's proof test (docs/SERVER_COOP_PLAN.md): SHADPS4_NET_FIREWALL names the only addresses
// a game may reach, "a.b.c.d:port" or "a.b.c.d:*", comma-separated; the rig sets its lane's
// servers. Every datagram and connection of the game's sockets and of the P2P transport to
// anything else is blocked and logged ("Net firewall: blocked"), and a datagram from anything
// else is dropped. A firewall in the game, not Windows': Windows does not filter loopback, where
// the rig's games and servers all are.
class NetFirewall {
public:
    // Nothing when the setting is empty (no firewall). A setting that does not read blocks
    // everything, so a broken proof test never passes.
    static std::optional<NetFirewall> Parse(std::string_view setting) {
        const auto trimmed = Trim(setting);
        if (trimmed.empty()) {
            return std::nullopt;
        }
        NetFirewall firewall;
        std::string_view rest = trimmed;
        while (true) {
            const size_t comma = rest.find(',');
            const auto entry = ReadEntry(Trim(rest.substr(0, comma)));
            if (!entry) {
                firewall.allowed.clear();
                return firewall;
            }
            firewall.allowed.push_back(*entry);
            if (comma == std::string_view::npos) {
                return firewall;
            }
            rest = rest.substr(comma + 1);
        }
    }

    // `ipv4` and `port` in host byte order.
    bool Allows(u32 ipv4, u16 port) const {
        for (const Entry& entry : allowed) {
            if (entry.ipv4 == ipv4 && (entry.any_port || entry.port == port)) {
                return true;
            }
        }
        return false;
    }

private:
    struct Entry {
        u32 ipv4 = 0;
        u16 port = 0;
        bool any_port = false;
    };

    static std::string_view Trim(std::string_view text) {
        while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) {
            text.remove_prefix(1);
        }
        while (!text.empty() && (text.back() == ' ' || text.back() == '\t')) {
            text.remove_suffix(1);
        }
        return text;
    }

    template <typename T>
    static bool ReadNumber(std::string_view text, T& value) {
        const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
        return !text.empty() && error == std::errc{} && end == text.data() + text.size();
    }

    static std::optional<Entry> ReadEntry(std::string_view text) {
        const size_t colon = text.rfind(':');
        if (colon == std::string_view::npos) {
            return std::nullopt;
        }
        Entry entry;
        std::string_view address = text.substr(0, colon);
        for (int part = 0; part < 4; ++part) {
            const size_t dot = part < 3 ? address.find('.') : address.size();
            u8 byte = 0;
            if (dot == std::string_view::npos || !ReadNumber(address.substr(0, dot), byte)) {
                return std::nullopt;
            }
            entry.ipv4 = (entry.ipv4 << 8) | byte;
            address = part < 3 ? address.substr(dot + 1) : std::string_view{};
        }
        const std::string_view port = text.substr(colon + 1);
        if (port == "*") {
            entry.any_port = true;
        } else if (!ReadNumber(port, entry.port)) {
            return std::nullopt;
        }
        return entry;
    }

    std::vector<Entry> allowed;
};

// The game's firewall from SHADPS4_NET_FIREWALL (read once): whether it lets the game reach (or
// hear from) `ipv4`:`port` (network byte order). A block is logged, the first ones of each kind
// with the address. `what` says what was blocked ("a datagram to", "a connection to", ...).
bool FirewallAllows(u32 ipv4_nbo, u16 port_nbo, std::string_view what);

} // namespace Libraries::Net
