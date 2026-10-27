// SPDX-License-Identifier: GPL-2.0-or-later
// Native 1.09 effect/VFX mapping: Paramdex contributors and vladtepes22/NightwatchCoop.
// Adapted to Kaniel's explicit hostile-body path. No game parameter data is distributed.
#pragma once
#include <array>
#include <cstdint>
#include <cstring>
#include <span>
namespace BbInvasionVisual {
inline constexpr int Effect=5700,SourceEffect=9026;
inline constexpr size_t RowSize=0x210;
// The normal invasion guest's VFX, in an unused row so the native cooperator controller
// cannot remove/re-add it every frame. Preserve the existing full-HP behavior.
inline bool Prepare(std::span<uint8_t> target,std::span<const uint8_t> blank,std::span<const uint8_t> source) {
    if(target.size()!=RowSize || blank.size()!=RowSize || source.size()!=RowSize) return false;
    uint16_t state;float hp,duration;
    std::memcpy(&state,source.data()+0x156,2);std::memcpy(&hp,source.data()+0x10,4);std::memcpy(&duration,source.data()+8,4);
    if(state!=276 || duration!=-1.f || (hp!=.7f && hp!=1.f)) return false;
    std::array<uint8_t,RowSize> visual;
    std::memcpy(visual.data(),source.data(),RowSize);hp=1.f;std::memcpy(visual.data()+0x10,&hp,4);
    if(std::memcmp(target.data(),blank.data(),RowSize) && std::memcmp(target.data(),visual.data(),RowSize)) return false;
    std::memcpy(target.data(),visual.data(),RowSize);return true;
}
}
