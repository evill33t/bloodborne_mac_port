// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <algorithm>
#include <map>
#include <optional>
#include <vector>

#include "common/types.h"

namespace Libraries::Net {

// A peer of the P2P transport (its address and port) going quiet or talking again.
struct SilenceChange {
    u64 peer{};
    bool silent{};
    s64 gap_ms{};  // how long nothing came from it
    u64 packets{}; // what came from it before
};

// Which peers stopped sending (Kaniel's friend 2026-10-11: the other players froze on her screen
// before her game left the session, and nothing said whether their packets still arrived). Each
// change is reported once; a peer quiet for a minute is forgotten (it left).
class PeerSilence {
public:
    explicit PeerSilence(s64 quiet_ms) : quiet_ms{quiet_ms} {}

    // A packet from `peer`: the peer talking again after it was reported quiet, or nothing.
    std::optional<SilenceChange> Heard(u64 peer, s64 now_ms) {
        Peer& seen = peers[peer];
        std::optional<SilenceChange> change;
        if (seen.reported) {
            change = SilenceChange{peer, false, now_ms - seen.last_ms, seen.packets};
            seen.reported = false;
        }
        if (seen.packets > 0) {
            const s64 interval = now_ms - seen.last_ms;
            seen.pace_ms = seen.pace_ms == 0 ? interval : (seen.pace_ms * 7 + interval) / 8;
        }
        seen.last_ms = now_ms;
        ++seen.packets;
        return change;
    }

    // Peers newly quiet for longer than quiet_ms.
    std::vector<SilenceChange> Check(s64 now_ms) {
        std::vector<SilenceChange> changes;
        for (auto it = peers.begin(); it != peers.end();) {
            Peer& seen = it->second;
            const s64 gap = now_ms - seen.last_ms;
            if (gap > ForgetMs) {
                it = peers.erase(it);
                continue;
            }
            // Against the peer's own pace: a slow one (the matching server's heartbeat, every
            // 3-4 s) is quiet only once it misses a few beats.
            if (!seen.reported && seen.packets > 1 && gap > std::max(quiet_ms, 3 * seen.pace_ms)) {
                seen.reported = true;
                changes.push_back({it->first, true, gap, seen.packets});
            }
            ++it;
        }
        return changes;
    }

private:
    static constexpr s64 ForgetMs = 60000;
    struct Peer {
        s64 last_ms{};
        s64 pace_ms{}; // its usual time between packets
        u64 packets{};
        bool reported{};
    };
    s64 quiet_ms;
    std::map<u64, Peer> peers;
};

} // namespace Libraries::Net
