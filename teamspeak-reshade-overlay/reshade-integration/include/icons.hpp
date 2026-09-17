// SPDX-License-Identifier: MIT
// Vector icon painter.
//
// Every indicator is drawn with ImDrawList primitives rather than a texture. That choice buys:
//   * no texture upload and no ImTextureID to keep alive across a device reset;
//   * clean scaling to any configured size with no mipmaps and no blurring;
//   * no image assets to ship, and so no possibility of shipping anyone else's artwork.
// The cost is that user-supplied image icons are not supported in v1 (docs/compatibility.md).
#ifndef TSRO_ICONS_HPP
#define TSRO_ICONS_HPP

#include "tsro/config.hpp"

struct ImDrawList;

namespace tsro::overlay {

/// Draws `shape` centred on (cx, cy) within a box `size` pixels across.
/// `color` is packed 0xAABBGGRR. A shape of IconShape::None draws nothing.
void draw_icon(ImDrawList* draw_list, IconShape shape, float cx, float cy, float size,
               std::uint32_t color, float thickness = 1.5f);

/// Soft radial halo behind an indicator, used for the speaking glow. Drawn as a few concentric
/// circles rather than a blur, which needs no render target and costs a handful of triangles.
void draw_glow(ImDrawList* draw_list, float cx, float cy, float radius, std::uint32_t color);

/// Rounded rectangle with an optional border, used for panels and notifications.
void draw_panel(ImDrawList* draw_list, float x, float y, float w, float h, float rounding,
                std::uint32_t fill, std::uint32_t border, float border_thickness);

}  // namespace tsro::overlay

#endif  // TSRO_ICONS_HPP
