// SPDX-License-Identifier: MIT
#include "tsro/overlay_client.hpp"

#include <algorithm>
#include <chrono>
#include <random>

#include "tsro/framing.hpp"
#include "tsro/log.hpp"

namespace tsro {
namespace {

constexpr char kComponent[] = "ipc.client";

std::int64_t now_ms() { return proto::now_unix_ms(); }

/// Jitter spreads reconnect attempts so several games launching at once do not synchronise.
int jittered(int base_ms) {
    static thread_local std::mt19937 rng{std::random_device{}()};
    if (base_ms <= 0) return 0;
    std::uniform_int_distribution<int> dist(base_ms / 2, base_ms);
    return dist(rng);
}

}  // namespace

const char* to_string(LinkState s) noexcept {
    switch (s) {
        case LinkState::Idle: return "idle";
        case LinkState::Connecting: return "connecting";
        case LinkState::Handshaking: return "handshaking";
        case LinkState::Connected: return "connected";
        case LinkState::Backoff: return "waiting to retry";
        case LinkState::Failed: return "failed";
    }
    return "idle";
}

OverlayClient::OverlayClient(OverlayClientConfig config) : config_(std::move(config)) {
    endpoint_ = default_endpoint(config_.endpoint);
    subscription_ = config_.subscription;

    StoreConfig sc;
    sc.stale_after_ms = config_.stale_after_ms;
    sc.chat_history = config_.chat_history;
    store_.set_config(sc);

    diag_.endpoint = endpoint_;
    diag_.state = LinkState::Idle;
}

OverlayClient::~OverlayClient() { stop(); }

void OverlayClient::start() {
    if (running_.exchange(true, std::memory_order_acq_rel)) return;
    thread_ = std::thread([this] { run(); });
}

void OverlayClient::stop() {
    if (!running_.exchange(false, std::memory_order_acq_rel)) return;
    {
        std::lock_guard<std::mutex> lock(config_mutex_);
        // Unblock a read that may be parked indefinitely on a healthy but silent connection.
        if (active_connection_ != nullptr) active_connection_->cancel();
    }
    wake_cv_.notify_all();
    if (thread_.joinable()) thread_.join();
}

void OverlayClient::set_link(LinkState state, std::string detail) {
    std::lock_guard<std::mutex> lock(diag_mutex_);
    diag_.state = state;
    diag_.detail = std::move(detail);
}

void OverlayClient::publish(LinkState link, std::string detail) {
    OverlayFrame& frame = frames_.write_slot();
    frame.state = store_.state();
    frame.chat.assign(store_.chat().begin(), store_.chat().end());
    frame.link = link;
    frame.link_detail = std::move(detail);
    frame.published_ms = now_ms();
    frame.revision = ++revision_;
    frames_.publish();
}

void OverlayClient::push_events(std::vector<OverlayEvent>& events) {
    if (events.empty()) return;
    std::lock_guard<std::mutex> lock(events_mutex_);
    for (auto& e : events) {
        pending_events_.push_back(std::move(e));
        // Bounded: if the renderer stops draining (paused game, alt-tab), the oldest events are
        // dropped rather than the queue growing without limit.
        if (pending_events_.size() > config_.max_pending_events) pending_events_.pop_front();
    }
    events.clear();
}

std::vector<OverlayEvent> OverlayClient::drain_events() {
    std::lock_guard<std::mutex> lock(events_mutex_);
    std::vector<OverlayEvent> out(pending_events_.begin(), pending_events_.end());
    pending_events_.clear();
    return out;
}

void OverlayClient::update_subscription(const proto::ConfigurationUpdatedPayload& subscription) {
    std::lock_guard<std::mutex> lock(config_mutex_);
    subscription_ = subscription;
    subscription_dirty_.store(true, std::memory_order_release);
    if (active_connection_ != nullptr) {
        // Wake the reader so the change is sent promptly rather than at the next heartbeat.
        wake_cv_.notify_all();
    }
}

void OverlayClient::set_endpoint(const std::string& endpoint) {
    {
        std::lock_guard<std::mutex> lock(config_mutex_);
        endpoint_ = default_endpoint(endpoint);
        if (active_connection_ != nullptr) active_connection_->cancel();
    }
    {
        std::lock_guard<std::mutex> lock(diag_mutex_);
        diag_.endpoint = endpoint_;
    }
    request_reconnect();
}

void OverlayClient::set_stale_after_ms(int ms) {
    StoreConfig sc;
    sc.stale_after_ms = ms;
    sc.chat_history = config_.chat_history;
    config_.stale_after_ms = ms;
    store_.set_config(sc);
}

void OverlayClient::request_reconnect() {
    reconnect_requested_.store(true, std::memory_order_release);
    {
        std::lock_guard<std::mutex> lock(config_mutex_);
        if (active_connection_ != nullptr) active_connection_->cancel();
    }
    wake_cv_.notify_all();
}

bool OverlayClient::send(Connection& connection, proto::MessageType type,
                         const json::Value& data) {
    const std::string line = proto::make_line(type, client_seq_++, now_ms(), "", data);
    return connection.write_all(line.data(), line.size());
}

void OverlayClient::run() {
    int backoff = config_.reconnect_initial_ms;
    int attempts = 0;

    while (running_.load(std::memory_order_acquire)) {
        std::string endpoint;
        {
            std::lock_guard<std::mutex> lock(config_mutex_);
            endpoint = endpoint_;
        }

        set_link(LinkState::Connecting, "opening " + endpoint);
        publish(LinkState::Connecting, "connecting");

        std::string error;
        std::unique_ptr<Connection> connection = connect_client(endpoint, 1000, error);
        if (!connection) {
            ++attempts;
            {
                std::lock_guard<std::mutex> lock(diag_mutex_);
                diag_.reconnect_attempts = attempts;
                diag_.next_retry_in_ms = backoff;
            }
            // TeamSpeak simply not being open is the common case and must not spam the log at
            // warning level on every retry.
            if (attempts == 1 || attempts % 20 == 0) {
                TSRO_INFO(kComponent, "TeamSpeak plugin not reachable (" + error + ")");
            }
            set_link(LinkState::Backoff, error);
            publish(LinkState::Backoff, error);

            std::unique_lock<std::mutex> lock(wake_mutex_);
            wake_cv_.wait_for(lock, std::chrono::milliseconds(jittered(backoff)), [this] {
                return !running_.load(std::memory_order_acquire) ||
                       reconnect_requested_.load(std::memory_order_acquire);
            });
            reconnect_requested_.store(false, std::memory_order_release);
            backoff = std::min(backoff * 2, config_.reconnect_max_ms);
            continue;
        }

        attempts = 0;
        backoff = config_.reconnect_initial_ms;
        TSRO_INFO(kComponent, "connected to the TeamSpeak plugin");
        session(std::move(connection));

        if (!running_.load(std::memory_order_acquire)) break;
        // A dropped link is normal (TeamSpeak restarted); retry from the shortest interval.
        std::unique_lock<std::mutex> lock(wake_mutex_);
        wake_cv_.wait_for(lock, std::chrono::milliseconds(jittered(config_.reconnect_initial_ms)),
                          [this] { return !running_.load(std::memory_order_acquire); });
    }

    set_link(LinkState::Idle, "stopped");
    publish(LinkState::Idle, "stopped");
}

void OverlayClient::session(std::unique_ptr<Connection> connection) {
    {
        std::lock_guard<std::mutex> lock(config_mutex_);
        active_connection_ = connection.get();
    }
    struct Clear {
        OverlayClient* self;
        ~Clear() {
            std::lock_guard<std::mutex> lock(self->config_mutex_);
            self->active_connection_ = nullptr;
        }
    } clear{this};

    client_seq_ = 0;
    set_link(LinkState::Handshaking, "sending client_hello");

    proto::ClientHelloPayload hello = config_.hello;
    hello.protocol = proto::kProtocolVersion;
    if (!send(*connection, proto::MessageType::ClientHello, proto::encode(hello))) return;

    {
        proto::ConfigurationUpdatedPayload sub;
        {
            std::lock_guard<std::mutex> lock(config_mutex_);
            sub = subscription_;
        }
        if (!send(*connection, proto::MessageType::ConfigurationUpdated, proto::encode(sub)))
            return;
        subscription_dirty_.store(false, std::memory_order_release);
    }

    {
        std::lock_guard<std::mutex> lock(diag_mutex_);
        diag_.connected_since_ms = now_ms();
        diag_.reconnect_attempts = 0;
        diag_.next_retry_in_ms = 0;
    }

    LineFramer framer(proto::kMaxMessageBytes);
    std::vector<std::string> lines;
    std::vector<OverlayEvent> events;
    char buffer[16384];
    std::int64_t last_ping = now_ms();
    std::int64_t ping_nonce = 1;

    while (running_.load(std::memory_order_acquire) &&
           !reconnect_requested_.load(std::memory_order_acquire)) {
        const int n = connection->read(buffer, sizeof(buffer));
        if (n <= 0) break;

        lines.clear();
        if (!framer.feed(std::string_view(buffer, static_cast<std::size_t>(n)), lines)) {
            TSRO_WARN(kComponent, "plugin sent an oversized frame; dropping the connection");
            break;
        }

        bool disconnect = false;
        for (const std::string& line : lines) {
            const proto::DecodeResult decoded = proto::decode(line);
            if (!decoded.ok()) {
                if (decoded.fatal()) {
                    TSRO_WARN(kComponent, "malformed message from the plugin: " + decoded.error);
                    disconnect = true;
                    break;
                }
                continue;
            }
            const StoreAction action = store_.apply(decoded.envelope, events);
            if (action == StoreAction::Disconnect) {
                TSRO_WARN(kComponent, "protocol error; dropping the connection");
                disconnect = true;
                break;
            }
            if (action == StoreAction::RequestSnapshot) {
                proto::RequestSnapshotPayload req;
                req.reason = "sequence_gap";
                if (!send(*connection, proto::MessageType::RequestSnapshot,
                          proto::encode(req))) {
                    disconnect = true;
                    break;
                }
            }
        }

        const std::int64_t now = now_ms();
        if (store_.tick(now, events) == StoreAction::RequestSnapshot) {
            proto::RequestSnapshotPayload req;
            req.reason = "stale";
            send(*connection, proto::MessageType::RequestSnapshot, proto::encode(req));
        }

        if (subscription_dirty_.exchange(false, std::memory_order_acq_rel)) {
            proto::ConfigurationUpdatedPayload sub;
            {
                std::lock_guard<std::mutex> lock(config_mutex_);
                sub = subscription_;
            }
            if (!send(*connection, proto::MessageType::ConfigurationUpdated,
                      proto::encode(sub))) {
                disconnect = true;
            }
        }

        if (!disconnect && config_.ping_interval_ms > 0 &&
            now - last_ping >= config_.ping_interval_ms) {
            proto::PingPayload ping;
            ping.nonce = ping_nonce++;
            store_.note_ping_sent(ping.nonce, now);
            last_ping = now;
            if (!send(*connection, proto::MessageType::Ping, proto::encode(ping)))
                disconnect = true;
        }

        push_events(events);
        {
            std::lock_guard<std::mutex> lock(diag_mutex_);
            const proto::HelloPayload& hp = store_.server_hello();
            diag_.plugin_version = hp.plugin_version;
            diag_.ts_client_version = hp.ts_client_version;
            diag_.plugin_api_version = hp.plugin_api_version;
            diag_.capabilities = hp.capabilities;
            diag_.store = store_.stats();
            diag_.round_trip_ms = store_.stats().round_trip_ms;
        }
        const LinkState link =
            store_.handshaken() ? LinkState::Connected : LinkState::Handshaking;
        set_link(link, store_.state().stale ? "stale" : "live");
        publish(link, store_.state().stale ? "stale" : "live");

        if (disconnect) break;
    }

    // The link is gone: clear the model so a stale user list cannot survive on screen, and tell
    // the renderer why.
    store_.on_disconnected(now_ms(), events);
    push_events(events);
    set_link(LinkState::Backoff, "disconnected from the TeamSpeak plugin");
    publish(LinkState::Backoff, "disconnected from the TeamSpeak plugin");
    reconnect_requested_.store(false, std::memory_order_release);
    TSRO_INFO(kComponent, "connection closed");
}

LinkDiagnostics OverlayClient::diagnostics() const {
    std::lock_guard<std::mutex> lock(diag_mutex_);
    return diag_;
}

}  // namespace tsro
