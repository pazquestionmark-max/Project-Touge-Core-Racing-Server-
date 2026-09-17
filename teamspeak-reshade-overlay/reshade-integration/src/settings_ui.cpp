// SPDX-License-Identifier: MIT
#include "settings_ui.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>

#include <imgui.h>
// reshade.hpp must follow imgui.h: it supplies the inline definitions for ImGui:: and
// ImDrawList:: that route through ReShade's function table. imgui.h alone only declares them,
// so omitting this compiles cleanly and then fails at link with unresolved externals.
#include <reshade.hpp>

#include "icons.hpp"

namespace tsro::overlay {
namespace {

/// Every control is a small helper that reports whether it changed, so the caller can mark the
/// configuration dirty without each call site remembering to.
bool colour_edit(const char* label, Color& colour) {
    float rgba[4] = {static_cast<float>(colour.r) / 255.0f, static_cast<float>(colour.g) / 255.0f,
                     static_cast<float>(colour.b) / 255.0f,
                     static_cast<float>(colour.a) / 255.0f};
    // AlphaBar + HDR off + a hex field: the brief asks for both a picker and hexadecimal entry.
    const bool changed = ImGui::ColorEdit4(
        label, rgba,
        ImGuiColorEditFlags_AlphaBar | ImGuiColorEditFlags_AlphaPreviewHalf |
            ImGuiColorEditFlags_DisplayHex | ImGuiColorEditFlags_InputRGB);
    if (changed) {
        colour.r = static_cast<std::uint8_t>(rgba[0] * 255.0f + 0.5f);
        colour.g = static_cast<std::uint8_t>(rgba[1] * 255.0f + 0.5f);
        colour.b = static_cast<std::uint8_t>(rgba[2] * 255.0f + 0.5f);
        colour.a = static_cast<std::uint8_t>(rgba[3] * 255.0f + 0.5f);
    }
    return changed;
}

bool optional_colour_edit(const char* label, std::optional<Color>& colour, Color fallback) {
    bool enabled = colour.has_value();
    bool changed = false;
    ImGui::PushID(label);
    if (ImGui::Checkbox("##enabled", &enabled)) {
        if (enabled) colour = fallback;
        else colour.reset();
        changed = true;
    }
    ImGui::SameLine();
    if (enabled && colour.has_value()) {
        Color value = *colour;
        if (colour_edit(label, value)) {
            colour = value;
            changed = true;
        }
    } else {
        ImGui::BeginDisabled();
        Color shown = fallback;
        colour_edit(label, shown);
        ImGui::EndDisabled();
    }
    ImGui::PopID();
    return changed;
}

template <typename E, std::size_t N>
bool enum_combo(const char* label, E& value, const E (&values)[N]) {
    const char* current = to_string(value);
    bool changed = false;
    if (ImGui::BeginCombo(label, current)) {
        for (std::size_t i = 0; i < N; ++i) {
            const bool selected = values[i] == value;
            if (ImGui::Selectable(to_string(values[i]), selected)) {
                value = values[i];
                changed = true;
            }
            if (selected) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }
    return changed;
}

constexpr Anchor kAnchors[] = {Anchor::TopLeft,    Anchor::TopCenter,   Anchor::TopRight,
                               Anchor::CenterLeft, Anchor::Center,      Anchor::CenterRight,
                               Anchor::BottomLeft, Anchor::BottomCenter, Anchor::BottomRight};
constexpr Align kAligns[] = {Align::Left, Align::Center, Align::Right};
constexpr IconShape kIcons[] = {
    IconShape::None,       IconShape::Dot,        IconShape::Circle,
    IconShape::Ring,       IconShape::Square,     IconShape::Diamond,
    IconShape::Triangle,   IconShape::Star,       IconShape::Chevron,
    IconShape::Microphone, IconShape::MicrophoneMuted, IconShape::Speaker,
    IconShape::SpeakerMuted, IconShape::Moon,     IconShape::Record,
    IconShape::Crown,      IconShape::Whisper,    IconShape::Bars};
constexpr Easing kEasings[] = {Easing::Linear,       Easing::EaseIn,      Easing::EaseOut,
                               Easing::EaseInOut,    Easing::EaseOutBack, Easing::EaseOutElastic};
constexpr OverflowMode kOverflow[] = {OverflowMode::Clip, OverflowMode::Ellipsis,
                                      OverflowMode::Wrap, OverflowMode::Shrink,
                                      OverflowMode::Scroll};
constexpr UserSort kSorts[] = {UserSort::ChannelOrder, UserSort::Alphabetical,
                               UserSort::TalkPower, UserSort::SpeakingFirst};
constexpr SpeakingAnimation kSpeakAnims[] = {SpeakingAnimation::None, SpeakingAnimation::ColorFade,
                                             SpeakingAnimation::Pulse, SpeakingAnimation::Glow,
                                             SpeakingAnimation::BorderSweep};
constexpr ChatOrder kChatOrders[] = {ChatOrder::NewestBottom, ChatOrder::NewestTop};
constexpr StackDirection kStacks[] = {StackDirection::Down, StackDirection::Up};

void help(const char* text) {
    ImGui::SameLine();
    ImGui::TextDisabled("(?)");
    if (ImGui::IsItemHovered()) {
        ImGui::BeginTooltip();
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 28.0f);
        ImGui::TextUnformatted(text);
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }
}

bool placement_editor(const char* label, Placement& placement) {
    bool changed = false;
    ImGui::PushID(label);
    if (ImGui::TreeNodeEx(label, ImGuiTreeNodeFlags_DefaultOpen)) {
        changed |= ImGui::Checkbox("Visible", &placement.visible);
        changed |= enum_combo("Anchor", placement.anchor, kAnchors);
        help("The corner or edge the offsets below are measured from. The element keeps that "
             "relationship at any resolution or aspect ratio.");
        changed |= ImGui::Checkbox("Offsets are a fraction of the screen", &placement.percent);
        if (placement.percent) {
            changed |= ImGui::SliderFloat("X", &placement.x, 0.0f, 1.0f, "%.3f");
            changed |= ImGui::SliderFloat("Y", &placement.y, 0.0f, 1.0f, "%.3f");
        } else {
            changed |= ImGui::DragFloat("X", &placement.x, 1.0f, -8192.0f, 8192.0f, "%.0f px");
            changed |= ImGui::DragFloat("Y", &placement.y, 1.0f, -8192.0f, 8192.0f, "%.0f px");
        }
        changed |= enum_combo("Alignment", placement.align, kAligns);
        ImGui::TreePop();
    }
    ImGui::PopID();
    return changed;
}

bool fade_editor(const char* label, Fade& fade) {
    bool changed = false;
    ImGui::PushID(label);
    if (ImGui::TreeNode(label)) {
        changed |= ImGui::SliderInt("Fade in (ms)", &fade.in_ms, 0, 3000);
        changed |= ImGui::SliderInt("Visible for (ms)", &fade.hold_ms, 0, 30000);
        changed |= ImGui::SliderInt("Fade out (ms)", &fade.out_ms, 0, 3000);
        changed |= ImGui::SliderFloat("Start opacity", &fade.start_opacity, 0.0f, 1.0f);
        changed |= ImGui::SliderFloat("End opacity", &fade.end_opacity, 0.0f, 1.0f);
        changed |= enum_combo("Easing", fade.easing, kEasings);
        ImGui::TreePop();
    }
    ImGui::PopID();
    return changed;
}

bool state_style_editor(const char* label, StateStyle& style, const char* note) {
    bool changed = false;
    ImGui::PushID(label);
    if (ImGui::TreeNode(label)) {
        if (note != nullptr) {
            ImGui::PushTextWrapPos(ImGui::GetFontSize() * 34.0f);
            ImGui::TextDisabled("%s", note);
            ImGui::PopTextWrapPos();
        }
        changed |= ImGui::Checkbox("Enabled", &style.enabled);
        changed |= ImGui::Checkbox("Show icon", &style.show_icon);
        changed |= enum_combo("Icon", style.icon, kIcons);
        changed |= ImGui::SliderFloat("Icon size", &style.icon_scale, 0.2f, 4.0f, "x%.2f");
        changed |= colour_edit("Icon colour", style.icon_color);
        changed |= ImGui::Checkbox("Override the name colour", &style.override_text_color);
        if (style.override_text_color) changed |= colour_edit("Name colour", style.text_color);
        changed |= ImGui::Checkbox("Background", &style.show_background);
        if (style.show_background) changed |= colour_edit("Background colour", style.background);
        changed |= ImGui::Checkbox("Border", &style.show_border);
        if (style.show_border) {
            changed |= colour_edit("Border colour", style.border);
            changed |= ImGui::SliderFloat("Border thickness", &style.border_thickness, 0.0f, 8.0f);
        }
        changed |= ImGui::Checkbox("Glow", &style.glow);
        if (style.glow) {
            changed |= colour_edit("Glow colour", style.glow_color);
            changed |= ImGui::SliderFloat("Glow radius", &style.glow_radius, 0.0f, 40.0f);
        }
        changed |= ImGui::SliderFloat("Opacity", &style.opacity, 0.0f, 1.0f);
        changed |= ImGui::Checkbox("Dim the whole entry", &style.dim_entry);
        if (style.dim_entry) changed |= ImGui::SliderFloat("Dim amount", &style.dim_amount, 0.0f, 1.0f);
        ImGui::TreePop();
    }
    ImGui::PopID();
    return changed;
}

/// Simple mode: enable/disable, colour, icon. Everything else stays at its default and is
/// reachable by turning on "Show every setting".
bool compact_state_editor(const char* label, StateStyle& style, bool show_icon_picker) {
    bool changed = false;
    ImGui::PushID(label);
    changed |= ImGui::Checkbox("##on", &style.enabled);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 9.0f);
    if (style.override_text_color) {
        changed |= colour_edit("##colour", style.text_color);
    } else {
        changed |= colour_edit("##colour", style.icon_color);
    }
    ImGui::SameLine();
    if (show_icon_picker) {
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 8.0f);
        changed |= enum_combo("##icon", style.icon, kIcons);
        ImGui::SameLine();
    }
    ImGui::TextUnformatted(label);
    ImGui::PopID();
    return changed;
}

bool notification_editor(const char* label, NotificationStyle& style, const char* placeholders) {
    bool changed = false;
    ImGui::PushID(label);
    if (ImGui::TreeNode(label)) {
        changed |= ImGui::Checkbox("Enabled", &style.enabled);

        char format[201];
        std::snprintf(format, sizeof(format), "%s", style.format.c_str());
        if (ImGui::InputText("Text", format, sizeof(format))) {
            style.format = format;
            changed = true;
        }
        help(placeholders);

        char prefix[17];
        std::snprintf(prefix, sizeof(prefix), "%s", style.prefix.c_str());
        if (ImGui::InputText("Prefix", prefix, sizeof(prefix))) {
            style.prefix = prefix;
            changed = true;
        }

        changed |= enum_combo("Icon", style.icon, kIcons);
        changed |= colour_edit("Icon colour", style.icon_color);
        changed |= colour_edit("Text colour", style.text);
        changed |= colour_edit("Name colour", style.name_color);
        changed |= ImGui::Checkbox("Background", &style.show_background);
        if (style.show_background) changed |= colour_edit("Background colour", style.background);
        changed |= ImGui::Checkbox("Border", &style.show_border);
        if (style.show_border) changed |= colour_edit("Border colour", style.border);
        changed |= fade_editor("Timing", style.fade);

        changed |= ImGui::Checkbox("Play a sound", &style.sound);
        if (style.sound) {
            char path[261];
            std::snprintf(path, sizeof(path), "%s", style.sound_file.c_str());
            if (ImGui::InputText("Sound file (.wav)", path, sizeof(path))) {
                style.sound_file = path;
                changed = true;
            }
        }
        ImGui::TreePop();
    }
    ImGui::PopID();
    return changed;
}

}  // namespace

void SettingsUi::refresh_profiles(ProfileStore& profiles) {
    profiles_ = profiles.list();
    selected_profile_ = std::min(selected_profile_, static_cast<int>(profiles_.size()) - 1);
    if (selected_profile_ < 0) selected_profile_ = 0;
}

void SettingsUi::set_status(std::string message, bool error) {
    status_ = std::move(message);
    status_is_error_ = error;
}

void SettingsUi::tab_general(Config& config, SettingsActions& actions) {
    actions.config_changed |= ImGui::Checkbox("Overlay enabled", &config.general.enabled);
    actions.config_changed |=
        ImGui::SliderFloat("Overall opacity", &config.general.master_opacity, 0.0f, 1.0f);
    actions.config_changed |=
        ImGui::SliderFloat("Overall scale", &config.general.scale, 0.25f, 4.0f, "x%.2f");
    help("Multiplies every size: fonts, icons, row heights, padding and panel widths.");

    ImGui::SeparatorText("When there is nothing to show");
    actions.config_changed |= ImGui::Checkbox("Show while not connected to a TeamSpeak server",
                                              &config.general.show_when_disconnected);
    actions.config_changed |= ImGui::Checkbox("Show while the TeamSpeak plugin is unavailable",
                                              &config.general.show_when_plugin_unavailable);
    help("The plugin is unavailable when TeamSpeak is closed or the plugin is not loaded. "
         "Turning this off hides the overlay entirely in that case.");

    ImGui::SeparatorText("Preview");
    ImGui::Checkbox("Show the overlay using example data", &preview_active_);
    help("Draws the HUD from fixed sample users so you can see every indicator at once, plus one "
         "of every notification type and a sample chat feed, so you can see where each piece "
         "lands. It does not create TeamSpeak events and does not change your real state.");
    if (preview_active_) {
        ImGui::TextDisabled("Showing sample users, every notification type and a sample chat "
                            "feed. A notification type you have switched off simply does not "
                            "appear. The chat panel is shown even if you have it hidden, so you "
                            "can position it.");
    }

    ImGui::SeparatorText("Settings");
    if (ImGui::Checkbox("Show every setting", &config.general.advanced_settings)) {
        actions.config_changed = true;
    }
    help("Off by default. The everyday settings fit on a few tabs; turning this on reveals "
         "per-state styling, animation tuning, per-channel themes and connection internals.");
    if (!config.general.advanced_settings) {
        ImGui::TextDisabled("Advanced tabs are hidden. Nothing is lost -- your saved settings "
                            "are untouched whether they are shown or not.");
    }
}

void SettingsUi::tab_appearance(Config& config, SettingsActions& actions) {
    AppearanceConfig& a = config.appearance;
    const bool advanced = config.general.advanced_settings;

    ImGui::SeparatorText("Text");

    // No font list here, deliberately. Enumerating ReShade's atlas means reading ImFontAtlas
    // fields at offsets from this build's imgui.h, and ImFontAtlas is not in ReShade's function
    // table -- the layouts need not match, and assuming they did crashed the game. The typeface
    // comes from ReShade's own setting instead, which is safe and needs no guessing.
    ImGui::TextWrapped(
        "Typeface: whatever ReShade is set to use. Open ReShade's own Settings tab and point its "
        "font option at a .ttf -- the release ships Roboto and a few others in the 'fonts' "
        "folder next to this add-on. The overlay follows that choice automatically.");
    ImGui::TextDisabled(
        "The overlay cannot load a typeface independently: ReShade owns the font atlas and "
        "rebuilds it, and reaching into it from an add-on is not safe across ReShade versions.");
    ImGui::Spacing();

    actions.config_changed |= ImGui::SliderFloat("Size", &a.font_size, 6.0f, 72.0f, "%.0f px");
    actions.config_changed |= colour_edit("Text colour", a.text_default);
    actions.config_changed |= ImGui::Checkbox("Outline the text", &a.text_outline);
    help("Draws a full outline around every glyph. Costs a little more than a drop shadow but "
         "stays readable over any background, not just most of them.");
    if (a.text_outline) {
        actions.config_changed |= colour_edit("Outline colour", a.text_outline_color);
        actions.config_changed |=
            ImGui::SliderFloat("Outline thickness", &a.text_outline_thickness, 0.5f, 4.0f, "%.1f px");
    } else {
        actions.config_changed |= ImGui::Checkbox("Drop shadow", &a.text_shadow);
        help("Cheaper than an outline and usually enough. Ignored while Outline is on.");
    }

    ImGui::SeparatorText("Spacing");
    actions.config_changed |= ImGui::SliderFloat("Line height", &a.row_height, 8.0f, 60.0f, "%.0f px");
    actions.config_changed |= ImGui::SliderFloat("Gap between lines", &a.row_spacing, 0.0f, 24.0f, "%.0f px");
    actions.config_changed |= ImGui::SliderFloat("Icon size", &a.icon_size, 4.0f, 32.0f, "%.0f px");

    ImGui::SeparatorText("Background panel");
    actions.config_changed |= ImGui::Checkbox("Draw a panel behind the overlay", &a.show_panel_background);
    if (a.show_panel_background) {
        actions.config_changed |= colour_edit("Panel colour", a.panel_background);
        actions.config_changed |= ImGui::SliderFloat("Padding across", &a.padding_x, 0.0f, 40.0f, "%.0f px");
        actions.config_changed |= ImGui::SliderFloat("Padding down", &a.padding_y, 0.0f, 40.0f, "%.0f px");
        actions.config_changed |= ImGui::SliderFloat("Corner radius", &a.corner_radius, 0.0f, 24.0f);
    }

    if (!advanced) return;

    ImGui::SeparatorText("Advanced");
    actions.config_changed |= colour_edit("Secondary text", a.text_secondary);
    actions.config_changed |= colour_edit("Accent", a.accent);
    if (a.text_shadow) {
        actions.config_changed |= colour_edit("Shadow colour", a.text_shadow_color);
        actions.config_changed |=
            ImGui::SliderFloat("Shadow offset", &a.text_shadow_offset, 0.0f, 6.0f, "%.1f px");
    }
    actions.config_changed |= colour_edit("Panel border", a.panel_border);
    actions.config_changed |=
        ImGui::SliderFloat("Border thickness", &a.panel_border_thickness, 0.0f, 8.0f);
}

void SettingsUi::tab_layout(Config& config, SettingsActions& actions) {
    const bool advanced = config.general.advanced_settings;

    ImGui::SeparatorText("Channel title and user list");
    actions.config_changed |=
        ImGui::Checkbox("Keep them together as one block", &config.group.enabled);
    help("On: the title sits directly above the list and the pair moves and scales as one. "
         "Off: each is positioned and anchored on its own.");

    if (config.group.enabled) {
        GroupConfig& g = config.group;
        actions.config_changed |= enum_combo("Corner", g.anchor, kAnchors);
        actions.config_changed |= ImGui::DragFloat("Move across", &g.x, 1.0f, -8192.0f, 8192.0f, "%.0f px");
        actions.config_changed |= ImGui::DragFloat("Move down", &g.y, 1.0f, -8192.0f, 8192.0f, "%.0f px");
        actions.config_changed |= enum_combo("Align", g.align, kAligns);
        actions.config_changed |= ImGui::SliderFloat("Size (both)", &g.scale, 0.25f, 3.0f, "x%.2f");
        help("Resizes the title and the list together. The two sliders below adjust each one "
             "relative to this.");
        actions.config_changed |= ImGui::SliderFloat("Gap between them", &g.spacing, 0.0f, 80.0f, "%.0f px");

        ImGui::Spacing();
        actions.config_changed |=
            ImGui::SliderFloat("Title size only", &config.channel_title.scale, 0.25f, 3.0f, "x%.2f");
        actions.config_changed |=
            ImGui::SliderFloat("List size only", &config.user_list.scale, 0.25f, 3.0f, "x%.2f");
        ImGui::TextDisabled("Both are multiplied by Size (both) and by the overall scale.");
    } else {
        actions.config_changed |= placement_editor("Channel title position", config.channel_title.placement);
        actions.config_changed |=
            ImGui::SliderFloat("Title size", &config.channel_title.scale, 0.25f, 3.0f, "x%.2f");
        actions.config_changed |= placement_editor("User list position", config.user_list.placement);
        actions.config_changed |=
            ImGui::SliderFloat("List size", &config.user_list.scale, 0.25f, 3.0f, "x%.2f");
    }

    ImGui::SeparatorText("Channel title");
    ChannelTitleConfig& t = config.channel_title;
    actions.config_changed |= ImGui::Checkbox("Show the channel title", &t.placement.visible);
    actions.config_changed |= ImGui::Checkbox("Show the parent channel", &t.show_parent);
    actions.config_changed |= ImGui::Checkbox("Show the user count", &t.show_user_count);
    actions.config_changed |= colour_edit("Title colour", t.text);
    actions.config_changed |= ImGui::SliderFloat("Title weight", &t.font_scale, 0.4f, 3.0f, "x%.2f");
    if (advanced) {
        actions.config_changed |= ImGui::Checkbox("Show the server name", &t.show_server_name);
        actions.config_changed |= ImGui::Checkbox("Show the channel topic", &t.show_topic);
        actions.config_changed |= ImGui::SliderFloat("Title opacity", &t.opacity, 0.0f, 1.0f);
        actions.config_changed |= ImGui::Checkbox("Title background", &t.show_background);
        if (t.show_background) actions.config_changed |= colour_edit("Title background colour", t.background);
        actions.config_changed |= ImGui::Checkbox("Title border", &t.show_border);
        if (t.show_border) actions.config_changed |= colour_edit("Title border colour", t.border);
        actions.config_changed |= enum_combo("Title icon", t.icon, kIcons);
        actions.config_changed |= colour_edit("Title icon colour", t.icon_color);
        char buffer[201];
        std::snprintf(buffer, sizeof(buffer), "%s", t.format.c_str());
        if (ImGui::InputText("Format", buffer, sizeof(buffer))) {
            t.format = buffer;
            actions.config_changed = true;
        }
        help("Placeholders: {channel} {parent} {server} {count}");
        std::snprintf(buffer, sizeof(buffer), "%s", t.parent_format.c_str());
        if (ImGui::InputText("Format with parent", buffer, sizeof(buffer))) {
            t.parent_format = buffer;
            actions.config_changed = true;
        }
        char disconnected[121];
        std::snprintf(disconnected, sizeof(disconnected), "%s", t.disconnected_text.c_str());
        if (ImGui::InputText("Text when disconnected", disconnected, sizeof(disconnected))) {
            t.disconnected_text = disconnected;
            actions.config_changed = true;
        }
    }

    ImGui::SeparatorText("User list");
    UserListConfig& u = config.user_list;
    actions.config_changed |= ImGui::Checkbox("Show the user list", &u.placement.visible);
    actions.config_changed |= ImGui::Checkbox("Show yourself", &u.show_local_user);
    actions.config_changed |= ImGui::Checkbox("Show muted users", &u.show_muted_users);
    actions.config_changed |= ImGui::Checkbox("Move speakers to the top", &u.speaking_first);
    actions.config_changed |= enum_combo("Sort by", u.sort, kSorts);
    actions.config_changed |= ImGui::SliderInt("Maximum users shown", &u.max_visible_users, 1, 64);
    actions.config_changed |=
        ImGui::SliderFloat("Name width", &u.max_name_width, 40.0f, 800.0f, "%.0f px");
    actions.config_changed |= enum_combo("Long names", u.name_overflow, kOverflow);
    help("How a name that does not fit is handled: cut off, ellipsis, wrapped, shrunk, or scrolled.");
    if (advanced) {
        actions.config_changed |= ImGui::Checkbox("Highlight yourself", &u.highlight_local_user);
        if (u.highlight_local_user) actions.config_changed |= colour_edit("Your colour", u.local_user_color);
        if (u.name_overflow == OverflowMode::Shrink) {
            actions.config_changed |=
                ImGui::SliderFloat("Smallest size", &u.min_font_scale, 0.3f, 1.0f, "x%.2f");
        }
        actions.config_changed |= ImGui::Checkbox("Show a '+N more' line", &u.show_overflow_count);
        actions.config_changed |=
            ImGui::SliderFloat("Gap around icons", &u.indicator_gap, 0.0f, 32.0f, "%.0f px");
    }

    ImGui::SeparatorText("Notifications");
    actions.config_changed |= placement_editor("Notification position", config.notifications.placement);
    actions.config_changed |= enum_combo("Stack direction", config.notifications.stack, kStacks);
    actions.config_changed |= ImGui::SliderInt("Maximum on screen", &config.notifications.max_visible, 1, 20);
    actions.config_changed |=
        ImGui::SliderFloat("Width", &config.notifications.width, 120.0f, 900.0f, "%.0f px");
    actions.config_changed |=
        ImGui::SliderFloat("Spacing", &config.notifications.spacing, 0.0f, 40.0f, "%.0f px");

    ImGui::SeparatorText("Chat");
    actions.config_changed |= placement_editor("Chat position", config.chat.placement);
    actions.config_changed |= ImGui::SliderFloat("Chat width", &config.chat.width, 120.0f, 1400.0f, "%.0f px");
}

void SettingsUi::tab_users(Config& config, const OverlayFrame& frame, SettingsActions& actions) {
    ImGui::TextDisabled(
        "Per-user settings are keyed on the TeamSpeak identity, so they survive nickname "
        "changes and reconnects. Two people cannot collide on one entry.");

    ImGui::SeparatorText("Friends");
    ImGui::TextWrapped(
        "Friends come from TeamSpeak's own Contacts list. There is no friend call anywhere in "
        "the plugin API, so the plugin reads the client's settings database directly -- "
        "read-only, and never while TeamSpeak holds a lock. Anyone you add in TeamSpeak's "
        "Contacts dialog appears here without being entered twice; the list below is for "
        "people TeamSpeak does not know about, and anything set there wins.");
    actions.config_changed |=
        ImGui::Checkbox("Use TeamSpeak's Contacts list", &config.user_list.use_teamspeak_friends);
    actions.config_changed |=
        ImGui::Checkbox("Colour friends differently", &config.user_list.color_friends);
    if (config.user_list.color_friends) {
        actions.config_changed |= colour_edit("Friend colour", config.user_list.friend_color);
        ImGui::TextDisabled("A friend's own name colour, if you set one, still wins.");
    }
    actions.config_changed |=
        ImGui::Checkbox("Show the friend's nickname as [Name]", &config.user_list.show_friend_tag);
    if (config.user_list.show_friend_tag) {
        actions.config_changed |= colour_edit("Tag colour", config.user_list.friend_tag_color);
    }

    ImGui::SeparatorText("People in your channel");
    if (frame.state.users.empty()) {
        ImGui::TextDisabled("Nobody visible. Connect TeamSpeak and join a channel to add entries "
                            "with one click.");
    }
    for (const UserState& user : frame.state.users) {
        ImGui::PushID(user.unique_id.c_str());
        ImGui::TextUnformatted(user.display_name.c_str());
        ImGui::SameLine();
        const bool exists = config.user_overrides.count(user.unique_id) != 0;
        if (exists) {
            ImGui::TextDisabled("(customised)");
        } else if (ImGui::SmallButton("Customise")) {
            UserOverride created;
            created.name_color = config.appearance.text_default;
            created.note = user.display_name;
            config.user_overrides[user.unique_id] = created;
            actions.config_changed = true;
        }
        if (!exists) {
            ImGui::SameLine();
            if (ImGui::SmallButton("Add as friend")) {
                UserOverride created;
                created.is_friend = true;
                created.friend_tag = user.display_name;
                created.note = user.display_name;
                config.user_overrides[user.unique_id] = created;
                actions.config_changed = true;
            }
        }
        ImGui::PopID();
    }

    ImGui::SeparatorText("Customised users");
    ImGui::InputText("Filter", user_filter_, sizeof(user_filter_));
    std::string to_erase;
    for (auto& [unique_id, override_entry] : config.user_overrides) {
        const std::string haystack = unique_id + " " + override_entry.note + " " +
                                     override_entry.display_override;
        if (user_filter_[0] != '\0' && haystack.find(user_filter_) == std::string::npos) continue;

        ImGui::PushID(unique_id.c_str());
        const std::string label =
            override_entry.note.empty() ? unique_id : override_entry.note + "  (" + unique_id + ")";
        if (ImGui::TreeNode(label.c_str())) {
            actions.config_changed |= ImGui::Checkbox("Enabled", &override_entry.enabled);
            actions.config_changed |= ImGui::Checkbox("Friend", &override_entry.is_friend);
            if (override_entry.is_friend) {
                char tag[33];
                std::snprintf(tag, sizeof(tag), "%s", override_entry.friend_tag.c_str());
                if (ImGui::InputText("Friend nickname", tag, sizeof(tag))) {
                    override_entry.friend_tag = tag;
                    actions.config_changed = true;
                }
                help("Shown as [Friend nickname] in front of their TeamSpeak name.");
            }

            char note[201];
            std::snprintf(note, sizeof(note), "%s", override_entry.note.c_str());
            if (ImGui::InputText("Label (yours, not shown in game)", note, sizeof(note))) {
                override_entry.note = note;
                actions.config_changed = true;
            }
            char display[65];
            std::snprintf(display, sizeof(display), "%s", override_entry.display_override.c_str());
            if (ImGui::InputText("Show this name instead", display, sizeof(display))) {
                override_entry.display_override = display;
                actions.config_changed = true;
            }

            actions.config_changed |=
                optional_colour_edit("Name colour", override_entry.name_color,
                                     config.appearance.text_default);
            actions.config_changed |=
                optional_colour_edit("Speaking colour", override_entry.speaking_color,
                                     config.indicators.speaking.text_color);
            actions.config_changed |=
                optional_colour_edit("Muted colour", override_entry.muted_color,
                                     config.indicators.mic_muted.text_color);
            actions.config_changed |=
                optional_colour_edit("Channel Commander colour", override_entry.commander_color,
                                     config.indicators.commander.icon_color);

            bool has_icon = override_entry.icon.has_value();
            if (ImGui::Checkbox("Custom icon", &has_icon)) {
                if (has_icon) override_entry.icon = IconShape::Star;
                else override_entry.icon.reset();
                actions.config_changed = true;
            }
            if (override_entry.icon.has_value()) {
                actions.config_changed |= enum_combo("Icon", *override_entry.icon, kIcons);
                actions.config_changed |=
                    optional_colour_edit("Icon colour", override_entry.icon_color,
                                         config.appearance.accent);
            }

            if (ImGui::Button("Reset this user")) {
                to_erase = unique_id;
            }
            ImGui::TreePop();
        }
        ImGui::PopID();
    }
    if (!to_erase.empty()) {
        config.user_overrides.erase(to_erase);
        actions.config_changed = true;
    }
    if (!config.user_overrides.empty() && ImGui::Button("Reset all user customisations")) {
        config.user_overrides.clear();
        actions.config_changed = true;
    }
}

void SettingsUi::tab_channels(Config& config, const OverlayFrame& frame,
                              SettingsActions& actions) {
    ImGui::TextDisabled(
        "Per-channel settings are keyed on the server identity plus the channel id, so the same "
        "channel number on two servers keeps two separate themes.");

    if (frame.state.channel.valid() && !frame.state.server.unique_id.empty()) {
        const std::string key =
            make_channel_key(frame.state.server.unique_id, frame.state.channel.id);
        ImGui::Text("Current channel: %s", frame.state.channel.name.c_str());
        if (config.channel_overrides.count(key) == 0) {
            ImGui::SameLine();
            if (ImGui::SmallButton("Customise this channel")) {
                ChannelOverride created;
                created.title_color = config.channel_title.text;
                config.channel_overrides[key] = created;
                actions.config_changed = true;
            }
        }
    } else {
        ImGui::TextDisabled("Not currently in a channel.");
    }

    ImGui::SeparatorText("Customised channels");
    std::string to_erase;
    for (auto& [key, override_entry] : config.channel_overrides) {
        ImGui::PushID(key.c_str());
        if (ImGui::TreeNode(key.c_str())) {
            actions.config_changed |= ImGui::Checkbox("Enabled", &override_entry.enabled);
            char display[65];
            std::snprintf(display, sizeof(display), "%s", override_entry.display_override.c_str());
            if (ImGui::InputText("Show this name instead", display, sizeof(display))) {
                override_entry.display_override = display;
                actions.config_changed = true;
            }
            actions.config_changed |= optional_colour_edit("Title colour", override_entry.title_color,
                                                           config.channel_title.text);
            actions.config_changed |= optional_colour_edit("Background", override_entry.background,
                                                           config.appearance.panel_background);
            actions.config_changed |= optional_colour_edit("Border", override_entry.border,
                                                           config.appearance.panel_border);
            actions.config_changed |= optional_colour_edit("User list colour",
                                                           override_entry.user_list_color,
                                                           config.appearance.text_default);
            bool has_icon = override_entry.icon.has_value();
            if (ImGui::Checkbox("Channel icon", &has_icon)) {
                if (has_icon) override_entry.icon = IconShape::Diamond;
                else override_entry.icon.reset();
                actions.config_changed = true;
            }
            if (override_entry.icon.has_value()) {
                actions.config_changed |= enum_combo("Icon", *override_entry.icon, kIcons);
                actions.config_changed |= optional_colour_edit("Icon colour",
                                                               override_entry.icon_color,
                                                               config.appearance.accent);
            }

            float scale = override_entry.font_scale.value_or(1.0f);
            bool has_scale = override_entry.font_scale.has_value();
            if (ImGui::Checkbox("Custom title size", &has_scale)) {
                if (has_scale) override_entry.font_scale = scale;
                else override_entry.font_scale.reset();
                actions.config_changed = true;
            }
            if (override_entry.font_scale.has_value()) {
                if (ImGui::SliderFloat("Title size", &scale, 0.4f, 3.0f, "x%.2f")) {
                    override_entry.font_scale = scale;
                    actions.config_changed = true;
                }
            }

            if (ImGui::Button("Reset this channel")) to_erase = key;
            ImGui::TreePop();
        }
        ImGui::PopID();
    }
    if (!to_erase.empty()) {
        config.channel_overrides.erase(to_erase);
        actions.config_changed = true;
    }
    if (!config.channel_overrides.empty() && ImGui::Button("Reset all channel customisations")) {
        config.channel_overrides.clear();
        actions.config_changed = true;
    }
}

void SettingsUi::tab_indicators(Config& config, SettingsActions& actions) {
    IndicatorsConfig& i = config.indicators;

    if (!config.general.advanced_settings) {
        ImGui::TextDisabled("On / colour / icon for each state. Turn on \"Show every setting\" "
                            "in General for borders, glow, dimming and backgrounds.");
        ImGui::Spacing();
        actions.config_changed |= compact_state_editor("Speaking", i.speaking, false);
        actions.config_changed |= compact_state_editor("Whispering to you", i.whispering, true);
        actions.config_changed |= compact_state_editor("Microphone muted", i.mic_muted, true);
        actions.config_changed |= compact_state_editor("Speakers muted", i.speaker_muted, true);

        ImGui::Spacing();
        ImGui::SeparatorText("Channel Commander");
        actions.config_changed |= ImGui::Checkbox("Show Channel Commander", &i.commander.enabled);
        if (i.commander.enabled) {
            // Circle, name colour, or both -- asked for explicitly, and clearer as one choice
            // than as two unrelated checkboxes.
            int mode = i.commander.show_icon ? (i.commander.override_text_color ? 2 : 0) : 1;
            static const char* kModes[] = {"Circle beside the name", "Colour the name",
                                           "Circle and colour"};
            if (ImGui::Combo("How to show it", &mode, kModes, 3)) {
                i.commander.show_icon = (mode != 1);
                i.commander.override_text_color = (mode != 0);
                actions.config_changed = true;
            }
            if (i.commander.show_icon) {
                actions.config_changed |= enum_combo("Shape", i.commander.icon, kIcons);
                actions.config_changed |= ImGui::SliderFloat("Circle size", &i.commander.icon_scale,
                                                             0.2f, 3.0f, "x%.2f");
                actions.config_changed |= colour_edit("Circle colour", i.commander.icon_color);
            }
            if (i.commander.override_text_color) {
                actions.config_changed |= colour_edit("Name colour", i.commander.text_color);
            }
        }
        ImGui::Spacing();
        actions.config_changed |= compact_state_editor("Away", i.away, true);
        actions.config_changed |= compact_state_editor("Recording", i.recording, true);
        ImGui::Spacing();
        ImGui::TextWrapped(
            "Microphone mute and speaker mute are separate states. Give them different icons and "
            "colours or you will not be able to tell them apart at a glance.");
        return;
    }

    ImGui::TextDisabled("Each state has its own independent appearance.");

    ImGui::SeparatorText("Voice");
    actions.config_changed |= state_style_editor("Speaking", i.speaking, nullptr);
    actions.config_changed |= state_style_editor(
        "Whispering to you", i.whispering,
        "TeamSpeak reports that someone's transmission reached you as a whisper. It does not "
        "expose whisper targets, whispers between other people, or your own outgoing whispers.");

    ImGui::SeparatorText("Muting");
    actions.config_changed |= state_style_editor(
        "Microphone muted", i.mic_muted,
        "Their microphone is muted. Separate from speaker mute: do not give these two the same "
        "icon and colour or you will not be able to tell them apart.");
    actions.config_changed |= state_style_editor(
        "Speakers muted", i.speaker_muted,
        "Their speakers are muted; their microphone may still be live.");
    actions.config_changed |= state_style_editor(
        "No capture device", i.mic_hardware_off,
        "TeamSpeak reports no open capture device for this user.");
    actions.config_changed |= state_style_editor(
        "Muted by you", i.locally_muted, "You have muted this user locally.");

    ImGui::SeparatorText("Status");
    actions.config_changed |= state_style_editor("Away", i.away, nullptr);
    actions.config_changed |= state_style_editor("Recording", i.recording, nullptr);
    actions.config_changed |= state_style_editor(
        "Channel Commander", i.commander,
        "Shown immediately before the name. The default is a small orange dot; both the colour "
        "and the shape can be changed here, and per user on the Users tab.");
    actions.config_changed |= state_style_editor("Priority speaker", i.priority_speaker, nullptr);
    actions.config_changed |= state_style_editor(
        "Suppressed", i.suppressed,
        "Derived from talk power: the user cannot currently transmit in a moderated channel.");
}

void SettingsUi::tab_notifications(Config& config, SettingsActions& actions) {
    NotificationsConfig& n = config.notifications;
    actions.config_changed |= ImGui::Checkbox("Merge identical notifications", &n.merge_duplicates);
    actions.config_changed |= ImGui::SliderInt("Ignore joins for (ms) after connecting",
                                               &n.suppress_after_connect_ms, 0, 15000);
    ImGui::SeparatorText("Box");
    // The same controls the basic view has. They were missing here, which is why the only view
    // that offers every other setting had no way to widen a toast.
    section_notification_box(config, actions);
    actions.config_changed |=
        ImGui::SliderFloat("Minimum height", &n.min_height, 12.0f, 120.0f, "%.0f px");
    ImGui::TextDisabled(
        "An event line takes the width it needs and grows towards the anchored edge. Only a "
        "chat or private message wraps, and each of those has its own switch below.");

    actions.config_changed |= notification_editor("Someone joins", n.join,
                                                  "Placeholders: {name} {channel} {count}");
    actions.config_changed |= notification_editor("Someone leaves", n.leave,
                                                  "Placeholders: {name} {channel} {count}");
    actions.config_changed |= notification_editor("You change channel", n.channel_switch,
                                                  "Placeholders: {channel} {previous} {count}");
    actions.config_changed |= notification_editor("Connection events", n.connection,
                                                  "Placeholders: {status} {server}");
    actions.config_changed |= notification_editor("Someone whispers you", n.whisper,
                                                  "Placeholders: {name}");
    actions.config_changed |= notification_editor("Chat message", n.chat,
                                                  "Placeholders: {name} {message} {channel}");
    actions.config_changed |= notification_editor("Private message", n.private_chat,
                                                  "Placeholders: {name} {message}");
}

void SettingsUi::tab_chat(Config& config, SettingsActions& actions) {
    ChatConfig& c = config.chat;

    // Visibility first: with it off nothing is drawn *and* nothing is even requested from the
    // plugin, which makes the whole tab look broken rather than switched off.
    if (ImGui::Checkbox("Show the chat feed", &c.placement.visible)) {
        actions.config_changed = true;
        actions.subscription_changed = true;
    }
    if (!c.placement.visible) {
        ImGui::TextDisabled("The chat feed is hidden. While it is off, TeamSpeak is not asked to "
                            "send messages at all.");
    } else {
        actions.config_changed |= placement_editor("Chat position", c.placement);
    }
    ImGui::SeparatorText("What to show");
    ImGui::TextDisabled(
        "Messages are only sent to the overlay for the categories enabled here. A category that "
        "is off is filtered inside TeamSpeak and never crosses the connection.");

    bool changed = false;
    changed |= ImGui::Checkbox("Channel messages", &c.show_channel_messages);
    changed |= ImGui::Checkbox("Server messages", &c.show_server_messages);
    changed |= ImGui::Checkbox("Private messages", &c.show_private_messages);
    help("Off by default. Private messages will be visible to anyone who can see your screen or "
         "your stream.");
    if (c.show_private_messages) {
        ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.3f, 1.0f),
                           "Private messages will be drawn on screen and captured by recording "
                           "and streaming software.");
    }
    if (changed) {
        actions.config_changed = true;
        actions.subscription_changed = true;  // the plugin must be told what to send
    }

    ImGui::SeparatorText("Presentation");
    actions.config_changed |= enum_combo("Order", c.order, kChatOrders);
    actions.config_changed |= ImGui::SliderInt("Messages shown", &c.max_visible_messages, 1, 30);
    actions.config_changed |= ImGui::SliderInt("Messages kept", &c.history_size, 1, 200);
    actions.config_changed |= ImGui::SliderInt("Maximum message length", &c.max_message_length, 20, 1024);
    actions.config_changed |= ImGui::SliderInt("Forget after (seconds)", &c.retention_seconds, 0, 3600);
    help("0 keeps messages until they are pushed out by newer ones.");
    actions.config_changed |= ImGui::Checkbox("Timestamps", &c.show_timestamp);
    actions.config_changed |= ImGui::Checkbox("Sender name", &c.show_sender);
    actions.config_changed |= ImGui::Checkbox("Channel name", &c.show_channel_name);
    actions.config_changed |= ImGui::Checkbox("Message type icon", &c.show_category_icon);
    actions.config_changed |= ImGui::Checkbox("Wrap long messages", &c.wrap);
    actions.config_changed |= ImGui::Checkbox("Use each sender's colour", &c.use_sender_color);
    actions.config_changed |= ImGui::SliderFloat("Text size", &c.font_scale, 0.4f, 2.0f, "x%.2f");
    actions.config_changed |= colour_edit("Message colour", c.text);
    actions.config_changed |= colour_edit("Sender colour", c.sender);
    actions.config_changed |= colour_edit("Timestamp colour", c.timestamp);
    actions.config_changed |= ImGui::Checkbox("Background", &c.show_background);
    if (c.show_background) actions.config_changed |= colour_edit("Background colour", c.background);
}

void SettingsUi::tab_animation(Config& config, SettingsActions& actions) {
    AnimationConfig& a = config.animation;
    actions.config_changed |= ImGui::Checkbox("Animations enabled", &a.enabled);

    ImGui::SeparatorText("Speaking");
    actions.config_changed |= enum_combo("Style", a.speaking, kSpeakAnims);
    actions.config_changed |= ImGui::SliderInt("Ramp up (ms)", &a.speaking_attack_ms, 0, 1000);
    actions.config_changed |= ImGui::SliderInt("Ramp down (ms)", &a.speaking_release_ms, 0, 2000);
    help("A short ramp keeps brief bursts visible and stops the indicator flickering.");
    if (a.speaking == SpeakingAnimation::Pulse) {
        actions.config_changed |= ImGui::SliderFloat("Pulse rate (Hz)", &a.speaking_pulse_hz, 0.2f, 8.0f);
        actions.config_changed |= ImGui::SliderFloat("Pulse depth", &a.speaking_pulse_depth, 0.0f, 1.0f);
    }

    ImGui::SeparatorText("State changes");
    actions.config_changed |= enum_combo("Easing", a.state_easing, kEasings);
    actions.config_changed |= ImGui::SliderInt("Transition (ms)", &a.state_transition_ms, 0, 2000);
    actions.config_changed |= ImGui::Checkbox("Animate list reordering", &a.animate_list_reorder);
    actions.config_changed |= ImGui::SliderInt("Reorder (ms)", &a.list_reorder_ms, 0, 2000);

    ImGui::SeparatorText("Idle");
    actions.config_changed |= ImGui::Checkbox("Fade the overlay when nothing happens", &a.fade_when_idle);
    if (a.fade_when_idle) {
        actions.config_changed |= ImGui::SliderInt("Idle after (ms)", &a.idle_after_ms, 1000, 120000);
        actions.config_changed |= ImGui::SliderFloat("Idle opacity", &a.idle_opacity, 0.0f, 1.0f);
    }
    actions.config_changed |= fade_editor("Overlay fade", a.overlay_fade);
}

void SettingsUi::tab_integration(Config& config, SettingsActions& actions) {
    IntegrationConfig& i = config.integration;
    ImGui::TextDisabled(
        "The overlay talks to the TeamSpeak plugin over a local named pipe restricted to your "
        "Windows account. No network connection is made and nothing leaves this machine.");

    char pipe[201];
    std::snprintf(pipe, sizeof(pipe), "%s", i.pipe_name.c_str());
    if (ImGui::InputText("Pipe name override", pipe, sizeof(pipe))) {
        i.pipe_name = pipe;
        actions.config_changed = true;
    }
    help("Leave empty for the default, which includes your account's identifier so two users on "
         "one machine do not collide. Only change this if support asks you to.");

    actions.config_changed |= ImGui::Checkbox("Connect automatically", &i.auto_connect);
    actions.config_changed |= ImGui::SliderInt("First retry after (ms)", &i.reconnect_initial_ms, 50, 5000);
    actions.config_changed |= ImGui::SliderInt("Longest retry interval (ms)", &i.reconnect_max_ms, 200, 60000);
    actions.config_changed |= ImGui::SliderInt("Treat as stale after (ms)", &i.stale_after_ms, 1000, 60000);
    help("If no message arrives for this long, the overlay stops presenting the user list as "
         "live and asks the plugin to resend it.");
    actions.config_changed |= ImGui::SliderInt("Latency check every (ms)", &i.ping_interval_ms, 1000, 120000);

    if (ImGui::Button("Reconnect now")) actions.reconnect_requested = true;
}

void SettingsUi::tab_profiles(Config& config, ProfileStore& profiles, SettingsActions& actions) {
    ImGui::Text("Configuration folder: %s", profiles.root().c_str());
    ImGui::SeparatorText("Profiles");

    if (profiles_.empty()) refresh_profiles(profiles);
    std::vector<const char*> names;
    names.reserve(profiles_.size());
    for (const ProfileInfo& info : profiles_) names.push_back(info.name.c_str());
    if (!names.empty()) {
        ImGui::Combo("Saved profiles", &selected_profile_, names.data(),
                     static_cast<int>(names.size()));
        ImGui::SameLine();
        if (ImGui::Button("Load")) {
            actions.switch_to_profile = profiles_[static_cast<std::size_t>(selected_profile_)].name;
        }
    } else {
        ImGui::TextDisabled("No profiles saved yet.");
    }

    ImGui::Text("Current profile: %s", config.general.profile_name.c_str());
    if (ImGui::Button("Save")) actions.save_requested = true;
    ImGui::SameLine();
    if (ImGui::Button("Reload from disk")) actions.reload_requested = true;
    ImGui::SameLine();
    if (ImGui::Button("Reset everything to defaults")) {
        const std::string keep = config.general.profile_name;
        config = Config::defaults();
        config.general.profile_name = keep;
        actions.config_changed = true;
        actions.subscription_changed = true;
        set_status("Settings reset to defaults. Save to keep this.", false);
    }

    ImGui::SeparatorText("Create");
    ImGui::InputText("New profile name", new_profile_name_, sizeof(new_profile_name_));
    ImGui::SameLine();
    if (ImGui::Button("Save as")) {
        const std::string name = ProfileStore::sanitise_profile_name(new_profile_name_);
        if (name.empty()) {
            set_status("That is not a usable profile name.", true);
        } else {
            std::string error;
            Config copy = config;
            copy.general.profile_name = name;
            if (profiles.save(name, copy, error)) {
                config.general.profile_name = name;
                refresh_profiles(profiles);
                set_status("Saved profile '" + name + "'.", false);
                new_profile_name_[0] = '\0';
            } else {
                set_status(error, true);
            }
        }
    }

    if (!profiles_.empty()) {
        ImGui::SameLine();
        if (ImGui::Button("Delete selected")) {
            std::string error;
            const std::string name = profiles_[static_cast<std::size_t>(selected_profile_)].name;
            if (profiles.remove(name, error)) {
                refresh_profiles(profiles);
                set_status("Deleted profile '" + name + "'.", false);
            } else {
                set_status(error, true);
            }
        }
    }

    ImGui::SeparatorText("Share");
    ImGui::InputText("Export to", export_path_, sizeof(export_path_));
    ImGui::SameLine();
    if (ImGui::Button("Export")) {
        std::string error;
        if (export_path_[0] == '\0') {
            set_status("Enter a full path to export to.", true);
        } else if (profiles.export_to(export_path_, config, error)) {
            set_status("Exported to " + std::string(export_path_), false);
        } else {
            set_status(error, true);
        }
    }
    ImGui::InputText("Import from", import_path_, sizeof(import_path_));
    ImGui::SameLine();
    if (ImGui::Button("Import")) {
        ConfigDiagnostics diagnostics;
        bool ok = false;
        Config imported = profiles.import_from(import_path_, diagnostics, ok);
        if (ok) {
            imported.general.profile_name = config.general.profile_name;
            config = std::move(imported);
            actions.config_changed = true;
            actions.subscription_changed = true;
            set_status("Imported. Save to keep this.", false);
        } else {
            set_status("Could not import that file.", true);
        }
    }

    ImGui::SeparatorText("Per-game profiles");
    actions.config_changed |= ImGui::Checkbox("Pick a profile automatically for each game",
                                              &config.general.auto_profile_by_executable);
    const std::string current_exe = current_executable_name();
    ImGui::Text("This game: %s", current_exe.empty() ? "(unknown)" : current_exe.c_str());
    if (!current_exe.empty() && !profiles_.empty()) {
        ImGui::SameLine();
        if (ImGui::Button("Use the selected profile here")) {
            std::string error;
            const std::string name = profiles_[static_cast<std::size_t>(selected_profile_)].name;
            if (profiles.map_executable(current_exe, name, error)) {
                set_status(current_exe + " will use '" + name + "'.", false);
            } else {
                set_status(error, true);
            }
        }
    }
    for (const auto& [executable, profile] : profiles.executable_mappings()) {
        ImGui::PushID(executable.c_str());
        ImGui::BulletText("%s -> %s", executable.c_str(), profile.c_str());
        ImGui::SameLine();
        if (ImGui::SmallButton("Remove")) {
            std::string error;
            profiles.unmap_executable(executable, error);
        }
        ImGui::PopID();
    }

    if (!status_.empty()) {
        ImGui::Separator();
        ImGui::TextColored(status_is_error_ ? ImVec4(1.0f, 0.45f, 0.45f, 1.0f)
                                            : ImVec4(0.55f, 0.9f, 0.6f, 1.0f),
                           "%s", status_.c_str());
    }
}

void SettingsUi::tab_diagnostics(const LinkDiagnostics& diagnostics, const OverlayFrame& frame,
                                 const FrameStats& stats,
                                 const ConfigDiagnostics& config_diagnostics, Config& config,
                                 SettingsActions& actions) {
    ImGui::SeparatorText("TeamSpeak plugin connection");
    ImGui::Text("Status: %s", to_string(diagnostics.state));
    if (!diagnostics.detail.empty()) ImGui::Text("Detail: %s", diagnostics.detail.c_str());
    ImGui::Text("Endpoint: %s", diagnostics.endpoint.c_str());
    if (diagnostics.state != LinkState::Connected) {
        ImGui::TextDisabled("Retry attempts: %d, next in %d ms", diagnostics.reconnect_attempts,
                            diagnostics.next_retry_in_ms);
        ImGui::TextWrapped(
            "If this stays disconnected: TeamSpeak must be running with the overlay plugin "
            "enabled (Tools > Options > Addons > Plugins). See docs/troubleshooting.md.");
    }
    if (ImGui::Button("Reconnect")) actions.reconnect_requested = true;

    ImGui::SeparatorText("Plugin");
    ImGui::Text("Plugin version: %s",
                diagnostics.plugin_version.empty() ? "(unknown)"
                                                   : diagnostics.plugin_version.c_str());
    ImGui::Text("TeamSpeak plugin API: %d", diagnostics.plugin_api_version);
    ImGui::Text("Protocol version: %d (this build speaks %d)", diagnostics.protocol_version,
                proto::kProtocolVersion);
    if (diagnostics.round_trip_ms >= 0) {
        ImGui::Text("Round trip: %lld ms", static_cast<long long>(diagnostics.round_trip_ms));
    } else {
        ImGui::TextDisabled("Round trip: not measured yet");
    }

    ImGui::SeparatorText("What this plugin can report");
    if (diagnostics.capabilities.empty()) {
        ImGui::TextDisabled("Not known until the plugin connects.");
    } else {
        for (const std::string& capability : diagnostics.capabilities) {
            ImGui::BulletText("%s", capability.c_str());
        }
        ImGui::TextWrapped(
            "Anything not listed is not exposed by the TeamSpeak plugin API, and the matching "
            "indicator stays hidden rather than showing a guess.");
    }

    ImGui::SeparatorText("Messages");
    ImGui::Text("Applied: %llu", static_cast<unsigned long long>(diagnostics.store.applied));
    ImGui::Text("Last message: %s", diagnostics.store.last_message_type.empty()
                                        ? "(none)"
                                        : diagnostics.store.last_message_type.c_str());
    ImGui::Text("Snapshots: %llu", static_cast<unsigned long long>(diagnostics.store.snapshots));
    ImGui::Text("Duplicates dropped: %llu",
                static_cast<unsigned long long>(diagnostics.store.duplicates_dropped));
    ImGui::Text("Sequence gaps: %llu",
                static_cast<unsigned long long>(diagnostics.store.gaps_detected));
    ImGui::Text("Malformed payloads: %llu",
                static_cast<unsigned long long>(diagnostics.store.malformed_payloads));
    ImGui::Text("Unknown message types: %llu",
                static_cast<unsigned long long>(diagnostics.store.unknown_types));

    ImGui::SeparatorText("State");
    ImGui::Text("Server: %s", frame.state.server.name.empty() ? "(none)"
                                                              : frame.state.server.name.c_str());
    ImGui::Text("Channel: %s", frame.state.channel.valid() ? frame.state.channel.name.c_str()
                                                           : "(none)");
    ImGui::Text("Users: %zu", frame.state.users.size());
    ImGui::Text("Synchronised: %s", frame.state.synchronised ? "yes" : "no");
    ImGui::Text("Stale: %s", frame.state.stale ? "yes" : "no");
    if (frame.state.stale) {
        ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.3f, 1.0f),
                           "No recent update from TeamSpeak; the user list is not being shown as "
                           "live.");
    }

    // Friends have now been reported broken twice from a screenshot alone, which is not enough
    // to tell "TeamSpeak never said they were a friend" apart from "we were told and did not
    // colour it". This says which, per person, in one place.
    ImGui::SeparatorText("Friends");
    if (frame.state.users.empty()) {
        ImGui::TextDisabled("Nobody in the channel.");
    } else {
        ImGui::Text("Colour friends differently: %s",
                    config.user_list.color_friends ? "on" : "OFF");
        ImGui::Text("Use TeamSpeak's Contacts: %s",
                    config.user_list.use_teamspeak_friends ? "on" : "OFF");
        if (ImGui::InputInt("Friend= value meaning friend",
                            &config.user_list.teamspeak_friend_value)) {
            actions.config_changed = true;
        }
        help("TeamSpeak stores the three contact states as a number and documents which is "
             "which nowhere. 2 is Friend, matching the order its own Contacts dialog lists "
             "them: Neutral, Blocked, Friend. If a client ever renumbers them, the raw value "
             "printed beside each person below is what to put here.");
        for (const UserState& user : frame.state.users) {
            const char* reported = !user.is_friend.has_value() ? "unknown"
                                                               : (*user.is_friend ? "friend"
                                                                                  : "not a friend");
            char raw[32] = "none";
            if (user.contact_flag.has_value()) {
                std::snprintf(raw, sizeof(raw), "%d", *user.contact_flag);
            }
            ImGui::Text("%s -- TeamSpeak says: %s (Friend=%s)%s%s", user.display_name.c_str(),
                        reported, raw, user.friend_nickname.empty() ? "" : ", nickname: ",
                        user.friend_nickname.c_str());
        }
        ImGui::TextDisabled(
            "\"unknown\" means the contact list could not be read at all -- the plugin's own "
            "line above says why.");
    }

    ImGui::SeparatorText("Rendering");
    ImGui::Text("Layout: %.3f ms", static_cast<double>(stats.last_layout_ms));
    ImGui::Text("Draw: %.3f ms", static_cast<double>(stats.last_draw_ms));
    ImGui::Text("Frames drawn: %llu", static_cast<unsigned long long>(stats.frames));
    ImGui::Text("Layouts computed: %llu", static_cast<unsigned long long>(stats.layouts));
    const TextMetricsSource metrics = text_metrics_source();
    if (metrics == TextMetricsSource::Estimated) {
        ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.25f, 1.0f), "Text metrics: %s",
                           text_metrics_source_name(metrics));
        ImGui::TextDisabled(
            "ReShade's ImGui returned no text width, so widths are estimated. Alignment will be "
            "a few pixels out but nothing is hidden.");
    } else {
        ImGui::Text("Text metrics: %s", text_metrics_source_name(metrics));
    }
    ImGui::TextDisabled(
        "The overlay adds no render pass and creates no GPU resource: it appends to the draw "
        "list ReShade already submits.");

    ImGui::SeparatorText("Configuration");
    if (config_diagnostics.issues.empty()) {
        ImGui::TextDisabled("Loaded without any repairs.");
    } else {
        for (const ConfigIssue& issue : config_diagnostics.issues) {
            const ImVec4 colour = issue.severity == ConfigIssue::Severity::Error
                                      ? ImVec4(1.0f, 0.45f, 0.45f, 1.0f)
                                      : (issue.severity == ConfigIssue::Severity::Warning
                                             ? ImVec4(1.0f, 0.75f, 0.3f, 1.0f)
                                             : ImVec4(0.7f, 0.7f, 0.7f, 1.0f));
            ImGui::TextColored(colour, "%s%s%s", issue.path.c_str(),
                               issue.path.empty() ? "" : ": ", issue.message.c_str());
        }
    }
    if (config_diagnostics.newer_than_supported) {
        ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.3f, 1.0f),
                           "This configuration was written by a newer version. Saving will "
                           "rewrite it in this version's format.");
    }

    ImGui::SeparatorText("Logging");
    static const char* kLevels[] = {"trace", "debug", "info", "warn", "error", "off"};
    int level_index = 2;
    for (int i = 0; i < 6; ++i) {
        if (config.logging.level == kLevels[i]) level_index = i;
    }
    if (ImGui::Combo("Log level", &level_index, kLevels, 6)) {
        config.logging.level = kLevels[level_index];
        actions.config_changed = true;
    }
    actions.config_changed |= ImGui::Checkbox("Write a log file", &config.logging.to_file);
    if (ImGui::Checkbox("Include chat message text in the log",
                        &config.logging.include_message_content)) {
        actions.config_changed = true;
    }
    ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.3f, 1.0f),
                       "Leave that off unless you are diagnosing a problem: it writes the "
                       "contents of chat messages to a file on disk.");
}

void SettingsUi::section_typography(Config& config, SettingsActions& actions) {
    AppearanceConfig& a = config.appearance;

    if (fonts_ == nullptr) {
        ImGui::TextDisabled("The font engine is not available in this build.");
    } else {
        // The list is of files the add-on rasterises itself. It deliberately does not touch
        // ReShade's font atlas: reading that atlas from an add-on is what crashed the game the
        // first time a font picker was attempted here, and the two are now fully independent --
        // this setting changes the overlay only, never ReShade's own interface.
        const std::vector<FontFile>& files = fonts_->available();
        std::string current = a.font_file.empty() ? std::string("ReShade's font") : a.font_file;
        for (const FontFile& f : files) {
            if (f.file == a.font_file && f.face_index == a.font_face_index) {
                current = f.label();
                break;
            }
        }
        if (ImGui::BeginCombo("Font", current.c_str())) {
            if (ImGui::Selectable("ReShade's font", a.font_file.empty())) {
                a.font_file.clear();
                a.font_face_index = 0;
                actions.config_changed = true;
            }
            for (const FontFile& f : files) {
                const bool selected = f.file == a.font_file && f.face_index == a.font_face_index;
                ImGui::PushID(f.file.c_str());
                ImGui::PushID(f.face_index);
                if (ImGui::Selectable(f.label().c_str(), selected)) {
                    a.font_file = f.file;
                    a.font_face_index = f.face_index;
                    actions.config_changed = true;
                }
                ImGui::PopID();
                ImGui::PopID();
            }
            ImGui::EndCombo();
        }
        if (ImGui::Button("Rescan fonts folder")) fonts_->rescan();
        ImGui::SameLine();
        ImGui::TextDisabled("%d found", static_cast<int>(files.size()));
        for (const std::string& dir : fonts_->directories()) {
            ImGui::TextDisabled("%s", dir.c_str());
        }
        if (!fonts_->error().empty()) {
            ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.4f, 1.0f), "%s", fonts_->error().c_str());
        }
    }

    // The three sizes are shown in pixels because that is how people think about them, and
    // written back as the multipliers the layout actually uses.
    float header_px = a.font_size * config.channel_title.font_scale;
    if (ImGui::SliderFloat("Channel header font size", &header_px, 6.0f, 64.0f, "%.0f px")) {
        config.channel_title.font_scale = header_px / std::max(1.0f, a.font_size);
        actions.config_changed = true;
    }
    actions.config_changed |=
        ImGui::SliderFloat("User list font size", &a.font_size, 6.0f, 64.0f, "%.0f px");
    float notif_px = a.font_size * config.notifications.font_scale;
    if (ImGui::SliderFloat("Notifications font size", &notif_px, 6.0f, 64.0f, "%.0f px")) {
        config.notifications.font_scale = notif_px / std::max(1.0f, a.font_size);
        actions.config_changed = true;
    }
    actions.config_changed |=
        ImGui::SliderFloat("Icon scale", &a.icon_size, 2.0f, 32.0f, "%.0f px");
    actions.config_changed |=
        ImGui::SliderFloat("Row height", &a.row_height, 8.0f, 64.0f, "%.0f px");
    actions.config_changed |= ImGui::SliderFloat("Overall scale", &config.general.scale, 0.5f,
                                                 3.0f, "%.2fx");
    actions.config_changed |= ImGui::Checkbox("Outline the text", &a.text_outline);
    if (a.text_outline) {
        actions.config_changed |= colour_edit("Outline colour", a.text_outline_color);
        actions.config_changed |=
            ImGui::SliderFloat("Outline thickness", &a.text_outline_thickness, 0.5f, 3.0f, "%.1f");
    } else {
        actions.config_changed |= ImGui::Checkbox("Drop shadow", &a.text_shadow);
    }
}

void SettingsUi::section_notification_box(Config& config, SettingsActions& actions) {
    NotificationsConfig& n = config.notifications;
    NotificationBoxStyle& b = n.box;

    actions.config_changed |= ImGui::Checkbox("Size each toast to its text", &b.auto_width);
    help("On, a notification is only as wide as its message, the way a chat feed reads. Off, "
         "every notification uses the fixed width below.");
    if (b.auto_width) {
        actions.config_changed |=
            ImGui::SliderFloat("Widest toast", &b.max_width, 120.0f, 1200.0f, "%.0f px");
    } else {
        actions.config_changed |= ImGui::SliderFloat("Width", &n.width, 120.0f, 1200.0f, "%.0f px");
    }

    actions.config_changed |= colour_edit("Box colour", b.background);
    help("Its alpha is how dark the box is over the game.");
    actions.config_changed |=
        ImGui::SliderFloat("Corner rounding", &b.corner_radius, 0.0f, 16.0f, "%.0f px");

    constexpr NotificationBorder kBorders[] = {NotificationBorder::None,
                                               NotificationBorder::Accent,
                                               NotificationBorder::Custom};
    actions.config_changed |= enum_combo("Edge", b.border, kBorders);
    help("Accent outlines each toast in the colour of the event it is reporting, so a join and "
         "a kick are distinguishable without reading them.");
    if (b.border != NotificationBorder::None) {
        actions.config_changed |=
            ImGui::SliderFloat("Edge thickness", &b.border_thickness, 0.0f, 4.0f, "%.1f px");
    }
    if (b.border == NotificationBorder::Custom) {
        actions.config_changed |= colour_edit("Edge colour", b.border_color);
    } else if (b.border == NotificationBorder::Accent) {
        actions.config_changed |= ImGui::SliderFloat("Edge strength", &b.border_accent_opacity,
                                                     0.0f, 1.0f, "%.2f");
    }

    actions.config_changed |= ImGui::Checkbox("Category stripe", &b.accent_bar);
    if (b.accent_bar) {
        actions.config_changed |=
            ImGui::SliderFloat("Stripe width", &b.accent_bar_width, 1.0f, 10.0f, "%.0f px");
    }
    actions.config_changed |=
        ImGui::SliderFloat("Inner padding", &n.padding_x, 0.0f, 32.0f, "%.0f px");
    actions.config_changed |=
        ImGui::SliderFloat("Inner padding (vertical)", &n.padding_y, 0.0f, 32.0f, "%.0f px");
    actions.config_changed |= ImGui::SliderFloat("Gap between", &n.spacing, 0.0f, 32.0f, "%.0f px");
}

void SettingsUi::basic_view(Config& config, const LinkDiagnostics& diagnostics,
                            const OverlayFrame& frame, ProfileStore& profiles,
                            const FrameStats& stats,
                            const ConfigDiagnostics& config_diagnostics,
                            SettingsActions& actions) {
    AppearanceConfig& a = config.appearance;
    UserListConfig& ul = config.user_list;
    ChannelTitleConfig& title = config.channel_title;
    NotificationsConfig& n = config.notifications;

    actions.config_changed |= ImGui::Checkbox("Show the overlay", &config.general.enabled);
    ImGui::SameLine();
    ImGui::Checkbox("Demo mode", &preview_active_);
    help("Draws sample users in every state, one of each notification and a sample chat feed, "
         "so the layout can be positioned without waiting for anyone to speak. Nothing is sent "
         "to TeamSpeak and no sound is played.");

    if (ImGui::CollapsingHeader("TeamSpeak 3 status", ImGuiTreeNodeFlags_DefaultOpen)) {
        if (diagnostics.state == LinkState::Connected) {
            ImGui::TextColored(ImVec4(0.45f, 0.85f, 0.5f, 1.0f), "Connected");
            const OverlayState& state = frame.state;
            if (!state.server.name.empty()) ImGui::Text("Server: %s", state.server.name.c_str());
            if (!state.channel.name.empty()) {
                ImGui::Text("Channel: %s", state.channel.name.c_str());
            }
            ImGui::Text("Clients: %d", static_cast<int>(state.users.size()));
        } else {
            ImGui::TextColored(ImVec4(1.0f, 0.65f, 0.3f, 1.0f), "Not connected to the plugin");
            ImGui::TextDisabled(
                "Install the TeamSpeak plugin and make sure TeamSpeak is running. The overlay "
                "retries on its own.");
            if (ImGui::Button("Reconnect now")) actions.reconnect_requested = true;
        }
    }

    if (ImGui::CollapsingHeader("Typography & font engine", ImGuiTreeNodeFlags_DefaultOpen)) {
        section_typography(config, actions);
    }

    if (ImGui::CollapsingHeader("HUD layout & positioning", ImGuiTreeNodeFlags_DefaultOpen)) {
        // Fractions of the screen rather than pixels, so a layout set at 1080p still lands in
        // the same place at 1440p.
        const auto place = [&](const char* label, Placement& p) {
            ImGui::PushID(label);
            bool changed = false;
            if (!p.percent) {
                // Migrate the stored pixel offsets the first time this view touches them, so the
                // slider below is not showing a fraction while the config holds pixels.
                p.percent = true;
                p.x = std::min(0.995f, p.x / 1920.0f);
                p.y = std::min(0.995f, p.y / 1080.0f);
                changed = true;
            }
            changed |= ImGui::SliderFloat((std::string(label) + " X").c_str(), &p.x, 0.0f, 1.0f,
                                          "%.3f");
            changed |= ImGui::SliderFloat((std::string(label) + " Y").c_str(), &p.y, 0.0f, 1.0f,
                                          "%.3f");
            ImGui::PopID();
            return changed;
        };

        actions.config_changed |= ImGui::Checkbox("Move the title and list together",
                                                  &config.group.enabled);
        if (config.group.enabled) {
            actions.config_changed |= enum_combo("Anchor corner", config.group.anchor, kAnchors);
            if (!config.group.percent) {
                config.group.percent = true;
                config.group.x = std::min(0.995f, config.group.x / 1920.0f);
                config.group.y = std::min(0.995f, config.group.y / 1080.0f);
                actions.config_changed = true;
            }
            actions.config_changed |=
                ImGui::SliderFloat("User list X", &config.group.x, 0.0f, 1.0f, "%.3f");
            actions.config_changed |=
                ImGui::SliderFloat("User list Y", &config.group.y, 0.0f, 1.0f, "%.3f");
            actions.config_changed |=
                ImGui::SliderFloat("Block size", &config.group.scale, 0.4f, 3.0f, "%.2fx");
            actions.config_changed |=
                ImGui::SliderFloat("Gap under the title", &config.group.spacing, 0.0f, 40.0f,
                                   "%.0f px");
        } else {
            actions.config_changed |= enum_combo("Title corner", title.placement.anchor, kAnchors);
            actions.config_changed |= place("Title", title.placement);
            actions.config_changed |= enum_combo("List corner", ul.placement.anchor, kAnchors);
            actions.config_changed |= place("User list", ul.placement);
        }

        actions.config_changed |=
            enum_combo("Notification corner", n.placement.anchor, kAnchors);
        actions.config_changed |= place("Notifications", n.placement);

        // One switch for the whole HUD: the alignment that matters is "which edge is fixed".
        const Align current = config.group.enabled ? config.group.align : ul.placement.align;
        bool right_aligned = current == Align::Right;
        if (ImGui::Checkbox("Right-aligned layout", &right_aligned)) {
            const Align chosen = right_aligned ? Align::Right : Align::Left;
            config.group.align = chosen;
            ul.placement.align = chosen;
            title.placement.align = chosen;
            n.placement.align = chosen;
            config.chat.placement.align = chosen;
            actions.config_changed = true;
        }
        help("Right means the right edge is the fixed point: longer names and longer channel "
             "names grow leftward instead of running off the screen.");

        actions.config_changed |= enum_combo("Sort mode", ul.sort, kSorts);
        actions.config_changed |= ImGui::SliderInt("Max visible users", &ul.max_visible_users, 1, 64);
        actions.config_changed |= ImGui::SliderFloat("Row spacing", &a.row_spacing, 0.0f, 24.0f,
                                                     "%.0f px");
        actions.config_changed |= ImGui::Checkbox("Show channel header", &title.placement.visible);
        actions.config_changed |=
            ImGui::Checkbox("Stealth mode (only show talking users)", &ul.only_show_talking);
        actions.config_changed |=
            ImGui::Checkbox("Highlight yourself in the channel", &ul.highlight_local_user);
    }

    if (ImGui::CollapsingHeader("Notifications & toasts", ImGuiTreeNodeFlags_DefaultOpen)) {
        actions.config_changed |= ImGui::Checkbox("Show notifications", &n.placement.visible);
        if (n.placement.visible) {
            // Opacity is the alpha of the box colour, exposed on its own because "how dark is
            // the box" is the thing people actually reach for.
            float opacity = static_cast<float>(n.box.background.a) / 255.0f;
            if (ImGui::SliderFloat("Toast background opacity", &opacity, 0.0f, 1.0f, "%.2f")) {
                n.box.background.a = static_cast<std::uint8_t>(opacity * 255.0f + 0.5f);
                actions.config_changed = true;
            }
            actions.config_changed |= ImGui::Checkbox("Show category accent bar", &n.box.accent_bar);
            if (n.box.accent_bar) {
                actions.config_changed |= ImGui::SliderFloat("Accent bar width",
                                                             &n.box.accent_bar_width, 1.0f, 10.0f,
                                                             "%.0f px");
            }

            // One duration for every category: six identical timers is not a setting, it is a
            // chore. Advanced still exposes them individually.
            float seconds = static_cast<float>(n.join.fade.hold_ms) / 1000.0f;
            if (ImGui::SliderFloat("Toast duration", &seconds, 0.5f, 20.0f, "%.1fs")) {
                const int ms = static_cast<int>(seconds * 1000.0f);
                for (NotificationStyle* style : {&n.join, &n.leave, &n.channel_switch,
                                                 &n.connection, &n.whisper, &n.chat}) {
                    style->fade.hold_ms = ms;
                }
                actions.config_changed = true;
            }

            section_notification_box(config, actions);

            ImGui::SeparatorText("Event filter toggles");
            actions.config_changed |= ImGui::Checkbox("Channel joins (+)", &n.join.enabled);
            actions.config_changed |= ImGui::Checkbox("Channel leaves (-)", &n.leave.enabled);
            actions.config_changed |= ImGui::Checkbox("Channel moves", &n.channel_switch.enabled);
            actions.config_changed |= ImGui::Checkbox("Connection changes", &n.connection.enabled);
            actions.config_changed |= ImGui::Checkbox("Whispers", &n.whisper.enabled);
            actions.config_changed |= ImGui::Checkbox("Channel chat", &n.chat.enabled);
            if (ImGui::Checkbox("Private messages##notif", &n.private_chat.enabled)) {
                actions.config_changed = true;
                actions.subscription_changed = true;
            }
            help("A message sent to you personally gets its own toast, naming the sender. This "
                 "also needs private messages switched on under Chat -- with them off the "
                 "plugin is never asked for them at all.");
        }
    }

    if (ImGui::CollapsingHeader("Chat")) {
        if (ImGui::Checkbox("Show the chat feed", &config.chat.placement.visible)) {
            actions.config_changed = true;
            actions.subscription_changed = true;
        }
        if (config.chat.placement.visible) {
            actions.config_changed |= enum_combo("Chat corner", config.chat.placement.anchor,
                                                 kAnchors);
            actions.config_changed |= ImGui::SliderFloat("Chat X", &config.chat.placement.x, 0.0f,
                                                         3840.0f, "%.0f px");
            actions.config_changed |= ImGui::SliderFloat("Chat Y", &config.chat.placement.y, 0.0f,
                                                         2160.0f, "%.0f px");
            if (ImGui::Checkbox("Channel messages", &config.chat.show_channel_messages)) {
                actions.config_changed = true;
                actions.subscription_changed = true;
            }
            if (ImGui::Checkbox("Server messages", &config.chat.show_server_messages)) {
                actions.config_changed = true;
                actions.subscription_changed = true;
            }
            if (ImGui::Checkbox("Private messages", &config.chat.show_private_messages)) {
                actions.config_changed = true;
                actions.subscription_changed = true;
            }
            help("Off by default and enforced in the plugin: while this is off, private "
                 "messages are never sent to the overlay at all.");
            actions.config_changed |=
                ImGui::SliderInt("Lines", &config.chat.max_visible_messages, 1, 20);
        } else {
            ImGui::TextDisabled("While this is off, the plugin is not asked for messages at all.");
        }
    }

    if (ImGui::CollapsingHeader("Colours")) {
        actions.config_changed |= colour_edit("Names", a.text_default);
        actions.config_changed |= colour_edit("Channel header", title.text);
        actions.config_changed |= colour_edit("Talking", config.indicators.speaking.icon_color);
        actions.config_changed |= colour_edit("Muted", config.indicators.mic_muted.icon_color);
        actions.config_changed |= colour_edit("You", ul.local_user_color);
        actions.config_changed |=
            colour_edit("Channel Commander", config.indicators.commander.icon_color);
        ImGui::SeparatorText("Friends");
        actions.config_changed |=
            ImGui::Checkbox("Use TeamSpeak's Contacts list", &ul.use_teamspeak_friends);
        help("Friends come from TeamSpeak itself: the plugin reads the client's own Contacts, "
             "so anyone you added there is already a friend here. Their TeamSpeak nickname is "
             "what shows in [brackets].");
        actions.config_changed |= ImGui::Checkbox("Colour friends differently", &ul.color_friends);
        if (ul.color_friends) actions.config_changed |= colour_edit("Friend", ul.friend_color);
        actions.config_changed |= ImGui::Checkbox("Show their [nickname]", &ul.show_friend_tag);
        if (ul.show_friend_tag) {
            actions.config_changed |= colour_edit("Nickname", ul.friend_tag_color);
        }
    }

    if (ImGui::CollapsingHeader("Profiles")) {
        tab_profiles(config, profiles, actions);
    }

    if (ImGui::CollapsingHeader("Diagnostics")) {
        tab_diagnostics(diagnostics, frame, stats, config_diagnostics, config, actions);
    }
}

SettingsActions SettingsUi::draw(Config& config, const LinkDiagnostics& diagnostics,
                                 const OverlayFrame& frame, ProfileStore& profiles,
                                 const FrameStats& stats,
                                 const ConfigDiagnostics& config_diagnostics) {
    SettingsActions actions;
    const bool advanced = config.general.advanced_settings;

    ImGui::TextColored(ImVec4(0.35f, 0.65f, 1.0f, 1.0f), "Paz' TeamSpeak Overlay v1.0");
    ImGui::SameLine();
    ImGui::TextDisabled("(build %s-%s)", TSRO_VERSION, TSRO_BUILD_ID);
    if (ImGui::Checkbox("All settings", &config.general.advanced_settings)) {
        actions.config_changed = true;
    }
    help("The basic view is one list of the settings people actually change. Turn this on for "
         "every colour, animation and timing the overlay has.");
    ImGui::SameLine();
    if (ImGui::Button("Save")) actions.save_requested = true;
    ImGui::SameLine();
    if (ImGui::Button("Reload")) actions.reload_requested = true;
    if (!status_.empty()) {
        ImGui::SameLine();
        ImGui::TextColored(status_is_error_ ? ImVec4(1.0f, 0.45f, 0.4f, 1.0f)
                                            : ImVec4(0.45f, 0.85f, 0.5f, 1.0f),
                           "%s", status_.c_str());
    }
    ImGui::Separator();

    if (!advanced) {
        basic_view(config, diagnostics, frame, profiles, stats, config_diagnostics, actions);
        return actions;
    }

    if (ImGui::BeginTabBar("tsro_tabs", ImGuiTabBarFlags_FittingPolicyScroll)) {
        if (ImGui::BeginTabItem("General")) {
            tab_general(config, actions);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Appearance")) {
            tab_appearance(config, actions);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Layout")) {
            tab_layout(config, actions);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Users")) {
            tab_users(config, frame, actions);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Channels")) {
            tab_channels(config, frame, actions);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Indicators")) {
            tab_indicators(config, actions);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Notifications")) {
            tab_notifications(config, actions);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Chat")) {
            tab_chat(config, actions);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Animation")) {
            tab_animation(config, actions);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Integration")) {
            tab_integration(config, actions);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Profiles")) {
            tab_profiles(config, profiles, actions);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Diagnostics")) {
            tab_diagnostics(diagnostics, frame, stats, config_diagnostics, config, actions);
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }

    return actions;
}

}  // namespace tsro::overlay
