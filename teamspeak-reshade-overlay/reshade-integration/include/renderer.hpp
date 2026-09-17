// SPDX-License-Identifier: MIT
// The HUD renderer.
//
// Runs inside ReShade's ImGui frame, on the game's render thread. Its contract:
//   * never block — it takes no lock that any other thread holds for long, and never touches a
//     pipe handle;
//   * never allocate in steady state — buffers are reserved once and reused;
//   * create no GPU resource — it only appends to ImGui's background draw list, which ReShade
//     already submits, so device resets and resolution changes need no handling here.
#ifndef TSRO_RENDERER_HPP
#define TSRO_RENDERER_HPP

#include <cstdint>
#include <string>
#include <vector>

#include "tsro/config.hpp"
#include "tsro/layout.hpp"
#include "tsro/notifications.hpp"
#include "tsro/overlay_client.hpp"

struct ImDrawList;
struct ImFont;

namespace tsro::overlay {

/// How ReShade's ImGui answered the last text measurement.
///
/// Text width drives every right-aligned position and every overflow budget, and ReShade's ImGui
/// is a different build from the one this add-on compiles against, so measurement is not
/// something that can simply be assumed to work. `Estimated` means neither table entry returned
/// a usable width and the overlay is running on a codepoint estimate -- still readable, but the
/// edges will be a few pixels out. The debug panel shows this.
enum class TextMetricsSource { Unknown, CalcTextSize, CalcTextSizeA, Estimated };

TextMetricsSource text_metrics_source() noexcept;
const char* text_metrics_source_name(TextMetricsSource source) noexcept;

/// Mock state for the settings preview. Built from fixed sample users so the preview can show
/// every indicator without inventing a TeamSpeak event or touching the live model.
OverlayState preview_state();
std::vector<ChatMessage> preview_chat();

struct FrameStats {
    float last_layout_ms = 0.0f;
    float last_draw_ms = 0.0f;
    std::uint64_t frames = 0;
    std::uint64_t layouts = 0;      ///< how often the layout was actually recomputed
    std::size_t draw_commands = 0;
};

class Renderer {
public:
    /// Draws one frame. `now_ms` is wall-clock; `viewport` comes from ImGui's display size.
    /// When `preview` is set, that state is drawn instead of the live one — used by the settings
    /// window's live preview, which never fabricates a TeamSpeak event.
    void draw(ImDrawList* draw_list, const Config& config, const OverlayFrame& frame,
              const Viewport& viewport, std::int64_t now_ms, const OverlayState* preview,
              const std::vector<ChatMessage>* preview_chat_messages);

    /// Feeds semantic events into the notification queue. Called before draw, on the same thread.
    void submit_events(const std::vector<OverlayEvent>& events, const Config& config,
                       std::int64_t now_ms);

    void note_connected(std::int64_t now_ms) { notifications_.note_connected(now_ms); }

    /// Fills the notification queue with one of every kind, for the settings preview, so the
    /// user can see where each type lands and how it is styled without waiting for the events to
    /// happen for real. Uses the same formatting and lifecycle path as live notifications --
    /// it is the real pipeline fed sample events, not a separate mock renderer.
    void seed_preview_notifications(const Config& config, std::int64_t now_ms);
    /// True once every seeded notification has aged out, so the caller can re-seed.
    bool notifications_empty() const noexcept { return notifications_.items().empty(); }
    void invalidate() { layout_dirty_ = true; }
    void clear_notifications() { notifications_.clear(); }

    const FrameStats& stats() const noexcept { return stats_; }
    NotificationQueue& notifications() noexcept { return notifications_; }

private:
    void draw_title(ImDrawList* dl, const Config& config, const LayoutResult& layout,
                    float opacity);
    void draw_users(ImDrawList* dl, const Config& config, const LayoutResult& layout,
                    float opacity, std::int64_t now_ms);
    void draw_notifications(ImDrawList* dl, const Config& config, const Viewport& viewport,
                            float opacity, std::int64_t now_ms);
    void draw_chat(ImDrawList* dl, const Config& config, const Viewport& viewport,
                   const std::vector<ChatMessage>& messages, float opacity, std::int64_t now_ms);
    void draw_text(ImDrawList* dl, const Config& config, float x, float y, float size,
                   std::uint32_t color, std::string_view text);

    LayoutResult layout_;
    NotificationQueue notifications_;
    SpeakingEnvelope envelope_;
    FrameStats stats_;

    bool layout_dirty_ = true;
    std::uint64_t last_revision_ = 0;
    std::int64_t last_frame_ms_ = 0;
    std::int64_t last_activity_ms_ = 0;
    std::uint64_t last_config_hash_ = 0;

    // Reused across frames so a steady-state frame performs no allocation.
    std::vector<std::pair<std::string, bool>> talking_scratch_;
    std::string text_scratch_;
};

}  // namespace tsro::overlay

#endif  // TSRO_RENDERER_HPP
