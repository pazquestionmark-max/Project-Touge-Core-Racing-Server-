// SPDX-License-Identifier: MIT
// Versioned, self-repairing overlay configuration.
//
// Two invariants hold throughout:
//   * Loading never fails and never throws. Bad values are clamped or defaulted and every
//     repair is recorded in ConfigDiagnostics so the user can see what happened.
//   * Unknown keys are preserved verbatim, so a config written by a newer build and opened by
//     an older one is not silently stripped of the settings it did not understand.
#ifndef TSRO_CONFIG_HPP
#define TSRO_CONFIG_HPP

#include <array>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "tsro/json.hpp"

namespace tsro {

inline constexpr int kConfigVersion = 1;

// --- primitives --------------------------------------------------------------------------

struct Color {
    std::uint8_t r = 255, g = 255, b = 255, a = 255;

    constexpr Color() = default;
    constexpr Color(std::uint8_t rr, std::uint8_t gg, std::uint8_t bb, std::uint8_t aa = 255)
        : r(rr), g(gg), b(bb), a(aa) {}

    /// Accepts #RGB, #RGBA, #RRGGBB, #RRGGBBAA, with or without the leading '#'.
    static std::optional<Color> from_hex(std::string_view);
    std::string to_hex() const;  ///< always "#RRGGBBAA"

    /// 0xAABBGGRR, the packed layout ImGui's draw list expects.
    std::uint32_t to_abgr() const noexcept {
        return (static_cast<std::uint32_t>(a) << 24) | (static_cast<std::uint32_t>(b) << 16) |
               (static_cast<std::uint32_t>(g) << 8) | static_cast<std::uint32_t>(r);
    }
    Color with_alpha_scale(float s) const noexcept;
    bool operator==(const Color& o) const noexcept {
        return r == o.r && g == o.g && b == o.b && a == o.a;
    }
    bool operator!=(const Color& o) const noexcept { return !(*this == o); }
};

enum class Anchor {
    TopLeft, TopCenter, TopRight,
    CenterLeft, Center, CenterRight,
    BottomLeft, BottomCenter, BottomRight,
};

enum class Align { Left, Center, Right };

enum class IconShape {
    None, Dot, Circle, Ring, Square, Diamond, Triangle, Star, Chevron,
    Microphone, MicrophoneMuted, Speaker, SpeakerMuted, Moon, Record, Crown, Whisper, Bars,
};

enum class Easing { Linear, EaseIn, EaseOut, EaseInOut, EaseOutBack, EaseOutElastic };

enum class OverflowMode { Clip, Ellipsis, Wrap, Shrink, Scroll };

enum class UserSort { ChannelOrder, Alphabetical, TalkPower, SpeakingFirst };

enum class SpeakingAnimation { None, ColorFade, Pulse, Glow, BorderSweep };

enum class ChatOrder { NewestBottom, NewestTop };

enum class StackDirection { Down, Up };

/// How a notification box's edge is drawn. Defined here so the enum helpers below can name it.
enum class NotificationBorder {
    None,
    Accent,   ///< the category's own colour, dimmed -- each toast is outlined in its own hue
    Custom,   ///< one colour for every category
};

const char* to_string(Anchor) noexcept;
const char* to_string(Align) noexcept;
const char* to_string(IconShape) noexcept;
const char* to_string(Easing) noexcept;
const char* to_string(OverflowMode) noexcept;
const char* to_string(UserSort) noexcept;
const char* to_string(SpeakingAnimation) noexcept;
const char* to_string(ChatOrder) noexcept;
const char* to_string(StackDirection) noexcept;
const char* to_string(NotificationBorder) noexcept;

bool parse_enum(std::string_view, Anchor&) noexcept;
bool parse_enum(std::string_view, Align&) noexcept;
bool parse_enum(std::string_view, IconShape&) noexcept;
bool parse_enum(std::string_view, Easing&) noexcept;
bool parse_enum(std::string_view, OverflowMode&) noexcept;
bool parse_enum(std::string_view, UserSort&) noexcept;
bool parse_enum(std::string_view, SpeakingAnimation&) noexcept;
bool parse_enum(std::string_view, ChatOrder&) noexcept;
bool parse_enum(std::string_view, StackDirection&) noexcept;
bool parse_enum(std::string_view, NotificationBorder&) noexcept;

/// Applies `easing` to t∈[0,1].
float ease(Easing, float t) noexcept;

/// Where an element sits. Offsets are pixels, or fractions of the viewport when `percent`.
struct Placement {
    bool visible = true;
    Anchor anchor = Anchor::TopLeft;
    float x = 24.0f;
    float y = 24.0f;
    bool percent = false;
    Align align = Align::Left;
};

struct Fade {
    int in_ms = 200;
    int out_ms = 350;
    int hold_ms = 4000;   ///< fully-visible time between the two animations
    float start_opacity = 0.0f;
    float end_opacity = 1.0f;
    Easing easing = Easing::EaseOutBack;
};

/// Visual treatment of one voice/user state. Every indicator has its own instance, so the
/// brief's requirement that each be independently configurable is structural, not incidental.
struct StateStyle {
    bool enabled = true;
    bool show_icon = true;
    IconShape icon = IconShape::Dot;
    float icon_scale = 1.0f;
    Color icon_color{255, 255, 255, 255};
    bool override_text_color = false;
    Color text_color{255, 255, 255, 255};
    bool show_background = false;
    Color background{0, 0, 0, 0};
    bool show_border = false;
    Color border{0, 0, 0, 0};
    float border_thickness = 1.5f;
    bool glow = false;
    Color glow_color{255, 255, 255, 90};
    float glow_radius = 6.0f;
    float opacity = 1.0f;
    bool dim_entry = false;
    float dim_amount = 0.45f;
    /// Reserved for a future release; see docs/compatibility.md. Ignored by the v1 renderer,
    /// but round-tripped so configs written against a later build are not damaged.
    std::string icon_image;
};

// --- sections ------------------------------------------------------------------------------

struct GeneralConfig {
    bool enabled = true;
    bool show_when_disconnected = true;
    bool show_when_plugin_unavailable = true;
    float master_opacity = 0.92f;
    float scale = 1.0f;
    std::string profile_name = "default";
    bool auto_profile_by_executable = true;
    /// Show every setting. Off by default: the common ones fit on two tabs, and burying them in
    /// two hundred others helps nobody.
    bool advanced_settings = false;
};

struct AppearanceConfig {
    /// The typeface the HUD draws with, as a file name inside the overlay's `fonts` folder
    /// (see docs/configuration.md). Empty means ReShade's own font.
    ///
    /// The add-on rasterises this itself rather than asking ReShade for it: ReShade owns the
    /// ImGui font atlas, and reading that atlas from an add-on is what crashed the game the
    /// first time this was attempted. Loading the .ttf here touches none of ReShade's ImGui
    /// state -- it produces a texture of our own and draws glyph quads from it -- so the font
    /// applies to the overlay alone and leaves ReShade's own UI untouched.
    std::string font_file;
    /// Face index inside a .ttc collection. 0 for an ordinary .ttf/.otf.
    int font_face_index = 0;
    /// Retained only so an older profile still loads; superseded by font_file.
    int font_index = 0;
    float font_size = 15.0f;
    float icon_size = 8.0f;
    // Rows sized to the text rather than padded out: 1.15x the font is a normal line height.
    float row_height = 17.0f;
    float row_spacing = 0.0f;
    float padding_x = 0.0f;
    float padding_y = 0.0f;
    float corner_radius = 3.0f;
    Color panel_background{16, 18, 22, 150};
    Color panel_border{255, 255, 255, 26};
    float panel_border_thickness = 1.0f;
    // Off by default: a translucent slab behind the names is more intrusive than the names are.
    bool show_panel_background = false;
    bool text_shadow = true;          ///< keeps text legible over bright game content
    Color text_shadow_color{0, 0, 0, 190};
    float text_shadow_offset = 1.0f;
    /// A full outline rather than a one-sided shadow. Costs eight extra text draws per string,
    /// so it is off by default, but it is the only thing that stays readable over *any*
    /// background rather than most of them.
    bool text_outline = false;
    Color text_outline_color{0, 0, 0, 230};
    float text_outline_thickness = 1.0f;
    Color text_default{230, 233, 238, 255};
    Color text_secondary{150, 156, 166, 255};
    Color accent{88, 166, 255, 255};
};

struct ChannelTitleConfig {
    Placement placement{true, Anchor::TopLeft, 24.0f, 24.0f, false, Align::Left};
    /// Multiplies this block only, on top of the global and group scales.
    float scale = 1.0f;
    bool show_parent = true;
    bool show_user_count = true;
    bool show_server_name = false;
    bool show_topic = false;
    float font_scale = 1.15f;
    float opacity = 1.0f;
    Color text{255, 255, 255, 255};
    Color background{0, 0, 0, 0};
    Color border{0, 0, 0, 0};
    bool show_background = false;
    bool show_border = false;
    IconShape icon = IconShape::None;
    Color icon_color{88, 166, 255, 255};
    /// Widest the title may be, in unscaled pixels. 0 means "as wide as the screen allows",
    /// which still clamps -- an unbounded title simply runs off the edge.
    float max_width = 0.0f;
    std::string format = "{channel}";
    std::string parent_format = "{parent} / {channel}";
    std::string disconnected_text = "TeamSpeak: not connected";
};

struct UserListConfig {
    Placement placement{true, Anchor::TopLeft, 24.0f, 54.0f, false, Align::Left};
    /// Multiplies this block only, on top of the global and group scales.
    float scale = 1.0f;
    bool show_local_user = true;
    bool highlight_local_user = true;
    Color local_user_color{255, 214, 102, 255};
    bool show_muted_users = true;
    /// Show only people who are actually talking. Turns the roster into a speaking indicator,
    /// which is what you want when the overlay is competing with a busy game for screen space.
    bool only_show_talking = false;
    bool speaking_first = false;
    UserSort sort = UserSort::Alphabetical;
    OverflowMode name_overflow = OverflowMode::Ellipsis;
    float max_name_width = 180.0f;
    float min_font_scale = 0.75f;     ///< floor for OverflowMode::Shrink
    int max_visible_users = 24;
    bool show_overflow_count = true;
    bool show_avatar_initial = false; ///< see docs/protocol.md §6: avatar images are unavailable
    float indicator_gap = 4.0f;

    /// Use the friend list from TeamSpeak's own Contacts, which the plugin reads out of the
    /// client's settings. Off falls back to whoever is marked a friend here, in Advanced.
    bool use_teamspeak_friends = true;
    /// Colour applied to anyone marked as a friend, unless they have their own name colour.
    bool color_friends = true;
    Color friend_color{126, 231, 135, 255};
    /// Draw the friend's tag as "[tag] " before their name.
    bool show_friend_tag = true;
    Color friend_tag_color{136, 200, 255, 255};
};

struct NotificationStyle {
    bool enabled = true;
    std::string format = "{name} joined {channel}";
    std::string prefix = "[+]";
    IconShape icon = IconShape::Chevron;
    Color icon_color{126, 231, 135, 255};
    Color text{230, 233, 238, 255};
    Color name_color{126, 231, 135, 255};
    Color background{16, 18, 22, 200};
    Color border{126, 231, 135, 120};
    bool show_background = true;
    bool show_border = true;
    Fade fade{180, 320, 3500, 0.0f, 1.0f, Easing::EaseOutBack};
    bool sound = false;
    std::string sound_file;
    /// Wrap the message onto further lines instead of widening the box.
    ///
    /// On for the two kinds that carry someone else's prose -- a chat or private message can be
    /// any length, and a toast as wide as the screen is unreadable. Off for the rest: "X left
    /// Y" is one short line and should simply take the room it needs.
    bool wrap = false;
    /// Most lines a wrapped message may use before it is ellipsised.
    int max_lines = 4;
};

/// The box every notification is drawn in.
///
/// Separate from the per-category colours so the *shape* of a toast -- how dark it is, how sharp
/// its corners are, whether it carries an edge -- is one setting rather than six.
struct NotificationBoxStyle {
    Color background{10, 12, 16, 235};
    /// 0 is a hard rectangle. Small values read as a panel; large ones as a pill.
    float corner_radius = 2.0f;
    NotificationBorder border = NotificationBorder::Accent;
    Color border_color{255, 255, 255, 48};
    float border_thickness = 1.0f;
    /// How strongly the category colour shows in the edge when `border` is Accent.
    float border_accent_opacity = 0.55f;
    /// The vertical stripe down the leading edge, in the category's colour.
    bool accent_bar = true;
    float accent_bar_width = 3.0f;
    /// Size each toast to its own text instead of a fixed column, as the reference layout does.
    bool auto_width = true;
    /// The cap on a toast that sizes itself. Generous on purpose: a one-line event like
    /// "someone left a channel" should extend rather than lose its ending to an ellipsis.
    float max_width = 900.0f;
};

struct NotificationsConfig {
    Placement placement{true, Anchor::TopRight, 24.0f, 24.0f, false, Align::Right};
    int max_visible = 5;
    StackDirection stack = StackDirection::Down;
    float spacing = 6.0f;
    /// Used when box.auto_width is off, and as the minimum width when it is on.
    float width = 280.0f;
    float min_height = 26.0f;
    NotificationBoxStyle box{};
    /// Independent of appearance.padding_*, which the user list may legitimately set to zero.
    float padding_x = 10.0f;
    float padding_y = 6.0f;
    /// Notification text size, relative to appearance.font_size.
    float font_scale = 1.0f;
    bool merge_duplicates = true;
    /// Events arriving within this window of a (re)connection are absorbed into the initial
    /// synchronisation instead of producing a burst of join notifications.
    int suppress_after_connect_ms = 2500;

    NotificationStyle join{};
    NotificationStyle leave{};
    NotificationStyle channel_switch{};
    NotificationStyle connection{};
    NotificationStyle whisper{};
    NotificationStyle chat{};
    /// Private messages get their own toast. A message sent to you personally is not the same
    /// event as one sent to the channel, and the thing you need from it first is who it is from.
    NotificationStyle private_chat{};
};

struct ChatConfig {
    Placement placement{false, Anchor::BottomLeft, 24.0f, 24.0f, false, Align::Left};
    bool show_channel_messages = true;
    bool show_server_messages = false;
    /// Off by default and enforced server-side: see docs/protocol.md §4.
    bool show_private_messages = false;
    ChatOrder order = ChatOrder::NewestBottom;
    int max_visible_messages = 6;
    int history_size = 50;
    int max_message_length = 200;
    int retention_seconds = 300;   ///< 0 keeps messages until evicted by history_size
    bool show_timestamp = true;
    bool show_sender = true;
    bool show_channel_name = false;
    bool show_category_icon = true;
    bool wrap = true;
    float width = 420.0f;
    float font_scale = 0.95f;
    Color text{214, 219, 228, 255};
    Color sender{88, 166, 255, 255};
    Color timestamp{120, 126, 136, 255};
    Color background{16, 18, 22, 150};
    bool show_background = true;
    bool use_sender_color = true;   ///< colour the sender using their per-user override
};

/// Binds the channel title and the user list into a single block.
///
/// With `enabled`, the two stack vertically and are placed, moved and scaled as one -- which is
/// what a corner roster wants, and what makes "move both together" a single control instead of
/// two that must be kept in step by hand. Turn it off and each falls back to its own placement,
/// so they can be put anywhere independently. Per-element `scale` works either way.
struct GroupConfig {
    bool enabled = true;
    Anchor anchor = Anchor::TopRight;
    float x = 16.0f;
    float y = 10.0f;
    bool percent = false;
    Align align = Align::Right;
    /// Scales the whole block. Per-element scales multiply on top of this.
    float scale = 1.0f;
    /// Gap between the title and the first row, in unscaled pixels.
    float spacing = 4.0f;
};

struct IndicatorsConfig {
    StateStyle speaking{};
    StateStyle whispering{};
    StateStyle mic_muted{};
    StateStyle speaker_muted{};
    StateStyle mic_hardware_off{};
    StateStyle away{};
    StateStyle recording{};
    StateStyle commander{};
    StateStyle priority_speaker{};
    StateStyle suppressed{};
    StateStyle locally_muted{};
};

struct AnimationConfig {
    bool enabled = true;
    SpeakingAnimation speaking = SpeakingAnimation::Pulse;
    int speaking_attack_ms = 90;
    int speaking_release_ms = 260;
    float speaking_pulse_hz = 2.2f;
    float speaking_pulse_depth = 0.35f;
    Easing state_easing = Easing::EaseOut;
    int state_transition_ms = 180;
    bool animate_list_reorder = true;
    int list_reorder_ms = 220;
    Fade overlay_fade{250, 400, 0, 0.0f, 1.0f, Easing::EaseOut};
    bool fade_when_idle = false;
    int idle_after_ms = 15000;
    float idle_opacity = 0.35f;
};

struct IntegrationConfig {
    /// Empty means the default `tsro.v1.<user-sid>`. Overriding is a diagnostic aid.
    std::string pipe_name;
    int reconnect_initial_ms = 250;
    int reconnect_max_ms = 5000;
    int stale_after_ms = 6000;
    int ping_interval_ms = 10000;
    bool auto_connect = true;
};

struct LoggingConfig {
    /// trace | debug | info | warn | error | off
    std::string level = "info";
    bool to_file = true;
    std::string file_name;  ///< empty ⇒ default location, see docs/troubleshooting.md
    int max_file_kb = 1024;
    /// Off by default. Chat content is never written to the log unless this is switched on,
    /// and the settings UI states the consequence next to the checkbox.
    bool include_message_content = false;
    bool show_diagnostics_overlay = false;
};

/// Per-user styling, keyed on CLIENT_UNIQUE_IDENTIFIER — stable across nickname changes and
/// reconnects, unlike a nickname or a session client id.
struct UserOverride {
    bool enabled = true;
    /// Marks this identity as a friend.
    ///
    /// TeamSpeak's own friend/foe list is NOT readable from a plugin -- it lives in the client's
    /// local Contacts database and appears nowhere in the plugin API (no property, no callback).
    /// So the list is kept here instead, keyed on the same stable identity as every other
    /// override, which means it survives nickname changes and reconnects just as they do.
    bool is_friend = false;
    /// Shown as "[tag] Nickname" in front of their TeamSpeak name. Empty shows no tag.
    std::string friend_tag;
    std::optional<Color> name_color;
    std::optional<Color> speaking_color;
    std::optional<Color> muted_color;
    std::optional<Color> commander_color;
    std::optional<IconShape> icon;
    std::optional<Color> icon_color;
    std::string display_override;   ///< empty ⇒ use the TeamSpeak display name
    std::string note;               ///< user's own reminder of who this identity is
};

/// Per-channel styling. The key is "<virtualserver_unique_identifier>:<channel_id>", because a
/// channel id alone is not unique across servers.
struct ChannelOverride {
    bool enabled = true;
    std::optional<Color> title_color;
    std::optional<Color> background;
    std::optional<Color> border;
    std::optional<Color> user_list_color;
    std::optional<IconShape> icon;
    std::optional<Color> icon_color;
    std::optional<float> font_scale;
    std::optional<float> opacity;
    std::string display_override;
};

std::string make_channel_key(std::string_view server_unique_id, std::uint64_t channel_id);

/// A repair the loader had to perform. Surfaced in the Diagnostics tab rather than swallowed.
struct ConfigIssue {
    enum class Severity { Info, Warning, Error } severity = Severity::Warning;
    std::string path;     ///< dotted path, e.g. "appearance.font_size"
    std::string message;
};

struct ConfigDiagnostics {
    std::vector<ConfigIssue> issues;
    bool migrated = false;
    int loaded_version = kConfigVersion;
    bool from_defaults = false;
    bool newer_than_supported = false;
    void add(ConfigIssue::Severity s, std::string path, std::string message) {
        issues.push_back({s, std::move(path), std::move(message)});
    }
    bool empty() const noexcept { return issues.empty(); }
};

struct Config {
    int config_version = kConfigVersion;
    GeneralConfig general;
    AppearanceConfig appearance;
    GroupConfig group;
    ChannelTitleConfig channel_title;
    UserListConfig user_list;
    IndicatorsConfig indicators;
    NotificationsConfig notifications;
    ChatConfig chat;
    AnimationConfig animation;
    IntegrationConfig integration;
    LoggingConfig logging;
    std::map<std::string, UserOverride> user_overrides;
    std::map<std::string, ChannelOverride> channel_overrides;
    /// Keys we did not recognise, kept so a round-trip through an older build is lossless.
    json::Object unknown;

    /// Defaults that are not merely zero — the shipped look. Defined in config_defaults.cpp.
    static Config defaults();

    json::Value to_json() const;
    /// Total: always returns a usable Config. `diag` records every repair.
    static Config from_json(const json::Value&, ConfigDiagnostics& diag);

    /// Parses text, migrating older versions. Malformed input yields defaults plus an error.
    static Config parse(std::string_view text, ConfigDiagnostics& diag);
    std::string serialise() const;

    const UserOverride* find_user_override(std::string_view unique_id) const;
    const ChannelOverride* find_channel_override(std::string_view server_uid,
                                                 std::uint64_t channel_id) const;

    /// Clamps every numeric field into its supported range, reporting each clamp.
    void clamp(ConfigDiagnostics& diag);
};

/// Runs migrate_v(n)→v(n+1) steps in sequence. A document newer than kConfigVersion is left
/// untouched and flagged, never downgraded.
bool migrate(json::Value& doc, ConfigDiagnostics& diag);

}  // namespace tsro

#endif  // TSRO_CONFIG_HPP
