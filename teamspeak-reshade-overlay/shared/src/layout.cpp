// SPDX-License-Identifier: MIT
#include "tsro/layout.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace tsro {
namespace {

bool is_left(Anchor a) noexcept {
    return a == Anchor::TopLeft || a == Anchor::CenterLeft || a == Anchor::BottomLeft;
}
bool is_right(Anchor a) noexcept {
    return a == Anchor::TopRight || a == Anchor::CenterRight || a == Anchor::BottomRight;
}
bool is_top(Anchor a) noexcept {
    return a == Anchor::TopLeft || a == Anchor::TopCenter || a == Anchor::TopRight;
}
bool is_bottom(Anchor a) noexcept {
    return a == Anchor::BottomLeft || a == Anchor::BottomCenter || a == Anchor::BottomRight;
}

std::string to_str(int v) {
    char buf[16];
    const int n = std::snprintf(buf, sizeof(buf), "%d", v);
    return std::string(buf, static_cast<std::size_t>(n > 0 ? n : 0));
}

/// Lowercase ASCII fold for sorting. Deliberately not locale-aware: a locale-dependent order
/// would make the list jump around between machines, and TeamSpeak nicknames are arbitrary
/// Unicode for which no single collation is right anyway.
int compare_names(const std::string& a, const std::string& b) {
    const std::size_t n = std::min(a.size(), b.size());
    for (std::size_t i = 0; i < n; ++i) {
        unsigned char ca = static_cast<unsigned char>(a[i]);
        unsigned char cb = static_cast<unsigned char>(b[i]);
        if (ca >= 'A' && ca <= 'Z') ca = static_cast<unsigned char>(ca + 32);
        if (cb >= 'A' && cb <= 'Z') cb = static_cast<unsigned char>(cb + 32);
        if (ca != cb) return ca < cb ? -1 : 1;
    }
    if (a.size() == b.size()) return 0;
    return a.size() < b.size() ? -1 : 1;
}

void push_indicator(std::vector<ResolvedUser::Indicator>& out, const StateStyle& s,
                    const std::optional<Color>& override_color) {
    if (!s.enabled || !s.show_icon || s.icon == IconShape::None) return;
    ResolvedUser::Indicator i;
    i.shape = s.icon;
    i.color = override_color.value_or(s.icon_color);
    i.scale = s.icon_scale;
    out.push_back(i);
}

}  // namespace

Rect resolve_placement(const Placement& p, float w, float h, const Viewport& vp) noexcept {
    const float ox = p.percent ? p.x * vp.width : p.x;
    const float oy = p.percent ? p.y * vp.height : p.y;

    Rect r;
    r.w = w;
    r.h = h;

    if (is_left(p.anchor)) {
        r.x = ox;
    } else if (is_right(p.anchor)) {
        r.x = vp.width - ox - w;
    } else {
        r.x = (vp.width - w) * 0.5f + ox;
    }

    if (is_top(p.anchor)) {
        r.y = oy;
    } else if (is_bottom(p.anchor)) {
        r.y = vp.height - oy - h;
    } else {
        r.y = (vp.height - h) * 0.5f + oy;
    }
    return r;
}

float align_offset(Align a, float box_w, float text_w) noexcept {
    switch (a) {
        case Align::Left: return 0.0f;
        case Align::Center: return (box_w - text_w) * 0.5f;
        case Align::Right: return box_w - text_w;
    }
    return 0.0f;
}

FittedText fit_text(std::string_view text, float max_width, float font_size, OverflowMode mode,
                    float min_font_scale, const MeasureFn& measure, float time_s) {
    FittedText out;
    out.text = std::string(text);
    out.font_scale = 1.0f;
    if (!measure || max_width <= 0.0f) {
        out.lines.push_back(out.text);
        return out;
    }
    const float full = measure(text, font_size);
    out.width = full;
    if (full <= max_width) {
        out.lines.push_back(out.text);
        return out;
    }

    switch (mode) {
        case OverflowMode::Clip: {
            // Binary search the longest prefix that fits, on UTF-8 boundaries.
            std::size_t lo = 0, hi = text.size();
            while (lo < hi) {
                std::size_t mid = (lo + hi + 1) / 2;
                while (mid > lo && mid < text.size() &&
                       (static_cast<unsigned char>(text[mid]) & 0xC0) == 0x80)
                    --mid;
                if (mid == lo) break;
                if (measure(text.substr(0, mid), font_size) <= max_width) lo = mid;
                else hi = mid - 1;
            }
            out.text = std::string(text.substr(0, lo));
            out.truncated = true;
            break;
        }
        case OverflowMode::Ellipsis: {
            static constexpr std::string_view kEllipsis = "\xE2\x80\xA6";  // U+2026
            const float ell = measure(kEllipsis, font_size);
            const float budget = max_width - ell;
            std::size_t keep = 0;
            if (budget > 0.0f) {
                for (std::size_t i = 1; i <= text.size(); ++i) {
                    if (i < text.size() &&
                        (static_cast<unsigned char>(text[i]) & 0xC0) == 0x80)
                        continue;
                    if (measure(text.substr(0, i), font_size) <= budget) keep = i;
                    else break;
                }
            }
            out.text = std::string(text.substr(0, keep));
            out.text.append(kEllipsis);
            out.truncated = true;
            break;
        }
        case OverflowMode::Shrink: {
            float scale = max_width / full;
            if (scale < min_font_scale) scale = min_font_scale;
            out.font_scale = scale;
            // Even at the floor the text may not fit, so clip whatever is still over.
            if (measure(text, font_size * scale) > max_width) {
                FittedText clipped = fit_text(text, max_width, font_size * scale,
                                              OverflowMode::Ellipsis, min_font_scale, measure);
                out.text = clipped.text;
                out.truncated = true;
            }
            break;
        }
        case OverflowMode::Wrap: {
            std::size_t start = 0;
            while (start < text.size()) {
                std::size_t best = start;
                std::size_t last_space = std::string_view::npos;
                for (std::size_t i = start + 1; i <= text.size(); ++i) {
                    if (i < text.size() &&
                        (static_cast<unsigned char>(text[i]) & 0xC0) == 0x80)
                        continue;
                    if (measure(text.substr(start, i - start), font_size) > max_width) break;
                    best = i;
                    if (i < text.size() && text[i - 1] == ' ') last_space = i;
                }
                if (best == start) {
                    // A single glyph wider than the box: emit it anyway to guarantee progress.
                    best = start + 1;
                    while (best < text.size() &&
                           (static_cast<unsigned char>(text[best]) & 0xC0) == 0x80)
                        ++best;
                }
                std::size_t cut = (last_space != std::string_view::npos && last_space > start)
                                      ? last_space
                                      : best;
                out.lines.push_back(std::string(text.substr(start, cut - start)));
                start = cut;
                while (start < text.size() && text[start] == ' ') ++start;
                if (out.lines.size() >= 8) break;  // hard cap: never let one name own the panel
            }
            out.truncated = out.lines.size() > 1;
            out.text = out.lines.empty() ? std::string() : out.lines.front();
            return out;
        }
        case OverflowMode::Scroll: {
            // Marquee: travel the overhang plus a gap, pause at both ends.
            const float overhang = full - max_width;
            const float gap = 24.0f;
            const float span = overhang + gap;
            const float period = 2.0f + span / 40.0f;
            float phase = std::fmod(time_s, period * 2.0f);
            if (phase < 0.0f) phase += period * 2.0f;
            float t = phase < period ? phase / period : (period * 2.0f - phase) / period;
            t = std::clamp(t * 1.25f - 0.125f, 0.0f, 1.0f);  // dwell at each end
            out.scroll_offset = overhang * t;
            out.truncated = true;
            break;
        }
    }
    out.lines.push_back(out.text);
    out.width = measure(out.text, font_size * out.font_scale);
    return out;
}

ResolvedUser resolve_user(const UserState& u, const Config& cfg, float speaking_intensity) {
    const UserOverride* ov = cfg.find_user_override(u.unique_id);
    ResolvedUser r;
    r.user = &u;
    r.display = (ov != nullptr && !ov->display_override.empty())
                    ? ov->display_override
                    : (u.display_name.empty() ? u.nickname : u.display_name);

    // Friendship comes from TeamSpeak's own Contacts list, which the plugin reads from the
    // client's settings and sends alongside everything else. Marking someone here is still
    // honoured -- it is an addition to that list, not a replacement for it, so a friend the
    // client does not know about can still be given a colour and a name.
    // When the raw contact value is available, the configured value decides; the plugin's own
    // classification is the fallback for a client that did not report one.
    bool teamspeak_says_friend = u.is_friend.value_or(false);
    if (u.contact_flag.has_value() && cfg.user_list.teamspeak_friend_value >= 0) {
        teamspeak_says_friend = *u.contact_flag == cfg.user_list.teamspeak_friend_value;
    }
    const bool from_teamspeak = cfg.user_list.use_teamspeak_friends && teamspeak_says_friend;
    r.is_friend = from_teamspeak || (ov != nullptr && ov->is_friend);

    // The tag the user typed here wins over the one TeamSpeak has, because someone who set one
    // in this window meant it for this window.
    std::string tag;
    if (ov != nullptr && !ov->friend_tag.empty()) {
        tag = ov->friend_tag;
    } else if (from_teamspeak) {
        tag = u.friend_nickname;
    }
    if (r.is_friend && cfg.user_list.show_friend_tag && !tag.empty()) {
        r.friend_tag = "[" + tag + "] ";
        r.display = r.friend_tag + r.display;
    }

    const IndicatorsConfig& ind = cfg.indicators;
    r.name_color = cfg.appearance.text_default;
    // A friend colour is a default, not an override: an explicit per-user colour still wins.
    if (r.is_friend && cfg.user_list.color_friends) r.name_color = cfg.user_list.friend_color;
    if (ov != nullptr && ov->name_color) r.name_color = *ov->name_color;
    if (u.is_self && cfg.user_list.highlight_local_user && (ov == nullptr || !ov->name_color))
        r.name_color = cfg.user_list.local_user_color;

    // State priority, least to most important: a later match overwrites the text colour, so
    // speaking wins over away, and away wins over nothing.
    auto apply_text = [&](const StateStyle& s, const std::optional<Color>& custom) {
        if (!s.enabled) return;
        if (custom.has_value()) r.name_color = *custom;
        else if (s.override_text_color) r.name_color = s.text_color;
        if (s.dim_entry) r.entry_opacity = std::min(r.entry_opacity, 1.0f - s.dim_amount);
        r.entry_opacity *= s.opacity;
    };

    const bool mic_muted = u.input_muted.value_or(false) || u.input_deactivated.value_or(false);
    const bool spk_muted = u.output_muted.value_or(false);
    const bool away = u.away.value_or(false);
    const bool locally_muted = u.locally_muted.value_or(false);
    const bool suppressed = u.suppressed().value_or(false);

    if (away) apply_text(ind.away, std::nullopt);
    if (suppressed && ind.suppressed.enabled) apply_text(ind.suppressed, std::nullopt);
    if (locally_muted) apply_text(ind.locally_muted, std::nullopt);
    // Speakers last, so speaker mute wins when both are set. It is the more consequential of
    // the two -- someone with their mic off can still hear you; someone with their speakers off
    // cannot -- and TeamSpeak sets both whenever the speaker button is pressed, so without this
    // ordering the speaker state would never be the one shown.
    if (mic_muted) apply_text(ind.mic_muted, ov ? ov->muted_color : std::nullopt);
    if (spk_muted) apply_text(ind.speaker_muted, ov ? ov->muted_color : std::nullopt);

    // Who someone is outranks what they are currently doing. A friend who has muted their mic
    // is still a friend, and the mute is already unmistakable from its icon and the dimmed row,
    // so nothing is lost by keeping the name in its identity colour -- whereas losing it means
    // the friend colour looks like it does nothing, which is exactly how this was reported.
    //
    // Your own row is deliberately not in this: your own mute state is feedback you want, and
    // you already know which row is yours.
    if (ov != nullptr && ov->name_color) {
        r.name_color = *ov->name_color;
    } else if (r.is_friend && cfg.user_list.color_friends) {
        r.name_color = cfg.user_list.friend_color;
    }

    if (u.talking && ind.speaking.enabled) {
        r.speaking = true;
        const Color base = ov && ov->speaking_color ? *ov->speaking_color
                                                    : (ind.speaking.override_text_color
                                                           ? ind.speaking.text_color
                                                           : r.name_color);
        // The animation envelope blends between the resting and speaking colours rather than
        // switching, so a short burst does not flash.
        const float t = std::clamp(speaking_intensity, 0.0f, 1.0f);
        r.name_color = Color{
            static_cast<std::uint8_t>(r.name_color.r + (base.r - r.name_color.r) * t),
            static_cast<std::uint8_t>(r.name_color.g + (base.g - r.name_color.g) * t),
            static_cast<std::uint8_t>(r.name_color.b + (base.b - r.name_color.b) * t),
            r.name_color.a};
        if (ind.speaking.show_border) {
            r.show_border = true;
            r.border = ind.speaking.border.with_alpha_scale(t);
            r.border_thickness = ind.speaking.border_thickness;
        }
        if (ind.speaking.glow) {
            r.glow = true;
            r.glow_color = ind.speaking.glow_color.with_alpha_scale(t);
            r.glow_radius = ind.speaking.glow_radius;
        }
        if (ind.speaking.show_background) {
            r.show_background = true;
            r.background = ind.speaking.background.with_alpha_scale(t);
        }
    }

    if (u.whispering_to_me && ind.whispering.enabled) {
        if (ind.whispering.override_text_color) r.name_color = ind.whispering.text_color;
        if (ind.whispering.show_border) {
            r.show_border = true;
            r.border = ind.whispering.border;
            r.border_thickness = ind.whispering.border_thickness;
        }
        if (ind.whispering.glow) {
            r.glow = true;
            r.glow_color = ind.whispering.glow_color;
            r.glow_radius = ind.whispering.glow_radius;
        }
    }

    // Leading indicators: Channel Commander sits immediately before the name, as specified.
    if (u.channel_commander.value_or(false)) {
        push_indicator(r.leading, ind.commander, ov ? ov->commander_color : std::nullopt);
    }
    if (u.priority_speaker.value_or(false)) {
        push_indicator(r.leading, ind.priority_speaker, std::nullopt);
    }
    if (ov != nullptr && ov->icon.has_value() && *ov->icon != IconShape::None) {
        ResolvedUser::Indicator custom;
        custom.shape = *ov->icon;
        custom.color = ov->icon_color.value_or(r.name_color);
        r.leading.push_back(custom);
    }

    // Trailing indicators, in a fixed order so rows stay visually aligned.
    if (u.whispering_to_me) push_indicator(r.trailing, ind.whispering, std::nullopt);
    if (mic_muted) push_indicator(r.trailing, ind.mic_muted, ov ? ov->muted_color : std::nullopt);
    if (spk_muted) push_indicator(r.trailing, ind.speaker_muted, std::nullopt);
    if (!u.input_hardware.value_or(true)) {
        push_indicator(r.trailing, ind.mic_hardware_off, std::nullopt);
    }
    if (away) push_indicator(r.trailing, ind.away, std::nullopt);
    if (u.recording.value_or(false)) push_indicator(r.trailing, ind.recording, std::nullopt);
    if (locally_muted) push_indicator(r.trailing, ind.locally_muted, std::nullopt);
    if (suppressed) push_indicator(r.trailing, ind.suppressed, std::nullopt);
    if (u.talking && !u.whispering_to_me) {
        push_indicator(r.trailing, ind.speaking, ov ? ov->speaking_color : std::nullopt);
    }

    r.entry_opacity = std::clamp(r.entry_opacity, 0.0f, 1.0f);
    return r;
}

std::vector<const UserState*> order_users(const OverlayState& state, const Config& cfg) {
    std::vector<const UserState*> out;
    out.reserve(state.users.size());
    for (const auto& u : state.users) {
        if (u.is_self && !cfg.user_list.show_local_user) continue;
        // Stealth: the roster collapses to whoever is actually speaking. Your own row stays so
        // the overlay does not vanish entirely while you are the only one talking.
        if (cfg.user_list.only_show_talking && !u.talking && !u.is_self) continue;
        if (!cfg.user_list.show_muted_users && !u.is_self &&
            (u.input_muted.value_or(false) || u.output_muted.value_or(false)))
            continue;
        out.push_back(&u);
    }

    const UserSort mode = cfg.user_list.sort;
    std::stable_sort(out.begin(), out.end(), [&](const UserState* a, const UserState* b) {
        if (cfg.user_list.speaking_first || mode == UserSort::SpeakingFirst) {
            if (a->talking != b->talking) return a->talking;
        }
        switch (mode) {
            case UserSort::Alphabetical:
            case UserSort::SpeakingFirst:
                return compare_names(a->display_name.empty() ? a->nickname : a->display_name,
                                     b->display_name.empty() ? b->nickname : b->display_name) < 0;
            case UserSort::TalkPower: {
                const int pa = a->talk_power.value_or(0);
                const int pb = b->talk_power.value_or(0);
                if (pa != pb) return pa > pb;
                return compare_names(a->nickname, b->nickname) < 0;
            }
            case UserSort::ChannelOrder:
                // TeamSpeak does not expose an in-channel ordering to plugins, so "channel
                // order" means the order the server listed clients in. stable_sort preserves it.
                return false;
        }
        return false;
    });
    return out;
}

std::string format_template(std::string_view tmpl, const FormatValues& v) {
    std::string out;
    out.reserve(tmpl.size() + 32);
    for (std::size_t i = 0; i < tmpl.size(); ++i) {
        if (tmpl[i] != '{') {
            out.push_back(tmpl[i]);
            continue;
        }
        const std::size_t close = tmpl.find('}', i);
        if (close == std::string_view::npos) {
            out.append(tmpl.substr(i));
            break;
        }
        const std::string_view key = tmpl.substr(i + 1, close - i - 1);
        const std::string* sub = nullptr;
        if (key == "name") sub = &v.name;
        else if (key == "channel") sub = &v.channel;
        else if (key == "parent") sub = &v.parent;
        else if (key == "previous") sub = &v.previous;
        else if (key == "count") sub = &v.count;
        else if (key == "status") sub = &v.status;
        else if (key == "message") sub = &v.message;
        else if (key == "server") sub = &v.server;
        else if (key == "time") sub = &v.time;
        if (sub != nullptr) {
            out.append(*sub);
        } else {
            // Unknown placeholder: echo it so a typo is visible in the overlay.
            out.append(tmpl.substr(i, close - i + 1));
        }
        i = close;
    }
    return out;
}

LayoutResult compute_layout(const OverlayState& state, const Config& cfg, const Viewport& vp,
                            const MeasureFn& measure, float time_s,
                            const std::function<float(const std::string&)>& speaking_intensity) {
    LayoutResult out;
    out.viewport = vp;
    out.scale = cfg.general.scale;

    // Three scales multiply: the global one, the group's (when the blocks are linked) and the
    // element's own. That is what lets "resize everything" and "resize just this" both be a
    // single control instead of one fighting the other.
    const float group_s = cfg.group.enabled ? cfg.group.scale : 1.0f;
    const float title_s = cfg.general.scale * group_s * cfg.channel_title.scale;
    const float s = cfg.general.scale * group_s * cfg.user_list.scale;

    const float font = cfg.appearance.font_size * s;
    const float pad_x = cfg.appearance.padding_x * s;
    const float pad_y = cfg.appearance.padding_y * s;
    const float row_h = cfg.appearance.row_height * s;
    const float row_gap = cfg.appearance.row_spacing * s;
    const float icon = cfg.appearance.icon_size * s;
    const float gap = cfg.user_list.indicator_gap * s;

    // One source of truth for alignment: the group's when linked, the element's own otherwise.
    const Align content_align =
        cfg.group.enabled ? cfg.group.align : cfg.user_list.placement.align;
    const Align title_align =
        cfg.group.enabled ? cfg.group.align : cfg.channel_title.placement.align;

    const bool connected = state.server.connection == ConnectionState::Connected;
    out.degraded = !connected || !state.synchronised || state.stale;
    if (!connected) out.degraded_reason = "not connected to TeamSpeak";
    else if (state.stale) out.degraded_reason = "no update from TeamSpeak";
    else if (!state.synchronised) out.degraded_reason = "synchronising";

    // --- channel title ---
    const ChannelTitleConfig& tc = cfg.channel_title;
    if (tc.placement.visible) {
        const ChannelOverride* co =
            cfg.find_channel_override(state.server.unique_id, state.channel.id);

        std::string text;
        if (!connected || !state.channel.valid()) {
            text = tc.disconnected_text;
        } else {
            FormatValues fv;
            fv.channel = (co != nullptr && !co->display_override.empty()) ? co->display_override
                                                                         : state.channel.name;
            fv.parent = state.channel.parent_name;
            fv.previous = state.channel.parent_name;
            fv.server = state.server.name;
            fv.count = to_str(static_cast<int>(state.users.size()));
            const bool with_parent = tc.show_parent && !state.channel.parent_name.empty();
            text = format_template(with_parent ? tc.parent_format : tc.format, fv);
            if (tc.show_server_name && !state.server.name.empty())
                text = state.server.name + " - " + text;
            if (tc.show_user_count) text += " (" + fv.count + ")";
            if (tc.show_topic && !state.channel.topic.empty()) text += " - " + state.channel.topic;
        }

        const float title_font = cfg.appearance.font_size * title_s * tc.font_scale *
                                 (co && co->font_scale ? *co->font_scale : 1.0f);
        const float title_pad_x = cfg.appearance.padding_x * title_s;
        const float title_pad_y = cfg.appearance.padding_y * title_s;
        const float title_icon = cfg.appearance.icon_size * title_s;
        const float title_gap = cfg.user_list.indicator_gap * title_s;
        // Cap the title's width. Without this a long channel name simply keeps growing and
        // runs off whichever edge the overlay is anchored to -- which is what happened with
        // "Parole Administrator I Aubrey Huy". With a cap, a right-anchored title keeps its
        // right edge pinned and grows leftward until it hits the limit, then ellipsises.
        const float chrome = title_pad_x * 2.0f +
                             (tc.icon != IconShape::None ? title_icon + title_gap : 0.0f);
        // An explicit max_width wins; otherwise allow the viewport minus a margin on each side,
        // so the title can be long but can never leave the screen.
        const float screen_limit = std::max(60.0f, vp.width * 0.9f - chrome);
        const float limit = tc.max_width > 0.0f
                                ? std::min(tc.max_width * title_s, screen_limit)
                                : screen_limit;

        const FittedText fitted =
            fit_text(text, limit, title_font, OverflowMode::Ellipsis, 0.75f, measure);
        text = fitted.text;

        const float text_w = measure ? measure(text, title_font) : 0.0f;
        const float w = text_w + chrome;
        const float h = title_font + title_pad_y * 2.0f;

        out.title.visible = true;
        out.title.align = title_align;
        out.title.rect = resolve_placement(tc.placement, w, h, vp);
        out.title.text = std::move(text);
        out.title.text_color = co && co->title_color ? *co->title_color : tc.text;
        out.title.background = co && co->background ? *co->background : tc.background;
        out.title.border = co && co->border ? *co->border : tc.border;
        out.title.show_background = tc.show_background;
        out.title.show_border = tc.show_border;
        out.title.icon = co && co->icon ? *co->icon : tc.icon;
        out.title.icon_color = co && co->icon_color ? *co->icon_color : tc.icon_color;
        out.title.font_size = title_font;
        out.title.opacity = tc.opacity * (co && co->opacity ? *co->opacity : 1.0f);
    }

    // --- user list ---
    const UserListConfig& ul = cfg.user_list;
    if (ul.placement.visible && connected) {
        const std::vector<const UserState*> ordered = order_users(state, cfg);
        const std::size_t limit =
            std::min<std::size_t>(ordered.size(), static_cast<std::size_t>(ul.max_visible_users));
        out.users.hidden_count = static_cast<int>(ordered.size() - limit);

        out.users.rows.reserve(limit);
        float widest = 0.0f;
        const float max_name_w = ul.max_name_width * s;

        for (std::size_t i = 0; i < limit; ++i) {
            const UserState& u = *ordered[i];
            const float intensity =
                speaking_intensity ? speaking_intensity(u.unique_id) : (u.talking ? 1.0f : 0.0f);
            UserRowLayout row;
            row.resolved = resolve_user(u, cfg, intensity);
            row.font_size = font;
            row.name = fit_text(row.resolved.display, max_name_w, font, ul.name_overflow,
                                ul.min_font_scale, measure, time_s);
            const float icons_w =
                static_cast<float>(row.resolved.leading.size() + row.resolved.trailing.size()) *
                (icon + gap);
            const float row_w = row.name.width + icons_w + pad_x * 2.0f;
            widest = std::max(widest, row_w);
            out.users.rows.push_back(std::move(row));
        }

        if (out.users.hidden_count > 0 && ul.show_overflow_count) {
            out.users.overflow_text = "+" + to_str(out.users.hidden_count) + " more";
            if (measure)
                widest = std::max(widest, measure(out.users.overflow_text, font) + pad_x * 2.0f);
        }

        const std::size_t visible_rows =
            out.users.rows.size() + (out.users.overflow_text.empty() ? 0u : 1u);
        const float list_h = visible_rows == 0
                                 ? 0.0f
                                 : static_cast<float>(visible_rows) * row_h +
                                       static_cast<float>(visible_rows - 1) * row_gap +
                                       pad_y * 2.0f;

        out.users.visible = visible_rows > 0;
        out.users.align = content_align;
        out.users.rect = resolve_placement(ul.placement, std::max(widest, 80.0f), list_h, vp);

        // Row rectangles are placed relative to the resolved list rect, so the whole list moves
        // as one when the anchor or resolution changes.
        float y = out.users.rect.y + pad_y;
        for (auto& row : out.users.rows) {
            row.rect = Rect{out.users.rect.x + pad_x, y, out.users.rect.w - pad_x * 2.0f, row_h};
            y += row_h + row_gap;
        }
    }

    // --- group: stack the title above the list and place the pair as one block ---
    if (cfg.group.enabled && (out.title.visible || out.users.visible)) {
        Placement group_placement;
        group_placement.visible = true;
        group_placement.anchor = cfg.group.anchor;
        group_placement.x = cfg.group.x;
        group_placement.y = cfg.group.y;
        group_placement.percent = cfg.group.percent;
        group_placement.align = cfg.group.align;

        const float spacing = cfg.group.spacing * cfg.general.scale * group_s;
        const float title_h = out.title.visible ? out.title.rect.h : 0.0f;
        const float list_h = out.users.visible ? out.users.rect.h : 0.0f;
        const float gap_h = (out.title.visible && out.users.visible) ? spacing : 0.0f;
        const float block_w = std::max(out.title.visible ? out.title.rect.w : 0.0f,
                                       out.users.visible ? out.users.rect.w : 0.0f);
        const float block_h = title_h + gap_h + list_h;

        Rect block = resolve_placement(group_placement, block_w, block_h, vp);
        // Right-anchored means the right edge is the fixed point: content grows leftward. Clamp
        // so the block can never be pushed past either edge of the screen, whichever way it
        // grew.
        if (block.right() > vp.width) block.x = vp.width - block.w;
        if (block.x < 0.0f) block.x = 0.0f;
        if (block.bottom() > vp.height) block.y = vp.height - block.h;
        if (block.y < 0.0f) block.y = 0.0f;

        // Each block keeps its own width but is aligned inside the group box, so a short title
        // over a wide list stays flush with whichever edge the group is anchored to.
        const auto align_in_block = [&](float w) {
            return block.x + align_offset(cfg.group.align, block_w, w);
        };

        float y = block.y;
        if (out.title.visible) {
            out.title.rect.x = align_in_block(out.title.rect.w);
            out.title.rect.y = y;
            y += out.title.rect.h + gap_h;
        }
        if (out.users.visible) {
            const float dx = align_in_block(out.users.rect.w) - out.users.rect.x;
            const float dy = y - out.users.rect.y;
            out.users.rect.x += dx;
            out.users.rect.y += dy;
            // Rows were positioned relative to the old rect; move them with it.
            for (UserRowLayout& row : out.users.rows) {
                row.rect.x += dx;
                row.rect.y += dy;
            }
        }
    }

    return out;
}

}  // namespace tsro
