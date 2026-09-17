// SPDX-License-Identifier: MIT
#include "icons.hpp"

#include <cmath>

#include <imgui.h>
// reshade.hpp must follow imgui.h: it supplies the inline definitions for ImGui:: and
// ImDrawList:: that route through ReShade's function table. imgui.h alone only declares them,
// so omitting this compiles cleanly and then fails at link with unresolved externals.
#include <reshade.hpp>

namespace tsro::overlay {
namespace {

constexpr float kPi = 3.14159265358979323846f;

std::uint32_t with_alpha(std::uint32_t color, float scale) {
    const float alpha = static_cast<float>((color >> 24) & 0xFF) * scale;
    const std::uint32_t a = static_cast<std::uint32_t>(alpha < 0.0f ? 0.0f
                                                       : (alpha > 255.0f ? 255.0f : alpha));
    return (color & 0x00FFFFFFu) | (a << 24);
}

/// Microphone body plus stand, at a size where a literal drawing would be mud.
void draw_microphone(ImDrawList* dl, float cx, float cy, float size, std::uint32_t color,
                     float thickness) {
    const float half = size * 0.5f;
    const float capsule_w = size * 0.28f;
    const float capsule_h = size * 0.46f;
    const float top = cy - half * 0.86f;

    dl->AddRectFilled(ImVec2(cx - capsule_w, top), ImVec2(cx + capsule_w, top + capsule_h),
                      color, capsule_w);
    // Cradle: an arc under the capsule, then the stand.
    dl->PathArcTo(ImVec2(cx, top + capsule_h * 0.55f), half * 0.62f, 0.15f * kPi, 0.85f * kPi,
                  12);
    dl->PathStroke(color, ImDrawFlags_None, thickness);
    dl->AddLine(ImVec2(cx, top + capsule_h * 0.55f + half * 0.62f),
                ImVec2(cx, cy + half * 0.86f), color, thickness);
}

/// Speaker cone plus two arcs for the emitted sound.
void draw_speaker(ImDrawList* dl, float cx, float cy, float size, std::uint32_t color,
                  float thickness, bool with_waves) {
    const float half = size * 0.5f;
    const float body = half * 0.42f;
    dl->AddRectFilled(ImVec2(cx - half * 0.8f, cy - body * 0.6f),
                      ImVec2(cx - half * 0.25f, cy + body * 0.6f), color, 1.0f);
    // Cone, as a filled triangle from the body out to the rim.
    dl->AddTriangleFilled(ImVec2(cx - half * 0.25f, cy - half * 0.7f),
                          ImVec2(cx - half * 0.25f, cy + half * 0.7f),
                          ImVec2(cx + half * 0.1f, cy), color);
    dl->AddTriangleFilled(ImVec2(cx - half * 0.25f, cy - half * 0.7f),
                          ImVec2(cx + half * 0.1f, cy),
                          ImVec2(cx + half * 0.1f, cy - half * 0.7f), color);
    dl->AddTriangleFilled(ImVec2(cx - half * 0.25f, cy + half * 0.7f),
                          ImVec2(cx + half * 0.1f, cy),
                          ImVec2(cx + half * 0.1f, cy + half * 0.7f), color);
    if (!with_waves) return;
    for (int i = 1; i <= 2; ++i) {
        const float radius = half * (0.28f + 0.26f * static_cast<float>(i));
        dl->PathArcTo(ImVec2(cx + half * 0.05f, cy), radius, -0.32f * kPi, 0.32f * kPi, 10);
        dl->PathStroke(with_alpha(color, 1.0f - 0.25f * static_cast<float>(i - 1)),
                       ImDrawFlags_None, thickness);
    }
}

/// Diagonal bar used to negate an icon, drawn with a dark backing stroke so it reads against the
/// icon it crosses whatever colour that is.
void draw_slash(ImDrawList* dl, float cx, float cy, float size, std::uint32_t color,
                float thickness) {
    const float half = size * 0.55f;
    dl->AddLine(ImVec2(cx - half, cy - half), ImVec2(cx + half, cy + half), 0xC0000000,
                thickness + 1.6f);
    dl->AddLine(ImVec2(cx - half, cy - half), ImVec2(cx + half, cy + half), color, thickness);
}

void draw_star(ImDrawList* dl, float cx, float cy, float size, std::uint32_t color) {
    const float outer = size * 0.5f;
    const float inner = outer * 0.45f;
    ImVec2 points[10];
    for (int i = 0; i < 10; ++i) {
        const float radius = (i % 2 == 0) ? outer : inner;
        const float angle = -kPi * 0.5f + static_cast<float>(i) * kPi / 5.0f;
        points[i] = ImVec2(cx + std::cos(angle) * radius, cy + std::sin(angle) * radius);
    }
    dl->AddConvexPolyFilled(points, 10, color);
}

void draw_crown(ImDrawList* dl, float cx, float cy, float size, std::uint32_t color) {
    const float half = size * 0.5f;
    const ImVec2 base_left(cx - half, cy + half * 0.55f);
    const ImVec2 base_right(cx + half, cy + half * 0.55f);
    dl->AddTriangleFilled(base_left, base_right, ImVec2(cx, cy - half * 0.1f), color);
    dl->AddTriangleFilled(base_left, ImVec2(cx - half * 0.33f, cy),
                          ImVec2(cx - half, cy - half * 0.7f), color);
    dl->AddTriangleFilled(base_right, ImVec2(cx + half * 0.33f, cy),
                          ImVec2(cx + half, cy - half * 0.7f), color);
    dl->AddTriangleFilled(ImVec2(cx - half * 0.33f, cy), ImVec2(cx + half * 0.33f, cy),
                          ImVec2(cx, cy - half * 0.95f), color);
}

/// Three bars of differing height: a level meter, which reads as "speaking" at a glance.
void draw_bars(ImDrawList* dl, float cx, float cy, float size, std::uint32_t color) {
    const float half = size * 0.5f;
    const float width = size * 0.18f;
    const float heights[3] = {0.55f, 1.0f, 0.72f};
    for (int i = 0; i < 3; ++i) {
        const float x = cx + (static_cast<float>(i) - 1.0f) * size * 0.3f;
        const float h = half * heights[i];
        dl->AddRectFilled(ImVec2(x - width * 0.5f, cy - h), ImVec2(x + width * 0.5f, cy + h),
                          color, width * 0.4f);
    }
}

/// Crescent: a filled disc with an offset disc punched out using the background-free trick of
/// drawing the notch in the same colour as nothing — instead we draw two arcs and fill between.
void draw_moon(ImDrawList* dl, float cx, float cy, float size, std::uint32_t color) {
    const float radius = size * 0.5f;
    dl->PathArcTo(ImVec2(cx, cy), radius, kPi * 0.42f, kPi * 1.58f, 16);
    dl->PathArcTo(ImVec2(cx + radius * 0.52f, cy), radius * 0.92f, kPi * 1.35f, kPi * 0.65f, 16);
    dl->PathFillConvex(color);
}

/// A speech tail on a rounded body: a whisper, visually distinct from a level meter.
void draw_whisper(ImDrawList* dl, float cx, float cy, float size, std::uint32_t color) {
    const float half = size * 0.5f;
    dl->AddRectFilled(ImVec2(cx - half, cy - half * 0.78f), ImVec2(cx + half, cy + half * 0.22f),
                      color, half * 0.35f);
    dl->AddTriangleFilled(ImVec2(cx - half * 0.32f, cy + half * 0.18f),
                          ImVec2(cx + half * 0.12f, cy + half * 0.18f),
                          ImVec2(cx - half * 0.52f, cy + half * 0.85f), color);
}

}  // namespace

void draw_icon(ImDrawList* dl, IconShape shape, float cx, float cy, float size,
               std::uint32_t color, float thickness) {
    if (dl == nullptr || shape == IconShape::None || size <= 0.0f) return;
    const float half = size * 0.5f;

    switch (shape) {
        case IconShape::None: return;
        case IconShape::Dot:
            dl->AddCircleFilled(ImVec2(cx, cy), half * 0.45f, color, 12);
            return;
        case IconShape::Circle:
            dl->AddCircleFilled(ImVec2(cx, cy), half, color, 16);
            return;
        case IconShape::Ring:
            dl->AddCircle(ImVec2(cx, cy), half * 0.86f, color, 16, thickness);
            return;
        case IconShape::Square:
            dl->AddRectFilled(ImVec2(cx - half * 0.8f, cy - half * 0.8f),
                              ImVec2(cx + half * 0.8f, cy + half * 0.8f), color, half * 0.22f);
            return;
        case IconShape::Diamond: {
            const ImVec2 points[4] = {ImVec2(cx, cy - half), ImVec2(cx + half, cy),
                                      ImVec2(cx, cy + half), ImVec2(cx - half, cy)};
            dl->AddConvexPolyFilled(points, 4, color);
            return;
        }
        case IconShape::Triangle:
            dl->AddTriangleFilled(ImVec2(cx, cy - half), ImVec2(cx + half, cy + half * 0.8f),
                                  ImVec2(cx - half, cy + half * 0.8f), color);
            return;
        case IconShape::Star: draw_star(dl, cx, cy, size, color); return;
        case IconShape::Chevron:
            dl->PathLineTo(ImVec2(cx - half * 0.6f, cy - half * 0.5f));
            dl->PathLineTo(ImVec2(cx + half * 0.5f, cy));
            dl->PathLineTo(ImVec2(cx - half * 0.6f, cy + half * 0.5f));
            dl->PathStroke(color, ImDrawFlags_None, thickness + 0.5f);
            return;
        case IconShape::Microphone:
            draw_microphone(dl, cx, cy, size, color, thickness);
            return;
        case IconShape::MicrophoneMuted:
            draw_microphone(dl, cx, cy, size, color, thickness);
            draw_slash(dl, cx, cy, size, color, thickness);
            return;
        case IconShape::Speaker: draw_speaker(dl, cx, cy, size, color, thickness, true); return;
        case IconShape::SpeakerMuted:
            draw_speaker(dl, cx, cy, size, color, thickness, false);
            draw_slash(dl, cx, cy, size, color, thickness);
            return;
        case IconShape::Moon: draw_moon(dl, cx, cy, size, color); return;
        case IconShape::Record:
            dl->AddCircleFilled(ImVec2(cx, cy), half * 0.62f, color, 14);
            dl->AddCircle(ImVec2(cx, cy), half * 0.92f, with_alpha(color, 0.55f), 16, thickness);
            return;
        case IconShape::Crown: draw_crown(dl, cx, cy, size, color); return;
        case IconShape::Whisper: draw_whisper(dl, cx, cy, size, color); return;
        case IconShape::Bars: draw_bars(dl, cx, cy, size, color); return;
    }
}

void draw_glow(ImDrawList* dl, float cx, float cy, float radius, std::uint32_t color) {
    if (dl == nullptr || radius <= 0.0f) return;
    // Four concentric circles approximate a falloff convincingly at overlay sizes and cost a few
    // dozen triangles, with no render target and no shader.
    constexpr int kLayers = 4;
    for (int i = kLayers; i >= 1; --i) {
        const float t = static_cast<float>(i) / static_cast<float>(kLayers);
        dl->AddCircleFilled(ImVec2(cx, cy), radius * t, with_alpha(color, (1.0f - t) * 0.5f), 18);
    }
}

void draw_panel(ImDrawList* dl, float x, float y, float w, float h, float rounding,
                std::uint32_t fill, std::uint32_t border, float border_thickness) {
    if (dl == nullptr || w <= 0.0f || h <= 0.0f) return;
    const ImVec2 min(x, y);
    const ImVec2 max(x + w, y + h);
    if ((fill >> 24) != 0) dl->AddRectFilled(min, max, fill, rounding);
    if ((border >> 24) != 0 && border_thickness > 0.0f) {
        dl->AddRect(min, max, border, rounding, ImDrawFlags_None, border_thickness);
    }
}

}  // namespace tsro::overlay
