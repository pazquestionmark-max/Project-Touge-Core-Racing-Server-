// SPDX-License-Identifier: MIT
#include "tsro/notifications.hpp"

#include <algorithm>
#include <cstdio>

namespace tsro {
namespace {

std::string to_str(int v) {
    char buf[16];
    const int n = std::snprintf(buf, sizeof(buf), "%d", v);
    return std::string(buf, static_cast<std::size_t>(n > 0 ? n : 0));
}

const NotificationStyle& style_for(NotificationKind k, const NotificationsConfig& c) {
    switch (k) {
        case NotificationKind::Join: return c.join;
        case NotificationKind::Leave: return c.leave;
        case NotificationKind::ChannelSwitch: return c.channel_switch;
        case NotificationKind::Connection: return c.connection;
        case NotificationKind::Whisper: return c.whisper;
        case NotificationKind::Chat: return c.chat;
        case NotificationKind::PrivateChat: return c.private_chat;
        case NotificationKind::Poke: return c.poke;
    }
    return c.join;
}

const char* connection_word(ConnectionState s, DisconnectReason r) {
    switch (s) {
        case ConnectionState::Connected: return "connected";
        case ConnectionState::Connecting: return "connecting";
        case ConnectionState::Disconnected:
            switch (r) {
                case DisconnectReason::ConnectionLost: return "connection lost";
                case DisconnectReason::Kicked: return "kicked";
                case DisconnectReason::Banned: return "banned";
                case DisconnectReason::ServerShutdown: return "server stopped";
                case DisconnectReason::Timeout: return "timed out";
                case DisconnectReason::User: return "disconnected";
                case DisconnectReason::Unknown: return "disconnected";
            }
            return "disconnected";
    }
    return "disconnected";
}

}  // namespace

std::int64_t Notification::lifetime_ms() const noexcept {
    return static_cast<std::int64_t>(fade.in_ms) + fade.hold_ms + fade.out_ms;
}

float Notification::opacity(std::int64_t now_ms) const noexcept {
    const std::int64_t age = now_ms - created_ms;
    if (age < 0) return fade.start_opacity;
    if (age < fade.in_ms) {
        const float t = fade.in_ms > 0 ? static_cast<float>(age) / static_cast<float>(fade.in_ms)
                                       : 1.0f;
        const float e = ease(fade.easing, t);
        return fade.start_opacity + (fade.end_opacity - fade.start_opacity) * e;
    }
    const std::int64_t visible_until = static_cast<std::int64_t>(fade.in_ms) + fade.hold_ms;
    if (age < visible_until) return fade.end_opacity;
    if (fade.out_ms <= 0) return 0.0f;
    const float t = static_cast<float>(age - visible_until) / static_cast<float>(fade.out_ms);
    if (t >= 1.0f) return 0.0f;
    // Fade-out eases linearly out of the held opacity; no easing that overshoots is used here,
    // because an overshoot on the way out reads as a flicker.
    return fade.end_opacity * (1.0f - ease(Easing::EaseOut, t));
}

bool Notification::finished(std::int64_t now_ms) const noexcept {
    return now_ms - created_ms >= lifetime_ms();
}

void NotificationQueue::submit(const OverlayEvent& ev, const Config& cfg, std::int64_t now_ms) {
    const NotificationsConfig& nc = cfg.notifications;

    NotificationKind kind;
    FormatValues fv;
    fv.name = ev.display_name;
    fv.channel = ev.channel_name;
    fv.previous = ev.previous_channel_name;
    fv.count = to_str(ev.user_count);

    switch (ev.kind) {
        case OverlayEventKind::UserJoined: kind = NotificationKind::Join; break;
        case OverlayEventKind::UserLeft: kind = NotificationKind::Leave; break;
        case OverlayEventKind::ChannelChanged: kind = NotificationKind::ChannelSwitch; break;
        case OverlayEventKind::ConnectionChanged:
            kind = NotificationKind::Connection;
            fv.status = connection_word(ev.connection, ev.reason);
            if (ev.connection == ConnectionState::Connected) connected_at_ms_ = now_ms;
            break;
        case OverlayEventKind::WhisperStarted: kind = NotificationKind::Whisper; break;
        case OverlayEventKind::ChatMessage:
            // A message sent to you personally is its own kind of event, with its own toast --
            // the first thing you need from it is who sent it, which a channel-chat toast
            // shares a format with and so cannot make obvious.
            if (ev.chat.category == ChatCategory::Poke) kind = NotificationKind::Poke;
            else if (ev.chat.category == ChatCategory::Private) kind = NotificationKind::PrivateChat;
            else kind = NotificationKind::Chat;
            fv.message = ev.chat.text;
            fv.name = ev.chat.sender_name;
            // A sender TeamSpeak could not name is still better than an anonymous message.
            if (fv.name.empty()) fv.name = "someone";
            break;
        default:
            return;  // speaking/commander/desync changes are indicator states, not notifications
    }

    const NotificationStyle& st = style_for(kind, nc);
    if (!st.enabled) return;

    // Post-connect suppression: joins and leaves that arrive as part of the initial
    // synchronisation are state, not news.
    if ((kind == NotificationKind::Join || kind == NotificationKind::Leave) &&
        connected_at_ms_ > 0 && now_ms - connected_at_ms_ < nc.suppress_after_connect_ms) {
        return;
    }

    Notification n;
    n.kind = kind;
    n.text = format_template(st.format, fv);
    n.name = fv.name;
    n.prefix = st.prefix;
    n.name_color = st.name_color;
    n.text_color = st.text;
    n.background = st.background;
    n.border = st.border;
    n.icon = st.icon;
    n.icon_color = st.icon_color;
    n.show_background = st.show_background;
    n.show_border = st.show_border;
    n.created_ms = now_ms;
    n.fade = st.fade;
    n.phase = NotificationPhase::FadeIn;
    n.play_sound = st.sound && !st.sound_file.empty();
    n.wrap = st.wrap;
    n.max_lines = st.max_lines;
    n.sound_file = st.sound_file;

    // Per-user colour overrides apply to the name inside notifications too, so a user who is
    // always cyan in the list is also cyan in their join message.
    if (const UserOverride* ov = cfg.find_user_override(ev.unique_id)) {
        if (ov->name_color) n.name_color = *ov->name_color;
    }

    if (nc.merge_duplicates && !items_.empty()) {
        Notification& last = items_.back();
        if (last.kind == n.kind && last.text == n.text &&
            last.phase != NotificationPhase::FadeOut) {
            // Restart the lifecycle rather than stacking an identical row.
            ++last.repeat_count;
            last.created_ms = now_ms;
            last.phase = NotificationPhase::Visible;
            return;
        }
    }

    if (n.play_sound) pending_sounds_.push_back(n.sound_file);
    n.id = next_id_++;
    items_.push_back(std::move(n));

    // Overflow: drop the oldest *immediately* rather than letting the queue grow. This is a
    // hard bound, so a flood of events cannot consume memory or screen.
    const std::size_t max = static_cast<std::size_t>(std::max(1, nc.max_visible));
    while (items_.size() > max) items_.pop_front();
}

std::size_t NotificationQueue::tick(std::int64_t now_ms, const Config& cfg) {
    const std::size_t before = items_.size();
    for (auto& n : items_) {
        const std::int64_t age = now_ms - n.created_ms;
        if (age < n.fade.in_ms) n.phase = NotificationPhase::FadeIn;
        else if (age < static_cast<std::int64_t>(n.fade.in_ms) + n.fade.hold_ms)
            n.phase = NotificationPhase::Visible;
        else if (age < n.lifetime_ms()) n.phase = NotificationPhase::FadeOut;
        else n.phase = NotificationPhase::Dead;
    }
    // Only fully-finished entries are erased: an entry mid-fade-out keeps its slot.
    items_.erase(std::remove_if(items_.begin(), items_.end(),
                                [&](const Notification& n) {
                                    return n.phase == NotificationPhase::Dead;
                                }),
                 items_.end());
    const std::size_t max = static_cast<std::size_t>(std::max(1, cfg.notifications.max_visible));
    while (items_.size() > max) items_.pop_front();
    return before - items_.size();
}

std::vector<std::string> NotificationQueue::drain_sounds() {
    std::vector<std::string> out;
    out.swap(pending_sounds_);
    return out;
}

void SpeakingEnvelope::update(const std::vector<std::pair<std::string, bool>>& talking,
                              std::int64_t dt_ms) {
    if (dt_ms < 0) dt_ms = 0;
    const float dt = static_cast<float>(dt_ms);
    const float up = dt / static_cast<float>(attack_ms_);
    const float down = dt / static_cast<float>(release_ms_);

    for (const auto& [uid, is_talking] : talking) {
        float& level = levels_[uid];
        if (is_talking) level = std::min(1.0f, level + up);
        else level = std::max(0.0f, level - down);
    }

    // Drop users who have fully decayed and are no longer present, so the map cannot grow
    // without bound over a long session in a busy channel.
    for (auto it = levels_.begin(); it != levels_.end();) {
        const bool present = std::any_of(
            talking.begin(), talking.end(),
            [&](const std::pair<std::string, bool>& p) { return p.first == it->first; });
        if (!present && it->second <= 0.0f) it = levels_.erase(it);
        else if (!present) {
            it->second = std::max(0.0f, it->second - down);
            ++it;
        } else {
            ++it;
        }
    }
}

float SpeakingEnvelope::intensity(const std::string& unique_id) const {
    const auto it = levels_.find(unique_id);
    return it == levels_.end() ? 0.0f : it->second;
}

}  // namespace tsro
