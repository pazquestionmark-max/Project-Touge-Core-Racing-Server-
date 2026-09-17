// SPDX-License-Identifier: MIT
#include "tsro/state_store.hpp"

#include <algorithm>

namespace tsro {
namespace {

OverlayEvent make_event(OverlayEventKind k, std::int64_t ts) {
    OverlayEvent e;
    e.kind = k;
    e.timestamp_ms = ts;
    return e;
}

}  // namespace

bool StateStore::has_capability(std::string_view name) const noexcept {
    return std::find(hello_.capabilities.begin(), hello_.capabilities.end(), name) !=
           hello_.capabilities.end();
}

UserState* StateStore::mutable_user(std::uint16_t client_id, std::string_view unique_id) {
    // unique_id is authoritative; client_id is a session-scoped hint that may already have been
    // recycled onto a different person, so it is only used as a fallback.
    if (!unique_id.empty()) {
        for (auto& u : state_.users) {
            if (u.unique_id == unique_id) return &u;
        }
        return nullptr;
    }
    for (auto& u : state_.users) {
        if (u.client_id == client_id) return &u;
    }
    return nullptr;
}

void StateStore::push_chat(ChatMessage m) {
    chat_.push_back(std::move(m));
    while (chat_.size() > cfg_.chat_history) chat_.pop_front();
}

void StateStore::mark_activity(std::int64_t ts) {
    state_.last_event_ms = ts;
    if (state_.stale) {
        state_.stale = false;
        stale_reported_ = false;
    }
}

StoreAction StateStore::apply(const proto::Envelope& env, std::vector<OverlayEvent>& events) {
    using proto::MessageType;

    if (!proto::is_server_to_client(env.type)) {
        // A client-direction message arriving on the client means the peer is confused; this is
        // not a droppable data error.
        ++stats_.malformed_payloads;
        return StoreAction::Disconnect;
    }

    // --- Ordering: duplicates dropped, gaps trigger resynchronisation ---
    StoreAction pending = StoreAction::None;
    if (env.type == MessageType::Hello) {
        have_seq_ = false;
        last_seq_ = -1;
    }
    if (have_seq_) {
        if (env.seq <= last_seq_) {
            ++stats_.duplicates_dropped;
            return StoreAction::None;
        }
        if (env.seq != last_seq_ + 1) {
            ++stats_.gaps_detected;
            OverlayEvent e = make_event(OverlayEventKind::Desynchronised, env.ts);
            events.push_back(e);
            pending = StoreAction::RequestSnapshot;
            // The message is still applied: it is newer than what we hold, and the snapshot we
            // just asked for will overwrite it shortly anyway.
        }
    }
    last_seq_ = env.seq;
    have_seq_ = true;

    ++stats_.applied;
    stats_.last_message_ms = env.ts;
    stats_.last_message_type = proto::to_string(env.type);
    mark_activity(env.ts);

    switch (env.type) {
        case MessageType::Hello: {
            proto::HelloPayload p;
            if (!proto::decode(env.data, p)) {
                ++stats_.malformed_payloads;
                return StoreAction::Disconnect;
            }
            if (proto::kProtocolVersion < p.protocol_min ||
                proto::kProtocolVersion > p.protocol_max) {
                // Refuse to speak a dialect we know the peer will reject, rather than
                // discovering it message by message.
                return StoreAction::Disconnect;
            }
            hello_ = std::move(p);
            handshaken_ = true;
            return pending;
        }

        case MessageType::StateSnapshot: {
            OverlayState ns;
            if (!decode_snapshot(env.data, ns)) {
                ++stats_.malformed_payloads;
                return pending;
            }
            if (ns.users.size() > proto::kMaxUsersPerSnapshot)
                ns.users.resize(proto::kMaxUsersPerSnapshot);
            const bool had_channel = state_.channel.valid();
            const std::uint64_t old_channel = state_.channel.id;
            const std::string old_name = state_.channel.name;

            ns.last_event_ms = env.ts;
            ns.stale = false;
            ns.synchronised = true;
            state_ = std::move(ns);
            stale_reported_ = false;
            ++stats_.snapshots;

            // A snapshot that lands on a different channel is a real channel change from the
            // user's point of view, even though no channel_changed event arrived.
            if (had_channel && state_.channel.valid() && old_channel != state_.channel.id) {
                OverlayEvent e = make_event(OverlayEventKind::ChannelChanged, env.ts);
                e.channel_name = state_.channel.name;
                e.previous_channel_name = old_name;
                e.user_count = static_cast<int>(state_.users.size());
                events.push_back(std::move(e));
            }
            return pending;
        }

        case MessageType::ConnectionChanged: {
            proto::ConnectionChangedPayload p;
            if (!proto::decode(env.data, p)) {
                ++stats_.malformed_payloads;
                return pending;
            }
            const ConnectionState before = state_.server.connection;
            state_.server.connection = p.connection;
            if (p.connection != ConnectionState::Connected) {
                state_.users.clear();
                state_.channel = ChannelState{};
                state_.synchronised = false;
            }
            // Suppress no-op transitions so reconnect churn does not produce a notification per
            // internal step.
            if (before != p.connection) {
                OverlayEvent e = make_event(OverlayEventKind::ConnectionChanged, env.ts);
                e.connection = p.connection;
                e.reason = p.reason;
                events.push_back(std::move(e));
            }
            return pending;
        }

        case MessageType::ChannelChanged: {
            proto::ChannelChangedPayload p;
            if (!proto::decode(env.data, p)) {
                ++stats_.malformed_payloads;
                return pending;
            }
            state_.channel = p.to;
            OverlayEvent e = make_event(OverlayEventKind::ChannelChanged, env.ts);
            e.channel_name = p.to.name;
            e.previous_channel_name = p.from.has_value() ? p.from->name : std::string();
            e.user_count = p.user_count;
            events.push_back(std::move(e));
            // The membership of the new channel is not implied by this message; the server
            // follows it with a snapshot. Until then the list is explicitly unsynchronised.
            state_.users.clear();
            state_.synchronised = false;
            return StoreAction::RequestSnapshot;
        }

        case MessageType::UserJoined: {
            proto::UserJoinedPayload p;
            if (!proto::decode(env.data, p)) {
                ++stats_.malformed_payloads;
                return pending;
            }
            if (UserState* existing = mutable_user(p.user.client_id, p.user.unique_id)) {
                *existing = p.user;  // idempotent: a repeated join is not a duplicate row
            } else {
                if (state_.users.size() >= proto::kMaxUsersPerSnapshot) return pending;
                state_.users.push_back(p.user);
            }
            OverlayEvent e = make_event(OverlayEventKind::UserJoined, env.ts);
            e.unique_id = p.user.unique_id;
            e.display_name = p.user.display_name.empty() ? p.user.nickname : p.user.display_name;
            e.channel_name = state_.channel.name;
            e.cause = p.cause;
            e.user_count = static_cast<int>(state_.users.size());
            events.push_back(std::move(e));
            return pending;
        }

        case MessageType::UserLeft: {
            proto::UserLeftPayload p;
            if (!proto::decode(env.data, p)) {
                ++stats_.malformed_payloads;
                return pending;
            }
            const auto it = std::find_if(
                state_.users.begin(), state_.users.end(),
                [&](const UserState& u) { return u.unique_id == p.user.unique_id; });
            if (it != state_.users.end()) state_.users.erase(it);
            OverlayEvent e = make_event(OverlayEventKind::UserLeft, env.ts);
            e.unique_id = p.user.unique_id;
            e.display_name = p.user.display_name.empty() ? p.user.nickname : p.user.display_name;
            e.channel_name = state_.channel.name;
            e.cause = p.cause;
            e.user_count = static_cast<int>(state_.users.size());
            events.push_back(std::move(e));
            return pending;
        }

        case MessageType::UserUpdated: {
            proto::UserUpdatedPayload p;
            if (!proto::decode(env.data, p)) {
                ++stats_.malformed_payloads;
                return pending;
            }
            UserState* u = mutable_user(p.user.client_id, p.user.unique_id);
            if (u == nullptr) {
                // An update for someone we do not have means we missed their join.
                if (state_.users.size() < proto::kMaxUsersPerSnapshot)
                    state_.users.push_back(p.user);
                return pending == StoreAction::None ? StoreAction::RequestSnapshot : pending;
            }
            const bool was_commander = u->channel_commander.value_or(false);
            // Talking state is owned by speaking_changed, which is far more frequent and may
            // legitimately be newer than the snapshot this update was built from.
            const bool talking = u->talking;
            const bool whisper = u->whispering_to_me;
            *u = p.user;
            u->talking = talking;
            u->whispering_to_me = whisper;
            const bool is_commander = u->channel_commander.value_or(false);
            if (was_commander != is_commander) {
                OverlayEvent e = make_event(OverlayEventKind::CommanderChanged, env.ts);
                e.unique_id = u->unique_id;
                e.display_name = u->display_name;
                events.push_back(std::move(e));
            }
            return pending;
        }

        case MessageType::SpeakingChanged: {
            proto::SpeakingChangedPayload p;
            if (!proto::decode(env.data, p)) {
                ++stats_.malformed_payloads;
                return pending;
            }
            UserState* u = mutable_user(p.client_id, p.unique_id);
            if (u == nullptr) return pending;
            const bool was_talking = u->talking;
            const bool was_whisper = u->whispering_to_me;
            u->talking = p.talking;
            u->whispering_to_me = p.talking && p.whisper;
            if (was_talking != u->talking) {
                OverlayEvent e = make_event(u->talking ? OverlayEventKind::SpeakingStarted
                                                       : OverlayEventKind::SpeakingStopped,
                                            env.ts);
                e.unique_id = u->unique_id;
                e.display_name = u->display_name;
                events.push_back(std::move(e));
            }
            if (was_whisper != u->whispering_to_me) {
                OverlayEvent e = make_event(u->whispering_to_me ? OverlayEventKind::WhisperStarted
                                                               : OverlayEventKind::WhisperStopped,
                                            env.ts);
                e.unique_id = u->unique_id;
                e.display_name = u->display_name;
                events.push_back(std::move(e));
            }
            return pending;
        }

        case MessageType::MuteChanged: {
            proto::MuteChangedPayload p;
            if (!proto::decode(env.data, p)) {
                ++stats_.malformed_payloads;
                return pending;
            }
            UserState* u = mutable_user(p.client_id, p.unique_id);
            if (u == nullptr) return pending;
            // Only overwrite what the message actually carried: an omitted field means the
            // sender could not determine it, not that it became unknown.
            if (p.input_muted.has_value()) u->input_muted = p.input_muted;
            if (p.output_muted.has_value()) u->output_muted = p.output_muted;
            if (p.input_hardware.has_value()) u->input_hardware = p.input_hardware;
            if (p.output_hardware.has_value()) u->output_hardware = p.output_hardware;
            if (p.input_deactivated.has_value()) u->input_deactivated = p.input_deactivated;
            return pending;
        }

        case MessageType::CommanderChanged: {
            proto::CommanderChangedPayload p;
            if (!proto::decode(env.data, p)) {
                ++stats_.malformed_payloads;
                return pending;
            }
            UserState* u = mutable_user(p.client_id, p.unique_id);
            if (u == nullptr) return pending;
            const bool before = u->channel_commander.value_or(false);
            u->channel_commander = p.channel_commander;
            if (before != p.channel_commander) {
                OverlayEvent e = make_event(OverlayEventKind::CommanderChanged, env.ts);
                e.unique_id = u->unique_id;
                e.display_name = u->display_name;
                events.push_back(std::move(e));
            }
            return pending;
        }

        case MessageType::WhisperChanged: {
            proto::WhisperChangedPayload p;
            if (!proto::decode(env.data, p)) {
                ++stats_.malformed_payloads;
                return pending;
            }
            UserState* u = mutable_user(p.client_id, p.unique_id);
            if (u == nullptr) return pending;
            const bool before = u->whispering_to_me;
            u->whispering_to_me = p.active;
            if (before != p.active) {
                OverlayEvent e = make_event(
                    p.active ? OverlayEventKind::WhisperStarted : OverlayEventKind::WhisperStopped,
                    env.ts);
                e.unique_id = u->unique_id;
                e.display_name = u->display_name;
                events.push_back(std::move(e));
            }
            return pending;
        }

        case MessageType::ChatMessageMsg: {
            ChatMessage m;
            if (!decode_chat(env.data, m)) {
                ++stats_.malformed_payloads;
                return pending;
            }
            m.text = json::truncate_utf8(m.text, proto::kMaxChatChars);
            OverlayEvent e = make_event(OverlayEventKind::ChatMessage, env.ts);
            e.chat = m;
            e.unique_id = m.sender_unique_id;
            e.display_name = m.sender_name;
            push_chat(std::move(m));
            events.push_back(std::move(e));
            return pending;
        }

        case MessageType::Heartbeat: {
            proto::HeartbeatPayload p;
            if (!proto::decode(env.data, p)) {
                ++stats_.malformed_payloads;
                return pending;
            }
            if (p.nonce.has_value() && *p.nonce == ping_nonce_ && ping_nonce_ >= 0) {
                stats_.round_trip_ms = env.ts - ping_sent_ms_;
                if (stats_.round_trip_ms < 0) stats_.round_trip_ms = 0;
                ping_nonce_ = -1;
            }
            return pending;
        }

        case MessageType::Error: {
            proto::ErrorPayload p;
            if (!proto::decode(env.data, p)) {
                ++stats_.malformed_payloads;
                return StoreAction::Disconnect;
            }
            return p.fatal ? StoreAction::Disconnect : pending;
        }

        default:
            ++stats_.unknown_types;
            return pending;
    }
}

StoreAction StateStore::tick(std::int64_t now_ms, std::vector<OverlayEvent>& events) {
    if (!handshaken_ || state_.last_event_ms == 0) return StoreAction::None;
    if (now_ms - state_.last_event_ms <= cfg_.stale_after_ms) return StoreAction::None;

    state_.stale = true;
    if (stale_reported_) return StoreAction::None;
    stale_reported_ = true;
    events.push_back(make_event(OverlayEventKind::Desynchronised, now_ms));
    return StoreAction::RequestSnapshot;
}

void StateStore::on_disconnected(std::int64_t now_ms, std::vector<OverlayEvent>& events) {
    const bool was_connected = state_.server.connection == ConnectionState::Connected;
    state_.users.clear();
    state_.channel = ChannelState{};
    state_.server.connection = ConnectionState::Disconnected;
    state_.synchronised = false;
    state_.stale = true;
    handshaken_ = false;
    have_seq_ = false;
    last_seq_ = -1;
    stale_reported_ = false;
    ping_nonce_ = -1;
    if (was_connected) {
        OverlayEvent e = make_event(OverlayEventKind::ConnectionChanged, now_ms);
        e.connection = ConnectionState::Disconnected;
        e.reason = DisconnectReason::ConnectionLost;
        events.push_back(std::move(e));
    }
}

}  // namespace tsro
