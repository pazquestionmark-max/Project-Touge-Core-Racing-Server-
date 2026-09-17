// SPDX-License-Identifier: MIT
// Enum <-> wire-name tables and the colour/easing primitives.
#include <cmath>
#include <cstdio>

#include "tsro/config.hpp"

namespace tsro {
namespace {

template <typename E, std::size_t N>
const char* lookup(const std::array<std::pair<E, const char*>, N>& table, E v) noexcept {
    for (const auto& e : table) {
        if (e.first == v) return e.second;
    }
    return table[0].second;
}

template <typename E, std::size_t N>
bool lookup_parse(const std::array<std::pair<E, const char*>, N>& table, std::string_view s,
                  E& out) noexcept {
    for (const auto& e : table) {
        if (s == e.second) {
            out = e.first;
            return true;
        }
    }
    return false;
}

constexpr std::array<std::pair<Anchor, const char*>, 9> kAnchors{{
    {Anchor::TopLeft, "top_left"},
    {Anchor::TopCenter, "top_center"},
    {Anchor::TopRight, "top_right"},
    {Anchor::CenterLeft, "center_left"},
    {Anchor::Center, "center"},
    {Anchor::CenterRight, "center_right"},
    {Anchor::BottomLeft, "bottom_left"},
    {Anchor::BottomCenter, "bottom_center"},
    {Anchor::BottomRight, "bottom_right"},
}};

constexpr std::array<std::pair<Align, const char*>, 3> kAligns{{
    {Align::Left, "left"}, {Align::Center, "center"}, {Align::Right, "right"},
}};

constexpr std::array<std::pair<IconShape, const char*>, 18> kIcons{{
    {IconShape::None, "none"},
    {IconShape::Dot, "dot"},
    {IconShape::Circle, "circle"},
    {IconShape::Ring, "ring"},
    {IconShape::Square, "square"},
    {IconShape::Diamond, "diamond"},
    {IconShape::Triangle, "triangle"},
    {IconShape::Star, "star"},
    {IconShape::Chevron, "chevron"},
    {IconShape::Microphone, "microphone"},
    {IconShape::MicrophoneMuted, "microphone_muted"},
    {IconShape::Speaker, "speaker"},
    {IconShape::SpeakerMuted, "speaker_muted"},
    {IconShape::Moon, "moon"},
    {IconShape::Record, "record"},
    {IconShape::Crown, "crown"},
    {IconShape::Whisper, "whisper"},
    {IconShape::Bars, "bars"},
}};

constexpr std::array<std::pair<Easing, const char*>, 6> kEasings{{
    {Easing::Linear, "linear"},
    {Easing::EaseIn, "ease_in"},
    {Easing::EaseOut, "ease_out"},
    {Easing::EaseInOut, "ease_in_out"},
    {Easing::EaseOutBack, "ease_out_back"},
    {Easing::EaseOutElastic, "ease_out_elastic"},
}};

constexpr std::array<std::pair<OverflowMode, const char*>, 5> kOverflow{{
    {OverflowMode::Clip, "clip"},
    {OverflowMode::Ellipsis, "ellipsis"},
    {OverflowMode::Wrap, "wrap"},
    {OverflowMode::Shrink, "shrink"},
    {OverflowMode::Scroll, "scroll"},
}};

constexpr std::array<std::pair<UserSort, const char*>, 4> kSorts{{
    {UserSort::ChannelOrder, "channel_order"},
    {UserSort::Alphabetical, "alphabetical"},
    {UserSort::TalkPower, "talk_power"},
    {UserSort::SpeakingFirst, "speaking_first"},
}};

constexpr std::array<std::pair<SpeakingAnimation, const char*>, 5> kSpeakAnims{{
    {SpeakingAnimation::None, "none"},
    {SpeakingAnimation::ColorFade, "color_fade"},
    {SpeakingAnimation::Pulse, "pulse"},
    {SpeakingAnimation::Glow, "glow"},
    {SpeakingAnimation::BorderSweep, "border_sweep"},
}};

constexpr std::array<std::pair<ChatOrder, const char*>, 2> kChatOrders{{
    {ChatOrder::NewestBottom, "newest_bottom"}, {ChatOrder::NewestTop, "newest_top"},
}};

constexpr std::array<std::pair<StackDirection, const char*>, 2> kStacks{{
    {StackDirection::Down, "down"}, {StackDirection::Up, "up"},
}};

constexpr std::array<std::pair<NotificationBorder, const char*>, 3> kNotifBorders{{
    {NotificationBorder::None, "none"},
    {NotificationBorder::Accent, "accent"},
    {NotificationBorder::Custom, "custom"},
}};

int hex_digit(char c) noexcept {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

}  // namespace

const char* to_string(Anchor v) noexcept { return lookup(kAnchors, v); }
const char* to_string(Align v) noexcept { return lookup(kAligns, v); }
const char* to_string(IconShape v) noexcept { return lookup(kIcons, v); }
const char* to_string(Easing v) noexcept { return lookup(kEasings, v); }
const char* to_string(OverflowMode v) noexcept { return lookup(kOverflow, v); }
const char* to_string(UserSort v) noexcept { return lookup(kSorts, v); }
const char* to_string(SpeakingAnimation v) noexcept { return lookup(kSpeakAnims, v); }
const char* to_string(ChatOrder v) noexcept { return lookup(kChatOrders, v); }
const char* to_string(StackDirection v) noexcept { return lookup(kStacks, v); }
const char* to_string(NotificationBorder v) noexcept { return lookup(kNotifBorders, v); }

bool parse_enum(std::string_view s, Anchor& o) noexcept { return lookup_parse(kAnchors, s, o); }
bool parse_enum(std::string_view s, Align& o) noexcept { return lookup_parse(kAligns, s, o); }
bool parse_enum(std::string_view s, IconShape& o) noexcept { return lookup_parse(kIcons, s, o); }
bool parse_enum(std::string_view s, Easing& o) noexcept { return lookup_parse(kEasings, s, o); }
bool parse_enum(std::string_view s, OverflowMode& o) noexcept {
    return lookup_parse(kOverflow, s, o);
}
bool parse_enum(std::string_view s, UserSort& o) noexcept { return lookup_parse(kSorts, s, o); }
bool parse_enum(std::string_view s, SpeakingAnimation& o) noexcept {
    return lookup_parse(kSpeakAnims, s, o);
}
bool parse_enum(std::string_view s, ChatOrder& o) noexcept {
    return lookup_parse(kChatOrders, s, o);
}
bool parse_enum(std::string_view s, StackDirection& o) noexcept {
    return lookup_parse(kStacks, s, o);
}
bool parse_enum(std::string_view s, NotificationBorder& o) noexcept {
    return lookup_parse(kNotifBorders, s, o);
}

std::optional<Color> Color::from_hex(std::string_view s) {
    if (!s.empty() && s.front() == '#') s.remove_prefix(1);
    if (s.size() != 3 && s.size() != 4 && s.size() != 6 && s.size() != 8) return std::nullopt;
    int d[8];
    for (std::size_t i = 0; i < s.size(); ++i) {
        d[i] = hex_digit(s[i]);
        if (d[i] < 0) return std::nullopt;
    }
    Color c;
    if (s.size() <= 4) {
        // Short form: each digit is doubled, so "#f80" is "#ff8800".
        c.r = static_cast<std::uint8_t>(d[0] * 17);
        c.g = static_cast<std::uint8_t>(d[1] * 17);
        c.b = static_cast<std::uint8_t>(d[2] * 17);
        c.a = s.size() == 4 ? static_cast<std::uint8_t>(d[3] * 17) : 255;
    } else {
        c.r = static_cast<std::uint8_t>(d[0] * 16 + d[1]);
        c.g = static_cast<std::uint8_t>(d[2] * 16 + d[3]);
        c.b = static_cast<std::uint8_t>(d[4] * 16 + d[5]);
        c.a = s.size() == 8 ? static_cast<std::uint8_t>(d[6] * 16 + d[7]) : 255;
    }
    return c;
}

std::string Color::to_hex() const {
    char buf[10];
    std::snprintf(buf, sizeof(buf), "#%02X%02X%02X%02X", r, g, b, a);
    return std::string(buf);
}

Color Color::with_alpha_scale(float s) const noexcept {
    if (s < 0.0f) s = 0.0f;
    if (s > 1.0f) s = 1.0f;
    Color c = *this;
    c.a = static_cast<std::uint8_t>(static_cast<float>(a) * s + 0.5f);
    return c;
}

float ease(Easing e, float t) noexcept {
    if (!(t > 0.0f)) return 0.0f;  // also catches NaN
    if (t >= 1.0f) return 1.0f;
    switch (e) {
        case Easing::Linear: return t;
        case Easing::EaseIn: return t * t;
        case Easing::EaseOut: return t * (2.0f - t);
        case Easing::EaseInOut:
            return t < 0.5f ? 2.0f * t * t : -1.0f + (4.0f - 2.0f * t) * t;
        case Easing::EaseOutBack: {
            constexpr float c1 = 1.70158f;
            constexpr float c3 = c1 + 1.0f;
            const float u = t - 1.0f;
            return 1.0f + c3 * u * u * u + c1 * u * u;
        }
        case Easing::EaseOutElastic: {
            constexpr float c4 = 6.283185307f / 3.0f;
            return std::pow(2.0f, -10.0f * t) * std::sin((t * 10.0f - 0.75f) * c4) + 1.0f;
        }
    }
    return t;
}

std::string make_channel_key(std::string_view server_unique_id, std::uint64_t channel_id) {
    std::string key(server_unique_id);
    key.push_back(':');
    char buf[24];
    const int n = std::snprintf(buf, sizeof(buf), "%llu", static_cast<unsigned long long>(channel_id));
    key.append(buf, static_cast<std::size_t>(n > 0 ? n : 0));
    return key;
}

}  // namespace tsro
