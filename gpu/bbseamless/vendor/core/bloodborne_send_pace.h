// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <algorithm>
#include <chrono>
#include <unordered_map>

#include "common/types.h"

namespace Core::Bloodborne {

// The send pace of the opt-in unlocked frame rate online (SHADPS4_BLOODBORNE_SEND_PACE, run.py
// sets it with BB_ONLINE_UNLOCKED_FPS). The game counts frames between some of its packets: the
// player state every FrameSkipCount frames (5 in the eboot, 10 with 60 FPS++, 15 with 90 FPS++),
// a second ChrSync counter every 10 (20, 30) and the enemy HP round every 4. Uncap FPS++ leaves
// the first two at the PS4's 30 FPS values, so a host at 150 FPS sent its state five times as
// often as at 60, and the guests saw everyone freeze and fly with hits landing late (Kaniel
// 2026-10-13). The pace keeps each counter's frame count and holds a send back until the 60 FPS
// mode's interval has passed for that object; at 60 FPS and below it never holds one.

constexpr std::chrono::microseconds PlayerStatePaceInterval{166'667};   // 10 frames at 60
constexpr std::chrono::microseconds SecondCounterPaceInterval{333'333}; // 20 frames at 60
constexpr std::chrono::microseconds EnemyHpPaceInterval{66'667};
// The host's event sync (channel 0x31, 0x012EE540) goes every frame an event's condition holds:
// at most once a 60 FPS frame for each event, slot and condition group.
constexpr double EventSyncPerSecond = 60.0;

/// The event sync's pace key: the event id (+0x28), its slot (+0x2C) and the condition group
/// (+0x48; one event can send two groups a frame).
constexpr u64 EventSyncKey(u32 event, u16 slot, u32 group) {
    return (u64{event} << 32) | (u64{slot} << 16) | (group & 0xFFFF);
} // 4 frames at 60

class SendPace {
public:
    using Clock = std::chrono::steady_clock;

    /// Whether `object` may send now; a send it allows starts its next interval. A send up to a
    /// tenth of the interval early (a frame of 10 at 60 FPS) still goes: the frame count spaces
    /// them, and 60 FPS frame jitter must never cost a send a whole frame.
    bool Allow(u64 object, Clock::time_point now, std::chrono::microseconds interval) {
        Forget(now);
        const auto early = interval / 10;
        auto [it, added] = last_.try_emplace(object, now);
        if (added) {
            return true;
        }
        if (now - it->second < interval - early) {
            return false;
        }
        it->second = now;
        return true;
    }

    /// Whether `key` may send now at `per_second` on average, a frame or two of burst allowed:
    /// for a stream the game sends every frame (the event sync), where an interval would drop a
    /// slightly early 60 FPS frame.
    bool AllowRate(u64 key, Clock::time_point now, double per_second) {
        constexpr double Burst = 2.0;
        Forget(now);
        auto [it, added] = buckets_.try_emplace(key, Bucket{Burst - 1.0, now});
        if (added) {
            return true;
        }
        Bucket& bucket = it->second;
        const double seconds = std::chrono::duration<double>(now - bucket.last).count();
        bucket.tokens = std::min(Burst, bucket.tokens + seconds * per_second);
        bucket.last = now;
        if (bucket.tokens < 1.0) {
            return false;
        }
        bucket.tokens -= 1.0;
        return true;
    }

    std::size_t Size() const {
        return last_.size() + buckets_.size();
    }

private:
    struct Bucket {
        double tokens;
        Clock::time_point last;
    };

    // Objects not seen for 5 s are gone (a map left, a member out): forgotten once the table
    // holds more than a few hundred.
    void Forget(Clock::time_point now) {
        if (last_.size() + buckets_.size() < 512) {
            return;
        }
        std::erase_if(last_, [now](const auto& entry) {
            return now - entry.second > std::chrono::seconds{5};
        });
        std::erase_if(buckets_, [now](const auto& entry) {
            return now - entry.second.last > std::chrono::seconds{5};
        });
    }

    std::unordered_map<u64, Clock::time_point> last_;
    std::unordered_map<u64, Bucket> buckets_;
};

/// The player state counter (+0xC8) as the game should load it: the game sends when the loaded
/// value is 1 or less; held back, 2 (the count goes on to 1 and asks again next frame).
constexpr s32 HeldCountdown(s32 loaded, bool allowed) {
    return loaded <= 1 && !allowed ? 2 : loaded;
}

/// The second counter (+0xF8) after its decrement: 0 or less sends; held back, 1.
constexpr s32 HeldDecremented(s32 decremented, bool allowed) {
    return decremented <= 0 && !allowed ? 1 : decremented;
}

/// Whether the enemy HP round sends for the count it loads (`test al, 3`).
constexpr bool RoundSends(u32 loaded) {
    return (loaded & 3) == 0;
}

/// The enemy HP round's count as the game should load it: held back on a send frame, one less
/// (the game stores it plus one, the same count, and asks again next frame).
constexpr u32 HeldRound(u32 loaded, bool allowed) {
    return RoundSends(loaded) && !allowed ? loaded - 1 : loaded;
}

} // namespace Core::Bloodborne
