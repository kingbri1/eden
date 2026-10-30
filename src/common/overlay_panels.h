// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <cmath>
#include <cstddef>
#include <mutex>
#include <vector>

#include "common/common_types.h"

namespace Common {

/// The screen corner a panel is placed against
enum class OverlayAnchor {
    TopLeft,
    BottomLeft,
    TopRight,
};

/// An image drawn over the game, inside the presented frame itself so it is visible inside
/// gamescope (Steam's Game Mode): a Qt widget can never composite over the game window. The
/// frontend fills it (RGBA8, via QPainter) and the presenter blends it in last.
struct OverlayPanel {
    std::mutex mutex;
    std::vector<u8> pixels; ///< RGBA8, width * height * 4
    u32 width = 0;
    u32 height = 0;
    bool enabled = false;
    /// Incremented on every push so presenters can cache by version
    u64 version = 0;
    OverlayAnchor anchor = OverlayAnchor::TopLeft;
    /// A top-anchored panel sits this share of the screen's height lower (clear of other
    /// overlays, such as a performance overlay, in the corner)
    float drop = 0.0f;
};

/// The panels, in drawing order
enum class OverlayPanelId : std::size_t {
    Players, ///< The controller players (top left)
    Hints,   ///< The controller hints (bottom left)
    Cheats,  ///< The cheats (top right)
    Count,
};

inline constexpr std::size_t OverlayPanelCount = static_cast<std::size_t>(OverlayPanelId::Count);

[[nodiscard]] inline std::array<OverlayPanel, OverlayPanelCount>& GetOverlayPanels() {
    static std::array<OverlayPanel, OverlayPanelCount> panels;
    static const bool placed = [] {
        panels[static_cast<std::size_t>(OverlayPanelId::Hints)].anchor =
            OverlayAnchor::BottomLeft;
        panels[static_cast<std::size_t>(OverlayPanelId::Cheats)].anchor =
            OverlayAnchor::TopRight;
        return true;
    }();
    (void)placed;
    return panels;
}

[[nodiscard]] inline OverlayPanel& GetOverlayPanel(OverlayPanelId id) {
    return GetOverlayPanels()[static_cast<std::size_t>(id)];
}

/// A panel's place, in the pixels of the screen area the game is drawn in
struct OverlayRect {
    float x;
    float y;
    float width;
    float height;
};

/// Where a panel goes on a screen area (left, top, right, bottom): the one placement both the
/// presenter and the frontend's mouse handling use. Whole pixels: between two, the image's
/// edge would be filtered with what lies outside it (a dark line). Read with the panel's mutex
/// held.
[[nodiscard]] inline OverlayRect PlaceOverlayPanel(const OverlayPanel& panel, float left,
                                                   float top, float right, float bottom) {
    constexpr float margin = 4.0f;
    const float width = static_cast<float>(panel.width);
    const float height = static_cast<float>(panel.height);
    const float x = panel.anchor == OverlayAnchor::TopRight ? right - width - margin
                                                            : left + margin;
    const float y = panel.anchor == OverlayAnchor::BottomLeft
                        ? bottom - height - margin
                        : top + margin + (bottom - top) * panel.drop;
    return {std::floor(x), std::floor(y), width, height};
}

} // namespace Common
