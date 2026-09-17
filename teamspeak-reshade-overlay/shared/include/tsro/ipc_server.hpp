// SPDX-License-Identifier: MIT
// Multi-client IPC server, hosted by the TeamSpeak plugin.
//
// Threading contract (see docs/architecture.md §6.3): callers may invoke broadcast* from the
// TeamSpeak callback thread. Those calls take one short per-session mutex, append to a bounded
// queue and return. They never write to a handle, never wait on I/O, and never block on a peer.
#ifndef TSRO_IPC_SERVER_HPP
#define TSRO_IPC_SERVER_HPP

#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "tsro/model.hpp"
#include "tsro/protocol.hpp"
#include "tsro/transport.hpp"

namespace tsro {

struct IpcServerConfig {
    std::string endpoint;               ///< empty ⇒ default_endpoint("")
    std::size_t queue_depth = 256;      ///< per client; overflow forces a resynchronisation
    int heartbeat_ms = proto::kHeartbeatIntervalMs;
    int max_clients = 8;
    /// Client→server messages above this rate indicate a malfunctioning or hostile peer.
    int max_client_messages_per_second = 200;
};

struct IpcServerStats {
    std::uint64_t connections_accepted = 0;
    std::uint64_t connections_rejected = 0;
    std::uint64_t messages_sent = 0;
    std::uint64_t messages_dropped = 0;
    std::uint64_t resyncs_forced = 0;
    std::uint64_t malformed_from_clients = 0;
    int connected_clients = 0;
};

class IpcServer {
public:
    /// Returns the `data` object for a `state_snapshot`. Called from IPC threads, so the
    /// implementation must be cheap and internally synchronised — the plugin serves it from a
    /// cache it refreshes on the TeamSpeak thread rather than querying TeamSpeak here.
    using SnapshotFn = std::function<json::Value()>;

    IpcServer(IpcServerConfig config, proto::HelloPayload hello, SnapshotFn snapshot);
    ~IpcServer();

    IpcServer(const IpcServer&) = delete;
    IpcServer& operator=(const IpcServer&) = delete;

    bool start(std::string& error);
    void stop();
    bool running() const noexcept { return running_.load(std::memory_order_acquire); }

    /// Queues a message to every connected client. Safe from the TeamSpeak callback thread.
    void broadcast(proto::MessageType type, json::Value data, std::string_view server_uid);

    /// Queues a chat message only to clients that subscribed to its category. A category nobody
    /// subscribed to is never serialised, so it cannot appear in a pipe trace.
    void broadcast_chat(const ChatMessage& message, std::string_view server_uid);

    /// Pushes a fresh snapshot to every client (used after a state change too large to express
    /// as events, and after a queue overflow).
    void broadcast_snapshot(std::string_view server_uid);

    IpcServerStats stats() const;
    std::string endpoint() const { return endpoint_; }
    /// Human-readable summary for the Diagnostics view.
    std::vector<std::string> describe_clients() const;

private:
    struct Session;

    void accept_loop();
    void heartbeat_loop();
    void session_read_loop(std::shared_ptr<Session> session);
    void session_write_loop(std::shared_ptr<Session> session);
    void enqueue(Session& session, std::string line);
    void enqueue_snapshot_locked(Session& session);
    void handle_client_message(Session& session, const proto::Envelope& envelope);
    void reap_finished_sessions();

    IpcServerConfig config_;
    proto::HelloPayload hello_;
    SnapshotFn snapshot_;
    std::string endpoint_;

    std::unique_ptr<ServerTransport> transport_;
    std::thread accept_thread_;
    std::thread heartbeat_thread_;
    std::atomic<bool> running_{false};

    mutable std::mutex sessions_mutex_;
    std::vector<std::shared_ptr<Session>> sessions_;
    std::condition_variable shutdown_cv_;
    std::mutex shutdown_mutex_;

    mutable std::mutex stats_mutex_;
    IpcServerStats stats_;
    std::int64_t started_ms_ = 0;
    std::atomic<std::uint64_t> next_chat_id_{1};
};

}  // namespace tsro

#endif  // TSRO_IPC_SERVER_HPP
