// SPDX-License-Identifier: MIT
#include "tsro/ipc_server.hpp"

#include <algorithm>
#include <chrono>

#include "tsro/framing.hpp"
#include "tsro/log.hpp"

namespace tsro {
namespace {
constexpr char kComponent[] = "ipc.server";
}

/// One connected overlay. Two threads: a reader (blocked on the transport) and a writer (blocked
/// on the outbound queue). Nothing else ever touches the connection handle.
struct IpcServer::Session {
    std::unique_ptr<Connection> connection;
    std::thread reader;
    std::thread writer;

    std::mutex mutex;                     ///< guards queue, subscription, seq, handshake
    std::condition_variable cv;
    std::deque<std::string> queue;
    proto::ChatSubscription subscription; ///< all false until the client opts in
    std::int64_t seq = 0;
    bool handshaken = false;
    bool want_speaking = true;
    std::atomic<bool> closing{false};
    std::atomic<bool> finished{false};

    std::string description;
    std::int64_t connected_ms = 0;
    std::int64_t rate_window_start_ms = 0;
    int rate_window_count = 0;
    std::uint64_t messages_sent = 0;
    std::uint64_t messages_dropped = 0;

    void shutdown() {
        closing.store(true, std::memory_order_release);
        if (connection) connection->cancel();
        cv.notify_all();
    }
};

IpcServer::IpcServer(IpcServerConfig config, proto::HelloPayload hello, SnapshotFn snapshot)
    : config_(std::move(config)), hello_(std::move(hello)), snapshot_(std::move(snapshot)) {
    endpoint_ = default_endpoint(config_.endpoint);
    if (config_.queue_depth < 8) config_.queue_depth = 8;
    if (config_.max_clients < 1) config_.max_clients = 1;
}

IpcServer::~IpcServer() { stop(); }

bool IpcServer::start(std::string& error) {
    if (running_.load(std::memory_order_acquire)) return true;
    transport_ = create_server(endpoint_, error);
    if (!transport_) {
        TSRO_ERROR(kComponent, "could not create the IPC endpoint: " + error);
        return false;
    }
    started_ms_ = proto::now_unix_ms();
    running_.store(true, std::memory_order_release);
    accept_thread_ = std::thread([this] { accept_loop(); });
    heartbeat_thread_ = std::thread([this] { heartbeat_loop(); });
    TSRO_INFO(kComponent, "listening on " + endpoint_);
    return true;
}

void IpcServer::stop() {
    if (!running_.exchange(false, std::memory_order_acq_rel)) return;
    TSRO_INFO(kComponent, "shutting down");

    if (transport_) transport_->stop();
    shutdown_cv_.notify_all();

    // Signal every session before joining any of them, so shutdown is concurrent rather than
    // serialised behind each connection's read timeout.
    std::vector<std::shared_ptr<Session>> sessions;
    {
        std::lock_guard<std::mutex> lock(sessions_mutex_);
        sessions = sessions_;
    }
    for (auto& s : sessions) s->shutdown();

    if (accept_thread_.joinable()) accept_thread_.join();
    if (heartbeat_thread_.joinable()) heartbeat_thread_.join();

    for (auto& s : sessions) {
        if (s->reader.joinable()) s->reader.join();
        if (s->writer.joinable()) s->writer.join();
    }
    {
        std::lock_guard<std::mutex> lock(sessions_mutex_);
        sessions_.clear();
    }
    transport_.reset();
}

void IpcServer::reap_finished_sessions() {
    std::vector<std::shared_ptr<Session>> dead;
    {
        std::lock_guard<std::mutex> lock(sessions_mutex_);
        for (auto it = sessions_.begin(); it != sessions_.end();) {
            if ((*it)->finished.load(std::memory_order_acquire)) {
                dead.push_back(*it);
                it = sessions_.erase(it);
            } else {
                ++it;
            }
        }
    }
    for (auto& s : dead) {
        if (s->reader.joinable()) s->reader.join();
        if (s->writer.joinable()) s->writer.join();
        TSRO_INFO(kComponent, "client disconnected (" + s->description + ")");
    }
}

void IpcServer::accept_loop() {
    while (running_.load(std::memory_order_acquire)) {
        reap_finished_sessions();

        std::unique_ptr<Connection> conn = transport_->accept(500);
        if (!conn) continue;
        if (!running_.load(std::memory_order_acquire)) break;

        std::size_t count;
        {
            std::lock_guard<std::mutex> lock(sessions_mutex_);
            count = sessions_.size();
        }
        if (count >= static_cast<std::size_t>(config_.max_clients)) {
            proto::ErrorPayload err;
            err.code = "rate_limited";
            err.message = "too many overlay clients are already connected";
            err.fatal = true;
            const std::string line =
                proto::make_line(proto::MessageType::Error, 0, proto::now_unix_ms(), "",
                                 proto::encode(err));
            conn->write_all(line.data(), line.size());
            {
                std::lock_guard<std::mutex> lock(stats_mutex_);
                ++stats_.connections_rejected;
            }
            TSRO_WARN(kComponent, "rejected a client: connection limit reached");
            continue;
        }

        auto session = std::make_shared<Session>();
        session->connection = std::move(conn);
        session->description = session->connection->peer_description();
        session->connected_ms = proto::now_unix_ms();
        session->rate_window_start_ms = session->connected_ms;

        // Greet before anything else: hello then a full snapshot, so a client is usable from
        // its very first frame without needing to have seen any prior event.
        {
            std::lock_guard<std::mutex> lock(session->mutex);
            session->queue.push_back(proto::make_line(proto::MessageType::Hello, session->seq++,
                                                      proto::now_unix_ms(), "",
                                                      proto::encode(hello_)));
            enqueue_snapshot_locked(*session);
        }

        session->reader = std::thread([this, session] { session_read_loop(session); });
        session->writer = std::thread([this, session] { session_write_loop(session); });

        {
            std::lock_guard<std::mutex> lock(sessions_mutex_);
            sessions_.push_back(session);
        }
        {
            std::lock_guard<std::mutex> lock(stats_mutex_);
            ++stats_.connections_accepted;
        }
        TSRO_INFO(kComponent, "client connected (" + session->description + ")");
    }
}

void IpcServer::heartbeat_loop() {
    while (running_.load(std::memory_order_acquire)) {
        {
            std::unique_lock<std::mutex> lock(shutdown_mutex_);
            shutdown_cv_.wait_for(lock, std::chrono::milliseconds(config_.heartbeat_ms),
                                  [this] { return !running_.load(std::memory_order_acquire); });
        }
        if (!running_.load(std::memory_order_acquire)) break;

        proto::HeartbeatPayload hb;
        hb.uptime_ms = proto::now_unix_ms() - started_ms_;
        {
            std::lock_guard<std::mutex> lock(sessions_mutex_);
            hb.connected_clients = static_cast<int>(sessions_.size());
        }
        broadcast(proto::MessageType::Heartbeat, proto::encode(hb), "");
    }
}

void IpcServer::enqueue_snapshot_locked(Session& session) {
    json::Value data = snapshot_ ? snapshot_() : json::Value(json::Object{});
    session.queue.push_back(proto::make_line(proto::MessageType::StateSnapshot, session.seq++,
                                             proto::now_unix_ms(), "", std::move(data)));
}

void IpcServer::enqueue(Session& session, std::string line) {
    std::lock_guard<std::mutex> lock(session.mutex);
    if (session.queue.size() >= config_.queue_depth) {
        // The client has stopped reading. Rather than growing without bound or blocking the
        // caller (which may be the TeamSpeak callback thread), throw the backlog away and queue
        // a snapshot: the client converges on the truth in one message instead of replaying a
        // stale event history.
        const std::size_t dropped = session.queue.size();
        session.queue.clear();
        session.messages_dropped += dropped;
        enqueue_snapshot_locked(session);
        {
            std::lock_guard<std::mutex> slock(stats_mutex_);
            stats_.messages_dropped += dropped;
            ++stats_.resyncs_forced;
        }
        TSRO_WARN(kComponent, "client queue overflowed; forced a resynchronisation");
        session.cv.notify_one();
        return;
    }
    session.queue.push_back(std::move(line));
    session.cv.notify_one();
}

void IpcServer::broadcast(proto::MessageType type, json::Value data, std::string_view server_uid) {
    std::vector<std::shared_ptr<Session>> targets;
    {
        std::lock_guard<std::mutex> lock(sessions_mutex_);
        targets = sessions_;
    }
    const std::int64_t ts = proto::now_unix_ms();
    for (auto& session : targets) {
        if (session->closing.load(std::memory_order_acquire)) continue;
        std::int64_t seq;
        {
            std::lock_guard<std::mutex> lock(session->mutex);
            if (!session->want_speaking && type == proto::MessageType::SpeakingChanged) continue;
            seq = session->seq++;
            ++session->messages_sent;
        }
        enqueue(*session, proto::make_line(type, seq, ts, server_uid, data));
    }
    std::lock_guard<std::mutex> lock(stats_mutex_);
    stats_.messages_sent += targets.size();
}

void IpcServer::broadcast_chat(const ChatMessage& message, std::string_view server_uid) {
    std::vector<std::shared_ptr<Session>> targets;
    {
        std::lock_guard<std::mutex> lock(sessions_mutex_);
        targets = sessions_;
    }
    ChatMessage copy = message;
    if (copy.id == 0) copy.id = next_chat_id_.fetch_add(1, std::memory_order_relaxed);
    const json::Value data = encode_chat(copy);
    const std::int64_t ts = proto::now_unix_ms();

    for (auto& session : targets) {
        if (session->closing.load(std::memory_order_acquire)) continue;
        std::int64_t seq;
        {
            std::lock_guard<std::mutex> lock(session->mutex);
            // Privacy gate: a category the client did not opt into is never serialised to it.
            if (!session->subscription.enabled_for(copy.category)) continue;
            seq = session->seq++;
        }
        enqueue(*session, proto::make_line(proto::MessageType::ChatMessageMsg, seq, ts,
                                           server_uid, data));
    }
}

void IpcServer::broadcast_snapshot(std::string_view server_uid) {
    if (!snapshot_) return;
    broadcast(proto::MessageType::StateSnapshot, snapshot_(), server_uid);
}

void IpcServer::session_write_loop(std::shared_ptr<Session> session) {
    while (true) {
        std::string line;
        {
            std::unique_lock<std::mutex> lock(session->mutex);
            session->cv.wait(lock, [&] {
                return !session->queue.empty() ||
                       session->closing.load(std::memory_order_acquire) ||
                       !running_.load(std::memory_order_acquire);
            });
            if (session->closing.load(std::memory_order_acquire) ||
                !running_.load(std::memory_order_acquire)) {
                break;
            }
            line = std::move(session->queue.front());
            session->queue.pop_front();
        }
        if (!session->connection->write_all(line.data(), line.size())) break;
    }
    session->shutdown();
    session->finished.store(true, std::memory_order_release);
}

void IpcServer::session_read_loop(std::shared_ptr<Session> session) {
    LineFramer framer(proto::kMaxMessageBytes);
    std::vector<std::string> lines;
    char buffer[8192];

    while (!session->closing.load(std::memory_order_acquire) &&
           running_.load(std::memory_order_acquire)) {
        const int n = session->connection->read(buffer, sizeof(buffer));
        if (n <= 0) break;

        lines.clear();
        if (!framer.feed(std::string_view(buffer, static_cast<std::size_t>(n)), lines)) {
            // An unterminated flood: the peer is malformed enough that continuing is pointless.
            TSRO_WARN(kComponent, "client sent an oversized frame; closing the connection");
            {
                std::lock_guard<std::mutex> lock(stats_mutex_);
                ++stats_.malformed_from_clients;
            }
            break;
        }

        bool fatal = false;
        for (const std::string& line : lines) {
            const std::int64_t now = proto::now_unix_ms();
            {
                std::lock_guard<std::mutex> lock(session->mutex);
                if (now - session->rate_window_start_ms >= 1000) {
                    session->rate_window_start_ms = now;
                    session->rate_window_count = 0;
                }
                if (++session->rate_window_count > config_.max_client_messages_per_second) {
                    fatal = true;
                }
            }
            if (fatal) {
                TSRO_WARN(kComponent, "client exceeded the message rate limit; closing");
                break;
            }

            const proto::DecodeResult decoded = proto::decode(line);
            if (!decoded.ok()) {
                std::lock_guard<std::mutex> lock(stats_mutex_);
                ++stats_.malformed_from_clients;
                if (decoded.fatal()) {
                    fatal = true;
                    break;
                }
                continue;  // droppable: a single bad message is not a bad peer
            }
            if (proto::is_server_to_client(decoded.envelope.type)) continue;
            handle_client_message(*session, decoded.envelope);
        }
        if (fatal) break;
    }

    session->shutdown();
    session->finished.store(true, std::memory_order_release);
}

void IpcServer::handle_client_message(Session& session, const proto::Envelope& envelope) {
    switch (envelope.type) {
        case proto::MessageType::ClientHello: {
            proto::ClientHelloPayload hello;
            if (!proto::decode(envelope.data, hello) || hello.protocol < proto::kProtocolMin ||
                hello.protocol > proto::kProtocolMax) {
                proto::ErrorPayload err;
                err.code = "unsupported_version";
                err.message = "client requested protocol " + std::to_string(hello.protocol) +
                              "; this plugin supports " + std::to_string(proto::kProtocolMin) +
                              ".." + std::to_string(proto::kProtocolMax);
                err.fatal = true;
                std::int64_t seq;
                {
                    std::lock_guard<std::mutex> lock(session.mutex);
                    seq = session.seq++;
                }
                enqueue(session, proto::make_line(proto::MessageType::Error, seq,
                                                  proto::now_unix_ms(), "", proto::encode(err)));
                TSRO_WARN(kComponent, "rejected a client with an unsupported protocol version");
                // Let the writer flush the error, then close.
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
                session.shutdown();
                return;
            }
            {
                std::lock_guard<std::mutex> lock(session.mutex);
                session.handshaken = true;
                session.description = hello.process.empty()
                                          ? session.description
                                          : hello.process + " (" + session.description + ")";
            }
            TSRO_INFO(kComponent, "handshake complete with " + hello.client + " " +
                                      hello.client_version);
            return;
        }

        case proto::MessageType::ConfigurationUpdated: {
            proto::ConfigurationUpdatedPayload cfg;
            if (!proto::decode(envelope.data, cfg)) return;
            {
                std::lock_guard<std::mutex> lock(session.mutex);
                session.subscription = cfg.chat;
                session.want_speaking = cfg.want_speaking_events;
            }
            TSRO_DEBUG(kComponent,
                       std::string("client subscription updated: channel=") +
                           (cfg.chat.channel ? "on" : "off") + " server=" +
                           (cfg.chat.server ? "on" : "off") + " private=" +
                           (cfg.chat.priv ? "on" : "off"));
            return;
        }

        case proto::MessageType::RequestSnapshot: {
            proto::RequestSnapshotPayload req;
            proto::decode(envelope.data, req);
            std::lock_guard<std::mutex> lock(session.mutex);
            enqueue_snapshot_locked(session);
            session.cv.notify_one();
            return;
        }

        case proto::MessageType::Ping: {
            proto::PingPayload ping;
            if (!proto::decode(envelope.data, ping)) return;
            proto::HeartbeatPayload hb;
            hb.uptime_ms = proto::now_unix_ms() - started_ms_;
            hb.nonce = ping.nonce;
            {
                std::lock_guard<std::mutex> lock(sessions_mutex_);
                hb.connected_clients = static_cast<int>(sessions_.size());
            }
            std::int64_t seq;
            {
                std::lock_guard<std::mutex> lock(session.mutex);
                seq = session.seq++;
            }
            enqueue(session, proto::make_line(proto::MessageType::Heartbeat, seq,
                                              proto::now_unix_ms(), "", proto::encode(hb)));
            return;
        }

        default:
            return;  // unknown client verbs are ignored, never dispatched
    }
}

IpcServerStats IpcServer::stats() const {
    std::lock_guard<std::mutex> lock(stats_mutex_);
    IpcServerStats copy = stats_;
    {
        std::lock_guard<std::mutex> slock(sessions_mutex_);
        copy.connected_clients = static_cast<int>(sessions_.size());
    }
    return copy;
}

std::vector<std::string> IpcServer::describe_clients() const {
    std::vector<std::string> out;
    std::lock_guard<std::mutex> lock(sessions_mutex_);
    out.reserve(sessions_.size());
    for (const auto& s : sessions_) {
        std::lock_guard<std::mutex> slock(s->mutex);
        out.push_back(s->description + (s->handshaken ? " [ready]" : " [handshaking]") +
                      " queued=" + std::to_string(s->queue.size()) +
                      " sent=" + std::to_string(s->messages_sent) +
                      " dropped=" + std::to_string(s->messages_dropped));
    }
    return out;
}

}  // namespace tsro
