// SPDX-License-Identifier: MIT
#include "renderer.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cfloat>
#include <cstdio>

#include <imgui.h>
// reshade.hpp must follow imgui.h: it supplies the inline definitions for ImGui:: and
// ImDrawList:: that route through ReShade's function table. imgui.h alone only declares them,
// so omitting this compiles cleanly and then fails at link with unresolved externals.
#include <reshade.hpp>

#include "icons.hpp"

namespace tsro::overlay {
namespace {

std::uint32_t packed(const Color& color, float opacity) {
    return color.with_alpha_scale(opacity).to_abgr();
}

/// The font the HUD draws with.
///
/// ReShade owns the Dear ImGui font atlas, and -- importantly -- ImFontAtlas is NOT part of the
/// function table ReShade exports. Reading io.Fonts->Fonts from an add-on therefore dereferences
/// struct offsets taken from *this* build's imgui.h against memory laid out by ReShade's own
/// ImGui build. When those differ it is a wild pointer read, which is exactly what crashed the
/// game when the font list was opened. The function table exists precisely because the layouts
/// cannot be assumed to match, so the only safe handle is the one ReShade hands back.
///
/// The typeface therefore follows ReShade's own font setting. docs/configuration.md explains how
/// to point ReShade at the Roboto (and other) .ttf files shipped in the `fonts` folder.
ImFont* overlay_font() { return ImGui::GetFont(); }

/// Estimated width of a run of text, used only when ReShade's ImGui refuses to measure.
///
/// A proportional UI face averages a little over half its pixel size per glyph, so counting
/// codepoints (not bytes -- UTF-8 continuation bytes are not glyphs) and scaling gets within a
/// few percent. That is wrong in the last pixel and right in the ones that matter: everything
/// stays on screen, in the right order, roughly where it belongs.
float estimate_text_width(std::string_view text, float font_size) {
    std::size_t glyphs = 0;
    for (const char c : text) {
        if ((static_cast<unsigned char>(c) & 0xC0) != 0x80) ++glyphs;
    }
    return static_cast<float>(glyphs) * font_size * 0.52f;
}

/// Which measurement route last answered. Purely diagnostic -- the debug panel shows it so a
/// measurement problem is visible in the overlay instead of having to be inferred from a
/// screenshot of misplaced text.
TextMetricsSource g_metrics_source = TextMetricsSource::Unknown;

/// Width of `text` when drawn at `font_size`.
///
/// This is the single most load-bearing number in the renderer: every right-aligned element is
/// positioned as `edge - width`, and every overflow budget is `panel - width`. A zero here does
/// not degrade the layout, it inverts it -- rows start at the right edge and grow off-screen,
/// notification bodies get a zero budget and vanish, and a chat line's body lands on top of its
/// own prefix. All three were reported together, which is what a zero looks like.
///
/// So measurement never returns zero for non-empty text. It only ever calls *through ReShade's
/// function table* -- reading ImGui structs directly is what crashed the font picker, proving the
/// layouts of this build's imgui.h and ReShade's own ImGui do not have to agree -- it tries both
/// table entries that can answer, and estimates if neither does.
float measure_text(std::string_view text, float font_size) {
    if (text.empty() || font_size <= 0.0f) return 0.0f;
    const char* const begin = text.data();
    const char* const end = begin + text.size();

    // The namespace-level entry measures at the *current* font size, so scale the result.
    const float base = ImGui::GetFontSize();
    if (base > 0.0f) {
        const float width = ImGui::CalcTextSize(begin, end, false, -1.0f).x;
        if (width > 0.0f && std::isfinite(width)) {
            g_metrics_source = TextMetricsSource::CalcTextSize;
            return width * (font_size / base);
        }
    }

    // The per-font entry takes the size directly, which avoids the scaling round-trip.
    if (ImFont* font = overlay_font(); font != nullptr) {
        const float width = font->CalcTextSizeA(font_size, FLT_MAX, 0.0f, begin, end).x;
        if (width > 0.0f && std::isfinite(width)) {
            g_metrics_source = TextMetricsSource::CalcTextSizeA;
            return width;
        }
    }

    g_metrics_source = TextMetricsSource::Estimated;
    return estimate_text_width(text, font_size);
}

/// Cheap change detector over the fields that affect geometry. Comparing a hash is far less
/// error-prone than remembering to add each new field to an equality check, and a false positive
/// only costs one extra layout pass.
std::uint64_t layout_hash(const Config& c) {
    std::uint64_t h = 1469598103934665603ULL;
    const auto mix = [&h](std::uint64_t value) {
        h ^= value;
        h *= 1099511628211ULL;
    };
    const auto mix_f = [&mix](float value) {
        mix(static_cast<std::uint64_t>(value * 1000.0f) + 0x9E3779B9ULL);
    };
    const auto mix_p = [&](const Placement& p) {
        mix(static_cast<std::uint64_t>(p.anchor));
        mix(static_cast<std::uint64_t>(p.align));
        mix_f(p.x);
        mix_f(p.y);
        mix(p.percent ? 1u : 0u);
        mix(p.visible ? 1u : 0u);
    };
    mix_f(c.general.scale);
    mix_f(c.appearance.font_size);
    mix_f(c.appearance.icon_size);
    mix_f(c.appearance.row_height);
    mix_f(c.appearance.row_spacing);
    mix_f(c.appearance.padding_x);
    mix_f(c.appearance.padding_y);
    mix_p(c.channel_title.placement);
    mix_p(c.user_list.placement);
    mix_p(c.notifications.placement);
    mix_p(c.chat.placement);
    mix_f(c.channel_title.font_scale);
    mix(c.channel_title.show_parent ? 1u : 0u);
    mix(c.channel_title.show_user_count ? 1u : 0u);
    mix(c.channel_title.show_server_name ? 1u : 0u);
    mix(c.channel_title.show_topic ? 1u : 0u);
    mix(static_cast<std::uint64_t>(c.user_list.sort));
    mix(static_cast<std::uint64_t>(c.user_list.name_overflow));
    mix_f(c.user_list.max_name_width);
    mix(static_cast<std::uint64_t>(c.user_list.max_visible_users));
    mix(c.user_list.show_local_user ? 1u : 0u);
    mix(c.user_list.show_muted_users ? 1u : 0u);
    mix(c.user_list.speaking_first ? 1u : 0u);
    mix(static_cast<std::uint64_t>(c.user_overrides.size()));
    mix(static_cast<std::uint64_t>(c.channel_overrides.size()));
    return h;
}

std::string format_clock(std::int64_t unix_ms) {
    const std::time_t seconds = static_cast<std::time_t>(unix_ms / 1000);
    std::tm tm{};
#if defined(_WIN32)
    localtime_s(&tm, &seconds);
#else
    localtime_r(&seconds, &tm);
#endif
    char buffer[16];
    std::snprintf(buffer, sizeof(buffer), "%02d:%02d", tm.tm_hour, tm.tm_min);
    return std::string(buffer);
}

const char* category_label(ChatCategory category) {
    switch (category) {
        case ChatCategory::Channel: return "#";
        case ChatCategory::Server: return "!";
        case ChatCategory::Private: return "@";
    }
    return "#";
}

UserState preview_user(const char* uid, const char* name) {
    UserState u;
    u.unique_id = uid;
    u.nickname = name;
    u.display_name = name;
    return u;
}

}  // namespace

TextMetricsSource text_metrics_source() noexcept { return g_metrics_source; }

const char* text_metrics_source_name(TextMetricsSource source) noexcept {
    switch (source) {
        case TextMetricsSource::CalcTextSize: return "ImGui::CalcTextSize";
        case TextMetricsSource::CalcTextSizeA: return "ImFont::CalcTextSizeA";
        case TextMetricsSource::Estimated: return "estimated (ImGui returned no width)";
        case TextMetricsSource::Unknown: break;
    }
    return "not measured yet";
}


OverlayState preview_state() {
    // Fixed sample data for the settings preview. It is never fed into the live model and never
    // reaches the notification pipeline as if it were a real event.
    OverlayState s;
    s.server.name = "Example TeamSpeak";
    s.server.unique_id = "preview-server=";
    s.server.connection = ConnectionState::Connected;
    s.channel.id = 1;
    s.channel.name = "Racing #1";
    s.channel.parent_name = "Games";
    s.channel.path = "Games/Racing #1";
    s.channel.topic = "Preview";
    s.self_unique_id = "preview-me=";
    s.synchronised = true;

    UserState me = preview_user("preview-me=", "You");
    me.is_self = true;
    me.input_muted = false;
    me.output_muted = false;
    me.input_hardware = true;

    UserState talking = preview_user("preview-a=", "Alice (speaking)");
    talking.talking = true;
    talking.input_muted = false;
    talking.output_muted = false;

    UserState commander = preview_user("preview-b=", "Bob (commander)");
    commander.channel_commander = true;
    commander.talk_power = 100;

    UserState mic_muted = preview_user("preview-c=", "Carol (mic muted)");
    mic_muted.input_muted = true;

    UserState speaker_muted = preview_user("preview-d=", "Dave (speakers muted)");
    speaker_muted.output_muted = true;

    UserState away = preview_user("preview-e=", "Erin (away)");
    away.away = true;
    away.away_message = "back soon";

    UserState whisper = preview_user("preview-f=", "Frank (whispering)");
    whisper.talking = true;
    whisper.whispering_to_me = true;

    UserState recording = preview_user("preview-g=", "Grace (recording)");
    recording.recording = true;

    UserState friend_user = preview_user("preview-friend=", "Friend User");

    UserState long_name =
        preview_user("preview-h=", "AnExtremelyLongTeamSpeakNicknameForTestingOverflow");

    s.users = {me,      talking, commander,   mic_muted, speaker_muted,
               away,    whisper, recording,   friend_user, long_name};
    return s;
}

std::vector<ChatMessage> preview_chat() {
    std::vector<ChatMessage> messages;
    const std::int64_t now = proto::now_unix_ms();
    ChatMessage a;
    a.category = ChatCategory::Channel;
    a.sender_name = "Alice";
    a.sender_unique_id = "preview-a=";
    a.channel_name = "Racing #1";
    a.text = "Preview channel message";
    a.timestamp_ms = now - 60000;
    ChatMessage b;
    b.category = ChatCategory::Server;
    b.sender_name = "Server";
    b.channel_name = "Racing #1";
    b.text = "Preview server notice";
    b.timestamp_ms = now - 30000;
    messages = {a, b};
    return messages;
}

void Renderer::seed_preview_notifications(const Config& config, std::int64_t now_ms) {
    notifications_.clear();
    // Zero disables the post-connect suppression window, which would otherwise swallow the
    // sample joins and leaves exactly as it does real ones just after connecting.
    notifications_.note_connected(0);

    const auto event = [&](OverlayEventKind kind, const char* name, const char* uid) {
        OverlayEvent e;
        e.kind = kind;
        e.display_name = name;
        e.unique_id = uid;
        e.channel_name = "Racing #1";
        e.previous_channel_name = "Lobby";
        e.user_count = 4;
        e.timestamp_ms = now_ms;
        return e;
    };

    notifications_.submit(event(OverlayEventKind::UserJoined, "Alice", "preview-a="), config,
                          now_ms);
    notifications_.submit(event(OverlayEventKind::UserLeft, "Bob", "preview-b="), config, now_ms);
    notifications_.submit(event(OverlayEventKind::ChannelChanged, "", ""), config, now_ms);

    OverlayEvent connected = event(OverlayEventKind::ConnectionChanged, "", "");
    connected.connection = ConnectionState::Connected;
    notifications_.submit(connected, config, now_ms);
    // note_connected fires again inside submit for a Connected event; undo it so the joins above
    // are not suppressed on the next re-seed.
    notifications_.note_connected(0);

    notifications_.submit(event(OverlayEventKind::WhisperStarted, "Frank", "preview-f="), config,
                          now_ms);

    OverlayEvent chat = event(OverlayEventKind::ChatMessage, "Alice", "preview-a=");
    chat.chat.category = ChatCategory::Channel;
    chat.chat.sender_name = "Alice";
    chat.chat.sender_unique_id = "preview-a=";
    chat.chat.text = "Example chat notification";
    chat.chat.timestamp_ms = now_ms;
    notifications_.submit(chat, config, now_ms);

    // Anything disabled in the configuration produces nothing, which is itself useful feedback:
    // an empty slot means that category is switched off, not that the preview is broken.
    notifications_.drain_sounds();  // never play sounds for a preview
}

void Renderer::submit_events(const std::vector<OverlayEvent>& events, const Config& config,
                             std::int64_t now_ms) {
    for (const OverlayEvent& event : events) {
        if (event.kind == OverlayEventKind::ConnectionChanged &&
            event.connection == ConnectionState::Connected) {
            notifications_.note_connected(now_ms);
        }
        notifications_.submit(event, config, now_ms);
        last_activity_ms_ = now_ms;
    }
    if (!events.empty()) layout_dirty_ = true;
}

void Renderer::draw_text(ImDrawList* dl, const Config& config, float x, float y, float size,
                         std::uint32_t color, std::string_view text) {
    if (text.empty()) return;
    ImFont* font = overlay_font();
    if (font == nullptr) return;
    const char* begin = text.data();
    const char* end = text.data() + text.size();

    // An outline is eight extra draws, so it is opt-in -- but unlike a one-sided shadow it stays
    // readable over *any* background rather than most of them.
    if (config.appearance.text_outline && config.appearance.text_outline_thickness > 0.0f) {
        const float t = config.appearance.text_outline_thickness;
        const std::uint32_t outline = config.appearance.text_outline_color.to_abgr();
        static constexpr float kOffsets[8][2] = {{-1, -1}, {0, -1}, {1, -1}, {-1, 0},
                                                 {1, 0},   {-1, 1}, {0, 1},  {1, 1}};
        for (const auto& o : kOffsets) {
            dl->AddText(font, size, ImVec2(x + o[0] * t, y + o[1] * t), outline, begin, end);
        }
    } else if (config.appearance.text_shadow) {
        // A one-pixel drop shadow is what keeps light text readable over bright game content,
        // which is the difference between usable and not on a snow map or a white car.
        const float offset = config.appearance.text_shadow_offset;
        dl->AddText(font, size, ImVec2(x + offset, y + offset),
                    config.appearance.text_shadow_color.to_abgr(), begin, end);
    }
    dl->AddText(font, size, ImVec2(x, y), color, begin, end);
}

void Renderer::draw_title(ImDrawList* dl, const Config& config, const LayoutResult& layout,
                          float opacity) {
    const ChannelTitleLayout& title = layout.title;
    if (!title.visible || title.text.empty()) return;

    const float alpha = opacity * title.opacity;
    const float pad_x = config.appearance.padding_x * config.general.scale;

    if (title.show_background || title.show_border) {
        draw_panel(dl, title.rect.x, title.rect.y, title.rect.w, title.rect.h,
                   config.appearance.corner_radius,
                   title.show_background ? packed(title.background, alpha) : 0u,
                   title.show_border ? packed(title.border, alpha) : 0u,
                   config.appearance.panel_border_thickness);
    }

    float x = title.rect.x + pad_x;
    if (title.align == Align::Right) {
        x = title.rect.right() - pad_x - measure_text(title.text, title.font_size) -
            (title.icon != IconShape::None
                 ? config.appearance.icon_size * config.general.scale +
                       config.user_list.indicator_gap * config.general.scale
                 : 0.0f);
    }
    x = std::max(x, title.rect.x + pad_x);
    const float centre_y = title.rect.y + title.rect.h * 0.5f;
    if (title.icon != IconShape::None) {
        const float icon = config.appearance.icon_size * config.general.scale;
        draw_icon(dl, title.icon, x + icon * 0.5f, centre_y, icon,
                  packed(title.icon_color, alpha));
        x += icon + config.user_list.indicator_gap * config.general.scale;
    }
    draw_text(dl, config, x, centre_y - title.font_size * 0.5f, title.font_size,
              packed(title.text_color, alpha), title.text);
}

void Renderer::draw_users(ImDrawList* dl, const Config& config, const LayoutResult& layout,
                          float opacity, std::int64_t now_ms) {
    const UserListLayout& users = layout.users;
    if (!users.visible) return;

    const float scale = config.general.scale;
    const float icon = config.appearance.icon_size * scale;
    const float gap = config.user_list.indicator_gap * scale;
    const float pad_x = config.appearance.padding_x * scale;

    if (config.appearance.show_panel_background) {
        draw_panel(dl, users.rect.x, users.rect.y, users.rect.w, users.rect.h,
                   config.appearance.corner_radius,
                   packed(config.appearance.panel_background, opacity),
                   packed(config.appearance.panel_border, opacity),
                   config.appearance.panel_border_thickness);
    }

    const float pulse_phase =
        static_cast<float>(now_ms % 100000) * 0.001f * config.animation.speaking_pulse_hz *
        6.28318530718f;

    for (const UserRowLayout& row : users.rows) {
        const ResolvedUser& resolved = row.resolved;
        float row_alpha = opacity * resolved.entry_opacity;

        // The pulse modulates only the speaking treatment, never the name's legibility.
        float emphasis = 1.0f;
        if (config.animation.enabled && resolved.speaking &&
            config.animation.speaking == SpeakingAnimation::Pulse) {
            emphasis = 1.0f - config.animation.speaking_pulse_depth * 0.5f *
                                  (1.0f - std::cos(pulse_phase));
        }

        if (resolved.glow) {
            const float radius = resolved.glow_radius * scale * emphasis;
            draw_glow(dl, row.rect.x + row.rect.w * 0.5f, row.rect.y + row.rect.h * 0.5f,
                      radius + row.rect.h * 0.5f, packed(resolved.glow_color, row_alpha));
        }
        if (resolved.show_background) {
            draw_panel(dl, row.rect.x, row.rect.y, row.rect.w, row.rect.h,
                       config.appearance.corner_radius * 0.6f,
                       packed(resolved.background, row_alpha), 0u, 0.0f);
        }
        if (resolved.show_border) {
            draw_panel(dl, row.rect.x, row.rect.y, row.rect.w, row.rect.h,
                       config.appearance.corner_radius * 0.6f, 0u,
                       packed(resolved.border, row_alpha * emphasis),
                       resolved.border_thickness * scale);
        }

        const float centre_y = row.rect.y + row.rect.h * 0.5f;

        // Right-aligned lists put the whole row flush against the right edge: dot, then name,
        // ending where the panel does. Measuring the row first is what lets the leading icons
        // stay attached to the name instead of floating at a fixed left margin.
        float x = row.rect.x;
        if (layout.users.align == Align::Right) {
            float content = row.name.width;
            for (const ResolvedUser::Indicator& indicator : resolved.leading) {
                content += icon * indicator.scale + gap;
            }
            for (const ResolvedUser::Indicator& indicator : resolved.trailing) {
                content += icon * indicator.scale + gap;
            }
            x = row.rect.right() - content;
        } else if (layout.users.align == Align::Center) {
            float content = row.name.width;
            for (const ResolvedUser::Indicator& indicator : resolved.leading) {
                content += icon * indicator.scale + gap;
            }
            x = row.rect.x + (row.rect.w - content) * 0.5f;
        }
        x = std::max(x, row.rect.x);

        // Leading indicators: Channel Commander sits immediately before the name, as specified.
        for (const ResolvedUser::Indicator& indicator : resolved.leading) {
            draw_icon(dl, indicator.shape, x + icon * 0.5f, centre_y, icon * indicator.scale,
                      packed(indicator.color, row_alpha), 1.5f * scale);
            x += icon * indicator.scale + gap;
        }

        const float font = row.font_size * row.name.font_scale;
        const float text_y = centre_y - font * 0.5f;
        if (row.name.lines.size() > 1) {
            // Wrapped names render inside the row height; the layout already capped the count.
            float y = row.rect.y + (row.rect.h - font * static_cast<float>(row.name.lines.size())) * 0.5f;
            for (const std::string& line : row.name.lines) {
                draw_text(dl, config, x, y, font, packed(resolved.name_color, row_alpha), line);
                y += font;
            }
        } else if (!resolved.friend_tag.empty() &&
                   row.name.text.rfind(resolved.friend_tag, 0) == 0) {
            // "[tag] " in its own colour, then the name. Only when the tag survived fitting --
            // a truncated name may have eaten it, in which case draw the line as one piece.
            const float tag_x = x - row.name.scroll_offset;
            draw_text(dl, config, tag_x, text_y, font,
                      packed(config.user_list.friend_tag_color, row_alpha), resolved.friend_tag);
            const float tag_w = measure_text(resolved.friend_tag, font);
            draw_text(dl, config, tag_x + tag_w, text_y, font,
                      packed(resolved.name_color, row_alpha),
                      std::string_view(row.name.text).substr(resolved.friend_tag.size()));
        } else {
            draw_text(dl, config, x - row.name.scroll_offset, text_y, font,
                      packed(resolved.name_color, row_alpha), row.name.text);
        }
        x += row.name.width + gap;

        // Trailing indicators are right-aligned so rows stay visually even.
        float right = row.rect.right();
        for (auto it = resolved.trailing.rbegin(); it != resolved.trailing.rend(); ++it) {
            right -= icon * it->scale;
            draw_icon(dl, it->shape, right + icon * 0.5f, centre_y, icon * it->scale,
                      packed(it->color, row_alpha * emphasis), 1.5f * scale);
            right -= gap;
        }
    }

    if (!users.overflow_text.empty()) {
        const float font = config.appearance.font_size * scale;
        const float y = users.rect.bottom() - config.appearance.padding_y * scale - font;
        draw_text(dl, config, users.rect.x + pad_x, y, font,
                  packed(config.appearance.text_secondary, opacity), users.overflow_text);
    }
}

void Renderer::draw_notifications(ImDrawList* dl, const Config& config, const Viewport& viewport,
                                  float opacity, std::int64_t now_ms) {
    const NotificationsConfig& nc = config.notifications;
    if (!nc.placement.visible || notifications_.items().empty()) return;

    const float scale = config.general.scale;
    const float width = nc.width * scale;
    const float font = config.appearance.font_size * scale;
    // Notifications carry their own padding. appearance.padding_* belongs to the user list,
    // which legitimately runs at zero, and borrowing it put toast text hard against its border.
    const float pad_x = nc.padding_x * scale;
    const float pad_y = nc.padding_y * scale;
    const float icon = config.appearance.icon_size * scale;
    const float gap = std::max(4.0f, config.user_list.indicator_gap * scale);
    const float height = std::max(nc.min_height * scale, font + pad_y * 2.0f);
    const float spacing = nc.spacing * scale;

    const std::size_t count = notifications_.items().size();
    const float stack_height =
        static_cast<float>(count) * height + static_cast<float>(count - 1) * spacing;
    const Rect area = resolve_placement(nc.placement, width, stack_height, viewport);

    const MeasureFn measure = [](std::string_view t, float size) { return measure_text(t, size); };

    std::size_t index = 0;
    for (const Notification& notification : notifications_.items()) {
        const float fade = notification.opacity(now_ms);
        if (fade <= 0.003f) {
            ++index;
            continue;
        }
        const float alpha = opacity * fade;
        // Stacking direction decides whether new notifications push downwards or upwards.
        const float offset = static_cast<float>(
            nc.stack == StackDirection::Down ? index : (count - 1 - index));
        const float y = area.y + offset * (height + spacing);

        draw_panel(dl, area.x, y, width, height, config.appearance.corner_radius,
                   notification.show_background ? packed(notification.background, alpha) : 0u,
                   notification.show_border ? packed(notification.border, alpha) : 0u,
                   config.appearance.panel_border_thickness);

        const float centre_y = y + height * 0.5f;
        const float text_top = centre_y - font * 0.5f;

        // Measure the whole line first, then place it. Deriving the message's budget from the
        // panel rather than from wherever the cursor happened to land is what makes this safe:
        // a bad prefix measurement can now only shift the text, never starve it of room. The
        // previous version skipped the body outright when the budget came out at zero, which is
        // why notification boxes drew completely empty.
        const float avail = std::max(16.0f, width - pad_x * 2.0f);
        const float icon_w = notification.icon != IconShape::None ? icon + gap : 0.0f;
        const float prefix_w =
            notification.prefix.empty() ? 0.0f : measure_text(notification.prefix, font) + gap;
        const float body_budget = std::max(16.0f, avail - icon_w - prefix_w);
        const float body_w = std::min(measure_text(notification.text, font), body_budget);
        const float content = std::min(avail, icon_w + prefix_w + body_w);

        float x = area.x + pad_x;
        if (nc.placement.align == Align::Right) {
            x = area.x + width - pad_x - content;
        } else if (nc.placement.align == Align::Center) {
            x = area.x + (width - content) * 0.5f;
        }
        x = std::max(x, area.x + pad_x);

        if (notification.icon != IconShape::None) {
            draw_icon(dl, notification.icon, x + icon * 0.5f, centre_y, icon,
                      packed(notification.icon_color, alpha), 1.5f * scale);
            x += icon_w;
        }
        if (!notification.prefix.empty()) {
            draw_text(dl, config, x, text_top, font, packed(notification.icon_color, alpha),
                      notification.prefix);
            x += prefix_w;
        }

        // The message is fitted to the budget reserved for it above, so it can never run past
        // the panel and can never be dropped.
        float remaining = body_budget;

        // The sender's name is drawn in its own colour, the rest of the line in the body colour.
        const std::size_t name_pos =
            notification.name.empty() ? std::string::npos
                                      : notification.text.find(notification.name);

        const auto draw_segment = [&](std::string_view segment, std::uint32_t colour) {
            if (segment.empty() || remaining <= 4.0f) return;
            const FittedText fitted =
                fit_text(segment, remaining, font, OverflowMode::Ellipsis, 0.75f, measure);
            draw_text(dl, config, x, text_top, font, colour, fitted.text);
            const float used = measure_text(fitted.text, font);
            x += used;
            remaining -= used;
        };

        if (name_pos == std::string::npos) {
            draw_segment(notification.text, packed(notification.text_color, alpha));
        } else {
            const std::string_view whole(notification.text);
            draw_segment(whole.substr(0, name_pos), packed(notification.text_color, alpha));
            draw_segment(whole.substr(name_pos, notification.name.size()),
                         packed(notification.name_color, alpha));
            draw_segment(whole.substr(name_pos + notification.name.size()),
                         packed(notification.text_color, alpha));
        }

        if (notification.repeat_count > 1) {
            text_scratch_ = "x" + std::to_string(notification.repeat_count);
            const float w = measure_text(text_scratch_, font);
            draw_text(dl, config, area.x + width - pad_x - w, text_top, font,
                      packed(config.appearance.text_secondary, alpha), text_scratch_);
        }
        ++index;
    }
}

void Renderer::draw_chat(ImDrawList* dl, const Config& config, const Viewport& viewport,
                         const std::vector<ChatMessage>& messages, float opacity,
                         std::int64_t now_ms) {
    const ChatConfig& cc = config.chat;
    if (!cc.placement.visible || messages.empty()) return;

    const float scale = config.general.scale;
    const float font = config.appearance.font_size * scale * cc.font_scale;
    const float width = cc.width * scale;
    // Chat has its own padding for the same reason notifications do: the user list may be at
    // zero, and a panel with text against its border is unreadable.
    const float pad_x = std::max(6.0f, config.notifications.padding_x * scale);
    const float pad_y = std::max(4.0f, config.notifications.padding_y * scale);
    const float line_height = font * 1.3f;
    const float text_width = std::max(40.0f, width - pad_x * 2.0f);

    const MeasureFn measure = [](std::string_view t, float size) { return measure_text(t, size); };

    // Select the visible window. Filtering here as well as in the plugin means turning a
    // category off hides messages already received, not just future ones.
    std::vector<const ChatMessage*> visible;
    visible.reserve(messages.size());
    for (auto it = messages.rbegin(); it != messages.rend(); ++it) {
        const ChatMessage& message = *it;
        if (message.category == ChatCategory::Channel && !cc.show_channel_messages) continue;
        if (message.category == ChatCategory::Server && !cc.show_server_messages) continue;
        if (message.category == ChatCategory::Private && !cc.show_private_messages) continue;
        if (cc.retention_seconds > 0 &&
            now_ms - message.timestamp_ms >
                static_cast<std::int64_t>(cc.retention_seconds) * 1000) {
            continue;
        }
        visible.push_back(&message);
        if (visible.size() >= static_cast<std::size_t>(cc.max_visible_messages)) break;
    }
    if (visible.empty()) return;
    if (cc.order == ChatOrder::NewestBottom) std::reverse(visible.begin(), visible.end());

    // Flatten every message into a list of *visual* lines up front. Laying out first and drawing
    // second is what guarantees the panel is exactly as tall as its contents and that no two
    // lines can ever land on the same y -- they previously overlapped because the height and the
    // drawing advanced independently.
    struct VisualLine {
        std::string prefix;   ///< timestamp / icon / sender, only on a message's first line
        Color prefix_color{};
        std::string body;
    };
    std::vector<VisualLine> lines;
    lines.reserve(visible.size() * 2);

    for (const ChatMessage* message : visible) {
        std::string prefix;
        if (cc.show_timestamp) prefix += format_clock(message->timestamp_ms) + " ";
        if (cc.show_category_icon) prefix += std::string(category_label(message->category)) + " ";
        if (cc.show_channel_name && !message->channel_name.empty()) {
            prefix += "[" + message->channel_name + "] ";
        }
        if (cc.show_sender) prefix += message->sender_name + ": ";

        Color prefix_color = cc.sender;
        if (cc.use_sender_color) {
            if (const UserOverride* ov = config.find_user_override(message->sender_unique_id)) {
                if (ov->name_color) prefix_color = *ov->name_color;
            }
        }

        const std::string text = json::truncate_utf8(
            message->text, static_cast<std::size_t>(cc.max_message_length));
        const float prefix_width = measure_text(prefix, font);
        const float body_budget = std::max(30.0f, text_width - prefix_width);

        if (cc.wrap) {
            const FittedText fitted =
                fit_text(text, body_budget, font, OverflowMode::Wrap, 0.75f, measure);
            bool first = true;
            for (const std::string& body : fitted.lines) {
                lines.push_back({first ? prefix : std::string(),
                                 first ? prefix_color : cc.text, body});
                first = false;
            }
            if (fitted.lines.empty()) lines.push_back({prefix, prefix_color, std::string()});
        } else {
            const FittedText fitted =
                fit_text(text, body_budget, font, OverflowMode::Ellipsis, 0.75f, measure);
            lines.push_back({prefix, prefix_color, fitted.text});
        }
    }

    const float height = static_cast<float>(lines.size()) * line_height + pad_y * 2.0f;
    const Rect area = resolve_placement(cc.placement, width, height, viewport);

    if (cc.show_background) {
        draw_panel(dl, area.x, area.y, area.w, area.h, config.appearance.corner_radius,
                   packed(cc.background, opacity), 0u, 0.0f);
    }

    // Alignment applies to chat as well: right-aligned means every line ends flush with the
    // panel's right edge and grows leftward.
    const Align align = cc.placement.align;
    float y = area.y + pad_y;
    for (const VisualLine& line : lines) {
        const float prefix_w = measure_text(line.prefix, font);
        const float body_w = measure_text(line.body, font);
        const float total = prefix_w + body_w;

        float x = area.x + pad_x;
        if (align == Align::Right) x = area.x + area.w - pad_x - total;
        else if (align == Align::Center) x = area.x + (area.w - total) * 0.5f;
        // Never start left of the panel: a line wider than the panel reads better clipped on the
        // right than drawn outside the background it is supposed to sit on.
        x = std::max(x, area.x + pad_x);

        if (!line.prefix.empty()) {
            draw_text(dl, config, x, y, font, packed(line.prefix_color, opacity), line.prefix);
            x += prefix_w;
        }
        // Message text is drawn as literal text: no markup, escape or URL in it is ever
        // interpreted, so a chat message cannot influence anything but its own glyphs.
        if (!line.body.empty()) {
            draw_text(dl, config, x, y, font, packed(cc.text, opacity), line.body);
        }
        y += line_height;
    }
}

void Renderer::draw(ImDrawList* dl, const Config& config, const OverlayFrame& frame,
                    const Viewport& viewport, std::int64_t now_ms, const OverlayState* preview,
                    const std::vector<ChatMessage>* preview_chat_messages) {
    if (dl == nullptr || !config.general.enabled) return;

    const auto start = std::chrono::steady_clock::now();
    ++stats_.frames;

    const OverlayState& state = preview != nullptr ? *preview : frame.state;
    const std::vector<ChatMessage>& chat =
        preview_chat_messages != nullptr ? *preview_chat_messages : frame.chat;

    const bool live = state.server.connection == ConnectionState::Connected;
    const bool plugin_available = preview != nullptr || frame.link == LinkState::Connected;
    if (!plugin_available && !config.general.show_when_plugin_unavailable) return;
    if (!live && !config.general.show_when_disconnected) return;

    const std::int64_t dt = last_frame_ms_ == 0 ? 16 : std::min<std::int64_t>(now_ms - last_frame_ms_, 250);
    last_frame_ms_ = now_ms;

    // Speaking envelopes advance every frame regardless of whether the layout is recomputed, so
    // the ramp stays smooth even when nothing else changed.
    envelope_.set_config(config.animation.speaking_attack_ms, config.animation.speaking_release_ms);
    talking_scratch_.clear();
    talking_scratch_.reserve(state.users.size());
    for (const UserState& user : state.users) {
        talking_scratch_.emplace_back(user.unique_id, user.talking);
    }
    envelope_.update(talking_scratch_, dt);

    notifications_.tick(now_ms, config);

    const std::uint64_t config_hash = layout_hash(config);
    const bool viewport_changed = std::fabs(layout_.viewport.width - viewport.width) > 0.5f ||
                                  std::fabs(layout_.viewport.height - viewport.height) > 0.5f;
    const bool needs_layout = layout_dirty_ || viewport_changed ||
                              config_hash != last_config_hash_ ||
                              frame.revision != last_revision_ || preview != nullptr ||
                              state.talking_count() > 0;

    if (needs_layout) {
        const MeasureFn measure = [](std::string_view text, float size) {
            return measure_text(text, size);
        };
        const float time_s = static_cast<float>(now_ms % 1000000) * 0.001f;
        layout_ = compute_layout(state, config, viewport, measure, time_s,
                                 [this](const std::string& uid) {
                                     return envelope_.intensity(uid);
                                 });
        layout_dirty_ = false;
        last_config_hash_ = config_hash;
        last_revision_ = frame.revision;
        ++stats_.layouts;
    }

    const auto laid_out = std::chrono::steady_clock::now();
    stats_.last_layout_ms =
        std::chrono::duration<float, std::milli>(laid_out - start).count();

    float opacity = config.general.master_opacity;
    if (config.animation.enabled && config.animation.fade_when_idle &&
        last_activity_ms_ > 0 && state.talking_count() == 0) {
        const std::int64_t idle = now_ms - last_activity_ms_;
        if (idle > config.animation.idle_after_ms) {
            opacity *= config.animation.idle_opacity;
        }
    } else if (state.talking_count() > 0) {
        last_activity_ms_ = now_ms;
    }

    // A degraded link must be visible as such: the brief forbids presenting a stale user list as
    // if it were live. compute_layout already hid the roster; this dims what remains.
    if (layout_.degraded) opacity *= 0.8f;

    draw_title(dl, config, layout_, opacity);
    draw_users(dl, config, layout_, opacity, now_ms);
    draw_notifications(dl, config, viewport, opacity, now_ms);
    draw_chat(dl, config, viewport, chat, opacity, now_ms);

    stats_.last_draw_ms =
        std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - laid_out)
            .count();
    stats_.draw_commands = static_cast<std::size_t>(dl->CmdBuffer.Size);
}

}  // namespace tsro::overlay
