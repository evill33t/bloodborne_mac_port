// SPDX-License-Identifier: GPL-2.0-or-later
// bbport co-op: the game's own HUD art for the party panel, read at runtime from the player's game
// files (menu/common.tpf.dcx); nothing of it is shipped. A DCX (zlib) holds a PS4 TPF whose
// textures are BC7 or BC1 blocks in the PS4's tiled order; the gauge strips the game's HP bar is
// drawn with sit in one of them (GaugeTexture, GaugeFrame/Fill/Regain).
#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace BbGameArt {

// The game's HP bar parts in MENU_Common_00091 (1024x512, BC7), in its pixels: the dark framed
// track, the red HP fill and the orange regain strip, drawn inside the track.
inline constexpr std::string_view GaugeTexture = "MENU_Common_00091";
struct PixelRect {
    int x0, y0, x1, y1; // x1/y1 one past the last pixel
};
inline constexpr PixelRect GaugeFrame{4, 4, 652, 21};
inline constexpr PixelRect GaugeFill{10, 26, 648, 35};
inline constexpr PixelRect GaugeRegain{10, 39, 648, 48};
// Pixels at each end of the frame kept unstretched (its end caps).
inline constexpr int GaugeFrameCap = 8;

// A DCX's payload: "DCX\0" ... "DCA\0" <u32 size> then a zlib stream. Nothing if it is not one.
std::optional<std::vector<std::uint8_t>> InflateDcx(std::span<const std::uint8_t> file);

struct TpfTexture {
    std::string name;
    std::uint32_t dxgi = 0; // 98 BC7, 71 BC1
    int width = 0, height = 0;
    std::uint32_t offset = 0, size = 0;
};
// A PS4 TPF's textures (UTF-16 names, entries of 0x24 bytes); nothing if it is not one.
std::optional<std::vector<TpfTexture>> ParseTpf(std::span<const std::uint8_t> tpf);

// The PS4's block order: 8x8-block tiles in row order, Morton (x, y interleaved) inside a tile.
// Returns the blocks in plain row order.
std::vector<std::uint8_t> DeswizzleBlocks(std::span<const std::uint8_t> data, int width_blocks,
                                          int height_blocks, int block_bytes);

// One BC7 block to 4x4 RGBA pixels (row by row).
std::array<std::uint8_t, 64> DecodeBc7Block(std::span<const std::uint8_t, 16> block);

// The RGBA pixels of `rect` of a BC7 texture whose blocks are in plain row order.
std::vector<std::uint8_t> DecodeBc7Rect(std::span<const std::uint8_t> blocks, int width,
                                        const PixelRect& rect);

// The gauge art: GaugeTexture's top rows (0 to `rows`) as RGBA, `width` wide; nothing when the
// file is missing or not as expected.
struct GaugeArt {
    int width = 0, height = 0;
    std::vector<std::uint8_t> rgba;
};
std::optional<GaugeArt> LoadGaugeArt(std::span<const std::uint8_t> common_tpf_dcx);

// Kaniel 2026-10-14: the team screens drawn with the game's own menu art and font, read at
// runtime from the player's game files like the gauge (nothing of it is shipped).

// One BC1 block to 4x4 RGBA pixels (row by row): two RGB565 colours, 2-bit indices; the
// three-colour mode (first <= second) has transparent black at index 3.
std::array<std::uint8_t, 64> DecodeBc1Block(std::span<const std::uint8_t, 8> block);

struct Image {
    int width = 0, height = 0;
    std::vector<std::uint8_t> rgba;
};

// A .tpf.dcx, inflated once: pieces of its BC7 (98) and BC1 (71) textures as RGBA.
class TpfFile {
public:
    static std::optional<TpfFile> Read(std::span<const std::uint8_t> tpf_dcx);
    // `rect` of texture `name` (all of it when `rect` is empty); nothing when the texture is not
    // there, not BC7 or BC1, or `rect` is not inside it.
    std::optional<Image> Piece(std::string_view name, const PixelRect& rect = {}) const;

private:
    std::vector<std::uint8_t> tpf;
    std::vector<TpfTexture> textures;
};

// Kaniel 2026-10-19: every button our screens name is drawn as the game's own key guide icon, never
// written out ("L1", "Square"). The icons are 32x32 textures in menu/common.tpf.dcx (KG_*); the
// launcher's button icons (bbport_icons.py) redraw the same textures for Xbox pads and keyboards in
// the game folder the game runs from, so they always show the player's own buttons.
// The texture of runtime_pad.c's input `input` (its order: cross, circle, square, triangle, l1, r1,
// l2, r2, l3, r3, options, touchpad left and right, d-pad up, down, left, right, move x4, look x4);
// empty past them. KG_OK is the Circle and KG_Cancel the Cross (the Japanese layout's names).
std::string_view KeyGuideTexture(int input);
// A button as our texts name it ("Hold {Circle} to give up": scripts/menu_texts.py's BUTTONS) and
// the game's icon of it; nothing for another name.
std::string_view KeyGuideTextureNamed(std::string_view name);
// A text for the game's own menus (the rows we add to its System menu) with each {Button} as the
// game's inline icon, as its own texts write them: <img src="KG_R1" vspace='-8'>.
std::string GameMenuIcons(std::string_view text);
// The icons for both sticks and the d-pad as a whole (move, choose).
inline constexpr std::string_view KeyGuideLeftStick = "KG_LS";
inline constexpr std::string_view KeyGuideRightStick = "KG_RS";
inline constexpr std::string_view KeyGuideDpad = "KG_L_UDLR";
inline constexpr std::string_view KeyGuideDpadUpDown = "KG_L_UD";
inline constexpr std::string_view KeyGuideDpadLeftRight = "KG_L_LR";

// The game's menu font (menu/<language>/font.gfx: a Scaleform file whose DefineFont3 holds the
// glyph outlines, quadratic like TrueType's) as a TrueType font the overlay can load; nothing
// when the file is not one.
std::optional<std::vector<std::uint8_t>> TtfFromGfxFont(std::span<const std::uint8_t> gfx);

} // namespace BbGameArt
