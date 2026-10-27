// SPDX-License-Identifier: GPL-2.0-or-later
// bbport co-op: where party death's HUD goes on screen (bbport_overlay.cpp Party::), apart from
// ImGui so a test can hold it clear of the game's own HUD. Places follow the game's frame inside
// the window (fitted whole and centred, as the presenter blits it), because the game's HUD moves
// with that frame when the window has another shape. `scale` is the overlay's (1 at 1080p).

#pragma once

#include <algorithm>
#include <cmath>

namespace BbPartyLayout {

/// The game's frame in display pixels.
struct Rect {
    float x, y, w, h;
};

/// The overlay's scale for a display this tall: 1 at 1080p, never under 0.75.
inline float Scale(float display_h) {
    return std::max(display_h / 1080.0f, 0.75f);
}

/// The team screen at scale 1 (bbport_overlay.cpp Team::Draw): its window and the margin beside it,
/// the inset of its contents, its button row (five buttons over the game) and its four cards.
inline constexpr float TeamWindowWidth = 1600.0f, TeamWindowMargin = 80.0f, TeamWindowInset = 24.0f;
inline constexpr float TeamButtonWidth = 260.0f, TeamCardWidth = 290.0f, TeamGap = 24.0f;
inline constexpr float TeamButtonsWidth = 5 * TeamButtonWidth + 4 * TeamGap;
inline constexpr float TeamCardsWidth = 4 * TeamCardWidth + 3 * TeamGap;

/// Kaniel 2026-10-26: in a narrow window (4:3 960x720, 16:10 1152x720) the overlay's scale stops at
/// 0.75 and the team screen ran off both sides: while it is open the overlay scales down to the width.
inline float TeamScreenScale(float scale, float display_w) {
    return std::min(scale, display_w / (TeamWindowWidth + TeamWindowMargin));
}

/// Kaniel 2026-10-24: the game's 16:9 screen inside a frame of another shape (21:9, 32:9, 16:10, 4:3):
/// its middle, as high as a wider frame and as wide as a taller one; the frame itself at 16:9. The
/// game's UI coordinates (its 1920x1080 stage) map onto it, the same scale both ways.
inline Rect Stage(const Rect& frame) {
    if (frame.w * 9.0f > frame.h * 16.0f + 0.5f) {
        const float w = frame.h * 16.0f / 9.0f;
        return {frame.x + (frame.w - w) * 0.5f, frame.y, w, frame.h};
    }
    if (frame.w * 9.0f + 0.5f < frame.h * 16.0f) {
        const float h = frame.w * 9.0f / 16.0f;
        return {frame.x, frame.y + (frame.h - h) * 0.5f, frame.w, h};
    }
    return frame;
}

/// How far the game's HUD groups are moved toward the frame's edges on another shape than 16:9, in
/// the stage's pixels (BbGuestHooks::HudEdgeShift): the player's own bars and items (PlayerHUD) go
/// left by dx and up by dy.
struct Shift {
    float dx = 0.0f, dy = 0.0f;
};

/// A point of the player's HUD on screen: (x, y) on the 1920x1080 stage, moved with PlayerHUD.
inline float HudX(const Rect& frame, float x, const Shift& shift) {
    const Rect stage = Stage(frame);
    return stage.x + (x - shift.dx) * stage.h / 1080.0f;
}
inline float HudY(const Rect& frame, float y, const Shift& shift) {
    const Rect stage = Stage(frame);
    return stage.y + (y - shift.dy) * stage.h / 1080.0f;
}

/// The game's own HUD, as fractions of its frame's height (measured on the rig's 1080p pictures):
/// the top-left quick items, their counts and the small round marker under them end about here...
inline constexpr float GameQuickItemsBottom = 210.0f / 1080.0f;
/// ...and the frame of the game's notices in the middle ("Entered the world of ...") starts about
/// here; their text ends about at the bottom line.
inline constexpr float GameNoticeTop = 700.0f / 1080.0f;
inline constexpr float GameNoticeBottom = 780.0f / 1080.0f;

/// The party list: its top left corner, beside the player's bars (with them at the frame's edges).
inline float ListLeft(const Rect& game, float scale, const Shift& shift = {}) {
    return HudX(game, 0.0f, shift) + 48.0f * scale;
}
inline float ListTop(const Rect& game, const Shift& shift = {}) {
    return std::round(HudY(game, 0.0f, shift) + Stage(game).h * 0.225f);
}

/// The downed player's own screen, from its revive ring's centre down to the give-up bar: the
/// ring's radius, then each line's offset below the centre (times `scale`).
inline constexpr float DownRingRadius = 40.0f;
inline constexpr float DownTitleBelow = 54.0f;
inline constexpr float DownHintBelow = 94.0f;
inline constexpr float DownGiveUpBelow = 128.0f;
inline constexpr float DownBarBelow = 152.0f;
inline constexpr float DownBarHeight = 4.0f;

/// The ring's centre, so that the give-up bar ends well above the game's notices.
inline float DownRingY(const Rect& game, float scale) {
    const Rect stage = Stage(game);
    return std::round(stage.y + stage.h * 0.64f - (DownBarBelow + DownBarHeight) * scale);
}

/// The party HUD's prompt (CoopPrompt, hud_patch.py PROMPT_AT): where the game's own prompt has its
/// button on the HUD's 1920x1080 stage (Button_0, 28 x 28 at -276.45, -11.5; its words start at
/// -235.75). The glyph ends at the button's right edge and grows to the left when wider (a key, R1),
/// so the words keep the game's spacing from it; its middle is the button's.
inline constexpr float PromptGlyphStageRight = 952.6f - 276.45f + 28.0f;
inline constexpr float PromptGlyphStageY = 808.0f - 11.5f + 14.0f;
inline constexpr float PromptGlyphStageSize = 28.0f;

/// The prompt to strike a downed member: low in the middle, as the game's own prompts.
inline float RevivePromptY(const Rect& game) {
    const Rect stage = Stage(game);
    return std::round(stage.y + stage.h * 0.74f);
}

} // namespace BbPartyLayout
