// SPDX-License-Identifier: MIT
// Overlay-side state management: applies protocol messages to an OverlayState, detects gaps,
// duplicates and staleness, and emits the semantic events the notification layer consumes.
//
// Entirely platform-independent and side-effect free, which is what makes the whole event
// pipeline unit-testable without a game, a GPU or a TeamSpeak client.
#ifndef TSRO_STATE_STORE_HPP
#define TSRO_STATE_STORE_HPP

#include <cstdint>
#include <deque>
#include <string>
#include <vector>

#include "tsro/model.hpp"
#include "tsro/protocol.hpp"

namespace tsro {

/// What the store wants the transport to do after applying a message.
enum class StoreAction {
    None,
    RequestSnapshot,  ///< sequence gap or staleness: ask the server to resynchronise
    Disconnect,       ///< fatal protocol condition; close and reconnect
};

enum class OverlayEventKind {
    UserJoined,
    UserLeft,
    ChannelChanged,
    ConnectionChanged,
    ChatMessage,
    SpeakingStarted,
    SpeakingStopped,
    CommanderChanged,
    WhisperStarted,
    WhisperStopped,
    Desynchronised,
};

/// A semantic change, already normalised for presentation. The notification layer turns these
/// into on-screen notifications; the renderer uses them for transition animations.
struct OverlayEvent {
    OverlayEventKind kind = OverlayEventKind::UserJoined;
    std::string unique_id;
    std::string display_name;
    std::string channel_name;
    std::string previous_channel_name;
    int user_count = 0;
    ConnectionState connection = ConnectionState::Disconnected;
    DisconnectReason reason = DisconnectReason::Unknown;
    JoinCause cause = JoinCause::Moved;
    ChatMessage chat;
    std::int64_t timestamp_ms = 0;
};

struct StoreStats {
    std::uint64_t applied = 0;
    std::uint64_t duplicates_dropped = 0;
    std::uint64_t gaps_detected = 0;
    std::uint64_t unknown_types = 0;
    std::uint64_t malformed_payloads = 0;
    std::uint64_t snapshots = 0;
    std::int64_t last_message_ms = 0;
    std::string last_message_type;
    std::int64_t round_trip_ms = -1;
};

struct StoreConfig {
    /// No message at all for this long ⇒ state is stale and must stop being presented as live.
    std::int64_t stale_after_ms = 6000;
    std::size_t chat_history = 50;
    std::size_t max_events_per_apply = 64;
};

class StateStore {
public:
    explicit StateStore(StoreConfig cfg = {}) : cfg_(cfg) {}

    /// Applies one decoded envelope. Emitted events are appended to `events`.
    StoreAction apply(const proto::Envelope& env, std::vector<OverlayEvent>& events);

    /// Advances wall-clock-driven behaviour (staleness). Call once per frame or per tick.
    /// Returns RequestSnapshot the first time the state goes stale, so the caller resynchronises
    /// rather than leaving an outdated user list on screen indefinitely.
    StoreAction tick(std::int64_t now_ms, std::vector<OverlayEvent>& events);

    /// Clears everything except statistics. Called when the pipe drops.
    void on_disconnected(std::int64_t now_ms, std::vector<OverlayEvent>& events);

    const OverlayState& state() const noexcept { return state_; }
    const std::deque<ChatMessage>& chat() const noexcept { return chat_; }
    const StoreStats& stats() const noexcept { return stats_; }
    const proto::HelloPayload& server_hello() const noexcept { return hello_; }
    bool has_capability(std::string_view name) const noexcept;
    bool handshaken() const noexcept { return handshaken_; }

    void set_config(const StoreConfig& c) { cfg_ = c; }
    void note_ping_sent(std::int64_t nonce, std::int64_t now_ms) {
        ping_nonce_ = nonce;
        ping_sent_ms_ = now_ms;
    }

private:
    UserState* mutable_user(std::uint16_t client_id, std::string_view unique_id);
    void push_chat(ChatMessage m);
    void mark_activity(std::int64_t ts);

    StoreConfig cfg_;
    OverlayState state_;
    std::deque<ChatMessage> chat_;
    StoreStats stats_;
    proto::HelloPayload hello_;
    bool handshaken_ = false;
    bool have_seq_ = false;
    std::int64_t last_seq_ = -1;
    std::int64_t ping_nonce_ = -1;
    std::int64_t ping_sent_ms_ = 0;
    bool stale_reported_ = false;
};

}  // namespace tsro

#endif  // TSRO_STATE_STORE_HPP
