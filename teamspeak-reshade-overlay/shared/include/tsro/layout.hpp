// SPDX-License-Identifier: MIT
// Geometry and style resolution. Pure functions over (state, config, viewport) with text
// measurement injected, so the whole visual pipeline short of the actual draw calls is testable
// without a GPU — which is how "tested at different resolutions and aspect ratios" is achieved
// in CI rather than only by hand.
#ifndef TSRO_LAYOUT_HPP
#define TSRO_LAYOUT_HPP

#include <functional>
#include <string>
#include <vector>

#include "tsro/config.hpp"
#include "tsro/model.hpp"

namespace tsro {

struct Viewport {
    float width = 1920.0f;
    float height = 1080.0f;
};

struct Rect {
    float x = 0, y = 0, w = 0, h = 0;
    float right() const noexcept { return x + w; }
    float bottom() const noexcept { return y + h; }
    bool contains(float px, float py) const noexcept {
        return px >= x && px <= right() && py >= y && py <= bottom();
    }
};

/// Measures the advance width of `text` at `font_size` pixels. Supplied by the renderer
/// (ImGui's font) in production and by a deterministic stub in tests.
using MeasureFn = std::function<float(std::string_view text, float font_size)>;

/// Top-left corner for a box of size (w,h) placed at `p` within `vp`.
/// Percentage offsets are fractions of the viewport; pixel offsets are absolute. The anchor
/// determines which viewport corner/edge the offset is measured from and which way it grows,
/// so a bottom-right anchored element stays bottom-right at any resolution.
Rect resolve_placement(const Placement& p, float w, float h, const Viewport& vp) noexcept;

/// Horizontal start for text of width `text_w` inside a box of width `box_w`.
float align_offset(Align a, float box_w, float text_w) noexcept;

struct FittedText {
    std::string text;              ///< possibly truncated, with the ellipsis already appended
    std::vector<std::string> lines;///< one entry unless the mode is Wrap
    float font_scale = 1.0f;       ///< < 1 only for OverflowMode::Shrink
    float width = 0.0f;
    bool truncated = false;
    /// Pixels to shift left this frame for OverflowMode::Scroll; 0 for every other mode.
    float scroll_offset = 0.0f;
};

/// Applies an overflow strategy so a long name cannot break the layout.
/// `time_s` drives Scroll; ignored otherwise.
FittedText fit_text(std::string_view text, float max_width, float font_size, OverflowMode mode,
                    float min_font_scale, const MeasureFn& measure, float time_s = 0.0f);

/// The visual treatment of one user, after per-user overrides and state priority are applied.
struct ResolvedUser {
    const UserState* user = nullptr;
    std::string display;        ///< after the friend tag and any per-user display override
    bool is_friend = false;
    std::string friend_tag;     ///< "[tag] " including brackets and trailing space, or empty        ///< after per-user display override
    Color name_color{};
    float entry_opacity = 1.0f;
    bool show_border = false;
    Color border{};
    float border_thickness = 0.0f;
    bool glow = false;
    Color glow_color{};
    float glow_radius = 0.0f;
    bool show_background = false;
    Color background{};
    bool speaking = false;
    struct Indicator {
        IconShape shape = IconShape::Dot;
        Color color{};
        float scale = 1.0f;
    };
    std::vector<Indicator> leading;   ///< drawn before the name (commander lives here)
    std::vector<Indicator> trailing;  ///< drawn after the name (mute/away/recording)
};

/// Resolves one user's appearance. `speaking_intensity` ∈ [0,1] is the animated envelope.
ResolvedUser resolve_user(const UserState& u, const Config& cfg, float speaking_intensity);

/// Orders users for display, honouring the sort mode, the speaking-first option and the
/// local-user / muted-user visibility switches. Returns pointers into `state.users`.
std::vector<const UserState*> order_users(const OverlayState& state, const Config& cfg);

/// Substitutes {name} {channel} {parent} {previous} {count} {status} {message} {server} {time}
/// in a notification or title format string. Unknown placeholders are left as written so a typo
/// is visible rather than silently swallowed.
struct FormatValues {
    std::string name;
    std::string channel;
    std::string parent;     ///< the channel's parent, distinct from {previous}
    std::string previous;   ///< the channel departed from, for a switch notification
    std::string count;
    std::string status;
    std::string message;
    std::string server;
    std::string time;
};
std::string format_template(std::string_view tmpl, const FormatValues& v);

/// Which placeholder a run of the formatted text came from, so the renderer can colour it.
enum class FormatField { None, Name, Channel, Parent, Previous, Count, Status, Message, Server, Time };

struct FormatSpan {
    std::size_t begin = 0;   ///< byte offset into the formatted string
    std::size_t end = 0;
    FormatField field = FormatField::None;
};

/// Same expansion, but also records where each substituted value landed. Anything not covered by
/// a span is literal text from the template.
std::string format_template(std::string_view tmpl, const FormatValues& v,
                            std::vector<FormatSpan>& spans);

/// The complete frame geometry the renderer walks.
struct ChannelTitleLayout {
    bool visible = false;
    Rect rect;
    /// The alignment that actually applies: the group's when the blocks are linked, this
    /// element's own otherwise. Resolved here so the renderer cannot disagree with the layout
    /// about which one wins -- it previously read the element's own and ignored the group's.
    Align align = Align::Left;
    std::string text;
    Color text_color{};
    Color background{};
    Color border{};
    bool show_background = false;
    bool show_border = false;
    IconShape icon = IconShape::None;
    Color icon_color{};
    float font_size = 16.0f;
    float opacity = 1.0f;
};

struct UserRowLayout {
    Rect rect;
    ResolvedUser resolved;
    FittedText name;
    float font_size = 16.0f;
};

struct UserListLayout {
    bool visible = false;
    Rect rect;
    Align align = Align::Left;
    std::vector<UserRowLayout> rows;
    int hidden_count = 0;
    std::string overflow_text;
};

struct LayoutResult {
    Viewport viewport;
    float scale = 1.0f;
    ChannelTitleLayout title;
    UserListLayout users;
    /// True when the overlay must present itself as not-live (disconnected, unsynchronised or
    /// stale). The renderer is required to honour this; it is not a styling hint.
    bool degraded = false;
    std::string degraded_reason;
};

/// Computes the frame layout. Pure: no globals, no clock, no I/O.
LayoutResult compute_layout(const OverlayState& state, const Config& cfg, const Viewport& vp,
                            const MeasureFn& measure, float time_s,
                            const std::function<float(const std::string&)>& speaking_intensity);

}  // namespace tsro

#endif  // TSRO_LAYOUT_HPP
