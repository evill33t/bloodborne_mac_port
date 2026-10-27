// SPDX-License-Identifier: GPL-2.0-or-later
// bbport co-op: the co-op screens' own art (Kaniel 2026-10-17, drawn for the port: gpu/art/coop/*.png),
// packed by scripts/coop_art.py into gpu/art/coop_art.bin, which the overlay embeds: the hunters' sigils
// and medallions, the cards, the dividers, the marks and the loading screen's backdrop.
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace BbCoopArt {

// The atlas's pieces, in coop_art.py's PIECES order.
enum class Piece {
    Sigil0,
    Sigil1,
    Sigil2,
    Sigil3,
    Medallion,
    MedallionReady,
    Card,
    Divider,
    TitleOrnament,
    MarkReady,
    MarkLeader,
    MarkInvite,
    Backdrop,
    Count
};

struct Rect {
    int x = 0, y = 0, width = 0, height = 0;
};

struct Atlas {
    int width = 0;
    int height = 0;
    std::vector<Rect> rects; // one per Piece
    std::vector<std::uint8_t> rgba;
};

// 'BBCA', width, height and piece count (little-endian u16 each), a piece's x, y, width and height
// (u16 each) for every piece, then the zlib-compressed RGBA. Nothing for anything else, another piece
// count or a piece outside the atlas.
std::optional<Atlas> Decode(const std::uint8_t* data, std::size_t size);

// Kaniel 2026-10-18: the team screens in the grey of the souls counter's numbers (sampled from
// the HUD at 4K: #C1C4C5) instead of gold.
constexpr std::uint8_t SoulsGrey[3] = {0xC1, 0xC4, 0xC5};
// The atlas with its gold made `grey`: each gold pixel (warm, yellow to orange) that grey at its own
// brightness; other colours (the wax seal's red, the dark inside) and coverage as they were.
void Greyed(Atlas& atlas, const std::uint8_t grey[3]);

// A part of a piece, in shares of its width and height.
struct Share {
    float x0, y0, x1, y1;
};

// The card's frame (measured on card.png): its corners' ornaments, a stretch of each edge without
// ornaments, a bit of its plain inside, the arch over its top and the flourish over its bottom. A card
// of any shape is drawn from them: the corners, arch and flourish at one scale, the rest stretched.
namespace CardParts {
constexpr float CornerX = 0.16f, CornerY = 0.115f;
constexpr Share Top{0.20f, 0.0f, 0.30f, CornerY};
constexpr Share Bottom{0.17f, 1.0f - CornerY, 0.23f, 1.0f};
constexpr Share Left{0.0f, 0.40f, CornerX, 0.60f};
constexpr Share Right{1.0f - CornerX, 0.40f, 1.0f, 0.60f};
constexpr Share Inside{0.40f, 0.40f, 0.60f, 0.60f};
constexpr Share Arch{0.355f, 0.0f, 0.645f, 0.195f};
constexpr Share Flourish{0.24f, 1.0f - CornerY, 0.77f, 1.0f};
} // namespace CardParts

} // namespace BbCoopArt
