// SPDX-License-Identifier: GPL-2.0-or-later
// bbport co-op: the co-op screens' own art the overlay embeds (gpu/art/coop_art.bin) decodes to every
// piece, each inside the atlas with a drawn middle and a clear ground where it should have one; the
// card's parts lie on its frame; a damaged atlas decodes to nothing.
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <vector>

#include "gpu/shim/bbport_coop_art.h"

using namespace BbCoopArt;

namespace {

std::vector<std::uint8_t> ReadFile(const char* path) {
    std::ifstream file(path, std::ios::binary);
    return {std::istreambuf_iterator<char>{file}, {}};
}

std::uint8_t AlphaAt(const Atlas& atlas, Piece piece, float sx, float sy) {
    const Rect& r = atlas.rects[std::size_t(piece)];
    const int x = r.x + int(sx * float(r.width - 1)), y = r.y + int(sy * float(r.height - 1));
    return atlas.rgba[(std::size_t(y) * std::size_t(atlas.width) + std::size_t(x)) * 4 + 3];
}

} // namespace

int main() {
    const std::vector<std::uint8_t> file = ReadFile(BB_COOP_ART_PATH);
    assert(!file.empty());
    const auto atlas = Decode(file.data(), file.size());
    assert(atlas);
    assert(atlas->rects.size() == std::size_t(Piece::Count));
    // The round pieces: clear corners. The medallions: a clear middle where the sigil goes, a drawn
    // ring. The sigils and marks: a drawn middle.
    for (const Piece piece : {Piece::Sigil0, Piece::Sigil1, Piece::Sigil2, Piece::Sigil3, Piece::Medallion,
                              Piece::MedallionReady, Piece::MarkReady, Piece::MarkLeader, Piece::MarkInvite}) {
        assert(AlphaAt(*atlas, piece, 0.01f, 0.01f) == 0);
        assert(AlphaAt(*atlas, piece, 0.99f, 0.99f) == 0);
    }
    for (const Piece piece : {Piece::Medallion, Piece::MedallionReady}) {
        assert(AlphaAt(*atlas, piece, 0.5f, 0.5f) == 0);
    }
    // The two medallions' rings are drawn at one scale (the ready one has a ring more round it).
    const Rect& plain = atlas->rects[std::size_t(Piece::Medallion)];
    const Rect& ready = atlas->rects[std::size_t(Piece::MedallionReady)];
    assert(ready.width > plain.width && ready.width < plain.width * 5 / 4);
    // The card: opaque inside and on its frame; its corner ornament, arch and flourish are drawn.
    assert(AlphaAt(*atlas, Piece::Card, 0.5f, 0.5f) == 255);
    assert(AlphaAt(*atlas, Piece::Card, 0.003f, 0.5f) > 200);
    assert(AlphaAt(*atlas, Piece::Card, 0.5f, 0.002f) > 200);
    for (const Share& part : {CardParts::Top, CardParts::Bottom, CardParts::Left, CardParts::Right,
                              CardParts::Inside, CardParts::Arch, CardParts::Flourish}) {
        assert(part.x0 >= 0.0f && part.x1 <= 1.0f && part.x0 < part.x1);
        assert(part.y0 >= 0.0f && part.y1 <= 1.0f && part.y0 < part.y1);
    }
    // The backdrop: a picture, opaque everywhere.
    assert(AlphaAt(*atlas, Piece::Backdrop, 0.0f, 0.0f) == 255 && AlphaAt(*atlas, Piece::Backdrop, 1.0f, 1.0f) == 255);
    // Kaniel 2026-10-18: the team screens in the souls counter's grey. The gold turns that grey (as
    // bright as it was, no hue), the wax seal's red and the dark inside stay.
    {
        Atlas tiny{.width = 4, .height = 1, .rects = {}, .rgba = {
            200, 176, 120, 255, // gold
            138, 28, 28, 255,   // the seal's red
            26, 29, 36, 255,    // the card's inside
            100, 88, 60, 128}}; // dim gold, half covered
        Greyed(tiny, SoulsGrey);
        const auto px = [&](int i) { return &tiny.rgba[std::size_t(i) * 4]; };
        assert(px(0)[0] <= px(0)[1] && px(0)[1] <= px(0)[2] && px(0)[2] - px(0)[0] <= 5); // grey
        assert(std::abs(int(px(0)[0]) - int(SoulsGrey[0])) <= 6 && px(0)[3] == 255);
        assert(px(1)[0] == 138 && px(1)[1] == 28 && px(1)[2] == 28);
        assert(px(2)[0] == 26 && px(2)[1] == 29 && px(2)[2] == 36);
        assert(px(3)[0] < px(0)[0] && px(3)[2] - px(3)[0] <= 5 && px(3)[3] == 128);
    }
    // Damaged: nothing.
    std::vector<std::uint8_t> damaged = file;
    damaged[0] = 'X';
    assert(!Decode(damaged.data(), damaged.size()));
    damaged = file;
    damaged[8] = 3; // another piece count
    assert(!Decode(damaged.data(), damaged.size()));
    damaged = file;
    damaged[10] = 0xFF; // a piece outside the atlas
    damaged[11] = 0xFF;
    assert(!Decode(damaged.data(), damaged.size()));
    assert(!Decode(file.data(), 20));
    assert(!Decode(nullptr, 0));
    std::printf("Co-op art: PASS (%d pieces in a %dx%d atlas)\n", int(atlas->rects.size()), atlas->width,
                atlas->height);
    return 0;
}
