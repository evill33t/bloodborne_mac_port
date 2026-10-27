// SPDX-License-Identifier: GPL-2.0-or-later
// Adapted from the pinned reference bbport/src/runtime_pad.c (MapHold).
#pragma once
#include <cstdint>
namespace BbSeamless {
struct TouchHold {
    uint64_t since{};bool fired{};int side{};
    uint64_t tap_until{};int tap_side{};
    uint64_t lone_until{};int lone_side{};
    // 1: held map; 2: double-tap ping. A lone tap reaches the original gestures menu.
    int Step(bool down,int right,uint64_t now,int& click) {
        int action=0;
        if(lone_until && now>=lone_until) {
            tap_until=lone_until+120000;tap_side=lone_side;lone_until=0;
        }
        if(down) {
            if(!since) {
                since=now?now:1;fired=false;side=right;
                if(lone_until) {lone_until=0;fired=true;action=2;}
            }
            if(!fired && now-since>=450000) {fired=true;action=1;}
        } else if(since) {
            if(!fired) {lone_until=now+250000;lone_side=side;}
            since=0;
        }
        click=!down && now<tap_until?tap_side:-1;
        return action;
    }
};
}
