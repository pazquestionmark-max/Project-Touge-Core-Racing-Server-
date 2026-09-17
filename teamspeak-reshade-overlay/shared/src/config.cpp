// SPDX-License-Identifier: MIT
#include "tsro/config.hpp"

#include <algorithm>
#include <cmath>

#include "tsro/protocol.hpp"

namespace tsro {
namespace {

const json::Value kEmptyObject{json::Object{}};

/// Tolerant reader. Every getter leaves `out` at its incoming (default) value when the key is
/// absent or the wrong type, and records a diagnostic when a present value had to be repaired.
struct R {
    const json::Value& v;
    ConfigDiagnostics& d;
    std::string path;

    R sub(const char* key) const {
        const json::Value* c = v.find(key);
        const bool ok = c != nullptr && c->is_object();
        if (c != nullptr && !ok) {
            d.add(ConfigIssue::Severity::Warning, join(key), "expected an object; using defaults");
        }
        return R{ok ? *c : kEmptyObject, d, join(key)};
    }

    std::string join(const char* key) const {
        return path.empty() ? std::string(key) : path + "." + key;
    }

    void b(const char* key, bool& out) const {
        const json::Value* c = v.find(key);
        if (c == nullptr) return;
        if (!c->is_bool()) {
            d.add(ConfigIssue::Severity::Warning, join(key), "expected a boolean; kept default");
            return;
        }
        out = c->as_bool();
    }

    void f(const char* key, float& out, float lo, float hi) const {
        const json::Value* c = v.find(key);
        if (c == nullptr) return;
        if (!c->is_number()) {
            d.add(ConfigIssue::Severity::Warning, join(key), "expected a number; kept default");
            return;
        }
        double n = c->as_double();
        if (!std::isfinite(n)) {
            d.add(ConfigIssue::Severity::Warning, join(key), "not a finite number; kept default");
            return;
        }
        if (n < lo || n > hi) {
            d.add(ConfigIssue::Severity::Info, join(key), "out of range; clamped");
            n = n < lo ? lo : hi;
        }
        out = static_cast<float>(n);
    }

    void i(const char* key, int& out, int lo, int hi) const {
        const json::Value* c = v.find(key);
        if (c == nullptr) return;
        if (!c->is_number()) {
            d.add(ConfigIssue::Severity::Warning, join(key), "expected a number; kept default");
            return;
        }
        long long n = c->as_int();
        if (n < lo || n > hi) {
            d.add(ConfigIssue::Severity::Info, join(key), "out of range; clamped");
            n = n < lo ? lo : hi;
        }
        out = static_cast<int>(n);
    }

    void s(const char* key, std::string& out, std::size_t max_chars) const {
        const json::Value* c = v.find(key);
        if (c == nullptr) return;
        if (!c->is_string()) {
            d.add(ConfigIssue::Severity::Warning, join(key), "expected a string; kept default");
            return;
        }
        std::string t = c->as_string();
        if (t.size() > max_chars) {
            d.add(ConfigIssue::Severity::Info, join(key), "too long; truncated");
            t = json::truncate_utf8(t, max_chars);
        }
        out = std::move(t);
    }

    void col(const char* key, Color& out) const {
        const json::Value* c = v.find(key);
        if (c == nullptr) return;
        if (!c->is_string()) {
            d.add(ConfigIssue::Severity::Warning, join(key), "expected a colour string");
            return;
        }
        if (auto parsed = Color::from_hex(c->as_string())) {
            out = *parsed;
        } else {
            d.add(ConfigIssue::Severity::Warning, join(key),
                  "not a valid #RRGGBB / #RRGGBBAA colour; kept default");
        }
    }

    void ocol(const char* key, std::optional<Color>& out) const {
        const json::Value* c = v.find(key);
        if (c == nullptr) return;
        if (c->is_null()) {
            out.reset();
            return;
        }
        if (!c->is_string()) {
            d.add(ConfigIssue::Severity::Warning, join(key), "expected a colour string");
            return;
        }
        if (auto parsed = Color::from_hex(c->as_string())) out = *parsed;
        else d.add(ConfigIssue::Severity::Warning, join(key), "not a valid colour; ignored");
    }

    template <typename E>
    void en(const char* key, E& out) const {
        const json::Value* c = v.find(key);
        if (c == nullptr) return;
        if (!c->is_string()) {
            d.add(ConfigIssue::Severity::Warning, join(key), "expected a string; kept default");
            return;
        }
        E parsed{};
        if (parse_enum(c->as_string(), parsed)) out = parsed;
        else d.add(ConfigIssue::Severity::Warning, join(key), "unrecognised value; kept default");
    }

    template <typename E>
    void oen(const char* key, std::optional<E>& out) const {
        const json::Value* c = v.find(key);
        if (c == nullptr) return;
        if (c->is_null()) {
            out.reset();
            return;
        }
        if (!c->is_string()) return;
        E parsed{};
        if (parse_enum(c->as_string(), parsed)) out = parsed;
        else d.add(ConfigIssue::Severity::Warning, join(key), "unrecognised value; ignored");
    }
};

json::Value write(const Placement& p) {
    json::Value o{json::Object{}};
    o.set("visible", json::Value(p.visible));
    o.set("anchor", json::Value(to_string(p.anchor)));
    o.set("x", json::Value(p.x));
    o.set("y", json::Value(p.y));
    o.set("percent", json::Value(p.percent));
    o.set("align", json::Value(to_string(p.align)));
    return o;
}

void read(const R& r, Placement& p) {
    r.b("visible", p.visible);
    r.en("anchor", p.anchor);
    // Percentage offsets are 0..1 of the viewport; pixel offsets are allowed to be negative so
    // an element can be pushed partly off-screen deliberately.
    r.f("x", p.x, -16384.0f, 16384.0f);
    r.f("y", p.y, -16384.0f, 16384.0f);
    r.b("percent", p.percent);
    r.en("align", p.align);
}

json::Value write(const Fade& f) {
    json::Value o{json::Object{}};
    o.set("in_ms", json::Value(f.in_ms));
    o.set("out_ms", json::Value(f.out_ms));
    o.set("hold_ms", json::Value(f.hold_ms));
    o.set("start_opacity", json::Value(f.start_opacity));
    o.set("end_opacity", json::Value(f.end_opacity));
    o.set("easing", json::Value(to_string(f.easing)));
    return o;
}

void read(const R& r, Fade& f) {
    r.i("in_ms", f.in_ms, 0, 10000);
    r.i("out_ms", f.out_ms, 0, 10000);
    r.i("hold_ms", f.hold_ms, 0, 600000);
    r.f("start_opacity", f.start_opacity, 0.0f, 1.0f);
    r.f("end_opacity", f.end_opacity, 0.0f, 1.0f);
    r.en("easing", f.easing);
}

json::Value write(const StateStyle& s) {
    json::Value o{json::Object{}};
    o.set("enabled", json::Value(s.enabled));
    o.set("show_icon", json::Value(s.show_icon));
    o.set("icon", json::Value(to_string(s.icon)));
    o.set("icon_scale", json::Value(s.icon_scale));
    o.set("icon_color", json::Value(s.icon_color.to_hex()));
    o.set("override_text_color", json::Value(s.override_text_color));
    o.set("text_color", json::Value(s.text_color.to_hex()));
    o.set("show_background", json::Value(s.show_background));
    o.set("background", json::Value(s.background.to_hex()));
    o.set("show_border", json::Value(s.show_border));
    o.set("border", json::Value(s.border.to_hex()));
    o.set("border_thickness", json::Value(s.border_thickness));
    o.set("glow", json::Value(s.glow));
    o.set("glow_color", json::Value(s.glow_color.to_hex()));
    o.set("glow_radius", json::Value(s.glow_radius));
    o.set("opacity", json::Value(s.opacity));
    o.set("dim_entry", json::Value(s.dim_entry));
    o.set("dim_amount", json::Value(s.dim_amount));
    o.set("icon_image", json::Value(s.icon_image));
    return o;
}

void read(const R& r, StateStyle& s) {
    r.b("enabled", s.enabled);
    r.b("show_icon", s.show_icon);
    r.en("icon", s.icon);
    r.f("icon_scale", s.icon_scale, 0.1f, 8.0f);
    r.col("icon_color", s.icon_color);
    r.b("override_text_color", s.override_text_color);
    r.col("text_color", s.text_color);
    r.b("show_background", s.show_background);
    r.col("background", s.background);
    r.b("show_border", s.show_border);
    r.col("border", s.border);
    r.f("border_thickness", s.border_thickness, 0.0f, 12.0f);
    r.b("glow", s.glow);
    r.col("glow_color", s.glow_color);
    r.f("glow_radius", s.glow_radius, 0.0f, 64.0f);
    r.f("opacity", s.opacity, 0.0f, 1.0f);
    r.b("dim_entry", s.dim_entry);
    r.f("dim_amount", s.dim_amount, 0.0f, 1.0f);
    r.s("icon_image", s.icon_image, 260);
}

json::Value write(const NotificationStyle& n) {
    json::Value o{json::Object{}};
    o.set("enabled", json::Value(n.enabled));
    o.set("format", json::Value(n.format));
    o.set("prefix", json::Value(n.prefix));
    o.set("icon", json::Value(to_string(n.icon)));
    o.set("icon_color", json::Value(n.icon_color.to_hex()));
    o.set("text", json::Value(n.text.to_hex()));
    o.set("name_color", json::Value(n.name_color.to_hex()));
    o.set("background", json::Value(n.background.to_hex()));
    o.set("border", json::Value(n.border.to_hex()));
    o.set("show_background", json::Value(n.show_background));
    o.set("show_border", json::Value(n.show_border));
    o.set("fade", write(n.fade));
    o.set("sound", json::Value(n.sound));
    o.set("sound_file", json::Value(n.sound_file));
    return o;
}

void read(const R& r, NotificationStyle& n) {
    r.b("enabled", n.enabled);
    r.s("format", n.format, 200);
    r.s("prefix", n.prefix, 16);
    r.en("icon", n.icon);
    r.col("icon_color", n.icon_color);
    r.col("text", n.text);
    r.col("name_color", n.name_color);
    r.col("background", n.background);
    r.col("border", n.border);
    r.b("show_background", n.show_background);
    r.b("show_border", n.show_border);
    read(r.sub("fade"), n.fade);
    r.b("sound", n.sound);
    r.s("sound_file", n.sound_file, 260);
}

json::Value write(const UserOverride& u) {
    json::Value o{json::Object{}};
    o.set("enabled", json::Value(u.enabled));
    if (u.name_color) o.set("name_color", json::Value(u.name_color->to_hex()));
    if (u.speaking_color) o.set("speaking_color", json::Value(u.speaking_color->to_hex()));
    if (u.muted_color) o.set("muted_color", json::Value(u.muted_color->to_hex()));
    if (u.commander_color) o.set("commander_color", json::Value(u.commander_color->to_hex()));
    if (u.icon) o.set("icon", json::Value(to_string(*u.icon)));
    if (u.icon_color) o.set("icon_color", json::Value(u.icon_color->to_hex()));
    if (!u.display_override.empty()) o.set("display_override", json::Value(u.display_override));
    if (!u.note.empty()) o.set("note", json::Value(u.note));
    return o;
}

void read(const R& r, UserOverride& u) {
    r.b("enabled", u.enabled);
    r.ocol("name_color", u.name_color);
    r.ocol("speaking_color", u.speaking_color);
    r.ocol("muted_color", u.muted_color);
    r.ocol("commander_color", u.commander_color);
    r.oen("icon", u.icon);
    r.ocol("icon_color", u.icon_color);
    r.s("display_override", u.display_override, 64);
    r.s("note", u.note, 200);
}

json::Value write(const ChannelOverride& c) {
    json::Value o{json::Object{}};
    o.set("enabled", json::Value(c.enabled));
    if (c.title_color) o.set("title_color", json::Value(c.title_color->to_hex()));
    if (c.background) o.set("background", json::Value(c.background->to_hex()));
    if (c.border) o.set("border", json::Value(c.border->to_hex()));
    if (c.user_list_color) o.set("user_list_color", json::Value(c.user_list_color->to_hex()));
    if (c.icon) o.set("icon", json::Value(to_string(*c.icon)));
    if (c.icon_color) o.set("icon_color", json::Value(c.icon_color->to_hex()));
    if (c.font_scale) o.set("font_scale", json::Value(*c.font_scale));
    if (c.opacity) o.set("opacity", json::Value(*c.opacity));
    if (!c.display_override.empty()) o.set("display_override", json::Value(c.display_override));
    return o;
}

void read(const R& r, ChannelOverride& c) {
    r.b("enabled", c.enabled);
    r.ocol("title_color", c.title_color);
    r.ocol("background", c.background);
    r.ocol("border", c.border);
    r.ocol("user_list_color", c.user_list_color);
    r.oen("icon", c.icon);
    r.ocol("icon_color", c.icon_color);
    if (const json::Value* f = r.v.find("font_scale")) {
        if (f->is_number()) c.font_scale = std::clamp(static_cast<float>(f->as_double()), 0.2f, 6.0f);
    }
    if (const json::Value* o = r.v.find("opacity")) {
        if (o->is_number()) c.opacity = std::clamp(static_cast<float>(o->as_double()), 0.0f, 1.0f);
    }
    r.s("display_override", c.display_override, 64);
}

constexpr const char* kKnownSections[] = {
    "config_version", "general",  "appearance", "group", "channel_title",     "user_list",
    "indicators",     "notifications", "chat",  "animation",         "integration",
    "logging",        "user_overrides", "channel_overrides",
};

bool is_known_section(std::string_view k) {
    for (const char* s : kKnownSections) {
        if (k == s) return true;
    }
    return false;
}

}  // namespace

json::Value Config::to_json() const {
    json::Value root{json::Object{}};
    root.set("config_version", json::Value(config_version));

    {
        json::Value o{json::Object{}};
        o.set("enabled", json::Value(general.enabled));
        o.set("show_when_disconnected", json::Value(general.show_when_disconnected));
        o.set("show_when_plugin_unavailable",
              json::Value(general.show_when_plugin_unavailable));
        o.set("master_opacity", json::Value(general.master_opacity));
        o.set("scale", json::Value(general.scale));
        o.set("profile_name", json::Value(general.profile_name));
        o.set("auto_profile_by_executable", json::Value(general.auto_profile_by_executable));
        o.set("advanced_settings", json::Value(general.advanced_settings));
        root.set("general", std::move(o));
    }
    {
        const AppearanceConfig& a = appearance;
        json::Value o{json::Object{}};
        o.set("font_index", json::Value(a.font_index));
        o.set("font_size", json::Value(a.font_size));
        o.set("icon_size", json::Value(a.icon_size));
        o.set("row_height", json::Value(a.row_height));
        o.set("row_spacing", json::Value(a.row_spacing));
        o.set("padding_x", json::Value(a.padding_x));
        o.set("padding_y", json::Value(a.padding_y));
        o.set("corner_radius", json::Value(a.corner_radius));
        o.set("panel_background", json::Value(a.panel_background.to_hex()));
        o.set("panel_border", json::Value(a.panel_border.to_hex()));
        o.set("panel_border_thickness", json::Value(a.panel_border_thickness));
        o.set("show_panel_background", json::Value(a.show_panel_background));
        o.set("text_shadow", json::Value(a.text_shadow));
        o.set("text_shadow_color", json::Value(a.text_shadow_color.to_hex()));
        o.set("text_shadow_offset", json::Value(a.text_shadow_offset));
        o.set("text_outline", json::Value(a.text_outline));
        o.set("text_outline_color", json::Value(a.text_outline_color.to_hex()));
        o.set("text_outline_thickness", json::Value(a.text_outline_thickness));
        o.set("text_default", json::Value(a.text_default.to_hex()));
        o.set("text_secondary", json::Value(a.text_secondary.to_hex()));
        o.set("accent", json::Value(a.accent.to_hex()));
        root.set("appearance", std::move(o));
    }
    {
        const GroupConfig& g = group;
        json::Value o{json::Object{}};
        o.set("enabled", json::Value(g.enabled));
        o.set("anchor", json::Value(to_string(g.anchor)));
        o.set("x", json::Value(g.x));
        o.set("y", json::Value(g.y));
        o.set("percent", json::Value(g.percent));
        o.set("align", json::Value(to_string(g.align)));
        o.set("scale", json::Value(g.scale));
        o.set("spacing", json::Value(g.spacing));
        root.set("group", std::move(o));
    }
    {
        const ChannelTitleConfig& c = channel_title;
        json::Value o{json::Object{}};
        o.set("placement", write(c.placement));
        o.set("scale", json::Value(c.scale));
        o.set("show_parent", json::Value(c.show_parent));
        o.set("show_user_count", json::Value(c.show_user_count));
        o.set("show_server_name", json::Value(c.show_server_name));
        o.set("show_topic", json::Value(c.show_topic));
        o.set("font_scale", json::Value(c.font_scale));
        o.set("opacity", json::Value(c.opacity));
        o.set("text", json::Value(c.text.to_hex()));
        o.set("background", json::Value(c.background.to_hex()));
        o.set("border", json::Value(c.border.to_hex()));
        o.set("show_background", json::Value(c.show_background));
        o.set("show_border", json::Value(c.show_border));
        o.set("icon", json::Value(to_string(c.icon)));
        o.set("icon_color", json::Value(c.icon_color.to_hex()));
        o.set("max_width", json::Value(c.max_width));
        o.set("format", json::Value(c.format));
        o.set("parent_format", json::Value(c.parent_format));
        o.set("disconnected_text", json::Value(c.disconnected_text));
        root.set("channel_title", std::move(o));
    }
    {
        const UserListConfig& u = user_list;
        json::Value o{json::Object{}};
        o.set("placement", write(u.placement));
        o.set("scale", json::Value(u.scale));
        o.set("show_local_user", json::Value(u.show_local_user));
        o.set("highlight_local_user", json::Value(u.highlight_local_user));
        o.set("local_user_color", json::Value(u.local_user_color.to_hex()));
        o.set("show_muted_users", json::Value(u.show_muted_users));
        o.set("speaking_first", json::Value(u.speaking_first));
        o.set("sort", json::Value(to_string(u.sort)));
        o.set("name_overflow", json::Value(to_string(u.name_overflow)));
        o.set("max_name_width", json::Value(u.max_name_width));
        o.set("min_font_scale", json::Value(u.min_font_scale));
        o.set("max_visible_users", json::Value(u.max_visible_users));
        o.set("show_overflow_count", json::Value(u.show_overflow_count));
        o.set("show_avatar_initial", json::Value(u.show_avatar_initial));
        o.set("indicator_gap", json::Value(u.indicator_gap));
        root.set("user_list", std::move(o));
    }
    {
        json::Value o{json::Object{}};
        o.set("speaking", write(indicators.speaking));
        o.set("whispering", write(indicators.whispering));
        o.set("mic_muted", write(indicators.mic_muted));
        o.set("speaker_muted", write(indicators.speaker_muted));
        o.set("mic_hardware_off", write(indicators.mic_hardware_off));
        o.set("away", write(indicators.away));
        o.set("recording", write(indicators.recording));
        o.set("commander", write(indicators.commander));
        o.set("priority_speaker", write(indicators.priority_speaker));
        o.set("suppressed", write(indicators.suppressed));
        o.set("locally_muted", write(indicators.locally_muted));
        root.set("indicators", std::move(o));
    }
    {
        const NotificationsConfig& n = notifications;
        json::Value o{json::Object{}};
        o.set("placement", write(n.placement));
        o.set("max_visible", json::Value(n.max_visible));
        o.set("stack", json::Value(to_string(n.stack)));
        o.set("spacing", json::Value(n.spacing));
        o.set("width", json::Value(n.width));
        o.set("min_height", json::Value(n.min_height));
        o.set("padding_x", json::Value(n.padding_x));
        o.set("padding_y", json::Value(n.padding_y));
        o.set("merge_duplicates", json::Value(n.merge_duplicates));
        o.set("suppress_after_connect_ms", json::Value(n.suppress_after_connect_ms));
        o.set("join", write(n.join));
        o.set("leave", write(n.leave));
        o.set("channel_switch", write(n.channel_switch));
        o.set("connection", write(n.connection));
        o.set("whisper", write(n.whisper));
        o.set("chat", write(n.chat));
        root.set("notifications", std::move(o));
    }
    {
        const ChatConfig& c = chat;
        json::Value o{json::Object{}};
        o.set("placement", write(c.placement));
        o.set("show_channel_messages", json::Value(c.show_channel_messages));
        o.set("show_server_messages", json::Value(c.show_server_messages));
        o.set("show_private_messages", json::Value(c.show_private_messages));
        o.set("order", json::Value(to_string(c.order)));
        o.set("max_visible_messages", json::Value(c.max_visible_messages));
        o.set("history_size", json::Value(c.history_size));
        o.set("max_message_length", json::Value(c.max_message_length));
        o.set("retention_seconds", json::Value(c.retention_seconds));
        o.set("show_timestamp", json::Value(c.show_timestamp));
        o.set("show_sender", json::Value(c.show_sender));
        o.set("show_channel_name", json::Value(c.show_channel_name));
        o.set("show_category_icon", json::Value(c.show_category_icon));
        o.set("wrap", json::Value(c.wrap));
        o.set("width", json::Value(c.width));
        o.set("font_scale", json::Value(c.font_scale));
        o.set("text", json::Value(c.text.to_hex()));
        o.set("sender", json::Value(c.sender.to_hex()));
        o.set("timestamp", json::Value(c.timestamp.to_hex()));
        o.set("background", json::Value(c.background.to_hex()));
        o.set("show_background", json::Value(c.show_background));
        o.set("use_sender_color", json::Value(c.use_sender_color));
        root.set("chat", std::move(o));
    }
    {
        const AnimationConfig& a = animation;
        json::Value o{json::Object{}};
        o.set("enabled", json::Value(a.enabled));
        o.set("speaking", json::Value(to_string(a.speaking)));
        o.set("speaking_attack_ms", json::Value(a.speaking_attack_ms));
        o.set("speaking_release_ms", json::Value(a.speaking_release_ms));
        o.set("speaking_pulse_hz", json::Value(a.speaking_pulse_hz));
        o.set("speaking_pulse_depth", json::Value(a.speaking_pulse_depth));
        o.set("state_easing", json::Value(to_string(a.state_easing)));
        o.set("state_transition_ms", json::Value(a.state_transition_ms));
        o.set("animate_list_reorder", json::Value(a.animate_list_reorder));
        o.set("list_reorder_ms", json::Value(a.list_reorder_ms));
        o.set("overlay_fade", write(a.overlay_fade));
        o.set("fade_when_idle", json::Value(a.fade_when_idle));
        o.set("idle_after_ms", json::Value(a.idle_after_ms));
        o.set("idle_opacity", json::Value(a.idle_opacity));
        root.set("animation", std::move(o));
    }
    {
        const IntegrationConfig& i = integration;
        json::Value o{json::Object{}};
        o.set("pipe_name", json::Value(i.pipe_name));
        o.set("reconnect_initial_ms", json::Value(i.reconnect_initial_ms));
        o.set("reconnect_max_ms", json::Value(i.reconnect_max_ms));
        o.set("stale_after_ms", json::Value(i.stale_after_ms));
        o.set("ping_interval_ms", json::Value(i.ping_interval_ms));
        o.set("auto_connect", json::Value(i.auto_connect));
        root.set("integration", std::move(o));
    }
    {
        const LoggingConfig& l = logging;
        json::Value o{json::Object{}};
        o.set("level", json::Value(l.level));
        o.set("to_file", json::Value(l.to_file));
        o.set("file_name", json::Value(l.file_name));
        o.set("max_file_kb", json::Value(l.max_file_kb));
        o.set("include_message_content", json::Value(l.include_message_content));
        o.set("show_diagnostics_overlay", json::Value(l.show_diagnostics_overlay));
        root.set("logging", std::move(o));
    }
    {
        json::Value o{json::Object{}};
        for (const auto& [k, v] : user_overrides) o.set(k, write(v));
        root.set("user_overrides", std::move(o));
    }
    {
        json::Value o{json::Object{}};
        for (const auto& [k, v] : channel_overrides) o.set(k, write(v));
        root.set("channel_overrides", std::move(o));
    }

    // Preserved keys from a newer writer go back out unchanged.
    for (const auto& [k, v] : unknown) root.set(k, v);
    return root;
}

Config Config::from_json(const json::Value& root, ConfigDiagnostics& diag) {
    Config c = Config::defaults();
    if (!root.is_object()) {
        diag.add(ConfigIssue::Severity::Error, "", "document is not an object; using defaults");
        diag.from_defaults = true;
        return c;
    }
    c.config_version = static_cast<int>(root.get_int("config_version", kConfigVersion));

    const R r{root, diag, ""};

    {
        const R g = r.sub("general");
        g.b("enabled", c.general.enabled);
        g.b("show_when_disconnected", c.general.show_when_disconnected);
        g.b("show_when_plugin_unavailable", c.general.show_when_plugin_unavailable);
        g.f("master_opacity", c.general.master_opacity, 0.0f, 1.0f);
        g.f("scale", c.general.scale, 0.25f, 4.0f);
        g.s("profile_name", c.general.profile_name, 64);
        g.b("auto_profile_by_executable", c.general.auto_profile_by_executable);
        g.b("advanced_settings", c.general.advanced_settings);
    }
    {
        const R a = r.sub("appearance");
        AppearanceConfig& x = c.appearance;
        a.i("font_index", x.font_index, 0, 32);
        a.f("font_size", x.font_size, 6.0f, 96.0f);
        a.f("icon_size", x.icon_size, 2.0f, 96.0f);
        a.f("row_height", x.row_height, 6.0f, 160.0f);
        a.f("row_spacing", x.row_spacing, 0.0f, 64.0f);
        a.f("padding_x", x.padding_x, 0.0f, 128.0f);
        a.f("padding_y", x.padding_y, 0.0f, 128.0f);
        a.f("corner_radius", x.corner_radius, 0.0f, 32.0f);
        a.col("panel_background", x.panel_background);
        a.col("panel_border", x.panel_border);
        a.f("panel_border_thickness", x.panel_border_thickness, 0.0f, 12.0f);
        a.b("show_panel_background", x.show_panel_background);
        a.b("text_shadow", x.text_shadow);
        a.col("text_shadow_color", x.text_shadow_color);
        a.f("text_shadow_offset", x.text_shadow_offset, 0.0f, 8.0f);
        a.b("text_outline", x.text_outline);
        a.col("text_outline_color", x.text_outline_color);
        a.f("text_outline_thickness", x.text_outline_thickness, 0.0f, 6.0f);
        a.col("text_default", x.text_default);
        a.col("text_secondary", x.text_secondary);
        a.col("accent", x.accent);
    }
    {
        const R g = r.sub("group");
        GroupConfig& x = c.group;
        g.b("enabled", x.enabled);
        g.en("anchor", x.anchor);
        g.f("x", x.x, -16384.0f, 16384.0f);
        g.f("y", x.y, -16384.0f, 16384.0f);
        g.b("percent", x.percent);
        g.en("align", x.align);
        g.f("scale", x.scale, 0.25f, 4.0f);
        g.f("spacing", x.spacing, 0.0f, 200.0f);
    }
    {
        const R t = r.sub("channel_title");
        ChannelTitleConfig& x = c.channel_title;
        read(t.sub("placement"), x.placement);
        t.f("scale", x.scale, 0.25f, 4.0f);
        t.b("show_parent", x.show_parent);
        t.b("show_user_count", x.show_user_count);
        t.b("show_server_name", x.show_server_name);
        t.b("show_topic", x.show_topic);
        t.f("font_scale", x.font_scale, 0.2f, 6.0f);
        t.f("opacity", x.opacity, 0.0f, 1.0f);
        t.col("text", x.text);
        t.col("background", x.background);
        t.col("border", x.border);
        t.b("show_background", x.show_background);
        t.b("show_border", x.show_border);
        t.en("icon", x.icon);
        t.col("icon_color", x.icon_color);
        t.f("max_width", x.max_width, 0.0f, 8192.0f);
        t.s("format", x.format, 200);
        t.s("parent_format", x.parent_format, 200);
        t.s("disconnected_text", x.disconnected_text, 120);
    }
    {
        const R u = r.sub("user_list");
        UserListConfig& x = c.user_list;
        read(u.sub("placement"), x.placement);
        u.f("scale", x.scale, 0.25f, 4.0f);
        u.b("show_local_user", x.show_local_user);
        u.b("highlight_local_user", x.highlight_local_user);
        u.col("local_user_color", x.local_user_color);
        u.b("show_muted_users", x.show_muted_users);
        u.b("speaking_first", x.speaking_first);
        u.en("sort", x.sort);
        u.en("name_overflow", x.name_overflow);
        u.f("max_name_width", x.max_name_width, 20.0f, 2000.0f);
        u.f("min_font_scale", x.min_font_scale, 0.2f, 1.0f);
        u.i("max_visible_users", x.max_visible_users, 1,
            static_cast<int>(proto::kMaxUsersPerSnapshot));
        u.b("show_overflow_count", x.show_overflow_count);
        u.b("show_avatar_initial", x.show_avatar_initial);
        u.f("indicator_gap", x.indicator_gap, 0.0f, 64.0f);
    }
    {
        const R i = r.sub("indicators");
        read(i.sub("speaking"), c.indicators.speaking);
        read(i.sub("whispering"), c.indicators.whispering);
        read(i.sub("mic_muted"), c.indicators.mic_muted);
        read(i.sub("speaker_muted"), c.indicators.speaker_muted);
        read(i.sub("mic_hardware_off"), c.indicators.mic_hardware_off);
        read(i.sub("away"), c.indicators.away);
        read(i.sub("recording"), c.indicators.recording);
        read(i.sub("commander"), c.indicators.commander);
        read(i.sub("priority_speaker"), c.indicators.priority_speaker);
        read(i.sub("suppressed"), c.indicators.suppressed);
        read(i.sub("locally_muted"), c.indicators.locally_muted);
    }
    {
        const R n = r.sub("notifications");
        NotificationsConfig& x = c.notifications;
        read(n.sub("placement"), x.placement);
        n.i("max_visible", x.max_visible, 1, 20);
        n.en("stack", x.stack);
        n.f("spacing", x.spacing, 0.0f, 64.0f);
        n.f("width", x.width, 80.0f, 2000.0f);
        n.f("min_height", x.min_height, 8.0f, 400.0f);
        n.f("padding_x", x.padding_x, 0.0f, 64.0f);
        n.f("padding_y", x.padding_y, 0.0f, 64.0f);
        n.b("merge_duplicates", x.merge_duplicates);
        n.i("suppress_after_connect_ms", x.suppress_after_connect_ms, 0, 60000);
        read(n.sub("join"), x.join);
        read(n.sub("leave"), x.leave);
        read(n.sub("channel_switch"), x.channel_switch);
        read(n.sub("connection"), x.connection);
        read(n.sub("whisper"), x.whisper);
        read(n.sub("chat"), x.chat);
    }
    {
        const R h = r.sub("chat");
        ChatConfig& x = c.chat;
        read(h.sub("placement"), x.placement);
        h.b("show_channel_messages", x.show_channel_messages);
        h.b("show_server_messages", x.show_server_messages);
        h.b("show_private_messages", x.show_private_messages);
        h.en("order", x.order);
        h.i("max_visible_messages", x.max_visible_messages, 1, 50);
        h.i("history_size", x.history_size, 1, 500);
        h.i("max_message_length", x.max_message_length, 1,
            static_cast<int>(proto::kMaxChatChars));
        h.i("retention_seconds", x.retention_seconds, 0, 86400);
        h.b("show_timestamp", x.show_timestamp);
        h.b("show_sender", x.show_sender);
        h.b("show_channel_name", x.show_channel_name);
        h.b("show_category_icon", x.show_category_icon);
        h.b("wrap", x.wrap);
        h.f("width", x.width, 80.0f, 4000.0f);
        h.f("font_scale", x.font_scale, 0.2f, 6.0f);
        h.col("text", x.text);
        h.col("sender", x.sender);
        h.col("timestamp", x.timestamp);
        h.col("background", x.background);
        h.b("show_background", x.show_background);
        h.b("use_sender_color", x.use_sender_color);
    }
    {
        const R a = r.sub("animation");
        AnimationConfig& x = c.animation;
        a.b("enabled", x.enabled);
        a.en("speaking", x.speaking);
        a.i("speaking_attack_ms", x.speaking_attack_ms, 0, 5000);
        a.i("speaking_release_ms", x.speaking_release_ms, 0, 5000);
        a.f("speaking_pulse_hz", x.speaking_pulse_hz, 0.1f, 20.0f);
        a.f("speaking_pulse_depth", x.speaking_pulse_depth, 0.0f, 1.0f);
        a.en("state_easing", x.state_easing);
        a.i("state_transition_ms", x.state_transition_ms, 0, 5000);
        a.b("animate_list_reorder", x.animate_list_reorder);
        a.i("list_reorder_ms", x.list_reorder_ms, 0, 5000);
        read(a.sub("overlay_fade"), x.overlay_fade);
        a.b("fade_when_idle", x.fade_when_idle);
        a.i("idle_after_ms", x.idle_after_ms, 1000, 3600000);
        a.f("idle_opacity", x.idle_opacity, 0.0f, 1.0f);
    }
    {
        const R i = r.sub("integration");
        IntegrationConfig& x = c.integration;
        i.s("pipe_name", x.pipe_name, 200);
        i.i("reconnect_initial_ms", x.reconnect_initial_ms, 50, 60000);
        i.i("reconnect_max_ms", x.reconnect_max_ms, 100, 300000);
        i.i("stale_after_ms", x.stale_after_ms, 1000, 120000);
        i.i("ping_interval_ms", x.ping_interval_ms, 1000, 600000);
        i.b("auto_connect", x.auto_connect);
    }
    {
        const R l = r.sub("logging");
        LoggingConfig& x = c.logging;
        l.s("level", x.level, 16);
        l.b("to_file", x.to_file);
        l.s("file_name", x.file_name, 260);
        l.i("max_file_kb", x.max_file_kb, 16, 102400);
        l.b("include_message_content", x.include_message_content);
        l.b("show_diagnostics_overlay", x.show_diagnostics_overlay);
    }
    if (const json::Value* uo = root.find("user_overrides")) {
        for (const auto& [key, val] : uo->as_object()) {
            if (key.empty() || key.size() > 128) {
                diag.add(ConfigIssue::Severity::Warning, "user_overrides",
                         "entry with an implausible identity key was dropped");
                continue;
            }
            UserOverride ov;
            read(R{val, diag, "user_overrides." + key}, ov);
            c.user_overrides.emplace(key, std::move(ov));
        }
    }
    if (const json::Value* co = root.find("channel_overrides")) {
        for (const auto& [key, val] : co->as_object()) {
            // Enforced key shape "<server-uid>:<channel-id>": a bare channel id is ambiguous
            // across servers and would apply one server's theme to another's channel.
            if (key.find(':') == std::string::npos) {
                diag.add(ConfigIssue::Severity::Warning, "channel_overrides." + key,
                         "key must be '<server_unique_id>:<channel_id>'; entry dropped");
                continue;
            }
            ChannelOverride ov;
            read(R{val, diag, "channel_overrides." + key}, ov);
            c.channel_overrides.emplace(key, std::move(ov));
        }
    }

    c.unknown.clear();
    for (const auto& [k, v] : root.as_object()) {
        if (!is_known_section(k)) c.unknown.emplace_back(k, v);
    }
    if (!c.unknown.empty()) {
        diag.add(ConfigIssue::Severity::Info, "",
                 "configuration contains keys this build does not use; they were preserved");
    }

    c.clamp(diag);
    return c;
}

void Config::clamp(ConfigDiagnostics& diag) {
    if (integration.reconnect_max_ms < integration.reconnect_initial_ms) {
        diag.add(ConfigIssue::Severity::Info, "integration.reconnect_max_ms",
                 "was below reconnect_initial_ms; raised to match");
        integration.reconnect_max_ms = integration.reconnect_initial_ms;
    }
    if (chat.max_visible_messages > chat.history_size) {
        diag.add(ConfigIssue::Severity::Info, "chat.max_visible_messages",
                 "cannot exceed history_size; lowered");
        chat.max_visible_messages = chat.history_size;
    }
    if (user_list.min_font_scale > 1.0f) user_list.min_font_scale = 1.0f;
    static constexpr const char* kLevels[] = {"trace", "debug", "info", "warn", "error", "off"};
    bool level_ok = false;
    for (const char* l : kLevels) {
        if (logging.level == l) level_ok = true;
    }
    if (!level_ok) {
        diag.add(ConfigIssue::Severity::Warning, "logging.level",
                 "unrecognised level; reset to 'info'");
        logging.level = "info";
    }
}

Config Config::parse(std::string_view text, ConfigDiagnostics& diag) {
    json::Limits lim;
    // Configuration files are legitimately larger and more deeply nested than wire messages.
    lim.max_total_bytes = 4u * 1024u * 1024u;
    lim.max_object_members = 4096;
    lim.max_array_elements = 4096;
    lim.max_depth = 24;
    lim.max_string_bytes = 4096;

    json::ParseResult p = json::parse(text, lim);
    if (!p.ok) {
        diag.add(ConfigIssue::Severity::Error, "", "could not be parsed: " + p.error);
        diag.from_defaults = true;
        return Config::defaults();
    }
    diag.loaded_version = static_cast<int>(p.value.get_int("config_version", kConfigVersion));
    if (!migrate(p.value, diag)) {
        diag.from_defaults = true;
        return Config::defaults();
    }
    return Config::from_json(p.value, diag);
}

std::string Config::serialise() const { return to_json().dump(); }

const UserOverride* Config::find_user_override(std::string_view unique_id) const {
    if (unique_id.empty()) return nullptr;
    const auto it = user_overrides.find(std::string(unique_id));
    if (it == user_overrides.end() || !it->second.enabled) return nullptr;
    return &it->second;
}

const ChannelOverride* Config::find_channel_override(std::string_view server_uid,
                                                     std::uint64_t channel_id) const {
    if (server_uid.empty() || channel_id == 0) return nullptr;
    const auto it = channel_overrides.find(make_channel_key(server_uid, channel_id));
    if (it == channel_overrides.end() || !it->second.enabled) return nullptr;
    return &it->second;
}

bool migrate(json::Value& doc, ConfigDiagnostics& diag) {
    if (!doc.is_object()) return false;
    long long v = doc.get_int("config_version", 0);
    if (v <= 0) {
        // Pre-versioning or hand-written: treat as v1 and let field validation do the rest.
        diag.add(ConfigIssue::Severity::Info, "config_version",
                 "missing; assuming version 1");
        doc.set("config_version", json::Value(1));
        v = 1;
    }
    if (v > kConfigVersion) {
        diag.add(ConfigIssue::Severity::Warning, "config_version",
                 "file was written by a newer version; unknown settings are preserved but not "
                 "applied, and saving will rewrite it in this version's format");
        diag.newer_than_supported = true;
        return true;
    }
    // Each step upgrades exactly one version, so a file from any past release converges.
    // Add a step here when kConfigVersion is raised, e.g.:
    //     if (v == 1) { migrate_v1_to_v2(doc); }
    //     else { ...unknown... }
    while (v < kConfigVersion) {
        diag.add(ConfigIssue::Severity::Error, "config_version",
                 "no migration path from this version; using defaults");
        return false;
    }
    return true;
}

}  // namespace tsro
