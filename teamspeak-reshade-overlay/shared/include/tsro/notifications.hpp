// SPDX-License-Identifier: MIT
// Notification lifecycle and the speaking-intensity envelope.
//
// Lifecycle, exactly as the brief specifies: received -> validated -> created -> fade-in ->
// visible -> fade-out -> removed. A notification is never removed mid-animation; expiry marks
// it for fade-out and removal happens only once the fade-out has finished.
#ifndef TSRO_NOTIFICATIONS_HPP
#define TSRO_NOTIFICATIONS_HPP

#include <cstdint>
#include <deque>
#include <string>
#include <unordered_map>
#include <vector>

#include "tsro/config.hpp"
#include "tsro/layout.hpp"
#include "tsro/state_store.hpp"

namespace tsro {

enum class NotificationKind {
    Join, Leave, ChannelSwitch, Connection, Whisper, Chat, PrivateChat
};

enum class NotificationPhase { FadeIn, Visible, FadeOut, Dead };

struct Notification {
    std::uint64_t id = 0;
    NotificationKind kind = NotificationKind::Join;
    std::string text;        ///< already formatted
    std::string name;        ///< the part drawn in name_color
    std::string prefix;
    Color name_color{};
    Color text_color{};
    Color background{};
    Color border{};
    Color icon_color{};
    IconShape icon = IconShape::None;
    bool show_background = true;
    bool show_border = true;
    bool play_sound = false;
    /// Carried from the style so the renderer does not need the configuration to decide whether
    /// this toast grows sideways or downwards.
    bool wrap = false;
    int max_lines = 4;
    std::string sound_file;

    std::int64_t created_ms = 0;
    Fade fade;
    NotificationPhase phase = NotificationPhase::FadeIn;
    int repeat_count = 1;    ///< >1 when merge_duplicates folded identical events together

    /// Current opacity, 0..1.
    float opacity(std::int64_t now_ms) const noexcept;
    /// Whether the whole lifecycle has completed and the entry can be dropped.
    bool finished(std::int64_t now_ms) const noexcept;
    /// Total lifetime in milliseconds.
    std::int64_t lifetime_ms() const noexcept;
};

/// Turns OverlayEvents into Notifications and ages them out. Bounded by config.max_visible;
/// overflow evicts the oldest so the queue cannot grow without limit.
class NotificationQueue {
public:
    /// `connected_at_ms` seeds the post-connect suppression window so an initial state
    /// synchronisation does not produce a join notification per channel member.
    void note_connected(std::int64_t now_ms) { connected_at_ms_ = now_ms; }

    void submit(const OverlayEvent& ev, const Config& cfg, std::int64_t now_ms);
    /// Advances lifecycles and removes finished entries. Returns the number removed.
    std::size_t tick(std::int64_t now_ms, const Config& cfg);
    void clear() { items_.clear(); }

    const std::deque<Notification>& items() const noexcept { return items_; }
    std::size_t size() const noexcept { return items_.size(); }
    /// Sounds requested since the last drain. The renderer plays and clears these; the queue
    /// itself never touches audio.
    std::vector<std::string> drain_sounds();

private:
    std::deque<Notification> items_;
    std::vector<std::string> pending_sounds_;
    std::uint64_t next_id_ = 1;
    std::int64_t connected_at_ms_ = 0;
};

/// Attack/release envelope per user, so a speaking indicator ramps rather than snapping and a
/// short burst still registers visibly.
class SpeakingEnvelope {
public:
    void set_config(int attack_ms, int release_ms) {
        attack_ms_ = attack_ms > 0 ? attack_ms : 1;
        release_ms_ = release_ms > 0 ? release_ms : 1;
    }
    /// Feeds the current talking states and advances by `dt_ms`.
    void update(const std::vector<std::pair<std::string, bool>>& talking, std::int64_t dt_ms);
    float intensity(const std::string& unique_id) const;
    void clear() { levels_.clear(); }
    std::size_t tracked() const noexcept { return levels_.size(); }

private:
    std::unordered_map<std::string, float> levels_;
    int attack_ms_ = 90;
    int release_ms_ = 260;
};

}  // namespace tsro

#endif  // TSRO_NOTIFICATIONS_HPP
