// SPDX-License-Identifier: MIT
// Overlay-side IPC client: owns the connection, the reconnect policy and the StateStore, and
// publishes finished frames for the render thread to pick up.
//
// Everything expensive — connecting, blocking reads, JSON parsing, state application — happens
// on this class's own thread. The render thread's entire interaction is `latest()`, which is one
// atomic exchange against a triple buffer.
#ifndef TSRO_OVERLAY_CLIENT_HPP
#define TSRO_OVERLAY_CLIENT_HPP

#include <atomic>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "tsro/protocol.hpp"
#include "tsro/state_store.hpp"
#include "tsro/transport.hpp"
#include "tsro/triple_buffer.hpp"

namespace tsro {

enum class LinkState { Idle, Connecting, Handshaking, Connected, Backoff, Failed };

const char* to_string(LinkState) noexcept;

struct OverlayClientConfig {
    std::string endpoint;                 ///< empty ⇒ default_endpoint("")
    int reconnect_initial_ms = 250;
    int reconnect_max_ms = 5000;
    int stale_after_ms = 6000;
    int ping_interval_ms = 10000;
    std::size_t chat_history = 50;
    std::size_t max_pending_events = 256;
    proto::ClientHelloPayload hello;
    proto::ConfigurationUpdatedPayload subscription;
};

/// A complete, self-consistent view of the world at one instant. Published as a unit so the
/// renderer never sees a half-updated user list.
struct OverlayFrame {
    OverlayState state;
    std::vector<ChatMessage> chat;
    LinkState link = LinkState::Idle;
    std::string link_detail;
    std::int64_t published_ms = 0;
    std::uint64_t revision = 0;
};

struct LinkDiagnostics {
    LinkState state = LinkState::Idle;
    std::string detail;
    std::string endpoint;
    int reconnect_attempts = 0;
    int next_retry_in_ms = 0;
    std::int64_t connected_since_ms = 0;
    std::int64_t round_trip_ms = -1;
    std::string plugin_version;
    std::string ts_client_version;
    int plugin_api_version = 0;
    int protocol_version = proto::kProtocolVersion;
    std::vector<std::string> capabilities;
    StoreStats store;
};

class OverlayClient {
public:
    explicit OverlayClient(OverlayClientConfig config);
    ~OverlayClient();

    OverlayClient(const OverlayClient&) = delete;
    OverlayClient& operator=(const OverlayClient&) = delete;

    void start();
    void stop();
    bool running() const noexcept { return running_.load(std::memory_order_acquire); }

    /// Newest published frame. Safe and cheap to call every frame from the render thread.
    const OverlayFrame& latest() noexcept { return frames_.acquire(); }

    /// Takes the semantic events accumulated since the last call, for the notification layer.
    std::vector<OverlayEvent> drain_events();

    /// Applies a new chat subscription and pushes it to the plugin. Safe from any thread.
    void update_subscription(const proto::ConfigurationUpdatedPayload& subscription);
    void set_endpoint(const std::string& endpoint);
    void set_stale_after_ms(int ms);

    /// Forces an immediate reconnect attempt (the Diagnostics tab's "Reconnect" button).
    void request_reconnect();

    LinkDiagnostics diagnostics() const;

private:
    void run();
    /// One connection's lifetime. Returns when the link drops for any reason.
    void session(std::unique_ptr<Connection> connection);
    void publish(LinkState link, std::string detail);
    void push_events(std::vector<OverlayEvent>& events);
    bool send(Connection& connection, proto::MessageType type, const json::Value& data);
    void set_link(LinkState state, std::string detail);

    OverlayClientConfig config_;
    std::string endpoint_;

    std::thread thread_;
    std::atomic<bool> running_{false};
    std::atomic<bool> reconnect_requested_{false};

    std::mutex wake_mutex_;
    std::condition_variable wake_cv_;

    TripleBuffer<OverlayFrame> frames_;
    std::uint64_t revision_ = 0;

    StateStore store_;
    std::int64_t client_seq_ = 0;

    mutable std::mutex events_mutex_;
    std::deque<OverlayEvent> pending_events_;

    mutable std::mutex diag_mutex_;
    LinkDiagnostics diag_;

    mutable std::mutex config_mutex_;
    proto::ConfigurationUpdatedPayload subscription_;
    std::atomic<bool> subscription_dirty_{false};
    Connection* active_connection_ = nullptr;  ///< guarded by config_mutex_; for cancel-on-stop
};

}  // namespace tsro

#endif  // TSRO_OVERLAY_CLIENT_HPP
