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
    // Speaking is shown by colouring the name, nothing more. A box that appears and disappears
    // around a row is the single most distracting thing an overlay can do in a game, and it also
    // makes the list jump. Border and glow remain available in Advanced for anyone who wants
    // them; they are simply not the default.
    StateStyle s;
    s.icon = IconShape::None;
    s.show_icon = false;
    s.icon_color = kSpeakingGreen;
    s.override_text_color = true;
    s.text_color = kSpeakingGreen;
    s.show_border = false;
    s.glow = false;
    return s;
}

StateStyle whisper_style() {
    StateStyle s;
    s.icon = IconShape::Whisper;
    s.icon_color = kWhisperCyan;
    s.override_text_color = true;
    s.text_color = kWhisperCyan;
    s.show_border = false;
    s.glow = false;
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
    // An orange circular indicator immediately before the name.
    s.icon = IconShape::Circle;
    s.icon_color = kCommanderOrange;
    s.icon_scale = 0.62f;  // a dot beside the name, not a bullet competing with it
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
    // Where they came from is the useful part: which channel you are in is not news to you.
    n.format = "{name} joined from {from}";
    n.prefix = "[+]";
    n.icon = IconShape::Chevron;
    n.icon_color = kSpeakingGreen;
    n.name_color = kSpeakingGreen;
    n.border = Color{126, 231, 135, 120};
    return n;
}

NotificationStyle leave_notification() {
    NotificationStyle n;
    n.format = "{name} left to {to}";
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
    // Someone else's prose, of unbounded length: wrap it rather than growing a toast as wide
    // as the screen.
    n.wrap = true;
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

NotificationStyle private_chat_notification() {
    NotificationStyle n;
    // Someone else's prose, of unbounded length: wrap it rather than growing a toast as wide
    // as the screen.
    n.wrap = true;
    // Off by default, like every other path to a private message in this project: the overlay
    // never surfaces private chat until it is asked to. Switching this on also asks the plugin
    // to start sending them -- until then it is not even told about them.
    n.enabled = false;
    n.format = "{name}: {message}";
    n.prefix = "[PM]";
    n.icon = IconShape::Whisper;
    n.icon_color = Color{197, 154, 255, 255};
    n.name_color = Color{197, 154, 255, 255};
    n.border = Color{197, 154, 255, 110};
    n.fade.hold_ms = 8000;   // long enough to read a sentence and act on it
    return n;
}

NotificationStyle poke_notification() {
    NotificationStyle n;
    // On by default, and reached only once private messages are switched on -- a poke rides the
    // same subscription, so until then the plugin is never asked to send one.
    n.enabled = true;
    n.format = "{name}: {message}";
    n.prefix = "[POKE]";
    n.icon = IconShape::Star;
    n.icon_color = Color{255, 197, 132, 255};
    n.name_color = Color{255, 197, 132, 255};
    n.border = Color{255, 197, 132, 130};
    // A poke is meant to interrupt; it earns a longer read than an ordinary message.
    n.fade.hold_ms = 10000;
    n.wrap = true;
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
    c.notifications.private_chat = private_chat_notification();
    c.notifications.poke = poke_notification();

    return c;
}

}  // namespace tsro
