// SPDX-License-Identifier: GPL-2.0-or-later
// Kaniel / lKaniel, Map::WorldOf and AtlasOffset, bbport_overlay.cpp at 5d2305e.
#pragma once
#include <cstdint>
namespace BbMapView {
inline int WorldOf(uint32_t map) {
    switch (map >> 24) {
    case 0x16:
    case 0x17:
    case 0x18:
    case 0x19:
    case 0x1b:
    case 0x1c:
    case 0x20:
        return 1;
    case 0x1a:
        return 2;
    case 0x21:
    case 0x22:
    case 0x23:
    case 0x24:
        return 3;
    default:
        return 100 + int(map >> 16);
    }
}

/// Kaniel 2026-10-19 ("every map should be just one picture"): where each world lies on the one
/// picture, in metres added to its own coordinates. The worlds share no coordinates with each
/// other (each is a dream of its own), so they are laid side by side with 150 m between them:
/// the waking world (x -575 to 495, z -492 to 782) in the middle, the Nightmare Frontier and the
/// Hunter's Nightmare (x -770 to 221, z -997 to 47) east of it, the Nightmare of Mensis (x -215
/// to 223, z -122 to 176) north of those, the Hunter's Dream and the Abandoned Old Workshop west.
struct Place {
    float dx = 0.0f, dz = 0.0f;
};
inline Place AtlasOffset(uint32_t map) {
    switch (WorldOf(map)) {
    case 1:
        return {};
    case 3:
        return {1414.0f, 620.0f};
    case 2:
        return {859.0f, 939.0f};
    default:
        break;
    }
    switch (map >> 16) {
    case 0x1500: // the Hunter's Dream
        return {-793.0f, 145.0f};
    case 0x1501: // the Abandoned Old Workshop
        return {-891.0f, -165.0f};
    default: // anything else (a chalice's): south-west of the waking world
        return {-900.0f, -800.0f};
    }
}

}
