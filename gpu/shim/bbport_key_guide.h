// SPDX-License-Identifier: GPL-2.0-or-later
// bbport co-op: the key guide at the bottom of our screens (bbport_overlay.cpp Ui::Keys), laid out
// apart from ImGui so a test can hold it inside its panel. Kaniel 2026-10-19: the guides ran into
// the panels' bottom ornament and off their sides; each entry is the game's button icons, then
// what they do, and a guide too wide for its panel gets a little smaller, then a second line.

#pragma once

#include <algorithm>
#include <span>
#include <vector>

namespace BbKeyGuide {

/// One entry's widths at full size, in screen pixels: its icons (with the gaps between them) and
/// its words.
struct Entry {
    float icons = 0.0f, words = 0.0f;
};

/// Where an entry goes: its line (0 the top one) and its left edge from the room's left.
struct Place {
    int line = 0;
    float x = 0.0f;
};

struct Layout {
    float scale = 1.0f; // what everything is drawn at
    int lines = 1;
    std::vector<Place> places; // one per entry
};

/// Never smaller than this: past it the guide takes a second line.
inline constexpr float MinScale = 0.85f;

/// From a panel's bottom edge, where its ornament sits, up to the bottom of its key guide at
/// 1080p: the ornament's scrollwork (about 26 px) and a clear gap over it.
inline constexpr float BottomClearance = 46.0f;

/// The entries in `room` pixels: one line at full size when it fits; else smaller (down to
/// MinScale); else lines at full size, as many entries on each as fit, each line centred.
/// `icon_gap` is between an entry's icons and its words, `entry_gap` between entries.
inline Layout Arrange(std::span<const Entry> entries, float icon_gap, float entry_gap, float room) {
    Layout layout;
    const auto width = [&](const Entry& e, float scale) {
        return (e.icons + (e.words > 0.0f ? icon_gap : 0.0f) + e.words) * scale;
    };
    float total = 0.0f;
    for (const Entry& e : entries) {
        total += width(e, 1.0f);
    }
    total += entry_gap * float(entries.empty() ? 0 : entries.size() - 1);
    if (total > room && total * MinScale <= room) {
        layout.scale = room / total;
    }
    // Lines: entries in order while the line fits (one entry at least on each).
    std::vector<std::pair<std::size_t, std::size_t>> spans; // [first, last) per line
    std::vector<float> widths;
    std::size_t first = 0;
    float line = 0.0f;
    for (std::size_t i = 0; i < entries.size(); ++i) {
        const float w = width(entries[i], layout.scale);
        const float grown = i == first ? w : line + entry_gap * layout.scale + w;
        if (i != first && grown > room) {
            spans.emplace_back(first, i);
            widths.push_back(line);
            first = i;
            line = w;
        } else {
            line = grown;
        }
    }
    if (!entries.empty()) {
        spans.emplace_back(first, entries.size());
        widths.push_back(line);
    }
    layout.lines = std::max<int>(1, int(spans.size()));
    layout.places.resize(entries.size());
    for (std::size_t l = 0; l < spans.size(); ++l) {
        float x = std::max(0.0f, (room - widths[l]) * 0.5f);
        for (std::size_t i = spans[l].first; i < spans[l].second; ++i) {
            layout.places[i] = Place{int(l), x};
            x += width(entries[i], layout.scale) + entry_gap * layout.scale;
        }
    }
    return layout;
}

/// Kaniel 2026-10-25: the co-op hint on the left of the screen (how to open the co-op menu, and that
/// duels are in it): fully shown for CoopHintShownS once the player is in its team's hunt, then
/// fading out over CoopHintFadeS. Its alpha `seconds` after it came.
inline constexpr double CoopHintShownS = 20.0, CoopHintFadeS = 2.0;
inline float CoopHintAlpha(double seconds) {
    if (seconds < 0.0 || seconds >= CoopHintShownS + CoopHintFadeS) {
        return 0.0f;
    }
    return seconds < CoopHintShownS ? 1.0f : float(1.0 - (seconds - CoopHintShownS) / CoopHintFadeS);
}

} // namespace BbKeyGuide
