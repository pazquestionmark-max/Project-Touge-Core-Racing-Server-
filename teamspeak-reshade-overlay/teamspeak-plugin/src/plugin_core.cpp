// SPDX-License-Identifier: MIT
#include "plugin_core.hpp"

#include <chrono>

#include <algorithm>

#include "tsro/log.hpp"

namespace tsro::plugin {
namespace {

constexpr char kComponent[] = "plugin";

ConnectionState to_model(TsConnectStatus status) {
    switch (status) {
        case TsConnectStatus::Disconnected: return ConnectionState::Disconnected;
        // TeamSpeak reports several intermediate states while a connection is being brought up.
        // Collapsing them into one `connecting` is what stops a reconnect producing a burst of
        // notifications for transitions the user never thinks of as separate.
        case TsConnectStatus::Connecting:
        case TsConnectStatus::Establishing: return ConnectionState::Connecting;
        case TsConnectStatus::Connected:
        case TsConnectStatus::Established: return ConnectionState::Connected;
    }
    return ConnectionState::Disconnected;
}

JoinCause to_model(MoveCause cause) {
    switch (cause) {
        case MoveCause::Moved: return JoinCause::Moved;
        case MoveCause::Connected: return JoinCause::Connected;
        case MoveCause::Disconnected: return JoinCause::Disconnected;
        case MoveCause::Timeout: return JoinCause::Timeout;
        case MoveCause::Kicked: return JoinCause::Kicked;
        case MoveCause::Banned: return JoinCause::Banned;
    }
    return JoinCause::Moved;
}

ChatCategory to_model(TsTextTarget target) {
    switch (target) {
        case TsTextTarget::Client: return ChatCategory::Private;
        case TsTextTarget::Channel: return ChatCategory::Channel;
        case TsTextTarget::Server: return ChatCategory::Server;
    }
    return ChatCategory::Channel;
}

}  // namespace

PluginCore::PluginCore(TsQuery& query, PluginCoreOptions options)
    : query_(query), options_(std::move(options)), state_(query) {}

PluginCore::~PluginCore() { stop(); }

bool PluginCore::start(std::string& error) {
    proto::HelloPayload hello;
    hello.protocol_min = proto::kProtocolMin;
    hello.protocol_max = proto::kProtocolMax;
    hello.plugin_version = options_.plugin_version;
    hello.ts_client_version = options_.ts_client_version;
    hello.plugin_api_version = options_.plugin_api_version;
    hello.capabilities = supported_capabilities();

    // The snapshot provider reads a cache the TeamSpeak thread refreshes. It must never call
    // back into TeamSpeak: it runs on IPC threads, where the client's own reentrancy rules do
    // not apply.
    ipc_ = std::make_unique<IpcServer>(options_.ipc, std::move(hello),
                                       [this] { return state_.snapshot(); });
    if (!ipc_->start(error)) {
        ipc_.reset();
        return false;
    }
    TSRO_INFO(kComponent, "overlay integration started on " + ipc_->endpoint());
    return true;
}

void PluginCore::stop() {
    if (ipc_) {
        ipc_->stop();
        ipc_.reset();
    }
}

bool PluginCore::is_active_server(std::uint64_t server) const {
    // The overlay follows one connection at a time — the tab the user is actually on. Events
    // from background connections are dropped rather than interleaved into one confusing list.
    return active_server_ != 0 && server == active_server_;
}

void PluginCore::set_config_directory(std::string dir) {
    config_dir_ = std::move(dir);
    refresh_contacts(true);
}

void PluginCore::refresh_contacts(bool force) {
    if (config_dir_.empty()) return;
    const std::int64_t now =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count();
    // Rate-limited because this reads a file: a busy channel raises the events that trigger it
    // far more often than anyone edits their contact list.
    if (!force && contacts_read_ms_ != 0 && now - contacts_read_ms_ < 10000) return;
    contacts_read_ms_ = now;

    std::vector<Contact> contacts;
    std::string error;
    if (!read_contacts(config_dir_, contacts, error)) {
        if (error != contacts_error_) {
            contacts_error_ = error;
            TSRO_WARN(kComponent, "contact list unavailable: " + error);
        }
        return;
    }
    contacts_error_.clear();
    const std::size_t total = contacts.size();
    state_.set_contacts(std::move(contacts));
    TSRO_INFO(kComponent, "contacts: " + std::to_string(total) + " read, " +
                              std::to_string(state_.friend_count()) + " friends");
}

void PluginCore::emit_snapshot() {
    if (!ipc_) return;
    ipc_->broadcast(proto::MessageType::StateSnapshot, state_.snapshot(), state_.server_uid());
    ++events_emitted_;
    last_event_ = "state_snapshot";
}

void PluginCore::emit_user_joined(const UserState& user, MoveCause cause) {
    if (!ipc_) return;
    proto::UserJoinedPayload payload;
    payload.user = user;
    payload.cause = to_model(cause);
    ipc_->broadcast(proto::MessageType::UserJoined, proto::encode(payload), state_.server_uid());
    ++events_emitted_;
    last_event_ = "user_joined";
}

void PluginCore::emit_user_left(const UserState& user, MoveCause cause,
                                std::uint64_t to_channel) {
    if (!ipc_) return;
    proto::UserLeftPayload payload;
    payload.user = user;
    payload.cause = to_model(cause);
    payload.to_channel_id = to_channel;
    ipc_->broadcast(proto::MessageType::UserLeft, proto::encode(payload), state_.server_uid());
    ++events_emitted_;
    last_event_ = "user_left";
}

void PluginCore::on_connect_status_changed(std::uint64_t server, TsConnectStatus status,
                                           unsigned error_code) {
    const ConnectionState mapped = to_model(status);

    if (mapped == ConnectionState::Connected) {
        active_server_ = server;
        // Someone may have edited their contacts between sessions.
        refresh_contacts(true);
    } else if (server == active_server_ && mapped == ConnectionState::Disconnected) {
        active_server_ = 0;
    } else if (active_server_ != 0 && server != active_server_) {
        return;  // a background connection changing state is not our business
    }

    state_.set_connection(server, mapped);

    if (mapped == ConnectionState::Connected) {
        state_.resynchronise(server);
        last_channel_ = state_.channel_id();
    } else {
        last_channel_ = 0;
    }

    // Suppress repeats so a flapping link does not emit a notification per internal transition.
    if (mapped != last_connection_) {
        last_connection_ = mapped;
        if (ipc_) {
            proto::ConnectionChangedPayload payload;
            payload.connection = mapped;
            payload.reason = error_code != 0 ? DisconnectReason::ConnectionLost
                                             : (mapped == ConnectionState::Disconnected
                                                    ? DisconnectReason::User
                                                    : DisconnectReason::Unknown);
            payload.error_code = error_code;
            ipc_->broadcast(proto::MessageType::ConnectionChanged, proto::encode(payload),
                            state_.server_uid());
            ++events_emitted_;
            last_event_ = "connection_changed";
        }
    }
    emit_snapshot();
}

void PluginCore::handle_self_moved(std::uint64_t server, std::uint64_t from_channel,
                                   std::uint64_t to_channel) {
    // to_channel is not read: resynchronise re-queries the client for where we actually are,
    // which is authoritative even if the callback and the client have raced.
    (void)to_channel;
    ChannelState from;
    const bool had_from = state_.read_channel(server, from_channel, from);

    state_.resynchronise(server);
    last_channel_ = state_.channel_id();

    if (ipc_) {
        proto::ChannelChangedPayload payload;
        if (had_from) payload.from = from;
        payload.to = state_.state().channel;
        payload.user_count = static_cast<int>(state_.state().users.size());
        ipc_->broadcast(proto::MessageType::ChannelChanged, proto::encode(payload),
                        state_.server_uid());
        ++events_emitted_;
        last_event_ = "channel_changed";
    }
    // The client asks for a snapshot after a channel change; sending it unprompted removes a
    // round trip and closes the window in which the overlay shows an empty list.
    emit_snapshot();
}

void PluginCore::resync_and_report(std::uint64_t server, MoveCause cause) {
    const std::vector<UserState> before = state_.state().users;
    state_.resynchronise(server);
    last_channel_ = state_.channel_id();
    const std::vector<UserState> after = state_.state().users;

    const auto holds = [](const std::vector<UserState>& list, const std::string& uid) {
        for (const UserState& u : list) {
            if (u.unique_id == uid) return true;
        }
        return false;
    };
    for (const UserState& u : after) {
        if (!holds(before, u.unique_id)) emit_user_joined(u, cause);
    }
    for (const UserState& u : before) {
        if (!holds(after, u.unique_id)) emit_user_left(u, cause, 0);
    }
    emit_snapshot();
}

void PluginCore::on_client_moved(std::uint64_t server, std::uint16_t client,
                                 std::uint64_t from_channel, std::uint64_t to_channel,
                                 MoveCause cause) {
    if (!is_active_server(server)) return;

    std::uint16_t own = 0;
    const bool have_own = query_.own_client_id(server, own);
    if (have_own && client == own) {
        if (to_channel == 0) {
            // We left the server entirely; the connection callback handles the rest.
            state_.clear();
            last_channel_ = 0;
            emit_snapshot();
            return;
        }
        handle_self_moved(server, from_channel, to_channel);
        return;
    }

    const std::uint64_t our_channel = state_.channel_id();
    if (our_channel == 0) return;

    if (to_channel == our_channel && from_channel != our_channel) {
        UserState user;
        if (!state_.read_user(server, client, user) || !state_.add_user(user)) {
            // Either TeamSpeak could not yet tell us who this is, or we already had them --
            // which happens when a visibility event beat the move event to us. Neither is a
            // reason to stay silent about someone arriving, so re-read the channel and report
            // whatever actually changed.
            resync_and_report(server, cause);
            return;
        }
        emit_user_joined(user, cause);
        return;
    }

    if (from_channel == our_channel && to_channel != our_channel) {
        UserState removed;
        if (state_.remove_user(client, removed)) {
            emit_user_left(removed, cause, to_channel);
        } else {
            resync_and_report(server, cause);
        }
        return;
    }
}

void PluginCore::on_talk_status_changed(std::uint64_t server, TsTalkStatus status,
                                        bool received_whisper, std::uint16_t client) {
    if (!is_active_server(server)) return;
    // TALKING_WHILE_DISABLED means their microphone is producing audio we are not transmitting;
    // it is not the speaking state the overlay should show.
    const bool talking = status == TsTalkStatus::Talking;

    const UserState* before = state_.find(client);
    if (before == nullptr) return;
    const std::string unique_id = before->unique_id;
    const bool whisper_before = before->whispering_to_me;

    if (!state_.set_talking(client, talking, received_whisper)) return;
    if (!ipc_) return;

    proto::SpeakingChangedPayload payload;
    payload.client_id = client;
    payload.unique_id = unique_id;
    payload.talking = talking;
    payload.whisper = talking && received_whisper;
    ipc_->broadcast(proto::MessageType::SpeakingChanged, proto::encode(payload),
                    state_.server_uid());
    ++events_emitted_;
    last_event_ = "speaking_changed";

    // A whisper transition is also reported on its own channel, so a client that only cares
    // about whispers does not have to infer them from speaking events.
    const UserState* after = state_.find(client);
    if (after != nullptr && after->whispering_to_me != whisper_before) {
        proto::WhisperChangedPayload whisper;
        whisper.client_id = client;
        whisper.unique_id = unique_id;
        whisper.active = after->whispering_to_me;
        whisper.direction = "incoming";  // the only direction Plugin API 26 reports
        ipc_->broadcast(proto::MessageType::WhisperChanged, proto::encode(whisper),
                        state_.server_uid());
        ++events_emitted_;
    }
}

void PluginCore::on_client_updated(std::uint64_t server, std::uint16_t client) {
    if (!is_active_server(server)) return;
    const UserState* before = state_.find(client);
    if (before == nullptr) return;
    const bool commander_before = before->channel_commander.value_or(false);

    const std::vector<std::string> changed = state_.refresh_user(server, client);
    if (changed.empty() || !ipc_) return;

    const UserState* after = state_.find(client);
    if (after == nullptr) return;

    proto::UserUpdatedPayload payload;
    payload.user = *after;
    payload.changed = changed;
    ipc_->broadcast(proto::MessageType::UserUpdated, proto::encode(payload),
                    state_.server_uid());
    ++events_emitted_;
    last_event_ = "user_updated";

    // Commander and mute have dedicated messages as well, because clients special-case them and
    // should not have to diff a full user record to notice.
    const bool commander_after = after->channel_commander.value_or(false);
    if (commander_before != commander_after) {
        proto::CommanderChangedPayload commander;
        commander.client_id = client;
        commander.unique_id = after->unique_id;
        commander.channel_commander = commander_after;
        ipc_->broadcast(proto::MessageType::CommanderChanged, proto::encode(commander),
                        state_.server_uid());
        ++events_emitted_;
    }

    const bool mute_changed =
        std::find(changed.begin(), changed.end(), "input_muted") != changed.end() ||
        std::find(changed.begin(), changed.end(), "output_muted") != changed.end() ||
        std::find(changed.begin(), changed.end(), "input_hardware") != changed.end() ||
        std::find(changed.begin(), changed.end(), "output_hardware") != changed.end() ||
        std::find(changed.begin(), changed.end(), "input_deactivated") != changed.end();
    if (mute_changed) {
        proto::MuteChangedPayload mute;
        mute.client_id = client;
        mute.unique_id = after->unique_id;
        mute.input_muted = after->input_muted;
        mute.output_muted = after->output_muted;
        mute.input_hardware = after->input_hardware;
        mute.output_hardware = after->output_hardware;
        mute.input_deactivated = after->input_deactivated;
        ipc_->broadcast(proto::MessageType::MuteChanged, proto::encode(mute),
                        state_.server_uid());
        ++events_emitted_;
    }
}

void PluginCore::on_self_variable_updated(std::uint64_t server) {
    if (!is_active_server(server)) return;
    std::uint16_t own = 0;
    if (!query_.own_client_id(server, own)) return;
    on_client_updated(server, own);
}

void PluginCore::on_channel_updated(std::uint64_t server, std::uint64_t channel) {
    if (!is_active_server(server)) return;
    // Only our own channel (or its parent, whose name we display) is worth reacting to.
    const ChannelState& current = state_.state().channel;
    if (channel != current.id && channel != current.parent_id) return;
    state_.resynchronise(server);
    emit_snapshot();
}

void PluginCore::on_subscription_finished(std::uint64_t server) {
    refresh_contacts(false);
    if (!is_active_server(server)) return;
    // Client visibility is only complete once subscription finishes, so this is the first point
    // at which the roster can be trusted.
    state_.resynchronise(server);
    last_channel_ = state_.channel_id();
    emit_snapshot();
}

void PluginCore::on_text_message(std::uint64_t server, TsTextTarget target,
                                 std::uint16_t from_client, const std::string& from_name,
                                 const std::string& from_unique_id, const std::string& message) {
    if (!is_active_server(server) || !ipc_) return;

    ChatMessage chat;
    chat.id = next_chat_id_++;
    chat.category = to_model(target);
    chat.sender_unique_id = from_unique_id;
    chat.sender_name = from_name;
    chat.channel_name = state_.state().channel.name;
    chat.text = json::truncate_utf8(message, proto::kMaxChatChars);
    chat.timestamp_ms = proto::now_unix_ms();

    std::uint16_t own = 0;
    chat.outgoing = query_.own_client_id(server, own) && own == from_client;

    // broadcast_chat filters per client subscription, so a category nobody opted into is never
    // serialised at all. Content is not logged: see LoggingConfig::include_message_content.
    ipc_->broadcast_chat(chat, state_.server_uid());
    ++events_emitted_;
    last_event_ = "chat_message";
}

void PluginCore::on_current_server_changed(std::uint64_t server) {
    if (server == active_server_) return;
    active_server_ = server;
    state_.clear();
    state_.set_connection(server, ConnectionState::Connected);
    state_.resynchronise(server);
    last_channel_ = state_.channel_id();
    emit_snapshot();
    TSRO_INFO(kComponent, "active server connection changed");
}

void PluginCore::on_server_stopped(std::uint64_t server) {
    if (!is_active_server(server)) return;
    state_.set_connection(server, ConnectionState::Disconnected);
    last_connection_ = ConnectionState::Disconnected;
    active_server_ = 0;
    last_channel_ = 0;
    if (ipc_) {
        proto::ConnectionChangedPayload payload;
        payload.connection = ConnectionState::Disconnected;
        payload.reason = DisconnectReason::ServerShutdown;
        ipc_->broadcast(proto::MessageType::ConnectionChanged, proto::encode(payload), "");
        ++events_emitted_;
    }
    emit_snapshot();
}

std::vector<std::string> PluginCore::diagnostics() const {
    std::vector<std::string> lines;
    lines.push_back("Endpoint: " + (ipc_ ? ipc_->endpoint() : std::string("not started")));
    lines.push_back("Connection: " + std::string(to_string(state_.state().server.connection)));
    lines.push_back("Server: " + (state_.state().server.name.empty()
                                      ? std::string("(none)")
                                      : state_.state().server.name));
    lines.push_back("Channel: " + (state_.state().channel.valid()
                                       ? state_.state().channel.name
                                       : std::string("(none)")));
    lines.push_back("Users in channel: " + std::to_string(state_.state().users.size()));
    lines.push_back("Contacts: " + std::to_string(state_.contact_count()) + " (" +
                    std::to_string(state_.friend_count()) + " friends)" +
                    (contacts_error_.empty() ? std::string() : " -- " + contacts_error_));
    lines.push_back("Events sent: " + std::to_string(events_emitted_));
    lines.push_back("Last event: " + (last_event_.empty() ? std::string("(none)") : last_event_));
    if (ipc_) {
        const IpcServerStats stats = ipc_->stats();
        lines.push_back("Overlay clients: " + std::to_string(stats.connected_clients));
        lines.push_back("Messages sent: " + std::to_string(stats.messages_sent));
        lines.push_back("Messages dropped: " + std::to_string(stats.messages_dropped));
        lines.push_back("Forced resyncs: " + std::to_string(stats.resyncs_forced));
        for (const std::string& client : ipc_->describe_clients()) {
            lines.push_back("  client: " + client);
        }
    }
    return lines;
}

}  // namespace tsro::plugin
