// SPDX-License-Identifier: MIT
// The shipped look. Kept apart from the (de)serialiser so the default theme can be reviewed and
// changed as a single readable unit.
#include "tsro/config.hpp"

namespace tsro {
namespace {

constexpr Color kSpeakingGreen{126, 231, 135, 255};
constexpr Color kCommanderOrange{255, 149, 43, 255};   // the brief's default commander colour
constexpr Color kMutedRed{240, 104, 104, 255};
constexpr Color kSpeakerMutedPurple{193, 122, 255, 255};
constexpr Color kAwayAmber{224, 184, 92, 255};
constexpr Color kRecordingRed{255, 85, 85, 255};
constexpr Color kPriorityBlue{88, 166, 255, 255};
constexpr Color kWhisperCyan{86, 214, 214, 255};
constexpr Color kSuppressedGrey{120, 126, 136, 255};

StateStyle speaking_style() {
    StateStyle s;
    s.icon = IconShape::Bars;
    s.icon_color = kSpeakingGreen;
    s.override_text_color = true;
    s.text_color = kSpeakingGreen;
    s.show_border = true;
    s.border = Color{126, 231, 135, 140};
    s.border_thickness = 1.5f;
    s.glow = true;
    s.glow_color = Color{126, 231, 135, 70};
    s.glow_radius = 7.0f;
    return s;
}

StateStyle whisper_style() {
    StateStyle s;
    s.icon = IconShape::Whisper;
    s.icon_color = kWhisperCyan;
    s.override_text_color = true;
    s.text_color = kWhisperCyan;
    s.show_border = true;
    s.border = Color{86, 214, 214, 150};
    s.glow = true;
    s.glow_color = Color{86, 214, 214, 70};
    return s;
}

StateStyle mic_muted_style() {
    StateStyle s;
    s.icon = IconShape::MicrophoneMuted;
    s.icon_color = kMutedRed;
    s.override_text_color = true;
    s.text_color = Color{164, 168, 176, 255};
    s.dim_entry = true;
    s.dim_amount = 0.55f;
    return s;
}

StateStyle speaker_muted_style() {
    StateStyle s;
    // Deliberately a different shape *and* colour from mic mute: the two states must never be
    // mistaken for one another at a glance.
    s.icon = IconShape::SpeakerMuted;
    s.icon_color = kSpeakerMutedPurple;
    s.override_text_color = true;
    s.text_color = Color{150, 140, 168, 255};
    s.dim_entry = true;
    s.dim_amount = 0.5f;
    return s;
}

StateStyle hardware_off_style() {
    StateStyle s;
    s.enabled = false;  // niche; off by default to keep the default HUD uncluttered
    s.icon = IconShape::Microphone;
    s.icon_color = kSuppressedGrey;
    s.opacity = 0.7f;
    return s;
}

StateStyle away_style() {
    StateStyle s;
    s.icon = IconShape::Moon;
    s.icon_color = kAwayAmber;
    s.override_text_color = true;
    s.text_color = Color{176, 166, 140, 255};
    s.dim_entry = true;
    s.dim_amount = 0.45f;
    return s;
}

StateStyle recording_style() {
    StateStyle s;
    s.icon = IconShape::Record;
    s.icon_color = kRecordingRed;
    s.override_text_color = true;
    s.text_color = kRecordingRed;
    return s;
}

StateStyle commander_style() {
    StateStyle s;
    // The brief's default: an orange circular indicator immediately before the name.
    s.icon = IconShape::Circle;
    s.icon_color = kCommanderOrange;
    s.icon_scale = 1.0f;
    s.override_text_color = true;
    s.text_color = Color{255, 197, 132, 255};
    return s;
}

StateStyle priority_style() {
    StateStyle s;
    s.icon = IconShape::Chevron;
    s.icon_color = kPriorityBlue;
    s.override_text_color = false;
    return s;
}

StateStyle suppressed_style() {
    StateStyle s;
    s.enabled = false;  // only meaningful in moderated channels
    s.icon = IconShape::Diamond;
    s.icon_color = kSuppressedGrey;
    s.override_text_color = true;
    s.text_color = kSuppressedGrey;
    s.dim_entry = true;
    s.dim_amount = 0.6f;
    return s;
}

StateStyle locally_muted_style() {
    StateStyle s;
    s.icon = IconShape::Square;
    s.icon_color = kSuppressedGrey;
    s.override_text_color = true;
    s.text_color = kSuppressedGrey;
    s.dim_entry = true;
    s.dim_amount = 0.5f;
    return s;
}

NotificationStyle join_notification() {
    NotificationStyle n;
    n.format = "{name} joined {channel}";
    n.prefix = "[+]";
    n.icon = IconShape::Chevron;
    n.icon_color = kSpeakingGreen;
    n.name_color = kSpeakingGreen;
    n.border = Color{126, 231, 135, 120};
    return n;
}

NotificationStyle leave_notification() {
    NotificationStyle n;
    n.format = "{name} left {channel}";
    n.prefix = "[-]";
    n.icon = IconShape::Chevron;
    n.icon_color = kMutedRed;
    n.name_color = kMutedRed;
    n.border = Color{240, 104, 104, 120};
    return n;
}

NotificationStyle switch_notification() {
    NotificationStyle n;
    n.format = "{previous} -> {channel} ({count})";
    n.prefix = "[>]";
    n.icon = IconShape::Chevron;
    n.icon_color = kPriorityBlue;
    n.name_color = kPriorityBlue;
    n.border = Color{88, 166, 255, 120};
    n.fade.hold_ms = 3000;
    return n;
}

NotificationStyle connection_notification() {
    NotificationStyle n;
    n.format = "TeamSpeak: {status}";
    n.prefix = "[*]";
    n.icon = IconShape::Circle;
    n.icon_color = kAwayAmber;
    n.name_color = kAwayAmber;
    n.border = Color{224, 184, 92, 120};
    n.fade.hold_ms = 4000;
    return n;
}

NotificationStyle whisper_notification() {
    NotificationStyle n;
    n.enabled = false;  // opt-in: whisper bursts are frequent enough to be intrusive
    n.format = "{name} is whispering";
    n.prefix = "[w]";
    n.icon = IconShape::Whisper;
    n.icon_color = kWhisperCyan;
    n.name_color = kWhisperCyan;
    n.border = Color{86, 214, 214, 120};
    n.fade.hold_ms = 2500;
    return n;
}

NotificationStyle chat_notification() {
    NotificationStyle n;
    n.enabled = false;  // the chat feed is the normal presentation; toasts are opt-in
    n.format = "{name}: {message}";
    n.prefix = "[#]";
    n.icon = IconShape::Square;
    n.icon_color = kPriorityBlue;
    n.name_color = kPriorityBlue;
    n.border = Color{88, 166, 255, 100};
    n.fade.hold_ms = 5000;
    return n;
}

}  // namespace

Config Config::defaults() {
    Config c;
    c.config_version = kConfigVersion;

    c.indicators.speaking = speaking_style();
    c.indicators.whispering = whisper_style();
    c.indicators.mic_muted = mic_muted_style();
    c.indicators.speaker_muted = speaker_muted_style();
    c.indicators.mic_hardware_off = hardware_off_style();
    c.indicators.away = away_style();
    c.indicators.recording = recording_style();
    c.indicators.commander = commander_style();
    c.indicators.priority_speaker = priority_style();
    c.indicators.suppressed = suppressed_style();
    c.indicators.locally_muted = locally_muted_style();

    c.notifications.join = join_notification();
    c.notifications.leave = leave_notification();
    c.notifications.channel_switch = switch_notification();
    c.notifications.connection = connection_notification();
    c.notifications.whisper = whisper_notification();
    c.notifications.chat = chat_notification();

    return c;
}

}  // namespace tsro
