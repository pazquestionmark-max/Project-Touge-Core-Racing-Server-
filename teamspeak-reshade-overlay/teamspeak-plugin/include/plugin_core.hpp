// SPDX-License-Identifier: MIT
// Event collection: the layer between TeamSpeak's callbacks and the IPC server.
//
// Every method here is called on the TeamSpeak callback thread. Each does bounded work — a few
// property reads, a state update, one serialisation — and hands the result to IpcServer, which
// queues it without blocking. No method performs a blocking write or waits on an IPC thread.
#ifndef TSRO_PLUGIN_CORE_HPP
#define TSRO_PLUGIN_CORE_HPP

#include <cstdint>
#include <memory>
#include <string>

#include "ts_state.hpp"
#include "tsro/ipc_server.hpp"

namespace tsro::plugin {

/// TeamSpeak's own connection states, named so the mapping is explicit and reviewable rather
/// than a bare integer comparison against an SDK constant.
enum class TsConnectStatus { Disconnected, Connecting, Connected, Establishing, Established };

/// TeamSpeak's talk status values.
enum class TsTalkStatus { NotTalking, Talking, TalkingWhileDisabled };

/// TeamSpeak's text message targets.
enum class TsTextTarget { Client, Channel, Server };

/// Why a client appeared in or vanished from our channel.
enum class MoveCause { Moved, Connected, Disconnected, Timeout, Kicked, Banned };

struct PluginCoreOptions {
    std::string plugin_version = "1.0.0";
    std::string ts_client_version;
    int plugin_api_version = 0;
    IpcServerConfig ipc;
};

class PluginCore {
public:
    PluginCore(TsQuery& query, PluginCoreOptions options);
    ~PluginCore();

    bool start(std::string& error);
    void stop();

    /// TeamSpeak's own configuration folder, from getConfigPath. That is where the client keeps
    /// the contact list the overlay reads its friends from.
    void set_config_directory(std::string dir);
    /// Re-reads the contact list, at most once every few seconds unless forced. Cheap when
    /// nothing changed and the only way friends stay current without a plugin API for them.
    void refresh_contacts(bool force);

    // --- TeamSpeak callbacks, normalised ---
    void on_connect_status_changed(std::uint64_t server, TsConnectStatus status,
                                   unsigned error_code);
    /// A client entered or left a channel. Covers joins, leaves, moves, kicks, bans and
    /// timeouts; `cause` says which.
    void on_client_moved(std::uint64_t server, std::uint16_t client, std::uint64_t from_channel,
                         std::uint64_t to_channel, MoveCause cause);
    void on_talk_status_changed(std::uint64_t server, TsTalkStatus status, bool received_whisper,
                                std::uint16_t client);
    void on_client_updated(std::uint64_t server, std::uint16_t client);
    void on_self_variable_updated(std::uint64_t server);
    void on_channel_updated(std::uint64_t server, std::uint64_t channel);
    /// A channel subscription batch finished: membership is only trustworthy from here.
    void on_subscription_finished(std::uint64_t server);
    void on_text_message(std::uint64_t server, TsTextTarget target, std::uint16_t from_client,
                         const std::string& from_name, const std::string& from_unique_id,
                         const std::string& message);
    /// A poke: TeamSpeak's own "look at me now", delivered by its own callback and carrying its
    /// own message. Rides the private-message subscription, being just as personal.
    void on_poke(std::uint64_t server, std::uint16_t from_client, const std::string& from_name,
                 const std::string& from_unique_id, const std::string& message);
    /// The user switched TeamSpeak tabs; the overlay follows the active connection.
    void on_current_server_changed(std::uint64_t server);
    void on_server_stopped(std::uint64_t server);

    const TsState& state() const noexcept { return state_; }
    IpcServer* ipc() noexcept { return ipc_.get(); }
    /// Lines for the plugin's in-client info panel and the overlay's Diagnostics tab.
    std::vector<std::string> diagnostics() const;

private:
    bool is_active_server(std::uint64_t server) const;
    /// Handles the local user moving between channels: emits channel_changed and resyncs.
    void handle_self_moved(std::uint64_t server, std::uint64_t from_channel,
                           std::uint64_t to_channel);
    void emit_snapshot();
    /// Re-reads the whole channel and emits a join or leave for every difference.
    ///
    /// The fallback whenever a move cannot be turned into an event directly -- TeamSpeak does
    /// not guarantee a client's properties are readable the instant it reports the move, and a
    /// client whose unique identifier cannot be read yet has no identity to announce. Re-reading
    /// is authoritative and cannot miss anyone.
    void resync_and_report(std::uint64_t server, MoveCause cause);
    void emit_user_joined(const UserState& user, MoveCause cause,
                          const std::string& from_channel_name);
    void emit_user_left(const UserState& user, MoveCause cause, std::uint64_t to_channel,
                        const std::string& to_channel_name);

    TsQuery& query_;
    PluginCoreOptions options_;
    TsState state_;
    std::unique_ptr<IpcServer> ipc_;
    std::uint64_t active_server_ = 0;
    std::uint64_t last_channel_ = 0;
    ConnectionState last_connection_ = ConnectionState::Disconnected;
    std::string config_dir_;
    std::int64_t contacts_read_ms_ = 0;
    std::string contacts_error_;
    std::string contacts_report_;
    std::uint64_t next_chat_id_ = 1;
    std::uint64_t events_emitted_ = 0;
    std::string last_event_;
};

}  // namespace tsro::plugin

#endif  // TSRO_PLUGIN_CORE_HPP
